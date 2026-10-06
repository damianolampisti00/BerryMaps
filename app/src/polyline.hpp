#ifndef POLYLINE_HPP_
#define POLYLINE_HPP_

#include <QByteArray>
#include <QPointF>
#include <QVector>
#include <QtCore/qmath.h>

// Google encoded polyline -> points (x = longitude, y = latitude).
// https://developers.google.com/maps/documentation/utilities/polylinealgorithm
namespace polyline {

inline QVector<QPointF> decode(const QByteArray &enc)
{
    QVector<QPointF> pts;
    int i = 0, lat = 0, lon = 0;
    const int n = enc.size();
    while (i < n) {
        int value[2];
        for (int k = 0; k < 2; ++k) {
            int shift = 0, result = 0, b;
            do {
                if (i >= n) return pts;
                b = enc.at(i++) - 63;
                result |= (b & 0x1f) << shift;
                shift += 5;
            } while (b >= 0x20);
            value[k] = (result & 1) ? ~(result >> 1) : (result >> 1);
        }
        lat += value[0];
        lon += value[1];
        pts.append(QPointF(lon * 1e-5, lat * 1e-5));
    }
    return pts;
}

// Great-circle distance in metres between two (lon, lat) points.
inline double distance(const QPointF &a, const QPointF &b)
{
    const double r = 6371000.0, k = M_PI / 180.0;
    double dLat = (b.y() - a.y()) * k, dLon = (b.x() - a.x()) * k;
    double h = qSin(dLat / 2) * qSin(dLat / 2) +
               qCos(a.y() * k) * qCos(b.y() * k) * qSin(dLon / 2) * qSin(dLon / 2);
    return 2 * r * qAtan2(qSqrt(h), qSqrt(1 - h));
}

} // namespace polyline

#endif /* POLYLINE_HPP_ */
