#include "networklocator.hpp"
#include "bbportlog.hpp"
#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tlsnetworkaccessmanager.hpp"
#endif

#include <QDate>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <bb/data/JsonDataAccess>
#include <bb/device/CellularNetworkInfo>
#include <bb/device/CellularTechnology>

namespace {

const int kPerDay = 100;                  // beaconDB is a volunteer-run service: stay light
const qint64 kMinGapMs = 5 * 60 * 1000;   // at most one beaconDB request every 5 minutes
const qint64 kLocalGapMs = 10000;         // local lookups: cheap, but no need for more
const int kMaxAccessPoints = 15;
const double kLearnAccuracy = 25;         // metres: only good GPS fixes teach the cache
const double kLearnMaxSpeed = 3;          // m/s: the system scan may be seconds old
const qint64 kLearnGapMs = 10000;

QVariantList wifiAccessPoints(const QList<WifiObservation> &seen)
{
    QVariantList aps;
    for (int i = 0; i < seen.size() && aps.size() < kMaxAccessPoints; ++i) {
        const quint64 b = seen.at(i).bssid;
        QVariantMap ap;
        ap["macAddress"] = QString().sprintf("%02x:%02x:%02x:%02x:%02x:%02x",
                                             uint(b >> 40) & 0xff, uint(b >> 32) & 0xff, uint(b >> 24) & 0xff,
                                             uint(b >> 16) & 0xff, uint(b >> 8) & 0xff, uint(b) & 0xff);
        ap["signalStrength"] = seen.at(i).rssi;
        aps << ap;
    }
    return aps;
}

QVariantList cellTowers()
{
    QVariantList towers;
    bb::device::CellularNetworkInfo info;
    QString radio;
    int area = -1;
    switch (info.technology()) {
    case bb::device::CellularTechnology::Gsm:
        radio = "gsm"; area = info.locationAreaCode(); break;
    case bb::device::CellularTechnology::Umts:
        radio = "wcdma"; area = info.locationAreaCode(); break;
    case bb::device::CellularTechnology::Lte:
        radio = "lte"; area = info.trackingAreaCode(); break;
    default:
        return towers;   // no network, or CDMA (not used in Italy)
    }
    bool mccOk = false, mncOk = false;
    int mcc = info.mobileCountryCode().toInt(&mccOk);
    int mnc = info.mobileNetworkCode().toInt(&mncOk);
    if (info.cellId() <= 0 || area < 0 || !mccOk || !mncOk) return towers;
    QVariantMap t;
    t["radioType"] = radio;
    t["cellId"] = info.cellId();
    t["locationAreaCode"] = area;
    t["mobileCountryCode"] = mcc;
    t["mobileNetworkCode"] = mnc;
    towers << t;
    return towers;
}

} // namespace

NetworkLocator::NetworkLocator(QObject *parent) :
    QObject(parent), m_nam(0)
{
#ifdef BBPORT_HAVE_NATIVE_TLS
    m_nam = new TlsNetworkAccessManager(this);
#else
    m_nam = new QNetworkAccessManager(this);
#endif
    // Qt 4.8's QElapsedTimer has no constructor: until invalidated it holds
    // garbage, and isValid()/elapsed() made locate() skip every attempt.
    m_sinceRequest.invalidate();
    m_sinceLocal.invalidate();
    m_sinceLearn.invalidate();
}

bool NetworkLocator::allow()
{
    QSettings s;
    QString today = QDate::currentDate().toString(Qt::ISODate);
    int count = s.value("geolocation/day").toString() == today ? s.value("geolocation/count").toInt() : 0;
    if (count >= kPerDay) return false;
    s.setValue("geolocation/day", today);
    s.setValue("geolocation/count", count + 1);
    return true;
}

void NetworkLocator::learn(double lat, double lon, double accuracy, double speed)
{
    if (accuracy <= 0 || accuracy > kLearnAccuracy || speed > kLearnMaxSpeed) return;
    if (m_sinceLearn.isValid() && m_sinceLearn.elapsed() < kLearnGapMs) return;
    m_sinceLearn.start();
    QList<WifiObservation> seen = WifiCache::scan();
    if (!seen.isEmpty()) m_cache.learn(seen, lat, lon, accuracy);
}

bool NetworkLocator::locate()
{
    // Offline first: access points this phone has already seen with the GPS on.
    if (!m_sinceLocal.isValid() || m_sinceLocal.elapsed() >= kLocalGapMs) {
        m_sinceLocal.start();
        QList<WifiObservation> seen = WifiCache::scan();
        double lat, lon, acc;
        int used = 0;
        if (m_cache.estimate(seen, &lat, &lon, &acc, &used)) {
            bbportLog(QString("[net] posizione da Wi-Fi memorizzate: %1 reti note su %2, precisione %3 m")
                          .arg(used).arg(seen.size()).arg(acc, 0, 'f', 0));
            emit located(lat, lon, acc);
            return true;
        }
    }

    if (m_reply) return false;
    if (m_sinceRequest.isValid() && m_sinceRequest.elapsed() < kMinGapMs) return false;
    m_sinceRequest.start();   // the gap counts attempts too: callers retry from a timer

    QVariantList aps = wifiAccessPoints(WifiCache::scan());
    QVariantList towers = cellTowers();
    if (aps.isEmpty() && towers.isEmpty()) {
        bbportLog("[net] posizione da rete: nessuna rete Wi-Fi o cella leggibile");
        return false;
    }
    if (!allow()) {
        bbportLog("[net] posizione da rete: limite giornaliero raggiunto");
        return false;
    }

    QVariantMap body, fallbacks;
    fallbacks["ipf"] = false;    // no IP guess
    fallbacks["lacf"] = true;    // cell area when the cell itself is unknown
    body["fallbacks"] = fallbacks;
    if (!aps.isEmpty()) body["wifiAccessPoints"] = aps;
    if (!towers.isEmpty()) body["cellTowers"] = towers;
    QByteArray json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(body, &json);

    QNetworkRequest req(QUrl("https://api.beacondb.net/v1/geolocate"));
    req.setRawHeader("Content-Type", "application/json");
    // beaconDB asks every client to identify itself.
    req.setRawHeader("User-Agent", "BerryMaps/0.1 (BlackBerry 10)");
    m_reply = m_nam->post(req, json);
    connect(m_reply, SIGNAL(finished()), this, SLOT(onReply()));
    bbportLog(QString("[net] posizione da rete: %1 reti Wi-Fi, %2 celle").arg(aps.size()).arg(towers.size()));
    return true;
}

void NetworkLocator::onReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r != m_reply) return;
    m_reply = 0;
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bb::data::JsonDataAccess jda;
    QVariantMap res = jda.loadFromBuffer(r->readAll()).toMap();
    QVariantMap loc = res.value("location").toMap();
    bool latOk = false, lonOk = false;
    double lat = loc.value("lat").toDouble(&latOk);
    double lon = loc.value("lng").toDouble(&lonOk);
    double acc = res.value("accuracy").toDouble();
    if (status != 200 || !latOk || !lonOk) {
        // 404 = beaconDB knows none of these networks, nor the cell's area.
        bbportLog(QString("[net] posizione da rete non riuscita (HTTP %1)").arg(status));
        emit failed(status == 404 ? QString("reti sconosciute") : QString("HTTP %1").arg(status));
        return;
    }
    // "fallback" is set when the answer comes from the cell area, not the networks.
    QString fallback = res.value("fallback").toString();
    bbportLog(QString("[net] posizione da rete: precisione %1 m%2").arg(acc, 0, 'f', 0)
                  .arg(fallback.isEmpty() ? QString() : QString(" (ripiego: %1)").arg(fallback)));
    emit located(lat, lon, acc);
}
