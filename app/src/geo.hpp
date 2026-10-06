#ifndef GEO_HPP_
#define GEO_HPP_

#include <QtCore/qmath.h>

// Web Mercator helpers. "World pixels" are measured in 512 px tiles (Google's
// scaleFactor2x tiles shown 1:1 on the Q5's 330 ppi screen): at zoom z the
// world is 512 * 2^z pixels wide and tall.
namespace geo {

const int kTileSize = 512;
const int kMinZoom = 3;   // world (4096 px) wider than the 720 px screen + margins
const int kMaxZoom = 19;  // maxZoomRects reported by the viewport API on land

inline double worldSize(int z) { return double(kTileSize) * double(1 << z); }

inline double lonToX(double lon, int z) { return (lon + 180.0) / 360.0 * worldSize(z); }

inline double latToY(double lat, int z)
{
    double s = qSin(lat * M_PI / 180.0);
    if (s > 0.9999) s = 0.9999;
    if (s < -0.9999) s = -0.9999;
    return (0.5 - qLn((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * worldSize(z);
}

inline double xToLon(double x, int z) { return x / worldSize(z) * 360.0 - 180.0; }

inline double yToLat(double y, int z)
{
    double n = M_PI * (1.0 - 2.0 * y / worldSize(z));
    return 180.0 / M_PI * qAtan(0.5 * (qExp(n) - qExp(-n)));
}

// Floor division / positive modulo for tile indices.
inline int floorDiv(double v, int d) { return int(qFloor(v / d)); }
inline int wrap(int v, int n) { int r = v % n; return r < 0 ? r + n : r; }

} // namespace geo

#endif /* GEO_HPP_ */
