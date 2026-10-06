#ifndef MAPCONTROLLER_HPP_
#define MAPCONTROLLER_HPP_

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QPointF>
#include <QVector>
#include <QtGui/QColor>

namespace bb { namespace cascades { class Container; class ImageView; class TranslateTransition; } }
class TileStore;
class LocationService;

// Map engine (PROGETTO.md §5): a layer of 512 px tiles inside a viewport-sized
// container. Panning moves the layer with translationX/Y (GPU only, no layout
// pass) and every half tile the tiles are re-laid out around the new center,
// so there is never a visible "recenter" jump. Fling = TranslateTransition
// (runs on Cascades' render thread). Pinch = scale of the outer layer, then a
// snap to the nearest integer zoom. ImageViews are recycled from a pool.
class MapController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString attribution READ attribution CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int zoom READ zoom NOTIFY zoomChanged)
    Q_PROPERTY(bool locating READ locating NOTIFY locationStateChanged)
    Q_PROPERTY(bool following READ following NOTIFY locationStateChanged)
    Q_PROPERTY(QString locationStatus READ locationStatus NOTIFY locationStatusChanged)
    // True only when visible tiles have been missing for more than 3 s
    // (BB10 guidelines: show an activity indicator only past 3 s of waiting).
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    // Map style: "auto" (night between sunset and sunrise) | "day" | "night".
    Q_PROPERTY(QString styleMode READ styleMode WRITE setStyleMode NOTIFY styleChanged)
    Q_PROPERTY(bool night READ night NOTIFY styleChanged)
public:
    explicit MapController(QObject *parent = 0);

    QString attribution() const { return m_attribution; }
    QString status() const { return m_status; }
    int zoom() const { return m_z; }
    bool locating() const { return m_locating; }
    bool following() const { return m_following; }
    QString locationStatus() const;
    bool loading() const { return m_loading; }
    QString version() const;
    QString styleMode() const { return m_styleMode; }
    void setStyleMode(const QString &mode);
    bool night() const { return m_night; }

    Q_INVOKABLE void attach(QObject *host);
    Q_INVOKABLE void setViewport(float w, float h);
    Q_INVOKABLE void panStart();
    Q_INVOKABLE void panBy(float dx, float dy);
    Q_INVOKABLE void panEnd(float vx, float vy);      // px per ms
    Q_INVOKABLE void pinchStart(float x, float y);
    Q_INVOKABLE void pinchUpdate(float ratio);
    Q_INVOKABLE void pinchEnd();
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void zoomInAt(float x, float y);
    Q_INVOKABLE void saveState();
    // "My location" button / key M: off -> on + follow; after a manual pan,
    // follow again; pressed while already following it turns the GPS off.
    Q_INVOKABLE void locateMe();
    // Search results / long press (Fase 3).
    Q_INVOKABLE void showPin(double lat, double lon, bool center);
    Q_INVOKABLE void clearPin();
    Q_INVOKABLE QVariantMap centerLatLon() const;
    Q_INVOKABLE QVariantMap screenToLatLon(float x, float y) const;
    // Metres from the current GPS position, -1 without a fix.
    Q_INVOKABLE double distanceTo(double lat, double lon) const;
    Q_INVOKABLE bool hasFix() const;
    Q_INVOKABLE QVariantMap myPosition() const;   // {lat, lon} of the last fix

    // Fase 4: route line + navigation mode (called from C++).
    // colors/walk are per segment (may be empty): transit line colours and
    // dashed walking legs of a public-transport route.
    void setRoute(const QVector<QPointF> &lonLat, const QVector<QColor> &colors = QVector<QColor>(),
                  const QVector<bool> &walk = QVector<bool>());
    void setNavigating(bool on);
    LocationService *locationService() const { return m_loc; }

signals:
    void statusChanged();
    void zoomChanged();
    void locationStateChanged();
    void locationStatusChanged();
    void loadingChanged();
    void styleChanged();
    // Transient problems shown as a 3 s toast with a suggested fix (guidelines).
    void toast(const QString &text);

private slots:
    void onTileReady(const QString &key, const QString &fileUrl);
    void onFlingEnded();
    void onStoreStatus(const QString &text);
    void onSettled();
    void onLocationUpdated();
    void onAppFullscreen();
    void onAppBackground();
    void onAppAsleep();
    void onAppAwake();
    void onLoadingTimeout();
    void updateStyle();

private:
    void relayout();
    void setLayerTranslation(float x, float y);
    void applyZoom(int newZoom, float focusX, float focusY);
    void releaseAll();
    void retireForZoom(double factor);
    void dropStale();
    bb::cascades::ImageView *obtainView();
    void normalizeCenter();
    void updateLocationOverlay();
    void centerOnLocation();
    void setFollowing(bool on);
    void updateForeground();
    void updateLoading();
    void updatePinOverlay();
    void updateRouteOverlay(const QHash<QString, QPair<int, int> > &needed);
    void clearRouteViews();
    bb::cascades::ImageView *renderRouteTile(int z, int tx, int ty);
    bb::cascades::ImageView *makeOverlayView(const char *asset, float size);

    TileStore *m_store;
    bb::cascades::Container *m_host;
    bb::cascades::Container *m_zoomLayer;   // viewport-sized, scaled during pinch
    bb::cascades::Container *m_panLayer;    // translated during pan/fling
    bb::cascades::Container *m_staleLayer;  // previous zoom's tiles, scaled, under the new ones
    bb::cascades::Container *m_tileLayer;   // tiles (inside m_panLayer)
    bb::cascades::Container *m_routeLayer;  // route line images (inside m_panLayer, above tiles)
    bb::cascades::Container *m_overlay;     // location symbols (inside m_panLayer, above tiles)
    bb::cascades::ImageView *m_accuracyView;
    bb::cascades::ImageView *m_arrowView;
    bb::cascades::ImageView *m_dotView;
    bool m_dotGrey;
    bb::cascades::ImageView *m_pinView;
    bool m_hasPin;
    double m_pinLat, m_pinLon;
    bb::cascades::TranslateTransition *m_fling;

    double m_cx, m_cy;          // map center in world pixels at zoom m_z
    double m_originX, m_originY; // world pixel at the pan layer's (0,0), set by relayout()
    int m_z;
    float m_vw, m_vh;           // viewport size
    float m_tx, m_ty;           // current layer translation
    bool m_flinging;
    bool m_pinching;
    float m_pinchX, m_pinchY, m_pinchScale;

    QHash<QString, bb::cascades::ImageView *> m_active;  // wrapped key -> view
    QSet<QString> m_shown;                                // keys whose image is set
    QList<bb::cascades::ImageView *> m_free;
    // After a zoom change the old tiles stay visible (scaled) until the new
    // visible ones have loaded: no grey flash. World rect at the current zoom.
    struct StaleTile { bb::cascades::ImageView *view; double wx, wy, size; };
    QList<StaleTile> m_stale;

    QTimer m_settleTimer;       // saves the position once movement stops
    QString m_attribution;
    QString m_status;

    LocationService *m_loc;
    bool m_locating;            // user wants the dot (persisted)
    bool m_following;           // map recenters on each fix
    bool m_fullscreen, m_awake;

    QSet<QString> m_visible;    // keys of the tiles actually on screen

    QVector<QPointF> m_route;                 // (lon, lat)
    QVector<QPointF> m_routePx;               // world pixels at m_routeZoom
    int m_routeZoom;
    QVector<QColor> m_routeColors;
    QVector<bool> m_routeWalk;
    QHash<QString, bb::cascades::ImageView *> m_routeViews;  // "unwrapped tx/ty" -> image
    bool m_navigating;

    QString m_styleMode;
    bool m_night;
    QTimer m_styleTimer;
    QTimer m_loadingTimer;
    bool m_loading;
    qint64 m_lastToastMs;
};

#endif /* MAPCONTROLLER_HPP_ */
