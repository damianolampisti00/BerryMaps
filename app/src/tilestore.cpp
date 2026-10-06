#include "tilestore.hpp"
#include "tilefetcher.hpp"
#include "bbportlog.hpp"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSettings>
#include <QThread>
#include <QUrl>
#include <QtAlgorithms>

namespace {

const int kConnections = 3;
// CARTO's free tier is 5M tiles/month (non-commercial): this cap only guards
// against a runaway loop, it is never reached by normal use.
const int kDailyTileLimit = 30000;
const qint64 kCacheMaxBytes = 200LL * 1024 * 1024;
const qint64 kCacheTrimTo = 160LL * 1024 * 1024;
const qint64 kRetryAfterFailureMs = 5000;
const qint64 kDefaultMaxAgeSecs = 7 * 86400;   // if a response ever lacks max-age

qint64 nowSecs() { return QDateTime::currentDateTime().toTime_t(); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

void removeTree(const QString &path)
{
    QDirIterator it(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) QFile::remove(it.next());
    QDirIterator dirs(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    QStringList all;
    while (dirs.hasNext()) all << dirs.next();
    for (int i = all.size() - 1; i >= 0; --i) QDir().rmdir(all.at(i));
    QDir().rmdir(path);
}

// Off the UI thread at startup: trims the cache to kCacheTrimTo when it exceeds
// kCacheMaxBytes (oldest first), and deletes the Google tiles cached by
// BerryMaps <= 0.1.0.5 (Google's terms don't allow keeping them once unused).
class CacheMaintenance : public QThread
{
public:
    CacheMaintenance(const QString &root, const QStringList &obsolete) : m_root(root), m_obsolete(obsolete) {}
protected:
    virtual void run()
    {
        foreach (const QString &dir, m_obsolete) {
            if (!QDir(dir).exists()) continue;
            removeTree(dir);
            bbportLog("[cache] eliminata cache obsoleta " + QDir(dir).dirName());
        }
        QList<QPair<qint64, QString> > files;  // (mtime, path)
        QHash<QString, qint64> sizes;
        qint64 total = 0;
        QDirIterator it(m_root, QStringList() << "*.png", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString p = it.next();
            QFileInfo fi(p);
            total += fi.size();
            sizes.insert(p, fi.size());
            files << qMakePair(qint64(fi.lastModified().toTime_t()), p);
        }
        bbportLog(QString("[cache] %1 tessere, %2 MB").arg(files.size()).arg(total / 1048576.0, 0, 'f', 1));
        if (total <= kCacheMaxBytes) return;
        qSort(files);
        for (int i = 0; i < files.size() && total > kCacheTrimTo; ++i) {
            QString p = files.at(i).second;
            QFile::remove(p);
            QFile::remove(p.left(p.size() - 4) + ".meta");
            total -= sizes.value(p);
        }
        bbportLog(QString("[cache] ridotta a %1 MB").arg(total / 1048576.0, 0, 'f', 1));
    }
private:
    QString m_root;
    QStringList m_obsolete;
};

} // namespace

TileStore::TileStore(QObject *parent) :
    QObject(parent), m_fetcher(0), m_style("voyager"), m_quotaCount(0)
{
    m_fetcher = new TileFetcher("basemaps.cartocdn.com", kConnections, this);
    connect(m_fetcher, SIGNAL(finished(QString,int,QByteArray,QByteArray,int,QString)),
            this, SLOT(onFetched(QString,int,QByteArray,QByteArray,int,QString)));

    // Voyager tiles stay where they always were; the night style lives in a
    // "dark" subfolder, so the startup trim covers both.
    m_baseRoot = QDir::homePath() + "/tiles/carto";
    m_cacheRoot = m_baseRoot;
    QDir().mkpath(m_cacheRoot);
    CacheMaintenance *maint = new CacheMaintenance(m_baseRoot, QStringList()
            << QDir::homePath() + "/tiles/roadmap"    // Google tiles (<= 0.1.0.5)
            << m_baseRoot + "/dark");                 // un-toned night tiles (0.1.0.23-25)
    connect(maint, SIGNAL(finished()), maint, SLOT(deleteLater()));
    maint->start(QThread::LowPriority);

    // The key is read from a file, never compiled in or logged (PROGETTO.md §11).
    const QString keyFiles[2] = { QString("/accounts/1000/shared/misc/berrymaps_cartokey.txt"),
                                  QDir::homePath() + "/berrymaps_cartokey.txt" };
    for (int i = 0; i < 2 && m_apiKey.isEmpty(); ++i) {
        QFile f(keyFiles[i]);
        if (f.open(QIODevice::ReadOnly)) m_apiKey = QString::fromUtf8(f.readAll()).trimmed();
    }

    QSettings s;
    m_quotaDay = s.value("quota/carto/day").toString();
    m_quotaCount = s.value("quota/carto/count").toInt();
    // Leftovers of the Google tile source (<= 0.1.0.5).
    s.remove("tiles");
    s.remove("quota/day");
    s.remove("quota/count");
    s.remove("map/source");

    if (m_apiKey.isEmpty())
        setStatus("Chiave mappa mancante: /accounts/1000/shared/misc/berrymaps_cartokey.txt");
}

TileStore::~TileStore()
{
    delete m_fetcher;  // stops and joins the worker threads first
    m_fetcher = 0;
}

QString TileStore::key(int z, int x, int y)
{
    return QString("%1/%2/%3").arg(z).arg(x).arg(y);
}

bool TileStore::parseKey(const QString &key, int *z, int *x, int *y)
{
    QStringList p = key.split('/');
    if (p.size() != 3) return false;
    *z = p.at(0).toInt();
    *x = p.at(1).toInt();
    *y = p.at(2).toInt();
    return true;
}

QString TileStore::rootFor(const QString &style) const
{
    // "night": Dark Matter re-toned at download (see tilefetcher.cpp).
    return style == "dark_all" ? m_baseRoot + "/night" : m_baseRoot;
}

void TileStore::setStyle(const QString &style)
{
    if (style == m_style) return;
    m_style = style;
    m_cacheRoot = rootFor(style);
    QDir().mkpath(m_cacheRoot);
    // Requests of the other style still in flight are kept for its cache but
    // never shown (see onFetched); their keys must not block this style's.
    m_inflight.clear();
    m_failedAt.clear();
}

QString TileStore::tilePath(int z, int x, int y) const
{
    return QString("%1/%2/%3/%4.png").arg(m_cacheRoot).arg(z).arg(x).arg(y);
}

bool TileStore::readMeta(int z, int x, int y, qint64 *expiry, QByteArray *etag) const
{
    QString p = tilePath(z, x, y);
    QFile f(p.left(p.size() - 4) + ".meta");
    if (!f.open(QIODevice::ReadOnly)) return false;
    QList<QByteArray> parts = f.readAll().trimmed().split(' ');
    *expiry = parts.value(0).toLongLong();
    *etag = parts.value(1);
    return true;
}

void TileStore::writeMeta(int z, int x, int y, qint64 expiry, const QByteArray &etag)
{
    QString p = tilePath(z, x, y);
    QFile f(p.left(p.size() - 4) + ".meta");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray::number(expiry) + " " + etag);
}

QString TileStore::freshTileUrl(int z, int x, int y) const
{
    qint64 expiry = 0;
    QByteArray etag;
    if (!readMeta(z, x, y, &expiry, &etag) || expiry <= nowSecs()) return QString();
    QString p = tilePath(z, x, y);
    if (!QFile::exists(p)) return QString();
    return QUrl::fromLocalFile(p).toString();
}

void TileStore::setStatus(const QString &text)
{
    if (text == m_status) return;
    m_status = text;
    if (!text.isEmpty()) bbportLog("[store] " + text);
    emit statusChanged(text);
}

bool TileStore::quotaAllows()
{
    QString today = QDate::currentDate().toString(Qt::ISODate);
    if (today != m_quotaDay) {
        m_quotaDay = today;
        m_quotaCount = 0;
    }
    return m_quotaCount < kDailyTileLimit;
}

void TileStore::request(int z, int x, int y, int priority)
{
    QString k = key(z, x, y);
    if (m_inflight.contains(k)) return;
    if (nowMs() - m_failedAt.value(k, 0) < kRetryAfterFailureMs) return;
    QString fresh = freshTileUrl(z, x, y);
    if (!fresh.isEmpty()) {
        emit tileReady(k, fresh);
        return;
    }
    if (m_apiKey.isEmpty()) return;
    Want w = { z, x, y, priority };
    startFetch(w);
}

void TileStore::startFetch(const Want &w)
{
    if (!quotaAllows()) {
        setStatus(QString("Limite giornaliero di %1 tessere raggiunto").arg(kDailyTileLimit));
        return;
    }
    QString k = key(w.z, w.x, w.y);
    qint64 expiry = 0;
    QByteArray etag;
    bool haveFile = QFile::exists(tilePath(w.z, w.x, w.y)) && readMeta(w.z, w.x, w.y, &expiry, &etag);
    TileJob job;
    job.key = m_style + ":" + k;      // the style travels with the request
    job.path = QString("/rastertiles/%1/%2/%3/%4@2x.png?key=%5")
                   .arg(m_style).arg(w.z).arg(w.x).arg(w.y).arg(m_apiKey).toUtf8();
    job.etag = haveFile ? etag : QByteArray();   // expired copy on disk -> revalidate
    job.priority = w.priority;
    m_inflight.insert(k);
    ++m_quotaCount;
    m_fetcher->enqueue(job);
}

void TileStore::setWanted(const QSet<QString> &keys)
{
    QSet<QString> jobKeys;
    foreach (const QString &k, keys) jobKeys.insert(m_style + ":" + k);
    m_fetcher->setWanted(jobKeys);
    // Jobs dropped from the fetcher queue would otherwise stay "in flight"
    // forever and never be asked again when scrolled back into view.
    QList<QString> stale;
    foreach (const QString &k, m_inflight)
        if (!keys.contains(k)) stale << k;
    foreach (const QString &k, stale) m_inflight.remove(k);
}

void TileStore::onFetched(const QString &jobKey, int status, const QByteArray &body,
                          const QByteArray &etag, int maxAge, const QString &error)
{
    const QString style = jobKey.section(':', 0, 0);
    const QString k = jobKey.section(':', 1);
    int z, x, y;
    if (!parseKey(k, &z, &x, &y)) return;
    if (style != m_style) {
        // Arrived after a day/night switch: cache it for later, don't show it.
        if (status == 200 && !body.isEmpty()) {
            QString p = QString("%1/%2/%3/%4.png").arg(rootFor(style)).arg(z).arg(x).arg(y);
            QDir().mkpath(QFileInfo(p).absolutePath());
            QFile f(p);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(body);
            QFile m(p.left(p.size() - 4) + ".meta");
            if (m.open(QIODevice::WriteOnly | QIODevice::Truncate))
                m.write(QByteArray::number(nowSecs() + (maxAge > 0 ? maxAge : kDefaultMaxAgeSecs)) + " " + etag);
        }
        return;
    }
    m_inflight.remove(k);
    QSettings s;
    s.setValue("quota/carto/day", m_quotaDay);
    s.setValue("quota/carto/count", m_quotaCount);
    const qint64 expiry = nowSecs() + (maxAge > 0 ? maxAge : kDefaultMaxAgeSecs);

    if (status == 200 && !body.isEmpty()) {
        QString p = tilePath(z, x, y);
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile tmp(p + ".tmp");
        if (tmp.open(QIODevice::WriteOnly | QIODevice::Truncate) && tmp.write(body) == body.size()) {
            tmp.close();
            QFile::remove(p);
            tmp.rename(p);
            writeMeta(z, x, y, expiry, etag);
            setStatus(QString());
            emit tileReady(k, QUrl::fromLocalFile(p).toString());
        }
        return;
    }
    if (status == 304) {
        qint64 oldExpiry = 0;
        QByteArray oldEtag;
        readMeta(z, x, y, &oldExpiry, &oldEtag);
        writeMeta(z, x, y, expiry, etag.isEmpty() ? oldEtag : etag);
        emit tileReady(k, QUrl::fromLocalFile(tilePath(z, x, y)).toString());
        return;
    }
    m_failedAt.insert(k, nowMs());
    if (status == 401 || status == 403)
        setStatus(QString("Chiave mappa CARTO rifiutata (HTTP %1)").arg(status));
    else
        setStatus(status == 0 ? "Rete non disponibile: " + error
                              : QString("Errore tessere HTTP %1").arg(status));
}
