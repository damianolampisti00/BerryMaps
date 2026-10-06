#ifndef NAVIGATOR_HPP_
#define NAVIGATOR_HPP_

#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

class RouteClient;
class LocationService;
class MapController;
namespace bb { namespace device { class VibrationController; } }

// Turn-by-turn guidance on the planned route (PROGETTO.md §7.3, §22).
//
// Each fix is projected onto the route (map-matching on the nearest segment
// around the last position); from that come the next manoeuvre and its
// distance, remaining time/distance and ETA. EEA safety requirements for a
// Google route shown on a non-Google map: while driving the route is
// recalculated at least every 15 s, and when the GPS position is old or
// imprecise the precise manoeuvre guidance is suspended (degraded state).
// Leaving the route (3 fixes farther than max(35 m, 2x accuracy)) reroutes.
class Navigator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool arrived READ arrived NOTIFY guidanceChanged)
    Q_PROPERTY(bool degraded READ degraded NOTIFY guidanceChanged)
    Q_PROPERTY(QString glyph READ glyph NOTIFY guidanceChanged)
    Q_PROPERTY(QString distanceText READ distanceText NOTIFY guidanceChanged)
    Q_PROPERTY(QString instruction READ instruction NOTIFY guidanceChanged)
    Q_PROPERTY(QString remainingText READ remainingText NOTIFY guidanceChanged)
public:
    Navigator(RouteClient *route, LocationService *loc, MapController *map, QObject *parent = 0);

    bool active() const { return m_active; }
    bool arrived() const { return m_arrived; }
    bool degraded() const { return m_degraded; }
    QString glyph() const { return m_glyph; }
    QString distanceText() const { return m_distanceText; }
    QString instruction() const { return m_instruction; }
    QString remainingText() const { return m_remainingText; }

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

signals:
    void activeChanged();
    void guidanceChanged();

private slots:
    void onFix();
    void onRouteChanged();
    void onRefreshTimer();

private:
    void update();
    bool matchPosition(double *offRouteM);
    void requestReroute(const char *why);
    static QString glyphFor(const QString &maneuver);

    RouteClient *m_route;
    LocationService *m_loc;
    MapController *m_map;
    bb::device::VibrationController *m_vibra;
    QTimer m_refresh;
    QElapsedTimer m_sinceReroute;
    bool m_active, m_arrived, m_degraded;
    int m_segment;          // index of the route segment last matched
    double m_progressM;     // metres travelled along the route
    int m_offCount;
    int m_vibratedStep;
    QString m_glyph, m_distanceText, m_instruction, m_remainingText;
};

#endif /* NAVIGATOR_HPP_ */
