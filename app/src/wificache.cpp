#include "wificache.hpp"
#include "bbportlog.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDataStream>
#include <QVector>
#include <QtCore/qmath.h>

#include <algorithm>

#include <wifi/wifi_service.h>

namespace {

const quint32 kMagic = 0x424d5743;    // "BMWC"
const qint32 kVersion = 1;
const int kMaxAccessPoints = 20000;
const double kMovedM = 500;           // seen this far from its centroid: it moved
const float kMaxRadiusM = 300;
const double kOutlierM = 300;         // estimate: drop APs this far from the others
const qint64 kSaveEveryMs = 60000;

double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    // Equirectangular: plenty for the few hundred metres compared here.
    const double k = M_PI / 180.0, r = 6371000.0;
    double x = (lon2 - lon1) * k * qCos((lat1 + lat2) / 2 * k);
    double y = (lat2 - lat1) * k;
    return r * qSqrt(x * x + y * y);
}

// A known access point seen in the current scan (C++03: no local template args).
struct Known { double lat, lon, w, radius; };

// Stronger signal = closer: amplitude ratio relative to -100 dBm.
double signalWeight(int rssi)
{
    return qPow(10.0, (qBound(-100, rssi, -20) + 100) / 20.0);
}

} // namespace

WifiCache::WifiCache() :
    m_path(QDir::homePath() + "/wifi_cache.dat"), m_dirty(false)
{
    load();
    m_sinceSave.start();
}

WifiCache::~WifiCache()
{
    save();
}

QList<WifiObservation> WifiCache::scan()
{
    QList<WifiObservation> seen;
    wifi_scan_results_t *results = 0;
    wifi_scan_report_t report;
    int count = 0;
    if (wifi_get_scan_results(&results, &report, &count) != WIFI_SUCCESS || !results)
        return seen;
    for (int i = 1; i <= count; ++i) {
        char ssid[WIFI_MAX_SSID_BUFFER_LEN] = { 0 };
        uint8_t mac[WIFI_BSSID_ADDR_LEN];
        int level = -100;
        if (wifi_get_scan_result_bssid(results, i, mac) != WIFI_SUCCESS) continue;
        if (mac[0] & 0x02) continue;   // locally administered: hotspot or randomised
        if (wifi_get_scan_result_ssid(results, i, ssid) == WIFI_SUCCESS
            && QString::fromUtf8(ssid).endsWith("_nomap")) continue;
        wifi_get_scan_result_signal_level(results, i, &level);
        WifiObservation o;
        o.bssid = 0;
        for (int b = 0; b < WIFI_BSSID_ADDR_LEN; ++b) o.bssid = (o.bssid << 8) | mac[b];
        o.rssi = level;
        seen << o;
    }
    wifi_free_scan_results(results);
    return seen;
}

void WifiCache::learn(const QList<WifiObservation> &seen, double lat, double lon, double accuracy)
{
    Q_UNUSED(accuracy);
    const quint32 now = quint32(QDateTime::currentDateTime().toTime_t());
    for (int i = 0; i < seen.size(); ++i) {
        const double w = signalWeight(seen.at(i).rssi);
        QHash<quint64, AccessPoint>::iterator it = m_aps.find(seen.at(i).bssid);
        if (it == m_aps.end()
            || distanceM(lat, lon, it->sumLat / it->sumW, it->sumLon / it->sumW) > kMovedM) {
            AccessPoint ap;
            ap.sumW = w;
            ap.sumLat = lat * w;
            ap.sumLon = lon * w;
            ap.radius = 0;
            ap.lastSeen = now;
            ap.count = 1;
            m_aps.insert(seen.at(i).bssid, ap);
            continue;
        }
        it->sumW += w;
        it->sumLat += lat * w;
        it->sumLon += lon * w;
        double d = distanceM(lat, lon, it->sumLat / it->sumW, it->sumLon / it->sumW);
        it->radius = qMin(kMaxRadiusM, qMax(it->radius, float(d)));
        it->lastSeen = now;
        ++it->count;
    }
    m_dirty = true;
    if (m_aps.size() > kMaxAccessPoints) evictOldest();
    if (m_sinceSave.elapsed() > kSaveEveryMs) save();
}

bool WifiCache::estimate(const QList<WifiObservation> &seen, double *lat, double *lon,
                         double *accuracy, int *used) const
{
    QVector<Known> known;
    for (int i = 0; i < seen.size(); ++i) {
        QHash<quint64, AccessPoint>::const_iterator it = m_aps.constFind(seen.at(i).bssid);
        if (it == m_aps.constEnd()) continue;
        Known k = { it->sumLat / it->sumW, it->sumLon / it->sumW,
                    signalWeight(seen.at(i).rssi), it->radius };
        known << k;
    }
    if (known.isEmpty()) return false;

    double cLat = 0, cLon = 0, sumW = 0;
    for (int pass = 0; pass < 2; ++pass) {
        double sLat = 0, sLon = 0, sW = 0;
        for (int i = 0; i < known.size(); ++i) {
            // Second pass: leave out APs far from the first centroid (moved, or
            // seen from far away).
            if (pass == 1 && distanceM(cLat, cLon, known.at(i).lat, known.at(i).lon) > kOutlierM) continue;
            sLat += known.at(i).lat * known.at(i).w;
            sLon += known.at(i).lon * known.at(i).w;
            sW += known.at(i).w;
        }
        if (sW <= 0) break;   // every AP was an outlier: keep the first centroid
        cLat = sLat / sW;
        cLon = sLon / sW;
        sumW = sW;
    }

    double spread = 0, radius = 0;
    int n = 0;
    for (int i = 0; i < known.size(); ++i) {
        double d = distanceM(cLat, cLon, known.at(i).lat, known.at(i).lon);
        if (d > kOutlierM) continue;
        spread += d * d * known.at(i).w;
        radius += known.at(i).radius * known.at(i).w;
        ++n;
    }
    if (n == 0) return false;   // the known APs disagree by kilometres
    spread = qSqrt(spread / sumW);
    radius /= sumW;
    *lat = cLat;
    *lon = cLon;
    // A single AP can be anywhere within its range of the phone.
    *accuracy = qMax(n == 1 ? 40.0 : 15.0, spread + radius / 2 + 10);
    *used = n;
    return true;
}

void WifiCache::evictOldest()
{
    // Drop the oldest tenth in one go, so this runs rarely.
    QVector<quint32> seen;
    seen.reserve(m_aps.size());
    for (QHash<quint64, AccessPoint>::const_iterator it = m_aps.constBegin(); it != m_aps.constEnd(); ++it)
        seen << it->lastSeen;
    QVector<quint32>::iterator nth = seen.begin() + seen.size() / 10;
    std::nth_element(seen.begin(), nth, seen.end());
    const quint32 cutoff = *nth;
    for (QHash<quint64, AccessPoint>::iterator it = m_aps.begin(); it != m_aps.end();) {
        if (it->lastSeen <= cutoff) it = m_aps.erase(it);
        else ++it;
    }
}

void WifiCache::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QDataStream in(&f);
    in.setVersion(QDataStream::Qt_4_8);
    quint32 magic = 0;
    qint32 version = 0, count = 0;
    in >> magic >> version >> count;
    if (magic != kMagic || version != kVersion || count < 0 || count > kMaxAccessPoints * 2) {
        bbportLog("[net] archivio Wi-Fi locale non valido, ignorato");
        return;
    }
    m_aps.reserve(count);
    for (qint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        quint64 bssid;
        AccessPoint ap;
        in >> bssid >> ap.sumW >> ap.sumLat >> ap.sumLon >> ap.radius >> ap.lastSeen >> ap.count;
        if (in.status() == QDataStream::Ok && ap.sumW > 0) m_aps.insert(bssid, ap);
    }
    bbportLog(QString("[net] archivio Wi-Fi locale: %1 reti").arg(m_aps.size()));
}

void WifiCache::save()
{
    if (!m_dirty) return;
    // Write a temporary file and rename it, so a crash never leaves half a file.
    const QString tmp = m_path + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QDataStream out(&f);
    out.setVersion(QDataStream::Qt_4_8);
    out << kMagic << kVersion << qint32(m_aps.size());
    for (QHash<quint64, AccessPoint>::const_iterator it = m_aps.constBegin(); it != m_aps.constEnd(); ++it)
        out << it.key() << it->sumW << it->sumLat << it->sumLon << it->radius << it->lastSeen << it->count;
    f.close();
    if (out.status() != QDataStream::Ok) return;
    QFile::remove(m_path);
    if (!QFile::rename(tmp, m_path)) return;
    m_dirty = false;
    m_sinceSave.restart();
    bbportLog(QString("[net] archivio Wi-Fi locale salvato: %1 reti").arg(m_aps.size()));
}
