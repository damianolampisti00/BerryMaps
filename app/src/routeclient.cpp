#include "routeclient.hpp"
#include "polyline.hpp"
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

#include <bb/data/JsonDataAccess>

namespace {

// Rerouting every 15 s while driving (EEA safety requirement) is ~240/hour:
// this app-side cap keeps a long day of driving inside the free tier.
const int kRoutesPerDay = 600;

QString formatDistance(double m)
{
    if (m < 1000) return QString("%1 m").arg(qRound(m / 10.0) * 10);
    return QString("%1 km").arg(m / 1000.0, 0, 'f', m < 10000 ? 1 : 0).replace('.', ',');
}

QString lineLabel(const QString &type, const QString &shortName, const QString &name)
{
    QString s = shortName.isEmpty() ? name : shortName;
    if (type == "SUBWAY" || type == "METRO_RAIL") return "M" + s;
    if (type == "BUS" || type == "INTERCITY_BUS" || type == "TROLLEYBUS") return "Bus " + s;
    if (type == "TRAM" || type == "LIGHT_RAIL") return "Tram " + s;
    if (type.contains("RAIL") || type.contains("TRAIN")) return "Treno " + s;
    return s;
}

QString formatDuration(int secs)
{
    int min = qRound(secs / 60.0);
    if (min < 60) return QString("%1 min").arg(qMax(1, min));
    return QString("%1 h %2 min").arg(min / 60).arg(min % 60);
}

} // namespace

RouteClient::RouteClient(QObject *parent) :
    QObject(parent), m_nam(0), m_busy(false), m_mode("DRIVE"), m_toLat(0), m_toLon(0),
    m_fromLat(0), m_fromLon(0), m_timeKind("now"), m_durationS(0)
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

QString RouteClient::summary() const
{
    if (m_points.isEmpty()) return QString();
    const QString sep = QString::fromUtf8("  \u00b7  ");
    if (m_mode == "TRANSIT") {
        QStringList lines;
        QString departure;
        for (int i = 0; i < m_steps.size(); ++i) {
            if (!m_steps.at(i).transit) continue;
            lines << m_steps.at(i).line;
            if (departure.isEmpty()) departure = m_steps.at(i).depTime;
        }
        QString s = formatDuration(m_durationS);
        if (!lines.isEmpty()) s += sep + lines.join(QString::fromUtf8(" \u2192 "));
        if (!departure.isEmpty()) s += sep + "parte alle " + departure;
        return s;
    }
    return formatDuration(m_durationS) + sep + formatDistance(totalMetres());
}

QVariantList RouteClient::stepsForUi() const
{
    QVariantList list;
    for (int i = 0; i < m_steps.size(); ++i) {
        const RouteStep &s = m_steps.at(i);
        QVariantMap m;
        if (s.transit) {
            m["instruction"] = s.vehicle + " " + s.line.section(' ', -1) +
                               (s.lineName.isEmpty() ? QString() : QString::fromUtf8(" \u00b7 ") + s.lineName) +
                               QString::fromUtf8(" \u2192 ") + s.headsign;
            m["distance"] = s.depTime + " " + s.depStop + QString::fromUtf8(" \u2192 ") + s.arrTime + " " + s.arrStop +
                            QString::fromUtf8(" \u00b7 ") + QString::number(s.stops) + (s.stops == 1 ? " fermata" : " fermate");
        } else {
            m["instruction"] = s.instruction;
            m["distance"] = s.lengthM > 0 ? formatDistance(s.lengthM) : QString();
        }
        list << m;
    }
    return list;
}

void RouteClient::setBusy(bool busy)
{
    if (busy == m_busy) return;
    m_busy = busy;
    emit busyChanged();
}

bool RouteClient::allow()
{
    QSettings s;
    QString today = QDate::currentDate().toString(Qt::ISODate);
    int count = s.value("routes/day").toString() == today ? s.value("routes/count").toInt() : 0;
    if (count >= kRoutesPerDay) {
        emit error("Limite giornaliero di percorsi raggiunto, riprova domani.");
        return false;
    }
    s.setValue("routes/day", today);
    s.setValue("routes/count", count + 1);
    return true;
}

void RouteClient::plan(double fromLat, double fromLon, double toLat, double toLon, const QString &mode)
{
    m_toLat = toLat;
    m_toLon = toLon;
    m_mode = mode;
    request(fromLat, fromLon, false);
}

void RouteClient::changeMode(double fromLat, double fromLon, const QString &mode)
{
    if (m_points.isEmpty() && !m_busy) return;
    m_mode = mode;
    request(fromLat, fromLon, false);
}

void RouteClient::setTime(const QString &kind, const QDateTime &when)
{
    m_timeKind = kind;
    m_time = when;
    if (m_mode == "TRANSIT" && (!m_points.isEmpty() || m_busy)) request(m_fromLat, m_fromLon, false);
}

void RouteClient::replan(double fromLat, double fromLon)
{
    if (m_points.isEmpty()) return;
    request(fromLat, fromLon, true);
}

void RouteClient::clear()
{
    if (m_reply) m_reply->abort();
    m_points.clear();
    m_cum.clear();
    m_steps.clear();
    m_segColor.clear();
    m_segWalk.clear();
    m_durationS = 0;
    setBusy(false);
    emit routeChanged();
}

void RouteClient::request(double fromLat, double fromLon, bool quiet)
{
    if (m_apiKey.isEmpty()) {
        emit error("Chiave Google mancante: /accounts/1000/shared/misc/berrymaps_apikey.txt");
        return;
    }
    if (!allow()) return;
    if (m_reply) m_reply->abort();
    m_fromLat = fromLat;
    m_fromLon = fromLon;

    QVariantMap origin, dest, oLatLng, dLatLng, oLoc, dLoc, body;
    oLatLng["latitude"] = fromLat;
    oLatLng["longitude"] = fromLon;
    oLoc["latLng"] = oLatLng;
    origin["location"] = oLoc;
    dLatLng["latitude"] = m_toLat;
    dLatLng["longitude"] = m_toLon;
    dLoc["latLng"] = dLatLng;
    dest["location"] = dLoc;
    body["origin"] = origin;
    body["destination"] = dest;
    body["travelMode"] = m_mode;
    if (m_mode == "DRIVE") body["routingPreference"] = "TRAFFIC_UNAWARE";   // Essentials
    if (m_mode == "TRANSIT" && m_timeKind != "now" && m_time.isValid()) {
        // RFC 3339 in UTC, e.g. 2026-10-05T20:25:00Z
        QString iso = m_time.toUTC().toString("yyyy-MM-ddTHH:mm:ss") + "Z";
        body[m_timeKind == "arrive" ? "arrivalTime" : "departureTime"] = iso;
    }
    body["languageCode"] = "it-IT";
    body["units"] = "METRIC";
    QByteArray json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(body, &json);

    QNetworkRequest req(QUrl("https://routes.googleapis.com/directions/v2:computeRoutes"));
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("X-Goog-Api-Key", m_apiKey.toUtf8());
    req.setRawHeader("X-Goog-FieldMask",
                     "routes.duration,routes.distanceMeters,routes.polyline.encodedPolyline,"
                     "routes.legs.steps.distanceMeters,routes.legs.steps.navigationInstruction,"
                     "routes.legs.steps.polyline.encodedPolyline,"
                     "routes.legs.steps.travelMode,routes.legs.steps.transitDetails");
    m_reply = m_nam->post(req, json);
    m_reply->setProperty("quiet", quiet);
    connect(m_reply, SIGNAL(finished()), this, SLOT(onReply()));
    if (!quiet) setBusy(true);
}

void RouteClient::onReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r != m_reply) return;
    setBusy(false);
    if (r->error() == QNetworkReply::OperationCanceledError) return;
    bool quiet = r->property("quiet").toBool();
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bb::data::JsonDataAccess jda;
    QVariantMap res = jda.loadFromBuffer(r->readAll()).toMap();
    QVariantList routes = res.value("routes").toList();
    if (status != 200 || routes.isEmpty()) {
        bbportLog(QString("[route] HTTP %1, %2 percorsi").arg(status).arg(routes.size()));
        if (!quiet) {
            if (status == 0) emit error("Percorso non disponibile: nessuna connessione. Controlla Wi-Fi o rete dati.");
            else if (status == 200) emit error(QString::fromUtf8("Nessun percorso trovato con questo mezzo, prova un'altra modalit\u00e0."));
            else emit error(QString("Percorso non calcolato (HTTP %1), riprova.").arg(status));
        }
        return;
    }
    QVariantMap route = routes.at(0).toMap();
    QVector<QPointF> pts;
    QVector<RouteStep> steps;
    QVector<int> pointStep;    // step index of each point (for segment styles)
    QVariantList legs = route.value("legs").toList();
    for (int l = 0; l < legs.size(); ++l) {
        QVariantList st = legs.at(l).toMap().value("steps").toList();
        for (int s = 0; s < st.size(); ++s) {
            QVariantMap step = st.at(s).toMap();
            QVector<QPointF> sp = polyline::decode(step.value("polyline").toMap().value("encodedPolyline").toByteArray());
            RouteStep rs;
            QVariantMap ni = step.value("navigationInstruction").toMap();
            rs.instruction = ni.value("instructions").toString().replace('\n', QString::fromUtf8(" \u00b7 "));
            rs.maneuver = ni.value("maneuver").toString();
            rs.startM = pts.size();               // temporarily: index of the first point
            rs.lengthM = step.value("distanceMeters").toDouble();
            rs.transit = false;
            rs.stops = 0;
            QVariantMap td = step.value("transitDetails").toMap();
            if (step.value("travelMode").toString() == "TRANSIT" && !td.isEmpty()) {
                QVariantMap line = td.value("transitLine").toMap();
                QVariantMap stops = td.value("stopDetails").toMap();
                QVariantMap local = td.value("localizedValues").toMap();
                QVariantMap vehicle = line.value("vehicle").toMap();
                rs.transit = true;
                rs.vehicle = vehicle.value("name").toMap().value("text").toString();
                rs.line = lineLabel(vehicle.value("type").toString(), line.value("nameShort").toString(),
                                    line.value("name").toString());
                rs.lineName = line.value("nameShort").toString().isEmpty() ? QString() : line.value("name").toString();
                rs.headsign = td.value("headsign").toString();
                rs.depStop = stops.value("departureStop").toMap().value("name").toString();
                rs.arrStop = stops.value("arrivalStop").toMap().value("name").toString();
                rs.depTime = local.value("departureTime").toMap().value("time").toMap().value("text").toString();
                rs.arrTime = local.value("arrivalTime").toMap().value("time").toMap().value("text").toString();
                rs.stops = td.value("stopCount").toInt();
                rs.color = QColor(line.value("color").toString());
                if (!rs.color.isValid()) rs.color = QColor(26, 115, 232);
            }
            for (int i = 0; i < sp.size(); ++i) {
                if (pts.isEmpty() || pts.last() != sp.at(i)) {
                    pts.append(sp.at(i));
                    pointStep.append(steps.size());
                }
            }
            steps.append(rs);
        }
    }
    if (pts.size() < 2) {
        pts = polyline::decode(route.value("polyline").toMap().value("encodedPolyline").toByteArray());
        pointStep.fill(-1, pts.size());
    }
    if (pts.size() < 2) {
        if (!quiet) emit error("Percorso ricevuto senza geometria, riprova.");
        return;
    }
    QVector<double> cum(pts.size());
    cum[0] = 0;
    for (int i = 1; i < pts.size(); ++i) cum[i] = cum[i - 1] + polyline::distance(pts.at(i - 1), pts.at(i));
    for (int i = 0; i < steps.size(); ++i) {
        int idx = qMin(int(steps.at(i).startM), pts.size() - 1);
        steps[i].startM = cum.at(idx);
    }
    m_points = pts;
    m_cum = cum;
    m_steps = steps;
    // Segment i (point i -> i+1) takes the style of the step its end point belongs to.
    m_segColor.fill(QColor(), qMax(0, pts.size() - 1));
    m_segWalk.fill(false, qMax(0, pts.size() - 1));
    for (int i = 0; i + 1 < pts.size() && i + 1 < pointStep.size(); ++i) {
        int st = pointStep.at(i + 1);
        if (st < 0 || st >= steps.size() || m_mode != "TRANSIT") continue;
        if (steps.at(st).transit) m_segColor[i] = steps.at(st).color;
        else m_segWalk[i] = true;
    }
    m_durationS = route.value("duration").toString().remove('s').toInt();
    bbportLog(QString("[route] %1 %2 m, %3 s, %4 passi, %5 punti%6")
                  .arg(m_mode).arg(totalMetres(), 0, 'f', 0).arg(m_durationS)
                  .arg(m_steps.size()).arg(m_points.size()).arg(quiet ? " (ricalcolo)" : ""));
    emit routeChanged();
}
