#include "invokehandler.hpp"
#include "placesclient.hpp"
#include "bbportlog.hpp"

#include <QRegExp>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <bb/data/JsonDataAccess>
#include <bb/system/InvokeManager>
#include <bb/system/InvokeRequest>

namespace {

bool validLatLon(double lat, double lon)
{
    return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && !(lat == 0 && lon == 0);
}

// "45.46,9.19" (optionally with spaces) -> coordinates.
bool parseLatLon(const QString &s, double *lat, double *lon)
{
    QRegExp re("^\\s*(-?\\d+(?:\\.\\d+)?)\\s*,\\s*(-?\\d+(?:\\.\\d+)?)");
    if (re.indexIn(s) < 0) return false;
    *lat = re.cap(1).toDouble();
    *lon = re.cap(2).toDouble();
    return validLatLon(*lat, *lon);
}

QString decodeComponent(const QString &s)
{
    QString t = s;
    t.replace('+', ' ');
    return QUrl::fromPercentEncoding(t.toUtf8()).trimmed();
}

} // namespace

InvokeHandler::InvokeHandler(PlacesClient *places, QObject *parent) :
    QObject(parent), m_invoke(new bb::system::InvokeManager(this)), m_places(places)
{
    connect(m_invoke, SIGNAL(invoked(const bb::system::InvokeRequest&)),
            this, SLOT(onInvoked(const bb::system::InvokeRequest&)));
    connect(m_places, SIGNAL(addressResolved(QString,QString,double,double)),
            this, SIGNAL(openPlace(QString,QString,double,double)));
}

void InvokeHandler::geocode(const QString &address)
{
    m_places->geocodeAddress(address);
}

void InvokeHandler::onInvoked(const bb::system::InvokeRequest &request)
{
    const QString mime = request.mimeType();
    const QByteArray data = request.data();
    QString uri = QString::fromUtf8(request.uri().toEncoded());
    bbportLog(QString("[invoke] %1 tipo=%2 uri=%3 dati=%4 byte")
                  .arg(request.action()).arg(mime).arg(uri.left(12)).arg(data.size()));

    if (mime == "application/vnd.blackberry.string.address") {
        // Contacts and others: a postal address, possibly on several lines.
        QString address = QString::fromUtf8(data).split('\n', QString::SkipEmptyParts).join(", ").simplified();
        if (!address.isEmpty()) geocode(address);
        return;
    }
    if (mime == "application/vnd.rim.map.action-v1") {
        bb::data::JsonDataAccess jda;
        QVariantMap m = jda.loadFromBuffer(data).toMap();
        QVariantMap c = m.value("center").toMap();
        if (c.isEmpty() && !m.value("locations").toList().isEmpty()) c = m.value("locations").toList().at(0).toMap();
        if (c.isEmpty()) c = m.value("placemark").toMap();
        double lat = c.value("latitude").toDouble(), lon = c.value("longitude").toDouble();
        QString name = c.value("name").toString();
        QString address = c.value("address").toString();
        if (validLatLon(lat, lon)) emit openPlace(name.isEmpty() ? QString("Punto condiviso") : name, address, lat, lon);
        else if (!address.isEmpty()) geocode(address);
        else emit message("Richiesta mappa senza posizione.");
        return;
    }
    if (uri.isEmpty()) uri = QString::fromUtf8(data).split('\n').value(0).trimmed();
    if (!handleLink(uri)) emit message("Non riesco a leggere questa posizione.");
}

bool InvokeHandler::handleLink(const QString &link)
{
    double lat = 0, lon = 0;
    if (link.startsWith("geo:", Qt::CaseInsensitive)) {
        // geo:lat,lon[,alt][;u=..][?q=lat,lon(label) | ?q=address]
        QString body = link.mid(4);
        QString query = body.section('?', 1);
        QString q;
        foreach (const QString &kv, query.split('&'))
            if (kv.startsWith("q=")) q = decodeComponent(kv.mid(2));
        if (!q.isEmpty()) {
            double qlat, qlon;
            if (parseLatLon(q, &qlat, &qlon)) {
                QRegExp label("\\(([^)]*)\\)");
                QString name = label.indexIn(q) >= 0 ? label.cap(1) : QString("Punto condiviso");
                emit openPlace(name, QString(), qlat, qlon);
                return true;
            }
        }
        if (parseLatLon(body.section('?', 0, 0).section(';', 0, 0), &lat, &lon)) {
            emit openPlace("Punto condiviso", QString(), lat, lon);
            return true;
        }
        if (!q.isEmpty()) { geocode(q); return true; }
        return false;
    }
    const QString lower = link.toLower();
    if (lower.contains("maps.app.goo.gl") || lower.contains("goo.gl/maps")) {
        emit message("I link brevi di Google Maps non sono supportati: apri il link completo.");
        return true;
    }
    if (!lower.contains("google.") || (!lower.contains("/maps") && !lower.contains("maps.google"))) return false;

    QRegExp at("@(-?\\d+\\.\\d+),(-?\\d+\\.\\d+)");
    QRegExp place("/place/([^/@?]+)");
    QString name = place.indexIn(link) >= 0 ? decodeComponent(place.cap(1)) : QString();
    if (at.indexIn(link) >= 0) {
        lat = at.cap(1).toDouble();
        lon = at.cap(2).toDouble();
        if (validLatLon(lat, lon)) {
            emit openPlace(name.isEmpty() ? QString("Punto condiviso") : name, QString(), lat, lon);
            return true;
        }
    }
    QRegExp param("[?&](q|query|ll|daddr|destination)=([^&#]+)");
    if (param.indexIn(link) >= 0) {
        QString v = decodeComponent(param.cap(2));
        if (parseLatLon(v, &lat, &lon)) {
            emit openPlace(name.isEmpty() ? QString("Punto condiviso") : name, QString(), lat, lon);
            return true;
        }
        geocode(v);
        return true;
    }
    if (!name.isEmpty()) { geocode(name); return true; }
    return false;
}
