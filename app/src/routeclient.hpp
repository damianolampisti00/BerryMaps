#ifndef ROUTECLIENT_HPP_
#define ROUTECLIENT_HPP_

#include <QObject>
#include <QPointF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QPointer>
#include <QDateTime>
#include <QtGui/QColor>

class QNetworkAccessManager;
class QNetworkReply;

// Google Routes API v2 computeRoutes (PROGETTO.md §7.2, §22).
// TRAFFIC_UNAWARE = Essentials SKU (10,000 free/month). The route geometry is
// the concatenation of the step polylines, so every step knows where it
// starts along the line (used by the Navigator for "next manoeuvre in N m").
struct RouteStep
{
    QString instruction;
    QString maneuver;
    double startM;      // metres from the route start, along the geometry
    double lengthM;
    // Public transport leg (travelMode TRANSIT), empty otherwise.
    bool transit;
    QString line;       // "M3", "Bus 90", ...
    QString lineName;   // "Linea Gialla"
    QString vehicle;    // "Metropolitana"
    QString headsign;   // direction
    QString depStop, depTime, arrStop, arrTime;   // local times "22:25"
    int stops;
    QColor color;
};

class RouteClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasRoute READ hasRoute NOTIFY routeChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY routeChanged)
    Q_PROPERTY(QVariantList steps READ stepsForUi NOTIFY routeChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY routeChanged)
public:
    explicit RouteClient(QObject *parent = 0);

    bool busy() const { return m_busy; }
    bool hasRoute() const { return !m_points.isEmpty(); }
    QString summary() const;
    QVariantList stepsForUi() const;
    QString mode() const { return m_mode; }

    // mode: DRIVE | WALK | BICYCLE | TRANSIT
    Q_INVOKABLE void plan(double fromLat, double fromLon, double toLat, double toLon, const QString &mode);
    Q_INVOKABLE void changeMode(double fromLat, double fromLon, const QString &mode);
    Q_INVOKABLE void clear();
    // Public transport only: kind = "now" | "depart" | "arrive". Replans the
    // current route if there is one. (Driving uses TRAFFIC_UNAWARE, which
    // ignores times, so the time is not sent for the other modes.)
    Q_INVOKABLE void setTime(const QString &kind, const QDateTime &when);
    // Same destination and mode from a new position (rerouting); no UI noise.
    void replan(double fromLat, double fromLon);

    const QVector<QPointF> &points() const { return m_points; }      // (lon, lat)
    const QVector<double> &cumulative() const { return m_cum; }      // metres at each point
    const QVector<RouteStep> &routeSteps() const { return m_steps; }
    // Per segment (point i -> i+1): line colour for transit legs (invalid
    // otherwise) and whether it is a walking leg of a transit route.
    const QVector<QColor> &segmentColors() const { return m_segColor; }
    const QVector<bool> &segmentWalk() const { return m_segWalk; }
    double totalMetres() const { return m_cum.isEmpty() ? 0 : m_cum.last(); }
    int durationSecs() const { return m_durationS; }

signals:
    void busyChanged();
    void routeChanged();
    void error(const QString &text);

private slots:
    void onReply();

private:
    bool allow();
    void request(double fromLat, double fromLon, bool quiet);
    void setBusy(bool busy);

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QString m_apiKey;
    bool m_busy;
    QString m_mode;
    double m_toLat, m_toLon;
    double m_fromLat, m_fromLon;
    QString m_timeKind;
    QDateTime m_time;
    QVector<QPointF> m_points;
    QVector<double> m_cum;
    QVector<RouteStep> m_steps;
    QVector<QColor> m_segColor;
    QVector<bool> m_segWalk;
    int m_durationS;
};

#endif /* ROUTECLIENT_HPP_ */
