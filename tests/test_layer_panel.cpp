// Phase 18 — Layer Panel overhaul (#6 pane grouping, #8 multi-select/delete, #1 in-panel
// drag). The panel's logic lives in free functions in panels/LayerGrouping.hpp so it is
// unit-testable without a QTreeWidget or an OpenGL canvas, matching the fvFilterPane /
// fvPaneColor precedent — TC-LYR-07 … TC-LYR-10.
#include <catch2/catch_test_macros.hpp>
#include "core/Layer.hpp"
#include "core/LayerManager.hpp"
#include "panels/LayerGrouping.hpp"
#include "render/Pane.hpp"          // fvNextPaneLabel (pane naming, TC-PNE-11)

#include <memory>
#include <vector>

namespace {
// Minimal concrete Layer so pane grouping can be exercised without a GDAL dataset.
class StubLayer : public Layer {
public:
    LayerType type() const override { return LayerType::Raster; }
};

std::shared_ptr<Layer> makeLayer(uint64_t paneId, const QString& name = "L") {
    auto l = std::make_shared<StubLayer>();
    l->setPaneId(paneId);
    l->setName(name);
    return l;
}
} // namespace

TEST_CASE("fvGroupLayersByPane groups in pane order, keeping list order within a pane",
          "[layerpanel][TC-LYR-07]") {
    std::vector<std::shared_ptr<Layer>> all{
        makeLayer(1, "a"), makeLayer(2, "b"), makeLayer(1, "c"),
        makeLayer(3, "d"), makeLayer(2, "e"),
    };
    const auto groups = fvGroupLayersByPane(all, {1, 2, 3});

    REQUIRE(groups.size() == 3);
    REQUIRE(groups[0].paneId == 1);
    REQUIRE(groups[1].paneId == 2);
    REQUIRE(groups[2].paneId == 3);
    // Within a group the LayerManager order (top → bottom = draw order) is preserved.
    REQUIRE(groups[0].layerIndices == std::vector<int>{0, 2});
    REQUIRE(groups[1].layerIndices == std::vector<int>{1, 4});
    REQUIRE(groups[2].layerIndices == std::vector<int>{3});

    // Every layer appears exactly once across the groups (the grouping is a partition).
    size_t total = 0;
    for (const auto& g : groups) total += g.layerIndices.size();
    REQUIRE(total == all.size());
}

TEST_CASE("fvGroupLayersByPane keeps empty panes visible and never drops an orphan layer",
          "[layerpanel][TC-LYR-07]") {
    // User decision: a pane with no layers still gets a group, so it stays visible as a drop
    // target and can be closed from the panel.
    std::vector<std::shared_ptr<Layer>> all{ makeLayer(1), makeLayer(1) };
    auto groups = fvGroupLayersByPane(all, {1, 2, 3});
    REQUIRE(groups.size() == 3);
    REQUIRE(groups[1].layerIndices.empty());
    REQUIRE(groups[2].layerIndices.empty());

    // A layer whose pane is not in the layout list still gets a trailing group — no layer
    // may become invisible in the panel.
    all.push_back(makeLayer(99));
    groups = fvGroupLayersByPane(all, {1, 2, 3});
    REQUIRE(groups.size() == 4);
    REQUIRE(groups[3].paneId == 99);
    REQUIRE(groups[3].layerIndices == std::vector<int>{2});

    // Null entries are tolerated (mirrors fvFilterPane).
    all.insert(all.begin(), nullptr);
    groups = fvGroupLayersByPane(all, {1});
    size_t total = 0;
    for (const auto& g : groups) total += g.layerIndices.size();
    REQUIRE(total == 3);
}

TEST_CASE("fvRemovalOrder makes a multi-selection safe to erase sequentially",
          "[layerpanel][TC-LYR-08]") {
    // Descending + de-duplicated: removing high indices first cannot invalidate the ones
    // still pending. Verified against a real LayerManager.
    const auto order = fvRemovalOrder({2, 0, 2, 4, -1});
    REQUIRE(order == std::vector<int>{4, 2, 0});

    LayerManager mgr;
    for (int i = 0; i < 5; ++i) mgr.addLayer(makeLayer(1, QString("L%1").arg(i)));
    const auto* keep1 = mgr.layerAt(1).get();
    const auto* keep3 = mgr.layerAt(3).get();

    for (int idx : order) mgr.removeLayer(idx);      // erase L4, L2, L0

    REQUIRE(mgr.count() == 2);
    REQUIRE(mgr.layerAt(0).get() == keep1);          // exactly the unselected layers survive
    REQUIRE(mgr.layerAt(1).get() == keep3);
}

TEST_CASE("fvMoveTargetIndex converts a drop position into a moveLayer destination",
          "[layerpanel][TC-LYR-09]") {
    // moveLayer erases then inserts, so a downward move must account for the vacated slot.
    REQUIRE(fvMoveTargetIndex(0, 3, 5) == 2);    // drop above row 3 ⇒ land at 2
    REQUIRE(fvMoveTargetIndex(4, 1, 5) == 1);    // upward move needs no adjustment
    REQUIRE(fvMoveTargetIndex(2, 2, 5) == 2);    // dropped on itself ⇒ no-op
    REQUIRE(fvMoveTargetIndex(2, 3, 5) == 2);    // dropped just below itself ⇒ no-op
    REQUIRE(fvMoveTargetIndex(0, 5, 5) == 4);    // "past the last row" ⇒ bottom
    // Degenerate input is clamped rather than producing an out-of-range move.
    REQUIRE(fvMoveTargetIndex(1, -7, 5) == 0);
    REQUIRE(fvMoveTargetIndex(1, 99, 5) == 4);
    REQUIRE(fvMoveTargetIndex(0, 0, 0)  == 0);

    // End-to-end against LayerManager: dragging the top layer onto the 4th row lands it
    // directly above the layer that was at index 3.
    LayerManager mgr;
    for (int i = 0; i < 5; ++i) mgr.addLayer(makeLayer(1, QString("L%1").arg(i)));
    const auto* moved  = mgr.layerAt(0).get();
    const auto* anchor = mgr.layerAt(3).get();
    mgr.moveLayer(0, fvMoveTargetIndex(0, 3, mgr.count()));
    REQUIRE(mgr.layerAt(2).get() == moved);
    REQUIRE(mgr.layerAt(3).get() == anchor);
}

TEST_CASE("fvPanesEmptiedBy reports only the panes losing every layer",
          "[layerpanel][TC-LYR-13]") {
    // Drives the "remove the layers only, or close the emptied pane too?" prompt: a pane
    // that keeps at least one layer must never be offered for closing.
    // Indices:            0(p1) 1(p2) 2(p1) 3(p3) 4(p2)
    std::vector<std::shared_ptr<Layer>> all{
        makeLayer(1), makeLayer(2), makeLayer(1), makeLayer(3), makeLayer(2),
    };
    REQUIRE(fvPanesEmptiedBy(all, {0, 2}) == std::vector<uint64_t>{1});     // pane 1 emptied
    REQUIRE(fvPanesEmptiedBy(all, {0}).empty());                            // pane 1 keeps #2
    REQUIRE(fvPanesEmptiedBy(all, {3}) == std::vector<uint64_t>{3});        // pane 3 had one
    REQUIRE(fvPanesEmptiedBy(all, {1, 4}) == std::vector<uint64_t>{2});
    // Removing everything empties every pane, reported in first-seen order.
    REQUIRE(fvPanesEmptiedBy(all, {0, 1, 2, 3, 4}) == std::vector<uint64_t>{1, 2, 3});
    REQUIRE(fvPanesEmptiedBy(all, {}).empty());
    // Null entries and out-of-range indices are ignored, not counted as removals.
    all.push_back(nullptr);
    REQUIRE(fvPanesEmptiedBy(all, {3, 99}) == std::vector<uint64_t>{3});
}

TEST_CASE("fvNextPaneLabel reuses the lowest free number above the highest in use",
          "[layerpanel][pane][TC-PNE-11]") {
    // Closing "Pane 2" while "Pane 1" remains must make the next pane "Pane 2" again —
    // pane LABELS follow the canvas, not the monotonic internal pane id.
    REQUIRE(fvNextPaneLabel({}) == "Pane 1");
    REQUIRE(fvNextPaneLabel({"Pane 1"}) == "Pane 2");
    REQUIRE(fvNextPaneLabel({"Pane 1", "Pane 2"}) == "Pane 3");
    REQUIRE(fvNextPaneLabel({"Pane 1", "Pane 3"}) == "Pane 4");   // highest in use + 1
    REQUIRE(fvNextPaneLabel({"Pane 2"}) == "Pane 3");
    REQUIRE(fvNextPaneLabel({"Pane 10", "Pane 2"}) == "Pane 11"); // numeric, not lexical
    // User-renamed panes without a trailing number never block a number.
    REQUIRE(fvNextPaneLabel({"Overview", "Pane 1"}) == "Pane 2");
    REQUIRE(fvNextPaneLabel({"Overview", "Detail"}) == "Pane 1");
    // A custom label that happens to end in a number still counts.
    REQUIRE(fvNextPaneLabel({"Scene 7"}) == "Pane 8");
}

TEST_CASE("fvPaneNeighbourIndex reorders a layer against its PANE siblings only",
          "[layerpanel][TC-LYR-10]") {
    // Indices:            0(p1) 1(p2) 2(p1) 3(p2) 4(p1)
    std::vector<std::shared_ptr<Layer>> all{
        makeLayer(1), makeLayer(2), makeLayer(1), makeLayer(2), makeLayer(1),
    };
    REQUIRE(fvPaneNeighbourIndex(all, 2, -1) == 0);   // pane 1: 0 ← 2 → 4 (skips pane-2 rows)
    REQUIRE(fvPaneNeighbourIndex(all, 2, +1) == 4);
    REQUIRE(fvPaneNeighbourIndex(all, 0, -1) == -1);  // topmost of its pane
    REQUIRE(fvPaneNeighbourIndex(all, 4, +1) == -1);  // bottom-most of its pane
    REQUIRE(fvPaneNeighbourIndex(all, 1, +1) == 3);
    REQUIRE(fvPaneNeighbourIndex(all, 1, -1) == -1);

    // Out-of-range / null-safe.
    REQUIRE(fvPaneNeighbourIndex(all, -1, +1) == -1);
    REQUIRE(fvPaneNeighbourIndex(all, 99, -1) == -1);
    all[3] = nullptr;
    REQUIRE(fvPaneNeighbourIndex(all, 1, +1) == -1);  // the only sibling was the null entry
}

// --------------------------------------------------------------------------
// Widget-level: a Shift/Ctrl multi-selection must survive the panel's own bookkeeping.
// Two regressions, both of which only bit on Linux:
//   • setCurrentItem() WITHOUT an explicit flag derives one from selectionCommand(index,
//     /*event=*/nullptr), which falls back to QGuiApplication::keyboardModifiers(). Under
//     ExtendedSelection that is Toggle while Ctrl is held and SelectCurrent under Shift —
//     so the programmatic "follow the active layer" call undid the user's own click. The
//     modifier state is live on X11 and stale on Windows, hence the platform split.
//   • rebuildList() re-selected the pane headers as it built them and then wiped them with
//     a ClearAndSelect from setCurrentItem, while the child rows kept their highlight role.

#include "panels/LayerPanel.hpp"

#include <QTreeWidget>
#include <QItemSelectionModel>

namespace {
QTreeWidget* panelTree(LayerPanel& p) { return p.findChild<QTreeWidget*>("layerTree"); }

// The panel casts to RasterLayer for the subdataset combo whenever type() is Raster, so the
// widget-level stub must NOT claim to be one.
class PanelStubLayer : public Layer {
public:
    LayerType type() const override { return LayerType::OSMBasemap; }
};

std::shared_ptr<Layer> makePanelLayer(uint64_t paneId, const QString& name) {
    auto l = std::make_shared<PanelStubLayer>();
    l->setPaneId(paneId);
    l->setName(name);
    return l;
}
} // namespace

TEST_CASE("TC-LYR-18 setCurrentItem with NoUpdate leaves a multi-selection alone",
          "[layerpanel][selection][TC-LYR-18]") {
    // Pins the Qt contract the panel now depends on: NoUpdate is the ONLY flag that makes a
    // programmatic current-row change modifier-proof.
    QTreeWidget tree;
    tree.setColumnCount(1);
    tree.setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (int i = 0; i < 3; ++i) tree.addTopLevelItem(new QTreeWidgetItem({QString::number(i)}));

    tree.topLevelItem(0)->setSelected(true);
    tree.topLevelItem(2)->setSelected(true);
    REQUIRE(tree.selectedItems().size() == 2);

    tree.setCurrentItem(tree.topLevelItem(0), 0, QItemSelectionModel::NoUpdate);
    CHECK(tree.selectedItems().size() == 2);
    CHECK(tree.currentItem() == tree.topLevelItem(0));
}

TEST_CASE("TC-LYR-19 a rebuild preserves a mixed layer + pane-header selection",
          "[layerpanel][selection][TC-LYR-19]") {
    LayerManager mgr;
    mgr.addLayer(makePanelLayer(1, "a"));    // index 0, pane 1
    mgr.addLayer(makePanelLayer(1, "b"));    // index 1, pane 1
    mgr.addLayer(makePanelLayer(2, "c"));    // index 2, pane 2

    LayerPanel panel;
    panel.setPaneListResolver([] {
        return std::vector<std::pair<quint64, QString>>{{1, "Pane 1"}, {2, "Pane 2"}};
    });
    panel.setLayerManager(&mgr);

    auto* tree = panelTree(panel);
    REQUIRE(tree != nullptr);
    REQUIRE(tree->topLevelItemCount() == 2);          // one group per pane

    auto* pane2 = tree->topLevelItem(1);
    auto* rowA  = tree->topLevelItem(0)->child(0);
    auto* rowB  = tree->topLevelItem(0)->child(1);
    REQUIRE(rowA != nullptr);
    REQUIRE(rowB != nullptr);

    // What a Ctrl-click sequence leaves behind: two layer rows of pane 1 plus pane 2's header.
    tree->clearSelection();
    rowA->setSelected(true);
    rowB->setSelected(true);
    pane2->setSelected(true);
    REQUIRE(tree->selectedItems().size() == 3);

    // Anything that re-groups the panel (a pane colour change, a sync-role change, a rename)
    // funnels through rebuildList.
    panel.refreshPanes();

    REQUIRE(tree->topLevelItemCount() == 2);
    CHECK(tree->topLevelItem(0)->child(0)->isSelected());
    CHECK(tree->topLevelItem(0)->child(1)->isSelected());
    CHECK(tree->topLevelItem(1)->isSelected());        // the header, previously dropped
    CHECK(tree->selectedItems().size() == 3);
}

// TC-LYR-20 — a selected row must keep its pane band while the pointer is over it. The
// theme's `QTreeWidget::item:hover` rule is an OPAQUE background-color, and the stylesheet
// paints it inside QStyledItemDelegate::paint — after the delegate has laid down the band —
// so hovering a Shift/Ctrl selection repainted the highlight away and the rows looked
// deselected. Pixels, not state: the selection model was never wrong, only the painting.
#include <QFile>
#include <QHoverEvent>
#include <QImage>
#include <QApplication>

namespace {
// Deliver a hover to the viewport and repaint. Sent directly rather than via a synthetic
// mouse move: QWidget only forwards hover events when WA_Hover is set, but
// QAbstractItemView::viewportEvent updates its hovered index from the event either way.
void hoverRow(QTreeWidget* tree, const QPoint& pos) {
    QHoverEvent ev(QEvent::HoverMove, QPointF(pos), QPointF(-1, -1));
    QApplication::sendEvent(tree->viewport(), &ev);
}

QImage rowPixels(QTreeWidget* tree, QTreeWidgetItem* item) {
    const QRect r = tree->visualItemRect(item);
    return tree->viewport()->grab(r).toImage();
}
} // namespace

TEST_CASE("TC-LYR-20 hover does not repaint a selected row's highlight away",
          "[layerpanel][selection][theme][TC-LYR-20]") {
    QFile qss(QStringLiteral(FV_SOURCE_DIR "/resources/themes/dark.qss"));
    REQUIRE(qss.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString theme = QString::fromUtf8(qss.readAll());
    REQUIRE(theme.contains(QStringLiteral("::item:hover")));   // the rule under test

    LayerManager mgr;
    mgr.addLayer(makePanelLayer(1, "a"));
    mgr.addLayer(makePanelLayer(1, "b"));

    LayerPanel panel;
    panel.setPaneListResolver([] {
        return std::vector<std::pair<quint64, QString>>{{1, "Pane 1"}};
    });
    panel.setPaneColorResolver([](uint64_t) { return QColor(0, 120, 255); });
    panel.setLayerManager(&mgr);
    panel.setStyleSheet(theme);
    panel.resize(320, 240);
    panel.show();

    auto* tree = panelTree(panel);
    REQUIRE(tree != nullptr);
    auto* rowA = tree->topLevelItem(0)->child(0);
    auto* rowB = tree->topLevelItem(0)->child(1);
    REQUIRE(rowA != nullptr);
    REQUIRE(rowB != nullptr);

    tree->clearSelection();
    rowA->setSelected(true);
    REQUIRE(rowA->isSelected());
    REQUIRE_FALSE(rowB->isSelected());

    const QRect rectA = tree->visualItemRect(rowA);
    const QRect rectB = tree->visualItemRect(rowB);
    REQUIRE(rectA.isValid());
    REQUIRE(rectB.isValid());

    // Baseline with the pointer parked off both rows.
    hoverRow(tree, QPoint(rectA.center().x(), tree->viewport()->height() - 1));
    const QImage selectedIdle = rowPixels(tree, rowA);
    const QImage plainIdle    = rowPixels(tree, rowB);

    // Control: an UNSELECTED row still takes the theme's hover. This also proves the hover
    // event reaches the view — without it the assertion below would pass vacuously.
    hoverRow(tree, rectB.center());
    CHECK(rowPixels(tree, rowB) != plainIdle);

    // The regression: hovering the SELECTED row must not change a single pixel of it.
    hoverRow(tree, rectA.center());
    CHECK(rowPixels(tree, rowA) == selectedIdle);
    CHECK(rowA->isSelected());
}

// TC-LYR-21 — a selection must survive letting go of the button away from the row. The tree
// enables drag, so Qt defers a press on an ALREADY-SELECTED row to the release
// (noSelectionOnMousePress — what makes a multi-selection draggable) and then applies that
// command to whatever sits under the pointer at release. Over empty space that index is
// invalid, so ClearAndSelect dropped the whole selection: the pane band went flat and the
// trash button greyed out, which read as "hover deselects".
#include <QMouseEvent>
#include <QAbstractButton>

namespace {
void sendMouse(QTreeWidget* tree, QEvent::Type type, const QPoint& p, Qt::MouseButton btn,
               Qt::MouseButtons held) {
    QMouseEvent ev(type, QPointF(p), tree->viewport()->mapToGlobal(QPointF(p)),
                   btn, held, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &ev);
}
void clickAt(QTreeWidget* tree, const QPoint& p) {
    sendMouse(tree, QEvent::MouseButtonPress,   p, Qt::LeftButton, Qt::LeftButton);
    sendMouse(tree, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
}
// Press on `from`, let go over `to`.
//
// Deliberately NO intermediate button-held MouseMove. The tree enables drag, so a synthetic
// move past the platform's start-drag distance takes QAbstractItemView::mouseMoveEvent into
// startDrag() → QDrag::exec(), which spins a NESTED, MODAL event loop. The release this
// helper sends next is never delivered — it is queued behind a loop that is waiting for it —
// so the case hangs until CTest times it out. That is what the conda Linux lane hit; whether
// a given Qt build starts the drag at all is a platform-plugin detail, which is why the same
// code passed elsewhere.
//
// Nothing is lost: the behaviour under test is what the RELEASE resolves to
// (LayerTreeWidget::selectionCommand, release + invalid index + m_press_on_row → NoUpdate).
// m_press_on_row is set by the press and the release carries its own position, so the
// intermediate move never entered the decision.
void pressReleaseElsewhere(QTreeWidget* tree, const QPoint& from, const QPoint& to) {
    sendMouse(tree, QEvent::MouseButtonPress,   from, Qt::LeftButton, Qt::LeftButton);
    sendMouse(tree, QEvent::MouseButtonRelease, to,   Qt::LeftButton, Qt::NoButton);
}
QAbstractButton* trashButton(LayerPanel& panel) {
    for (auto* b : panel.findChildren<QAbstractButton*>())
        if (b->toolTip().contains(QStringLiteral("Remove the selected"))) return b;
    return nullptr;
}
} // namespace

TEST_CASE("TC-LYR-21 releasing away from a row keeps the selection and the trash button",
          "[layerpanel][selection][TC-LYR-21]") {
    LayerManager mgr;
    mgr.addLayer(makePanelLayer(1, "a"));
    mgr.addLayer(makePanelLayer(1, "b"));

    LayerPanel panel;
    panel.setPaneListResolver([] {
        return std::vector<std::pair<quint64, QString>>{{1, "Pane 1"}, {2, "Pane 2"}};
    });
    panel.setPaneColorResolver([](uint64_t) { return QColor(0, 120, 255); });
    panel.setLayerManager(&mgr);
    panel.resize(320, 260);
    panel.show();

    auto* tree  = panelTree(panel);
    auto* trash = trashButton(panel);
    REQUIRE(tree != nullptr);
    REQUIRE(trash != nullptr);

    auto* rowA  = tree->topLevelItem(0)->child(0);
    auto* rowB  = tree->topLevelItem(0)->child(1);
    auto* pane2 = tree->topLevelItem(1);
    REQUIRE(rowA != nullptr);
    REQUIRE(rowB != nullptr);
    REQUIRE(pane2 != nullptr);

    // Somewhere inside the viewport with no row under it.
    const QPoint empty(5, tree->viewport()->height() - 2);
    REQUIRE_FALSE(tree->indexAt(empty).isValid());

    SECTION("pane header: press, slide off, release") {
        clickAt(tree, tree->visualItemRect(pane2).center());
        REQUIRE(pane2->isSelected());
        REQUIRE(trash->isEnabled());

        pressReleaseElsewhere(tree, tree->visualItemRect(pane2).center(), empty);
        CHECK(pane2->isSelected());
        CHECK(trash->isEnabled());
    }

    SECTION("layer row: press, slide off, release") {
        clickAt(tree, tree->visualItemRect(rowA).center());
        REQUIRE(rowA->isSelected());

        pressReleaseElsewhere(tree, tree->visualItemRect(rowA).center(), empty);
        CHECK(rowA->isSelected());
        CHECK(trash->isEnabled());
    }

    SECTION("a multi-selection survives the same gesture") {
        clickAt(tree, tree->visualItemRect(rowA).center());
        rowB->setSelected(true);
        pane2->setSelected(true);
        REQUIRE(tree->selectedItems().size() == 3);

        pressReleaseElsewhere(tree, tree->visualItemRect(rowB).center(), empty);
        CHECK(tree->selectedItems().size() == 3);
        CHECK(trash->isEnabled());
    }

    SECTION("pressing the empty background still deselects") {
        // The conventional gesture is preserved: only the DEFERRED release command is
        // intercepted, never a press that genuinely lands on the background.
        clickAt(tree, tree->visualItemRect(rowA).center());
        REQUIRE(rowA->isSelected());

        clickAt(tree, empty);
        CHECK_FALSE(rowA->isSelected());
        CHECK_FALSE(trash->isEnabled());
    }
}

// TC-LYR-16 — the subdataset (NetCDF/HDF variable) picker must not consume the wheel while
// unfocused. The pointer crosses that combo whenever the Layers tree is scrolled, and
// QComboBox's default handler stepped to the next variable — which re-opens the dataset,
// re-derives the stretch and re-renders every tile — instead of scrolling the panel.
#include "io/DatasetFactory.hpp"
#include "core/RasterLayer.hpp"
#include "fixtures/FixtureFactory.hpp"
#include <QComboBox>
#include <QWheelEvent>

namespace {
void wheelOver(QWidget* w, int delta) {
    const QPointF p = w->rect().center();
    QWheelEvent ev(p, w->mapToGlobal(p), QPoint(0, delta), QPoint(0, delta),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}
} // namespace

TEST_CASE("TC-LYR-16 scrolling over the variable picker does not switch the variable",
          "[layerpanel][subdataset][TC-LYR-16]") {
    FixtureFactory ff;
    auto fx = ff.netcdfMultiVar(8, 8);
    if (fx.path.empty()) SKIP("GDAL netCDF driver unavailable");
    auto subs = DatasetFactory::listSubdatasets(fx.path);
    REQUIRE(subs.size() >= 2);
    auto ds0 = DatasetFactory::openSubdataset(subs[0].first);
    REQUIRE(ds0 != nullptr);

    auto layer = std::make_shared<RasterLayer>(ds0);
    layer->initSubdatasetMeta(fx.path, subs, 0);
    layer->setPaneId(1);

    LayerManager mgr;
    mgr.addLayer(layer);

    LayerPanel panel;
    panel.setPaneListResolver([] {
        return std::vector<std::pair<quint64, QString>>{{1, "Pane 1"}};
    });
    panel.setPaneColorResolver([](uint64_t) { return QColor(0, 120, 255); });
    panel.setLayerManager(&mgr);
    panel.resize(340, 300);
    panel.show();

    auto* tree  = panelTree(panel);
    REQUIRE(tree != nullptr);
    auto* combo = tree->findChild<QComboBox*>();
    REQUIRE(combo != nullptr);
    REQUIRE(combo->count() >= 2);
    REQUIRE(combo->currentIndex() == 0);

    SECTION("unfocused: the wheel is ignored, in both directions") {
        REQUIRE_FALSE(combo->hasFocus());
        wheelOver(combo, -120);
        CHECK(combo->currentIndex() == 0);
        CHECK(layer->subdatasetIndex() == 0);   // the dataset was never re-opened
        wheelOver(combo, 120);
        CHECK(combo->currentIndex() == 0);
    }

    SECTION("the wheel must not focus the combo either") {
        // ClickFocus, not the default WheelFocus — otherwise the first scroll would arm the
        // behaviour for the second.
        wheelOver(combo, -120);
        CHECK_FALSE(combo->hasFocus());
    }

    SECTION("focused by a deliberate click: the wheel works normally") {
        panel.activateWindow();
        QApplication::setActiveWindow(&panel);
        combo->setFocus(Qt::MouseFocusReason);
        QApplication::processEvents();
        if (!combo->hasFocus())
            SKIP("offscreen platform will not give the window keyboard focus");
        wheelOver(combo, -120);
        CHECK(combo->currentIndex() == 1);
        CHECK(layer->subdatasetIndex() == 1);
    }
}

// TC-LYR-17 — moving the pointer over the subdataset (NetCDF variable) picker must not
// disturb the selection. setItemWidget registers that combo as a PERSISTENT EDITOR for its
// row, so the view's mouse-move handling handed the row to edit(), which focuses the editor
// and took the selection with it: hovering the drop-down cleared every selected pane and
// layer and greyed out the trash button, with no click and no scroll. Note this reproduces
// only with the theme applied and a real multi-variable dataset, which is why the earlier
// hover tests missed it.
TEST_CASE("TC-LYR-17 hovering the variable picker leaves the selection alone",
          "[layerpanel][subdataset][selection][TC-LYR-17]") {
    FixtureFactory ff;
    auto fx = ff.netcdfMultiVar(8, 8);
    if (fx.path.empty()) SKIP("GDAL netCDF driver unavailable");
    auto subs = DatasetFactory::listSubdatasets(fx.path);
    REQUIRE(subs.size() >= 2);
    auto ds0 = DatasetFactory::openSubdataset(subs[0].first);
    REQUIRE(ds0 != nullptr);

    auto layer = std::make_shared<RasterLayer>(ds0);
    layer->initSubdatasetMeta(fx.path, subs, 0);
    layer->setPaneId(1);

    LayerManager mgr;
    mgr.addLayer(layer);
    mgr.addLayer(makePanelLayer(1, "plain"));

    QFile qss(QStringLiteral(FV_SOURCE_DIR "/resources/themes/dark.qss"));
    REQUIRE(qss.open(QIODevice::ReadOnly | QIODevice::Text));

    LayerPanel panel;
    panel.setPaneListResolver([] {
        return std::vector<std::pair<quint64, QString>>{{1, "Pane 1"}, {2, "Pane 2"}};
    });
    panel.setPaneColorResolver([](uint64_t) { return QColor(0, 120, 255); });
    panel.setLayerManager(&mgr);
    panel.setStyleSheet(QString::fromUtf8(qss.readAll()));
    panel.resize(340, 300);
    panel.show();

    auto* tree  = panelTree(panel);
    auto* trash = trashButton(panel);
    REQUIRE(tree != nullptr);
    REQUIRE(trash != nullptr);
    auto* combo = tree->findChild<QComboBox*>();
    REQUIRE(combo != nullptr);

    // The panel reports its selection to MainWindow through this signal; a spurious 0/0 is
    // what blanked the property panels as well as the trash button.
    int lastLayers = -1, lastPanes = -1;
    QObject::connect(&panel, &LayerPanel::selectionSummaryChanged,
                     [&](int l, int p) { lastLayers = l; lastPanes = p; });

    clickAt(tree, tree->visualItemRect(tree->topLevelItem(0)->child(0)).center());
    tree->topLevelItem(1)->setSelected(true);      // a pane header joins the selection
    QApplication::processEvents();
    REQUIRE(tree->selectedItems().size() == 2);
    REQUIRE(trash->isEnabled());

    // A hover is a move with NO button held, delivered to the viewport at the combo's row.
    const QPoint overCombo = tree->viewport()->mapFrom(combo->parentWidget(),
                                                       combo->geometry().center());
    REQUIRE(tree->indexAt(overCombo).isValid());
    QMouseEvent hover(QEvent::MouseMove, QPointF(overCombo),
                      tree->viewport()->mapToGlobal(QPointF(overCombo)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &hover);
    QApplication::processEvents();

    CHECK(tree->selectedItems().size() == 2);
    CHECK(trash->isEnabled());
    CHECK(lastLayers == 1);                 // never re-announced as 0/0
    CHECK(lastPanes == 1);
    CHECK_FALSE(combo->hasFocus());         // the editor must not steal focus on a hover
}

// Phase 26.10 follow-up. The ACTIVE layer's name is drawn bold, from a role stamped on the
// name column. That role was re-stamped only in the `activeLayerChanged` handler, which
// returns early under `m_selecting` — the guard that stops a panel-driven activation from
// collapsing a multi-row selection. So clicking a row activated the layer everywhere else in
// the application and left its own row unbolded: the panel that DROVE the change was the one
// place that did not show it.
TEST_CASE("TC-LYR-22 clicking a row bolds it as the active layer",
          "[layerpanel][selection][TC-LYR-22]") {
    LayerManager mgr;
    mgr.addLayer(makePanelLayer(1, "a"));    // index 0
    mgr.addLayer(makePanelLayer(1, "b"));    // index 1

    LayerPanel panel;
    panel.setPaneListResolver([] {
        return std::vector<std::pair<quint64, QString>>{{1, "Pane 1"}};
    });
    panel.setLayerManager(&mgr);
    panel.resize(320, 240);
    panel.show();
    QApplication::processEvents();

    auto* tree = panelTree(panel);
    REQUIRE(tree != nullptr);
    auto* rowB = tree->topLevelItem(0)->child(0);   // b (index 1, top visual row)
    auto* rowA = tree->topLevelItem(0)->child(1);   // a (index 0, bottom visual row)
    REQUIRE(rowB != nullptr);
    REQUIRE(rowA != nullptr);

    // The delegate's bold flag: Qt::UserRole + 2 on the name column (kActiveRole in
    // LayerPanel.cpp). Read as state rather than rendered, so the case pins what the painter
    // is driven by without depending on a font metric.
    const int kActiveRole = Qt::UserRole + 2;
    const int kColName    = 0;

    clickAt(tree, tree->visualItemRect(rowB).center());
    QApplication::processEvents();

    REQUIRE(mgr.activeIndex() == 1);                              // it really did activate
    CHECK(rowB->data(kColName, kActiveRole).toBool());            // ...and the row says so
    CHECK_FALSE(rowA->data(kColName, kActiveRole).toBool());      // exactly one bold row

    // And back, so the flag follows the active layer rather than accumulating.
    clickAt(tree, tree->visualItemRect(rowA).center());
    QApplication::processEvents();
    REQUIRE(mgr.activeIndex() == 0);
    CHECK(rowA->data(kColName, kActiveRole).toBool());
    CHECK_FALSE(rowB->data(kColName, kActiveRole).toBool());

    // A programmatic activation — a pane click elsewhere in the app — still works. That path
    // was never broken, and must not break now that both go through one helper.
    mgr.setActiveLayer(1);
    QApplication::processEvents();
    CHECK(rowB->data(kColName, kActiveRole).toBool());
    CHECK_FALSE(rowA->data(kColName, kActiveRole).toBool());
}
