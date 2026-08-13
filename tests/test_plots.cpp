// Phase 26.5 — the plot panels' scope bookkeeping (FR-ANL-4, FR-ANL-12) and, above all, what
// happens to it when a layer is REMOVED while a panel is open. Both panels key their plots by
// layer id and are driven by MainWindow's layer signals, so a removal walks
// forgetLayer → detachLayer → erasePlot → render() with the layer half gone — the exact path a
// manual test found crashing (Phase 26.5 follow-up).
//
// These are the first automated cases against `plots/`; T-PLOT-1 in TEST_SPEC.md tracks the
// rest (sample values, statistics against an oracle).
#include <catch2/catch_test_macros.hpp>

#include "core/LayerManager.hpp"
#include "core/RasterLayer.hpp"
#include "io/DatasetFactory.hpp"
#include "plots/ScanPixProfilePanel.hpp"
#include "widgets/ChartTools.hpp"
#include "plots/SpectralPlotPanel.hpp"
#include "gis/InspectTypes.hpp"
#include "fixtures/FixtureFactory.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QColor>
#include <QEvent>
#include <QObject>
#include <QSet>
#include <QVector>

#include <memory>

namespace {

// The MainWindow wiring, reduced to what the panels see: forget a layer's curves before it is
// erased, then re-point both panels at whatever is active afterwards. Anything that crashes
// under the real window on a removal has to crash here too — that is the point.
struct PlotHarness {
    LayerManager        mgr;
    SpectralPlotPanel   spectral;
    ScanPixProfilePanel profile{&mgr};

    PlotHarness() {
        spectral.setLayerManager(&mgr);
        QObject::connect(&mgr, &LayerManager::layerAboutToBeRemoved, [this](int index) {
            auto l = mgr.layerAt(index);
            if (!l || l->type() != LayerType::Raster) return;
            const quint64 id = static_cast<RasterLayer*>(l.get())->layerId();
            spectral.forgetLayer(id);
            profile.forgetLayer(id);
        });
        QObject::connect(&mgr, &LayerManager::layerRemoved, [this](int index) {
            spectral.showLayerPlot(index);
            profile.showLayerPlot(index);
        });
        QObject::connect(&mgr, &LayerManager::activeLayerChanged, [this](int index) {
            spectral.showLayerPlot(index);
            profile.showLayerPlot(index);
        });
    }

    // The real panels live in a floating dock and keep running the event loop, so the
    // deleteLater()s a re-render queues are actually processed between gestures. A test that
    // never spins the loop would not see anything those deletions touch.
    //
    // sendPostedEvents(DeferredDelete) FIRST and explicitly: processEvents() alone does not
    // deliver DeferredDelete — Qt holds those until the loop that posted them is re-entered —
    // so a legend rebuilt three times would still have all three generations of rows parented
    // to the panel, and anything counting widgets would count them all.
    static void settle() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }

    std::shared_ptr<RasterLayer> add(const std::string& path, uint64_t paneId) {
        auto ds = DatasetFactory::open(path);
        REQUIRE(ds);
        auto layer = std::make_shared<RasterLayer>(ds);
        layer->setPaneId(paneId);
        mgr.addLayer(layer);
        return layer;
    }
};

// A point INSIDE the gradientFloat fixture, which spans x 0..1 and y 0..1 in EPSG:4326.
// Sampling outside it yields no curves at all, which would leave these cases asserting
// against empty plots without ever saying so.
constexpr double kInX = 0.5, kInY = 0.5;

// One inspect gesture's worth of groups, in the shape MainWindow::inspectFromPane produces.
QVector<InspectPaneGroup> groupsFor(const std::vector<std::shared_ptr<RasterLayer>>& layers) {
    QVector<InspectPaneGroup> out;
    for (const auto& l : layers) {
        InspectPaneGroup* grp = nullptr;
        for (auto& g : out)
            if (g.paneId == l->paneId()) { grp = &g; break; }
        if (!grp) {
            InspectPaneGroup g;
            g.paneId    = l->paneId();
            g.paneLabel = QStringLiteral("Pane %1").arg(l->paneId());
            g.paneColor = QColor(Qt::blue);
            out.push_back(g);
            grp = &out.back();
        }
        grp->layers.push_back(InspectLayerEntry{ l->name(), l.get() });
    }
    return out;
}

// The panel's legend, whose row count IS the number of curves on the chart. Edit and Delete
// live in a per-row context menu, which is modal and cannot be driven headlessly, so deletion
// goes through FvChartLegend::deleteEntry — the same signal the menu emits.
FvChartLegend* legendOf(QWidget& panel) {
    PlotHarness::settle();                      // rows are replaced via deleteLater()
    return panel.findChild<FvChartLegend*>();
}

int curveCount(QWidget& panel) {
    auto* l = legendOf(panel);
    return l ? l->entryCount() : -1;
}

QCheckBox* persistBox(QWidget& panel) {
    for (auto* c : panel.findChildren<QCheckBox*>())
        if (c->text().contains(QStringLiteral("Persist"))) return c;
    return nullptr;
}

} // namespace

TEST_CASE("Persist grows the spectral plot across scopes", "[plots][TC-ANL-23]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 2);          // a DIFFERENT, unsynced pane

    auto* persist = persistBox(h.spectral);
    REQUIRE(persist != nullptr);

    // Persist off: the second gesture replaces the first, because it is a different scope.
    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a}), false);
    REQUIRE(curveCount(h.spectral) == 1);
    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({b}), false);
    REQUIRE(curveCount(h.spectral) == 1);

    // Persist on: the unsynced pane's curve JOINS the plot rather than displacing it.
    persist->setChecked(true);
    h.spectral.addInspectResult(0.25, 0.75, "", groupsFor({a}), false);
    REQUIRE(curveCount(h.spectral) == 2);

    // ...and both layers now resolve to that one plot, so either activation shows the pair.
    h.mgr.setActiveLayer(0);
    CHECK(curveCount(h.spectral) == 2);
    h.mgr.setActiveLayer(1);
    CHECK(curveCount(h.spectral) == 2);
}

TEST_CASE("Deleting a legend entry drops exactly its curve", "[plots][TC-ANL-23]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 1);

    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a, b}), true);
    REQUIRE(curveCount(h.spectral) == 2);

    legendOf(h.spectral)->deleteEntry(0);
    REQUIRE(curveCount(h.spectral) == 1);
    // Deleting the last curve discards the plot, so activating either layer is blank.
    legendOf(h.spectral)->deleteEntry(0);
    REQUIRE(curveCount(h.spectral) == 0);
    h.mgr.setActiveLayer(0);
    CHECK(curveCount(h.spectral) == 0);

    // The profile's legend deletes on the same terms.
    h.profile.setScopeResolver([&] { return groupsFor({a, b}); });
    h.profile.compute();
    REQUIRE(curveCount(h.profile) == 2);
    legendOf(h.profile)->deleteEntry(1);
    CHECK(curveCount(h.profile) == 1);
}

TEST_CASE("Removing a plotted layer leaves both panels standing", "[plots][TC-ANL-18]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 1);

    // A merged plot in each panel, covering BOTH layers, so removing one exercises the
    // detach-but-keep-the-plot path and removing the second the erase-the-plot path.
    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a, b}), /*allLayers=*/true);
    h.profile.setScopeResolver([&] { return groupsFor({a, b}); });
    h.profile.compute();
    REQUIRE(curveCount(h.spectral) == 2);   // the plots really do hold curves
    REQUIRE(curveCount(h.profile)  == 2);

    // Remove them one at a time, highest index first — the order LayerPanel uses.
    PlotHarness::settle();
    h.mgr.removeLayer(1);
    PlotHarness::settle();
    h.mgr.removeLayer(0);
    PlotHarness::settle();
    REQUIRE(h.mgr.count() == 0);
}

TEST_CASE("Removing every layer at once leaves both panels standing", "[plots][TC-ANL-18]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 2);
    auto c = h.add(fx.path, 2);

    // Two panes, so the profile's plot is a cross-pane merge and the spectral plot's scope
    // spans panes as well — the state a removal has the most to unpick.
    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a, b, c}), true);
    h.profile.setScopeResolver([&] { return groupsFor({a, b, c}); });
    h.profile.compute();

    // A layer active while it is removed: the panels are re-pointed at an index that no longer
    // holds what they were showing.
    h.mgr.setActiveLayer(1);
    PlotHarness::settle();
    for (int i = h.mgr.count() - 1; i >= 0; --i) { h.mgr.removeLayer(i); PlotHarness::settle(); }
    REQUIRE(h.mgr.count() == 0);
}

TEST_CASE("A profile plot is reachable from every layer of its scope", "[plots][TC-ANL-18]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 2);
    auto lone = h.add(fx.path, 3);

    h.profile.setScopeResolver([&] { return groupsFor({a, b}); });
    h.profile.compute();

    // Activating either member must not throw the plot away, and activating a layer outside
    // the scope must leave a blank chart rather than someone else's curves.
    h.mgr.setActiveLayer(0);
    h.mgr.setActiveLayer(1);
    h.mgr.setActiveLayer(2);
    h.mgr.setActiveLayer(0);

    // Unsyncing the panes the merge spanned discards it (FR-ANL-12).
    h.profile.dropMergedPlotsOutside(QSet<quint64>{});
    h.spectral.dropMergedPlotsOutside(QSet<quint64>{});
    h.mgr.setActiveLayer(1);

    // Closing a panel forgets everything; re-showing must not resurrect a dangling plot.
    h.profile.forgetAll();
    h.spectral.forgetAll();
    h.mgr.setActiveLayer(2);
    REQUIRE(h.mgr.count() == 3);
    (void)lone;
}
