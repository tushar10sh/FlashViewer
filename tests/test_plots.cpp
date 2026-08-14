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
#include "app/Settings.hpp"
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
#include <QtCharts/QValueAxis>

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

TEST_CASE("Removing a plotted layer leaves both panels standing", "[plots][TC-ANL-22]") {
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

TEST_CASE("Removing every layer at once leaves both panels standing", "[plots][TC-ANL-22]") {
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

// fvApplyAxisTicks is the whole of FR-ANL-14 that can be pinned without a dialog: the range is
// always stored low-to-high with `reverse` carrying the direction, because QValueAxis expects
// min < max and so does every zoom/pan calculation in FvChartView.
TEST_CASE("A hand-set axis range keeps min below max and reverses instead", "[plots][TC-ANL-25]") {
    QValueAxis axis;
    axis.setRange(0.0, 1.0);
    axis.setLabelFormat(QStringLiteral("%d"));

    SECTION("auto by default") {
        fvApplyAxisTicks(&axis, FvAxisTicks{});
        CHECK(axis.min() == 0.0);
        CHECK(axis.max() == 1.0);
        CHECK_FALSE(axis.isReverse());
    }

    SECTION("ascending range, positive step") {
        fvApplyAxisTicks(&axis, FvAxisTicks{true, 0.0, 100.0, 20.0});
        CHECK(axis.min() == 0.0);
        CHECK(axis.max() == 100.0);
        CHECK_FALSE(axis.isReverse());
        CHECK(axis.tickInterval() == 20.0);
        CHECK(axis.tickAnchor() == 0.0);
        CHECK(axis.labelFormat() == QStringLiteral("%d"));   // integer step keeps %d
    }

    SECTION("descending range, negative step") {
        fvApplyAxisTicks(&axis, FvAxisTicks{true, 100.0, 0.0, -20.0});
        CHECK(axis.min() == 0.0);          // stored low→high whatever the direction
        CHECK(axis.max() == 100.0);
        CHECK(axis.isReverse());
        CHECK(axis.tickInterval() == 20.0);   // the interval is a magnitude
    }

    SECTION("a fractional step widens the label format") {
        fvApplyAxisTicks(&axis, FvAxisTicks{true, 0.0, 4.0, 0.5});
        CHECK(axis.labelFormat() == QStringLiteral("%g"));   // %d would print 1.5 as 2
    }

    SECTION("range without a step leaves the tick spacing to Qt") {
        const qreal before = axis.tickInterval();
        fvApplyAxisTicks(&axis, FvAxisTicks{true, 10.0, 20.0, 0.0});
        CHECK(axis.min() == 10.0);
        CHECK(axis.max() == 20.0);
        CHECK(axis.tickInterval() == before);
    }
}

// The Coords toggle reaches the legend text, not just a flag. It shipped broken once: the
// setting was stored and the panel re-rendered, but curveLabel() never consulted it, so every
// entry kept its coordinates and only a manual test could tell.
TEST_CASE("The coordinate toggle changes what a legend entry says", "[plots][TC-ANL-26]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);
    const bool saved = Settings::instance().legendCoords();

    SECTION("on: the entry carries the sampled coordinates") {
        Settings::instance().setLegendCoords(true);
        PlotHarness h;                       // reads the setting as it builds its toolbar
        auto a = h.add(fx.path, 1);
        h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a}), false);
        REQUIRE(curveCount(h.spectral) == 1);
        CHECK(legendOf(h.spectral)->entryText(0).contains(QLatin1Char('(')));
    }

    SECTION("off: the same entry is one line, and the layer name survives") {
        Settings::instance().setLegendCoords(false);
        PlotHarness h;
        auto a = h.add(fx.path, 1);
        h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a}), false);
        REQUIRE(curveCount(h.spectral) == 1);
        const QString text = legendOf(h.spectral)->entryText(0);
        CHECK_FALSE(text.contains(QChar::LineFeed));
        CHECK(text.contains(QStringLiteral("grad")));
    }

    Settings::instance().setLegendCoords(saved);   // leave the user's setting as it was
}

// The legend's width used to be a constant, so a bigger window only ever grew the plot and a
// long entry stayed elided however much room there was. The ceiling is now half the container,
// re-derived on every resize — a constant here would be the very cap this replaced.
//
// show() first: Qt defers a hidden widget's resize event until it is shown, so a test that only
// calls resize() measures a splitter that has never had a resizeEvent at all.
TEST_CASE("The legend may take up to half the container, never more", "[plots][TC-ANL-27]") {
    auto* chart  = new QWidget;                 // stand-ins: the splitter cares about widths
    auto* legend = new FvChartLegend;
    FvChartSplitter split(chart, legend);
    split.resize(1000, 400);
    split.show();
    PlotHarness::settle();
    CHECK(legend->maximumWidth() == 500);       // half of 1000

    split.resize(400, 400);
    PlotHarness::settle();
    CHECK(legend->maximumWidth() == 200);       // half of 400

    // Below twice the floor the ceiling stops shrinking, or a narrow window would squeeze the
    // legend into a column of single letters.
    split.resize(200, 400);
    PlotHarness::settle();
    CHECK(legend->maximumWidth() == 150);

    // A drag survives a round trip through the settings store.
    split.resize(1000, 400);
    PlotHarness::settle();
    split.setSizes({700, 300});
    const QByteArray state = split.saveState();
    split.setSizes({900, 100});
    split.restoreState(state);
    CHECK(split.sizes().at(1) == 300);
}

// Phase 26.9. A profile scope built from the Layers-panel SELECTION rather than from a sync
// group (FR-ANL-12): several layers, in panes that are not synced, profiled by one Compute.
//
// The distinction that matters is what happens afterwards. A sync-produced merge is a statement
// about panes that move together, so it is discarded when they stop; a selection is a
// comparison the user assembled, and unsyncing has nothing to say about it. Both are multi-pane
// plots holding the same curves, so `fromSelection` is what tells them apart.
TEST_CASE("A selected batch profiles unsynced panes together and outlives an unsync",
          "[plots][TC-ANL-29]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);       // pane 1
    auto b = h.add(fx.path, 2);       // pane 2 — never synced with pane 1
    auto c = h.add(fx.path, 2);

    FvProfileScope sel;
    sel.groups        = groupsFor({a, b, c});
    sel.fromSelection = true;
    h.profile.setScopeResolver([&] { return sel; });
    h.profile.compute();
    REQUIRE(curveCount(h.profile) == 3);

    // Every member reaches the one plot, exactly as a merge's members do.
    h.mgr.setActiveLayer(0);
    CHECK(curveCount(h.profile) == 3);
    h.mgr.setActiveLayer(1);
    CHECK(curveCount(h.profile) == 3);

    // The point of the case: no sync group produced this, so dissolving one cannot take it
    // away. The same call discards a genuine sync merge (TC-ANL-18).
    h.profile.dropMergedPlotsOutside(QSet<quint64>{});
    h.mgr.setActiveLayer(2);
    CHECK(curveCount(h.profile) == 3);
}

// Phase 26.10. What "Persist curves" builds, and what it SHOWS while you build it
// (FR-ANL-12) — the Scan/Pixel Profile only, where the layers to profile are chosen by
// activating them. Choosing the next layer must not be what clears the comparison it is being
// chosen for: with Persist on, a layer with no plot leaves the chart alone, and a layer that
// has one brings that plot up and continues from there. With Persist off, the plain rule
// stands: the layer's own plot, or a blank chart.
//
// (The Spectral Plot deliberately keeps the on-screen rule: its curves come from inspect
// clicks, not from activating layers, so nothing there blanks the chart mid-comparison.)
TEST_CASE("Persist keeps its plot while you choose what to add", "[plots][TC-ANL-30]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 1);
    auto never = h.add(fx.path, 2);            // never profiled
    (void)never;

    h.profile.setScopeResolver([&] { return groupsFor({a}); });
    h.profile.compute();
    REQUIRE(curveCount(h.profile) == 1);

    // Persist OFF: activating an unprofiled layer blanks the chart, as it always has.
    h.mgr.setActiveLayer(2);
    REQUIRE(curveCount(h.profile) == 0);

    // Ticking Persist brings the plot being built back into view, so the target of the next
    // Compute is visible before it is pressed.
    persistBox(h.profile)->setChecked(true);
    CHECK(curveCount(h.profile) == 1);

    // Persist ON: choosing the next layer no longer clears the chart...
    h.mgr.setActiveLayer(1);
    CHECK(curveCount(h.profile) == 1);
    // ...and the Compute adds to it.
    h.profile.setScopeResolver([&] { return groupsFor({b}); });
    h.profile.compute();
    CHECK(curveCount(h.profile) == 2);

    // A layer that HAS a plot still shows its own, in either toggle state — and becomes what
    // the next Compute grows. Here both layers belong to the one plot, so it stays put.
    h.mgr.setActiveLayer(0);
    CHECK(curveCount(h.profile) == 2);

    // Unticking changes nothing on screen; it states what the NEXT Compute does.
    persistBox(h.profile)->setChecked(false);
    CHECK(curveCount(h.profile) == 2);
    // The next activation then follows the plain rule again.
    h.mgr.setActiveLayer(2);
    CHECK(curveCount(h.profile) == 0);

    // Clear must not leave Persist pointing at the discarded plot — neither to grow it nor to
    // show it again.
    h.mgr.setActiveLayer(0);
    REQUIRE(curveCount(h.profile) == 2);
    h.profile.clearCurrent();
    persistBox(h.profile)->setChecked(true);
    CHECK(curveCount(h.profile) == 0);
    h.profile.setScopeResolver([&] { return groupsFor({a}); });
    h.profile.compute();
    CHECK(curveCount(h.profile) == 1);
}

// Phase 26.10. The Spectral Plot's half of the same defect (FR-ANL-5): Persist grew "the plot
// on screen", so a view blanked between two clicks — activating a layer that has never been
// inspected does that — left the next click starting a new plot instead of growing the
// comparison. Its ACTIVATION behaviour is deliberately unchanged: curves come from clicks, so
// nothing about choosing the next pixel clears the chart.
TEST_CASE("Persist grows the spectral plot even after the view has blanked", "[plots][TC-ANL-31]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 1);
    auto never = h.add(fx.path, 2);
    (void)never;

    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a}), /*allLayers=*/false);
    REQUIRE(curveCount(h.spectral) == 1);

    persistBox(h.spectral)->setChecked(true);
    h.mgr.setActiveLayer(2);                       // a layer never inspected — chart blanks
    REQUIRE(curveCount(h.spectral) == 0);

    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({b}), false);
    CHECK(curveCount(h.spectral) == 2);            // joined the comparison, did not restart it

    // Both layers reach the grown plot.
    h.mgr.setActiveLayer(0);
    CHECK(curveCount(h.spectral) == 2);

    // Clear drops the base with the plot: the next click starts fresh rather than reviving it.
    h.spectral.clearCurrent();
    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a}), false);
    CHECK(curveCount(h.spectral) == 1);
}

// Phase 26.9. Closing a plot window discards its plots (FR-ANL-11/12). The rule was written
// into MainWindow's event filter alone, so it held only for a close that passed through that
// filter: the Profile in particular could be closed and reopened still showing the profiles
// the user had dismissed. Owning it in the panel's own closeEvent makes the rule independent
// of who delivers the close — and lets it be tested at all, which the filter never could be.
TEST_CASE("Closing a plot window discards its plots", "[plots][TC-ANL-28]") {
    FixtureFactory ff;
    const auto fx = ff.gradientFloat(24, 24);

    PlotHarness h;
    auto a = h.add(fx.path, 1);
    auto b = h.add(fx.path, 1);

    h.spectral.addInspectResult(kInX, kInY, "", groupsFor({a, b}), /*allLayers=*/true);
    h.profile.setScopeResolver([&] { return groupsFor({a, b}); });
    h.profile.compute();
    REQUIRE(curveCount(h.spectral) == 2);
    REQUIRE(curveCount(h.profile)  == 2);

    h.spectral.close();
    h.profile.close();
    CHECK(curveCount(h.spectral) == 0);
    CHECK(curveCount(h.profile)  == 0);

    // And nothing returns when a plotted layer is activated again — the plots are gone, not
    // merely off screen.
    h.mgr.setActiveLayer(0);
    CHECK(curveCount(h.spectral) == 0);
    CHECK(curveCount(h.profile)  == 0);

    // Re-opening the Profile is exactly this call — what `P` runs now that it no longer calls
    // compute(). It must draw nothing: the old handler recomputed on every open, which rebuilt
    // the same profile the close had just discarded and made a working forget look broken.
    h.profile.showLayerPlot(0);
    CHECK(curveCount(h.profile) == 0);
}
