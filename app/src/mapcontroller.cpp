#include "mapcontroller.hpp"
#include "tilestore.hpp"
#include "locationservice.hpp"
#include "geo.hpp"
#include "bbportlog.hpp"

#include <QDateTime>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QSettings>
#include <QUrl>
#include <QtCore/qmath.h>

#include <bb/ApplicationInfo>
#include <bb/ImageData>
#include <bb/PixelFormat>
#include <bb/cascades/Image>
#include <bb/cascades/Window>
#include <bb/cascades/ScreenIdleMode>
#include <bb/cascades/AbsoluteLayout>
#include <bb/cascades/Application>
#include <bb/cascades/AbsoluteLayoutProperties>
#include <bb/cascades/Color>
#include <bb/cascades/Container>
#include <bb/cascades/ImageView>
#include <bb/cascades/ImageViewLoadEffect>
#include <bb/cascades/ImplicitAnimationController>
#include <bb/cascades/ScalingMethod>
#include <bb/cascades/StockCurve>
#include <bb/cascades/TranslateTransition>

using namespace bb::cascades;

namespace {

const float kRelayoutStep = geo::kTileSize / 2.0f;  // re-lay out tiles every half tile of drag
const float kMaxFling = 300.0f;   // px; stays within the 1-tile ring loaded around the view
const float kMinFlingSpeed = 0.3f; // px/ms

// Default view: Italy.
const double kDefaultLat = 42.5;
const double kDefaultLon = 12.5;
const int kDefaultZoom = 6;

} // namespace

MapController::MapController(QObject *parent) :
    QObject(parent),
    m_store(new TileStore(this)),
    m_host(0), m_zoomLayer(0), m_panLayer(0), m_staleLayer(0), m_tileLayer(0), m_routeLayer(0), m_overlay(0),
    m_accuracyView(0), m_arrowView(0), m_dotView(0), m_dotGrey(false),
    m_pinView(0), m_hasPin(false), m_pinLat(0), m_pinLon(0), m_fling(0),
    m_cx(0), m_cy(0), m_originX(0), m_originY(0), m_z(kDefaultZoom), m_vw(720), m_vh(720), m_tx(0), m_ty(0),
    m_flinging(false), m_pinching(false), m_pinchX(0), m_pinchY(0), m_pinchScale(1),
    m_attribution(QString::fromUtf8("© OpenStreetMap contributors, © CARTO")),
    m_loc(new LocationService(this)), m_locating(false), m_following(false),
    m_fullscreen(true), m_awake(true), m_routeZoom(-1), m_navigating(false),
    m_loading(false), m_lastToastMs(0)
{
    QSettings s;
    double lat = s.value("map/lat", kDefaultLat).toDouble();
    double lon = s.value("map/lon", kDefaultLon).toDouble();
    m_z = qBound(geo::kMinZoom, s.value("map/zoom", kDefaultZoom).toInt(), geo::kMaxZoom);
    m_cx = geo::lonToX(lon, m_z);
    m_cy = geo::latToY(lat, m_z);

    connect(m_store, SIGNAL(tileReady(QString,QString)), this, SLOT(onTileReady(QString,QString)));
    connect(m_store, SIGNAL(statusChanged(QString)), this, SLOT(onStoreStatus(QString)));
    m_status = m_store->status();
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(1500);
    connect(&m_settleTimer, SIGNAL(timeout()), this, SLOT(onSettled()));
    m_loadingTimer.setSingleShot(true);
    m_loadingTimer.setInterval(3000);
    connect(&m_loadingTimer, SIGNAL(timeout()), this, SLOT(onLoadingTimeout()));

    connect(m_loc, SIGNAL(updated()), this, SLOT(onLocationUpdated()));
    connect(m_loc, SIGNAL(statusChanged()), this, SIGNAL(locationStatusChanged()));
    // GPS only while the map is actually on screen (PROGETTO.md §6.4).
    Application *app = Application::instance();
    connect(app, SIGNAL(fullscreen()), this, SLOT(onAppFullscreen()));
    connect(app, SIGNAL(thumbnail()), this, SLOT(onAppBackground()));
    connect(app, SIGNAL(invisible()), this, SLOT(onAppBackground()));
    connect(app, SIGNAL(asleep()), this, SLOT(onAppAsleep()));
    connect(app, SIGNAL(awake()), this, SLOT(onAppAwake()));
    m_locating = s.value("location/on", false).toBool();
    m_loc->setWanted(m_locating);
}

void MapController::attach(QObject *hostObj)
{
    m_host = qobject_cast<Container *>(hostObj);
    if (!m_host) {
        bbportLog("[map] ERRORE: contenitore mappa non valido");
        return;
    }
    m_zoomLayer = Container::create().layout(AbsoluteLayout::create());
    m_zoomLayer->setImplicitLayoutAnimationsEnabled(false);
    m_zoomLayer->setBackground(Color::fromARGB(0xfff2efe9));   // Voyager land color while tiles load
    // setViewport() may already have run (LayoutUpdateHandler fires early).
    m_zoomLayer->setPreferredSize(m_vw, m_vh);
    m_panLayer = Container::create().layout(AbsoluteLayout::create());
    m_panLayer->setImplicitLayoutAnimationsEnabled(false);
    m_tileLayer = Container::create().layout(AbsoluteLayout::create());
    m_tileLayer->setImplicitLayoutAnimationsEnabled(false);
    m_overlay = Container::create().layout(AbsoluteLayout::create());
    m_overlay->setImplicitLayoutAnimationsEnabled(false);
    m_routeLayer = Container::create().layout(AbsoluteLayout::create());
    m_routeLayer->setImplicitLayoutAnimationsEnabled(false);
    m_staleLayer = Container::create().layout(AbsoluteLayout::create());
    m_staleLayer->setImplicitLayoutAnimationsEnabled(false);
    m_panLayer->add(m_staleLayer);    // old zoom level, below everything else
    m_panLayer->add(m_tileLayer);
    m_panLayer->add(m_routeLayer);    // route line above the tiles...
    m_panLayer->add(m_overlay);       // ...and the location symbols above both
    m_zoomLayer->add(m_panLayer);
    m_host->add(m_zoomLayer);
    // Bottom to top: accuracy circle, heading cone, dot.
    m_accuracyView = makeOverlayView("asset:///loc_accuracy.png", 256);
    m_arrowView = makeOverlayView("asset:///loc_arrow.png", 160);
    m_dotView = makeOverlayView("asset:///loc_dot.png", 48);
    m_pinView = makeOverlayView("asset:///pin.png", 56);   // on top of everything
    m_pinView->setPreferredSize(56, 80);

    m_fling = TranslateTransition::create(m_panLayer)
                  .duration(320)
                  .easingCurve(StockCurve::QuarticOut)
                  .autoDeleted(false);
    m_fling->setParent(this);
    connect(m_fling, SIGNAL(ended()), this, SLOT(onFlingEnded()));
    relayout();
}

void MapController::setViewport(float w, float h)
{
    if (w <= 0 || h <= 0 || (w == m_vw && h == m_vh && m_zoomLayer && m_zoomLayer->preferredWidth() == w)) return;
    m_vw = w;
    m_vh = h;
    if (m_zoomLayer) {
        m_zoomLayer->setPreferredSize(w, h);
        relayout();
    }
}

void MapController::setLayerTranslation(float x, float y)
{
    m_tx = x;
    m_ty = y;
    // Without this guard Cascades animates every translation change implicitly.
    ImplicitAnimationController guard = ImplicitAnimationController::create(m_panLayer).enabled(false);
    m_panLayer->setTranslation(x, y);
}

ImageView *MapController::makeOverlayView(const char *asset, float size)
{
    ImageView *iv = ImageView::create().preferredSize(size, size);
    iv->setScalingMethod(ScalingMethod::Fill);
    iv->setLoadEffect(ImageViewLoadEffect::None);
    iv->setImplicitLayoutAnimationsEnabled(false);
    iv->setLayoutProperties(AbsoluteLayoutProperties::create());
    iv->setImageSource(QUrl(asset));
    iv->setVisible(false);
    m_overlay->add(iv);
    return iv;
}

void MapController::normalizeCenter()
{
    double world = geo::worldSize(m_z);
    m_cx = fmod(m_cx, world);
    if (m_cx < 0) m_cx += world;
    double half = m_vh / 2.0;
    if (world > m_vh) m_cy = qBound(half, m_cy, world - half);
    else m_cy = world / 2.0;
}

ImageView *MapController::obtainView()
{
    if (!m_free.isEmpty()) return m_free.takeLast();
    ImageView *iv = ImageView::create().preferredSize(geo::kTileSize, geo::kTileSize);
    iv->setScalingMethod(ScalingMethod::Fill);
    // No fade-in: it would be an implicit animation per tile (60 fps redraws).
    iv->setLoadEffect(ImageViewLoadEffect::None);
    iv->setImplicitLayoutAnimationsEnabled(false);
    iv->setLayoutProperties(AbsoluteLayoutProperties::create());
    m_tileLayer->add(iv);
    return iv;
}

void MapController::retireForZoom(double factor)
{
    dropStale();   // a previous, still unfinished zoom: its leftovers go first
    const int T = geo::kTileSize;
    QHash<QString, ImageView *>::iterator it = m_active.begin();
    for (; it != m_active.end(); ++it) {
        ImageView *iv = it.value();
        AbsoluteLayoutProperties *lp = qobject_cast<AbsoluteLayoutProperties *>(iv->layoutProperties());
        if (!m_shown.contains(it.key()) || !lp) {
            iv->setVisible(false);
            iv->resetImage();
            m_free << iv;
            continue;
        }
        // Same map area at the new zoom: position and size scale by `factor`.
        StaleTile st;
        st.view = iv;
        st.wx = (lp->positionX() + m_originX) * factor;
        st.wy = (lp->positionY() + m_originY) * factor;
        st.size = T * factor;
        m_tileLayer->remove(iv);
        m_staleLayer->add(iv);
        iv->setPreferredSize(float(st.size), float(st.size));
        m_stale << st;
    }
    m_active.clear();
    m_shown.clear();
}

void MapController::dropStale()
{
    const int T = geo::kTileSize;
    for (int i = 0; i < m_stale.size(); ++i) {
        ImageView *iv = m_stale.at(i).view;
        m_staleLayer->remove(iv);
        iv->setVisible(false);
        iv->resetImage();
        iv->setPreferredSize(T, T);
        m_tileLayer->add(iv);
        m_free << iv;
    }
    m_stale.clear();
}

void MapController::releaseAll()
{
    QHash<QString, ImageView *>::iterator it = m_active.begin();
    for (; it != m_active.end(); ++it) {
        it.value()->setVisible(false);
        it.value()->resetImage();
        m_free << it.value();
    }
    m_active.clear();
    m_shown.clear();
}

void MapController::relayout()
{
    if (!m_tileLayer) return;
    normalizeCenter();
    const int T = geo::kTileSize;
    const int n = 1 << m_z;
    const double originX = m_cx - m_vw / 2.0;
    const double originY = m_cy - m_vh / 2.0;
    m_originX = originX;
    m_originY = originY;
    for (int i = 0; i < m_stale.size(); ++i) {
        AbsoluteLayoutProperties *lp = qobject_cast<AbsoluteLayoutProperties *>(m_stale.at(i).view->layoutProperties());
        if (lp) {
            lp->setPositionX(float(m_stale.at(i).wx - originX));
            lp->setPositionY(float(m_stale.at(i).wy - originY));
        }
    }
    const int vx0 = geo::floorDiv(originX, T), vx1 = geo::floorDiv(originX + m_vw - 1, T);
    const int vy0 = geo::floorDiv(originY, T), vy1 = geo::floorDiv(originY + m_vh - 1, T);

    // Visible tiles plus a one-tile ring (covers dragging and flings up to kMaxFling).
    QHash<QString, QPair<int, int> > needed;   // wrapped key -> unwrapped (tx, ty)
    QHash<QString, int> priority;
    for (int ty = vy0 - 1; ty <= vy1 + 1; ++ty) {
        if (ty < 0 || ty >= n) continue;
        for (int tx = vx0 - 1; tx <= vx1 + 1; ++tx) {
            QString k = TileStore::key(m_z, geo::wrap(tx, n), ty);
            if (needed.contains(k)) continue;
            needed.insert(k, qMakePair(tx, ty));
            bool visible = tx >= vx0 && tx <= vx1 && ty >= vy0 && ty <= vy1;
            priority.insert(k, visible ? 0 : 1);
        }
    }

    // Release views whose tile is no longer needed.
    QList<QString> gone;
    for (QHash<QString, ImageView *>::const_iterator it = m_active.constBegin(); it != m_active.constEnd(); ++it)
        if (!needed.contains(it.key())) gone << it.key();
    foreach (const QString &k, gone) {
        ImageView *iv = m_active.take(k);
        iv->setVisible(false);
        iv->resetImage();
        m_shown.remove(k);
        m_free << iv;
    }

    QSet<QString> wanted;
    m_visible.clear();
    for (QHash<QString, QPair<int, int> >::const_iterator it = needed.constBegin(); it != needed.constEnd(); ++it) {
        const QString &k = it.key();
        wanted.insert(k);
        if (priority.value(k) == 0) m_visible.insert(k);
        ImageView *iv = m_active.value(k, 0);
        if (!iv) {
            iv = obtainView();
            m_active.insert(k, iv);
        }
        AbsoluteLayoutProperties *lp = qobject_cast<AbsoluteLayoutProperties *>(iv->layoutProperties());
        if (lp) {
            lp->setPositionX(float(it.value().first * T - originX));
            lp->setPositionY(float(it.value().second * T - originY));
        }
        iv->setVisible(true);
        if (!m_shown.contains(k)) {
            int z, x, y;
            QStringList p = k.split('/');
            z = p.at(0).toInt(); x = p.at(1).toInt(); y = p.at(2).toInt();
            QString url = m_store->freshTileUrl(z, x, y);
            if (!url.isEmpty()) {
                iv->setImageSource(QUrl(url));
                m_shown.insert(k);
            } else {
                m_store->request(z, x, y, priority.value(k));
            }
        }
    }
    m_store->setWanted(wanted);
    updateLoading();
    updateRouteOverlay(needed);
    updateLocationOverlay();
    updatePinOverlay();
    // Same event-loop turn as the new positions, so both land in one frame.
    setLayerTranslation(0, 0);
}

void MapController::onTileReady(const QString &key, const QString &fileUrl)
{
    ImageView *iv = m_active.value(key, 0);
    if (!iv || m_shown.contains(key)) return;
    iv->setImageSource(QUrl(fileUrl));
    m_shown.insert(key);
    updateLoading();
}

void MapController::panStart()
{
    m_settleTimer.stop();
    setFollowing(false);   // the user takes over the map
}

void MapController::panBy(float dx, float dy)
{
    if (m_flinging || m_pinching || !m_tileLayer) return;
    m_cx -= dx;
    double before = m_cy;
    m_cy -= dy;
    normalizeCenter();
    float realDy = float(before - m_cy);   // vertical clamp at the poles
    float tx = m_tx + dx, ty = m_ty + realDy;
    if (qAbs(tx) > kRelayoutStep || qAbs(ty) > kRelayoutStep) relayout();
    else setLayerTranslation(tx, ty);
}

void MapController::panEnd(float vx, float vy)
{
    if (m_flinging || m_pinching || !m_tileLayer) return;
    float speed = qSqrt(vx * vx + vy * vy);
    if (speed < kMinFlingSpeed) {
        relayout();
        m_settleTimer.start();
        return;
    }
    // Distance ~ v * 250 ms, capped so the ring of preloaded tiles covers it.
    float dx = vx * 250.0f, dy = vy * 250.0f;
    float d = qSqrt(dx * dx + dy * dy);
    if (d > kMaxFling) { dx *= kMaxFling / d; dy *= kMaxFling / d; }
    m_cx -= dx;
    double before = m_cy;
    m_cy -= dy;
    normalizeCenter();
    dy = float(before - m_cy);
    m_flinging = true;
    m_fling->setFromX(m_tx);
    m_fling->setFromY(m_ty);
    m_fling->setToX(m_tx + dx);
    m_fling->setToY(m_ty + dy);
    m_tx += dx;
    m_ty += dy;
    m_fling->play();
}

void MapController::onFlingEnded()
{
    m_flinging = false;
    relayout();
    m_settleTimer.start();
}

void MapController::pinchStart(float x, float y)
{
    if (m_flinging || !m_zoomLayer) return;
    relayout();               // commit any pending translation first
    m_pinching = true;
    setFollowing(false);
    m_pinchX = x;
    m_pinchY = y;
    m_pinchScale = 1;
    ImplicitAnimationController guard = ImplicitAnimationController::create(m_zoomLayer).enabled(false);
    // Pivot is relative to the layer's center.
    m_zoomLayer->setPivot(x - m_vw / 2.0f, y - m_vh / 2.0f);
}

void MapController::pinchUpdate(float ratio)
{
    if (!m_pinching) return;
    m_pinchScale = qBound(0.125f, ratio, 8.0f);
    ImplicitAnimationController guard = ImplicitAnimationController::create(m_zoomLayer).enabled(false);
    m_zoomLayer->setScale(m_pinchScale);
}

void MapController::pinchEnd()
{
    if (!m_pinching) return;
    m_pinching = false;
    int dz = int(qFloor(qLn(m_pinchScale) / qLn(2.0) + 0.5));
    {
        ImplicitAnimationController guard = ImplicitAnimationController::create(m_zoomLayer).enabled(false);
        m_zoomLayer->setScale(1);
        m_zoomLayer->setPivot(0, 0);
    }
    applyZoom(m_z + dz, m_pinchX, m_pinchY);
}

void MapController::applyZoom(int newZoom, float focusX, float focusY)
{
    newZoom = qBound(geo::kMinZoom, newZoom, geo::kMaxZoom);
    if (newZoom == m_z) {
        relayout();
        return;
    }
    // Keep the map point under the focus (finger/pinch midpoint) in place.
    double fx = m_cx - m_vw / 2.0 + focusX;
    double fy = m_cy - m_vh / 2.0 + focusY;
    double factor = qPow(2.0, newZoom - m_z);
    m_cx = fx * factor - (focusX - m_vw / 2.0);
    m_cy = fy * factor - (focusY - m_vh / 2.0);
    m_z = newZoom;
    retireForZoom(factor);
    relayout();
    emit zoomChanged();
    if (m_following) centerOnLocation();   // zoom buttons/keys keep the dot centered
    m_settleTimer.start();
}

void MapController::zoomIn() { if (!m_flinging && !m_pinching) applyZoom(m_z + 1, m_vw / 2, m_vh / 2); }
void MapController::zoomOut() { if (!m_flinging && !m_pinching) applyZoom(m_z - 1, m_vw / 2, m_vh / 2); }
void MapController::zoomInAt(float x, float y) { if (!m_flinging && !m_pinching) applyZoom(m_z + 1, x, y); }

void MapController::onSettled()
{
    saveState();
}

void MapController::saveState()
{
    QSettings s;
    s.setValue("map/lat", geo::yToLat(m_cy, m_z));
    s.setValue("map/lon", geo::xToLon(m_cx, m_z));
    s.setValue("map/zoom", m_z);
}

void MapController::onStoreStatus(const QString &text)
{
    // Network trouble is transient: a toast with the fix, at most every 30 s.
    // Persistent problems (missing key, daily cap) stay in the banner.
    if (text.startsWith("Rete non disponibile") || text.startsWith("Errore tessere")) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastToastMs > 30000) {
            m_lastToastMs = now;
            emit toast(text.startsWith("Rete")
                           ? "Mappa non aggiornata: nessuna connessione. Controlla Wi-Fi o rete dati."
                           : "Il server della mappa non risponde, riprovo tra poco.");
        }
        if (!m_status.isEmpty()) { m_status.clear(); emit statusChanged(); }
        return;
    }
    m_status = text;
    emit statusChanged();
}

QString MapController::version() const
{
    return bb::ApplicationInfo().version();
}

void MapController::updateLoading()
{
    bool missing = false;
    foreach (const QString &k, m_visible)
        if (!m_shown.contains(k)) { missing = true; break; }
    if (!missing) {
        if (!m_stale.isEmpty()) dropStale();   // the new zoom level is complete
        m_loadingTimer.stop();
        if (m_loading) { m_loading = false; emit loadingChanged(); }
    } else if (!m_loading && !m_loadingTimer.isActive()) {
        m_loadingTimer.start();
    }
}

void MapController::onLoadingTimeout()
{
    foreach (const QString &k, m_visible) {
        if (!m_shown.contains(k)) {
            m_loading = true;
            emit loadingChanged();
            return;
        }
    }
}


// ------------------------------------------------------------------ location

QString MapController::locationStatus() const
{
    return m_locating ? m_loc->status() : QString();
}

void MapController::setFollowing(bool on)
{
    if (on == m_following) return;
    m_following = on;
    m_loc->setFast(on);   // 1 s updates only while the map follows the dot
    emit locationStateChanged();
}

void MapController::locateMe()
{
    if (m_locating && m_following && m_loc->hasFix()) {
        // Pressed while already following a known position: location off.
        // Before the first fix a press must not stop the search (repeated
        // "is it working?" taps restarted it every time on the Q5).
        m_locating = false;
        setFollowing(false);
        m_loc->setWanted(false);
    } else {
        m_locating = true;
        m_loc->setWanted(true);
        setFollowing(true);
        if (m_loc->hasFix()) centerOnLocation();
    }
    QSettings().setValue("location/on", m_locating);
    updateLocationOverlay();
    emit locationStateChanged();
    emit locationStatusChanged();
}

void MapController::centerOnLocation()
{
    if (!m_loc->hasFix() || m_flinging || m_pinching) return;
    m_cx = geo::lonToX(m_loc->longitude(), m_z);
    m_cy = geo::latToY(m_loc->latitude(), m_z);
    relayout();
    m_settleTimer.start();
}

void MapController::onLocationUpdated()
{
    if (m_following && !m_flinging && !m_pinching) centerOnLocation();   // relayout() updates the dot
    else updateLocationOverlay();
}

void MapController::updateLocationOverlay()
{
    if (!m_overlay) return;
    bool show = m_locating && m_loc->hasFix();
    if (!show) {
        m_dotView->setVisible(false);
        m_accuracyView->setVisible(false);
        m_arrowView->setVisible(false);
        return;
    }
    const double world = geo::worldSize(m_z);
    double wx = geo::lonToX(m_loc->longitude(), m_z);
    const double wy = geo::latToY(m_loc->latitude(), m_z);
    // Nearest copy of the world horizontally.
    const double centerX = m_originX + m_vw / 2.0;
    while (wx - centerX > world / 2) wx -= world;
    while (centerX - wx > world / 2) wx += world;
    const float px = float(wx - m_originX);
    const float py = float(wy - m_originY);
    const bool reliable = m_loc->reliable();

    // Grey dot when the position is old or imprecise (safety requirement).
    if (reliable == m_dotGrey) {
        m_dotGrey = !reliable;
        m_dotView->setImageSource(QUrl(m_dotGrey ? "asset:///loc_dot_grey.png" : "asset:///loc_dot.png"));
    }
    AbsoluteLayoutProperties *lp = qobject_cast<AbsoluteLayoutProperties *>(m_dotView->layoutProperties());
    if (lp) { lp->setPositionX(px - 24); lp->setPositionY(py - 24); }
    m_dotView->setVisible(true);

    // Accuracy circle: metres -> world pixels at this latitude.
    double metresPerPixel = 40075016.686 * qCos(m_loc->latitude() * M_PI / 180.0) / world;
    double r = m_loc->accuracy() > 0 ? m_loc->accuracy() / metresPerPixel : 0;
    if (r > 26 && r < 3000) {
        m_accuracyView->setPreferredSize(float(2 * r), float(2 * r));
        lp = qobject_cast<AbsoluteLayoutProperties *>(m_accuracyView->layoutProperties());
        if (lp) { lp->setPositionX(px - float(r)); lp->setPositionY(py - float(r)); }
        m_accuracyView->setVisible(true);
    } else {
        m_accuracyView->setVisible(false);
    }

    if (m_loc->headingValid() && reliable) {
        lp = qobject_cast<AbsoluteLayoutProperties *>(m_arrowView->layoutProperties());
        if (lp) { lp->setPositionX(px - 80); lp->setPositionY(py - 80); }
        ImplicitAnimationController guard = ImplicitAnimationController::create(m_arrowView).enabled(false);
        m_arrowView->setRotationZ(float(m_loc->heading()));
        m_arrowView->setVisible(true);
    } else {
        m_arrowView->setVisible(false);
    }
}

void MapController::updateForeground()
{
    // While navigating the GPS keeps running with the app minimized
    // (run_when_backgrounded); otherwise only with the map on screen.
    m_loc->setForeground((m_fullscreen && m_awake) || m_navigating);
}

void MapController::onAppFullscreen() { m_fullscreen = true; updateForeground(); }
void MapController::onAppBackground() { m_fullscreen = false; updateForeground(); saveState(); }
void MapController::onAppAsleep() { m_awake = false; updateForeground(); }
void MapController::onAppAwake() { m_awake = true; updateForeground(); }

// ------------------------------------------------------------------ pin / search

void MapController::updatePinOverlay()
{
    if (!m_pinView) return;
    if (!m_hasPin) {
        m_pinView->setVisible(false);
        return;
    }
    const double world = geo::worldSize(m_z);
    double wx = geo::lonToX(m_pinLon, m_z);
    const double centerX = m_originX + m_vw / 2.0;
    while (wx - centerX > world / 2) wx -= world;
    while (centerX - wx > world / 2) wx += world;
    AbsoluteLayoutProperties *lp = qobject_cast<AbsoluteLayoutProperties *>(m_pinView->layoutProperties());
    if (lp) {
        // The pin's tip (bottom centre) sits on the place.
        lp->setPositionX(float(wx - m_originX) - 28);
        lp->setPositionY(float(geo::latToY(m_pinLat, m_z) - m_originY) - 78);
    }
    m_pinView->setVisible(true);
}

void MapController::showPin(double lat, double lon, bool center)
{
    m_hasPin = true;
    m_pinLat = lat;
    m_pinLon = lon;
    if (center && !m_flinging && !m_pinching) {
        setFollowing(false);
        if (m_z < 16) {
            const double factor = qPow(2.0, 16 - m_z);
            m_z = 16;
            retireForZoom(factor);
            emit zoomChanged();
        }
        m_cx = geo::lonToX(lon, m_z);
        m_cy = geo::latToY(lat, m_z);
        relayout();
        m_settleTimer.start();
    } else {
        updatePinOverlay();
    }
}

void MapController::clearPin()
{
    m_hasPin = false;
    updatePinOverlay();
}

QVariantMap MapController::centerLatLon() const
{
    QVariantMap m;
    m["lat"] = geo::yToLat(m_cy, m_z);
    m["lon"] = geo::xToLon(m_cx, m_z);
    return m;
}

QVariantMap MapController::screenToLatLon(float x, float y) const
{
    // m_cx/m_cy already include any drag in progress.
    QVariantMap m;
    m["lat"] = geo::yToLat(m_cy - m_vh / 2.0 + y, m_z);
    double lon = geo::xToLon(m_cx - m_vw / 2.0 + x, m_z);
    while (lon > 180) lon -= 360;
    while (lon < -180) lon += 360;
    m["lon"] = lon;
    return m;
}

double MapController::distanceTo(double lat, double lon) const
{
    if (!m_loc->hasFix()) return -1;
    const double r = 6371000.0, k = M_PI / 180.0;
    double dLat = (lat - m_loc->latitude()) * k, dLon = (lon - m_loc->longitude()) * k;
    double a = qSin(dLat / 2) * qSin(dLat / 2) +
               qCos(m_loc->latitude() * k) * qCos(lat * k) * qSin(dLon / 2) * qSin(dLon / 2);
    return 2 * r * qAtan2(qSqrt(a), qSqrt(1 - a));
}

// ------------------------------------------------------------------ route / navigation

bool MapController::hasFix() const
{
    return m_loc->hasFix();
}

QVariantMap MapController::myPosition() const
{
    QVariantMap m;
    m["lat"] = m_loc->latitude();
    m["lon"] = m_loc->longitude();
    return m;
}

void MapController::setNavigating(bool on)
{
    if (on == m_navigating) return;
    m_navigating = on;
    // Screen stays on while guiding; normal timeout afterwards.
    Application::instance()->mainWindow()->setScreenIdleMode(
        on ? ScreenIdleMode::KeepAwake : ScreenIdleMode::Normal);
    if (on) {
        m_locating = true;
        m_loc->setWanted(true);
        setFollowing(true);
        m_loc->setFast(true);
        if (m_z < 17) {
            const double factor = qPow(2.0, 17 - m_z);
            m_z = 17;
            retireForZoom(factor);
            emit zoomChanged();
        }
        if (m_loc->hasFix()) centerOnLocation();
        else relayout();
    }
    updateForeground();
    emit locationStateChanged();
}

void MapController::setRoute(const QVector<QPointF> &lonLat, const QVector<QColor> &colors,
                             const QVector<bool> &walk)
{
    m_route = lonLat;
    m_routeColors = colors;
    m_routeWalk = walk;
    m_routeZoom = -1;
    clearRouteViews();
    relayout();
}

void MapController::clearRouteViews()
{
    QHash<QString, ImageView *>::iterator it = m_routeViews.begin();
    for (; it != m_routeViews.end(); ++it) {
        m_routeLayer->remove(it.value());
        it.value()->deleteLater();
    }
    m_routeViews.clear();
}

void MapController::updateRouteOverlay(const QHash<QString, QPair<int, int> > &needed)
{
    if (!m_routeLayer) return;
    if (m_route.size() < 2) {
        if (!m_routeViews.isEmpty()) clearRouteViews();
        return;
    }
    if (m_routeZoom != m_z) {
        // Project the route to world pixels once per zoom level.
        clearRouteViews();
        m_routePx.resize(m_route.size());
        for (int i = 0; i < m_route.size(); ++i)
            m_routePx[i] = QPointF(geo::lonToX(m_route.at(i).x(), m_z), geo::latToY(m_route.at(i).y(), m_z));
        m_routeZoom = m_z;
    }
    const int T = geo::kTileSize;
    QSet<QString> keep;
    for (QHash<QString, QPair<int, int> >::const_iterator it = needed.constBegin(); it != needed.constEnd(); ++it) {
        int tx = it.value().first, ty = it.value().second;
        QString k = QString("%1/%2").arg(tx).arg(ty);
        keep.insert(k);
        ImageView *iv = m_routeViews.value(k, 0);
        if (!iv) {
            iv = renderRouteTile(m_z, tx, ty);
            if (!iv) continue;          // route doesn't cross this tile
            m_routeViews.insert(k, iv);
        }
        AbsoluteLayoutProperties *lp = qobject_cast<AbsoluteLayoutProperties *>(iv->layoutProperties());
        if (lp) {
            lp->setPositionX(float(tx * T - m_originX));
            lp->setPositionY(float(ty * T - m_originY));
        }
    }
    QList<QString> gone;
    for (QHash<QString, ImageView *>::const_iterator it = m_routeViews.constBegin(); it != m_routeViews.constEnd(); ++it)
        if (!keep.contains(it.key())) gone << it.key();
    foreach (const QString &k, gone) {
        ImageView *iv = m_routeViews.take(k);
        m_routeLayer->remove(iv);
        iv->deleteLater();
    }
}

ImageView *MapController::renderRouteTile(int z, int tx, int ty)
{
    Q_UNUSED(z);
    const int T = geo::kTileSize;
    const double x0 = double(tx) * T, y0 = double(ty) * T;
    const double margin = 12;   // half the outline width
    // Segments whose bounding box touches this tile (with the pen margin).
    QVector<int> segs;
    for (int i = 0; i + 1 < m_routePx.size(); ++i) {
        const QPointF &a = m_routePx.at(i), &b = m_routePx.at(i + 1);
        if (qMax(a.x(), b.x()) >= x0 - margin && qMin(a.x(), b.x()) <= x0 + T + margin &&
            qMax(a.y(), b.y()) >= y0 - margin && qMin(a.y(), b.y()) <= y0 + T + margin)
            segs.append(i);
    }
    if (segs.isEmpty()) return 0;

    QImage img(T, T, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Two passes so outlines never cover the core of a neighbouring segment.
        // Driving/walking/cycling: dark outline + blue core (clearly distinct
        // from the map, safety requirement). Transit: the line's own colour;
        // walking legs of a transit trip: grey dashes.
        for (int pass = 0; pass < 2; ++pass) {
            for (int s = 0; s < segs.size(); ++s) {
                int i = segs.at(s);
                QPointF a(m_routePx.at(i).x() - x0, m_routePx.at(i).y() - y0);
                QPointF b(m_routePx.at(i + 1).x() - x0, m_routePx.at(i + 1).y() - y0);
                bool walk = i < m_routeWalk.size() && m_routeWalk.at(i);
                QColor color = i < m_routeColors.size() ? m_routeColors.at(i) : QColor();
                QPen pen;
                if (walk) {
                    pen = pass == 0 ? QPen(QColor(255, 255, 255), 11, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                                    : QPen(QColor(95, 99, 104), 7, Qt::DotLine, Qt::RoundCap, Qt::RoundJoin);
                } else if (color.isValid()) {
                    pen = pass == 0 ? QPen(color.darker(160), 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                                    : QPen(color, 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                } else {
                    pen = pass == 0 ? QPen(QColor(11, 87, 208), 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                                    : QPen(QColor(66, 133, 244), 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                }
                p.setPen(pen);
                p.drawLine(a, b);
            }
        }
    }
    // QImage keeps 0xAARRGGBB words (BGRA bytes on ARM little endian);
    // rgbSwapped() gives the RGBA byte order Cascades expects. fromPixels copies.
    QImage rgba = img.rgbSwapped();
    bb::ImageData data = bb::ImageData::fromPixels(rgba.constBits(), bb::PixelFormat::RGBA_Premultiplied,
                                                   T, T, rgba.bytesPerLine());
    ImageView *iv = ImageView::create().preferredSize(T, T);
    iv->setScalingMethod(ScalingMethod::Fill);
    iv->setLoadEffect(ImageViewLoadEffect::None);
    iv->setImplicitLayoutAnimationsEnabled(false);
    iv->setLayoutProperties(AbsoluteLayoutProperties::create());
    iv->setImage(Image(data));
    m_routeLayer->add(iv);
    return iv;
}
