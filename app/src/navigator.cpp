#include "navigator.hpp"
#include "routeclient.hpp"
#include "locationservice.hpp"
#include "mapcontroller.hpp"
#include "bbportlog.hpp"

#include <QDateTime>
#include <QtCore/qmath.h>

#include <bb/device/VibrationController>

namespace {

const int kRefreshMs = 15000;          // EEA: recalculate at least every 15 s while driving
const double kDrivingSpeed = 3.0;      // m/s: below this the periodic refresh is skipped
const int kMinRerouteGapMs = 10000;
const double kOffRouteMin = 35.0;      // metres
const int kOffRouteFixes = 3;
const double kArrivalM = 25.0;

QString formatDistance(double m)
{
    if (m < 0) return QString::fromUtf8("\u2013");
    if (m < 1000) return QString("%1 m").arg(qMax(10, qRound(m / 10.0) * 10));
    return QString("%1 km").arg(m / 1000.0, 0, 'f', m < 10000 ? 1 : 0).replace('.', ',');
}

// Local planar approximation around `ref` (metres), good enough for snapping.
void toMetres(const QPointF &p, const QPointF &ref, double *x, double *y)
{
    *x = (p.x() - ref.x()) * 111320.0 * qCos(ref.y() * M_PI / 180.0);
    *y = (p.y() - ref.y()) * 110540.0;
}

} // namespace

Navigator::Navigator(RouteClient *route, LocationService *loc, MapController *map, QObject *parent) :
    QObject(parent), m_route(route), m_loc(loc), m_map(map),
    m_vibra(new bb::device::VibrationController(this)),
    m_active(false), m_arrived(false), m_degraded(false),
    m_segment(0), m_progressM(0), m_offCount(0), m_vibratedStep(-1)
{
    connect(m_loc, SIGNAL(updated()), this, SLOT(onFix()));
    connect(m_route, SIGNAL(routeChanged()), this, SLOT(onRouteChanged()));
    m_refresh.setInterval(kRefreshMs);
    connect(&m_refresh, SIGNAL(timeout()), this, SLOT(onRefreshTimer()));
}

void Navigator::start()
{
    if (!m_route->hasRoute() || m_active) return;
    m_active = true;
    m_arrived = false;
    m_segment = 0;
    m_progressM = 0;
    m_offCount = 0;
    m_vibratedStep = -1;
    m_sinceReroute.start();
    m_map->setNavigating(true);
    if (m_route->mode() == "DRIVE") m_refresh.start();
    bbportLog("[nav] avviata (" + m_route->mode() + ")");
    emit activeChanged();
    update();
}

void Navigator::stop()
{
    if (!m_active) return;
    m_active = false;
    m_refresh.stop();
    m_map->setNavigating(false);
    bbportLog("[nav] terminata");
    emit activeChanged();
}

void Navigator::onRouteChanged()
{
    // The map draws whatever route exists, guided or not.
    m_map->setRoute(m_route->points(), m_route->segmentColors(), m_route->segmentWalk());
    if (!m_active) return;
    if (!m_route->hasRoute()) {
        stop();
        return;
    }
    // New geometry (reroute): match again from the start of it.
    m_segment = 0;
    m_offCount = 0;
    update();
}

void Navigator::onFix()
{
    if (m_active) update();
}

void Navigator::onRefreshTimer()
{
    if (m_active && !m_arrived && m_loc->hasFix() && m_loc->speed() > kDrivingSpeed)
        requestReroute("aggiornamento periodico");
}

void Navigator::requestReroute(const char *why)
{
    if (m_sinceReroute.elapsed() < kMinRerouteGapMs || !m_loc->hasFix()) return;
    m_sinceReroute.restart();
    bbportLog(QString("[nav] ricalcolo: %1").arg(why));
    m_route->replan(m_loc->latitude(), m_loc->longitude());
}

bool Navigator::matchPosition(double *offRouteM)
{
    const QVector<QPointF> &pts = m_route->points();
    const QVector<double> &cum = m_route->cumulative();
    if (pts.size() < 2 || !m_loc->hasFix()) return false;
    const QPointF me(m_loc->longitude(), m_loc->latitude());

    double best = 1e18, bestProgress = m_progressM;
    int bestSeg = m_segment;
    // Search forward from the last match first (cheap, avoids jumping onto a
    // nearby parallel part of the route); fall back to the whole route.
    for (int pass = 0; pass < 2; ++pass) {
        int from = pass == 0 ? qMax(0, m_segment - 5) : 0;
        int to = pass == 0 ? qMin(pts.size() - 1, m_segment + 80) : pts.size() - 1;
        for (int i = from; i < to; ++i) {
            double ax, ay, bx, by;
            toMetres(pts.at(i), me, &ax, &ay);
            toMetres(pts.at(i + 1), me, &bx, &by);
            double dx = bx - ax, dy = by - ay;
            double len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? qBound(0.0, -(ax * dx + ay * dy) / len2, 1.0) : 0;
            double px = ax + t * dx, py = ay + t * dy;
            double d = qSqrt(px * px + py * py);
            if (d < best) {
                best = d;
                bestSeg = i;
                bestProgress = cum.at(i) + t * (cum.at(i + 1) - cum.at(i));
            }
        }
        if (best < 100) break;
    }
    m_segment = bestSeg;
    m_progressM = bestProgress;
    *offRouteM = best;
    return true;
}

void Navigator::update()
{
    if (!m_active || !m_route->hasRoute()) return;
    const double total = m_route->totalMetres();
    double off = 0;
    bool matched = matchPosition(&off);
    m_degraded = !m_loc->reliable();

    if (matched && !m_degraded) {
        double limit = qMax(kOffRouteMin, 2.0 * m_loc->accuracy());
        m_offCount = off > limit ? m_offCount + 1 : 0;
        if (m_offCount >= kOffRouteFixes && m_route->mode() != "TRANSIT") {
            m_offCount = 0;
            requestReroute("fuori percorso");
        }
    }

    const double remaining = qMax(0.0, total - m_progressM);
    if (matched && !m_degraded && remaining < kArrivalM) {
        if (!m_arrived) {
            m_arrived = true;
            m_refresh.stop();
            m_vibra->start(100, 600);
            bbportLog("[nav] arrivato");
        }
        m_glyph = QString::fromUtf8("\u25c9");
        m_distanceText = QString();
        m_instruction = QString::fromUtf8("Sei arrivato a destinazione");
        m_remainingText = QString();
        emit guidanceChanged();
        return;
    }

    // The next manoeuvre is the start of the step after the current one.
    const QVector<RouteStep> &steps = m_route->routeSteps();
    int next = -1;
    for (int i = 0; i < steps.size(); ++i) {
        if (steps.at(i).startM > m_progressM + 1) { next = i; break; }
    }
    double toNext = next >= 0 ? steps.at(next).startM - m_progressM : remaining;
    int current = next > 0 ? next - 1 : (next < 0 ? steps.size() - 1 : -1);

    if (current >= 0 && steps.at(current).transit) {
        // On board: timetable information, valid even without GPS (underground).
        const RouteStep &s = steps.at(current);
        m_glyph = s.line;
        m_distanceText = s.arrTime.isEmpty() ? QString() : "scendi alle " + s.arrTime;
        m_instruction = s.vehicle + QString::fromUtf8(" \u2192 ") + s.headsign + ": scendi a " + s.arrStop +
                        " (" + QString::number(s.stops) + (s.stops == 1 ? " fermata)" : " fermate)");
    } else if (next >= 0 && steps.at(next).transit && !m_degraded) {
        // Walking to the stop: say what to catch and when.
        const RouteStep &s = steps.at(next);
        m_glyph = QString::fromUtf8("\u2191");
        m_distanceText = formatDistance(toNext);
        m_instruction = "Raggiungi la fermata " + s.depStop + ": " + s.line + QString::fromUtf8(" \u2192 ") +
                        s.headsign + (s.depTime.isEmpty() ? QString() : " alle " + s.depTime);
    } else if (m_degraded) {
        // Safety: an uncertain position must not drive precise instructions.
        m_glyph = QString::fromUtf8("\u26a0");
        m_distanceText = QString();
        m_instruction = QString::fromUtf8("Segnale GPS debole: indicazioni sospese");
    } else if (next >= 0) {
        m_glyph = glyphFor(steps.at(next).maneuver);
        m_distanceText = formatDistance(toNext);
        m_instruction = steps.at(next).instruction;
        double warnAt = m_route->mode() == "DRIVE" ? 80 : 25;
        if (toNext < warnAt && m_vibratedStep != next) {
            m_vibratedStep = next;
            m_vibra->start(80, 300);
        }
    } else {
        m_glyph = QString::fromUtf8("\u25c9");
        m_distanceText = formatDistance(toNext);
        m_instruction = QString::fromUtf8("Destinazione");
    }

    int secs = total > 0 ? int(m_route->durationSecs() * remaining / total) : 0;
    QString eta = QDateTime::currentDateTime().addSecs(secs).toString("HH:mm");
    int min = qMax(1, qRound(secs / 60.0));
    QString dur = min < 60 ? QString("%1 min").arg(min) : QString("%1 h %2 min").arg(min / 60).arg(min % 60);
    m_remainingText = dur + QString::fromUtf8("  \u00b7  ") + formatDistance(remaining) +
                      QString::fromUtf8("  \u00b7  arrivo ") + eta;
    emit guidanceChanged();
}

QString Navigator::glyphFor(const QString &m)
{
    // Plain arrows (U+2190 block) render with the system font on BB10.
    if (m.contains("UTURN")) return QString::fromUtf8("\u21b6");
    if (m.contains("ROUNDABOUT")) return QString::fromUtf8("\u21bb");
    if (m.contains("SHARP_LEFT") || m == "TURN_LEFT") return QString::fromUtf8("\u2190");
    if (m.contains("SHARP_RIGHT") || m == "TURN_RIGHT") return QString::fromUtf8("\u2192");
    if (m.contains("LEFT")) return QString::fromUtf8("\u2196");
    if (m.contains("RIGHT")) return QString::fromUtf8("\u2197");
    return QString::fromUtf8("\u2191");
}
