#include "app/MainWindow.hpp"
#include "app/Application.hpp"
#include "app/GpuInfoDialog.hpp"
#include "app/AboutLicensesDialog.hpp"
#include "app/CoordAssignDialog.hpp"
#include "app/NewPaneDialog.hpp"
#include "app/Settings.hpp"
#include "plots/SpectralPlotPanel.hpp"
#include "plots/ScanPixProfilePanel.hpp"
#include "plots/PlotWindowChrome.hpp"   // fvApplyPlotWindowFlags — what kind of window a plot is
#include "render/MapCanvas.hpp"
#include "render/PaneLayout.hpp"
#include "core/Layer.hpp"             // kDefaultPaneId
#include "core/RasterLayer.hpp"
#include "core/LayerManager.hpp"
#include "io/DatasetFactory.hpp"
#include "io/PyramidBuilder.hpp"
#include "io/BinaryImportDialog.hpp"
#include "io/BinaryRasterParser.hpp"
#include "io/BandStackVrt.hpp"
#include "io/CloudReader.hpp"
#include "io/UrlGuard.hpp"
#include <QProgressDialog>
#include <QMessageBox>
#include <QFontDatabase>
#include "util/Logger.hpp"
#include "util/ErrorReporter.hpp"
#include "util/TempFile.hpp"
#include "util/PerfMetrics.hpp"
#include <QElapsedTimer>
#include "panels/GpuMonitorPanel.hpp"
#include "panels/MarqueeLabel.hpp"
#include "widgets/UiKit.hpp"          // fvMakeSection
#include "panels/LayerPanel.hpp"
#include "panels/HistogramPanel.hpp"
#include "panels/BandSelectorWidget.hpp"
#include "panels/ColormapSelectorWidget.hpp"
#include "panels/RasterInfoPanel.hpp"
#include "panels/NoDataWidget.hpp"
#include "panels/NumericDumpPanel.hpp"
#include "panels/VectorLayerPanel.hpp"
#include "core/VectorLayer.hpp"
#include "io/VectorDataset.hpp"
#include "app/SettingsDialog.hpp"
#include "gis/AttributeInspector.hpp"
#include "gis/CrsUtil.hpp"
#include "gis/CrsPickerDialog.hpp"
#include "math/RasterMathDialog.hpp"
#include "gis/GdalOpsDialog.hpp"
#include "render/ColormapLegend.hpp"
#include "render/OsmTileRenderer.hpp"

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QScrollArea>
#include <QToolBar>
#include <QToolButton>
#include <QStatusBar>
#include <QDockWidget>
#include <QPlainTextEdit>
#include <QLabel>
#include <QEvent>
#include <QCloseEvent>
#include <QScrollBar>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPixmap>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QColorDialog>
#include <QUrl>
#include <QInputDialog>
#include <QPainter>
#include <QLineEdit>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QDateTime>
#include <spdlog/spdlog.h>
#include <algorithm>

static const char* kLevelColorsDark[] = {
    "#848d97",  // TRACE    — fg-muted
    "#58a6ff",  // DEBUG    — accent-fg
    "#e6edf3",  // INFO     — fg-default
    "#d29922",  // WARN     — attention-fg
    "#f85149",  // ERROR    — danger-fg
    "#ff7b72",  // CRITICAL
    "#e6edf3",  // OFF
};
static const char* kLevelColorsLight[] = {
    "#6e7781",  // TRACE    — fg-subtle   (4.6:1 on #fff)
    "#0969da",  // DEBUG    — accent-fg   (4.6:1)
    "#1f2328",  // INFO     — fg-default  (16:1)
    "#9a6700",  // WARN     — attention-fg(4.7:1)
    "#cf222e",  // ERROR    — danger-fg   (5.5:1)
    "#a40e26",  // CRITICAL              (6.8:1)
    "#1f2328",  // OFF
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("FlashViewer");
    setMinimumSize(900, 600);
    resize(1280, 800);
    setAcceptDrops(true);

    m_layer_mgr = new LayerManager(this);            // single app-wide layer model
    m_pane_layout = new PaneLayout(m_layer_mgr, this);
    m_canvas = m_pane_layout->addPane(false);        // primary pane (Pane 1, active)

    // Central area: a non-fatal error banner (FR-ERR-8), hidden until ErrorReporter
    // reports a warning/error, above the pane layout.
    auto* central = new QWidget(this);
    auto* centralLay = new QVBoxLayout(central);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);
    m_error_banner = new QFrame(central);
    m_error_banner->setObjectName("ErrorBanner");
    m_error_banner->setVisible(false);
    {
        auto* bl = new QHBoxLayout(m_error_banner);
        bl->setContentsMargins(8, 3, 8, 3);   // even, slim — a sleek bar hugging its text (FR-CRS-6)
        bl->setSpacing(6);
        m_error_banner_label = new MarqueeLabel(m_error_banner);   // single-line, marquee on overflow
        auto* closeBtn = new QToolButton(m_error_banner);
        closeBtn->setText(QStringLiteral("✕"));
        closeBtn->setAutoRaise(true);
        closeBtn->setToolTip(tr("Dismiss"));
        closeBtn->setFocusPolicy(Qt::NoFocus);
        closeBtn->setFixedSize(18, 18);        // compact — don't inflate the bar height
        connect(closeBtn, &QToolButton::clicked, m_error_banner, &QWidget::hide);
        bl->addWidget(m_error_banner_label, 1);
        bl->addWidget(closeBtn, 0);
    }
    centralLay->addWidget(m_error_banner);
    centralLay->addWidget(m_pane_layout, 1);
    setCentralWidget(central);

    m_banner_timer = new QTimer(this);
    m_banner_timer->setSingleShot(true);
    connect(m_banner_timer, &QTimer::timeout, this, [this] {
        if (m_error_banner) m_error_banner->hide();
    });
    // Route every ErrorReporter report (GDAL errors, caught exceptions) to the
    // banner; queued so worker-thread reports land on the UI thread safely.
    connect(&ErrorReporter::instance(), &ErrorReporter::reported,
            this, &MainWindow::showErrorBanner);

    // A pane selected via its region pill / dragged into a region becomes active (6.4).
    connect(m_pane_layout, &PaneLayout::paneActivationRequested, this, [this](uint64_t pid) {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == pid) { setActivePane(i); break; }
    });

    // Sync roles changed (Phase 6.5): refresh each pane's sync badge (★/mirror, drawn in the
    // group master's colour) and clear any stale ghost-cursor markers. The Layers panel shows
    // the same badge on its pane headers, so it is rebuilt here too (Phase 20).
    connect(m_pane_layout, &PaneLayout::syncRolesChanged, this, [this] {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (auto* c = m_pane_layout->paneCanvas(i)) {
                c->setSyncInfo(m_pane_layout->syncInfoAt(i));
                c->setGhostCursor(0, 0, false);
                c->update();
            }
        if (m_layer_panel) m_layer_panel->refreshPanes();
        // A merged spectral plot is a statement about panes that move together; once they no
        // longer do, it is discarded (Phase 26). Recomputing the group each time — rather than
        // clearing on any role change — keeps merges alive across a master rename/recolour,
        // which emits this same signal.
        if (m_spectral_panel || m_profile_panel) {
            QSet<quint64> synced;
            for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
                const uint64_t id = m_pane_layout->paneId(i);
                if (m_pane_layout->paneSynced(id)) synced.insert(id);
            }
            if (m_spectral_panel) m_spectral_panel->dropMergedPlotsOutside(synced);
            // The profile merges across a sync group too since Phase 26.5, so its merges are
            // invalidated by the same event.
            if (m_profile_panel) m_profile_panel->dropMergedPlotsOutside(synced);
        }
    });

    // A layer dropped onto a region: create a pane there if the region is empty, then assign
    // the layer to that pane (Phase 6.4.1, Issue 4). A populated region routes to its front pane.
    connect(m_pane_layout, &PaneLayout::layerDroppedOnRegion, this,
            [this](int layerIndex, int region) {
        uint64_t target = 0;
        if (m_pane_layout->regionIsEmpty(region)) {
            addPane();                                       // full wiring (colour/label/theme/OSM)
            const int newIdx = m_pane_layout->paneCount() - 1;
            const uint64_t newPid = m_pane_layout->paneId(newIdx);
            m_pane_layout->movePaneToRegion(newPid, region); // place the new pane in the dropped region
            target = newPid;
        } else {
            target = m_pane_layout->frontPaneIdInRegion(region);
        }
        if (target) assignLayerToPane(layerIndex, target);
    });

    // A layer dropped onto a region PILL names its target pane explicitly, so it works even
    // when that pane is stacked BEHIND the displayed one — the drop path that makes issue #1
    // resolvable in Full-Window mode. The target is then brought forward so the move is
    // visible (this is an explicit user gesture, unlike a layer-row selection).
    connect(m_pane_layout, &PaneLayout::layerDroppedOnPane, this,
            [this](int layerIndex, uint64_t paneId) {
        assignLayerToPane(layerIndex, paneId);
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == paneId) {
                setActivePane(i, /*selectTopLayer=*/false, /*bringToFront=*/true);
                break;
            }
    });

    // Allow docking to the upper/lower half (or full) of a side area, not just
    // tab/replace, so a dragged panel snaps into split positions (FR-APP-10).
    setDockNestingEnabled(true);

    setupMenuBar();
    setupToolBar();
    setupDocks();
    setupStatusBar();
    restoreLayout();

    auto* app = qobject_cast<Application*>(qApp);
    if (app) {
        connect(app, &Application::themeChanged, this, &MainWindow::onThemeChanged);
        m_canvas->setDarkBackground(app->currentTheme() == Theme::Dark);
    }

    QObject* relay = Logger::instance().relay();
    if (relay) {
        connect(relay, SIGNAL(messageLogged(int, const QString&)),
                this,  SLOT(appendLog(int, const QString&)),
                Qt::QueuedConnection);
    }

    // Per-canvas signals (status bar, inspect, pane activation) for the primary pane.
    // wireCanvasSignals is called again for every pane added later (Phase 6).
    wireCanvasSignals(m_canvas);
    m_canvas->setPaneLabel(m_pane_layout->paneLabel(0));
    // The default startup pane is coloured with the theme accent blue (Phase 6.2.1);
    // its layers are colour-coded like any other pane.
    {
        const QColor accent = qApp->palette().highlight().color();
        m_pane_layout->setPaneColor(0, accent);
        m_canvas->setPaneColor(accent);
        if (m_layer_panel) m_layer_panel->refreshPaneColors();
    }
    m_canvas->setActive(true);   // primary pane starts active (highlight border)

    // Apply persisted OSM tile URL (may differ from the compiled-in default)
    m_canvas->osmRenderer()->provider()->setUrlTemplate(
        Settings::instance().osmTileUrl());

    // Resource Monitor poll: sum every pane's estimated resident VRAM, and sample CPU/RAM/GPU.
    m_gpu_timer = new QTimer(this);
    m_gpu_timer->setInterval(250);
    connect(m_gpu_timer, &QTimer::timeout, this, [this] {
        if (!m_gpu_monitor || !m_pane_layout) return;
        std::size_t bytes = 0; int tiles = 0;
        const int panes = m_pane_layout->paneCount();
        for (int i = 0; i < panes; ++i)
            if (auto* c = m_pane_layout->paneCanvas(i)) {
                bytes += c->gpuResidentBytes();
                tiles += c->gpuResidentTiles();
            }
        m_gpu_monitor->addSample(bytes, tiles, panes);
        const auto& gi = m_canvas->glInfo();
        m_gpu_monitor->setGpuIdentity(gi.renderer, gi.vendor, gi.version);
    });
    m_gpu_timer->start();

    // Main-thread stall detector (NFR-PERF-2, Phase 12): a fixed-interval watchdog on
    // the UI event loop. When the gap between ticks exceeds the interval by more than
    // the 50 ms stall threshold, the loop was blocked → record a stall (feeds the HUD;
    // the verbose log line is gated behind FV_PERF_INSTRUMENT).
    constexpr int kStallCheckIntervalMs = 250;
    m_stall_timer = new QTimer(this);
    m_stall_timer->setInterval(kStallCheckIntervalMs);
    connect(m_stall_timer, &QTimer::timeout, this, [this] {
        static QElapsedTimer clock;
        if (!clock.isValid()) { clock.start(); return; }
        const double actual = double(clock.restart());
        if (fvIsStall(kStallCheckIntervalMs, actual)) {
            const double over = actual - kStallCheckIntervalMs;
            PerfMetrics::instance().recordStall("ui", over);
#ifdef FV_PERF_INSTRUMENT
            FV_WARN("perf: UI-thread stall {:.0f} ms (NFR-PERF-2)", over);
#endif
        }
    });
    m_stall_timer->start();

    // Apply the persisted Performance HUD state (FR-APP-14) to the primary pane.
    if (Settings::instance().perfHudVisible() && m_canvas)
        m_canvas->setPerfHudVisible(true);

    FV_INFO("MainWindow ready");
}

MainWindow::~MainWindow() = default;

// --------------------------------------------------------------------------

uint64_t MainWindow::preparePaneForRasterLayer(const std::shared_ptr<RasterDataset>& ds, const QString& layerName) {
    if (!ds) return activePaneId();

    uint64_t targetPaneId = activePaneId();
    std::vector<std::shared_ptr<VectorLayer>> existingVectors;
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && fvLayerInPane(*l, targetPaneId) && l->type() == LayerType::Vector) {
            existingVectors.push_back(std::static_pointer_cast<VectorLayer>(l));
        }
    }

    if (!existingVectors.empty()) {
        const std::string rasterCrs = ds->crsWkt();
        if (!rasterCrs.empty()) {
            // Reproject existing vector layers to raster dataset CRS
            for (auto& vl : existingVectors) {
                if (!vl->dataset()) continue;
                if (vl->dataset()->canReprojectTo(rasterCrs)) {
                    QProgressDialog progress(
                        tr("Reprojecting vector layer '%1' to raster CRS (%2)…")
                            .arg(vl->name()).arg(fvCrsShortName(rasterCrs)),
                        tr("Cancel"), 0, 100, this);
                    progress.setWindowModality(Qt::ApplicationModal);
                    progress.setMinimumDuration(150);
                    progress.setValue(0);
                    progress.show();
                    progress.raise();
                    progress.activateWindow();

                    std::function<bool(int, int)> progressCb = [&](int cur, int tot) -> bool {
                        progress.setValue(cur * 100 / std::max(1, tot));
                        QApplication::processEvents();
                        return !progress.wasCanceled();
                    };
                    vl->dataset()->geometriesForCrs(rasterCrs, progressCb);
                }
            }
            if (auto* c = activeCanvas()) {
                c->setProjectCrsWkt(rasterCrs, false);
            }
            return targetPaneId;
        } else {
            auto* newCanvas = addPane();
            uint64_t newPid = newCanvas ? newCanvas->paneId() : targetPaneId;
            if (newCanvas) {
                int newPaneIdx = m_pane_layout->indexOfCanvas(newCanvas);
                newCanvas->clearProjectCrsOverride();
                newCanvas->setProjectCrsWkt("", false);
                showErrorBanner(1, tr("Raster '%1' has no CRS: loaded into new unsynchronized %2.")
                    .arg(layerName).arg(m_pane_layout->paneLabel(newPaneIdx)));
            }
            return newPid;
        }
    }

    return targetPaneId;
}

void MainWindow::openFiles(const QStringList& paths) {
    for (const auto& path : paths) {
        const std::string stdPath = path.toStdString();
        const QString fname = QFileInfo(path).fileName();
        bool needsBinary = false;

        // For NetCDF/HDF5 files: ALWAYS enumerate subdatasets first and show
        // the picker even if GDAL could open the file directly. This prevents
        // GDAL from silently choosing the first/wrong variable.
        if (DatasetFactory::isMultiVariableFormat(stdPath)) {
            auto subs = runWithCancelDialog(
                tr("Scanning Dataset"),
                tr("Reading subdatasets in '%1'…").arg(fname),
                [&](std::atomic<bool>&) {
                    return DatasetFactory::listSubdatasets(stdPath);
                });

            if (!subs.empty()) {
                SubdatasetChoice choice = showSubdatasetMultiPicker(path, subs);
                const QList<int>& selected = choice.indices;
                if (selected.isEmpty()) continue;

                // "Combine into one multi-band layer" (FR-IO-13): stack the selected
                // variables into a single N-band layer. On an incompatible grid or a
                // VRT-build failure this reports why and falls through to the
                // one-layer-per-variable path below, so the load never dead-ends.
                if (choice.combine && selected.size() > 1 &&
                    loadCombinedSubdatasets(path, stdPath, subs, selected)) {
                    statusBar()->showMessage(
                        tr("Loaded %1 variable(s) from %2 as one multi-band layer")
                            .arg(selected.size())
                            .arg(fname), 4000);
                    continue;
                }

                for (int idx : selected) {
                    const std::string& subPath = subs[static_cast<size_t>(idx)].first;
                    const QString varName = DatasetFactory::extractVarName(subPath);

                    auto sub_ds = runWithCancelDialog(
                        tr("Loading Variable"),
                        tr("Opening variable '%1'…").arg(varName),
                        [&](std::atomic<bool>&) {
                            return DatasetFactory::openSubdataset(subPath);
                        });

                    if (!sub_ds) continue;
                    // Missing grid OR missing CRS: offer coordinate assignment (FR-IO-9).
                    // This runs BEFORE the layer is built because a 2-D geolocation
                    // assignment swaps in a warped dataset, and RasterLayer's constructor
                    // auto-stretches off whatever dataset it is given.
                    std::optional<CoordAssignment> coords;
                    bool geoloc_applied = false;
                    if (sub_ds->isGeoTransformIdentity() || sub_ds->crsWkt().empty()) {
                        coords = showCoordAssignDialog(path, stdPath, sub_ds, subs);
                        if (coords) geoloc_applied = applyCoordAssignment(sub_ds, *coords);
                    }
                    auto layer = std::make_shared<RasterLayer>(sub_ds);
                    layer->initSubdatasetMeta(stdPath, subs, idx);
                    layer->setName(varName);
                    if (geoloc_applied) registerCoordAssignment(*layer, *coords);
                    layer->setPaneId(preparePaneForRasterLayer(sub_ds, varName));
                    PerfMetrics::instance().markOpenStart(layer->layerId());  // NFR-PERF-3/4
                    m_layer_mgr->addLayer(layer);
                    FV_INFO("Loaded subdataset '{}' from '{}'", subPath, stdPath);
                }
                statusBar()->showMessage(
                    tr("Loaded %1 variable(s) from %2")
                        .arg(selected.size())
                        .arg(fname), 4000);
                continue;
            }
            // No subdatasets listed — fall through to direct open (single-variable file)
        }

        if (PyramidBuilder::shouldPromptPyramids(stdPath)) {
            auto [rw, rh] = PyramidBuilder::getRasterDimensions(stdPath);
            auto res = QMessageBox::question(
                this,
                tr("Generate Image Pyramids?"),
                tr("The raster \"%1\" (%2 × %3) does not have overview pyramids.\n\n"
                   "Generating image pyramids allows instant zooming, smooth panning, and minimal memory usage.\n\n"
                   "Would you like to build binary pyramids next to the file?")
                    .arg(fname).arg(rw).arg(rh),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);

            // Remember that user answered or dismissed the prompt for this path
            PyramidBuilder::dismissPrompt(stdPath);

            if (res == QMessageBox::Yes) {
                QProgressDialog progress(
                    tr("Generating image pyramids for %1...").arg(fname),
                    tr("Cancel"), 0, 100, this);
                progress.setWindowTitle(tr("Building Pyramids"));
                progress.setWindowModality(Qt::ApplicationModal);
                progress.setMinimumDuration(0);
                progress.setValue(0);
                progress.show();
                progress.raise();
                progress.activateWindow();
                QApplication::processEvents();

                bool ok = PyramidBuilder::buildPyramids(
                    stdPath, "AVERAGE",
                    [&progress](double fraction) {
                        progress.setValue(static_cast<int>(fraction * 100));
                        QApplication::processEvents();
                        return !progress.wasCanceled();
                    });

                progress.setValue(100);
                if (ok) {
                    statusBar()->showMessage(
                        tr("Generated pyramids for %1").arg(fname), 4000);
                } else if (progress.wasCanceled()) {
                    statusBar()->showMessage(
                        tr("Pyramid generation cancelled for %1").arg(fname), 4000);
                }
            }
        }

        auto ds = runWithCancelDialog(
            tr("Loading Dataset"),
            tr("Opening dataset '%1'…").arg(fname),
            [&](std::atomic<bool>&) {
                return DatasetFactory::open(stdPath, &needsBinary);
            });

        if (needsBinary) {
            BinaryImportDialog dlg(path, this);
            if (dlg.exec() != QDialog::Accepted) continue;
            auto spec = dlg.spec();
            std::string vrt = createVrtForBinary(spec);
            if (vrt.empty()) {
                FV_WARN("Binary import failed: could not create VRT for '{}'", stdPath);
                statusBar()->showMessage(
                    tr("Import failed: could not build VRT for %1").arg(fname), 6000);
                continue;
            }
            ds = runWithCancelDialog(
                tr("Loading Binary VRT"),
                tr("Parsing binary VRT for '%1'…").arg(fname),
                [&](std::atomic<bool>&) {
                    return DatasetFactory::open(vrt);
                });
        }

        if (!ds) {
            FV_WARN("Open failed: could not open '{}'", stdPath);
            statusBar()->showMessage(tr("Open failed: %1").arg(fname), 6000);
            continue;
        }

        // Single-variable formats get the same escape hatch as the NetCDF/HDF5 path
        // (FR-IO-9): anything that opens WITHOUT a grid or WITHOUT a CRS — a headless
        // binary raster just imported through BinaryImportDialog, a GeoTIFF written with
        // no projection — is offered coordinate/CRS assignment before it becomes a layer.
        // There are no subdatasets to choose from here, so the arrays come from another
        // file via the dialog's Browse buttons.
        std::optional<CoordAssignment> coords;
        bool geoloc_applied = false;
        if (ds->isGeoTransformIdentity() || ds->crsWkt().empty()) {
            static const std::vector<std::pair<std::string,std::string>> kNoSubs;
            coords = showCoordAssignDialog(path, stdPath, ds, kNoSubs);
            if (coords) geoloc_applied = applyCoordAssignment(ds, *coords);
        }

        auto layer = std::make_shared<RasterLayer>(ds);
        if (geoloc_applied) registerCoordAssignment(*layer, *coords);
        layer->setPaneId(preparePaneForRasterLayer(ds, fname));
        PerfMetrics::instance().markOpenStart(layer->layerId());   // NFR-PERF-3/4 probe
        m_layer_mgr->addLayer(layer);
        FV_INFO("Loaded '{}'", stdPath);
        statusBar()->showMessage(tr("Loaded: %1").arg(fname), 4000);
    }
}

MainWindow::SubdatasetChoice MainWindow::showSubdatasetMultiPicker(
    const QString& path,
    const std::vector<std::pair<std::string,std::string>>& subs)
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Select Variables"));
    dlg.setMinimumWidth(400);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(
        tr("File: %1\nCtrl+click to select multiple variables:")
            .arg(QFileInfo(path).fileName()), &dlg));
    auto* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto& [name, desc] : subs)
        list->addItem(QString::fromStdString(desc.empty() ? name : desc));
    if (list->count() > 0) list->setCurrentRow(0);
    lay->addWidget(list);

    // Load mode (FR-IO-13). "Separate" is the historical behaviour and stays the
    // default; "Combine" stacks the picked variables into one N-band layer so they can
    // be driven as an RGB composite from the Band Selector.
    // A section frame, not a QGroupBox: the group box hangs its title in the top margin,
    // so "Load as" overlapped the frame border. fvMakeSection puts the heading inside.
    QVBoxLayout* modeLay = nullptr;
    auto* modeBox   = fvMakeSection(tr("Load as"), modeLay, &dlg);
    auto* rbSep     = new QRadioButton(tr("Separate layers (one per variable)"), modeBox);
    auto* rbCombine = new QRadioButton(
        tr("One multi-band layer (enables RGB display)"), modeBox);
    rbSep->setChecked(true);
    rbCombine->setToolTip(
        tr("Stacks the selected variables as bands of a single layer.\n"
           "Requires them to share raster size, grid, and CRS; otherwise\n"
           "they are loaded as separate layers."));
    modeLay->addWidget(rbSep);
    modeLay->addWidget(rbCombine);
    lay->addWidget(modeBox);

    // Combining a single variable is a no-op — keep the option off until 2+ are picked.
    auto syncCombineEnabled = [&] {
        const bool multi = list->selectedItems().size() > 1;
        rbCombine->setEnabled(multi);
        if (!multi) rbSep->setChecked(true);
    };
    connect(list, &QListWidget::itemSelectionChanged, &dlg, syncCombineEnabled);
    syncCombineEnabled();

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(btns);

    if (dlg.exec() != QDialog::Accepted) return {};
    SubdatasetChoice choice;
    for (auto* item : list->selectedItems())
        choice.indices << list->row(item);
    std::sort(choice.indices.begin(), choice.indices.end());   // stack in list order
    choice.combine = rbCombine->isChecked();
    return choice;
}

bool MainWindow::loadCombinedSubdatasets(
    const QString& path,
    const std::string& stdPath,
    const std::vector<std::pair<std::string,std::string>>& subs,
    const QList<int>& indices)
{
    std::vector<BandStackSource> sources;
    std::vector<std::string>     paths;
    sources.reserve(static_cast<size_t>(indices.size()));
    paths.reserve(static_cast<size_t>(indices.size()));
    for (int idx : indices) {
        const std::string& gdal_path = subs[static_cast<size_t>(idx)].first;
        paths.push_back(gdal_path);
        sources.push_back({gdal_path,
                           DatasetFactory::extractVarName(gdal_path).toStdString()});
    }

    // Grid guard: mismatched variables would stack onto a union extent with
    // misregistered bands, so report and let the caller load them separately.
    const BandStackCompat compat = fvCheckBandStackCompatible(paths);
    if (!compat.ok) {
        ErrorReporter::instance().report(3, tr("Open"),
            tr("Cannot combine variables: %1. Loading them as separate layers instead.")
                .arg(QString::fromStdString(compat.reason)));
        return false;
    }

    // The stack VRT is a managed temp file: tagged ownsTempFile so the existing
    // layer-removal reaper deletes it when the layer goes away (FR-OPS-8).
    const QString vrt_path = QDir(QDir::tempPath()).filePath(
        QStringLiteral("fv_stack_%1_%2.vrt")
            .arg(QFileInfo(path).completeBaseName())
            .arg(QDateTime::currentMSecsSinceEpoch()));
    if (fvBuildBandStackVrt(sources, vrt_path.toStdString()).empty()) {
        ErrorReporter::instance().report(3, tr("Open"),
            tr("Could not build a combined multi-band layer for %1. "
               "Loading the variables as separate layers instead.")
                .arg(QFileInfo(path).fileName()));
        return false;
    }

    auto ds = DatasetFactory::open(vrt_path.toStdString());
    if (!ds) {
        fvRemoveTempFile(vrt_path);
        ErrorReporter::instance().report(3, tr("Open"),
            tr("Could not open the combined multi-band layer for %1. "
               "Loading the variables as separate layers instead.")
                .arg(QFileInfo(path).fileName()));
        return false;
    }

    // Same identity-geotransform escape hatch as the per-variable path: a non-CF file
    // stacks into a VRT that is equally unreferenced, so offer coordinate assignment
    // once for the whole stack (all bands share one grid by construction).
    std::optional<CoordAssignment> coords;
    bool geoloc_applied = false;
    if (ds->isGeoTransformIdentity() || ds->crsWkt().empty()) {
        coords = showCoordAssignDialog(path, stdPath, ds, subs);
        if (coords) geoloc_applied = applyCoordAssignment(ds, *coords);
    }

    auto layer = std::make_shared<RasterLayer>(ds);
    layer->setOwnsTempFile(true);
    if (geoloc_applied) {
        registerCoordAssignment(*layer, *coords);
        // The layer's source file is now the warped VRT, so the stack VRT it was warped
        // FROM is no longer reachable through ownsTempFile — carry it as a sidecar.
        layer->addTempSidecar(vrt_path.toStdString());
    }
    // Name it after the source file + variable count, not the temp VRT (the full temp
    // path is still visible in the Layer Info panel's File row).
    layer->setName(tr("%1 (%2 variables)")
                       .arg(QFileInfo(path).fileName())
                       .arg(indices.size()));
    layer->setPaneId(preparePaneForRasterLayer(ds, layer->name()));
    PerfMetrics::instance().markOpenStart(layer->layerId());   // NFR-PERF-3/4
    m_layer_mgr->addLayer(layer);
    FV_INFO("Loaded {} variable(s) from '{}' as one {}-band layer",
            indices.size(), stdPath, ds->bandCount());
    return true;
}

std::optional<CoordAssignment> MainWindow::showCoordAssignDialog(
    const QString& displayPath,
    const std::string& parentPath,
    const std::shared_ptr<RasterDataset>& ds,
    const std::vector<std::pair<std::string,std::string>>& subs)
{
    QString varName = QFileInfo(displayPath).fileName();
    CoordAssignDialog dlg(varName, parentPath, subs, ds, this);
    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    return dlg.assignment();
}

bool MainWindow::applyCoordAssignment(std::shared_ptr<RasterDataset>& ds,
                                      const CoordAssignment& a)
{
    if (a.use_geoloc) {
        if (auto warped = DatasetFactory::open(a.warped_path)) {
            ds = warped;
            FV_INFO("Georeferenced via 2-D geolocation arrays → '{}'", a.warped_path);
            return true;
        }
        // The warp reported success but its VRT will not open: drop the orphaned temps and
        // fall through to the affine override, which at least places the layer sensibly.
        for (const auto& f : a.temp_files) fvRemoveTempFile(QString::fromStdString(f));
        ErrorReporter::instance().report(3, tr("Open"),
            tr("Could not open the projected result of the geolocation arrays; "
               "the layer was placed using an approximate geotransform instead."));
    }
    // set_gt false ⇒ the raster's own grid is valid and only the CRS was assigned.
    if (a.set_gt) ds->setGeoTransformOverride(a.gt);
    if (!a.crs_wkt.empty()) ds->setCrsOverride(a.crs_wkt);
    return false;
}

void MainWindow::registerCoordAssignment(RasterLayer& layer,
                                         const CoordAssignment& a)
{
    layer.setOwnsTempFile(true);
    for (const auto& f : a.temp_files) layer.addTempSidecar(f);
    layer.setGeolocSource({a.x_path, a.y_path, a.crs_wkt});
}


void MainWindow::setupMenuBar() {
    // ---- File ----
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    auto* actOpen = fileMenu->addAction(tr("&Open…"));
    actOpen->setShortcut(QKeySequence::Open);
    connect(actOpen, &QAction::triggered, this, [this] {
        QString filter = tr("All Supported Datasets (*.tif *.tiff *.nc *.h5 *.hdf5 *.png *.jpg *.jpeg *.webp *.vrt *.bin *.shp *.SHP);;Raster Images (*.tif *.tiff *.nc *.h5 *.hdf5 *.png *.jpg *.jpeg *.webp *.vrt *.bin);;ESRI Shapefiles (*.shp *.SHP);;All Files (*.*)");
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open Dataset (Raster or Vector)"), QString(), filter);
        if (files.isEmpty()) return;
        QStringList rasterFiles, vectorFiles;
        for (const auto& f : files) {
            if (f.endsWith(".shp", Qt::CaseInsensitive)) {
                vectorFiles << f;
            } else {
                rasterFiles << f;
            }
        }
        if (!rasterFiles.isEmpty())
            ErrorReporter::runGuarded("Open", [&] { openFiles(rasterFiles); });
        if (!vectorFiles.isEmpty())
            ErrorReporter::runGuarded("Open Vector", [&] { openVectorFiles(vectorFiles); });
    });

    auto* actOpenRaster = fileMenu->addAction(tr("Open &Raster Dataset…"));
    connect(actOpenRaster, &QAction::triggered, this, [this] {
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open Raster"),
            QString(),
            QString::fromUtf8(DatasetFactory::openFilter()));
        if (!files.isEmpty())
            ErrorReporter::runGuarded("Open", [&] { openFiles(files); });
    });

    auto* actOpenVector = fileMenu->addAction(tr("Open &Vector Layer (SHP)…"));
    actOpenVector->setShortcut(QKeySequence("Ctrl+Shift+V"));
    connect(actOpenVector, &QAction::triggered, this, [this] {
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open Vector Layer (ESRI Shapefile)"),
            QString(),
            tr("ESRI Shapefiles (*.shp *.SHP);;All Files (*.*)"));
        if (!files.isEmpty())
            openVectorFiles(files);
    });

    auto* actOpenUrl = fileMenu->addAction(tr("Open &URL (COG)…"));
    actOpenUrl->setShortcut(QKeySequence("Ctrl+U"));
    connect(actOpenUrl, &QAction::triggered, this, [this] {
        bool ok;
        QString url = QInputDialog::getText(this, tr("Open Cloud/COG URL"),
            tr("Enter URL (http/https/s3/gs):"), QLineEdit::Normal, {}, &ok);
        if (!ok || url.isEmpty()) return;
        // Early SSRF/scheme rejection with immediate modal feedback (FR-SEC-1/2/3);
        // CloudReader re-checks as defense-in-depth on the open path.
        UrlGuard::Result guard = UrlGuard::check(url.toStdString());
        if (!guard.ok) {
            FV_WARN("Open URL rejected: {}", guard.reason.toStdString());
            QMessageBox::warning(this, tr("URL Rejected"), guard.reason);
            return;
        }
        ErrorReporter::runGuarded("Open URL", [&] { openFiles({url}); });
    });

    fileMenu->addSeparator();

    auto* actTileSource = fileMenu->addAction(tr("OSM &Tile Source…"));
    connect(actTileSource, &QAction::triggered, this, [this] {
        OsmTileProvider* prov = m_canvas->osmRenderer()->provider();
        bool ok;
        QString url = QInputDialog::getText(this,
            tr("OSM Tile Source"),
            tr("Enter tile URL template ({z}, {x}, {y} placeholders):\n"
               "Example: https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
            QLineEdit::Normal,
            prov->urlTemplate(),
            &ok);
        if (ok && !url.isEmpty()) {
            prov->setUrlTemplate(url);
            Settings::instance().setOsmTileUrl(url);
            m_canvas->clearOsmCache();
            m_canvas->update();
        }
    });

    fileMenu->addSeparator();

    auto* actExit = fileMenu->addAction(tr("E&xit"));
    actExit->setShortcut(QKeySequence::Quit);
    // close() (not qApp->quit()) so closeEvent fires and FR-APP-6 state is
    // persisted on Ctrl+Q; quitOnLastWindowClosed (default) then exits the app.
    connect(actExit, &QAction::triggered, this, [this]{ close(); });

    // ---- View ----
    auto* viewMenu = menuBar()->addMenu(tr("&View"));

    auto* themeMenu  = viewMenu->addMenu(tr("&Theme"));
    auto* themeGroup = new QActionGroup(this);

    auto* actLight = themeMenu->addAction(tr("&Light"));
    actLight->setCheckable(true);
    themeGroup->addAction(actLight);

    auto* actDark = themeMenu->addAction(tr("&Dark"));
    actDark->setCheckable(true);
    themeGroup->addAction(actDark);

    auto* app = qobject_cast<Application*>(qApp);
    if (app) {
        bool isDark = (app->currentTheme() == Theme::Dark);
        actDark->setChecked(isDark);
        actLight->setChecked(!isDark);

        connect(actLight, &QAction::triggered, this, [app]{ app->applyTheme(Theme::Light); });
        connect(actDark,  &QAction::triggered, this, [app]{ app->applyTheme(Theme::Dark);  });

        connect(app, &Application::themeChanged, this, [actLight, actDark](Theme t) {
            actDark->setChecked(t == Theme::Dark);
            actLight->setChecked(t == Theme::Light);
        });
    }

    viewMenu->addSeparator();
    auto* actFit = viewMenu->addAction(tr("&Fit to Layers"));
    actFit->setShortcut(Qt::Key_Space);
    connect(actFit, &QAction::triggered, this,
            [this]{ if (auto* c = activeCanvas()) c->fitToLayers(); });

    auto* actFitActive = viewMenu->addAction(tr("Fit to &Active Layer"));
    actFitActive->setShortcut(Qt::Key_F);
    connect(actFitActive, &QAction::triggered, this, [this] {
        auto active = m_layer_mgr->activeLayer();
        if (!active || active->type() != LayerType::Raster) {
            m_canvas->fitToLayers();
            return;
        }
        auto ext = static_cast<RasterLayer*>(active.get())->extent();
        if (!ext.isValid()) { m_canvas->fitToLayers(); return; }
        Camera cam = m_canvas->camera();
        cam.fitToExtent(ext);
        m_canvas->setCamera(cam);
        m_canvas->update();
    });

    viewMenu->addSeparator();
    m_act_osm = viewMenu->addAction(tr("&OSM Basemap"));
    m_act_osm->setCheckable(true);
    m_act_osm->setChecked(m_canvas->osmRenderer()->isEnabled());
    connect(m_act_osm, &QAction::toggled, this, &MainWindow::setOsmBasemapEnabled);

    // ---- Performance HUD (FR-APP-14, Phase 12) ----
    // App-wide overlay of live frame stats / open-latency / stalls / VRAM, mirroring
    // the OSM-basemap toggle: applied to every pane and persisted.
    auto* actPerfHud = viewMenu->addAction(tr("&Performance HUD"));
    actPerfHud->setCheckable(true);
    actPerfHud->setChecked(Settings::instance().perfHudVisible());
    connect(actPerfHud, &QAction::toggled, this, [this](bool on) {
        Settings::instance().setPerfHudVisible(on);
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (auto* c = m_pane_layout->paneCanvas(i)) c->setPerfHudVisible(on);
    });

    // ---- Display resampling (FR-RND-10) ----
    auto* resampleMenu = viewMenu->addMenu(tr("Display &Resampling"));
    m_resample_group = new QActionGroup(this);
    const QString resampleNames[3] = {
        tr("&Bilinear"),
        tr("Bicubic — &smooth (B-spline)"),
        tr("Bicubic — s&harp (Catmull-Rom)"),
    };
    const int defaultResample = Settings::instance().displayResampling();
    for (int i = 0; i < 3; ++i) {
        m_resample_acts[i] = resampleMenu->addAction(resampleNames[i]);
        m_resample_acts[i]->setCheckable(true);
        m_resample_acts[i]->setChecked(i == defaultResample);
        m_resample_group->addAction(m_resample_acts[i]);
        connect(m_resample_acts[i], &QAction::triggered, this, [this, i] {
            Settings::instance().setDisplayResampling(i);   // persisted default
            auto active = m_layer_mgr->activeLayer();
            if (active && active->type() == LayerType::Raster) {
                static_cast<RasterLayer*>(active.get())->setDisplayResampling(
                    static_cast<RasterLayer::DisplayResampling>(i));
                m_canvas->update();
            }
        });
    }

    // View → Panels (FR-APP-9): populated in buildPanelsMenu() once the docks exist.
    viewMenu->addSeparator();
    m_panels_menu = viewMenu->addMenu(tr("&Panels"));

    viewMenu->addSeparator();
    // Phase 6 (point 13): "New Pane" (no auto-sync; sync is opt-in via the pane gear
    // menu). Pane removal moved to the per-pane gear menu, so no "Remove Pane" here.
    auto* actAddPane = viewMenu->addAction(tr("&New Pane"));
    actAddPane->setShortcut(QKeySequence("Ctrl+Shift+N"));
    connect(actAddPane, &QAction::triggered, this, &MainWindow::addPaneInteractive);

    // ---- Pane Layout (FR-PNE-8): Full / Half-H / Half-V / Quarter, radio-exclusive ----
    auto* layoutMenu = viewMenu->addMenu(tr("Pane &Layout"));
    auto* layoutGroup = new QActionGroup(this);
    struct LayoutItem { const char* text; PaneLayoutMode mode; };
    const LayoutItem layoutItems[] = {
        { QT_TR_NOOP("&Full"),                PaneLayoutMode::Full    },
        { QT_TR_NOOP("Half - &Side by Side"), PaneLayoutMode::HalfH   },
        { QT_TR_NOOP("Half - &Top/Bottom"),   PaneLayoutMode::HalfV   },
        { QT_TR_NOOP("&Quarter (2x2)"),       PaneLayoutMode::Quarter },
    };
    // The action order here IS the m_layout_acts index order applyPaneLayoutMode uses.
    for (int i = 0; i < 4; ++i) {
        const auto& it = layoutItems[i];
        auto* act = layoutMenu->addAction(tr(it.text));
        act->setCheckable(true);
        act->setChecked(it.mode == m_pane_layout->mode());
        layoutGroup->addAction(act);
        m_layout_acts[i] = act;
        const PaneLayoutMode mode = it.mode;
        connect(act, &QAction::triggered, this, [this, mode] { applyPaneLayoutMode(mode); });
    }

    // ---- Tools ----
    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    auto* actRasterMath = toolsMenu->addAction(tr("&Raster Math…"));
    actRasterMath->setShortcut(QKeySequence("Ctrl+M"));
    connect(actRasterMath, &QAction::triggered, this, [this] {
        const uint64_t activePane = activePaneId();
        // Each pane's Project CRS is now authoritative per-pane state (Phase 11) — read it
        // from the canvas (honours user overrides), empty ⇒ geographic. Feeds the Output CRS
        // list (distinct CRS) and Output pane list; Output CRS defaults to the active pane's.
        QVector<RasterMathDialog::PaneCrsInfo> panes;
        for (int p = 0; p < m_pane_layout->paneCount(); ++p) {
            const uint64_t pid = m_pane_layout->paneId(p);
            panes.push_back({ pid, m_pane_layout->paneLabel(p), paneProjectCrs(pid) });
        }
        // "New Pane" output option: create a pane on demand and return its id.
        auto createPane = [this]() -> uint64_t {
            MapCanvas* c = addPane();
            return m_pane_layout->paneId(m_pane_layout->indexOfCanvas(c));
        };
        RasterMathDialog dlg(m_layer_mgr, activePane, panes, createPane, this);
        dlg.exec();
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (auto* c = m_pane_layout->paneCanvas(i)) c->update();
    });

    auto* actGdalOps = toolsMenu->addAction(tr("&GDAL Operations (Merge/Warp)…"));
    connect(actGdalOps, &QAction::triggered, this, [this] {
        const uint64_t activePane = activePaneId();
        // Same pane enumeration as Raster Math: feeds each tab's "Output pane" dropdown so the
        // result lands in a chosen pane (FR-OPS-4). Project CRS read from the canvas (Phase 11).
        QVector<GdalOpsDialog::PaneInfo> panes;
        for (int p = 0; p < m_pane_layout->paneCount(); ++p) {
            const uint64_t pid = m_pane_layout->paneId(p);
            panes.push_back({ pid, m_pane_layout->paneLabel(p), paneProjectCrs(pid) });
        }
        auto createPane = [this]() -> uint64_t {
            MapCanvas* c = addPane();
            return m_pane_layout->paneId(m_pane_layout->indexOfCanvas(c));
        };
        GdalOpsDialog dlg(m_layer_mgr, activePane, panes, createPane, this);
        dlg.exec();
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (auto* c = m_pane_layout->paneCanvas(i)) c->update();
    });

    // The Spectral Plot is a DOCK now (Phase 26), not a Tools window — but it keeps its Tools
    // entry and its `S` shortcut as the way to summon it. Deliberately NOT the View → Panels
    // Show + raise + focus, wherever the user last left the window. ApplicationShortcut so `S`
    // works while the plot itself — a separate top-level window — holds focus. The window is
    // built in setupDocks(), which runs later, hence the null check inside showPlotWindow.
    auto* actSpectral = toolsMenu->addAction(tr("&Spectral Plot (S)"));
    actSpectral->setShortcut(Qt::Key_S);
    actSpectral->setShortcutContext(Qt::ApplicationShortcut);
    connect(actSpectral, &QAction::triggered, this,
            [this] { showPlotWindow(m_spectral_panel, QStringLiteral("spectral")); });

    // Same terms as the Spectral Plot above. Opening the window does NOT compute (Phase 26.9):
    // a profile is a raster read the user asks for with the Compute button, and running it on
    // every open both cost that read unbidden and made forget-on-close look broken — the window
    // was closed, its plots were genuinely discarded, and then the open handler immediately
    // rebuilt the same profile of the same active layer. It shows what is STORED for the active
    // layer instead, which after a close is nothing: a blank chart named for that layer.
    auto* actProfile = toolsMenu->addAction(tr("Scan/Pixel &Profile (P)"));
    actProfile->setShortcut(Qt::Key_P);
    actProfile->setShortcutContext(Qt::ApplicationShortcut);
    connect(actProfile, &QAction::triggered, this, [this] {
        showPlotWindow(m_profile_panel, QStringLiteral("profile"));
        if (m_profile_panel && m_layer_mgr)
            m_profile_panel->showLayerPlot(m_layer_mgr->activeIndex());
    });

    toolsMenu->addSeparator();

    auto* actToolsSettings = toolsMenu->addAction(tr("&Preferences / Settings…"));
    actToolsSettings->setMenuRole(QAction::NoRole);
    actToolsSettings->setShortcut(QKeySequence("Ctrl+,"));
    connect(actToolsSettings, &QAction::triggered, this, &MainWindow::showSettingsDialog);

    // ---- Settings ----
    auto* settingsMenu = menuBar()->addMenu(tr("&Settings"));
    auto* actPrefs = settingsMenu->addAction(tr("&Preferences / Settings…"));
    actPrefs->setMenuRole(QAction::NoRole);
    actPrefs->setShortcut(QKeySequence("Ctrl+,"));
    connect(actPrefs, &QAction::triggered, this, &MainWindow::showSettingsDialog);

    settingsMenu->addSeparator();

    auto* settsThemeMenu = settingsMenu->addMenu(tr("&Theme"));
    auto* actSettsLight = settsThemeMenu->addAction(tr("&Light"));
    auto* actSettsDark  = settsThemeMenu->addAction(tr("&Dark"));
    if (auto* app = qobject_cast<Application*>(qApp)) {
        actSettsLight->setCheckable(true);
        actSettsDark->setCheckable(true);
        actSettsDark->setChecked(app->currentTheme() == Theme::Dark);
        actSettsLight->setChecked(app->currentTheme() == Theme::Light);
        connect(actSettsLight, &QAction::triggered, this, [app]{ app->applyTheme(Theme::Light); });
        connect(actSettsDark,  &QAction::triggered, this, [app]{ app->applyTheme(Theme::Dark);  });
        connect(app, &Application::themeChanged, this, [actSettsLight, actSettsDark](Theme t) {
            actSettsDark->setChecked(t == Theme::Dark);
            actSettsLight->setChecked(t == Theme::Light);
        });
    }

    // ---- Help ----
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* actGpu = helpMenu->addAction(tr("&GPU Information…"));
    connect(actGpu, &QAction::triggered, this, [this] {
        GpuInfoDialog dlg({m_canvas->glInfo().renderer,
                           m_canvas->glInfo().vendor,
                           m_canvas->glInfo().version}, this);
        dlg.exec();
    });
    helpMenu->addSeparator();
    auto* actAbout = helpMenu->addAction(tr("&About FlashViewer"));
    connect(actAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About FlashViewer"),
            tr("<b>FlashViewer</b> v%1<br/>"
               "Ultra-fast Satellite Image Rendering Application<br/><br/>"
               "OpenGL 4.1 Core · Qt %2 · GDAL")
            .arg(QApplication::applicationVersion())
            .arg(QString::fromUtf8(qVersion())));
    });

    auto* actLicenses = helpMenu->addAction(tr("&Licenses…"));
    connect(actLicenses, &QAction::triggered, this, [this] {
        AboutLicensesDialog dlg(this);
        dlg.exec();
    });
}

void MainWindow::setupToolBar() {
    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName("MainToolBar");
    toolbar->setMovable(false);

    auto* actOpen = new QAction(tr("Open"), this);
    actOpen->setToolTip(tr("Open raster imagery or vector dataset (Ctrl+O)"));
    actOpen->setShortcut(QKeySequence::Open);
    connect(actOpen, &QAction::triggered, this, [this] {
        QString filter = tr("All Supported Datasets (*.tif *.tiff *.nc *.h5 *.hdf5 *.png *.jpg *.jpeg *.webp *.vrt *.bin *.shp *.SHP);;Raster Images (*.tif *.tiff *.nc *.h5 *.hdf5 *.png *.jpg *.jpeg *.webp *.vrt *.bin);;ESRI Shapefiles (*.shp *.SHP);;All Files (*.*)");
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open Dataset (Raster or Vector)"), QString(), filter);
        if (files.isEmpty()) return;
        QStringList rasterFiles, vectorFiles;
        for (const auto& f : files) {
            if (f.endsWith(".shp", Qt::CaseInsensitive)) {
                vectorFiles << f;
            } else {
                rasterFiles << f;
            }
        }
        if (!rasterFiles.isEmpty())
            ErrorReporter::runGuarded("Open", [&] { openFiles(rasterFiles); });
        if (!vectorFiles.isEmpty())
            ErrorReporter::runGuarded("Open Vector", [&] { openVectorFiles(vectorFiles); });
    });
    toolbar->addAction(actOpen);

    auto* actOpenVector = new QAction(tr("Open Vector"), this);
    actOpenVector->setToolTip(tr("Open ESRI Shapefile vector overlay (.shp) (Ctrl+Shift+V)"));
    actOpenVector->setShortcut(QKeySequence("Ctrl+Shift+V"));
    connect(actOpenVector, &QAction::triggered, this, [this] {
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open Vector Layer (ESRI Shapefile)"),
            QString(),
            tr("ESRI Shapefiles (*.shp *.SHP);;All Files (*.*)"));
        if (!files.isEmpty())
            openVectorFiles(files);
    });
    toolbar->addAction(actOpenVector);

    toolbar->addSeparator();

    auto* actFitAll = new QAction(tr("Fit All"), this);
    actFitAll->setToolTip(tr("Fit view to all layers (Space)"));
    connect(actFitAll, &QAction::triggered, this,
            [this]{ if (auto* c = activeCanvas()) c->fitToLayers(); });
    toolbar->addAction(actFitAll);

    auto* actFitAct = new QAction(tr("Fit Active"), this);
    actFitAct->setToolTip(tr("Fit view to active layer (F)"));
    connect(actFitAct, &QAction::triggered, this, [this] {
        auto active = m_layer_mgr->activeLayer();
        if (!active) { m_canvas->fitToLayers(); return; }
        if (active->type() == LayerType::Raster) {
            auto ext = static_cast<RasterLayer*>(active.get())->extent();
            if (ext.isValid()) {
                Camera cam = m_canvas->camera();
                cam.fitToExtent(ext);
                m_canvas->setCamera(cam);
                m_canvas->update();
                return;
            }
        } else if (active->type() == LayerType::Vector) {
            auto ext = static_cast<VectorLayer*>(active.get())->extent();
            if (ext.isValid()) {
                Camera cam = m_canvas->camera();
                cam.fitToExtent(ext);
                m_canvas->setCamera(cam);
                m_canvas->update();
                return;
            }
        }
        m_canvas->fitToLayers();
    });
    toolbar->addAction(actFitAct);

    toolbar->addSeparator();

    // Quick access to View → New Pane (same slot; the menu action owns the Ctrl+Shift+N
    // shortcut — don't set it here too, or the two actions form an ambiguous shortcut).
    auto* actNewPane = new QAction(tr("New Pane"), this);
    actNewPane->setToolTip(tr("Add a new pane (Ctrl+Shift+N)"));
    connect(actNewPane, &QAction::triggered, this, &MainWindow::addPaneInteractive);
    toolbar->addAction(actNewPane);

    toolbar->addSeparator();

    auto* actInspect = new QAction(tr("Inspect"), this);
    actInspect->setToolTip(tr("Pixel Inspect mode: left-click=active layer, right-click=all layers (I)"));
    actInspect->setCheckable(true);
    actInspect->setShortcut(QKeySequence(Qt::Key_I));
    // Inspect mode is an app-wide UI mode: apply it to every pane (Phase 6).
    connect(actInspect, &QAction::toggled, this, [this](bool on) {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            if (auto* c = m_pane_layout->paneCanvas(i)) {
                c->setInspectMode(on);
                if (!on) c->clearInspectHighlight();
            }
        }
    });
    connect(m_canvas, &MapCanvas::inspectModeChanged, actInspect, &QAction::setChecked);
    toolbar->addAction(actInspect);

    toolbar->addSeparator();
    auto* actShot = new QAction(tr("Screenshot"), this);
    actShot->setToolTip(tr("Save screenshot of map canvas with overlays (Ctrl+Shift+S)"));
    actShot->setShortcut(QKeySequence("Ctrl+Shift+S"));
    connect(actShot, &QAction::triggered, this, &MainWindow::captureScreenshot);
    toolbar->addAction(actShot);

    toolbar->addSeparator();
    auto* actSettings = new QAction(tr("Settings"), this);
    actSettings->setToolTip(tr("Open Preferences / Settings Dialog (Ctrl+,)"));
    connect(actSettings, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    toolbar->addAction(actSettings);
}

void MainWindow::setupDocks() {
    auto* layerDock = new QDockWidget(tr("Layers"), this);
    layerDock->setObjectName("LayerDock");
    layerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_layer_panel = new LayerPanel(layerDock);
    m_layer_panel->setLayerManager(m_layer_mgr);
    // Color-code each layer row by its pane's colour (Phase 6.2).
    m_layer_panel->setPaneColorResolver(
        [this](uint64_t pid) { return m_pane_layout->paneColorForId(pid); });
    // "To Pane" submenu source: the current panes (id + label) — Phase 6.3.
    m_layer_panel->setPaneListResolver([this] {
        std::vector<std::pair<quint64, QString>> v;
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            v.emplace_back(m_pane_layout->paneId(i), m_pane_layout->paneLabel(i));
        return v;
    });
    // Sync badge on each pane header: role + the group MASTER's colour/label (Phase 20).
    m_layer_panel->setPaneSyncResolver(
        [this](quint64 pid) { return m_pane_layout->syncInfoForId(pid); });
    connect(m_layer_panel, &LayerPanel::paneAssignmentRequested,
            this, [this](int layerIndex, quint64 paneId) {
                assignLayerToPane(layerIndex, paneId);
            });
    // --- Phase 18: pane groups, multi-select and the ribbon-drag fix ---
    // Closing a pane from the Layers panel reuses the canvas gear-menu path, so the
    // "cannot drop below one pane" rule and the layer-reassignment are honoured (#8).
    connect(m_layer_panel, &LayerPanel::paneCloseRequested, this, [this](quint64 pid) {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == pid) { closePane(m_pane_layout->paneCanvas(i)); break; }
        m_layer_panel->refreshPanes();
    });
    // Double-click / "Show This Pane": the deliberate gesture that promotes a pane to the
    // front of its stacked region (#1b) — the counterpart of the single-click that must not.
    connect(m_layer_panel, &LayerPanel::paneFocusRequested, this, [this](quint64 pid) {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == pid) {
                setActivePane(i, /*selectTopLayer=*/false, /*bringToFront=*/true);
                break;
            }
    });
    // A drag out of the Layers panel freezes region re-stacking for its duration (#1).
    connect(m_layer_panel, &LayerPanel::dragActiveChanged,
            this, [this](bool active) { m_layer_drag_active = active; });
    // >1 layer selected ⇒ no single subject: blank the per-layer panels, exactly as when no
    // image is loaded. Dropping back to ≤1 restores them from the active layer (#8).
    connect(m_layer_panel, &LayerPanel::selectionSummaryChanged,
            this, [this](int layerCount, int) {
        const bool multi = layerCount > 1;
        if (multi == m_multi_select) return;
        m_multi_select = multi;
        if (m_histo_panel) m_histo_panel->setSuppressed(multi);
        if (m_info_panel)  m_info_panel->setSuppressed(multi);
        onActiveLayerChanged(m_layer_mgr->activeIndex());
    });
    layerDock->setWidget(m_layer_panel);
    addDockWidget(Qt::LeftDockWidgetArea, layerDock);

    auto* propDock = new QDockWidget(tr("Layer Properties"), this);
    propDock->setObjectName("LayerPropDock");
    propDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* propWidget = new QWidget(propDock);
    auto* propLay    = new QVBoxLayout(propWidget);
    propLay->setContentsMargins(8, 8, 8, 8);   // comfortable margin from the 4 edges
    propLay->setSpacing(6);

    m_band_sel = new BandSelectorWidget(propWidget);
    m_cm_sel   = new ColormapSelectorWidget(propWidget);
    m_nodata_widget = new NoDataWidget(propWidget);
    m_nodata_widget->setCanvas(m_canvas);

    // The colormap is only relevant in Gray mode, so it lives on the Gray page of the
    // band stack (which sizes to the current page). In RGB mode it therefore occupies
    // no space at all — no reserved colormap null space.
    m_band_sel->setColormapWidget(m_cm_sel);

    propLay->addWidget(m_band_sel);
    propLay->addWidget(m_nodata_widget);
    propLay->addStretch();

    // Keep each property element a FIXED height regardless of how much vertical space
    // the dock spans (half or full of the panel): elements are never stretched or
    // compressed, the trailing stretch absorbs surplus space, and a QScrollArea scrolls
    // when the dock is shorter than the content.
    for (QWidget* w : {static_cast<QWidget*>(m_band_sel),
                       static_cast<QWidget*>(m_nodata_widget)})
        w->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* propScroll = new QScrollArea(propDock);
    propScroll->setWidget(propWidget);
    propScroll->setWidgetResizable(true);
    propScroll->setFrameShape(QFrame::NoFrame);
    propScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    propDock->setWidget(propScroll);

    addDockWidget(Qt::LeftDockWidgetArea, propDock);

    auto* histoDock = new QDockWidget(tr("Histogram"), this);
    histoDock->setObjectName("HistoDock");
    histoDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                                | Qt::BottomDockWidgetArea);
    m_histo_panel = new HistogramPanel(histoDock);
    m_histo_panel->setLayerManager(m_layer_mgr);
    histoDock->setWidget(m_histo_panel);
    addDockWidget(Qt::LeftDockWidgetArea, histoDock);

    auto* vectorDock = new QDockWidget(tr("Vector Configurator"), this);
    vectorDock->setObjectName("VectorConfigDock");
    vectorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_vector_panel = new VectorLayerPanel(vectorDock);
    m_vector_panel->setLayerManager(m_layer_mgr);
    m_vector_panel->setPaneListResolver([this] {
        std::vector<std::pair<quint64, QString>> v;
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            v.emplace_back(m_pane_layout->paneId(i), m_pane_layout->paneLabel(i));
        return v;
    });
    vectorDock->setWidget(m_vector_panel);
    addDockWidget(Qt::LeftDockWidgetArea, vectorDock);

    tabifyDockWidget(propDock, histoDock);
    tabifyDockWidget(histoDock, vectorDock);

    connect(m_vector_panel, &VectorLayerPanel::duplicateLayerRequested, this, [this](VectorLayer*, uint64_t) {
        if (m_canvas) m_canvas->update();
        if (m_layer_panel) m_layer_panel->refreshPanes();
    });

    connect(m_vector_panel, &VectorLayerPanel::openVectorRequested, this, [this] {
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Open Vector Layer (ESRI Shapefile)"),
            QString(),
            tr("ESRI Shapefiles (*.shp *.SHP);;All Files (*.*)"));
        if (!files.isEmpty())
            openVectorFiles(files);
    });
    connect(m_vector_panel, &VectorLayerPanel::fitToLayerRequested, this, [this](int idx) {
        auto layerPtr = m_layer_mgr->layerAt(idx);
        if (!layerPtr) return;
        if (layerPtr->type() == LayerType::Vector) {
            auto* vl = static_cast<VectorLayer*>(layerPtr.get());
            Extent ext = vl->extent();
            if (!ext.isValid()) return;
            Camera cam = m_canvas->camera();
            cam.fitToExtent(ext);
            m_canvas->setCamera(cam);
            m_canvas->update();
        }
    });
    connect(m_vector_panel, &VectorLayerPanel::layerStyleChanged, this, [this](VectorLayer*) {
        if (m_canvas) m_canvas->update();
    });

    auto* logDock = new QDockWidget(tr("Log"), this);
    logDock->setObjectName("LogDock");
    logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    // Log dock = a small action bar (Export / Clear, FR-APP-10) above the text view.
    auto* logContainer = new QWidget(logDock);
    auto* logLay = new QVBoxLayout(logContainer);
    logLay->setContentsMargins(0, 0, 0, 0);
    logLay->setSpacing(0);
    auto* logBar = new QToolBar(logContainer);
    logBar->setMovable(false);
    logBar->setIconSize(QSize(16, 16));
    auto* actExportLog = logBar->addAction(tr("Export Logs…"));
    actExportLog->setToolTip(tr("Save the log panel contents to a file"));
    connect(actExportLog, &QAction::triggered, this, &MainWindow::exportLogs);
    auto* actClearLog = logBar->addAction(tr("Clear"));
    actClearLog->setToolTip(tr("Clear the log panel (does not affect the log file)"));
    connect(actClearLog, &QAction::triggered, this, [this] {
        m_log_entries.clear();
        if (m_log_widget) m_log_widget->clear();
    });
    m_log_widget = new QPlainTextEdit(logContainer);
    m_log_widget->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log_widget->setReadOnly(true);
    m_log_widget->setMaximumBlockCount(5000);
    m_log_widget->setObjectName("LogWidget");
    logLay->addWidget(logBar);
    logLay->addWidget(m_log_widget, 1);
    logDock->setWidget(logContainer);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);
    resizeDocks({logDock}, {150}, Qt::Vertical);

    connect(m_layer_mgr, &LayerManager::activeLayerChanged,
            this, &MainWindow::onActiveLayerChanged);
    connect(m_layer_mgr, &LayerManager::layerRemoved,
            this, &MainWindow::onActiveLayerChanged);
    // Reap managed temp result files AFTER the erase: the layer's shared_ptr has dropped, so
    // ~RasterDataset has run GDALClose and the file is now deletable (matters on Windows).
    connect(m_layer_mgr, &LayerManager::layerRemoved, this, [this](int) {
        for (const QString& path : m_pending_temp_deletions) {
            if (fvRemoveTempFile(path))
                FV_INFO("Deleted temp result '{}'", path.toStdString());
            else
                FV_WARN("Could not delete temp result '{}'", path.toStdString());
        }
        m_pending_temp_deletions.clear();
    });
    // Also dispose on ANY graceful exit (Ctrl+Q, window close, QApplication::quit, exec() return):
    // aboutToQuit fires while MainWindow is still alive, so it can release datasets + reap temps.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::disposeTempFiles);
    // Release a removed raster's cached GPU tiles (FR-LYR-4). Fires before the erase, so the
    // layer is still resolvable; broadcast to every pane (each canvas owns its own TileCache;
    // the non-owning panes simply find no matching keys).
    connect(m_layer_mgr, &LayerManager::layerAboutToBeRemoved, this, [this](int index) {
        auto l = m_layer_mgr->layerAt(index);
        if (!l || l->type() != LayerType::Raster) return;
        auto* rl_removed = static_cast<RasterLayer*>(l.get());
        // Managed temp result (GDAL op / Raster Math "temporary output"): record its file so it
        // can be deleted once the layer is erased and its GDAL handle is closed (see below). The
        // layer is still live here, so dataset()->filePath() is resolvable; the file must not be
        // deleted yet (still open on Windows).
        if (rl_removed->ownsTempFile() && rl_removed->dataset())
            m_pending_temp_deletions
                << QString::fromStdString(rl_removed->dataset()->filePath());
        // Geolocation-warp sidecars (FR-IO-14): the warped VRT alone is not the whole
        // temp — it references masked X/Y rasters and an intermediate VRT that would
        // otherwise be orphaned in the temp directory.
        for (const auto& f : rl_removed->tempSidecars())
            m_pending_temp_deletions << QString::fromStdString(f);
        const uint64_t id = rl_removed->layerId();
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (auto* c = m_pane_layout->paneCanvas(i)) c->invalidateLayer(id);
        // If the removed layer is its pane's active/representative layer, the Pixel
        // Inspector's drop-down for that pane now shows stale data — drop it (matches the
        // representative-layer pick in inspectFromPane).
        if (m_attr_insp) {
            const uint64_t pid = l->paneId();
            auto active = m_layer_mgr->activeLayer();
            std::shared_ptr<Layer> rep;
            if (active && active->type() == LayerType::Raster && active->paneId() == pid)
                rep = active;
            else
                rep = m_layer_mgr->layerAt(topLayerIndexInPane(pid));
            if (rep.get() == l.get()) m_attr_insp->removePaneGroup(pid);
        }
        // Drop the removed layer's spectra (and its membership of any merged plot) — Phase 26.
        if (m_spectral_panel) m_spectral_panel->forgetLayer(rl_removed->layerId());
        // The profile keeps the same per-layer bookkeeping since Phase 26.5 (FR-ANL-12).
        if (m_profile_panel) m_profile_panel->forgetLayer(rl_removed->layerId());
    });
    // A layer change (e.g. visibility toggle) re-evaluates each pane's legend so the colorbar
    // appears/disappears with the rendered layer (Phase 6.4.2, point 1).
    connect(m_layer_mgr, &LayerManager::layerChanged, this, [this](int) { updatePaneLegends(); });
    // Selecting a layer activates the pane that owns it (keeping THIS layer active), so
    // Layer-Properties / Histogram edits immediately repaint the pane showing it (Phase 6.3).
    connect(m_layer_mgr, &LayerManager::activeLayerChanged, this, [this](int idx) {
        auto l = m_layer_mgr->layerAt(idx);
        if (!l) return;
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == l->paneId()) {
                // bringToFront=false (Phase 18 #1): selecting a layer marks its pane active
                // (border + panel state) but must NOT re-stack the region — otherwise a
                // press on a Pane-1 row would pull Pane 1 in front of Pane 2 and the pane
                // the user was about to drop onto would vanish before the drag begins.
                setActivePane(i, false, false);
                break;
            }
    });
    // New raster layers inherit the persisted display-resampling default (FR-RND-10).
    connect(m_layer_mgr, &LayerManager::layerAdded,
            this, [this](int index) {
                auto layerPtr = m_layer_mgr->layerAt(index);
                if (layerPtr && layerPtr->type() == LayerType::Raster)
                    static_cast<RasterLayer*>(layerPtr.get())->setDisplayResampling(
                        static_cast<RasterLayer::DisplayResampling>(
                            Settings::instance().displayResampling()));
            });
    connect(m_layer_panel, &LayerPanel::activeLayerChanged,
            this, [this](int idx) {
                m_layer_mgr->setActiveLayer(idx);
            });
    connect(m_layer_panel, &LayerPanel::fitToLayerRequested,
            this, [this](int idx) {
                auto layerPtr = m_layer_mgr->layerAt(idx);
                if (!layerPtr || layerPtr->type() != LayerType::Raster) return;
                auto* rl = static_cast<RasterLayer*>(layerPtr.get());
                Extent ext = rl->extent();
                if (!ext.isValid()) return;
                Camera cam = m_canvas->camera();
                cam.fitToExtent(ext);
                m_canvas->setCamera(cam);
                m_canvas->update();
            });
    connect(m_layer_panel, &LayerPanel::layerDatasetChanged,
            this, [this](int idx) {
                auto layer = m_layer_mgr->layerAt(idx);
                if (layer && layer->type() == LayerType::Raster)
                    m_canvas->invalidateLayer(
                        static_cast<RasterLayer*>(layer.get())->layerId());
                m_canvas->update();
                onActiveLayerChanged(idx);
            });

    connect(m_band_sel, &BandSelectorWidget::bandMappingChanged, this, [this]{
        if (m_canvas) m_canvas->update();
        // Refresh the histogram so it follows the new band mapping (1 view in Gray,
        // 3 R/G/B views in composite) — FR-HST-6.
        auto* mgr = m_layer_mgr;
        if (mgr && mgr->activeIndex() >= 0) mgr->notifyLayerChanged(mgr->activeIndex());
        updatePaneLegends();   // RGB↔Gray toggles the legend's visibility (per pane)
    });
    connect(m_cm_sel,   &ColormapSelectorWidget::colormapChanged, this, [this]{
        if (m_canvas) m_canvas->update();
        updatePaneLegends();   // repaint the legend of whichever pane shows this layer
    });
    connect(m_histo_panel, &HistogramPanel::stretchChanged,
            this, [this](RasterLayer*, float, float) {
        if (m_canvas) m_canvas->update();
        updatePaneLegends();   // legend tick labels follow the stretch range
    });

    auto* infoDock = new QDockWidget(tr("Layer Info"), this);
    infoDock->setObjectName("InfoDock");
    infoDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_info_panel = new RasterInfoPanel(infoDock);
    m_info_panel->setLayerManager(m_layer_mgr);
    infoDock->setWidget(m_info_panel);
    addDockWidget(Qt::RightDockWidgetArea, infoDock);

    // GPU Monitor dock (FR-APP-13): live estimated-VRAM sparkline. Default placement
    // = right column, lower half. It is split BELOW infoDock *now*, while infoDock is
    // still alone — splitDockWidget only creates a real vertical split against a dock
    // that is not yet tabbed (once attrDock is tabbed in below, a split would instead
    // add a new tab). attrDock then joins the TOP group; GPU Monitor stays bottom.
    auto* gpuDock = new QDockWidget(tr("Resource Monitor"), this);
    gpuDock->setObjectName("GpuDock");
    gpuDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                              | Qt::BottomDockWidgetArea);
    m_gpu_monitor = new GpuMonitorPanel(gpuDock);
    gpuDock->setWidget(m_gpu_monitor);
    splitDockWidget(infoDock, gpuDock, Qt::Vertical);   // Resource Monitor below Info

    auto* attrDock = new QDockWidget(tr("Pixel Inspector"), this);
    attrDock->setObjectName("AttrDock");
    attrDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                               | Qt::BottomDockWidgetArea);
    m_attr_insp = new AttributeInspector(attrDock);
    m_attr_insp->setLayerManager(m_layer_mgr);
    attrDock->setWidget(m_attr_insp);
    addDockWidget(Qt::RightDockWidgetArea, attrDock);
    tabifyDockWidget(infoDock, attrDock);               // Pixel Inspector joins the TOP group

    auto* numDumpDock = new QDockWidget(tr("Numeric Dump"), this);
    numDumpDock->setObjectName("NumericDumpDock");
    numDumpDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                                 | Qt::BottomDockWidgetArea);
    m_numeric_dump = new NumericDumpPanel(numDumpDock);
    m_numeric_dump->setLayerManager(m_layer_mgr);
    numDumpDock->setWidget(m_numeric_dump);
    addDockWidget(Qt::RightDockWidgetArea, numDumpDock);
    tabifyDockWidget(attrDock, numDumpDock);            // Numeric Dump joins next to Pixel Inspector

    resizeDocks({infoDock, gpuDock}, {300, 300}, Qt::Vertical);   // ≈ half each

    // The two plots are WINDOWS, not docks (Phase 26.8, FR-ANL-1/2). They were docks from
    // Phase 26 until manual testing showed the dock frame, the tab bar and the panel list were
    // all overhead for two panels nobody ever parked: what was actually wanted is a window that
    // stays above the main one — always above FlashViewer, never above the browser you switch
    // to. A parented Qt::Window delivered that on Windows ONLY; the flags that mean it on all
    // three platforms live in fvApplyPlotWindowFlags (see PlotWindowChrome.hpp). Each is closed
    // on a fresh profile: a plot is a deliberate tool, not something that opens itself.
    m_spectral_panel = new SpectralPlotPanel(this);
    m_spectral_panel->setLayerManager(m_layer_mgr);
    fvApplyPlotWindowFlags(m_spectral_panel);
    m_spectral_panel->setWindowTitle(tr("Spectral Plot"));
    m_spectral_panel->resize(720, 430);
    m_spectral_panel->hide();
    m_spectral_panel->installEventFilter(this);

    m_profile_panel = new ScanPixProfilePanel(m_layer_mgr, this);
    // The profiled layer's pane colour — the panel is pane-agnostic, MainWindow owns the
    // PaneLayout, exactly as the Layers panel's colour resolver is wired.
    m_profile_panel->setPaneColorResolver(
        [this](quint64 pid) { return m_pane_layout->paneColorForId(pid); });
    // What one Compute profiles (Phase 26.5, FR-ANL-12): the active layer alone, or — when its
    // pane is synced — every visible raster across the whole sync group, merged into one plot.
    // Assembled here for the same reason the inspect groups are: only MainWindow knows the
    // sync roles, and using the SAME InspectPaneGroup shape keeps the profile's idea of a
    // "scope" identical to the Pixel Inspector's and the Spectral Plot's.
    m_profile_panel->setScopeResolver([this] { return buildProfileScope(); });
    fvApplyPlotWindowFlags(m_profile_panel);
    m_profile_panel->setWindowTitle(tr("Scan/Pixel Profile"));
    m_profile_panel->resize(760, 520);
    m_profile_panel->hide();
    m_profile_panel->installEventFilter(this);

    // Track every dock with its fresh-build placement so View → Panels can re-open a
    // closed panel at its original location (FR-APP-9). reserve() first: buildPanelsMenu
    // captures &element into lambdas, so the vector must never reallocate afterward.
    m_docks.reserve(8);
    m_docks.append({layerDock,   Qt::LeftDockWidgetArea,   nullptr,  nullptr});
    m_docks.append({propDock,    Qt::LeftDockWidgetArea,   nullptr,  nullptr});
    m_docks.append({histoDock,   Qt::LeftDockWidgetArea,   propDock, nullptr});
    m_docks.append({infoDock,    Qt::RightDockWidgetArea,  nullptr,  nullptr});
    m_docks.append({attrDock,    Qt::RightDockWidgetArea,  infoDock, nullptr});
    m_docks.append({numDumpDock, Qt::RightDockWidgetArea,  attrDock, nullptr});
    m_docks.append({logDock,     Qt::BottomDockWidgetArea, nullptr,  nullptr});
    m_docks.append({gpuDock,     Qt::RightDockWidgetArea,  nullptr,  nullptr});

    // Default left-column split: the Layers list (top) and the tabbed Layer
    // Properties / Histogram group (bottom) each take ≈ half the column's height, so
    // the Histogram tab has room for the three RGB histograms (FR-HST-6). propDock and
    // histoDock share one cell (tabbed), so sizing propDock sizes the whole group.
    resizeDocks({layerDock, propDock}, {300, 300}, Qt::Vertical);

    // Pristine default arrangement (taken before restoreLayout() applies saved state)
    // — used by "Reset Panel Layout".
    m_default_layout_state = saveState();
    buildPanelsMenu();

    // The scale bar is now a per-pane overlay owned by each MapCanvas (Phase 6.4.5) — no
    // single app-wide bar here.
}

void MainWindow::buildPanelsMenu() {
    if (!m_panels_menu) return;

    for (DockEntry& e : m_docks) {
        e.action = m_panels_menu->addAction(e.dock->windowTitle());
        e.action->setCheckable(true);
        e.action->setChecked(!e.dock->isHidden());
        connect(e.action, &QAction::toggled, this, [this, &e](bool on) {
            if (on) reopenDockToDefault(e);
            else    e.dock->hide();
        });
    }

    m_panels_menu->addSeparator();
    QAction* actReset = m_panels_menu->addAction(tr("&Reset Panel Layout"));
    connect(actReset, &QAction::triggered, this, [this] {
        restoreState(m_default_layout_state);
    });

    // Refresh checkmarks from the live state each time the menu opens. Use isHidden()
    // (true only when explicitly closed) — isVisible()/visibilityChanged would falsely
    // report a dock that is merely the non-front tab as hidden.
    connect(m_panels_menu, &QMenu::aboutToShow, this, [this] {
        for (DockEntry& e : m_docks) {
            if (!e.action) continue;
            QSignalBlocker block(e.action);
            e.action->setChecked(!e.dock->isHidden());
        }
    });
}

// Suffix for the second half of a plot window's remembered layout: the chart/legend divider,
// stored beside the frame under the same key family.
static const QString kSplitSuffix = QStringLiteral("Split");

void MainWindow::showPlotWindow(QWidget* w, const QString& key) {
    if (!w) return;
    // Restore the saved frame ONCE, on the first open of the session: doing it on every show
    // would undo a move the user made and then closed the window from.
    if (w->property("fvGeometryRestored").isNull()) {
        const QByteArray geo = Settings::instance().plotWindowGeometry(key);
        if (!geo.isEmpty()) w->restoreGeometry(geo);
        // The chart/legend divider travels with the frame: a divider that reset every launch
        // would be worse than one that could not be dragged at all.
        const QByteArray split = Settings::instance().plotWindowGeometry(key + kSplitSuffix);
        if (!split.isEmpty()) {
            if (auto* sp = qobject_cast<SpectralPlotPanel*>(w))        sp->restoreSplitState(split);
            else if (auto* pp = qobject_cast<ScanPixProfilePanel*>(w)) pp->restoreSplitState(split);
        }
        w->setProperty("fvGeometryRestored", true);
        w->setProperty("fvGeometryKey", key);
    }
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::reopenDockToDefault(const DockEntry& e) {
    addDockWidget(e.area, e.dock);                 // relocate to the fresh-build area
    if (e.tabWith && !e.tabWith->isHidden() && !e.tabWith->isFloating())
        tabifyDockWidget(e.tabWith, e.dock);       // rejoin its original tab group
    e.dock->show();
    e.dock->raise();
}

void MainWindow::setupStatusBar() {
    m_coord_label = new QLabel("X: --  Y: --  |  Lat: --  Lon: --", this);
    m_coord_label->setMinimumWidth(380);
    statusBar()->addWidget(m_coord_label);
    statusBar()->addWidget(new QLabel(" | ", this));

    m_zoom_label = new QLabel("Display Scale: --", this);
    m_zoom_label->setMinimumWidth(120);
    statusBar()->addWidget(m_zoom_label);
    statusBar()->addWidget(new QLabel(" | ", this));

    m_crs_label = new QLabel("CRS: --", this);
    m_crs_label->setMinimumWidth(150);
    // Clickable → open the Project-CRS picker for the active pane (Phase 11, FR-CRS-2).
    m_crs_label->setCursor(Qt::PointingHandCursor);
    m_crs_label->setToolTip(tr("Project CRS of the active pane. Click to change."));
    m_crs_label->installEventFilter(this);
    statusBar()->addWidget(m_crs_label);
    statusBar()->addWidget(new QLabel(" | ", this));

    m_pixel_label = new QLabel("Col: --  Row: --", this);
    m_pixel_label->setMinimumWidth(180);
    statusBar()->addWidget(m_pixel_label);

    statusBar()->showMessage(tr("Ready"), 3000);
}

void MainWindow::restoreLayout() {
    // Saved state from a different dock configuration causes
    // QDockAreaLayoutItem::skip() to crash during traversal.
    // Guard by checking the layout schema version stored at close time.
    if (Settings::instance().layoutVersion() != Settings::kCurrentLayoutVersion) {
        Settings::instance().clearLayoutState();
        return;
    }
    const QByteArray geo   = Settings::instance().loadGeometry();
    const QByteArray state = Settings::instance().loadState();
    if (!geo.isEmpty())   restoreGeometry(geo);
    if (!state.isEmpty() && !restoreState(state))
        Settings::instance().clearLayoutState();  // corrupt data — discard
}

// --------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event) {
    // Persist the full FR-APP-6 state set: geometry, dock layout (+version),
    // theme, and OSM tile URL. Theme/OSM-URL are also saved at change-time;
    // saving them here makes the documented quit behaviour explicit and robust.
    // (Project CRS is added in Phase 11.)
    Settings::instance().setLayoutVersion(Settings::kCurrentLayoutVersion);
    Settings::instance().saveGeometry(saveGeometry());
    Settings::instance().saveState(saveState());
    // The plot windows are not part of saveState() any more, so each saves its own frame.
    if (m_spectral_panel && m_spectral_panel->isVisible()) {
        Settings::instance().setPlotWindowGeometry(QStringLiteral("spectral"),
                                                   m_spectral_panel->saveGeometry());
        Settings::instance().setPlotWindowGeometry(QStringLiteral("spectral") + kSplitSuffix,
                                                   m_spectral_panel->saveSplitState());
    }
    if (m_profile_panel && m_profile_panel->isVisible()) {
        Settings::instance().setPlotWindowGeometry(QStringLiteral("profile"),
                                                   m_profile_panel->saveGeometry());
        Settings::instance().setPlotWindowGeometry(QStringLiteral("profile") + kSplitSuffix,
                                                   m_profile_panel->saveSplitState());
    }
    if (auto* app = qobject_cast<Application*>(qApp))
        Settings::instance().setTheme(app->currentTheme());
    if (m_canvas && m_canvas->osmRenderer() && m_canvas->osmRenderer()->provider())
        Settings::instance().setOsmTileUrl(
            m_canvas->osmRenderer()->provider()->urlTemplate());
    FV_INFO("MainWindow closing --- state saved (geometry/layout/theme/OSM URL)");
    disposeTempFiles();   // delete managed temp results (also wired to aboutToQuit for any exit path)
    event->accept();
}

void MainWindow::disposeTempFiles() {
    if (m_temp_disposed) return;
    m_temp_disposed = true;
    if (!m_layer_mgr) return;

    // Gather every managed temp path BEFORE closing datasets: any already queued by the
    // per-removal reaper, plus all currently-loaded temp-owning layers.
    QStringList paths = m_pending_temp_deletions;
    m_pending_temp_deletions.clear();
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && l->type() == LayerType::Raster) {
            auto* rl = static_cast<RasterLayer*>(l.get());
            if (rl->ownsTempFile() && rl->dataset())
                paths << QString::fromStdString(rl->dataset()->filePath());
            for (const auto& f : rl->tempSidecars())
                paths << QString::fromStdString(f);
        }
    }
    // Release the datasets (GDALClose) so the files are deletable (esp. on Windows), then reap.
    m_layer_mgr->clear();
    int gone = 0;
    for (const QString& p : paths)
        if (fvRemoveTempFile(p)) ++gone;
    if (!paths.isEmpty())
        FV_INFO("Shutdown: deleted {}/{} managed temp result file(s)", gone, paths.size());
}

MapCanvas* MainWindow::activeCanvas() const {
    return m_pane_layout->paneCanvas(m_active_pane_idx);
}

uint64_t MainWindow::activePaneId() const {
    return m_pane_layout->paneId(m_active_pane_idx);
}

QString MainWindow::paneProjectCrs(uint64_t paneId) const {
    // Authoritative per-pane Project CRS (Phase 11): read the canvas's stored CRS (which
    // honours a user override), not a fresh bottom-raster scan. Empty ⇒ geographic.
    for (int p = 0; p < m_pane_layout->paneCount(); ++p)
        if (m_pane_layout->paneId(p) == paneId)
            if (auto* c = m_pane_layout->paneCanvas(p))
                return QString::fromStdString(c->projectCrsWkt());
    return {};
}

void MainWindow::setActivePane(int idx, bool selectTopLayer, bool bringToFront) {
    if (idx < 0 || idx >= m_pane_layout->paneCount()) return;
    const bool paneChanged = (idx != m_active_pane_idx);
    m_active_pane_idx = idx;
    if (auto* c = m_pane_layout->paneCanvas(idx)) {
        m_canvas = c;   // menu camera actions & member-capturing lambdas follow the active pane
        c->emitZoomLevel();   // refresh the m/px readout for the newly-active pane (FR-GIS-3)
    }

    // Active-pane highlight border (point 10): always reflect the current active pane.
    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (auto* c = m_pane_layout->paneCanvas(i)) c->setActive(i == idx);

    // Keep the active pane visible: if it is stacked behind others in its region, bring
    // it to the front (Phase 6.4). showPaneInRegion does not re-emit activation.
    // Skipped for layer-selection-driven activation and while a layer drag from the Layers
    // panel is in flight — re-stacking a region there would hide the user's drop target
    // before the drop lands (Phase 18 #1).
    if (bringToFront && !m_layer_drag_active)
        m_pane_layout->showPaneInRegion(m_pane_layout->paneId(idx));

    // On switching to a different pane, make that pane's topmost layer the active
    // layer: highlights it in the Layers panel and drives the property panels
    // (Band/Colormap/NoData/Histogram/Info). Clicking within the same pane keeps the
    // user's current layer selection. (Skipped during initial construction, and when the
    // activation was driven by a layer selection — selectTopLayer=false.)
    if (paneChanged && selectTopLayer && m_layer_mgr)
        m_layer_mgr->setActiveLayer(topLayerIndexInPane(m_pane_layout->paneId(idx)));

    updateProjectCrsStatus();   // status-bar CRS follows the active pane (Phase 11)
}

void MainWindow::wireCanvasSignals(MapCanvas* canvas) {
    if (!canvas) return;

    // Clicking a pane makes it the active pane.
    connect(canvas, &MapCanvas::activated, this, [this, canvas] {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneCanvas(i) == canvas) { setActivePane(i); break; }
    });

    // Status bar — whichever pane the cursor is over updates the readouts.
    connect(canvas, &MapCanvas::cursorGeoPos, this, [this, canvas](double x, double y) {
        auto fmt = fvFormatCoordinates(x, y, canvas->projectCrsWkt());
        m_coord_label->setText(fmt.single_line);
    });
    connect(canvas, &MapCanvas::cursorPixelPos, this, [this](int col, int row) {
        m_pixel_label->setText(
            (col < 0 || row < 0)
            ? "Col: --  Row: --"
            : QString("Col: %1  Row: %2").arg(col).arg(row));
    });
    // FR-GIS-3: metres-per-pixel readout (auto m/px ↔ km/px). The signal now carries metres/px
    // (geographic scales converted via the L-1 approximation inside MapCanvas).
    connect(canvas, &MapCanvas::zoomLevelChanged, this, [this](double mpp) {
        m_zoom_label->setText(mpp >= 1000.0
            ? QString("Display Scale: %1 km/px").arg(mpp / 1000.0, 0, 'g', 4)
            : QString("Display Scale: %1 m/px").arg(mpp, 0, 'g', 4));
    });

    // Inspect mode (Phase 6.6): left-click → the clicked pane's active/representative layer,
    // right-click → all its visible rasters; if the clicked pane is synced, aggregate across
    // its whole sync group (one row-group per pane).
    connect(canvas, &MapCanvas::pixelInspectRequest, this, [this, canvas](double x, double y) {
        inspectFromPane(canvas, x, y, /*allLayers=*/false);
    });
    connect(canvas, &MapCanvas::pixelInspectAllRequest, this, [this, canvas](double x, double y) {
        inspectFromPane(canvas, x, y, /*allLayers=*/true);
    });

    // Pane gear-menu actions (Phase 6.1 / 6.2).
    connect(canvas, &MapCanvas::paneCloseRequested,  this, [this, canvas]{ closePane(canvas);  });
    connect(canvas, &MapCanvas::paneRenameRequested, this, [this, canvas]{ renamePane(canvas); });
    connect(canvas, &MapCanvas::paneColorRequested,  this, [this, canvas]{ colorPane(canvas);  });

    // Sync With (Phase 6.5): the gear submenu lists the other panes (resolver), toggling one
    // makes this canvas the master; Unsync dissolves the group.
    canvas->setSyncInfoResolver([this, canvas]() {
        std::vector<PaneSyncEntry> out;
        const uint64_t mId = canvas->paneId();
        const bool mSynced = m_pane_layout->paneSynced(mId);
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            const uint64_t id = m_pane_layout->paneId(i);
            if (id == mId) continue;
            out.push_back(PaneSyncEntry{ id, m_pane_layout->paneLabel(i),
                                         mSynced && m_pane_layout->paneSynced(id) });
        }
        return out;
    });
    connect(canvas, &MapCanvas::paneSyncToggleRequested, this, [this, canvas](uint64_t otherId) {
        m_pane_layout->syncToggle(canvas->paneId(), otherId);
    });
    connect(canvas, &MapCanvas::paneUnsyncRequested, this, [this] { m_pane_layout->clearSync(); });

    // Ghost cursor (Phase 6.5 / 6.5.1): mirror this pane's cursor onto EVERY synced pane
    // (including the source) so all ghost markers sit at the same geographic point and move
    // together. In the source pane the marker coincides with the live cursor.
    connect(canvas, &MapCanvas::cursorGeoPos, this, [this, canvas](double x, double y) {
        const uint64_t srcId = canvas->paneId();
        if (!m_pane_layout->paneSynced(srcId)) return;
        // (x, y) are in the SOURCE pane's Project CRS, which a synced sibling need not share
        // (Phase 11) — same rule as the inspect highlight below: transform into each pane's
        // own CRS, and show no ghost at all where the point does not exist there, since a
        // marker in the wrong place is worse than none.
        const std::string srcWkt = canvas->projectCrsWkt();
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            const uint64_t id = m_pane_layout->paneId(i);
            if (!m_pane_layout->paneSynced(id)) continue;
            auto* c = m_pane_layout->paneCanvas(i);
            if (!c) continue;
            double gx = x, gy = y;
            if (fvTransformPoint(srcWkt, c->projectCrsWkt(), gx, gy))
                c->setGhostCursor(gx, gy, true);
            else
                c->setGhostCursor(0.0, 0.0, false);
        }
    });

    // A layer dragged from the Layers panel onto this pane → reassign it here (Phase 6.3).
    connect(canvas, &MapCanvas::layerDropped, this, [this, canvas](int layerIndex) {
        assignLayerToPane(layerIndex, canvas->paneId());
    });

    // Project CRS (Phase 11): keep the status-bar CRS readout in sync when this pane's
    // Project CRS changes, and open the CRS picker from the pane gear-menu entry.
    connect(canvas, &MapCanvas::projectCrsChanged, this, [this, canvas](const QString&) {
        if (canvas == m_canvas) updateProjectCrsStatus();
    });
    connect(canvas, &MapCanvas::paneCrsRequested, this, [this, canvas] {
        openProjectCrsPicker(canvas);
    });
    // Per-pane "Show Colorbar" toggled (Phase 16 #5): re-evaluate which layer's legend shows.
    connect(canvas, &MapCanvas::colorbarVisibilityChanged, this, [this] { updatePaneLegends(); });
    // On-the-fly reprojection notice → non-modal banner (Phase 11, FR-CRS-6): amber for the
    // informational notice, red/error styling when a layer was omitted (failed).
    connect(canvas, &MapCanvas::reprojectionNotice, this, [this](const QString& msg, bool failed) {
        showErrorBanner(failed ? 4 : 3, msg);
    });
}

void MainWindow::assignLayerToPane(int layerIndex, uint64_t paneId) {
    auto l = m_layer_mgr->layerAt(layerIndex);
    if (!l || l->paneId() == paneId) return;

    // If moving a vector layer to a new pane, convert to target pane's dataset CRS
    if (l->type() == LayerType::Vector) {
        auto* vl = static_cast<VectorLayer*>(l.get());
        if (paneId > 0 && vl->dataset()) {
            MapCanvas* target = nullptr;
            QString targetLabel = tr("Pane %1").arg(paneId);
            for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
                if (m_pane_layout->paneId(i) == paneId) {
                    target = m_pane_layout->paneCanvas(i);
                    targetLabel = m_pane_layout->paneLabel(i);
                    break;
                }
            }

            // Check if the target pane contains an unreferenced raster dataset with no CRS
            bool targetHasUnrefRaster = false;
            QString unrefRasterName;
            for (int i = 0; i < m_layer_mgr->count(); ++i) {
                auto cand = m_layer_mgr->layerAt(i);
                if (cand && fvLayerInPane(*cand, paneId) && cand->type() == LayerType::Raster) {
                    auto* rl = static_cast<RasterLayer*>(cand.get());
                    if (!rl->dataset() || rl->dataset()->crsWkt().empty()) {
                        targetHasUnrefRaster = true;
                        unrefRasterName = rl->name();
                        break;
                    }
                }
            }
            if (targetHasUnrefRaster) {
                QMessageBox::warning(this, tr("Cannot Overlay on Unreferenced Pane"),
                    tr("Cannot move vector layer '%1' to %2 because it contains unreferenced raster dataset '%3' with no CRS.\n\n"
                       "Vector layers require a georeferenced pane.")
                    .arg(vl->name()).arg(targetLabel).arg(unrefRasterName));
                if (m_layer_panel) m_layer_panel->refreshPanes();
                return;
            }

            const std::string targetCrs = target ? target->projectCrsWkt() : std::string();
            if (!targetCrs.empty() && targetCrs != vl->dataset()->crsWkt() && !vl->dataset()->crsWkt().empty()) {
                std::string reason;
                if (!vl->dataset()->canReprojectTo(targetCrs, &reason)) {
                    QMessageBox::warning(this, tr("CRS Conversion Failed"),
                        tr("Cannot move vector layer '%1' to %2.\n\n"
                           "Reason: %3\n\n"
                           "Vector Layer CRS: %4\n"
                           "Target Dataset CRS: %5")
                        .arg(vl->name())
                        .arg(targetLabel)
                        .arg(QString::fromStdString(reason))
                        .arg(fvCrsShortName(vl->dataset()->crsWkt()))
                        .arg(fvCrsShortName(targetCrs)));
                    if (m_layer_panel) m_layer_panel->refreshPanes();
                    return;
                }

                // Show progress bar during conversion
                QProgressDialog progress(
                    tr("Converting vector layer '%1' to %2 CRS (%3)…")
                        .arg(vl->name())
                        .arg(targetLabel)
                        .arg(fvCrsShortName(targetCrs)),
                    tr("Cancel"), 0, 100, this);
                progress.setWindowModality(Qt::ApplicationModal);
                progress.setMinimumDuration(150);
                progress.setValue(0);

                std::function<bool(int, int)> progressCb = [&](int cur, int tot) -> bool {
                    progress.setValue(cur * 100 / std::max(1, tot));
                    QApplication::processEvents();
                    return !progress.wasCanceled();
                };

                auto geoms = vl->dataset()->geometriesForCrs(targetCrs, progressCb);

                if (progress.wasCanceled() || !geoms) {
                    if (progress.wasCanceled()) {
                        showErrorBanner(2, tr("Vector layer move canceled."));
                    } else {
                        QMessageBox::warning(this, tr("CRS Conversion Failed"),
                            tr("Vector geometry coordinate conversion failed for '%1'.").arg(vl->name()));
                    }
                    if (m_layer_panel) m_layer_panel->refreshPanes();
                    return;
                }
            }
        }
    }

    // Was the target pane empty? If so, fit it to the newly-assigned layer.
    int targetCount = 0;
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto x = m_layer_mgr->layerAt(i);
        if (x && x->paneId() == paneId) ++targetCount;
    }

    l->setPaneId(paneId);

    MapCanvas* target = nullptr;
    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (m_pane_layout->paneId(i) == paneId) { target = m_pane_layout->paneCanvas(i); break; }
    if (target && targetCount == 0) {
        // Phase 17 #4: an empty pane adopts the dropped layer's CRS (FR-CRS-3), then fits to
        // it. Derive the CRS BEFORE fitting so the camera is fitted in the new CRS rather
        // than reprojected afterwards (mirrors the layerAdded handler). refreshDerivedProjectCrs
        // is a no-op if the user has pinned this pane's CRS, and fires for NetCDF/HDF layers
        // too (their CRS is on the dataset by the time the layer exists). A NON-empty pane
        // keeps its CRS and reprojects/native-falls-back the dropped layer (TileRenderer).
        target->refreshDerivedProjectCrs();
        target->fitToLayers();
    }

    // Layer leaves its source pane and appears on the target — refresh every canvas, the
    // per-pane legends, and the Layers-panel colour-coding.
    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (auto* c = m_pane_layout->paneCanvas(i)) c->update();
    updatePaneLegends();
    if (m_layer_panel) m_layer_panel->refreshPaneColors();
    FV_INFO("Layer {} reassigned to pane id {}", layerIndex, paneId);
}

void MainWindow::applyPaneLayoutMode(PaneLayoutMode m) {
    m_pane_layout->setMode(m);
    // Keep the active pane visible + selected after the regions are rebuilt.
    m_pane_layout->showPaneInRegion(activePaneId());
    // Re-tick the menu. setChecked emits `toggled`, not `triggered`, so this cannot recurse
    // back into the lambda above.
    int idx = 0;
    switch (m) {
        case PaneLayoutMode::Full:    idx = 0; break;
        case PaneLayoutMode::HalfH:   idx = 1; break;
        case PaneLayoutMode::HalfV:   idx = 2; break;
        case PaneLayoutMode::Quarter: idx = 3; break;
    }
    if (m_layout_acts[idx]) m_layout_acts[idx]->setChecked(true);
}

void MainWindow::addPaneInteractive() {
    // Ask for the name AND the position before creating the pane. The name is pre-filled with
    // the default the pane would get anyway ("Pane N", N one past the highest number currently
    // on the canvas — so a closed Pane 2 frees that name again); the position picker offers
    // every cell of every layout preset, pre-selecting the first free region of the current
    // one. Cancel aborts; a blank name takes the default.
    QVector<ExistingPaneInfo> panes;
    panes.reserve(m_pane_layout->paneCount());
    int activeIdx = 0;
    const uint64_t activeId = activePaneId();
    for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
        panes.push_back({ m_pane_layout->paneLabel(i), m_pane_layout->paneColor(i),
                          m_pane_layout->regionOfPane(i) });
        if (m_pane_layout->paneId(i) == activeId) activeIdx = i;
    }

    NewPaneDialog dlg(m_pane_layout->nextPaneLabel(), m_pane_layout->mode(),
                      panes, activeIdx, this);
    if (dlg.exec() != QDialog::Accepted) return;

    // Switch the layout FIRST. setMode redistributes the existing panes by the auto-fill rule
    // — which is exactly what the picker previewed — so the new pane's target region is only
    // meaningful once the mode it belongs to is in force.
    const PaneSlotTarget target = fvSlotTarget(dlg.slot());
    if (target.mode != m_pane_layout->mode()) applyPaneLayoutMode(target.mode);

    MapCanvas* canvas = addPane(dlg.paneName());
    const int idx = m_pane_layout->indexOfCanvas(canvas);
    if (idx >= 0) {
        // Auto-fill may already have landed it elsewhere; movePaneToRegion also brings it to
        // the front of the target region's stack and re-activates it.
        m_pane_layout->movePaneToRegion(m_pane_layout->paneId(idx), target.region);
        FV_INFO("New pane placed in region {} of layout mode {}",
                target.region, static_cast<int>(target.mode));
    }
}

MapCanvas* MainWindow::addPane(const QString& label) {
    // not auto-synced (Phase 6, point 13); empty label ⇒ PaneLayout's default
    auto* canvas = m_pane_layout->addPane(false, label);
    wireCanvasSignals(canvas);
    // Inherit the current basemap + inspect mode + Performance HUD so the new pane
    // matches the others.
    if (m_canvas) {
        canvas->osmRenderer()->setEnabled(m_canvas->osmRenderer()->isEnabled());
        canvas->setInspectMode(m_canvas->inspectMode());
        canvas->setPerfHudVisible(m_canvas->perfHudVisible());   // FR-APP-14
    }
    canvas->osmRenderer()->provider()->setUrlTemplate(Settings::instance().osmTileUrl());
    if (auto* app = qobject_cast<Application*>(qApp))
        canvas->setDarkBackground(app->currentTheme() == Theme::Dark);
    const int newIdx = m_pane_layout->paneCount() - 1;
    canvas->setPaneLabel(m_pane_layout->paneLabel(newIdx));
    // Theme-aware colour for the new pane, chosen against the colours CURRENTLY on the canvas
    // rather than the pane's index — closing a pane must free its colour instead of shifting
    // the sequence, so no two live panes ever share one (FR-PNE-7). The near-duplicate test
    // inside fvNextPaneColor also keeps palette[0] away from the default pane's accent blue.
    const bool dark = qobject_cast<Application*>(qApp)
                          ? qobject_cast<Application*>(qApp)->currentTheme() == Theme::Dark : true;
    const QColor paneCol = m_pane_layout->nextPaneColor(dark);
    m_pane_layout->setPaneColor(newIdx, paneCol);
    canvas->setPaneColor(paneCol);   // border + ID label
    if (m_layer_panel) m_layer_panel->refreshPaneColors();
    setActivePane(newIdx);   // newly-opened layers land here
    FV_INFO("Added pane {} (id {})", newIdx, activePaneId());
    return canvas;
}

void MainWindow::closePane(MapCanvas* canvas) {
    const int idx = m_pane_layout->indexOfCanvas(canvas);
    if (idx < 0) return;
    const uint64_t pid = m_pane_layout->paneId(idx);

    if (m_pane_layout->paneCount() <= 1) {
        // Can't drop below one pane (point 12): clear this pane's layers instead so it
        // becomes an empty default pane (removal high→low so indices stay valid).
        for (int i = m_layer_mgr->count() - 1; i >= 0; --i) {
            auto l = m_layer_mgr->layerAt(i);
            if (l && l->paneId() == pid) m_layer_mgr->removeLayer(i);
        }
        return;
    }

    // Reassign this pane's layers to a surviving pane (no data loss), then remove the pane.
    // Prefer another pane in the SAME region (so the layers stay in the stack the user is
    // viewing); else fall back to the lowest-index remaining pane (Phase 6.4.2, point 4).
    const int region = m_pane_layout->regionOfPane(idx);
    int targetIdx = -1;
    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (i != idx && m_pane_layout->regionOfPane(i) == region) { targetIdx = i; break; }
    if (targetIdx < 0) targetIdx = (idx == 0) ? 1 : 0;
    const uint64_t targetPid = m_pane_layout->paneId(targetIdx);

    // Was the target empty before the move? If so its camera was never fit — fit it after,
    // so the reassigned layers are actually visible (mirrors assignLayerToPane).
    int targetCountBefore = 0;
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && l->paneId() == targetPid) ++targetCountBefore;
    }
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && l->paneId() == pid) l->setPaneId(targetPid);
    }
    m_pane_layout->removePane(idx);

    // Re-establish the active pane (indices shifted); force a refresh by clearing first.
    const int newActive = std::min(m_active_pane_idx, m_pane_layout->paneCount() - 1);
    m_active_pane_idx = -1;
    setActivePane(std::max(0, newActive));

    // Fit the target canvas to the moved layers if it had none before (else they'd render
    // off-screen and a re-drag onto the same pane would be a no-op).
    if (targetCountBefore == 0) {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == targetPid) {
                if (auto* tc = m_pane_layout->paneCanvas(i)) tc->fitToLayers();
                break;
            }
    }

    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (auto* c = m_pane_layout->paneCanvas(i)) c->update();
    updatePaneLegends();
    if (m_layer_panel) m_layer_panel->refreshPaneColors();   // reassigned layers recolour
}

void MainWindow::renamePane(MapCanvas* canvas) {
    const int idx = m_pane_layout->indexOfCanvas(canvas);
    if (idx < 0) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Edit Pane ID"), tr("Pane name:"), QLineEdit::Normal,
        m_pane_layout->paneLabel(idx), &ok);
    if (!ok || name.isEmpty()) return;
    m_pane_layout->setPaneLabel(idx, name);
    canvas->setPaneLabel(name);
    if (m_layer_panel) m_layer_panel->refreshPanes();   // group header shows the pane label
}

void MainWindow::colorPane(MapCanvas* canvas) {
    const int idx = m_pane_layout->indexOfCanvas(canvas);
    if (idx < 0) return;
    QColor current = m_pane_layout->paneColor(idx);
    if (!current.isValid()) current = Qt::white;
    // Reuses the no-data picker pattern (FR-PNE-6). The chosen base colour is applied at
    // ~50 % alpha behind the layer rows and full strength on the Vis/opacity widgets.
    QColor chosen = QColorDialog::getColor(current, this, tr("Pane Color"));
    if (!chosen.isValid()) return;
    m_pane_layout->setPaneColor(idx, chosen);
    canvas->setPaneColor(chosen);   // border + ID label follow the new colour
    if (m_layer_panel) m_layer_panel->refreshPaneColors();
}

void MainWindow::removeActivePane() {
    if (m_pane_layout->paneCount() <= 1) return;
    m_pane_layout->removePane(m_active_pane_idx);
    setActivePane(std::max(0, m_active_pane_idx - 1));
    if (m_layer_panel) m_layer_panel->refreshPanes();   // drop the removed pane's group
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList rasterPaths;
    QStringList vectorPaths;
    for (const auto& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            QString fn = url.toLocalFile();
            if (fn.endsWith(".shp", Qt::CaseInsensitive)) {
                vectorPaths << fn;
            } else {
                rasterPaths << fn;
            }
        }
    }
    if (!rasterPaths.isEmpty())
        ErrorReporter::runGuarded("Drop-open Raster", [&] { openFiles(rasterPaths); });
    if (!vectorPaths.isEmpty())
        ErrorReporter::runGuarded("Drop-open Vector", [&] { openVectorFiles(vectorPaths); });
}

void MainWindow::onThemeChanged(Theme t) {
    // Every pane's canvas must follow the theme, not just the active one (Phase 6).
    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (auto* c = m_pane_layout->paneCanvas(i))
            c->setDarkBackground(t == Theme::Dark);
    // The default startup pane tracks the theme accent blue (Phase 6.2.1) — resolved by
    // ID, never by position. m_panes is a vector, so closing the default pane shifts every
    // later pane down and index 0 becomes somebody else; re-accenting index 0 then stamped
    // the accent onto an unrelated pane. Repro: add Pane 2, close Pane 1, add Pane 3 (which
    // correctly reclaims the freed palette slot), toggle the theme — Pane 2, now at index 0,
    // turned the default pane's blue. Pane ids are monotonic and never reused, so once the
    // default pane is gone NO pane wears the accent, which is the correct outcome.
    int defaultIdx = -1;
    for (int i = 0; i < m_pane_layout->paneCount(); ++i)
        if (m_pane_layout->paneId(i) == kDefaultPaneId) { defaultIdx = i; break; }
    if (defaultIdx >= 0) {
        const QColor accent = qApp->palette().highlight().color();
        m_pane_layout->setPaneColor(defaultIdx, accent);
        if (auto* c = m_pane_layout->paneCanvas(defaultIdx)) c->setPaneColor(accent);
    }
    if (m_layer_panel) m_layer_panel->refreshPaneColors();
    if (m_log_widget) {
        m_log_widget->clear();
        for (const auto& e : m_log_entries)
            renderLogEntry(e.level, e.text);
        m_log_widget->verticalScrollBar()->setValue(
            m_log_widget->verticalScrollBar()->maximum());
    }
    // Keep an on-screen notification banner theme-compliant across a live theme toggle.
    if (m_error_banner && m_error_banner->isVisible())
        applyBannerStyle(m_banner_level);
}

int MainWindow::topLayerIndexInPane(uint64_t paneId) const {
    if (!m_layer_mgr) return -1;
    // List order is top→bottom, so the lowest index in the pane is its topmost layer.
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && l->paneId() == paneId) return i;
    }
    return -1;
}

FvProfileScope MainWindow::buildProfileScope() const {
    QVector<InspectPaneGroup> groups;
    if (!m_layer_mgr || !m_pane_layout) return groups;

    // Layers-panel SELECTION first (FR-ANL-12, FR-LYR-10). Two or more selected rasters are a
    // deliberate statement of what to compare, so they are profiled together whatever the sync
    // roles say — which is the whole point: comparing layers across panes used to require
    // syncing those panes, i.e. changing how they navigate in order to ask a question about
    // their data. selectedLayerIndices() already folds in every layer of a selected PANE
    // header, so selecting a pane profiles its contents.
    //
    // Hidden rasters are skipped and COUNTED, not silently dropped: every scope rule in the app
    // is written in terms of visible rasters, but the user ticked these deliberately, so the
    // panel says how many were left out.
    if (m_layer_panel) {
        QVector<InspectPaneGroup> sel;
        int hidden = 0, visible = 0;
        // Pane order from the layout, layer order from the manager, so the batch reads in the
        // same order the panels list it.
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            const uint64_t pid = m_pane_layout->paneId(i);
            InspectPaneGroup grp;
            grp.paneId    = pid;
            grp.paneLabel = m_pane_layout->paneLabel(i);
            grp.paneColor = m_pane_layout->paneColorForId(pid);
            for (int idx : m_layer_panel->selectedLayerIndices()) {
                auto l = m_layer_mgr->layerAt(idx);
                if (!l || l->type() != LayerType::Raster || l->paneId() != pid) continue;
                if (!l->visible()) { ++hidden; continue; }
                ++visible;
                grp.layers.push_back(InspectLayerEntry{ l->name(),
                                                        static_cast<RasterLayer*>(l.get()) });
            }
            if (!grp.layers.isEmpty()) sel.push_back(std::move(grp));
        }
        // ONE selected layer is not a batch — it is the ordinary case, and it must keep the
        // sync behaviour below, or selecting a row in a synced pane would quietly narrow the
        // scope the sync group is meant to give it.
        if (visible >= 2) {
            FvProfileScope out;
            out.groups        = std::move(sel);
            out.hiddenSkipped = hidden;
            out.fromSelection = true;
            return out;
        }
    }

    auto active = m_layer_mgr->activeLayer();
    if (!active || active->type() != LayerType::Raster) return groups;
    const uint64_t activePane = active->paneId();

    // Unsynced: the active layer alone, which is what the profile always did — including when
    // it is hidden, since the user chose it explicitly in the Layers panel.
    if (!m_pane_layout->paneSynced(activePane)) {
        InspectPaneGroup grp;
        grp.paneId    = activePane;
        grp.paneColor = m_pane_layout->paneColorForId(activePane);
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == activePane) {
                grp.paneLabel = m_pane_layout->paneLabel(i);
                break;
            }
        grp.layers.push_back(InspectLayerEntry{ active->name(),
                                                static_cast<RasterLayer*>(active.get()) });
        groups.push_back(std::move(grp));
        return groups;
    }

    // Synced: every VISIBLE raster in every pane of the group, merged into one plot — the
    // right-click rule of inspectFromPane, applied to profiles. A hidden layer is excluded for
    // the same reason it is there: its numbers are not on screen to be compared against.
    for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
        const uint64_t pid = m_pane_layout->paneId(i);
        if (!m_pane_layout->paneSynced(pid)) continue;

        InspectPaneGroup grp;
        grp.paneId    = pid;
        grp.paneLabel = m_pane_layout->paneLabel(i);
        grp.paneColor = m_pane_layout->paneColorForId(pid);
        for (int j = 0; j < m_layer_mgr->count(); ++j) {   // list order is top→bottom
            auto l = m_layer_mgr->layerAt(j);
            if (!l || l->paneId() != pid) continue;
            if (l->type() != LayerType::Raster || !l->visible()) continue;
            grp.layers.push_back(InspectLayerEntry{ l->name(),
                                                    static_cast<RasterLayer*>(l.get()) });
        }
        if (!grp.layers.isEmpty()) groups.push_back(std::move(grp));
    }
    return groups;
}

void MainWindow::inspectFromPane(MapCanvas* clicked, double gx, double gy, bool allLayers) {
    if (!clicked || !m_layer_mgr || !m_pane_layout) return;
    const int clickedIdx = m_pane_layout->indexOfCanvas(clicked);
    if (clickedIdx < 0) return;
    const uint64_t clickedId = m_pane_layout->paneId(clickedIdx);

    // Target panes: the whole sync group when the clicked pane is synced, else just it
    // (Phase 6.6). Synced panes share a coordinate space, so one geo point samples them all.
    std::vector<uint64_t> panes;
    if (m_pane_layout->paneSynced(clickedId)) {
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            const uint64_t id = m_pane_layout->paneId(i);
            if (m_pane_layout->paneSynced(id)) panes.push_back(id);
        }
    } else {
        panes.push_back(clickedId);
    }
    auto active = m_layer_mgr->activeLayer();
    QVector<InspectPaneGroup> groups;
    for (uint64_t pid : panes) {
        InspectPaneGroup grp;
        grp.paneId = pid;
        grp.paneColor = m_pane_layout->paneColorForId(pid);
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == pid) { grp.paneLabel = m_pane_layout->paneLabel(i); break; }

        // A hidden layer's pixel value is never shown in the inspector (gates both the
        // left-click representative and the right-click all-layers paths).
        auto addLayer = [&](const std::shared_ptr<Layer>& l) {
            if (!l || l->type() != LayerType::Raster || !l->visible()) return;
            grp.layers.push_back(InspectLayerEntry{ l->name(), static_cast<RasterLayer*>(l.get()) });
        };

        if (allLayers) {
            for (int i = 0; i < m_layer_mgr->count(); ++i) {
                auto l = m_layer_mgr->layerAt(i);
                if (l && l->paneId() == pid && l->visible() && l->type() == LayerType::Raster)
                    addLayer(l);
            }
        } else {
            // The pane's representative layer: the global active layer if it lives here, else
            // the pane's topmost layer (mirrors updatePaneLegends).
            std::shared_ptr<Layer> rep;
            if (active && active->type() == LayerType::Raster && active->paneId() == pid)
                rep = active;
            else
                rep = m_layer_mgr->layerAt(topLayerIndexInPane(pid));
            addLayer(rep);
        }
        groups.push_back(std::move(grp));
    }

    // (gx,gy) are in the clicked pane's Project CRS; pass it so the inspector samples each
    // layer's SOURCE pixel by transforming into the layer's source CRS (Phase 11, FR-CRS-4).
    const std::string geoWkt = clicked->projectCrsWkt();
    m_attr_insp->inspectGroups(gx, gy, geoWkt, groups);
    if (m_numeric_dump)
        m_numeric_dump->inspectGroups(gx, gy, geoWkt, groups);

    // The Spectral Plot is fed the SAME groups (Phase 26), so its curves and the inspector's
    // rows always describe one selection: left-click ⇒ the topmost/representative layer of
    // each pane, right-click ⇒ every visible raster — merged into one plot when the gesture
    // spans more than one layer.
    //
    // Only while the window is OPEN, though: inspect mode is used for the Pixel Inspector far
    // more often than for a spectrum, and silently accumulating plots behind a closed window
    // both costs a raster read per click and means opening it later shows a history the user
    // never asked to build.
    if (m_spectral_panel && m_spectral_panel->isVisible())
        m_spectral_panel->addInspectResult(gx, gy, geoWkt, groups, allLayers);

    // Mirror the red inspect-highlight square onto every target pane at the SAME GROUND POINT
    // (Phase 6.8.3): the clicked pane already self-highlights in MapCanvas::mousePressEvent;
    // this draws it on the synced siblings too (idempotent for the clicked pane). Out-of-bounds
    // points clear that pane's overlay.
    //
    // Project CRS is PER PANE (Phase 11), so a sync group can hold panes in different CRS —
    // the shared camera is already reprojected between them (SyncGroup carries the source
    // WKT). The click, though, arrives in the CLICKED pane's CRS, and was being handed to the
    // siblings unchanged: pane 2 then read metres as degrees and marked a point that could be
    // continents away. Transform per target, and clear rather than guess when the point has no
    // image in that CRS (FR-CRS-2/4).
    for (uint64_t pid : panes)
        for (int i = 0; i < m_pane_layout->paneCount(); ++i)
            if (m_pane_layout->paneId(i) == pid) {
                if (auto* c = m_pane_layout->paneCanvas(i)) {
                    double mx = gx, my = gy;
                    if (fvTransformPoint(geoWkt, c->projectCrsWkt(), mx, my))
                        c->updateHighlightForGeo(mx, my);
                    else
                        c->clearInspectHighlight();
                }
                break;
            }
}

void MainWindow::updatePaneLegends() {
    if (!m_layer_mgr || !m_pane_layout) return;
    for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
        auto* c = m_pane_layout->paneCanvas(i);
        if (!c) continue;
        RasterLayer* rl = nullptr;
        // The colorbar is decoupled from the active layer (Phase 16 #5): it represents the
        // TOPMOST VISIBLE layer in the pane — i.e. the topmost raster that is visible AND has
        // opacity > 0 AND is grayscale (RGB composites have no colorbar). A per-pane "Show
        // Colorbar" gear toggle is the master gate; when off, the legend never shows. When all
        // layers in a pane are hidden/opacity-0, no candidate is found and the legend hides.
        if (c->colorbarVisible()) {
            const uint64_t pid = m_pane_layout->paneId(i);
            for (int j = 0; j < m_layer_mgr->count(); ++j) {   // list order is top→bottom
                auto l = m_layer_mgr->layerAt(j);
                if (!l || l->paneId() != pid) continue;
                if (!l->visible() || l->opacity() <= 0.0f) continue;
                if (l->type() != LayerType::Raster) continue;
                auto* cand = static_cast<RasterLayer*>(l.get());
                if (cand->bandMapping().isGrayscale()) { rl = cand; break; }
            }
        }
        c->colormapLegend()->setLayer(rl);   // nullptr hides the legend
    }
}

void MainWindow::onActiveLayerChanged(int index) {
    if (!m_layer_mgr) return;
    auto layerPtr = m_layer_mgr->layerAt(index);
    RasterLayer* rl = nullptr;
    if (layerPtr && layerPtr->type() == LayerType::Raster)
        rl = static_cast<RasterLayer*>(layerPtr.get());
    else if (layerPtr && layerPtr->type() == LayerType::Vector && m_vector_panel)
        m_vector_panel->configureFor(static_cast<VectorLayer*>(layerPtr.get()));

    // Phase 18 #8: a multi-selection has no single subject — the per-layer property widgets
    // show the same empty state as with no image loaded.
    if (m_multi_select) rl = nullptr;
    refreshLayerProperties(rl);
    updatePaneLegends();

    // Phase 26: the Spectral Plot follows the activated layer — its stored plot if inspect
    // mode has sampled it (the whole merged plot when the layer belongs to one), else a blank
    // chart titled with the layer's name. It is NOT gated on m_multi_select: a merged plot is
    // inherently about several layers, so blanking it on a multi-selection would hide exactly
    // the case it exists for. It IS driven by `index`, not `rl`, so the panel keeps naming
    // the layer even when the per-layer property widgets are blanked.
    if (m_spectral_panel) m_spectral_panel->showLayerPlot(index);
    // The Scan/Pixel Profile follows the activated layer on the same terms since Phase 26.5
    // (FR-ANL-12): its stored profile if one has been computed — the whole merged plot when the
    // layer belongs to one — else a blank chart titled with the layer's name.
    if (m_profile_panel) m_profile_panel->showLayerPlot(index);

    // Reflect the active layer's display-resampling mode in the View menu radios
    // (fall back to the persisted default when there is no active raster layer).
    if (m_resample_group) {
        int mode = rl ? static_cast<int>(rl->displayResampling())
                      : Settings::instance().displayResampling();
        if (mode >= 0 && mode < 3 && m_resample_acts[mode]) {
            QSignalBlocker block(m_resample_group);
            m_resample_acts[mode]->setChecked(true);
        }
    }
}

void MainWindow::refreshLayerProperties(RasterLayer* layer) {
    m_band_sel->setLayer(layer);
    m_cm_sel->setLayer(layer);
    // NOTE: the colormap legend is NOT set here — it is per-pane and bound to each
    // pane's own layer by updatePaneLegends() (a pane's legend must not follow the
    // globally-active layer, which may live in a different pane).
    m_nodata_widget->setLayer(layer);

    // The status-bar CRS readout shows the active pane's PROJECT CRS (Phase 11, FR-GIS-4),
    // not the active layer's source CRS — layers may be reprojected on the fly into it.
    updateProjectCrsStatus();
}

void MainWindow::updateProjectCrsStatus() {
    if (!m_crs_label) return;
    MapCanvas* c = (m_active_pane_idx >= 0 && m_active_pane_idx < m_pane_layout->paneCount())
                       ? m_pane_layout->paneCanvas(m_active_pane_idx) : nullptr;
    if (!c) { m_crs_label->setText("CRS: --"); return; }

    const std::string paneWkt = c->projectCrsWkt();
    // Count this pane's raster layers being reprojected on the fly (source CRS ≠ pane CRS).
    const uint64_t pid = c->paneId();
    int reproj = 0;
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && l->paneId() == pid && l->type() == LayerType::Raster) {
            auto* rl = static_cast<RasterLayer*>(l.get());
            if (rl->dataset() && !fvSameCrsWkt(rl->dataset()->crsWkt(), paneWkt))
                ++reproj;
        }
    }
    QString txt = "CRS: " + fvCrsShortName(paneWkt);
    if (reproj > 0) txt += QString("  ↻%1").arg(reproj);   // ↻N reprojected
    m_crs_label->setText(txt);
    m_crs_label->setToolTip(
        reproj > 0
        ? tr("Project CRS of the active pane — %1 layer(s) reprojected on the fly. "
             "Click to change.").arg(reproj)
        : tr("Project CRS of the active pane. Click to change."));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Closing a plot WINDOW saves its frame and its divider, so it reopens where the user left
    // it rather than where it was first shown. Only MainWindow knows the Settings key, hence
    // the filter; DISCARDING the plots is the panel's own closeEvent (FR-ANL-11), because that
    // rule must hold however the window is closed — including on paths that never reach here.
    if (event->type() == QEvent::Close
        && (watched == m_spectral_panel || watched == m_profile_panel)) {
        auto* w = static_cast<QWidget*>(watched);
        const QVariant key = w->property("fvGeometryKey");
        if (key.isValid()) {
            Settings::instance().setPlotWindowGeometry(key.toString(), w->saveGeometry());
            Settings::instance().setPlotWindowGeometry(key.toString() + kSplitSuffix,
                                                       watched == m_spectral_panel
                                                           ? m_spectral_panel->saveSplitState()
                                                           : m_profile_panel->saveSplitState());
        }
        return QMainWindow::eventFilter(watched, event);
    }
    if (watched == m_crs_label && event->type() == QEvent::MouseButtonRelease) {
        MapCanvas* c = (m_active_pane_idx >= 0 && m_active_pane_idx < m_pane_layout->paneCount())
                           ? m_pane_layout->paneCanvas(m_active_pane_idx) : nullptr;
        if (c) openProjectCrsPicker(c);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::openProjectCrsPicker(MapCanvas* canvas) {
    if (!canvas) return;
    CrsPickerDialog dlg(canvas->projectCrsWkt(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (dlg.useLayerCrs())
        canvas->clearProjectCrsOverride();                       // revert to derived default
    else
        canvas->setProjectCrsWkt(dlg.resultWkt(), /*userInitiated=*/true);
    updateProjectCrsStatus();
}

void MainWindow::renderLogEntry(int level, const QString& text) {
    const int clamped = std::clamp(level, 0, 6);
    auto* app = qobject_cast<Application*>(qApp);
    bool dark = !app || app->currentTheme() == Theme::Dark;
    const char* color = (dark ? kLevelColorsDark : kLevelColorsLight)[clamped];
    for (const QString& line : text.split('\n'))
        m_log_widget->appendHtml(
            QString("<span style=\"color:%1;\">%2</span>")
            .arg(color, line.toHtmlEscaped()));
}

void MainWindow::appendLog(int level, const QString& text) {
    if (!m_log_widget) return;
    m_log_entries.append({level, text});
    if (m_log_entries.size() > 5000) m_log_entries.removeFirst();
    renderLogEntry(level, text);
    m_log_widget->verticalScrollBar()->setValue(
        m_log_widget->verticalScrollBar()->maximum());
}

void MainWindow::exportLogs() {
    // FR-APP-10: write the in-panel log buffer to a user-chosen file. Each entry's
    // text already carries the "[time] [level] message" prefix.
    QString dir = Settings::instance().logExportDir();
    if (dir.isEmpty()) dir = QDir::homePath();
    const QString suggested = dir + "/flashviewer-log.txt";
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Logs"), suggested,
        tr("Text (*.txt *.log);;All files (*)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export Failed"),
            tr("Could not write log to:\n%1").arg(path));
        return;
    }
    QTextStream out(&f);
    for (const LogEntry& e : m_log_entries) out << e.text << '\n';
    f.close();

    Settings::instance().setLogExportDir(QFileInfo(path).absolutePath());
    statusBar()->showMessage(tr("Exported %1 log entries to %2")
                                 .arg(m_log_entries.size()).arg(path), 5000);
    FV_INFO("Exported {} log entries to '{}'",
            m_log_entries.size(), path.toStdString());
}

void MainWindow::applyBannerStyle(int level) {
    if (!m_error_banner) return;
    auto* app = qobject_cast<Application*>(qApp);
    const bool dark = !app || app->currentTheme() == Theme::Dark;
    // Theme-compliant, contrast-safe background+foreground PAIRS per level (Primer subtle-bg
    // + matching fg, ≥ 4.5:1). Both vary by level so red error text never lands on the amber
    // warn background. warn=3, error=4, critical≥5.
    const char* bg;
    const char* fg;
    if (level >= 5) {            // critical → danger-subtle bg + strong danger fg
        bg = dark ? "#2b1a19" : "#ffebe9";
        fg = dark ? "#ff7b72" : "#a40e26";
    } else if (level == 4) {     // error → danger-subtle
        bg = dark ? "#2b1a19" : "#ffebe9";
        fg = dark ? "#f85149" : "#cf222e";
    } else {                     // warn → attention-subtle (amber)
        bg = dark ? "#272115" : "#fff8c5";
        fg = dark ? "#d29922" : "#9a6700";
    }
    m_error_banner->setStyleSheet(
        QString("#ErrorBanner{background:%1;border-left:3px solid %2;}").arg(bg, fg));
    m_error_banner_label->setTextColor(QColor(fg));
}

void MainWindow::showErrorBanner(int level, const QString& text) {
    if (!m_error_banner || level < 3) return;   // info/debug → log only, no banner
    m_banner_level = level;
    applyBannerStyle(level);
    m_error_banner_label->setText(text);
    m_error_banner->show();
    m_banner_timer->start(8000);   // auto-hide after 8 s
}

QPixmap MainWindow::grabLayoutComposite() {
    // Composite each region's FRONT pane at its on-screen geometry; empty regions and the
    // pill strips / splitter gaps are filled with the theme canvas background (matching the
    // single-pane grab), not transparent (Phase 7 follow-up). grabForExport() omits each pane's
    // chrome + border and keeps its visible map furniture (legend/scale bar/highlight).
    const bool dark = qobject_cast<Application*>(qApp)
                          ? qobject_cast<Application*>(qApp)->currentTheme() == Theme::Dark : true;
    const QColor bg = dark ? QColor(0x0d, 0x11, 0x17)   // GitHub Dark  #0d1117
                           : QColor(0xf6, 0xf8, 0xfa);   // GitHub Light #f6f8fa
    const qreal dpr = m_pane_layout->devicePixelRatioF();
    QPixmap composite(QSize(qRound(m_pane_layout->width()  * dpr),
                            qRound(m_pane_layout->height() * dpr)));
    composite.setDevicePixelRatio(dpr);
    composite.fill(bg);
    QPainter p(&composite);
    for (int r = 0; r < m_pane_layout->regionCount(); ++r)
        if (auto* c = m_pane_layout->frontCanvasInRegion(r))
            p.drawPixmap(c->mapTo(m_pane_layout, QPoint(0, 0)), c->grabForExport());
    p.end();
    return composite;
}

void MainWindow::captureScreenshot() {
    QPixmap px;
    // Multi-pane: let the user choose what to capture (Phase 7 follow-up). A single pane skips
    // the prompt (both modes would produce the same image).
    if (m_pane_layout->paneCount() > 1) {
        const QStringList modes{ tr("Active pane"), tr("Entire pane layout") };
        bool ok = false;
        const QString mode = QInputDialog::getItem(
            this, tr("Screenshot"), tr("Capture:"), modes, 0, /*editable=*/false, &ok);
        if (!ok) return;
        px = (mode == modes[1]) ? grabLayoutComposite() : m_canvas->grabForExport();
    } else {
        px = m_canvas->grabForExport();       // omit pane chrome (ID+gear) and border
    }

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Screenshot"),
        QDir::homePath() + "/screenshot.png",
        tr("PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif *.tiff)"));
    if (path.isEmpty()) return;
    if (!px.save(path))
        QMessageBox::warning(this, tr("Save Failed"),
            tr("Could not save screenshot to:\n%1").arg(path));
}

void MainWindow::openVectorFiles(const QStringList& paths, uint64_t targetPaneId) {
    uint64_t pid = (targetPaneId == 0) ? activePaneId() : targetPaneId;
    MapCanvas* targetCanvas = nullptr;
    QString targetLabel = tr("Pane %1").arg(pid);
    for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
        if (m_pane_layout->paneId(i) == pid) {
            targetCanvas = m_pane_layout->paneCanvas(i);
            targetLabel = m_pane_layout->paneLabel(i);
            break;
        }
    }

    // Check if the target pane contains an unreferenced raster dataset with no CRS
    bool targetHasUnreferencedRaster = false;
    QString unrefRasterName;
    for (int i = 0; i < m_layer_mgr->count(); ++i) {
        auto l = m_layer_mgr->layerAt(i);
        if (l && fvLayerInPane(*l, pid) && l->type() == LayerType::Raster) {
            auto* rl = static_cast<RasterLayer*>(l.get());
            if (!rl->dataset() || rl->dataset()->crsWkt().empty()) {
                targetHasUnreferencedRaster = true;
                unrefRasterName = rl->name();
                break;
            }
        }
    }

    if (targetHasUnreferencedRaster) {
        auto res = QMessageBox::warning(
            this, tr("No CRS on Target Pane"),
            tr("The target %1 contains raster dataset '%2' with no Coordinate Reference System (CRS).\n\n"
               "Adding vector layers to an unreferenced pane is discouraged because vector features cannot be spatially aligned with pixel-space data.\n\n"
               "Would you like to add this vector layer to a new pane instead?")
                .arg(targetLabel).arg(unrefRasterName),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::Yes);

        if (res == QMessageBox::Cancel) {
            return;
        } else if (res == QMessageBox::Yes) {
            auto* newCanvas = addPane();
            if (newCanvas) {
                targetCanvas = newCanvas;
                pid = newCanvas->paneId();
                targetLabel = m_pane_layout->paneLabel(m_pane_layout->indexOfCanvas(newCanvas));
            }
        }
    }

    const std::string targetCrs = targetCanvas ? targetCanvas->projectCrsWkt() : std::string();

    for (const QString& path : paths) {
        const QString fname = QFileInfo(path).fileName();
        auto ds = runWithCancelDialog(
            tr("Loading Vector Layer"),
            tr("Reading vector dataset '%1'…").arg(fname),
            [&](std::atomic<bool>& cancel) {
                return VectorDataset::open(path.toStdString(), &cancel);
            });

        if (!ds) {
            continue;
        }

        // Always convert vector layer to dataset CRS before rendering if target pane has a dataset CRS
        if (!targetCrs.empty() && targetCrs != ds->crsWkt() && !ds->crsWkt().empty()) {
            std::string reason;
            if (!ds->canReprojectTo(targetCrs, &reason)) {
                QMessageBox::warning(this, tr("CRS Conversion Failed"),
                    tr("Cannot load vector layer '%1' into %2.\n\n"
                       "Reason: %3\n\n"
                       "Vector Layer CRS: %4\n"
                       "Target Dataset CRS: %5")
                    .arg(QFileInfo(path).fileName())
                    .arg(targetLabel)
                    .arg(QString::fromStdString(reason))
                    .arg(fvCrsShortName(ds->crsWkt()))
                    .arg(fvCrsShortName(targetCrs)));
                continue;
            }

            // Convert / pre-project geometries with progress dialog
            QProgressDialog progress(
                tr("Converting vector layer '%1' to target CRS (%2)…")
                    .arg(QFileInfo(path).fileName())
                    .arg(fvCrsShortName(targetCrs)),
                tr("Cancel"), 0, 100, this);
            progress.setWindowModality(Qt::ApplicationModal);
            progress.setMinimumDuration(150);
            progress.setValue(0);

            std::function<bool(int, int)> progressCb = [&](int cur, int tot) -> bool {
                progress.setValue(cur * 100 / std::max(1, tot));
                QApplication::processEvents();
                return !progress.wasCanceled();
            };

            auto geoms = ds->geometriesForCrs(targetCrs, progressCb);

            if (progress.wasCanceled() || !geoms) {
                if (progress.wasCanceled()) {
                    showErrorBanner(2, tr("Vector layer loading canceled by user."));
                } else {
                    QMessageBox::warning(this, tr("CRS Conversion Failed"),
                        tr("Vector geometry coordinate conversion failed for '%1'.").arg(path));
                }
                continue;
            }
        }

        auto layer = std::make_shared<VectorLayer>(ds);
        layer->setPaneId(pid);
        m_layer_mgr->addLayer(layer);
        FV_INFO("Opened vector layer '{}' in pane {}", path.toStdString(), pid);
    }
    if (m_canvas) m_canvas->update();
}

void MainWindow::setOsmBasemapEnabled(bool on) {
    const std::string osmCrsWkt = fvGetEpsgWkt(3857);
    const QString osmCrsName = fvCrsShortName(osmCrsWkt);

    if (on) {
        // When loading/displaying datasets with no CRS, OSM basemap selection should throw an error dialog box
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            auto* c = m_pane_layout->paneCanvas(i);
            if (!c) continue;
            uint64_t pid = c->paneId();
            for (int li = 0; li < m_layer_mgr->count(); ++li) {
                auto l = m_layer_mgr->layerAt(li);
                if (!l || !fvLayerInPane(*l, pid) || l->type() != LayerType::Raster) continue;
                auto* rl = static_cast<RasterLayer*>(l.get());
                if (!rl->dataset() || rl->dataset()->crsWkt().empty()) {
                    QMessageBox::critical(
                        this, tr("Cannot Activate OSM Basemap"),
                        tr("Cannot activate OpenStreetMap Basemap:\n\n"
                           "The raster dataset '%1' in %2 has no Coordinate Reference System (CRS).\n\n"
                           "OSM Basemap requires georeferenced data with a valid spatial reference.")
                            .arg(rl->name())
                            .arg(m_pane_layout->paneLabel(i)));
                    if (m_act_osm) {
                        QSignalBlocker blocker(m_act_osm);
                        m_act_osm->setChecked(false);
                    }
                    return;
                }
            }
        }

        showErrorBanner(1, tr("OSM Basemap activated: Pane CRS set to %1 (Web Mercator). Reprojecting vector and raster layers…").arg(osmCrsName));

        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            auto* c = m_pane_layout->paneCanvas(i);
            if (!c) continue;

            // Remember previous CRS for this pane
            if (!c->hasPreOsmCrs()) {
                c->setPreOsmCrs(c->projectCrsWkt());
            }

            uint64_t pid = c->paneId();

            // Reproject all vector layers on this pane
            for (int li = 0; li < m_layer_mgr->count(); ++li) {
                auto l = m_layer_mgr->layerAt(li);
                if (!l || !fvLayerInPane(*l, pid) || l->type() != LayerType::Vector) continue;

                auto* vl = static_cast<VectorLayer*>(l.get());
                if (!vl->dataset()) continue;

                if (!vl->dataset()->canReprojectTo(osmCrsWkt)) {
                    FV_WARN("Vector layer '{}' cannot be reprojected to {}", vl->name().toStdString(), osmCrsName.toStdString());
                    continue;
                }

                // Convert with progress bar
                QProgressDialog progress(
                    tr("Converting vector layer '%1' to %2…").arg(vl->name(), osmCrsName),
                    tr("Cancel"), 0, 100, this);
                progress.setWindowModality(Qt::ApplicationModal);
                progress.setMinimumDuration(150);
                progress.setValue(0);

                std::function<bool(int, int)> progressCb = [&](int cur, int tot) -> bool {
                    progress.setValue(cur * 100 / std::max(1, tot));
                    QApplication::processEvents();
                    return !progress.wasCanceled();
                };

                vl->dataset()->geometriesForCrs(osmCrsWkt, progressCb);
            }

            // Set pane project CRS to EPSG:3857 (triggers raster tile reprojection & camera reprojection)
            c->setProjectCrsWkt(osmCrsWkt, /*userInitiated=*/false);
            c->osmRenderer()->setEnabled(true);

            if (pid && m_layer_mgr->count() == 0) {
                c->resetToWorldView();
            }
            c->update();
        }
    } else {
        // Revert OSM Basemap
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            auto* c = m_pane_layout->paneCanvas(i);
            if (!c) continue;

            c->osmRenderer()->setEnabled(false);

            if (c->hasPreOsmCrs()) {
                std::string prevCrs = c->preOsmCrs();
                c->clearPreOsmCrs();
                if (!prevCrs.empty()) {
                    c->setProjectCrsWkt(prevCrs, /*userInitiated=*/false);
                } else {
                    c->clearProjectCrsOverride();
                    c->refreshDerivedProjectCrs();
                }
            } else {
                c->clearProjectCrsOverride();
                c->refreshDerivedProjectCrs();
            }
            c->update();
        }

        showErrorBanner(1, tr("OSM Basemap removed: Restored previous dataset CRS."));
    }

    updateProjectCrsStatus();
    updatePaneLegends();
    if (m_layer_panel) m_layer_panel->refreshPaneColors();
}

void MainWindow::showSettingsDialog() {
    SettingsDialog dlg(this);
    connect(&dlg, &SettingsDialog::themeChanged, this, [this] {
        onThemeChanged(Settings::instance().theme());
    });
    connect(&dlg, &SettingsDialog::settingsApplied, this, [this] {
        const QString osmUrl = Settings::instance().osmTileUrl();
        for (int i = 0; i < m_pane_layout->paneCount(); ++i) {
            if (auto* c = m_pane_layout->paneCanvas(i)) {
                if (c->osmRenderer() && c->osmRenderer()->provider()) {
                    c->osmRenderer()->provider()->setUrlTemplate(osmUrl);
                }
                c->update();
            }
        }
        if (m_numeric_dump) m_numeric_dump->update();
    });
    dlg.exec();
}

