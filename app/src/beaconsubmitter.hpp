#ifndef BEACONSUBMITTER_HPP_
#define BEACONSUBMITTER_HPP_

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

// Optional contribution to beaconDB (Settings, off by default): good GPS fixes
// with the Wi-Fi access points and cell seen there, sent to its geosubmit API
// so the network position improves for everyone, BerryMaps included.
//
// A measurement is queued only 25 m or more from the previous one (no repeats
// while standing still) and sent in batches: at 20 queued, or 10 minutes after
// the first. Failed batches stay queued (500 at most). Timestamps come from the
// phone's clock: the Q10 modem's GPS time is 1024 weeks off.
class BeaconSubmitter : public QObject
{
    Q_OBJECT
public:
    explicit BeaconSubmitter(QObject *parent = 0);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    int sentTotal() const { return m_sent; }

    // aps / towers in the Ichnaea JSON shape (macAddress, signalStrength; ...).
    void add(double lat, double lon, double accuracy, double speed,
             const QVariantList &aps, const QVariantList &towers);

signals:
    void changed();

private slots:
    void onReply();

private:
    void submit();

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QVariantList m_pending;
    int m_inFlight;
    bool m_enabled;
    int m_sent;
    bool m_haveLast;
    double m_lastLat, m_lastLon;
    QElapsedTimer m_sinceFirstPending;
};

#endif /* BEACONSUBMITTER_HPP_ */
