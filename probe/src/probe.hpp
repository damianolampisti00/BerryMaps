#ifndef PROBE_HPP_
#define PROBE_HPP_

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantMap>
#include <QNetworkReply>

#include <QtLocationSubset/QGeoPositionInfoSource>
#include <QtLocationSubset/QGeoSatelliteInfoSource>
#include <QtSensors/QCompass>
#include <QtSensors/QMagnetometer>
#include <QtSensors/QLightSensor>

#include <bb/cascades/ImageView>
#include <bb/cascades/ImageTracker>
#include <bb/cascades/Container>
#include <bb/cascades/ResourceState>

// Unqualified type names in signal/slot signatures: Qt 4 matches SIGNAL()/SLOT()
// strings textually, and the location/sensor headers declare their signals with
// the bare names (inside their namespaces).
QTMS_USE_NAMESPACE
QTM_USE_NAMESPACE

class QNetworkAccessManager;
namespace bb { class MemoryInfo; namespace device { class BatteryInfo; } }

// Fase 0 diagnostics (see ../../PROGETTO.md, section 13). Every test writes to
// berryprobe.log (app data dir + shared/misc) and to the on-screen log through
// logLine(). Tests are independent; the GPS test can run alongside the others.
class Probe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool batteryLogging READ batteryLogging NOTIFY batteryLoggingChanged)
    Q_PROPERTY(bool gpsRunning READ gpsRunning NOTIFY gpsRunningChanged)
public:
    explicit Probe(QObject *parent = 0);
    virtual ~Probe();

    bool batteryLogging() const { return m_batteryTimer.isActive(); }
    bool gpsRunning() const { return m_pos != 0; }

    Q_INVOKABLE void runDeviceInfo();
    Q_INVOKABLE void runGlTest();
    Q_INVOKABLE void runDecodeTest();
    Q_INVOKABLE void runImageViewTest(QObject *container);
    Q_INVOKABLE void runNetTest();
    Q_INVOKABLE void runSensorTest();
    // mode: default | gnss | cold | supl | cellsite | wifi
    Q_INVOKABLE void startGps(const QString &mode);
    Q_INVOKABLE void stopGps();
    Q_INVOKABLE void toggleBatteryLog();
    Q_INVOKABLE void runAllQuick(QObject *container);

public slots:
    void log(const QString &line);

signals:
    void logLine(const QString &line);
    void batteryLoggingChanged();
    void gpsRunningChanged();
    void imageViewTestRunning(bool running);

private slots:
    // GPS
    void onPositionUpdated(const QGeoPositionInfo &info);
    void onPositionTimeout();
    void onSatellitesInView(const QList<QGeoSatelliteInfo> &sats);
    void onSatellitesInUse(const QList<QGeoSatelliteInfo> &sats);
    void onGpsTick();
    // sensors
    void onSensorTick();
    // battery
    void onBatteryTick();
    // ImageView test
    void onTrackerStateChanged(bb::cascades::ResourceState::Type state);
    void onImageViewPhase();
    // chained "run all" steps
    void onRunAllNext();

private:
    qint64 processMemKB() const;
    qint64 availableMemKB() const;
    QString memSummary() const;
    QString apiKey() const;
    QString tilesDir() const;
    QStringList decodeFiles() const;
    void dumpSourceProperties(QObject *src, const QString &tag);
    void setGpsProperty(const char *name, const QVariant &value);
    void finishGps(const QString &reason);
    // blocking helper (nested event loop) used only by the network test
    QNetworkReply *waitFor(QNetworkReply *reply, int timeoutMs, double *ms);
    void logReplyHeaders(QNetworkReply *reply);
    void startImageViewPass();

    QNetworkAccessManager *m_nam;
    bb::MemoryInfo *m_mem;
    bb::device::BatteryInfo *m_battery;
    QTimer m_batteryTimer;
    QElapsedTimer m_batteryClock;
    int m_batteryStartLevel;

    // GPS state
    QGeoPositionInfoSource *m_pos;
    QGeoSatelliteInfoSource *m_sat;
    QString m_gpsMode;
    QElapsedTimer m_gpsClock;
    QTimer m_gpsTick;
    qint64 m_ttffMs;
    double m_bestAccuracy;
    double m_lastAccuracy;
    int m_fixCount;
    int m_satInView, m_satInUse;
    int m_bestSnr;
    bool m_netTestRunning;

    // sensors
    QCompass *m_compass;
    QMagnetometer *m_magnet;
    QLightSensor *m_light;
    QTimer m_sensorTick;
    int m_sensorSeconds;

    // ImageView test
    bb::cascades::Container *m_ivContainer;
    QList<bb::cascades::ImageTracker *> m_trackers;
    QList<bb::cascades::ImageView *> m_views;
    QElapsedTimer m_ivClock;
    // One single-shot timer for every phase delay: restarting it cancels the
    // previous one, so a stale 20 s safety timeout can't fire into pass 2.
    QTimer m_ivTimer;
    int m_ivLoaded, m_ivFailed, m_ivPass, m_ivPhase;
    qint64 m_ivMemBefore;

    // run all
    QObject *m_runAllContainer;
    int m_runAllStep;
};

#endif /* PROBE_HPP_ */
