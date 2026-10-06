#include "probe.hpp"
#include "benchthreads.hpp"
#include "bbportlog.hpp"
#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tlsnetworkaccessmanager.hpp"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QMetaProperty>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <QtCore/qmath.h>

#include <bb/MemoryInfo>
#include <bb/data/JsonDataAccess>
#include <bb/device/BatteryInfo>
#include <bb/device/DisplayInfo>
#include <bb/device/HardwareInfo>
#include <bb/cascades/ImageViewLoadEffect>
#include <bb/cascades/ScalingMethod>

using namespace bb::cascades;

namespace {

// Centro di Roma: stessa zona delle tessere di prova in assets/.
const double kLat = 41.8986;
const double kLon = 12.4769;

int lonToTileX(double lon, int z) { return int((lon + 180.0) / 360.0 * (1 << z)); }
int latToTileY(double lat, int z)
{
    double r = lat * M_PI / 180.0;
    return int((1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (1 << z));
}

QString yesNo(bool b) { return b ? "si" : "no"; }

} // namespace

Probe::Probe(QObject *parent) :
    QObject(parent),
    m_nam(0), m_mem(new bb::MemoryInfo(this)), m_battery(new bb::device::BatteryInfo(this)),
    m_batteryStartLevel(0),
    m_pos(0), m_sat(0), m_ttffMs(-1), m_bestAccuracy(-1), m_lastAccuracy(-1), m_fixCount(0),
    m_satInView(0), m_satInUse(0), m_bestSnr(0), m_netTestRunning(false),
    m_compass(0), m_magnet(0), m_light(0), m_sensorSeconds(0),
    m_ivContainer(0), m_ivLoaded(0), m_ivFailed(0), m_ivPass(0), m_ivPhase(0), m_ivMemBefore(0),
    m_runAllContainer(0), m_runAllStep(0)
{
#ifdef BBPORT_HAVE_NATIVE_TLS
    m_nam = new TlsNetworkAccessManager(this);
#else
    m_nam = new QNetworkAccessManager(this);
#endif
    m_batteryTimer.setInterval(60000);
    connect(&m_batteryTimer, SIGNAL(timeout()), this, SLOT(onBatteryTick()));
    m_gpsTick.setInterval(1000);
    connect(&m_gpsTick, SIGNAL(timeout()), this, SLOT(onGpsTick()));
    m_sensorTick.setInterval(1000);
    connect(&m_sensorTick, SIGNAL(timeout()), this, SLOT(onSensorTick()));
    m_ivTimer.setSingleShot(true);
    connect(&m_ivTimer, SIGNAL(timeout()), this, SLOT(onImageViewPhase()));
    log("===== BerryProbe avviato =====");
}

Probe::~Probe()
{
    stopGps();
}

void Probe::log(const QString &line)
{
    bbportLog(line);
    emit logLine(line);
}

qint64 Probe::processMemKB() const { return m_mem->memoryUsedByCurrentProcess() / 1024; }
qint64 Probe::availableMemKB() const { return m_mem->availableDeviceMemory() / 1024; }

QString Probe::memSummary() const
{
    return QString("processo %1 MB, libera sistema %2 MB")
        .arg(processMemKB() / 1024.0, 0, 'f', 1).arg(availableMemKB() / 1024.0, 0, 'f', 0);
}

// The key never goes in the .bar or the log: drop it as a one-line text file in
// shared/misc (or the app's data dir) and delete it after the test.
QString Probe::apiKey() const
{
    const QString paths[2] = { QString("/accounts/1000/shared/misc/berryprobe_apikey.txt"),
                               QDir::homePath() + "/apikey.txt" };
    for (int i = 0; i < 2; ++i) {
        QFile f(paths[i]);
        if (f.open(QIODevice::ReadOnly)) {
            QString key = QString::fromUtf8(f.readAll()).trimmed();
            if (!key.isEmpty()) return key;
        }
    }
    return QString();
}

QString Probe::tilesDir() const
{
    QString d = QDir::homePath() + "/gtiles";
    QDir().mkpath(d);
    return d;
}

QStringList Probe::decodeFiles() const
{
    QStringList files;
    files << "app/native/assets/tile_test.png" << "app/native/assets/tile_test_pal.png"
          << "app/native/assets/tile_test.jpg";
    // Real Google tiles saved by the network test, if it ran with a key.
    QDir g(tilesDir());
    QStringList names = g.entryList(QStringList() << "*.png" << "*.jpg", QDir::Files, QDir::Name);
    for (int i = 0; i < names.size() && i < 4; ++i) files << g.filePath(names.at(i));
    return files;
}

// ------------------------------------------------------------ device info

void Probe::runDeviceInfo()
{
    log("---- Info dispositivo ----");
    bb::device::HardwareInfo hw;
    log(QString("[dev] modello=%1 (%2) nome=%3 tastiera fisica=%4")
            .arg(hw.modelName()).arg(hw.modelNumber()).arg(hw.deviceName())
            .arg(yesNo(hw.isPhysicalKeyboardDevice())));
    log(QString("[dev] core CPU (idealThreadCount)=%1").arg(QThread::idealThreadCount()));
    log(QString("[dev] memoria: totale %1 MB, %2")
            .arg(m_mem->totalDeviceMemory() / 1048576.0, 0, 'f', 0).arg(memSummary()));
    bb::device::DisplayInfo di;
    log(QString("[dev] display %1x%2 px, fisico %3x%4 mm, risoluzione %5x%6 px/m (%7 ppi)")
            .arg(di.pixelSize().width()).arg(di.pixelSize().height())
            .arg(di.physicalSize().width(), 0, 'f', 1).arg(di.physicalSize().height(), 0, 'f', 1)
            .arg(di.resolution().width(), 0, 'f', 0).arg(di.resolution().height(), 0, 'f', 0)
            .arg(di.resolution().width() * 0.0254, 0, 'f', 0));
    log(QString("[dev] batteria %1%, temperatura %2 C, stato carica %3, cicli %4")
            .arg(m_battery->level()).arg(m_battery->temperature(), 0, 'f', 1)
            .arg(int(m_battery->chargingState())).arg(m_battery->cycleCount()));
    log(QString("[dev] home=%1").arg(QDir::homePath()));
    log(QString("[dev] chiave API Google: %1").arg(apiKey().isEmpty()
            ? "assente (test rete solo TLS)" : "presente"));
}

// ------------------------------------------------------------ GPU / decode

void Probe::runGlTest()
{
    log("---- Test GPU ----");
    GlBenchThread *t = new GlBenchThread(this);
    // UI only: the thread already wrote the line to the log file itself.
    connect(t, SIGNAL(message(QString)), this, SIGNAL(logLine(QString)));
    connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
    connect(t, SIGNAL(finished()), this, SLOT(onRunAllNext()));
    t->start();
}

void Probe::runDecodeTest()
{
    log("---- Test decodifica ----");
    DecodeBenchThread *t = new DecodeBenchThread(decodeFiles(), this);
    // UI only: the thread already wrote the line to the log file itself.
    connect(t, SIGNAL(message(QString)), this, SIGNAL(logLine(QString)));
    connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
    connect(t, SIGNAL(finished()), this, SLOT(onRunAllNext()));
    t->start();
}

// ------------------------------------------------------------ ImageView x40
//
// Pass 1: 40 distinct file:/// URLs (= 40 distinct textures, like 40 different
// map tiles). Measures time until all are loaded and process memory with them
// on screen; then the views are destroyed and memory is measured again after 3
// s, to see whether Cascades' texture cache keeps them. Pass 2 reloads the same
// URLs (cache hit or not?).

void Probe::runImageViewTest(QObject *containerObj)
{
    m_ivContainer = qobject_cast<Container *>(containerObj);
    if (!m_ivContainer) {
        log("[iv] ERRORE: container non valido");
        onRunAllNext();
        return;
    }
    if (!m_trackers.isEmpty()) {
        log("[iv] test gia' in corso");
        return;
    }
    log("---- Test ImageView x40 ----");
    QString dir = QDir::homePath() + "/ivtest";
    QDir().mkpath(dir);
    for (int i = 0; i < 40; ++i) {
        QString dst = QString("%1/t_%2.png").arg(dir).arg(i, 2, 10, QChar('0'));
        if (!QFile::exists(dst) && !QFile::copy("app/native/assets/tile_test_pal.png", dst)) {
            log("[iv] ERRORE copia " + dst);
            onRunAllNext();
            return;
        }
    }
    m_ivPass = 1;
    emit imageViewTestRunning(true);
    startImageViewPass();
}

void Probe::startImageViewPass()
{
    m_ivMemBefore = processMemKB();
    log(QString("[iv] passata %1, prima: %2").arg(m_ivPass).arg(memSummary()));
    m_ivLoaded = m_ivFailed = 0;
    m_ivPhase = 0;
    m_ivClock.start();
    QString dir = QDir::homePath() + "/ivtest";
    for (int i = 0; i < 40; ++i) {
        QUrl url = QUrl::fromLocalFile(QString("%1/t_%2.png").arg(dir).arg(i, 2, 10, QChar('0')));
        ImageView *iv = ImageView::create().preferredSize(36, 36);
        iv->setScalingMethod(ScalingMethod::AspectFit);
        iv->setLoadEffect(ImageViewLoadEffect::None);
        m_ivContainer->add(iv);
        m_views << iv;
        ImageTracker *tr = new ImageTracker(this);
        connect(tr, SIGNAL(stateChanged(bb::cascades::ResourceState::Type)),
                this, SLOT(onTrackerStateChanged(bb::cascades::ResourceState::Type)));
        m_trackers << tr;
        tr->setImageSource(url);
    }
    // Safety net: report whatever happened after 20 s even if some never load.
    m_ivTimer.start(20000);
}

void Probe::onTrackerStateChanged(bb::cascades::ResourceState::Type state)
{
    ImageTracker *tr = qobject_cast<ImageTracker *>(sender());
    int idx = m_trackers.indexOf(tr);
    if (idx < 0 || m_ivPhase != 0) return;
    if (state == ResourceState::Loaded) {
        m_views.at(idx)->setImage(tr->image());
        ++m_ivLoaded;
    } else if (state == ResourceState::ErrorNotFound || state == ResourceState::ErrorInvalidFormat
               || state == ResourceState::ErrorMemory) {
        ++m_ivFailed;
        log(QString("[iv] immagine %1 fallita, stato=%2").arg(idx).arg(int(state)));
    } else {
        return;
    }
    if (m_ivLoaded + m_ivFailed == m_trackers.size()) onImageViewPhase();
}

void Probe::onImageViewPhase()
{
    if (m_trackers.isEmpty()) return;
    if (m_ivPhase == 0) {
        m_ivPhase = 1;
        log(QString("[iv] passata %1: %2 caricate, %3 fallite in %4 ms")
                .arg(m_ivPass).arg(m_ivLoaded).arg(m_ivFailed).arg(m_ivClock.elapsed()));
        m_ivTimer.start(3000);
    } else if (m_ivPhase == 1) {
        m_ivPhase = 2;
        log(QString("[iv] passata %1 con 40 tessere a schermo: %2 (delta processo %3 MB)")
                .arg(m_ivPass).arg(memSummary()).arg((processMemKB() - m_ivMemBefore) / 1024.0, 0, 'f', 1));
        // removeAll() already deletes the controls (documented: "frees up their
        // memory"), so the ImageViews must not be deleted again here.
        m_ivContainer->removeAll();
        m_views.clear();
        for (int i = 0; i < m_trackers.size(); ++i) m_trackers.at(i)->deleteLater();
        // trackers still referenced until the next phase so late signals are ignored
        m_ivTimer.start(3000);
    } else {
        log(QString("[iv] passata %1, 3 s dopo la rimozione: %2 (delta %3 MB = memoria ancora trattenuta)")
                .arg(m_ivPass).arg(memSummary()).arg((processMemKB() - m_ivMemBefore) / 1024.0, 0, 'f', 1));
        m_trackers.clear();
        if (m_ivPass == 1) {
            m_ivPass = 2;
            startImageViewPass();
        } else {
            log("[iv] fine");
            emit imageViewTestRunning(false);
            onRunAllNext();
        }
    }
}

// ------------------------------------------------------------ network

QNetworkReply *Probe::waitFor(QNetworkReply *reply, int timeoutMs, double *ms)
{
    QElapsedTimer t;
    t.start();
    QEventLoop loop;
    connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    QTimer::singleShot(timeoutMs, &loop, SLOT(quit()));
    if (!reply->isFinished()) loop.exec();
    if (!reply->isFinished()) reply->abort();
    if (ms) *ms = t.nsecsElapsed() / 1000000.0;
    return reply;
}

void Probe::logReplyHeaders(QNetworkReply *reply)
{
    const char *names[] = { "Content-Type", "Content-Length", "Cache-Control", "Expires", "ETag",
                            "Last-Modified", "Age", "Date", "Connection" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (reply->hasRawHeader(names[i]))
            log(QString("[net]     %1: %2").arg(names[i]).arg(QString::fromLatin1(reply->rawHeader(names[i]))));
    }
}

void Probe::runNetTest()
{
    if (m_netTestRunning) return;
    m_netTestRunning = true;
    log("---- Test rete ----");

    // 1) Plain HTTPS cost to Google's tile host (no key needed): every request
    //    is a fresh TCP + TLS 1.2 connection with this client (Connection: close).
    for (int i = 0; i < 3; ++i) {
        double ms = 0;
        QNetworkReply *r = waitFor(m_nam->get(QNetworkRequest(QUrl("https://tile.googleapis.com/"))), 20000, &ms);
        log(QString("[net] GET https://tile.googleapis.com/ #%1: HTTP %2, %3 ms, errore=%4")
                .arg(i + 1).arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                .arg(ms, 0, 'f', 0).arg(int(r->error())));
        r->deleteLater();
    }

    const QString key = apiKey();
    if (key.isEmpty()) {
        log("[net] nessuna chiave: metti la chiave in /accounts/1000/shared/misc/berryprobe_apikey.txt per il test completo");
        m_netTestRunning = false;
        onRunAllNext();
        return;
    }

    bb::data::JsonDataAccess jda;
    struct Variant { const char *label; const char *body; const char *ext; int tiles; } variants[] = {
        { "roadmap 2x PNG",
          "{\"mapType\":\"roadmap\",\"language\":\"it-IT\",\"region\":\"IT\",\"scale\":\"scaleFactor2x\",\"highDpi\":true,\"imageFormat\":\"png\"}",
          "png", 4 },
        { "roadmap 2x JPEG",
          "{\"mapType\":\"roadmap\",\"language\":\"it-IT\",\"region\":\"IT\",\"scale\":\"scaleFactor2x\",\"highDpi\":true,\"imageFormat\":\"jpeg\"}",
          "jpg", 2 },
        { "satellite JPEG",
          "{\"mapType\":\"satellite\",\"language\":\"it-IT\",\"region\":\"IT\",\"imageFormat\":\"jpeg\"}",
          "jpg", 1 }
    };
    const int z = 16;
    const int tx = lonToTileX(kLon, z), ty = latToTileY(kLat, z);
    QString firstSession, firstTileUrl;
    QByteArray firstEtag;

    for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        QNetworkRequest req(QUrl("https://tile.googleapis.com/v1/createSession?key=" + key));
        req.setRawHeader("Content-Type", "application/json");
        double ms = 0;
        QNetworkReply *r = waitFor(m_nam->post(req, QByteArray(variants[v].body)), 20000, &ms);
        QByteArray body = r->readAll();
        int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r->deleteLater();
        log(QString("[net] createSession %1: HTTP %2, %3 ms").arg(variants[v].label).arg(status).arg(ms, 0, 'f', 0));
        QVariantMap map = jda.loadFromBuffer(body).toMap();
        QString session = map.value("session").toString();
        if (status != 200 || session.isEmpty()) {
            log("[net]   risposta: " + QString::fromUtf8(body.left(400)));
            continue;
        }
        log(QString("[net]   expiry=%1 tileWidth=%2 tileHeight=%3 imageFormat=%4")
                .arg(map.value("expiry").toString()).arg(map.value("tileWidth").toString())
                .arg(map.value("tileHeight").toString()).arg(map.value("imageFormat").toString()));

        for (int i = 0; i < variants[v].tiles; ++i) {
            int x = tx + (i % 2), y = ty + (i / 2);
            QString path = QString("/v1/2dtiles/%1/%2/%3").arg(z).arg(x).arg(y);
            QString url = "https://tile.googleapis.com" + path + "?session=" + session + "&key=" + key;
            double tms = 0;
            QNetworkReply *tr = waitFor(m_nam->get(QNetworkRequest(QUrl(url))), 20000, &tms);
            QByteArray img = tr->readAll();
            int ts = tr->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            log(QString("[net]   tessera %1 %2: HTTP %3, %4 KB, %5 ms")
                    .arg(variants[v].label).arg(path).arg(ts).arg(img.size() / 1024.0, 0, 'f', 1).arg(tms, 0, 'f', 0));
            logReplyHeaders(tr);
            if (ts == 200) {
                QFile out(QString("%1/%2_%3_%4_%5.%6").arg(tilesDir())
                              .arg(QString(variants[v].label).section(' ', 0, 0) + (v == 1 ? "jpg" : ""))
                              .arg(z).arg(x).arg(y).arg(variants[v].ext));
                if (out.open(QIODevice::WriteOnly)) out.write(img);
                if (firstTileUrl.isEmpty()) {
                    firstTileUrl = url;
                    firstSession = session;
                    firstEtag = tr->rawHeader("ETag");
                }
            } else {
                log("[net]     corpo: " + QString::fromUtf8(img.left(300)));
            }
            tr->deleteLater();
        }
    }

    if (!firstSession.isEmpty()) {
        // Attribution text for the viewport (needed on screen by the terms).
        double d = 0.01;
        QString url = QString("https://tile.googleapis.com/tile/v1/viewport?session=%1&key=%2&zoom=%3"
                              "&north=%4&south=%5&east=%6&west=%7")
                          .arg(firstSession).arg(key).arg(z)
                          .arg(kLat + d, 0, 'f', 5).arg(kLat - d, 0, 'f', 5)
                          .arg(kLon + d, 0, 'f', 5).arg(kLon - d, 0, 'f', 5);
        double ms = 0;
        QNetworkReply *r = waitFor(m_nam->get(QNetworkRequest(QUrl(url))), 20000, &ms);
        log(QString("[net] viewport: HTTP %1, %2 ms, %3")
                .arg(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()).arg(ms, 0, 'f', 0)
                .arg(QString::fromUtf8(r->readAll().left(300))));
        r->deleteLater();

        // Conditional revalidation: does Google answer 304 to If-None-Match?
        if (!firstEtag.isEmpty()) {
            QNetworkRequest req((QUrl(firstTileUrl)));
            req.setRawHeader("If-None-Match", firstEtag);
            QNetworkReply *c = waitFor(m_nam->get(req), 20000, &ms);
            log(QString("[net] rivalidazione con If-None-Match: HTTP %1, %2 byte, %3 ms")
                    .arg(c->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                    .arg(c->readAll().size()).arg(ms, 0, 'f', 0));
            c->deleteLater();
        } else {
            log("[net] nessun ETag: rivalidazione condizionale non possibile");
        }
    }
    log("[net] fine (tessere salvate in " + tilesDir() + ", usate dal test decodifica)");
    m_netTestRunning = false;
    onRunAllNext();
}

// ------------------------------------------------------------ sensors

void Probe::runSensorTest()
{
    if (m_sensorTick.isActive()) return;
    log("---- Test sensori (20 s, tieni il telefono in piano e poi ruotalo) ----");
    m_compass = new QCompass(this);
    m_magnet = new QMagnetometer(this);
    m_light = new QLightSensor(this);
    m_compass->setSkipDuplicates(true);
    m_light->setSkipDuplicates(true);
    QSensor *all[3] = { m_compass, m_magnet, m_light };
    const char *names[3] = { "bussola", "magnetometro", "luce" };
    for (int i = 0; i < 3; ++i) {
        bool backend = all[i]->connectToBackend();
        bool started = backend && all[i]->start();
        log(QString("[sens] %1: backend=%2 avviato=%3 identifier=%4 dataRate=%5")
                .arg(names[i]).arg(yesNo(backend)).arg(yesNo(started))
                .arg(QString::fromLatin1(all[i]->identifier())).arg(all[i]->dataRate()));
    }
    m_sensorSeconds = 0;
    m_sensorTick.start();
}

void Probe::onSensorTick()
{
    ++m_sensorSeconds;
    QString line = QString("[sens] t=%1s").arg(m_sensorSeconds);
    if (QCompassReading *c = m_compass ? m_compass->reading() : 0)
        line += QString(" azimut=%1 cal=%2").arg(c->azimuth(), 0, 'f', 1).arg(c->calibrationLevel(), 0, 'f', 2);
    else
        line += " azimut=n/d";
    if (QMagnetometerReading *m = m_magnet ? m_magnet->reading() : 0)
        line += QString(" mag=(%1,%2,%3)uT cal=%4").arg(m->x() * 1e6, 0, 'f', 1).arg(m->y() * 1e6, 0, 'f', 1)
                    .arg(m->z() * 1e6, 0, 'f', 1).arg(m->calibrationLevel(), 0, 'f', 2);
    else
        line += " mag=n/d";
    if (QLightReading *l = m_light ? m_light->reading() : 0)
        line += QString(" luce=%1 lux").arg(l->lux(), 0, 'f', 0);
    else
        line += " luce=n/d";
    log(line);
    if (m_sensorSeconds >= 20) {
        m_sensorTick.stop();
        delete m_compass; m_compass = 0;
        delete m_magnet; m_magnet = 0;
        delete m_light; m_light = 0;
        log("[sens] fine");
    }
}

// ------------------------------------------------------------ GPS

void Probe::dumpSourceProperties(QObject *src, const QString &tag)
{
    const QMetaObject *mo = src->metaObject();
    QStringList parts;
    for (int i = 0; i < mo->propertyCount(); ++i) {
        QMetaProperty p = mo->property(i);
        if (QByteArray(p.name()) == "objectName") continue;
        parts << QString("%1=%2").arg(p.name()).arg(p.read(src).toString());
    }
    QList<QByteArray> dyn = src->dynamicPropertyNames();
    for (int i = 0; i < dyn.size(); ++i)
        parts << QString("%1(dyn)=%2").arg(QString(dyn.at(i))).arg(src->property(dyn.at(i)).toString());
    log(QString("[gps] %1 classe=%2 proprieta: %3").arg(tag).arg(mo->className()).arg(parts.join(", ")));
}

void Probe::setGpsProperty(const char *name, const QVariant &value)
{
    bool ok = m_pos->setProperty(name, value);
    log(QString("[gps] setProperty(%1, %2) -> %3, rilettura=%4")
            .arg(name).arg(value.toString()).arg(ok ? "proprieta dichiarata" : "dinamica")
            .arg(m_pos->property(name).toString()));
}

void Probe::startGps(const QString &mode)
{
    if (m_pos) finishGps("interrotto da nuovo test");
    log(QString("---- Test GPS modalita '%1' ----").arg(mode));
    m_pos = QGeoPositionInfoSource::createDefaultSource(this);
    if (!m_pos) {
        log("[gps] ERRORE: nessuna sorgente di posizione (permesso access_location_services?)");
        return;
    }
    emit gpsRunningChanged();
    m_gpsMode = mode;
    dumpSourceProperties(m_pos, "prima");

    // Property names/values from BlackBerry's Location Diagnostics sample.
    if (mode == "gnss" || mode == "cold" || mode == "supl") {
        m_pos->setPreferredPositioningMethods(QGeoPositionInfoSource::SatellitePositioningMethods);
        setGpsProperty("provider", "gnss");
        setGpsProperty("fixType", mode == "supl" ? "gps_ms_based" : "gps_autonomous");
        // SUPL also starts from a cold reset, so its time-to-first-fix is directly
        // comparable with the plain "cold" run (which got no fix in 180 s).
        if (mode == "cold" || mode == "supl") setGpsProperty("reset", "cold");
        // Google's public SUPL server; 7276 is the plain-text port (the phone's
        // SUPL client is configured for TLS 1.0, which 7275 likely refuses).
        // The property is declared as "slpUrl" on GeoPositionInfoSourceBb (seen
        // in the property dump): "slpURL" only created an ignored dynamic one.
        if (mode == "supl") setGpsProperty("slpUrl", "supl.google.com:7276");
    } else if (mode == "cellsite" || mode == "wifi") {
        m_pos->setPreferredPositioningMethods(QGeoPositionInfoSource::NonSatellitePositioningMethods);
        setGpsProperty("provider", "network");
        setGpsProperty("fixType", mode);
    } else {
        m_pos->setPreferredPositioningMethods(QGeoPositionInfoSource::AllPositioningMethods);
    }

    connect(m_pos, SIGNAL(positionUpdated(const QGeoPositionInfo &)),
            this, SLOT(onPositionUpdated(const QGeoPositionInfo &)));
    connect(m_pos, SIGNAL(updateTimeout()), this, SLOT(onPositionTimeout()));
    m_pos->setUpdateInterval(1000);

    m_sat = QGeoSatelliteInfoSource::createDefaultSource(this);
    if (m_sat) {
        connect(m_sat, SIGNAL(satellitesInViewUpdated(const QList<QGeoSatelliteInfo> &)),
                this, SLOT(onSatellitesInView(const QList<QGeoSatelliteInfo> &)));
        connect(m_sat, SIGNAL(satellitesInUseUpdated(const QList<QGeoSatelliteInfo> &)),
                this, SLOT(onSatellitesInUse(const QList<QGeoSatelliteInfo> &)));
        m_sat->startUpdates();
    } else {
        log("[gps] nessuna sorgente satelliti");
    }

    m_ttffMs = -1;
    m_bestAccuracy = m_lastAccuracy = -1;
    m_fixCount = 0;
    m_satInView = m_satInUse = m_bestSnr = 0;
    m_gpsClock.start();
    m_pos->startUpdates();
    m_gpsTick.start();
    log("[gps] avviato: attendo il primo fix (max 180 s). Meglio all'aperto o vicino a una finestra.");
}

void Probe::onPositionUpdated(const QGeoPositionInfo &info)
{
    ++m_fixCount;
    double acc = info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)
        ? info.attribute(QGeoPositionInfo::HorizontalAccuracy) : -1;
    m_lastAccuracy = acc;
    if (acc >= 0 && (m_bestAccuracy < 0 || acc < m_bestAccuracy)) m_bestAccuracy = acc;
    QString coord = QString("%1, %2").arg(info.coordinate().latitude(), 0, 'f', 5)
                        .arg(info.coordinate().longitude(), 0, 'f', 5);
    if (m_ttffMs < 0) {
        m_ttffMs = m_gpsClock.elapsed();
        log(QString("[gps] PRIMO FIX dopo %1 s: %2 precisione %3 m, satelliti in uso %4/%5")
                .arg(m_ttffMs / 1000.0, 0, 'f', 1).arg(coord).arg(acc, 0, 'f', 0)
                .arg(m_satInUse).arg(m_satInView));
        dumpSourceProperties(m_pos, "dopo il primo fix");
    } else if (m_fixCount % 5 == 0) {
        log(QString("[gps] fix #%1 t=%2 s: %3 precisione %4 m velocita %5 m/s")
                .arg(m_fixCount).arg(m_gpsClock.elapsed() / 1000.0, 0, 'f', 0).arg(coord).arg(acc, 0, 'f', 0)
                .arg(info.hasAttribute(QGeoPositionInfo::GroundSpeed)
                         ? info.attribute(QGeoPositionInfo::GroundSpeed) : -1, 0, 'f', 1));
    }
}

void Probe::onPositionTimeout()
{
    log(QString("[gps] updateTimeout a t=%1 s, replyErrorCode=%2 replyErrStr=%3")
            .arg(m_gpsClock.elapsed() / 1000.0, 0, 'f', 0)
            .arg(m_pos ? m_pos->property("replyErrorCode").toString() : QString())
            .arg(m_pos ? m_pos->property("replyErrStr").toString() : QString()));
}

void Probe::onSatellitesInView(const QList<QGeoSatelliteInfo> &sats)
{
    m_satInView = sats.size();
    for (int i = 0; i < sats.size(); ++i) m_bestSnr = qMax(m_bestSnr, sats.at(i).signalStrength());
}

void Probe::onSatellitesInUse(const QList<QGeoSatelliteInfo> &sats)
{
    m_satInUse = sats.size();
}

void Probe::onGpsTick()
{
    if (!m_pos) return;
    qint64 s = m_gpsClock.elapsed() / 1000;
    if (m_ttffMs < 0) {
        if (s % 10 == 0)
            log(QString("[gps] t=%1 s nessun fix, satelliti visibili %2 in uso %3, SNR max %4")
                    .arg(s).arg(m_satInView).arg(m_satInUse).arg(m_bestSnr));
        if (s >= 180) finishGps("nessun fix in 180 s");
    } else if (m_gpsClock.elapsed() - m_ttffMs >= 30000) {
        finishGps("30 s di fix raccolti");
    }
}

void Probe::finishGps(const QString &reason)
{
    if (!m_pos) return;
    m_gpsTick.stop();
    m_pos->stopUpdates();
    if (m_sat) m_sat->stopUpdates();
    log(QString("[gps] RIEPILOGO modalita '%1' (%2): TTFF=%3, fix ricevuti=%4, precisione migliore=%5 m, "
                "ultima=%6 m, satelliti visibili=%7 in uso=%8 SNR max=%9")
            .arg(m_gpsMode).arg(reason)
            .arg(m_ttffMs < 0 ? QString("nessuno") : QString("%1 s").arg(m_ttffMs / 1000.0, 0, 'f', 1))
            .arg(m_fixCount).arg(m_bestAccuracy, 0, 'f', 0).arg(m_lastAccuracy, 0, 'f', 0)
            .arg(m_satInView).arg(m_satInUse).arg(m_bestSnr));
    m_pos->deleteLater();
    m_pos = 0;
    if (m_sat) m_sat->deleteLater();
    m_sat = 0;
    emit gpsRunningChanged();
}

void Probe::stopGps()
{
    finishGps("fermato a mano");
}

// ------------------------------------------------------------ battery logger

void Probe::toggleBatteryLog()
{
    if (m_batteryTimer.isActive()) {
        m_batteryTimer.stop();
        log("[batt] registro fermato");
    } else {
        m_batteryStartLevel = m_battery->level();
        m_batteryClock.start();
        log("[batt] registro avviato: un campione al minuto (continua anche con l'app ridotta)");
        onBatteryTick();
        m_batteryTimer.start();
    }
    emit batteryLoggingChanged();
}

void Probe::onBatteryTick()
{
    double minutes = m_batteryClock.elapsed() / 60000.0;
    int used = m_batteryStartLevel - m_battery->level();
    log(QString("[batt] t=%1 min livello=%2% (usato %3%%4) temp=%5 C carica=%6 GPS=%7 %8")
            .arg(minutes, 0, 'f', 1).arg(m_battery->level()).arg(used)
            .arg(minutes >= 10 ? QString(", %1%/ora").arg(used / minutes * 60.0, 0, 'f', 1) : QString())
            .arg(m_battery->temperature(), 0, 'f', 1).arg(int(m_battery->chargingState()))
            .arg(m_pos ? m_gpsMode : QString("spento")).arg(memSummary()));
}

// ------------------------------------------------------------ run all

// Device info, GPU, network, decoding (after network, so real Google tiles are
// included if a key is present), ImageView. GPS and sensors stay manual: they
// need the user to move/hold the phone.
void Probe::runAllQuick(QObject *container)
{
    if (m_runAllStep != 0) return;
    m_runAllContainer = container;
    m_runAllStep = 1;
    log("===== Esecuzione test rapidi =====");
    runDeviceInfo();
    runGlTest();
}

void Probe::onRunAllNext()
{
    if (m_runAllStep == 0) return;
    ++m_runAllStep;
    switch (m_runAllStep) {
    case 2: runNetTest(); break;
    case 3: runDecodeTest(); break;
    case 4: runImageViewTest(m_runAllContainer); break;
    default:
        m_runAllStep = 0;
        log("===== Test rapidi completati: ora prova Sensori e GPS =====");
        break;
    }
}
