#ifndef NETWORKLOCATOR_HPP_
#define NETWORKLOCATOR_HPP_

#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <QPointer>

#include "wificache.hpp"

class BeaconSubmitter;

class QNetworkAccessManager;
class QNetworkReply;

// Approximate position from the visible Wi-Fi networks and the serving cell
// (PROGETTO.md §6.3), without Google. BB10's own network positioning is dead
// and the Q10 modem's GPS needs minutes for a cold fix, so this gives the dot a
// starting place in a second or two; the GPS replaces it as soon as it has a
// fix (LocationService).
//
// The local WifiCache answers first, offline, wherever this phone has been
// with the GPS on. Elsewhere it asks beaconDB: a community, opt-in database
// with an MLS/Ichnaea-compatible API and no key. Its Wi-Fi coverage is still
// sparse; it falls back to the cell (from the final Mozilla Location Service
// dump). The IP fallback is off: an IP-based guess is often in another city.
class NetworkLocator : public QObject
{
    Q_OBJECT
public:
    explicit NetworkLocator(QObject *parent = 0);

    // Tries the local Wi-Fi cache (at most every 10 s, answers at once), then
    // beaconDB: one request unless one is in flight, the last attempt was under
    // 5 minutes ago, no network is visible or today's cap is used up.
    // Returns whether a position was given or a request sent.
    bool locate();
    // A GPS fix: if good and slow enough, teach the local cache the visible
    // access points (at most every 10 s) and, if enabled, queue them with the
    // cell for beaconDB.
    void learn(double lat, double lon, double accuracy, double speed);
    BeaconSubmitter *submitter() const { return m_submitter; }

signals:
    void located(double lat, double lon, double accuracy);
    void failed(const QString &reason);

private slots:
    void onReply();

private:
    bool allow();

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QElapsedTimer m_sinceRequest;
    QElapsedTimer m_sinceLocal;
    QElapsedTimer m_sinceLearn;
    WifiCache m_cache;
    BeaconSubmitter *m_submitter;
};

#endif /* NETWORKLOCATOR_HPP_ */
