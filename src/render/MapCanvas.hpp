#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_1_Core>
#include <QElapsedTimer>
#include <QColor>
#include <QPixmap>
#include <memory>
#include "render/GlslProgram.hpp"
#include "render/Camera.hpp"
#include "render/TileRenderer.hpp"
#include "render/PaneChrome.hpp"   // PaneSyncEntry (Sync With resolver, Phase 6.5)
#include "core/LayerManager.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class OsmTileRenderer;
class VectorRenderer;
class ColormapLegend;
class PixelHighlightOverlay;
class ScaleBar;
class QTimer;

// FR-RND-7: progressive refresh stops once all visible tiles are ready OR a
// bounded timeout elapses. kRepaintBudgetMs is that bound (≤ 30 s per the SRS).
// Exposed as a free predicate so the boundary is unit-testable without a widget.
static constexpr qint64 kRepaintBudgetMs = 30000;
inline bool fvRepaintWithinBudget(qint64 elapsed_ms) {
    return elapsed_ms <= kRepaintBudgetMs;
}

class MapCanvas : public QOpenGLWidget, protected QOpenGLFunctions_4_1_Core {
    Q_OBJECT
public:
    // The LayerManager is shared across all panes (owned by MainWindow). Each canvas
    // renders only the layers whose paneId() == this canvas's paneId (Phase 6).
    explicit MapCanvas(LayerManager* layers, uint64_t paneId, QWidget* parent = nullptr);
    ~MapCanvas() override;

    void setDarkBackground(bool dark);
    LayerManager* layerManager() { return m_layers; }
    uint64_t      paneId() const { return m_pane_id; }
    void          setPaneId(uint64_t id) { m_pane_id = id; }
    // Pane chrome (Phase 6.1): the top-left ID label + gear menu, and the active-pane
    // highlight border drawn around this canvas.
    void          setPaneLabel(const QString& label);
    void          setPaneColor(const QColor& c);   // border + ID label colour (Phase 6.2.1)
    void          setActive(bool active);
    // Sync (Phase 6.5): the master/slave badge (drawn in the group master's colour), the
    // "Sync With" submenu resolver, and a "ghost cursor" marker mirrored from a synced sibling.
    void          setSyncInfo(const FvPaneSyncInfo& info);
    void          setSyncInfoResolver(std::function<std::vector<PaneSyncEntry>()> r);
    void          setGhostCursor(double geo_x, double geo_y, bool active);
    bool          isActivePane() const { return m_active; }
    const Camera& camera() const { return m_camera; }

    // Invalidate cached tiles for a layer (schedules re-decode on next render)
    void invalidateLayer(uint64_t layer_id);
    // Schedules a background redecode of a layer's RESIDENT tiles without
    // deleting them first -- see TileCache::markLayerDirty()'s doc comment.
    // Unlike invalidateLayer(), needs no GL context (no textures touched
    // directly), so callable from a plain timer without makeCurrent().
    void markLayerDirty(uint64_t layer_id);

    // Camera access for sync
    void setCamera(const Camera& cam);
    // Apply a camera broadcast from a synced sibling whose Project CRS is src_wkt,
    // reprojecting it into this pane's Project CRS first (Phase 11, FR-CRS-2). A no-op
    // copy when the CRS match. Applies an echo guard so feedback loops don't recurse.
    void setCameraFromSync(const Camera& cam, const std::string& src_wkt);

    // Metres-per-pixel for this pane's current camera (geographic scales converted via the
    // documented L-1 approximation) — drives the status-bar readout (FR-GIS-3, Phase 7).
    double metersPerPixel() const;
    // Emit zoomLevelChanged with the current metres-per-pixel — call wherever the camera scale
    // changes (or to refresh the readout for the active pane) so the status readout stays live.
    void emitZoomLevel();

    // Grab the canvas for a screenshot, omitting the pane chrome (ID label + gear) and the
    // pane border, while keeping the map furniture (legend, scale bar, pixel highlight).
    QPixmap grabForExport();

    // Per-pane scale-bar show/hide (gear menu, Phase 7 follow-up). Hidden ⇒ also absent from
    // screenshots (it's a child widget, so grab() won't render it).
    void setScaleBarVisible(bool on);
    bool scaleBarVisible() const { return m_scalebar_visible; }

    // Per-pane colorbar (colormap legend) show/hide (gear menu, Phase 16 #5). This is the
    // master gate; when off the legend never shows regardless of the topmost visible layer.
    void setColorbarVisible(bool on);
    bool colorbarVisible() const { return m_colorbar_visible; }

    // Fit camera to show all loaded layers
    void fitToLayers();

    // OSM basemap control
    OsmTileRenderer* osmRenderer() { return m_osm_renderer.get(); }
    void clearOsmCache();
    // Reset to a geographic world view (project CRS → geographic) so the basemap is
    // visible when no layer pins the camera to a projected CRS.
    void resetToWorldView();

    // --- Per-pane Project CRS (Phase 11, FR-CRS-1..5) ---------------------------
    // This pane's Project CRS (WKT; empty = geographic/identity). Every raster layer in
    // the pane is rendered reprojected into it (FR-CRS-1); the basemap follows it too.
    std::string projectCrsWkt() const { return m_project_wkt; }
    std::shared_ptr<class CrsTransformer> wgs84Transformer() const;
    // THE single choke point for changing the Project CRS. Validates the WKT, reprojects
    // the camera so the view stays put (FR-CRS-2), bumps the tile-cache epoch and purges
    // stale tiles, updates the basemap, and emits projectCrsChanged(). userInitiated=true
    // marks an explicit user override that then wins over layer-derived defaults.
    void setProjectCrsWkt(const std::string& wkt, bool userInitiated);
    // True once the user has explicitly overridden the CRS (blocks auto-derivation).
    bool projectCrsUserSet() const { return m_project_crs_user_set; }
    uint32_t crsEpoch() const { return m_crs_epoch; }
    // Clear a user override and revert to the layer-derived Project CRS default (FR-CRS-3).
    void clearProjectCrsOverride();
    // Re-derive the Project CRS from the bottom-most raster layer (empty ⇒ geographic)
    // and apply it via setProjectCrsWkt(..., userInitiated=false). No-op once the user
    // has explicitly overridden the CRS. Call whenever the layer set changes (Phase 11,
    // FR-CRS-3 default = first layer's CRS; Phase 17 #4 = empty pane adopts a dropped layer).
    void refreshDerivedProjectCrs();

    // Stored Project CRS before OSM Basemap activation for reversibility
    const std::string& preOsmCrs() const { return m_pre_osm_crs; }
    void setPreOsmCrs(const std::string& wkt) { m_pre_osm_crs = wkt; m_has_pre_osm_crs = true; }
    bool hasPreOsmCrs() const { return m_has_pre_osm_crs; }
    void clearPreOsmCrs() { m_has_pre_osm_crs = false; m_pre_osm_crs.clear(); }

    // Colormap legend overlay
    ColormapLegend* colormapLegend() { return m_cm_legend; }

    // Canvas interaction mode
    enum class ToolMode {
        Navigate = 0,
        Inspect,
        MeasureDistance,
        MeasureArea,
        Snr,
        Mtf
    };

    enum class DisplayFilterMode {
        None = 0,
        Checkerboard,
        VerticalSwipe,
        HorizontalSwipe
    };

    ToolMode toolMode() const { return m_tool_mode; }
    void setToolMode(ToolMode mode);
    void clearMeasurement();
    const std::vector<QPointF>& measurementPoints() const { return m_measure_points; }
    bool isMeasurementFinished() const { return m_measure_finished; }

    DisplayFilterMode displayFilterMode() const { return m_filter_mode; }
    void setDisplayFilterMode(DisplayFilterMode mode);

    int displayFilterCheckSize() const { return m_filter_check_size; }
    void setDisplayFilterCheckSize(int sz);

    float displayFilterSwipeX() const { return m_filter_swipe_x; }
    void setDisplayFilterSwipeX(float x);

    float displayFilterSwipeY() const { return m_filter_swipe_y; }
    void setDisplayFilterSwipeY(float y);

    int snrMtfWindowSize() const { return m_snr_mtf_window_size; }
    void setSnrMtfWindowSize(int sz);
    void clearSnrMtfRegion();

    std::shared_ptr<Layer> activeLayerInPane() const;

    // Inspect mode: left-click → inspect active layer, right-click → inspect all
    bool inspectMode() const { return m_tool_mode == ToolMode::Inspect; }
    void setInspectMode(bool on) { setToolMode(on ? ToolMode::Inspect : ToolMode::Navigate); }
    // Draw the red inspect-highlight square at a geographic point, snapped to this pane's
    // representative raster (public so MainWindow can mirror it onto synced sibling panes,
    // Phase 6.8.3). Out-of-bounds points clear this pane's highlight.
    //
    // (geo_x, geo_y) are read in THIS pane's Project CRS. A caller mirroring a click from
    // another pane must transform first (fvTransformPoint) — synced panes may hold different
    // Project CRS since Phase 11 (FR-CRS-2/4).
    void updateHighlightForGeo(double geo_x, double geo_y);
    /// Drop this pane's inspect highlight — used when a mirrored point has no image in this
    /// pane's CRS, where drawing anything at all would be drawing it in the wrong place.
    void clearInspectHighlight();

    struct GlInfo { QString renderer, vendor, version; };
    const GlInfo& glInfo() const { return m_gl_info; }

    // GPU Monitor (FR-APP-11): estimated resident VRAM + tile count for this pane's
    // tile cache. Thread-safe (TileCache locks internally).
    std::size_t gpuResidentBytes() const;
    int         gpuResidentTiles() const;

    // Performance HUD (FR-APP-14, Phase 12): a compact on-canvas overlay of live frame
    // stats (FPS / p99 / max), last open-latency, stall count, and this pane's VRAM.
    // App-wide toggle (View → Performance HUD), applied to every pane and persisted.
    void setPerfHudVisible(bool on);
    bool perfHudVisible() const { return m_perf_hud_visible; }

public slots:
    void onThemeChanged();

signals:
    void activated();                        // emitted on mouse press → MainWindow sets active pane
    // Pane gear-menu intents (Phase 6.1) — MainWindow performs the action on this pane.
    void paneCloseRequested();
    void paneRenameRequested();
    void paneColorRequested();
    void paneSyncRequested();
    void paneSyncToggleRequested(uint64_t otherPaneId);   // a "Sync With" entry toggled (Phase 6.5)
    void paneUnsyncRequested();                            // "Unsync" chosen
    void cursorGeoPos(double lon, double lat);
    void cursorPixelPos(int col, int row);   // -1,-1 = outside image / no active layer
    void zoomLevelChanged(double scale);
    void cameraChanged(const Camera& camera);
    void pixelInspectRequest(double geo_x, double geo_y);     // inspect active layer
    void pixelInspectAllRequest(double geo_x, double geo_y);  // inspect all layers
    void snrRequested(RasterLayer* layer, int col, int row, double geo_x, double geo_y);
    void mtfRequested(RasterLayer* layer, int col, int row, double geo_x, double geo_y);
    void canvasResized(int w, int h);
    void inspectModeChanged(bool on);
    void toolModeChanged(ToolMode mode);
    void displayFilterChanged(DisplayFilterMode mode);
    void measurementUpdated(double distanceMeters, double areaM2, const QString& summary);
    void layerDropped(int layerIndex);   // a layer was dragged from the panel onto this pane
    void paneAssignDropped(uint64_t paneId);   // a pane was dragged (by ID label) onto this pane (Phase 6.4)
    void projectCrsChanged(const QString& wkt);   // this pane's Project CRS changed (Phase 11)
    void paneCrsRequested();   // gear-menu "Project CRS…" → MainWindow opens the picker (Phase 11)
    void colorbarVisibilityChanged();   // per-pane "Show Colorbar" toggled → MainWindow re-runs updatePaneLegends (Phase 16 #5)
    // On-the-fly reprojection notice (Phase 11, FR-CRS-6): message for the non-modal banner;
    // failed=true → a layer could not be reprojected and was omitted (FR-CRS-5, error styling).
    void reprojectionNotice(const QString& message, bool failed);

protected:
    void initializeGL()            override;
    void resizeGL(int w, int h)    override;
    void paintGL()                 override;
    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event)       override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event)        override;
    void mouseReleaseEvent(QMouseEvent* event)     override;
    void wheelEvent(QWheelEvent* event)            override;
    void keyPressEvent(QKeyEvent* event)           override;
    void dragEnterEvent(QDragEnterEvent* event) override;   // accept layer drags (Phase 6.3)
    void dropEvent(QDropEvent* event)           override;

private:
    // Phase 1 triangle (shown when no layers are loaded)
    std::unique_ptr<GlslProgram> m_tri_shader;
    GLuint m_tri_vao{0};
    GLuint m_tri_vbo{0};

    // Phase 2+ rendering. m_layers is the shared, app-wide manager (NOT owned here);
    // m_pane_id selects which of its layers this canvas draws (Phase 6).
    LayerManager*                    m_layers{nullptr};
    uint64_t                         m_pane_id{kDefaultPaneId};
    Camera                           m_camera;

    // Shared tile cache across all layers in this pane
    std::unique_ptr<TileCache> m_tile_cache;

    // Sub-renderers
    std::unique_ptr<TileRenderer>    m_tile_renderer;
    std::unique_ptr<VectorRenderer>  m_vector_renderer;
    std::unique_ptr<OsmTileRenderer> m_osm_renderer;
    ColormapLegend*                  m_cm_legend{nullptr};

    // UI overlays
    PixelHighlightOverlay*  m_highlight_overlay{nullptr};
    ScaleBar*               m_scale_bar{nullptr};
    PaneChrome*             m_chrome{nullptr};
    bool                    m_active{false};   // active-pane highlight
    QColor                  m_pane_color;      // pane colour for border + chrome
    // Ghost cursor (Phase 6.5): a sibling synced pane's cursor mirrored here at a geo position.
    bool                    m_ghost_active{false};
    double                  m_ghost_x{0.0};
    double                  m_ghost_y{0.0};
    // Synchronous pixel highlight state (drawn inside paintGL without widget lag)
    bool                    m_highlight_active{false};
    std::array<QPointF, 4>  m_highlight_corners{};
    void                    drawPixelHighlight();

    // Measurement tool state
    ToolMode                m_tool_mode{ToolMode::Navigate};
    std::vector<QPointF>    m_measure_points;       // points in Project CRS
    QPointF                 m_measure_cursor_geo;   // cursor pos in Project CRS
    bool                    m_measure_has_cursor{false};
    bool                    m_measure_finished{false};

    // Repaint timer: fires while tiles are still loading (FR-RND-7).
    // m_repaint_clock measures elapsed time since the current load burst began;
    // refresh stops once tiles are ready or kRepaintBudgetMs elapses.
    QTimer*       m_repaint_timer{nullptr};
    QElapsedTimer m_repaint_clock;
    bool          m_repaint_timed_out{false};

    // FPS / pan jitter detection
    std::chrono::steady_clock::time_point m_last_paint_time;
    std::chrono::steady_clock::time_point m_last_pan_time;

    // The canonical world view: the FULL longitude range centred on the prime meridian, so
    // the Americas sit left of centre and Asia/India to the right, plus Mercator's usable
    // latitude band. One definition, shared by the startup view, Fit with no layers, and
    // resetToWorldView().
    static Extent worldExtent() { return Extent{-180.0, -85.0, 180.0, 85.0}; }
    void fitCameraToWorld();

    bool    m_initial_view_done{false};   // world view applied on first sizing
    bool    m_gl_ready{false};  // true only after initializeGL() fully succeeds
    bool    m_dark_bg{true};
    int     m_sync_role{0};     // 0=None, 1=Master, 2=Slave (mirrors PaneLayout::syncRoleAt)
    bool    m_capturing{false}; // true only during grabForExport() → paintGL skips the border
    bool    m_scalebar_visible{true};   // per-pane scale-bar show/hide (Phase 7 follow-up)
    bool    m_colorbar_visible{true};   // per-pane colorbar show/hide (Phase 16 #5)
    bool    m_perf_hud_visible{false};  // Performance HUD overlay (FR-APP-14, Phase 12)
    GlInfo  m_gl_info;
    QPointF m_last_mouse_pos;
    bool    m_panning{false};
    bool    m_mid_panning{false};

    void initTriangle();
    void renderTriangle();
    void repositionOverlays();
    void drawMeasurementOverlay();
    void drawSnrMtfOverlay();
    void drawDisplayFilterOverlay();
    // Draw the Performance HUD overlay (FR-APP-14) — called at the end of paintGL,
    // after the frame time is recorded, so the HUD's own paint cost isn't counted.
    void drawPerfHud();

    // Display filter & SNR/MTF state
    DisplayFilterMode m_filter_mode{DisplayFilterMode::None};
    int               m_filter_check_size{64};
    float             m_filter_swipe_x{0.5f};
    float             m_filter_swipe_y{0.5f};
    bool              m_dragging_v_swipe{false};
    bool              m_dragging_h_swipe{false};

    int               m_snr_mtf_window_size{5};
    bool              m_snr_mtf_has_region{false};
    int               m_snr_mtf_col{0};
    int               m_snr_mtf_row{0};
    double            m_snr_mtf_geo_x{0.0};
    double            m_snr_mtf_geo_y{0.0};
    bool              m_snr_mtf_has_hover{false};
    double            m_snr_mtf_hover_geo_x{0.0};
    double            m_snr_mtf_hover_geo_y{0.0};
    // GL context-loss recovery (FR-ERR-7): free GL resources while the dying
    // context is still current so the next initializeGL() rebuilds them.
    void handleContextLoss();
    // Layers shown in THIS pane (filtered from the shared manager by paneId).
    std::vector<std::shared_ptr<Layer>> paneLayers() const;
    int  paneLayerCount() const;
    // Whether this pane's representative raster (active-in-pane, else the first raster the pane
    // shows) is in a geographic CRS — drives the scale bar's distance units (Phase 6.4.5).
    bool paneIsGeographic() const;
    // Drop this pane's cached tiles for all its layers (used on Project-CRS change so
    // tiles warped into the old CRS are freed promptly — FR-CRS-2).
    void purgePaneTiles();
    // Raise the user-facing notice when a layer is displayed reprojected on the fly
    // (requirement #2) or cannot be reprojected and is instead shown in its native CRS
    // (warning, Phase 17 #4 / FR-CRS-5). Fired once per (layer, CRS-epoch) from the
    // TileRenderer status callback, deferred out of paintGL. `nativeFallback` selects the
    // warning vs the informational notice.
    void showReprojectionNotice(uint64_t layer_id, bool nativeFallback);
    void drawToolModeBadge();

    // --- Per-pane Project CRS state (Phase 11) ---------------------------------
    std::string m_project_wkt;                 // "" = geographic/identity
    bool        m_project_crs_user_set{false}; // user override freezes auto-derivation
    bool        m_project_is_geographic{true}; // cached: is m_project_wkt geographic? (scale units)
    uint32_t    m_crs_epoch{0};                // bumped on every CRS change (TileKey dim)
    // (layer_id, epoch) pairs already announced to the user, so the on-the-fly
    // reprojection notice fires once per layer per CRS change, not per frame (Phase 11 §7).
    std::set<std::pair<uint64_t, uint32_t>> m_reproject_announced;

    std::string m_pre_osm_crs;
    bool        m_has_pre_osm_crs{false};
};
