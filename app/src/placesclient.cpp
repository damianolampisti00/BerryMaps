#include "placesclient.hpp"
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
#include <QUuid>

#include <bb/data/JsonDataAccess>

namespace {

// App-side daily caps (the Cloud console quotas are the real backstop).
const int kAutocompletePerDay = 300;
const int kDetailsPerDay = 150;
const int kGeocodePerDay = 150;
const int kDebounceMs = 300;
const int kMinChars = 2;
const double kBiasRadiusM = 50000;   // Places (New) maximum for a circle bias

QVariantMap parseJson(const QByteArray &body)
{
    bb::data::JsonDataAccess jda;
    return jda.loadFromBuffer(body).toMap();
}

QString networkMessage(QNetworkReply *r)
{
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) return "Ricerca non disponibile: nessuna connessione. Controlla Wi-Fi o rete dati.";
    if (status == 403) return "Ricerca rifiutata da Google: controlla che l'API sia attiva sulla chiave.";
    if (status == 429) return "Troppe ricerche per oggi: limite raggiunto.";
    return QString("Ricerca non riuscita (HTTP %1), riprova.").arg(status);
}

} // namespace

PlacesClient::PlacesClient(QObject *parent) :
    QObject(parent), m_nam(0), m_busy(false), m_biasLat(0), m_biasLon(0), m_haveBias(false)
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
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, SIGNAL(timeout()), this, SLOT(sendAutocomplete()));
}

bool PlacesClient::allow(const char *api, int perDay)
{
    QSettings s;
    QString today = QDate::currentDate().toString(Qt::ISODate);
    QString dayKey = QString("places/%1/day").arg(api), countKey = QString("places/%1/count").arg(api);
    int count = s.value(dayKey).toString() == today ? s.value(countKey).toInt() : 0;
    if (count >= perDay) {
        emit error("Limite giornaliero di ricerche raggiunto, riprova domani.");
        return false;
    }
    s.setValue(dayKey, today);
    s.setValue(countKey, count + 1);
    return true;
}

void PlacesClient::setBusy(bool busy)
{
    if (busy == m_busy) return;
    m_busy = busy;
    emit busyChanged();
}

void PlacesClient::newSession()
{
    m_session = QUuid::createUuid().toString().mid(1, 36);
}

void PlacesClient::setBias(double lat, double lon)
{
    m_biasLat = lat;
    m_biasLon = lon;
    m_haveBias = true;
}

void PlacesClient::autocomplete(const QString &text)
{
    m_pendingText = text.trimmed();
    if (m_pendingText.size() < kMinChars) {
        m_debounce.stop();
        if (m_acReply) m_acReply->abort();
        m_suggestions.clear();
        emit suggestionsChanged();
        return;
    }
    m_debounce.start();
}

void PlacesClient::sendAutocomplete()
{
    if (m_apiKey.isEmpty()) {
        emit error("Chiave Google mancante: /accounts/1000/shared/misc/berrymaps_apikey.txt");
        return;
    }
    if (!allow("autocomplete", kAutocompletePerDay)) return;
    if (m_session.isEmpty()) newSession();
    if (m_acReply) m_acReply->abort();   // an older query is now irrelevant

    QVariantMap body;
    body["input"] = m_pendingText;
    body["languageCode"] = "it";
    body["regionCode"] = "it";
    body["sessionToken"] = m_session;
    if (m_haveBias) {
        QVariantMap center, circle, bias;
        center["latitude"] = m_biasLat;
        center["longitude"] = m_biasLon;
        circle["center"] = center;
        circle["radius"] = kBiasRadiusM;
        bias["circle"] = circle;
        body["locationBias"] = bias;
    }
    QByteArray json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(body, &json);

    QNetworkRequest req(QUrl("https://places.googleapis.com/v1/places:autocomplete"));
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("X-Goog-Api-Key", m_apiKey.toUtf8());
    m_acReply = m_nam->post(req, json);
    connect(m_acReply, SIGNAL(finished()), this, SLOT(onAutocompleteReply()));
    setBusy(true);
}

void PlacesClient::onAutocompleteReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r != m_acReply) return;          // superseded by a newer query
    setBusy(false);
    if (r->error() == QNetworkReply::OperationCanceledError) return;
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        emit error(networkMessage(r));
        return;
    }
    QVariantList list = parseJson(r->readAll()).value("suggestions").toList();
    m_suggestions.clear();
    for (int i = 0; i < list.size(); ++i) {
        QVariantMap p = list.at(i).toMap().value("placePrediction").toMap();
        if (p.isEmpty()) continue;
        QVariantMap fmt = p.value("structuredFormat").toMap();
        QVariantMap item;
        item["placeId"] = p.value("placeId").toString();
        item["main"] = fmt.value("mainText").toMap().value("text").toString();
        item["secondary"] = fmt.value("secondaryText").toMap().value("text").toString();
        if (item["main"].toString().isEmpty())
            item["main"] = p.value("text").toMap().value("text").toString();
        m_suggestions << item;
    }
    emit suggestionsChanged();
}

void PlacesClient::choose(int index)
{
    if (index < 0 || index >= m_suggestions.size()) return;
    if (!allow("details", kDetailsPerDay)) return;
    m_debounce.stop();
    if (m_acReply) m_acReply->abort();
    QVariantMap item = m_suggestions.at(index).toMap();
    requestDetails(item.value("placeId").toString(), item.value("main").toString());
    m_session.clear();   // the details call closes the billing session
}

void PlacesClient::requestDetails(const QString &placeId, const QString &name)
{
    m_chosenName = name;
    QUrl url("https://places.googleapis.com/v1/places/" + placeId);
    url.addQueryItem("languageCode", "it");
    if (!m_session.isEmpty()) url.addQueryItem("sessionToken", m_session);
    QNetworkRequest req(url);
    req.setRawHeader("X-Goog-Api-Key", m_apiKey.toUtf8());
    // Essentials fields only (see the class comment).
    req.setRawHeader("X-Goog-FieldMask", "id,formattedAddress,location");
    QNetworkReply *r = m_nam->get(req);
    r->setProperty("placeId", placeId);
    connect(r, SIGNAL(finished()), this, SLOT(onDetailsReply()));
    setBusy(true);
}

void PlacesClient::onDetailsReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    setBusy(false);
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        emit error(networkMessage(r));
        return;
    }
    QVariantMap place = parseJson(r->readAll());
    QVariantMap loc = place.value("location").toMap();
    if (loc.isEmpty()) {
        emit error("Il luogo scelto non ha una posizione, prova un altro risultato.");
        return;
    }
    m_suggestions.clear();
    emit suggestionsChanged();
    m_lastPlaceId = r->property("placeId").toString();
    emit placeResolved(m_chosenName, place.value("formattedAddress").toString(),
                       loc.value("latitude").toDouble(), loc.value("longitude").toDouble());
}

void PlacesClient::cancel()
{
    m_debounce.stop();
    if (m_acReply) m_acReply->abort();
    m_session.clear();
    m_suggestions.clear();
    emit suggestionsChanged();
    setBusy(false);
}

void PlacesClient::reverseGeocode(double lat, double lon)
{
    if (m_apiKey.isEmpty()) {
        emit error("Chiave Google mancante: /accounts/1000/shared/misc/berrymaps_apikey.txt");
        return;
    }
    if (!allow("geocode", kGeocodePerDay)) return;
    // The Geocoding API takes the key only as a query parameter; the TLS log
    // strips queries, so it never reaches berrymaps.log.
    QUrl url("https://maps.googleapis.com/maps/api/geocode/json");
    url.addQueryItem("latlng", QString("%1,%2").arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
    url.addQueryItem("language", "it");
    url.addQueryItem("key", m_apiKey);
    QNetworkReply *r = m_nam->get(QNetworkRequest(url));
    r->setProperty("lat", lat);
    r->setProperty("lon", lon);
    connect(r, SIGNAL(finished()), this, SLOT(onGeocodeReply()));
    setBusy(true);
}

void PlacesClient::onGeocodeReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    setBusy(false);
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        emit error(networkMessage(r));
        return;
    }
    QVariantMap res = parseJson(r->readAll());
    QVariantList results = res.value("results").toList();
    QString address = results.isEmpty() ? QString()
                                        : results.at(0).toMap().value("formatted_address").toString();
    if (address.isEmpty()) address = QString::fromUtf8("Nessun indirizzo trovato qui");
    m_lastPlaceId.clear();   // a map point: the user's own coordinates
    emit placeResolved(QString::fromUtf8("Punto selezionato"), address,
                       r->property("lat").toDouble(), r->property("lon").toDouble());
}

// ------------------------------------------------------------------ favourites

QVariantList PlacesClient::loadFavorites() const
{
    QSettings s;
    QVariantList list;
    int n = s.beginReadArray("favorites");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        QVariantMap f;
        f["id"] = s.value("id");
        f["kind"] = s.value("kind");
        f["name"] = s.value("name");
        f["placeId"] = s.value("placeId");
        f["lat"] = s.value("lat");
        f["lon"] = s.value("lon");
        list << f;
    }
    s.endArray();
    return list;
}

void PlacesClient::storeFavorites(const QVariantList &list)
{
    QSettings s;
    s.remove("favorites");
    s.beginWriteArray("favorites", list.size());
    for (int i = 0; i < list.size(); ++i) {
        s.setArrayIndex(i);
        QVariantMap f = list.at(i).toMap();
        s.setValue("id", f.value("id"));
        s.setValue("kind", f.value("kind"));
        s.setValue("name", f.value("name"));
        s.setValue("placeId", f.value("placeId"));
        s.setValue("lat", f.value("lat"));
        s.setValue("lon", f.value("lon"));
    }
    s.endArray();
    emit favoritesChanged();
}

// For the list: Casa, Lavoro, then the others, with a display label.
QVariantList PlacesClient::favorites() const
{
    QVariantList all = loadFavorites(), out;
    const char *order[3] = { "home", "work", "fav" };
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < all.size(); ++i) {
            QVariantMap f = all.at(i).toMap();
            if (f.value("kind").toString() != order[k]) continue;
            f["label"] = k == 0 ? QString("Casa") : k == 1 ? QString("Lavoro") : f.value("name").toString();
            f["detail"] = k < 2 ? QString() : QString("Preferito");
            out << f;
        }
    }
    return out;
}

void PlacesClient::saveCurrent(const QString &kind, const QString &name, double lat, double lon)
{
    QVariantList list = loadFavorites();
    if (kind == "home" || kind == "work") {   // only one Casa / Lavoro
        for (int i = list.size() - 1; i >= 0; --i)
            if (list.at(i).toMap().value("kind").toString() == kind) list.removeAt(i);
    }
    QVariantMap f;
    f["id"] = QUuid::createUuid().toString().mid(1, 8);
    f["kind"] = kind;
    // Casa/Lavoro keep no name at all; other favourites keep the label the
    // user typed/confirmed (Google names/addresses are not copied).
    f["name"] = kind == "fav" ? name : QString();
    if (!m_lastPlaceId.isEmpty()) {
        f["placeId"] = m_lastPlaceId;          // resolved again when opened
    } else {
        f["lat"] = lat;                        // the user's own point
        f["lon"] = lon;
    }
    list << f;
    storeFavorites(list);
}

void PlacesClient::removeFavorite(const QString &id)
{
    QVariantList list = loadFavorites();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list.at(i).toMap().value("id").toString() == id) list.removeAt(i);
    storeFavorites(list);
}

void PlacesClient::openFavorite(const QString &id)
{
    QVariantList list = favorites();
    for (int i = 0; i < list.size(); ++i) {
        QVariantMap f = list.at(i).toMap();
        if (f.value("id").toString() != id) continue;
        QString label = f.value("label").toString();
        if (!f.value("placeId").toString().isEmpty()) {
            if (!allow("details", kDetailsPerDay)) return;
            m_debounce.stop();
            if (m_acReply) m_acReply->abort();
            requestDetails(f.value("placeId").toString(), label);
        } else {
            m_lastPlaceId.clear();
            emit placeResolved(label, QString(), f.value("lat").toDouble(), f.value("lon").toDouble());
        }
        return;
    }
}
