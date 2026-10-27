#ifndef WIFICACHE_HPP_
#define WIFICACHE_HPP_

#include <QHash>
#include <QList>
#include <QString>
#include <QElapsedTimer>

// One access point from the system's latest Wi-Fi scan.
struct WifiObservation
{
    quint64 bssid;   // 48-bit MAC in the low bits
    int rssi;        // dBm
};

// Local, offline Wi-Fi position database (PROGETTO.md §6.3), learned from this
// phone's own good GPS fixes: where the user has been with the GPS on, the
// visible access points give a position in a moment, with no network request
// and nothing leaving the phone. Elsewhere NetworkLocator asks beaconDB.
//
// Each access point keeps a signal-weighted centroid of the fixes it was seen
// at. One seen more than 500 m from its centroid is taken to have moved and
// starts over. Saved to the app's data folder; the 20,000 most recently seen
// access points are kept.
class WifiCache
{
public:
    WifiCache();
    ~WifiCache();

    // The latest scan the system made, without *_nomap networks (the owner's
    // opt-out) and locally administered MACs (phone hotspots move around).
    // Empty with Wi-Fi off.
    static QList<WifiObservation> scan();

    void learn(const QList<WifiObservation> &seen, double lat, double lon, double accuracy);
    // Position from the known access points in `seen`; false when none is known.
    bool estimate(const QList<WifiObservation> &seen, double *lat, double *lon,
                  double *accuracy, int *used) const;
    int size() const { return m_aps.size(); }
    void save();

private:
    struct AccessPoint
    {
        double sumW, sumLat, sumLon;   // signal-weighted centroid
        float radius;                  // metres: farthest fix from the centroid
        quint32 lastSeen;              // seconds since 1970
        quint32 count;
    };

    void load();
    void evictOldest();

    QHash<quint64, AccessPoint> m_aps;
    QString m_path;
    bool m_dirty;
    QElapsedTimer m_sinceSave;
};

#endif /* WIFICACHE_HPP_ */
