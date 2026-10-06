#ifndef TILESTORE_HPP_
#define TILESTORE_HPP_

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QByteArray>

class TileFetcher;

// Base map tiles on disk + daily guard (PROGETTO.md §4.2, §18).
//
// Source: CARTO Voyager @2x (OpenStreetMap data), free key, 5M tiles/month for
// non-commercial use. Measured headers: "public, max-age=15552000" (180 days)
// plus ETag. A tile is shown only while fresh; once expired it is revalidated
// with If-None-Match (304 = still valid) before being shown again.
// Files: <data>/tiles/carto/z/x/y.png plus y.meta ("expiry-epoch etag").
class TileStore : public QObject
{
    Q_OBJECT
public:
    explicit TileStore(QObject *parent = 0);
    virtual ~TileStore();

    static QString key(int z, int x, int y);

    // file:// URL of a fresh cached tile, or empty if it must be fetched.
    QString freshTileUrl(int z, int x, int y) const;
    // Asks for a tile (x already wrapped); tileReady() follows when available.
    void request(int z, int x, int y, int priority);
    // Tiles still on/near screen; anything else waiting in queue is dropped.
    void setWanted(const QSet<QString> &keys);

    QString status() const { return m_status; }

signals:
    void tileReady(const QString &key, const QString &fileUrl);
    void statusChanged(const QString &text);

private slots:
    void onFetched(const QString &key, int status, const QByteArray &body,
                   const QByteArray &etag, int maxAge, const QString &error);

private:
    struct Want { int z, x, y, priority; };

    QString tilePath(int z, int x, int y) const;
    bool readMeta(int z, int x, int y, qint64 *expiry, QByteArray *etag) const;
    void writeMeta(int z, int x, int y, qint64 expiry, const QByteArray &etag);
    void startFetch(const Want &w);
    bool quotaAllows();
    void setStatus(const QString &text);
    static bool parseKey(const QString &key, int *z, int *x, int *y);

    TileFetcher *m_fetcher;
    QString m_apiKey;
    QSet<QString> m_inflight;
    QHash<QString, qint64> m_failedAt;       // ms epoch of the last failure, for backoff
    QString m_cacheRoot;
    QString m_status;
    QString m_quotaDay;
    int m_quotaCount;
};

#endif /* TILESTORE_HPP_ */
