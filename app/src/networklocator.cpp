#include "networklocator.hpp"
#include "bbportlog.hpp"
#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tlsnetworkaccessmanager.hpp"
#endif

#include <QDate>
#include <QDir>
#include <QFile>
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

#include <wifi/wifi_service.h>

namespace {

const int kPerDay = 300;                  // PROGETTO.md §22: Geolocation 300/day
const qint64 kMinGapMs = 5 * 60 * 1000;   // at most one request every 5 minutes
const int kMaxAccessPoints = 15;

QVariantList wifiAccessPoints()
{
    QVariantList aps;
    wifi_scan_results_t *results = 0;
    wifi_scan_report_t report;
    int count = 0;
    // The latest scan the system made; with Wi-Fi off the list is empty.
    if (wifi_get_scan_results(&results, &report, &count) != WIFI_SUCCESS || !results)
        return aps;
    for (int i = 1; i <= count && aps.size() < kMaxAccessPoints; ++i) {
        char ssid[WIFI_MAX_SSID_BUFFER_LEN] = { 0 };
        uint8_t bssid[WIFI_BSSID_ADDR_LEN];
        int level = 0;
        if (wifi_get_scan_result_bssid(results, i, bssid) != WIFI_SUCCESS) continue;
        if (wifi_get_scan_result_ssid(results, i, ssid) == WIFI_SUCCESS
            && QString::fromUtf8(ssid).endsWith("_nomap")) continue;
        QVariantMap ap;
        ap["macAddress"] = QString().sprintf("%02x:%02x:%02x:%02x:%02x:%02x",
                                             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        if (wifi_get_scan_result_signal_level(results, i, &level) == WIFI_SUCCESS)
            ap["signalStrength"] = level;
        aps << ap;
    }
    wifi_free_scan_results(results);
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
    const QString keyFiles[2] = { QString("/accounts/1000/shared/misc/berrymaps_apikey.txt"),
                                  QDir::homePath() + "/berrymaps_apikey.txt" };
    for (int i = 0; i < 2 && m_apiKey.isEmpty(); ++i) {
        QFile f(keyFiles[i]);
        if (f.open(QIODevice::ReadOnly)) m_apiKey = QString::fromUtf8(f.readAll()).trimmed();
    }
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

bool NetworkLocator::locate()
{
    if (m_apiKey.isEmpty() || m_reply) return false;
    if (m_sinceRequest.isValid() && m_sinceRequest.elapsed() < kMinGapMs) return false;
    m_sinceRequest.start();   // the gap counts attempts too: callers retry from a timer

    QVariantList aps = wifiAccessPoints();
    QVariantList towers = cellTowers();
    if (aps.isEmpty() && towers.isEmpty()) {
        bbportLog("[net] posizione da rete: nessuna rete Wi-Fi o cella leggibile");
        return false;
    }
    if (!allow()) {
        bbportLog("[net] posizione da rete: limite giornaliero raggiunto");
        return false;
    }

    QVariantMap body;
    body["considerIp"] = false;
    if (!aps.isEmpty()) body["wifiAccessPoints"] = aps;
    if (!towers.isEmpty()) body["cellTowers"] = towers;
    QByteArray json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(body, &json);

    QUrl url("https://www.googleapis.com/geolocation/v1/geolocate");
    url.addQueryItem("key", m_apiKey);   // the TLS log strips queries
    QNetworkRequest req(url);
    req.setRawHeader("Content-Type", "application/json");
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
        // 404 = Google knows none of these networks.
        bbportLog(QString("[net] posizione da rete non riuscita (HTTP %1)").arg(status));
        emit failed(status == 404 ? QString("reti sconosciute") : QString("HTTP %1").arg(status));
        return;
    }
    bbportLog(QString("[net] posizione da rete: precisione %1 m").arg(acc, 0, 'f', 0));
    emit located(lat, lon, acc);
}
