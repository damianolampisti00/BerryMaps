#ifndef NETWORKLOCATOR_HPP_
#define NETWORKLOCATOR_HPP_

#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;

// Approximate position from the visible Wi-Fi networks and the serving cell,
// via the Google Geolocation API (PROGETTO.md §6.3). BB10's own network
// positioning is dead and the Q10 modem's GPS needs minutes for a cold fix, so
// this gives the dot a starting place in a second or two; the GPS replaces it
// as soon as it has a fix (LocationService).
//
// Wi-Fi access points named *_nomap are left out (the owner's opt-out).
// considerIp is off: an IP-based guess is often in another city.
class NetworkLocator : public QObject
{
    Q_OBJECT
public:
    explicit NetworkLocator(QObject *parent = 0);

    // Sends one request unless one is in flight, the last attempt was under
    // 5 minutes ago, no network is visible or today's cap is used up.
    // Returns whether a request was sent.
    bool locate();

signals:
    void located(double lat, double lon, double accuracy);
    void failed(const QString &reason);

private slots:
    void onReply();

private:
    bool allow();

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QString m_apiKey;
    QElapsedTimer m_sinceRequest;
};

#endif /* NETWORKLOCATOR_HPP_ */
