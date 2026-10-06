#ifndef PLACESCLIENT_HPP_
#define PLACESCLIENT_HPP_

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QTimer>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;

// Search with Google Places (New) + reverse geocoding (PROGETTO.md §7.1, §21).
//
// Cost rules (personal use inside the free caps):
//  - Autocomplete (New) with a session token: the session ends with one Place
//    Details call, so the typing is billed together with it.
//  - Place Details asks only for id, formattedAddress, location (Essentials);
//    the name shown comes from the suggestion (displayName would be Pro).
//  - Enter picks the first suggestion: no separate Text Search (Pro).
//  - Daily caps in the app on top of the Cloud console quotas.
// Terms: names/addresses are never stored (no search history on disk). The
// key travels in the X-Goog-Api-Key header, never in a logged URL.
class PlacesClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList suggestions READ suggestions NOTIFY suggestionsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    explicit PlacesClient(QObject *parent = 0);

    QVariantList suggestions() const { return m_suggestions; }
    bool busy() const { return m_busy; }

    // Map center used to bias suggestions towards the visible area.
    Q_INVOKABLE void setBias(double lat, double lon);
    Q_INVOKABLE void autocomplete(const QString &text);   // debounced
    Q_INVOKABLE void choose(int index);                   // -> placeResolved()
    Q_INVOKABLE void cancel();                            // search closed
    Q_INVOKABLE void reverseGeocode(double lat, double lon); // -> placeResolved()

signals:
    void suggestionsChanged();
    void busyChanged();
    void placeResolved(const QString &name, const QString &address, double lat, double lon);
    void error(const QString &text);

private slots:
    void sendAutocomplete();
    void onAutocompleteReply();
    void onDetailsReply();
    void onGeocodeReply();

private:
    bool allow(const char *api, int perDay);
    void setBusy(bool busy);
    void newSession();

    QNetworkAccessManager *m_nam;
    QString m_apiKey;
    QString m_session;
    QString m_pendingText;
    QTimer m_debounce;
    QPointer<QNetworkReply> m_acReply;
    QVariantList m_suggestions;
    QString m_chosenName;
    bool m_busy;
    double m_biasLat, m_biasLon;
    bool m_haveBias;
};

#endif /* PLACESCLIENT_HPP_ */
