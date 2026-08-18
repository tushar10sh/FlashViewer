#pragma once
#include <QMainWindow>
#include <QVector>
#include <QStringList>
#include <QByteArray>
#include <QPixmap>
#include "app/CoordAssignDialog.hpp"   // CoordAssignment (returned by the assign dialog)
#include "app/Settings.hpp"
#include "gis/InspectTypes.hpp"        // InspectPaneGroup — the shared inspect/profile scope
#include "panels/NoDataWidget.hpp"
#include "render/PaneLayoutMode.hpp"   // applyPaneLayoutMode / the View → Pane Layout radio
#include <QApplication>
#include <QEventLoop>
#include <QProgressDialog>
#include <QStatusBar>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <future>

class MapCanvas;
class PaneLayout;
class LayerManager;
class QPlainTextEdit;
class QLabel;
class QFrame;
class QTimer;
class QDockWidget;
class QMenu;
class QAction;
class GpuMonitorPanel;
class MarqueeLabel;
class LayerPanel;
class HistogramPanel;
class BandSelectorWidget;
class ColormapSelectorWidget;
class RasterInfoPanel;
class AttributeInspector;
class NumericDumpPanel;
class VectorLayerPanel;
class PlotWindow;
class RasterLayer;
class RasterDataset;
class SpectralPlotPanel;
class ScanPixProfilePanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFiles(const QStringList& paths);
    void openVectorFiles(const QStringList& paths, uint64_t targetPaneId = 0);
    void showSettingsDialog();

public slots:
    void onThemeChanged(Theme t);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    // Click on the status-bar CRS label → open the Project-CRS picker (Phase 11).
    bool eventFilter(QObject* watched, QEvent* event) override;

    // Shutdown disposal (any graceful exit path): delete every managed temp result file, after
    // releasing the layers' datasets (GDALClose) so the files are removable. Runs at most once.
    void disposeTempFiles();

private slots:
    void onActiveLayerChanged(int index);
    // `label` empty ⇒ the auto default ("Pane N", N one past the highest number in use).
    MapCanvas* addPane(const QString& label = QString());
    // View → New Pane / toolbar: ask for the new pane's name AND its position (a Windows-11
    // style snap picker) before creating it. Panes created implicitly (a layer dropped on an
    // empty region, a Tools dialog's "New Pane" output) skip the prompt and take the default.
    void       addPaneInteractive();
    void removeActivePane();
    void appendLog(int level, const QString& text);
    void exportLogs();                              // FR-APP-10 (Export Logs)
    void showErrorBanner(int level, const QString& text);  // FR-ERR-8 non-fatal banner
    void applyBannerStyle(int level);   // theme-compliant per-level bg/fg (contrast-safe)
    void captureScreenshot();
    QPixmap grabLayoutComposite();   // whole-layout screenshot: front pane per region (Phase 7)

private:
    void setupMenuBar();
    void setupToolBar();
    void setupDocks();
    void setupStatusBar();
    void restoreLayout();
    void refreshLayerProperties(RasterLayer* layer);
    MapCanvas* activeCanvas() const;
    // Phase 6: each pane's colormap legend is bound to that pane's OWN layer (the
    // active layer if it lives in the pane, else the pane's topmost raster, else
    // cleared) — independent of which pane is currently active.
    void       updatePaneLegends();
    int        topLayerIndexInPane(uint64_t paneId) const;
    // The layers one Scan/Pixel Profile Compute covers (Phase 26.5, FR-ANL-12), in priority
    // order: the Layers-panel selection when it holds 2+ visible rasters (any panes, sync
    // ignored); else every visible raster across the sync group when the active layer's pane
    // is synced; else the active layer alone. Same InspectPaneGroup shape as the inspect
    // selection, so both plots and the Pixel Inspector share one notion of a scope.
    FvProfileScope buildProfileScope() const;
    // Pane gear-menu actions (Phase 6.1 / 6.2).
    void       closePane(MapCanvas* canvas);
    void       renamePane(MapCanvas* canvas);
    void       colorPane(MapCanvas* canvas);
    // Phase 6.3: move a layer to a pane (drag-drop or "To Pane" menu) + refresh views.
    void       assignLayerToPane(int layerIndex, uint64_t paneId);
    // Phase 6.6: pixel-inspect scoped to the clicked pane — aggregates across its sync group
    // when synced. allLayers=false → each pane's representative/active layer; true → all
    // visible rasters per pane.
    void       inspectFromPane(MapCanvas* clicked, double geo_x, double geo_y, bool allLayers);
    // Phase 6: the pane that newly-opened layers are assigned to / that menu
    // camera actions act on. Updated when a canvas is clicked.
    uint64_t   activePaneId() const;
    // Authoritative Project CRS (WKT) for a pane, read from its canvas (Phase 11). Empty
    // ⇒ geographic. Replaces the ad-hoc bottom-raster scans in the Tools dialogs.
    QString    paneProjectCrs(uint64_t paneId) const;
    // Refresh the status-bar CRS readout from the active pane's Project CRS + reprojected
    // layer count (Phase 11, FR-GIS-4).
    void       updateProjectCrsStatus();
    // Open the Project-CRS picker for a pane (gear-menu / clickable status label, Phase 11).
    void       openProjectCrsPicker(MapCanvas* canvas);
    void       setOsmBasemapEnabled(bool on);
    // selectTopLayer: on a pane change, also make the pane's topmost layer active (for a
    // canvas click). Pass false when the activation is driven by a layer selection, so the
    // already-selected layer stays active (Phase 6.3 fix).
    // bringToFront: also raise the pane to the front of its STACKED region. Pass false for
    // layer-selection-driven activation — otherwise merely clicking a Pane-1 layer row would
    // swap Pane 1 in front of Pane 2 and destroy the drop target the user was aiming at
    // (Phase 18 #1). Explicit gestures (pill click, canvas click, double-click a layer row)
    // keep true.
    void       setActivePane(int idx, bool selectTopLayer = true, bool bringToFront = true);
    // Connect a pane's per-canvas signals (cursor/zoom/inspect/activation) so the
    // status bar and inspector follow whichever pane the user interacts with.
    void       wireCanvasSignals(MapCanvas* canvas);
    // Switch the pane layout and keep the View → Pane Layout radio in step. The ONLY way the
    // mode should change: the New-Pane snap picker changes it programmatically, and a stale
    // tick there would misreport the layout the user is looking at.
    void       applyPaneLayoutMode(PaneLayoutMode m);

    // View → Panels: re-open closed docks at their fresh-build location (FR-APP-9).
    // A dock tracked with its default area + tab partner so it can be restored exactly.
    struct DockEntry {
        QDockWidget*       dock{nullptr};
        Qt::DockWidgetArea area{Qt::LeftDockWidgetArea};
        QDockWidget*       tabWith{nullptr};   // re-tab partner, or nullptr
        QAction*           action{nullptr};    // its checkable menu item
    };
    void buildPanelsMenu();
    void reopenDockToDefault(const DockEntry& e);

    // Result of the "Select Variables" dialog (FR-IO-13, Phase 19): which subdatasets
    // the user picked, and whether they want one layer per variable (the default) or a
    // single multi-band layer stacking them all (which enables RGB display).
    struct SubdatasetChoice {
        QList<int> indices;
        bool       combine{false};
    };
    SubdatasetChoice showSubdatasetMultiPicker(
        const QString& path,
        const std::vector<std::pair<std::string,std::string>>& subs);

    // Load `indices` from `subs` as ONE multi-band layer via a `-separate` band-stack
    // VRT (FR-IO-13). Returns false when the variables sit on incompatible grids or the
    // VRT could not be built — the caller then falls back to separate layers.
    bool loadCombinedSubdatasets(
        const QString& path,
        const std::string& stdPath,
        const std::vector<std::pair<std::string,std::string>>& subs,
        const QList<int>& indices);

    // Shows the NetCDF coordinate assignment dialog when a subdataset opens with
    // identity geotransform. Returns nullopt when user clicks Skip.
    std::optional<CoordAssignment> showCoordAssignDialog(
        const QString& displayPath,
        const std::string& parentPath,
        const std::shared_ptr<RasterDataset>& ds,
        const std::vector<std::pair<std::string,std::string>>& subs);

    // Put an assignment into effect, BEFORE the RasterLayer is built (the layer's
    // constructor auto-stretches off whatever dataset it is handed). A 1-D assignment
    // overrides the geotransform/CRS in place; a 2-D geolocation assignment REPLACES `ds`
    // with the warped VRT and returns true, in which case the caller must follow up with
    // registerCoordAssignment() so the warp's temp files are reaped with the layer.
    // Returns false — after deleting the orphaned temps — when the warped VRT will not open.
    bool applyCoordAssignment(std::shared_ptr<RasterDataset>& ds,
                              const CoordAssignment& a);

    // Record a successful geolocation warp on the layer: its temp sidecars, and the
    // coordinate arrays that produced it (so a later variable switch re-applies the warp).
    void registerCoordAssignment(RasterLayer& layer, const CoordAssignment& a);

    PaneLayout*            m_pane_layout{nullptr};
    LayerManager*          m_layer_mgr{nullptr};  // single app-wide layer model (Phase 6)
    MapCanvas*             m_canvas{nullptr};  // the active pane's canvas
    QPlainTextEdit*        m_log_widget{nullptr};
    // Non-fatal error banner (FR-ERR-8): shown above the pane layout on a
    // warn/error report from ErrorReporter, auto-hidden after a few seconds.
    QFrame*                m_error_banner{nullptr};
    MarqueeLabel*          m_error_banner_label{nullptr};
    QTimer*                m_banner_timer{nullptr};
    int                    m_banner_level{0};   // last shown level, for live theme re-style
    // GPU Monitor dock (FR-APP-11): live estimated-VRAM sparkline + poll timer.
    GpuMonitorPanel*       m_gpu_monitor{nullptr};
    QTimer*                m_gpu_timer{nullptr};
    // Main-thread stall detector (NFR-PERF-2, Phase 12): a UI-loop watchdog + its clock.
    QTimer*                m_stall_timer{nullptr};
    QLabel*                m_coord_label{nullptr};
    QLabel*                m_zoom_label{nullptr};
    QLabel*                m_crs_label{nullptr};
    QLabel*                m_pixel_label{nullptr};

    LayerPanel*            m_layer_panel{nullptr};
    HistogramPanel*        m_histo_panel{nullptr};
    BandSelectorWidget*    m_band_sel{nullptr};
    ColormapSelectorWidget* m_cm_sel{nullptr};
    RasterInfoPanel*       m_info_panel{nullptr};
    NoDataWidget*          m_nodata_widget{nullptr};
    AttributeInspector*    m_attr_insp{nullptr};
    NumericDumpPanel*      m_numeric_dump{nullptr};
    VectorLayerPanel*      m_vector_panel{nullptr};

    // View → Display Resampling (FR-RND-10): radio group + the 3 mode actions,
    // kept so onActiveLayerChanged can reflect the active layer's current mode.
    QActionGroup*          m_resample_group{nullptr};
    QAction*               m_resample_acts[3]{nullptr, nullptr, nullptr};

    // View → Pane Layout (FR-PNE-8): the 4 checkable mode actions, indexed Full / HalfH /
    // HalfV / Quarter, kept so applyPaneLayoutMode can re-tick them after a programmatic
    // mode change.
    QAction*               m_layout_acts[4]{nullptr, nullptr, nullptr, nullptr};

    // View → Panels (FR-APP-9): tracked docks + pristine default layout snapshot.
    QVector<DockEntry>     m_docks;
    QByteArray             m_default_layout_state;
    QMenu*                 m_panels_menu{nullptr};

    struct LogEntry { int level; QString text; };
    QVector<LogEntry>      m_log_entries;
    void                   renderLogEntry(int level, const QString& text);

    int                    m_active_pane_idx{0};

    // Phase 18 #1: true while a layer drag started in the Layers panel is in flight. Any
    // front-switch request is ignored then, so nothing can re-stack a region and steal the
    // drop target out from under the cursor mid-drag.
    bool                   m_layer_drag_active{false};
    // Phase 18 #8: true while >1 layer is selected. The single-subject panels (Band,
    // Colormap, No-data, Histogram, Layer Info) have no meaningful subject then and stay
    // blank, exactly as with no image loaded.
    bool                   m_multi_select{false};

    // Managed temp result files captured in layerAboutToBeRemoved (layer still live) and
    // deleted in layerRemoved (after GDALClose). See util/TempFile.hpp / RasterLayer::ownsTempFile.
    QStringList            m_pending_temp_deletions;
    bool                   m_temp_disposed{false};   // disposeTempFiles() runs once

    // The two plots are top-level WINDOWS parented to this one (Phase 26.8), not docks: they
    // float above the main window without covering other applications, are closed on a fresh
    // profile, and are summoned by `S` / `P` or the Tools menu. Each remembers its own
    // geometry (FR-APP-6) — QMainWindow::saveState no longer carries them.
    SpectralPlotPanel*     m_spectral_panel{nullptr};
    ScanPixProfilePanel*   m_profile_panel{nullptr};
    /// Show, raise and focus a plot window, restoring its saved geometry the first time.
    void showPlotWindow(QWidget* w, const QString& key);
    // Closing one is handled in two places: the panel's own closeEvent() discards its plots
    // (so the rule holds however the window is closed), and the eventFilter() here saves the
    // frame and the divider, which only MainWindow knows the Settings key for.

    // Modal background loading with cancel dialog (blocks background UI)
    template <typename Fn>
    auto runWithCancelDialog(const QString& title, const QString& labelText, Fn&& fn)
        -> decltype(fn(std::declval<std::atomic<bool>&>()));
};

template <typename Fn>
auto MainWindow::runWithCancelDialog(const QString& title, const QString& labelText, Fn&& fn)
    -> decltype(fn(std::declval<std::atomic<bool>&>()))
{
    using ResultType = decltype(fn(std::declval<std::atomic<bool>&>()));
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    QProgressDialog progress(labelText, tr("Cancel"), 0, 0, this);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    progress.show();
    progress.raise();
    progress.activateWindow();
    QApplication::processEvents();

    auto future = std::async(std::launch::async, [fn = std::forward<Fn>(fn), cancelFlag]() mutable -> ResultType {
        return fn(*cancelFlag);
    });

    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (progress.wasCanceled()) {
            cancelFlag->store(true, std::memory_order_relaxed);
            break;
        }
    }

    if (progress.wasCanceled()) {
        progress.close();
        if (statusBar()) statusBar()->showMessage(tr("Loading cancelled by user."), 4000);
        if (future.valid()) {
            future.wait_for(std::chrono::milliseconds(200));
        }
        return ResultType{};
    }

    progress.close();
    return future.get();
}
