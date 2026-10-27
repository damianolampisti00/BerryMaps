#include "beaconsubmitter.hpp"
#include "bbportlog.hpp"
#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tlsnetworkaccessmanager.hpp"
#endif

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>
#include <QtCore/qmath.h>

#include <bb/data/JsonDataAccess>

namespace {

const double kMinMoveM = 25;
const int kBatch = 20;
const qint64 kMaxWaitMs = 10 * 60 * 1000;
const int kMaxPending = 500;
const int kMaxPerRequest = 50;

double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double k = M_PI / 180.0, r = 6371000.0;
    double x = (lon2 - lon1) * k * qCos((lat1 + lat2) / 2 * k);
    double y = (lat2 - lat1) * k;
    return r * qSqrt(x * x + y * y);
}

} // namespace

BeaconSubmitter::BeaconSubmitter(QObject *parent) :
    QObject(parent), m_nam(0), m_inFlight(0), m_enabled(false), m_sent(0),
    m_haveLast(false), m_lastLat(0), m_lastLon(0)
{
#ifdef BBPORT_HAVE_NATIVE_TLS
    m_nam = new TlsNetworkAccessManager(this);
#else
    m_nam = new QNetworkAccessManager(this);
#endif
    QSettings s;
    m_enabled = s.value("beacondb/contribute", false).toBool();
    m_sent = s.value("beacondb/sent", 0).toInt();
    m_sinceFirstPending.invalidate();   // Qt 4.8: no constructor, garbage otherwise
}

void BeaconSubmitter::setEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    QSettings().setValue("beacondb/contribute", on);
    if (!on) {
        // Nothing queued under "on" leaves the phone after "off".
        m_pending.clear();
        m_sinceFirstPending.invalidate();
    }
    bbportLog(on ? "[beacondb] invio dati attivato" : "[beacondb] invio dati disattivato");
    emit changed();
}

void BeaconSubmitter::add(double lat, double lon, double accuracy, double speed,
                          const QVariantList &aps, const QVariantList &towers)
{
    if (!m_enabled || (aps.isEmpty() && towers.isEmpty())) return;
    if (m_haveLast && distanceM(m_lastLat, m_lastLon, lat, lon) < kMinMoveM) return;
    m_haveLast = true;
    m_lastLat = lat;
    m_lastLon = lon;

    QVariantMap position;
    position["latitude"] = lat;
    position["longitude"] = lon;
    position["accuracy"] = accuracy;
    position["speed"] = speed;
    position["source"] = "gps";
    QVariantMap item;
    item["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    item["position"] = position;
    if (!aps.isEmpty()) item["wifiAccessPoints"] = aps;
    if (!towers.isEmpty()) item["cellTowers"] = towers;
    if (m_pending.isEmpty()) m_sinceFirstPending.start();
    m_pending << item;
    while (m_pending.size() > kMaxPending) m_pending.removeFirst();

    if (m_pending.size() >= kBatch || m_sinceFirstPending.elapsed() >= kMaxWaitMs) submit();
}

void BeaconSubmitter::submit()
{
    if (m_reply || m_pending.isEmpty()) return;
    m_inFlight = qMin(m_pending.size(), kMaxPerRequest);
    QVariantMap body;
    body["items"] = m_pending.mid(0, m_inFlight);
    QByteArray json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(body, &json);

    QNetworkRequest req(QUrl("https://api.beacondb.net/v2/geosubmit"));
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("User-Agent", "BerryMaps/0.1 (BlackBerry 10)");
    m_reply = m_nam->post(req, json);
    connect(m_reply, SIGNAL(finished()), this, SLOT(onReply()));
}

void BeaconSubmitter::onReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r != m_reply) return;
    m_reply = 0;
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        bbportLog(QString("[beacondb] invio non riuscito (HTTP %1), %2 misure in coda")
                      .arg(status).arg(m_pending.size()));
        m_sinceFirstPending.start();   // retry with the next measurement after the wait
        return;
    }
    // Turned off meanwhile: the queue was already emptied.
    const int done = qMin(m_inFlight, m_pending.size());
    for (int i = 0; i < done; ++i) m_pending.removeFirst();
    m_sent += m_inFlight;
    QSettings().setValue("beacondb/sent", m_sent);
    bbportLog(QString("[beacondb] inviate %1 misure (totale %2)").arg(m_inFlight).arg(m_sent));
    if (!m_pending.isEmpty()) m_sinceFirstPending.start();
    else m_sinceFirstPending.invalidate();
    emit changed();
}
