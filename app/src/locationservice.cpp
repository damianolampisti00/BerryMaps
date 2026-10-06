#include "locationservice.hpp"
#include "bbportlog.hpp"

#include <QtCore/qmath.h>
#include <bb/location/PositionErrorCode>

namespace {

const int kFastIntervalMs = 1000;
const int kSlowIntervalMs = 5000;
const int kStaleMs = 15000;            // no fix for this long -> grey dot (unless stationary)
const double kMaxReliableAccuracy = 50; // metres
const double kMovingSpeed = 2.0;       // m/s: above this the GPS course is the heading
const double kMinField = 20e-6, kMaxField = 70e-6;  // tesla; Earth's field in Italy ~46 uT
const int kWarnStationary = 0x10002;   // bb::location::PositionErrorCode::WarnStationary

} // namespace

LocationService::LocationService(QObject *parent) :
    QObject(parent), m_src(0), m_compass(0), m_magnet(0),
    m_wanted(false), m_foreground(true), m_fast(false), m_running(false),
    m_hasFix(false), m_stationary(false), m_fixThisRun(false),
    m_lat(0), m_lon(0), m_accuracy(-1), m_speed(0),
    m_headingValid(false), m_heading(0), m_gpsHeading(false)
{
    m_tick.setInterval(300);   // compass refresh + staleness check
    connect(&m_tick, SIGNAL(timeout()), this, SLOT(onTick()));
}

LocationService::~LocationService()
{
    stop();
}

void LocationService::setWanted(bool wanted) { m_wanted = wanted; apply(); }
void LocationService::setForeground(bool fg) { m_foreground = fg; apply(); }

void LocationService::setFast(bool fast)
{
    if (fast == m_fast) return;
    m_fast = fast;
    if (m_src) m_src->setUpdateInterval(m_fast ? kFastIntervalMs : kSlowIntervalMs);
}

bool LocationService::reliable() const
{
    if (!m_hasFix) return false;
    if (m_accuracy < 0 || m_accuracy > kMaxReliableAccuracy) return false;
    return m_stationary || m_sinceFix.elapsed() <= kStaleMs;
}

void LocationService::setStatus(const QString &s)
{
    if (s == m_status) return;
    m_status = s;
    if (!s.isEmpty()) bbportLog("[gps] " + s);
    emit statusChanged();
}

void LocationService::apply()
{
    bool shouldRun = m_wanted && m_foreground;
    if (shouldRun && !m_running) start();
    else if (!shouldRun && m_running) stop();
}

void LocationService::start()
{
    m_src = QGeoPositionInfoSource::createDefaultSource(this);
    if (!m_src) {
        setStatus("GPS non disponibile (permesso posizione?)");
        return;
    }
    if (!m_src->property("locationServicesEnabled").toBool()) {
        setStatus("Localizzazione disattivata nelle impostazioni del telefono");
        delete m_src;
        m_src = 0;
        return;
    }
    // Settings proven in BerryProbe (PROGETTO.md §17.4c).
    m_src->setPreferredPositioningMethods(QGeoPositionInfoSource::SatellitePositioningMethods);
    m_src->setProperty("provider", "gnss");
    m_src->setProperty("fixType", "gps_autonomous");
    // No stationary detection yet: with the phone lying still it paused updates
    // before the first fix was ever delivered (the dot never appeared on the
    // Q5). It is switched on in onPosition() once this run has a fix.
    m_fixThisRun = false;
    m_src->setUpdateInterval(m_fast ? kFastIntervalMs : kSlowIntervalMs);
    connect(m_src, SIGNAL(positionUpdated(const QGeoPositionInfo &)),
            this, SLOT(onPosition(const QGeoPositionInfo &)));
    connect(m_src, SIGNAL(updateTimeout()), this, SLOT(onTimeout()));
    m_src->startUpdates();

    m_compass = new QCompass(this);
    m_compass->setSkipDuplicates(true);
    m_magnet = new QMagnetometer(this);
    m_magnet->setSkipDuplicates(true);
    m_compass->start();
    m_magnet->start();

    m_running = true;
    m_stationary = false;
    m_sinceStart.start();
    m_tick.start();
    if (!m_hasFix) setStatus(QString::fromUtf8("Ricerca segnale GPS…"));
    bbportLog(QString("[gps] avviato (%1 ms)").arg(m_fast ? kFastIntervalMs : kSlowIntervalMs));
}

void LocationService::stop()
{
    m_tick.stop();
    if (m_src) {
        m_src->stopUpdates();
        delete m_src;
        m_src = 0;
    }
    delete m_compass;
    m_compass = 0;
    delete m_magnet;
    m_magnet = 0;
    if (m_running) bbportLog("[gps] fermato");
    m_running = false;
    setStatus(QString());
}

void LocationService::onPosition(const QGeoPositionInfo &info)
{
    if (!info.coordinate().isValid()) return;
    bool first = !m_hasFix;
    m_hasFix = true;
    m_stationary = false;
    m_sinceFix.start();
    m_lat = info.coordinate().latitude();
    m_lon = info.coordinate().longitude();
    m_accuracy = info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)
        ? info.attribute(QGeoPositionInfo::HorizontalAccuracy) : -1;
    m_speed = info.hasAttribute(QGeoPositionInfo::GroundSpeed)
        ? info.attribute(QGeoPositionInfo::GroundSpeed) : 0;
    if (m_speed > kMovingSpeed && info.hasAttribute(QGeoPositionInfo::Direction)) {
        m_heading = info.attribute(QGeoPositionInfo::Direction);
        m_headingValid = true;
        m_gpsHeading = true;
    } else {
        m_gpsHeading = false;
        updateCompassHeading();
    }
    if (!m_fixThisRun) {
        m_fixThisRun = true;
        bbportLog(QString("[gps] primo fix dopo %1 ms, precisione %2 m")
                      .arg(m_sinceStart.elapsed()).arg(m_accuracy, 0, 'f', 0));
        // Now that there is a position, let the location manager pause updates
        // while the phone is still (battery); the property is read at start.
        m_src->stopUpdates();
        m_src->setProperty("stationaryDetectionEnabled", true);
        m_src->startUpdates();
    }
    Q_UNUSED(first);
    setStatus(m_accuracy > kMaxReliableAccuracy ? "Segnale GPS debole: posizione approssimativa" : QString());
    emit updated();
}

void LocationService::onTimeout()
{
    if (!m_src) return;
    // The property is a bb::location::PositionErrorCode::Type: QVariant::toInt()
    // can't convert that registered enum and always gave 0 on the Q5, so the
    // "stationary" warning was never recognised.
    QVariant v = m_src->property("replyErrorCode");
    int code = v.canConvert<bb::location::PositionErrorCode::Type>()
        ? int(v.value<bb::location::PositionErrorCode::Type>()) : v.toInt();
    bbportLog(QString("[gps] timeout a %1 s, replyErrorCode=0x%2 %3")
                  .arg(m_sinceStart.elapsed() / 1000).arg(code, 0, 16)
                  .arg(m_src->property("replyErrStr").toString()));
    if (code == kWarnStationary) {
        // Updates paused because the phone is still: the last fix stays valid.
        m_stationary = true;
        emit updated();
        return;
    }
    if (!m_hasFix) setStatus(QString::fromUtf8("Ricerca segnale GPS…"));
}

void LocationService::updateCompassHeading()
{
    if (m_gpsHeading) return;
    bool valid = false;
    QCompassReading *c = m_compass ? m_compass->reading() : 0;
    QMagnetometerReading *m = m_magnet ? m_magnet->reading() : 0;
    if (c && m) {
        double field = qSqrt(m->x() * m->x() + m->y() * m->y() + m->z() * m->z());
        valid = c->calibrationLevel() >= 0.5 && field >= kMinField && field <= kMaxField;
        if (valid) m_heading = c->azimuth();
    }
    m_headingValid = valid;
}

void LocationService::onTick()
{
    if (!m_hasFix) return;
    double before = m_heading;
    bool beforeValid = m_headingValid;
    bool beforeReliable = reliable();
    updateCompassHeading();
    if (!m_stationary && m_sinceFix.elapsed() > kStaleMs)
        setStatus("Posizione non aggiornata: segnale GPS perso");
    if (m_headingValid != beforeValid || qAbs(m_heading - before) > 2.0 || reliable() != beforeReliable)
        emit updated();
}
