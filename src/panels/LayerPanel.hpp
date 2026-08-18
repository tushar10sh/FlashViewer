#pragma once
#include <QWidget>
#include <QTreeWidget>
#include <QDropEvent>
#include <QEvent>
#include <QColor>
#include "widgets/SvgIconButton.hpp"
#include "widgets/UiKit.hpp"   // FvPaneSyncInfo (pane-header sync badge)
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class LayerManager;
class Layer;

// QTreeWidget subclass driving the pane-GROUPED layer list (Phase 18 #6): top-level items
// are pane headers, layer rows are their children, and a subdataset variable combo is a
// grandchild. Qt's InternalMove would clone/insert rows and wreck that structure, so drag
// and drop is handled here explicitly: a drop is turned into a
// `layerDropRequested(from, targetPaneId, insertBefore)` description and the panel rebuilds
// from the LayerManager — the model, not the view, is the source of truth.
class LayerTreeWidget : public QTreeWidget {
    Q_OBJECT
    int m_drag_src{-1};      // layer index being dragged (-1 = none)
    // Whether the press that began the current click landed on a row. Read by
    // selectionCommand to tell "slid off a row before letting go" (keep the selection) from
    // "clicked the empty background" (clear it) — both arrive as a release on an invalid index.
    bool m_press_on_row{false};
public:
    explicit LayerTreeWidget(QWidget* parent = nullptr);

signals:
    // A layer row was dropped: move layer `from` so it sits directly above the layer at
    // `insertBefore` (== LayerManager::count() ⇒ bottom), within pane `targetPaneId`.
    // `insertBefore` < 0 ⇒ keep the list position, only the pane changes.
    void layerDropRequested(int from, quint64 targetPaneId, int insertBefore);
    // A drag started/finished here. MainWindow uses this to stop a stacked region from
    // re-ordering itself mid-drag (Phase 18 #1, belt-and-suspenders).
    void dragActiveChanged(bool active);
    // Del pressed with a selection in the tree.
    void deleteRequested();

protected:
    // QTreeView paints the branch/indent gutter itself — with the STYLE's selection panel,
    // never through the item delegate — so a selected row grew a theme-blue tab to the left
    // of its pane-coloured band. drawRow repaints that strip with the same band.
    void drawRow(QPainter* p, const QStyleOptionViewItem& opt,
                 const QModelIndex& index) const override;
    // Guards the selection against a release that lands on no row. Drag is enabled here, so
    // a press on an ALREADY-SELECTED row defers its selection command to the release (Qt's
    // noSelectionOnMousePress, which is what lets a multi-selection be dragged). Qt then
    // applies that command to whatever is under the pointer at release — and over empty
    // space that is an invalid index, so ClearAndSelect drops the entire selection. Sliding
    // the pointer off a label before letting go therefore deselected everything and greyed
    // out the trash button.
    QItemSelectionModel::SelectionFlags selectionCommand(
        const QModelIndex& index, const QEvent* event = nullptr) const override;
    void mousePressEvent(QMouseEvent* e) override;
    // Swallows button-less moves. setItemWidget registers the subdataset combo as a
    // PERSISTENT EDITOR for its row, and the view's move handling hands such a row to
    // edit(), which focuses the editor — taking the selection down with it. A hover has no
    // business reaching that machinery. Hover PAINTING is unaffected: it runs off
    // QEvent::HoverMove in viewportEvent, not off mouse moves.
    void mouseMoveEvent(QMouseEvent* e) override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    // Custom mime payload (the dragged layer's index) so a layer can also be dropped onto a
    // MapCanvas, a region, or a region PILL to reassign its pane (Phase 6.3 / Phase 18 #1).
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
};

// Layer list panel: pane-grouped collapsible rows, per-layer visibility toggle and opacity
// slider, Shift/Ctrl multi-select over layers AND pane groups, move up/down, delete, and a
// right-click context menu.
class LayerPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayerPanel(QWidget* parent = nullptr);
    void setLayerManager(LayerManager* mgr);
    // Phase 6.2: resolve a layer's pane colour (invalid = uncoloured). Set by MainWindow.
    void setPaneColorResolver(std::function<QColor(uint64_t)> fn);
    void refreshPaneColors();   // re-apply colours after a pane colour / set change
    // Phase 6.3 / 18: resolve the list of panes (id, label) in LAYOUT order. Drives both
    // the "To Pane" submenu and the pane grouping (every pane gets a group, even empty).
    void setPaneListResolver(std::function<std::vector<std::pair<quint64, QString>>()> fn);
    // Resolve a pane's sync role + its group master's colour/label. Drives the badge drawn at
    // the far right of each pane header, matching the pane chrome and the region pills.
    void setPaneSyncResolver(std::function<FvPaneSyncInfo(quint64)> fn);
    // Rebuild the grouped list — call after panes are added/removed/renamed (Phase 18 #6).
    void refreshPanes();
    // The HIGHLIGHTED rows, in LayerManager index order, with every layer of a selected PANE
    // header folded in — panes and layers of different panes mix freely in one Shift/Ctrl
    // selection (FR-LYR-10). It is the removal set, and since Phase 26.9 also what one
    // Scan/Pixel Profile Compute covers when 2+ rasters are selected (FR-ANL-12), which is why
    // MainWindow can read it.
    std::vector<int> selectedLayerIndices() const;

signals:
    void activeLayerChanged(int index);
    void layerDatasetChanged(int index);  // emitted when subdataset combo switches
    void fitToLayerRequested(int index);
    void layerSettingsRequested(int index);
    void paneAssignmentRequested(int layerIndex, quint64 paneId);  // "To Pane" (Phase 6.3)
    // Phase 18 #8: how many layers / pane groups are highlighted. MainWindow blanks the
    // single-subject panels (Histogram / Band / Layer Info) when >1 layer is selected.
    void selectionSummaryChanged(int layerCount, int paneCount);
    // Phase 18 #8: close these panes (routed to MainWindow::closePane).
    void paneCloseRequested(quint64 paneId);
    // Phase 18 #1b: double-click a row → deliberately bring that layer's pane to the FRONT
    // of its stacked region (single-click selection must never steal the ribbon).
    void paneFocusRequested(quint64 paneId);
    // Phase 18 #1: a drag from this panel started/finished.
    void dragActiveChanged(bool active);

private slots:
    void onLayerAdded(int index);
    void onLayerRemoved(int index);
    void onLayerChanged(int index);
    void onItemChanged(QTreeWidgetItem* item, int col);
    void onSelectionChanged();
    void onContextMenu(const QPoint& pos);
    void deleteSelection();

protected:
    void changeEvent(QEvent* e) override;

private:
    void rebuildList();
    // Apply a drop described by LayerTreeWidget: reassign the pane (if it changed) and move
    // the layer so it lands directly above `insertBefore` (< 0 ⇒ keep the list position).
    void applyLayerDrop(int from, quint64 targetPaneId, int insertBefore);
    void updateMoveButtons();
    void updateArrowIcons();
    void resizeHeaderColumns();
    QTreeWidgetItem* itemForIndex(int index) const;    // layer row for a LayerManager index
    QTreeWidgetItem* groupItemFor(quint64 paneId) const;

    // Row classification helpers (roles are set in rebuildList).
    static bool isGroupItem(const QTreeWidgetItem* item);
    static int  layerIndexOf(const QTreeWidgetItem* item);   // -1 = not a layer row
    static quint64 paneIdOf(const QTreeWidgetItem* item);

    // The HIGHLIGHTED rows are the removal set — there are no separate "select for delete"
    // checkboxes. A selected pane group contributes every layer it holds, and panes and
    // layers of other panes mix freely in one Shift/Ctrl selection.
    std::vector<quint64> selectedPaneIds() const;        // highlighted group headers

    QColor paneColorFor(int layerIndex) const;
    /// Re-stamp the bold-name role so it names `activeIndex`, whatever drove the change.
    void   markActiveRow(int activeIndex);

    LayerManager*     m_mgr{nullptr};
    LayerTreeWidget*  m_tree{nullptr};
    SvgIconButton*    m_btn_up{nullptr};
    SvgIconButton*    m_btn_down{nullptr};
    SvgIconButton*    m_btn_del{nullptr};
    bool              m_updating{false};   // suppress model→UI→model feedback on rebuilds
    bool              m_selecting{false};  // this panel is driving setActiveLayer right now
    std::function<QColor(uint64_t)> m_pane_color;   // layer paneId → pane colour
    std::function<std::vector<std::pair<quint64, QString>>()> m_pane_list;  // panes, layout order
    std::function<FvPaneSyncInfo(quint64)> m_pane_sync;   // pane id → sync badge state
};
