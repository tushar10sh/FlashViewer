#include "render/MapCanvas.hpp"
#include "render/OsmTileRenderer.hpp"
#include "render/ColormapLegend.hpp"
#include "render/PixelHighlightOverlay.hpp"
#include "render/PaneChrome.hpp"
#include "gis/ScaleBar.hpp"
#include "gis/GeoScale.hpp"
#include "gis/CrsUtil.hpp"
#include "gis/WarpResampling.hpp"
#include "core/RasterLayer.hpp"
#include "core/VectorLayer.hpp"
#include "render/VectorRenderer.hpp"
#include "util/Logger.hpp"
#include "util/ErrorReporter.hpp"
#include "util/PerfMetrics.hpp"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>
#include <QOpenGLContext>
#include <QMessageBox>
#include <QCoreApplication>
#include <QPainter>
#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QStringList>

#include <ogr_spatialref.h>
#include <cmath>
#include <chrono>
#include <algorithm>

// --------------------------------------------------------------------------
// Phase 1 triangle shaders (shown when no layers are loaded)

static constexpr const char* kTriVert = R"glsl(
#version 410 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec3 a_color;
out vec3 v_color;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_color = a_color;
}
)glsl";

static constexpr const char* kTriFrag = R"glsl(
#version 410 core
in  vec3 v_color;
out vec4 frag_color;
void main() { frag_color = vec4(v_color, 1.0); }
)glsl";

// MIME type carrying a dragged layer's row index, set by LayerPanel's tree (Phase 6.3).
static constexpr const char* kLayerMime = "application/x-flashviewer-layer";
// MIME type carrying a dragged pane's id, set by PaneChrome's ID label (Phase 6.4).
static constexpr const char* kPaneMime = "application/x-flashviewer-pane";

namespace {
// Reproject a camera's center+scale from oldWkt to newWkt so the view stays put across a
// Project-CRS change (Phase 11, FR-CRS-2). Empty WKT = geographic (EPSG:4326). Returns
// false (camera left unchanged) when a transform cannot be built or yields non-finite
// coordinates — FR-CRS-5 spirit: never crash, best-effort.
bool fvReprojectCameraCrs(Camera& cam, const std::string& oldWkt, const std::string& newWkt) {
    auto setSR = [](OGRSpatialReference& sr, const std::string& wkt) -> bool {
        const char* in = wkt.empty() ? "EPSG:4326" : wkt.c_str();
        if (sr.SetFromUserInput(in) != OGRERR_NONE) return false;
        sr.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);   // x=east/lon, y=north/lat
        return true;
    };
    OGRSpatialReference srcSR, dstSR;
    if (!setSR(srcSR, oldWkt) || !setSR(dstSR, newWkt)) return false;
    if (srcSR.IsSame(&dstSR)) return true;   // nothing to do

    OGRCoordinateTransformation* ct = OGRCreateCoordinateTransformation(&srcSR, &dstSR);
    if (!ct) return false;

    const glm::dvec2 c = cam.center();
    double px = c.x,             py = c.y;                 // center
    double ox = c.x + cam.scale(), oy = c.y;               // one pixel east (old units)
    const int ok1 = ct->Transform(1, &px, &py);
    const int ok2 = ct->Transform(1, &ox, &oy);
    OGRCoordinateTransformation::DestroyCT(ct);
    if (!ok1 || !ok2 ||
        !std::isfinite(px) || !std::isfinite(py) ||
        !std::isfinite(ox) || !std::isfinite(oy))
        return false;

    cam.setCenter({px, py});
    const double newScale = std::hypot(ox - px, oy - py);   // new units-per-pixel
    if (std::isfinite(newScale) && newScale > 0.0) cam.setScale(newScale);
    return true;
}
}  // namespace

// --------------------------------------------------------------------------

MapCanvas::MapCanvas(LayerManager* layers, uint64_t paneId, QWidget* parent)
    : QOpenGLWidget(parent)
    , m_layers(layers)
    , m_pane_id(paneId)
    , m_tile_renderer(std::make_unique<TileRenderer>())
    , m_osm_renderer(std::make_unique<OsmTileRenderer>(this, this))
    , m_vector_renderer(std::make_unique<VectorRenderer>())
    , m_cm_legend(new ColormapLegend(this))
    , m_highlight_overlay(new PixelHighlightOverlay(this))
    , m_chrome(new PaneChrome(this))
    , m_scale_bar(new ScaleBar(this))   // per-pane ruler (Phase 6.4.5)
{
    // Small minimum so several panes always fit the central area (the QSplitter can
    // shrink them); without this, N×400 px exceeded the centre width and panes overlapped.
    setMinimumSize(120, 120);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);   // accept layers dragged from the Layers panel (Phase 6.3)

    // Pane chrome (Phase 6.1): forward gear-menu intents up to MainWindow, and make
    // the pane active when its menu is opened.
    connect(m_chrome, &PaneChrome::activatedByMenu, this, &MapCanvas::activated);
    connect(m_chrome, &PaneChrome::closeRequested,  this, &MapCanvas::paneCloseRequested);
    connect(m_chrome, &PaneChrome::renameRequested, this, &MapCanvas::paneRenameRequested);
    connect(m_chrome, &PaneChrome::colorRequested,  this, &MapCanvas::paneColorRequested);
    connect(m_chrome, &PaneChrome::syncRequested,   this, &MapCanvas::paneSyncRequested);
    connect(m_chrome, &PaneChrome::syncToggled,     this,
            [this](quint64 otherId){ emit paneSyncToggleRequested(otherId); });
    connect(m_chrome, &PaneChrome::unsyncRequested, this, &MapCanvas::paneUnsyncRequested);
    connect(m_chrome, &PaneChrome::scaleBarToggled, this, &MapCanvas::setScaleBarVisible);
    connect(m_chrome, &PaneChrome::colorbarToggled, this, &MapCanvas::setColorbarVisible);   // Phase 16 #5
    connect(m_chrome, &PaneChrome::crsRequested,    this, &MapCanvas::paneCrsRequested);   // Phase 11
    m_chrome->setScaleBarVisible(m_scalebar_visible);   // init the gear-menu checkmark state
    m_chrome->setColorbarVisible(m_colorbar_visible);   // init the gear-menu checkmark state (Phase 16 #5)
    m_chrome->setPaneDragId(m_pane_id);   // ID label drags carry this pane's id (Phase 6.4)
    m_chrome->move(8, 8);
    m_chrome->raise();

    // Repaint timer: keeps updating until all tiles are loaded
    m_repaint_timer = new QTimer(this);
    m_repaint_timer->setInterval(150);
    connect(m_repaint_timer, &QTimer::timeout, this, [this]{ update(); });

    // FR-NAV-4: auto-fit only when the FIRST layer is added to THIS pane; later adds
    // keep the user's current view (Space / single-layer fit remain available on demand).
    // The manager is shared across panes, so every signal checks this pane's own count.
    connect(m_layers, &LayerManager::layerAdded,   this, [this]{
        // Derive/apply this pane's Project CRS from the bottom raster BEFORE fitting, so
        // on the first layer fitToLayers() has the final say on the camera (in the new
        // CRS) rather than being reprojected afterwards (Phase 11, FR-CRS-3).
        refreshDerivedProjectCrs();   // basemap + raster tiles follow the (bottom) layer's CRS
        if (paneLayerCount() == 1) fitToLayers();
        update();
    });
    connect(m_layers, &LayerManager::layerRemoved, this, [this]{
        // When this pane's last layer goes, the camera is still parked in the old
        // (possibly projected) CRS — reset to a world view so the basemap reappears.
        // BUT a *synced* pane must not reset: resetToWorldView emits cameraChanged, which
        // the sync group broadcasts to siblings — dragging a still-populated sibling to
        // world view (its layer "disappears"). A blank synced pane keeps the shared camera
        // instead; it's empty anyway (Phase 6.9).
        if (paneLayerCount() == 0) { if (m_sync_role == 0) resetToWorldView(); }
        else                       refreshDerivedProjectCrs();
        update();
    });
    connect(m_layers, &LayerManager::layerChanged, this, [this]{
        refreshDerivedProjectCrs();
        update();
    });

    // On-the-fly reprojection notices (Phase 11, requirement #2 / FR-CRS-5). The TileRenderer
    // reports each visible layer's reprojection status per frame; announce once per
    // (layer, CRS-epoch), deferred out of paintGL so the modal doesn't run inside a paint.
    m_tile_renderer->setReprojectStatusCallback(
        [this](uint64_t layerId, bool reprojecting, bool nativeFallback) {
            if (!reprojecting && !nativeFallback) return;
            const auto key = std::make_pair(layerId, m_crs_epoch);
            if (m_reproject_announced.count(key)) return;
            m_reproject_announced.insert(key);
            QTimer::singleShot(0, this, [this, layerId, nativeFallback] {
                showReprojectionNotice(layerId, nativeFallback);
            });
        });
}

// --------------------------------------------------------------------------
// Per-pane layer selection (Phase 6): the manager is shared; this pane shows
// only the layers assigned to its paneId.

std::vector<std::shared_ptr<Layer>> MapCanvas::paneLayers() const {
    if (!m_layers) return {};
    return fvFilterPane(m_layers->layers(), m_pane_id);
}

int MapCanvas::paneLayerCount() const {
    if (!m_layers) return 0;
    int n = 0;
    for (const auto& l : m_layers->layers())
        if (l && l->paneId() == m_pane_id) ++n;
    return n;
}

std::shared_ptr<Layer> MapCanvas::activeLayerInPane() const {
    if (!m_layers) return nullptr;
    auto active = m_layers->activeLayer();
    if (active && active->paneId() == m_pane_id) return active;
    return nullptr;
}

bool MapCanvas::paneIsGeographic() const {
    // The scale-bar / readout unit depends on the pane's PROJECT CRS — the CRS the camera
    // operates in — NOT the source raster's CRS. A UTM raster shown in a geographic Project
    // CRS pans in degrees, so the ruler must use degrees (Phase 11 fix). Cached in
    // m_project_is_geographic (refreshed on every Project-CRS change; empty CRS = geographic).
    return m_project_is_geographic;
}

MapCanvas::~MapCanvas() {
    // FR-ERR-7 safety: handleContextLoss() must NOT run during base-class
    // (~QOpenGLWidget) teardown — by then m_tile_renderer/m_osm_renderer are already
    // freed, so the slot would use freed memory (crash on pane close). Disconnect the
    // context signal here; normal GL cleanup is done right below. Genuine runtime
    // context loss still fires the slot: the widget is alive then and this destructor
    // has not run.
    if (QOpenGLContext* c = context())
        disconnect(c, &QOpenGLContext::aboutToBeDestroyed,
                   this, &MapCanvas::handleContextLoss);
    if (!m_gl_ready) return;
    makeCurrent();
    if (m_tri_vao) glDeleteVertexArrays(1, &m_tri_vao);
    if (m_tri_vbo) glDeleteBuffers(1, &m_tri_vbo);
    if (m_tri_shader) m_tri_shader->destroy(*this);
    m_tile_renderer->destroy(*this);
    m_osm_renderer->destroy(*this);
    doneCurrent();
    m_gl_ready = false;   // defense-in-depth: any late slot call is a no-op
}

// --------------------------------------------------------------------------

static void showGlError(const QString& msg) {
    QMetaObject::invokeMethod(qApp, [msg]{
        QMessageBox::critical(nullptr, "FlashViewer \xe2\x80\x94 OpenGL Error", msg);
        QCoreApplication::exit(1);
    }, Qt::QueuedConnection);
}

void MapCanvas::initializeGL() {
    // Step 1: verify a valid context was handed to us by Qt's platform layer.
    // This catches the case where xcb-glx or mesa-dri-drivers are absent —
    // the platform plugin creates an invalid context rather than crashing, but
    // proceeding with it would segfault on the first GL call.
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (!ctx || !ctx->isValid()) {
        FV_CRITICAL("MapCanvas::initializeGL: no valid OpenGL context. "
                    "Ensure mesa-dri-drivers and libxcb-glx are installed. "
                    "On headless servers use: xvfb-run -a ./FlashViewer");
        showGlError(
            "Could not obtain a valid OpenGL context.\n\n"
            "On RHEL / AlmaLinux / Rocky Linux 9, install the missing runtime packages:\n"
            "  sudo dnf install mesa-dri-drivers libxcb-glx\n\n"
            "On a headless server, prefix the launch command with xvfb-run -a\n"
            "(Xvfb requires mesa-dri-drivers for its software OpenGL rasteriser).");
        return;
    }

    // Step 2: resolve all GL 4.1 Core function pointers.
    if (!initializeOpenGLFunctions()) {
        FV_CRITICAL("MapCanvas::initializeGL: failed to resolve OpenGL 4.1 Core "
                    "functions — context exists but does not expose GL 4.1 Core Profile.");
        showGlError(
            "OpenGL 4.1 Core Profile functions could not be resolved.\n\n"
            "The GPU driver reports a context but does not expose the required\n"
            "OpenGL 4.1 Core entry points. Update your GPU driver.\n\n"
            "Minimum supported GPUs: NVIDIA Kepler (GTX 600+), "
            "AMD GCN (HD 7000+), Intel HD Graphics 4000+.");
        return;
    }

    // Step 3: confirm the driver actually gave us at least GL 4.1.
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* ren = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    // All checks passed — safe to make GL calls from here on.
    m_gl_ready = true;
    const char* vnd = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    m_gl_info = {
        QString::fromUtf8(ren ? ren : "unknown"),
        QString::fromUtf8(vnd ? vnd : "unknown"),
        QString::fromUtf8(ver ? ver : "unknown"),
    };

    glEnable(GL_BLEND);
    // Per-layer opacity compositing, QGIS/ENVI-style: each layer is blended "source-over" onto
    // the opaque map canvas (and the layers already drawn beneath it), so as opacity drops the
    // canvas background (or the layer below) shows through — NOT the desktop.
    //   • COLOUR: GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA  → rgb = layer·a + below·(1−a).
    //   • ALPHA:  GL_ZERO / GL_ONE                        → dst_a keeps the cleared 1.0.
    // Keeping the framebuffer ALPHA at 1 is the crucial part: a QOpenGLWidget whose FBO alpha
    // drops below 1 is composited TRANSLUCENTLY by the window compositor (the Linux/WSL
    // "see-through to the desktop" and the whitish tinge; Windows can't do layered GL contexts
    // so it instead showed no fade). With alpha pinned to 1 the widget is always opaque and the
    // opacity effect lives purely in RGB, identically on Windows/Linux/macOS (#7). The clear
    // colour below is opaque, and `glClear` seeds dst_a = 1 every frame.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    if (m_dark_bg)
        glClearColor(0.051f, 0.067f, 0.090f, 1.0f);   // GitHub Dark  #0d1117
    else
        glClearColor(0.965f, 0.973f, 0.980f, 1.0f);   // GitHub Light #f6f8fa

    initTriangle();
    m_tile_renderer->init(*this);
    m_osm_renderer->init(*this);

    // GL context-loss recovery (FR-ERR-7): when the platform tears down the GPU
    // context (driver reset, GPU switch), free resources while it is still current;
    // Qt calls initializeGL() again on the fresh context and we rebuild.
    if (ctx)
        connect(ctx, &QOpenGLContext::aboutToBeDestroyed,
                this, &MapCanvas::handleContextLoss, Qt::UniqueConnection);
}

void MapCanvas::handleContextLoss() {
    if (!m_gl_ready) return;
    makeCurrent();
    if (m_tri_vao) { glDeleteVertexArrays(1, &m_tri_vao); m_tri_vao = 0; }
    if (m_tri_vbo) { glDeleteBuffers(1, &m_tri_vbo); m_tri_vbo = 0; }
    if (m_tri_shader) { m_tri_shader->destroy(*this); m_tri_shader.reset(); }
    if (m_tile_renderer) m_tile_renderer->destroy(*this);
    if (m_osm_renderer)  m_osm_renderer->destroy(*this);
    doneCurrent();
    m_gl_ready = false;   // next initializeGL() rebuilds everything
    ErrorReporter::instance().report(
        3, QStringLiteral("OpenGL"),
        QStringLiteral("GPU rendering context was lost; rebuilding resources."));
}

std::size_t MapCanvas::gpuResidentBytes() const {
    return m_tile_renderer ? m_tile_renderer->tileCache().residentBytes() : 0;
}

int MapCanvas::gpuResidentTiles() const {
    return m_tile_renderer ? m_tile_renderer->tileCache().size() : 0;
}

void MapCanvas::resizeGL(int w, int h) {
    if (!m_gl_ready) return;
    glViewport(0, 0, w, h);
    m_camera.setViewportSize(w, h);

    // First real sizing: park the camera on the canonical world view, so the basemap opens
    // as a world map — prime meridian centred, Americas left, Asia right. Without this the
    // camera keeps Camera{}'s defaults (centre 0,0 at 1 degree per pixel), which on a
    // typical viewport spans ~1000 degrees: the basemap then arrives as several wrapped
    // copies of the world instead of one. Guarded so it can never override a pane that
    // already has content, is driven by a sync group, or works in a projected CRS.
    if (!m_initial_view_done && w > 0 && h > 0) {
        m_initial_view_done = true;
        if (paneLayerCount() == 0 && m_sync_role == 0 && m_project_wkt.empty()) {
            m_camera.fitToExtent(worldExtent());
            emitZoomLevel();   // not cameraChanged: this runs inside a resize
        }
    }
}

void MapCanvas::resizeEvent(QResizeEvent* event) {
    QOpenGLWidget::resizeEvent(event);
    repositionOverlays();
    emit canvasResized(width(), height());
}

void MapCanvas::repositionOverlays() {
    if (m_cm_legend) {
        // Pin legend to top-right corner with a 10 px margin
        int x = width()  - m_cm_legend->width()  - 10;
        int y = 10;
        m_cm_legend->move(std::max(0, x), y);
    }
    if (m_highlight_overlay)
        m_highlight_overlay->resize(size());
    if (m_chrome) { m_chrome->move(8, 8); m_chrome->raise(); }
    if (m_scale_bar) {
        m_scale_bar->move(10, height() - m_scale_bar->height() - 10);   // bottom-left
        m_scale_bar->raise();
    }
}

void MapCanvas::setPaneLabel(const QString& label) {
    if (m_chrome) m_chrome->setLabelText(label);
}

void MapCanvas::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (m_chrome) m_chrome->setActiveAppearance(active);
    update();   // repaint the active-pane border
}

void MapCanvas::setPaneColor(const QColor& c) {
    m_pane_color = c;
    if (m_chrome) m_chrome->setPaneColor(c);
    update();   // repaint border
}

void MapCanvas::setSyncInfo(const FvPaneSyncInfo& info) {
    m_sync_role = info.role;   // 0 = None; !=0 means this pane is in a sync group (Phase 6.9)
    if (m_chrome) m_chrome->setSyncInfo(info);
}

void MapCanvas::setSyncInfoResolver(std::function<std::vector<PaneSyncEntry>()> r) {
    if (m_chrome) m_chrome->setSyncInfoResolver(std::move(r));
}

void MapCanvas::setGhostCursor(double geo_x, double geo_y, bool active) {
    m_ghost_active = active;
    m_ghost_x = geo_x;
    m_ghost_y = geo_y;
    update();   // redraw the marker overlay
}

void MapCanvas::paintGL() {
    if (!m_gl_ready) return;
    // Frame timer (NFR-PERF-1): time the render cost of this frame; recorded below,
    // before the HUD is drawn, so the HUD's own paint isn't counted.
    const auto perf_t0 = std::chrono::steady_clock::now();

    // Re-establish the compositing GL state EVERY frame before drawing the basemap + tiles.
    // The pane border / scale bar / HUD are drawn later in this method with QPainter, which
    // reconfigures the context's blend func, colour mask and clear colour and does NOT restore
    // them — so on every frame after the first, the tiles would otherwise be drawn with
    // QPainter's leftover state instead of ours. That silently broke per-layer opacity: the
    // tile RGB was written unblended (full image → no fade on Windows) while opacity leaked
    // into the framebuffer alpha (→ see-through to the desktop on Linux/WSL). Setting it here,
    // not just in initializeGL(), is the fix (#7). Colour: src-over so opacity blends the layer
    // over the canvas/layers beneath; ALPHA pinned to the cleared 1.0 (GL_ZERO, GL_ONE) so the
    // widget stays fully opaque and never leaks to the compositor.
    if (m_dark_bg) glClearColor(0.051f, 0.067f, 0.090f, 1.0f);   // GitHub Dark  #0d1117
    else           glClearColor(0.965f, 0.973f, 0.980f, 1.0f);   // GitHub Light #f6f8fa
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    bool any_missing = false;

    // OSM basemap drawn first (bottom-most)
    if (m_osm_renderer->isEnabled())
        m_osm_renderer->render(*this, m_camera);

    auto pane_layers = paneLayers();
    if (!pane_layers.empty())
        any_missing = !m_tile_renderer->render(*this, m_camera, pane_layers,
                                               m_project_wkt, m_crs_epoch);

    // Vector layer overlay drawn on top of imagery
    if (m_vector_renderer && !pane_layers.empty()) {
        QPainter p(this);
        m_vector_renderer->render(p, m_camera, pane_layers, m_project_wkt);
    }

    // FR-RND-7: keep refreshing while tiles load, but stop once they are all
    // ready OR a bounded timeout (kRepaintBudgetMs ≤ 30 s) elapses — a layer
    // that never finishes loading must not repaint forever.
    if (any_missing) {
        if (!m_repaint_timer->isActive()) {
            m_repaint_clock.restart();   // start of a fresh load burst
            m_repaint_timed_out = false;
        }
        if (fvRepaintWithinBudget(m_repaint_clock.elapsed())) {
            m_repaint_timer->start();
        } else {
            m_repaint_timer->stop();
            if (!m_repaint_timed_out) {
                m_repaint_timed_out = true;
                FV_WARN("MapCanvas: tile refresh hit the {} s budget with tiles "
                        "still loading — stopping progressive refresh.",
                        kRepaintBudgetMs / 1000);
            }
        }
    } else {
        m_repaint_timer->stop();
        m_repaint_timed_out = false;
    }

    // Pane border (Phase 6.2.1): drawn in the pane's own colour — SOLID when this is the
    // active pane, faint/translucent when dormant. QPainter over the QOpenGLWidget is
    // supported when created inside paintGL().
    if (m_pane_color.isValid() && !m_capturing) {   // skip the border in screenshots (grabForExport)
        QColor c = m_pane_color;
        c.setAlpha(m_active ? 255 : 55);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(c, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(1, 1, width() - 2, height() - 2);
    }

    // Per-pane scale bar (Phase 6.4.5): track this pane's own camera + representative layer's
    // CRS. update() only recomputes the bar and schedules the child widget's own repaint (no
    // paintGL recursion); isGeographic() is cached so this is cheap per frame. Width can change
    // with the label, so re-anchor it bottom-left.
    if (m_scale_bar && m_scalebar_visible) {
        m_scale_bar->update(m_camera, paneIsGeographic());
        m_scale_bar->move(10, height() - m_scale_bar->height() - 10);
    }

    // Ghost cursor (Phase 6.5): a thin close-stroke "X" at a synced sibling pane's cursor,
    // mapped through THIS pane's camera (panes in a sync group share a coordinate space).
    // Drawn directly (not an SVG) so it can adopt the pane colour over a dark halo cheaply.
    if (m_ghost_active) {
        const glm::dvec2 s = m_camera.geoToScreen(m_ghost_x, m_ghost_y);
        if (s.x >= 0 && s.y >= 0 && s.x <= width() && s.y <= height()) {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing, true);
            const QColor c = m_pane_color.isValid() ? m_pane_color : QColor(255, 255, 255);
            // Halo: a much darker shade of the pane colour (not pure black) so the marker
            // stays recognisably "this pane's" while keeping contrast over imagery.
            QColor halo = c.darker(450);
            halo.setAlpha(210);
            const int x = int(s.x), y = int(s.y), r = 6;   // half-diagonal extent
            auto drawX = [&](const QPen& pen) {
                p.setPen(pen);
                p.drawLine(x - r, y - r, x + r, y + r);
                p.drawLine(x - r, y + r, x + r, y - r);
            };
            drawX(QPen(halo, 3.0, Qt::SolidLine, Qt::RoundCap));   // much darker halo
            drawX(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));      // X on top
        }
    }

    // Frame timer (NFR-PERF-1): record this frame's render cost, then draw the HUD
    // (FR-APP-14) last so its own paint isn't included in the measured frame time.
    const double frame_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - perf_t0).count();
    PerfMetrics::instance().pushFrame(frame_ms);
    if (m_perf_hud_visible && !m_capturing)   // omitted from screenshots (grabForExport)
        drawPerfHud();
}

// Performance HUD overlay (FR-APP-14): a compact bottom-right readout of live frame stats,
// last open-latency, stall count, and this pane's estimated VRAM. Reads always-on
// PerfMetrics + the pane's tile-cache accounting; no GL queries. QPainter-over-GL is
// valid when constructed inside paintGL (same pattern as the pane border / ghost cursor).
void MapCanvas::drawPerfHud() {
    const FrameStats fs = PerfMetrics::instance().frameStats();
    const double vram_mb = double(gpuResidentBytes()) / (1024.0 * 1024.0);
    const double open_ms = PerfMetrics::instance().lastOpenLatencyMs();
    const int    stalls  = PerfMetrics::instance().stallCount();

    QStringList lines;
    lines << QStringLiteral("FPS %1   p99 %2 ms   max %3 ms")
                 .arg(fs.fps(), 0, 'f', 0)
                 .arg(fs.p99_ms, 0, 'f', 1)
                 .arg(fs.max_ms, 0, 'f', 1);
    lines << QStringLiteral("open %1 ms   stalls %2   >100ms %3")
                 .arg(open_ms, 0, 'f', 0)
                 .arg(stalls)
                 .arg(fs.over_100ms);
    lines << QStringLiteral("VRAM %1 MB / %2 tiles")
                 .arg(vram_mb, 0, 'f', 1)
                 .arg(gpuResidentTiles());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSizeF(p.font().pointSizeF() * 0.9);
    p.setFont(f);
    const QFontMetrics fm(f);
    int tw = 0;
    for (const auto& l : lines) tw = std::max(tw, fm.horizontalAdvance(l));
    const int pad = 6, lh = fm.height();
    const int boxW = tw + 2 * pad;
    const int boxH = int(lines.size()) * lh + 2 * pad;
    // Anchored bottom-right; the scale bar owns the bottom-left, so they never collide.
    const int x = width() - boxW - 10, y = height() - boxH - 10;

    // A frame over the NFR-PERF-1 hard cap tints the panel red as a quick visual cue.
    const bool warn = fs.max_ms > kFrameBudgetHardMs;
    QColor bg = warn ? QColor(90, 20, 20, 200) : QColor(0, 0, 0, 170);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(x, y, boxW, boxH, 4, 4);

    p.setPen(QColor(230, 230, 230));
    for (int i = 0; i < lines.size(); ++i)
        p.drawText(x + pad, y + pad + (i + 1) * lh - fm.descent(), lines[i]);
}

void MapCanvas::setPerfHudVisible(bool on) {
    if (m_perf_hud_visible == on) return;
    m_perf_hud_visible = on;
    update();
}

// --------------------------------------------------------------------------

void MapCanvas::initTriangle() {
    m_tri_shader = std::make_unique<GlslProgram>();
    if (!m_tri_shader->compile(*this, kTriVert, kTriFrag))
        FV_ERROR("MapCanvas: triangle shader failed");

    static const float kVerts[] = {
         0.0f,  0.6f,  0.20f, 0.60f, 1.00f,
        -0.5f, -0.3f,  0.00f, 0.80f, 0.50f,
         0.5f, -0.3f,  0.80f, 0.30f, 0.90f,
    };
    glGenVertexArrays(1, &m_tri_vao);
    glGenBuffers(1, &m_tri_vbo);
    glBindVertexArray(m_tri_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_tri_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), reinterpret_cast<void*>(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void MapCanvas::renderTriangle() {
    if (!m_tri_shader || !m_tri_shader->isValid()) return;
    m_tri_shader->bind(*this);
    glBindVertexArray(m_tri_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    m_tri_shader->release(*this);
}

// --------------------------------------------------------------------------

void MapCanvas::setDarkBackground(bool dark) {
    m_dark_bg = dark;
    if (!m_gl_ready) return;
    makeCurrent();
    if (dark)
        glClearColor(0.051f, 0.067f, 0.090f, 1.0f);   // GitHub Dark  #0d1117
    else
        glClearColor(0.965f, 0.973f, 0.980f, 1.0f);   // GitHub Light #f6f8fa
    doneCurrent();
    update();
}

void MapCanvas::onThemeChanged() {
    setDarkBackground(m_dark_bg);
}

void MapCanvas::clearOsmCache() {
    if (!m_gl_ready) return;
    makeCurrent();
    m_osm_renderer->clearCache(*this);
    doneCurrent();
}

void MapCanvas::refreshDerivedProjectCrs() {
    if (m_project_crs_user_set) return;   // an explicit user CRS wins over the derived default
    // Derived Project CRS = the bottom-most raster layer's CRS (the first one drawn).
    // Scanning front→back, the LAST raster seen is the bottom (TileRenderer draws in
    // reverse). This is the FR-CRS-3 default (first layer into an empty pane).
    std::string wkt;
    for (const auto& l : paneLayers()) {
        if (l->type() == LayerType::Raster) {
            auto* rl = static_cast<RasterLayer*>(l.get());
            if (rl->dataset()) wkt = rl->dataset()->crsWkt();
        }
    }
    setProjectCrsWkt(wkt, /*userInitiated=*/false);
}

void MapCanvas::setProjectCrsWkt(const std::string& wkt, bool userInitiated) {
    // Validate: reject an unparseable WKT/EPSG string (keep the current CRS). "" = geographic.
    if (!wkt.empty()) {
        OGRSpatialReference sr;
        if (sr.SetFromUserInput(wkt.c_str()) != OGRERR_NONE) {
            FV_WARN("MapCanvas: rejected invalid Project CRS input for pane {}", m_pane_id);
            return;
        }
    }
    if (userInitiated) m_project_crs_user_set = true;
    if (wkt == m_project_wkt) return;   // no CRS change → nothing to re-render

    const std::string oldWkt = m_project_wkt;
    m_project_wkt = wkt;
    ++m_crs_epoch;
    // Cache geographic-ness for the scale bar / m-per-px readout (units follow the Project CRS).
    if (m_project_wkt.empty()) {
        m_project_is_geographic = true;
    } else {
        OGRSpatialReference sr;
        m_project_is_geographic =
            (sr.SetFromUserInput(m_project_wkt.c_str()) == OGRERR_NONE) && sr.IsGeographic();
    }

    // Keep the view put across the change (FR-CRS-2). Best-effort: on failure the camera
    // is left as-is (never crash — FR-CRS-5).
    fvReprojectCameraCrs(m_camera, oldWkt, m_project_wkt);
    m_camera.setViewportSize(width(), height());

    if (m_osm_renderer) m_osm_renderer->setProjectCrs(m_project_wkt);   // basemap follows pane CRS
    purgePaneTiles();                    // free tiles warped into the old CRS (FR-CRS-2)
    m_reproject_announced.clear();       // re-announce on-the-fly reprojection under the new CRS

    emit projectCrsChanged(QString::fromStdString(m_project_wkt));
    emit cameraChanged(m_camera);        // synced siblings reproject the shared camera themselves
    emitZoomLevel();
    update();
}

std::shared_ptr<CrsTransformer> MapCanvas::wgs84Transformer() const {
    return fvGetWgs84Transformer(m_project_wkt);
}

void MapCanvas::clearProjectCrsOverride() {
    m_project_crs_user_set = false;
    refreshDerivedProjectCrs();   // re-apply the bottom-raster default (no-op if unchanged)
}

void MapCanvas::purgePaneTiles() {
    if (!m_gl_ready || !m_tile_renderer) return;
    makeCurrent();
    for (const auto& l : paneLayers())
        if (l && l->type() == LayerType::Raster)
            m_tile_renderer->invalidateLayer(*this, static_cast<RasterLayer*>(l.get())->layerId());
    doneCurrent();
}

void MapCanvas::showReprojectionNotice(uint64_t layer_id, bool nativeFallback) {
    // Resolve the layer's display name + source CRS for the message.
    QString layerName = tr("Layer");
    std::string srcWkt;
    for (const auto& l : paneLayers()) {
        if (l->type() != LayerType::Raster) continue;
        auto* rl = static_cast<RasterLayer*>(l.get());
        if (rl->layerId() == layer_id) {
            layerName = rl->name();
            if (rl->dataset()) srcWkt = rl->dataset()->crsWkt();
            break;
        }
    }
    const QString from = fvCrsShortName(srcWkt);
    const QString to   = fvCrsShortName(m_project_wkt);

    if (nativeFallback) {
        // Phase 17 #4: the layer can't be reprojected into the pane CRS, so it's drawn in
        // its own (native) CRS and may not align with this pane's other layers. Log + a
        // non-modal warning banner (FR-CRS-5/6) — no raw PROJ/GDAL error surfaces.
        FV_WARN("On-the-fly reprojection unavailable: layer '{}' ({}) → pane CRS {}; shown in native CRS",
                layerName.toStdString(), from.toStdString(), to.toStdString());
        emit reprojectionNotice(
            tr("⚠ \"%1\" cannot be reprojected into %2 — shown in its native CRS (%3); "
               "it may not align with this pane.")
                .arg(layerName, to, from),
            /*failed=*/true);
        return;
    }

    // FR-CRS-6: a non-modal notice that the layer is displayed reprojected on the fly.
    FV_INFO("On-the-fly reprojection: layer '{}' {} → pane CRS {}",
            layerName.toStdString(), from.toStdString(), to.toStdString());
    emit reprojectionNotice(
        tr("↻ \"%1\" is displayed reprojected on the fly (%2 → %3). Analysis uses source pixels.")
            .arg(layerName, from, to),
        /*failed=*/false);
}

double MapCanvas::metersPerPixel() const {
    return fvMetersPerPixel(m_camera.scale(), paneIsGeographic());
}

void MapCanvas::emitZoomLevel() {
    emit zoomLevelChanged(metersPerPixel());
}

void MapCanvas::setScaleBarVisible(bool on) {
    m_scalebar_visible = on;
    if (m_scale_bar) m_scale_bar->setVisible(on);
    if (m_chrome) m_chrome->setScaleBarVisible(on);   // keep the gear-menu checkmark in sync
    update();
}

void MapCanvas::setColorbarVisible(bool on) {
    m_colorbar_visible = on;
    if (m_chrome) m_chrome->setColorbarVisible(on);   // keep the gear-menu checkmark in sync
    // MainWindow::updatePaneLegends honours colorbarVisible() and picks the topmost visible
    // grayscale layer (or hides the legend when off); ask it to re-evaluate now (Phase 16 #5).
    emit colorbarVisibilityChanged();
    update();
}

QPixmap MapCanvas::grabForExport() {
    // Omit UI controls from the screenshot: hide the pane chrome (ID label + gear) and skip
    // the pane border while rendering. The map furniture (colorbar legend, scale bar, pixel
    // highlight) are other child overlays and remain in the grab.
    const bool chromeWasVisible = m_chrome && m_chrome->isVisible();
    if (m_chrome) m_chrome->hide();
    m_capturing = true;
    QPixmap px = grab();
    m_capturing = false;
    if (m_chrome && chromeWasVisible) m_chrome->show();
    update();
    return px;
}

void MapCanvas::resetToWorldView() {
    // Empty pane → geographic identity. Clear any user CRS override and bump the epoch so
    // tiles warped into the old (projected) CRS are invalidated (Phase 11). The camera is
    // set to the world extent directly here, so no camera reprojection is needed.
    if (!m_project_wkt.empty() || m_project_crs_user_set) {
        m_project_wkt.clear();
        m_project_crs_user_set = false;
        m_project_is_geographic = true;   // world view is geographic (degrees)
        ++m_crs_epoch;
        purgePaneTiles();
        m_reproject_announced.clear();
        emit projectCrsChanged(QString());
    }
    m_osm_renderer->setProjectCrs(std::string());   // geographic (identity)
    fitCameraToWorld();
    update();
    emit cameraChanged(m_camera);
    emitZoomLevel();
}

void MapCanvas::invalidateLayer(uint64_t layer_id) {
    if (!m_gl_ready) return;
    makeCurrent();
    m_tile_renderer->invalidateLayer(*this, layer_id);
    doneCurrent();
    update();
}

void MapCanvas::setCamera(const Camera& cam) {
    m_camera = cam;
    m_camera.setViewportSize(width(), height());
    update();
    emitZoomLevel();   // keep the status readout live under sync / programmatic camera sets
}

void MapCanvas::setCameraFromSync(const Camera& cam, const std::string& src_wkt) {
    Camera adjusted = cam;
    // Reproject center+scale from the source pane's CRS into ours when they differ, so the
    // two panes stay over the same ground location (Phase 11, FR-CRS-2). Best-effort: on a
    // transform failure the camera is left as broadcast (never crash — FR-CRS-5).
    if (src_wkt != m_project_wkt)
        fvReprojectCameraCrs(adjusted, src_wkt, m_project_wkt);
    adjusted.setViewportSize(width(), height());
    if (adjusted == m_camera) return;   // echo guard: nothing changed for this pane
    setCamera(adjusted);
}

void MapCanvas::fitCameraToWorld() {
    m_camera.setViewportSize(width(), height());
    m_camera.fitToExtent(worldExtent());
}

void MapCanvas::fitToLayers() {
    auto pane_layers = paneLayers();
    if (pane_layers.empty()) {
        // No layers, but the basemap is still drawn — fit the world instead of doing
        // nothing, so Fit All re-centres on the prime meridian. That is also the way back
        // after panning east or west into a wrapped world copy. Only meaningful while the
        // pane is geographic: under a projected Project CRS the +/-180 degree extent is not
        // this pane's world, so the old no-op stands.
        if (!m_project_wkt.empty() && !m_project_is_geographic) return;
        fitCameraToWorld();
        update();
        emit cameraChanged(m_camera);
        emitZoomLevel();
        return;
    }

    Extent combined = Extent::invalid();
    for (const auto& l : pane_layers) {
        if (!l || !l->visible()) continue;
        if (l->type() == LayerType::Raster) {
            auto* rl = static_cast<RasterLayer*>(l.get());
            Extent e = rl->extent();
            if (auto* ds = rl->dataset()) {
                auto wv = ds->warpedView(m_project_wkt);
                if (!wv.failed && wv.extent.isValid()) e = wv.extent;
            }
            combined = combined.isValid() ? combined.united(e) : e;
        } else if (l->type() == LayerType::Vector) {
            auto* vl = static_cast<VectorLayer*>(l.get());
            if (vl->dataset()) {
                auto geoms = vl->dataset()->geometriesForCrs(m_project_wkt);
                if (geoms && geoms->extent.isValid()) {
                    combined = combined.isValid() ? combined.united(geoms->extent) : geoms->extent;
                }
            }
        }
    }
    if (combined.isValid()) {
        m_camera.setViewportSize(width(), height());
        m_camera.fitToExtent(combined);
        update();
        emit cameraChanged(m_camera);
        emitZoomLevel();
    }
}

// --------------------------------------------------------------------------
// Mouse events

void MapCanvas::setInspectMode(bool on) {
    if (m_inspect_mode == on) return;
    m_inspect_mode = on;
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    if (!on && m_highlight_overlay)
        m_highlight_overlay->clearHighlight();
    emit inspectModeChanged(on);
}

void MapCanvas::clearInspectHighlight() {
    if (m_highlight_overlay) m_highlight_overlay->clearHighlight();
}

void MapCanvas::updateHighlightForGeo(double geo_x, double geo_y) {
    if (!m_highlight_overlay) return;
    // Representative raster: the active-in-pane layer if it is a raster, else the first raster
    // this pane shows (mirrors paneIsGeographic / the inspector's representative-layer pick, so
    // a synced sibling snaps the click to its own dataset — Phase 6.8.3).
    std::shared_ptr<Layer> rep = activeLayerInPane();
    if (!rep || rep->type() != LayerType::Raster) {
        for (const auto& l : paneLayers())
            if (l && l->type() == LayerType::Raster) { rep = l; break; }
    }
    if (!rep || rep->type() != LayerType::Raster) {
        m_highlight_overlay->clearHighlight();
        return;
    }
    auto* rl = static_cast<RasterLayer*>(rep.get());
    auto* ds = rl->dataset();
    if (!ds) { m_highlight_overlay->clearHighlight(); return; }

    // Snap the highlight to the WARPED DISPLAY CELL under the cursor (ESRI/ImageLinker style):
    // the visible pixel-blocks are cells of the warped VRT grid, so outlining that cell frames
    // exactly what the user sees — the source-pixel polygon (QGIS-style) floated because
    // FlashViewer warps to a fixed-resolution grid distinct from the source pixels. The warp
    // uses nearest-neighbour for categorical data (fvDefaultResampling) so displayed values are
    // unaltered, and the inspected VALUE is still read from the source pixel (FR-CRS-4). The
    // warped grid is north-up in the Project CRS, so the cell is an axis-aligned rectangle; with
    // no reprojection the view is sameAsSource (gt == source), degenerating to the source pixel.
    const std::string resamp =
        fvDefaultResampling(static_cast<GDALDataType>(ds->bandDataType(1)),
                            ds->bandHasColorTable(1));
    RasterDataset::WarpedView wv = ds->warpedView(m_project_wkt, resamp);
    if (wv.failed) { m_highlight_overlay->clearHighlight(); return; }

    // The click is already in the pane Project CRS → locate the warped cell directly.
    // geoToPixel is pixel-CENTRE based (integer == centre), so the containing cell index is
    // round(), not floor().
    auto px = wv.gt.geoToPixel(geo_x, geo_y);
    int col = static_cast<int>(std::round(px.x));
    int row = static_cast<int>(std::round(px.y));
    if (col < 0 || row < 0 || col >= wv.width || row >= wv.height) {
        m_highlight_overlay->clearHighlight();
        return;
    }
    // Cell corners are the raw affine EDGES (pixelToGeo would return cell centres, offsetting the
    // box half a cell). North-up warped grid ⇒ g[2]/g[4] are 0, but include them for generality.
    const double* g = wv.gt.gt;
    auto cornerGeo = [&](int P, int L) -> QPointF {
        return QPointF(g[0] + P * g[1] + L * g[2],
                       g[3] + P * g[4] + L * g[5]);
    };
    std::array<QPointF, 4> corners{ cornerGeo(col, row),          // TL
                                    cornerGeo(col + 1, row),      // TR
                                    cornerGeo(col + 1, row + 1),  // BR
                                    cornerGeo(col, row + 1) };    // BL
    m_highlight_overlay->setHighlight(corners, &m_camera);
}

void MapCanvas::mousePressEvent(QMouseEvent* event) {
    emit activated();  // clicking a pane makes it the active pane (Phase 6)

    // Middle button always pans regardless of current mode
    if (event->button() == Qt::MiddleButton) {
        m_mid_panning = true;
        m_last_mouse_pos = event->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (m_inspect_mode) {
        auto geo = m_camera.screenToGeo(event->position().x(), event->position().y());
        if (event->button() == Qt::LeftButton) {
            updateHighlightForGeo(geo.x, geo.y);
            emit pixelInspectRequest(geo.x, geo.y);
        } else if (event->button() == Qt::RightButton) {
            updateHighlightForGeo(geo.x, geo.y);
            emit pixelInspectAllRequest(geo.x, geo.y);
        }
        return;  // suppress panning and Qt context menu in inspect mode
    }

    if (event->button() == Qt::LeftButton) {
        if (event->modifiers() & Qt::ControlModifier) {
            auto geo = m_camera.screenToGeo(event->position().x(), event->position().y());
            updateHighlightForGeo(geo.x, geo.y);
            emit pixelInspectRequest(geo.x, geo.y);
        } else {
            m_panning = true;
            m_last_mouse_pos = event->position();
            setCursor(Qt::ClosedHandCursor);
        }
    }
    QOpenGLWidget::mousePressEvent(event);
}

void MapCanvas::mouseMoveEvent(QMouseEvent* event) {
    QPointF pos = event->position();

    if (m_panning || m_mid_panning) {
        QPointF delta = pos - m_last_mouse_pos;
        m_camera.pan(delta.x(), delta.y());
        m_last_mouse_pos = pos;
        update();
        emit cameraChanged(m_camera);
    }

    // Emit cursor geo position for status bar
    auto geo = m_camera.screenToGeo(pos.x(), pos.y());
    emit cursorGeoPos(geo.x, geo.y);

    // Emit image pixel coordinates for active layer (only if it's in this pane)
    auto active = activeLayerInPane();
    if (active && active->type() == LayerType::Raster) {
        auto* rl = static_cast<RasterLayer*>(active.get());
        auto* ds = rl->dataset();
        if (ds) {
            // Cursor geo is in the pane's Project CRS; map to the SOURCE raster pixel by
            // transforming into the layer's source CRS first (Phase 11, FR-CRS-4).
            double sx = geo.x, sy = geo.y;
            fvTransformPoint(m_project_wkt, ds->crsWkt(), sx, sy);
            const auto& gt = ds->geoTransform();
            auto px = gt.geoToPixel(sx, sy);
            int col = static_cast<int>(std::round(px.x));
            int row = static_cast<int>(std::round(px.y));
            bool ok = (col >= 0 && row >= 0 && col < ds->width() && row < ds->height());
            emit cursorPixelPos(ok ? col : -1, ok ? row : -1);
        } else {
            emit cursorPixelPos(-1, -1);
        }
    } else {
        emit cursorPixelPos(-1, -1);
    }

    QOpenGLWidget::mouseMoveEvent(event);
}

void MapCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)   m_panning     = false;
    if (event->button() == Qt::MiddleButton) m_mid_panning = false;

    if (!m_panning && !m_mid_panning)
        setCursor(m_inspect_mode ? Qt::CrossCursor : Qt::ArrowCursor);

    QOpenGLWidget::mouseReleaseEvent(event);
}

void MapCanvas::wheelEvent(QWheelEvent* event) {
    double factor = 1.0 + event->angleDelta().y() / 1200.0;
    factor = std::clamp(factor, 0.2, 5.0);
    QPointF pos = event->position();
    m_camera.zoom(factor, pos.x(), pos.y());
    emitZoomLevel();
    emit cameraChanged(m_camera);
    update();
    QOpenGLWidget::wheelEvent(event);
}

void MapCanvas::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
        fitToLayers();
        update();
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        m_camera.zoom(1.2, width()*0.5, height()*0.5);
        update();
        emitZoomLevel();               // keep the m/px readout live (FR-GIS-3)
        emit cameraChanged(m_camera);  // parity with the wheel → keyboard zoom syncs (Phase 7)
        break;
    case Qt::Key_Minus:
        m_camera.zoom(1.0/1.2, width()*0.5, height()*0.5);
        update();
        emitZoomLevel();
        emit cameraChanged(m_camera);
        break;
    default:
        break;
    }
    QOpenGLWidget::keyPressEvent(event);
}

// --------------------------------------------------------------------------
// Drag-and-drop: a layer dragged from the Layers panel onto this pane (Phase 6.3).

void MapCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(kLayerMime) ||
        event->mimeData()->hasFormat(kPaneMime))
        event->acceptProposedAction();
}

void MapCanvas::dropEvent(QDropEvent* event) {
    bool ok = false;
    if (event->mimeData()->hasFormat(kPaneMime)) {
        // A pane dragged by its ID label onto this canvas → re-home it into this
        // canvas's region (PaneLayout resolves the region; Phase 6.4).
        const qulonglong id = event->mimeData()->data(kPaneMime).toULongLong(&ok);
        if (!ok) return;
        event->acceptProposedAction();
        emit paneAssignDropped(static_cast<uint64_t>(id));
        return;
    }
    if (!event->mimeData()->hasFormat(kLayerMime)) return;
    const int layerIndex = event->mimeData()->data(kLayerMime).toInt(&ok);
    if (!ok) return;
    event->acceptProposedAction();
    emit layerDropped(layerIndex);   // MainWindow reassigns the layer to this pane's id
}
