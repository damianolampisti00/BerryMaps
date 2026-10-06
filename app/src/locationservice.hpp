#ifndef LOCATIONSERVICE_HPP_
#define LOCATIONSERVICE_HPP_

#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

#include <QtLocationSubset/QGeoPositionInfoSource>
#include <QtSensors/QCompass>
#include <QtSensors/QMagnetometer>

// Unqualified names in signal/slot signatures: Qt 4 matches SIGNAL()/SLOT()
// strings textually and the Mobility headers declare them unqualified.
QTMS_USE_NAMESPACE
QTM_USE_NAMESPACE

// GPS + compass for the "you are here" dot (PROGETTO.md §6, §17.4b, §17.4c).
//
// Measured on the Q5: the default (hybrid) source never fixes and BB10's
// network positioning is dead, so this always asks for provider=gnss,
// fixType=gps_autonomous (assistance comes from XTRA). The BB10 source's own
// stationary detection pauses updates while the phone is still.
//
// Runs only while wanted AND the app is in the foreground with the screen on.
// Heading: GPS course when moving (> 2 m/s), compass when still - and only if
// the compass reports itself calibrated and the magnetic field is plausible
// (a nearby magnet read ~550 uT and froze the compass in the sensor test).
class LocationService : public QObject
{
    Q_OBJECT
public:
    explicit LocationService(QObject *parent = 0);
    virtual ~LocationService();

    void setWanted(bool wanted);        // user turned "my location" on
    void setForeground(bool fg);        // app fullscreen and screen awake
    void setFast(bool fast);            // 1 s updates (following) vs 5 s

    bool hasFix() const { return m_hasFix; }
    double latitude() const { return m_lat; }
    double longitude() const { return m_lon; }
    double accuracy() const { return m_accuracy; }      // metres, -1 unknown
    double speed() const { return m_speed; }            // m/s
    bool headingValid() const { return m_headingValid; }
    double heading() const { return m_heading; }        // degrees from north
    // Safety requirement: an old or imprecise position must not look precise.
    bool reliable() const;
    QString status() const { return m_status; }

signals:
    void updated();
    void statusChanged();

private slots:
    void onPosition(const QGeoPositionInfo &info);
    void onTimeout();
    void onTick();

private:
    void apply();
    void start();
    void stop();
    void setStatus(const QString &s);
    void updateCompassHeading();

    QGeoPositionInfoSource *m_src;
    QCompass *m_compass;
    QMagnetometer *m_magnet;
    QTimer m_tick;
    QElapsedTimer m_sinceFix;
    QElapsedTimer m_sinceStart;
    bool m_wanted, m_foreground, m_fast, m_running;
    bool m_hasFix, m_stationary;
    bool m_fixThisRun;          // stationary detection is armed only after it
    double m_lat, m_lon, m_accuracy, m_speed;
    bool m_headingValid;
    double m_heading;
    bool m_gpsHeading;
    QString m_status;
};

#endif /* LOCATIONSERVICE_HPP_ */
