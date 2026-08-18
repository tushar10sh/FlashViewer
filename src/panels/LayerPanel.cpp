#include "panels/LayerPanel.hpp"
#include "panels/LayerGrouping.hpp"
#include "widgets/UiKit.hpp"          // fvPaintTickBox, FvTickCheckBox
#include "core/LayerManager.hpp"
#include "core/Layer.hpp"
#include "core/RasterLayer.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFontMetrics>
#include <QHeaderView>
#include <QTreeWidgetItem>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QToolTip>
#include <QStyledItemDelegate>
#include <QApplication>
#include <QStyle>
#include <QPainter>
#include <QItemSelectionModel>
#include <algorithm>
#include <utility>                    // std::as_const (QSet iteration without a detach)
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QMimeData>
#include <QHash>
#include <QSet>
#include <QStringList>

// MIME type carrying a dragged layer's row index (drop onto a MapCanvas / region / region
// pill → reassign pane; drop inside this tree → regroup + reorder).
static constexpr const char* kLayerMime = "application/x-flashviewer-layer";

static constexpr int kColName   = 0;
static constexpr int kColOrder  = 1;
static constexpr int kColVis    = 2;
static constexpr int kColOp     = 3;
// Per-item roles. kLayerIndexRole is the LayerManager index of a layer row and is -1 on
// pane-group headers and on the subdataset combo grandchild, so every row can be classified
// without walking the tree (Phase 18 #6).
static constexpr int kLayerIndexRole = Qt::UserRole;
// The layer's pane colour (Phase 6.2); invalid = uncoloured.
static constexpr int kPaneColorRole = Qt::UserRole + 1;
// True on the row that is the ACTIVE layer (Phase 15 #3). Drives the bold name,
// independent of pane colouring and of the selection highlight.
static constexpr int kActiveRole = Qt::UserRole + 2;
// Pane id — set on both the group header and every layer row beneath it (Phase 18).
static constexpr int kPaneIdRole = Qt::UserRole + 3;
// True on a pane-group header row (Phase 18 #6).
static constexpr int kGroupRole = Qt::UserRole + 4;
// True on a layer row whose PANE HEADER is selected. Selecting a pane selects its contents,
// but only the header sits in Qt's selection model — mirroring the children into it would
// fight ClearAndSelect (a plain click on one child would immediately be undone). The rows
// are highlighted through this role instead, and selectedLayerIndices() folds them in.
static constexpr int kPaneSelRole = Qt::UserRole + 5;
// Sync badge on a pane-GROUP header: the pane's role (0 none / 1 master / 2 slave) and the
// colour of the group's MASTER, which both roles are drawn in (Phase 20).
static constexpr int kSyncRoleRole  = Qt::UserRole + 6;
static constexpr int kSyncColorRole = Qt::UserRole + 7;

// Edge length of the pane-header sync badge, and its inset from the row's trailing edge.
static constexpr int kSyncBadgePx     = 14;
static constexpr int kSyncBadgeMargin = 6;

// Delegate that paints every row of the tree through ONE path (Phase 18):
//  • the row background is a translucent PANE-COLOURED band — stronger for a pane-GROUP
//    header (mirroring AttributeInspector's per-pane drop-downs) than for a layer row, and
//    stronger again when selected, replacing the theme's blue selection panel;
//  • the row TEXT keeps the theme's normal foreground (user decision) — the band already
//    attributes a row to its pane, so tinting the name too was redundant and cost contrast;
//  • weight is the only text emphasis: pane headers and the ACTIVE layer's name are bold
//    (Phase 15 #3).
// LayerTreeWidget::drawRow paints the same band across the branch/indent gutter, which the
// delegate never sees, so a selected row is uniform edge to edge.
namespace {
// The tick-box painter (Phase 18 follow-up #4) and FvTickCheckBox now live in
// widgets/UiKit.hpp — the Raster Math and GDAL Operations dialogs paint their
// checkboxes with the same glyph, so it can no longer belong to this file. Here the
// tick is drawn in the row's PANE colour, making a ticked box immediately attributable
// to its pane; the dialogs pass no accent and inherit the theme's blue.

// The pane-coloured band a row is painted with. Shared by PaneColorDelegate (which fills the
// cell area) and LayerTreeWidget::drawRow (which fills the branch/indent gutter QTreeView
// reserves for the expander), so the two halves of a row can never disagree — the band spans
// the FULL row width for every row type, selected or not.
QColor fvRowBand(const QModelIndex& index, const QPalette& pal, bool selected) {
    const QModelIndex name = index.siblingAtColumn(kColName);
    QColor c = name.data(kPaneColorRole).value<QColor>();
    if (!c.isValid()) c = pal.color(QPalette::Highlight);   // pane colour is always set today
    const bool group = name.data(kGroupRole).toBool();
    c.setAlpha(group ? (selected ? 110 : 70)     // header band: stronger, reads as a title
                     : (selected ? 95  : 55));   // layer row: ~38 % selected, ~22 % idle
    return c;
}

class PaneColorDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override {
        const QModelIndex name = index.siblingAtColumn(kColName);
        const bool group    = name.data(kGroupRole).toBool();
        const bool active   = name.data(kActiveRole).toBool();
        const bool boldName = group || (active && index.column() == kColName);
        // Selecting a pane group highlights every layer under it, in that pane's colour —
        // the header carries the selection in the model, the rows follow via kPaneSelRole.
        const bool selected = (opt.state & QStyle::State_Selected)
                              || name.data(kPaneSelRole).toBool();

        // ONE painting path for headers and layer rows alike, so the highlight is uniform:
        // fill with the shared pane band, suppress the style's blue selection panel, then
        // let the base draw text/decorations over it.
        //
        // A SELECTED row is first flooded with the view's own background. The band is
        // translucent by design, and QTreeView::drawRow has already painted the row panel
        // underneath — including the theme's opaque `::item:hover` fill, which it derives
        // from its OWN hovered index, so no edit to the option we are handed can suppress
        // it. Compositing the band over hover lightened it towards the idle alpha, and the
        // row read as deselected for as long as the pointer sat on it. Flooding first makes
        // the selected band's colour independent of whatever was drawn below, exactly as
        // LayerTreeWidget::drawRow already does for the branch gutter. Unselected rows are
        // left alone, so they keep the theme's hover feedback.
        if (selected) {
            const auto* view = qobject_cast<const QAbstractItemView*>(opt.widget);
            p->fillRect(opt.rect, view ? view->viewport()->palette().color(QPalette::Base)
                                       : opt.palette.color(QPalette::Base));
        }
        p->fillRect(opt.rect, fvRowBand(index, opt.palette, selected));

        QStyleOptionViewItem o(opt);
        o.state &= ~QStyle::State_Selected;     // the band above already conveys selection
        // Hover has to go the same way on a selected row. The theme's
        // `QTreeWidget::item:hover` rule is an OPAQUE background-color, and the stylesheet
        // fills the item rect with it before the text is drawn — repainting the pane band
        // away, so a Shift/Ctrl selection looked like it dropped every row the pointer
        // crossed. Unselected rows keep the theme's hover feedback.
        if (selected) o.state &= ~QStyle::State_MouseOver;
        o.backgroundBrush = Qt::NoBrush;
        if (boldName) o.font.setBold(true);     // header label / active layer name
        // Row TEXT keeps the theme's normal foreground (user decision): the pane-coloured
        // band behind the row already attributes it to its pane, so tinting the name too was
        // redundant and cost contrast. Only weight distinguishes a header / the active layer.
        QStyledItemDelegate::paint(p, o, index);

        // Sync badge, far right of a pane header (Phase 20): ★ when this pane is the sync
        // master, the mirror glyph when it is a slave — both in the MASTER's pane colour, so
        // the panel says at a glance which panes follow which. A pane header spans all three
        // columns, so opt.rect here is the whole row and the badge sits on its trailing edge.
        if (!group || index.column() != kColName) return;
        FvPaneSyncInfo sync;
        sync.role        = name.data(kSyncRoleRole).toInt();
        sync.masterColor = name.data(kSyncColorRole).value<QColor>();
        const qreal dpr = opt.widget ? opt.widget->devicePixelRatioF() : qreal(1);
        const QPixmap badge = fvSyncRoleIcon(sync, kSyncBadgePx, dpr);
        if (badge.isNull()) return;
        const int y = opt.rect.y() + (opt.rect.height() - kSyncBadgePx) / 2;
        const int x = (opt.direction == Qt::RightToLeft)
            ? opt.rect.left() + kSyncBadgeMargin
            : opt.rect.right() - kSyncBadgePx - kSyncBadgeMargin + 1;
        p->drawPixmap(QRect(x, y, kSyncBadgePx, kSyncBadgePx), badge);
    }
};

// The subdataset (NetCDF/HDF variable) picker. A combo living inside a scrollable panel must
// not eat the wheel: the pointer crosses it whenever the Layers tree is scrolled, and
// QComboBox's default handler would step to the next variable — which re-opens the dataset,
// re-derives the stretch and re-renders every tile — instead of scrolling the list. The wheel
// is honoured only once the combo has been deliberately focused by a click; otherwise it is
// ignored so it bubbles up to the tree's scroll area.
class FvVariableComboBox : public QComboBox {
public:
    explicit FvVariableComboBox(QWidget* parent = nullptr) : QComboBox(parent) {
        // ClickFocus, not the default WheelFocus: a wheel must never focus the combo, or the
        // first scroll would arm the very behaviour this class exists to prevent.
        setFocusPolicy(Qt::ClickFocus);
    }

protected:
    void wheelEvent(QWheelEvent* e) override {
        if (hasFocus()) {
            int delta = e->angleDelta().y();
            if (delta != 0) {
                int nextIdx = currentIndex() + (delta < 0 ? 1 : -1);
                if (nextIdx >= 0 && nextIdx < count()) {
                    setCurrentIndex(nextIdx);
                    e->accept();
                    return;
                }
            }
            QComboBox::wheelEvent(e);
            return;
        }
        e->ignore();
    }
};

// Layer index carried by a row (-1 = pane header / subdataset combo row).
int rowLayerIndex(const QTreeWidgetItem* it) {
    if (!it) return -1;
    const QVariant v = it->data(kColName, kLayerIndexRole);
    return v.isValid() ? v.toInt() : -1;
}
} // namespace

// --- LayerTreeWidget implementation ---

LayerTreeWidget::LayerTreeWidget(QWidget* parent) : QTreeWidget(parent) {}

void LayerTreeWidget::drawRow(QPainter* p, const QStyleOptionViewItem& opt,
                             const QModelIndex& index) const {
    // NOTE: hover cannot be suppressed by editing the option passed here — QTreeView::drawRow
    // re-derives State_MouseOver from its own hovered index. The selected row's band is made
    // immune to it in PaneColorDelegate::paint instead, by flooding the cell first.
    QTreeWidget::drawRow(p, opt, index);

    const bool selected = (selectionModel() && selectionModel()->isSelected(index))
                          || index.siblingAtColumn(kColName).data(kPaneSelRole).toBool();

    // The branch/indent gutter is drawn by QTreeView with the style's row panel (the theme's
    // blue selection), and the item delegate never sees it — a SELECTED pane therefore showed
    // a blue tab beside its pane-coloured band. Repaint that strip with the SAME band
    // (opaquely, since the style has already painted underneath), then put the expander back
    // on top. Only selected rows are touched: an idle row's expander area stays plain
    // background, so the drop-down arrow is never tinted until the row is actually picked.
    if (!selected) return;

    int depth = 0;
    for (QModelIndex a = index.parent(); a.isValid(); a = a.parent()) ++depth;
    const int w = indentation() * (depth + (rootIsDecorated() ? 1 : 0));
    if (w <= 0) return;
    const QRect gutter = isRightToLeft()
        ? QRect(opt.rect.right() - w + 1, opt.rect.y(), w, opt.rect.height())
        : QRect(opt.rect.left(),          opt.rect.y(), w, opt.rect.height());

    p->save();
    p->fillRect(gutter, viewport()->palette().color(QPalette::Base));   // erase the blue
    p->fillRect(gutter, fvRowBand(index, palette(), /*selected=*/true));
    p->restore();
    drawBranches(p, gutter, index);
}

void LayerTreeWidget::mousePressEvent(QMouseEvent* e) {
    m_press_on_row = indexAt(e->position().toPoint()).isValid();
    QTreeWidget::mousePressEvent(e);
}

void LayerTreeWidget::mouseMoveEvent(QMouseEvent* e) {
    // Hover: nothing here may act on it. Drag-and-drop, rubber-band selection and
    // auto-scroll all arrive with a button held, so they still reach the base class.
    if (e->buttons() == Qt::NoButton) { e->ignore(); return; }
    QTreeWidget::mouseMoveEvent(e);
}

QItemSelectionModel::SelectionFlags LayerTreeWidget::selectionCommand(
    const QModelIndex& index, const QEvent* event) const {
    // Both gestures reach here as a release on an INVALID index, so the press decides which
    // one it was. Pressing the empty background still clears the selection — the conventional
    // "click away to deselect". Pressing a row and letting go once the pointer has slid off
    // it must not, which is the case Qt would otherwise resolve as ClearAndSelect(nothing).
    if (event && event->type() == QEvent::MouseButtonRelease
        && !index.isValid() && m_press_on_row)
        return QItemSelectionModel::NoUpdate;

    return QTreeWidget::selectionCommand(index, event);
}

void LayerTreeWidget::startDrag(Qt::DropActions actions) {
    m_drag_src = rowLayerIndex(currentItem());
    if (m_drag_src < 0) return;    // pane headers and combo rows are not draggable
    // QDrag::exec() inside the base implementation blocks until the drop completes, so the
    // "drag in flight" window is exactly this call (Phase 18 #1).
    emit dragActiveChanged(true);
    QTreeWidget::startDrag(actions);
    emit dragActiveChanged(false);
    m_drag_src = -1;
}

void LayerTreeWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kLayerMime)) e->acceptProposedAction();
    else e->ignore();
}

void LayerTreeWidget::dragMoveEvent(QDragMoveEvent* e) {
    if (!e->mimeData()->hasFormat(kLayerMime)) { e->ignore(); return; }
    // Let the base class compute + paint the drop indicator (it also updates
    // dropIndicatorPosition(), which dropEvent reads), then accept regardless of the
    // target item's drop flags — the drop is interpreted by us, not by the view.
    QTreeWidget::dragMoveEvent(e);
    e->acceptProposedAction();
}

void LayerTreeWidget::dropEvent(QDropEvent* e) {
    const int from = m_drag_src;
    if (from < 0 || !e->mimeData()->hasFormat(kLayerMime)) { e->ignore(); return; }

    // NOTE: QTreeWidget::dropEvent is deliberately NOT called. Qt's InternalMove clones the
    // dragged item and re-inserts it, which would flatten/duplicate the pane grouping. We
    // describe the drop instead and let LayerPanel re-apply it to the LayerManager and
    // rebuild — the model stays the single source of truth (Phase 18 #6).
    QTreeWidgetItem* target = itemAt(e->position().toPoint());
    const DropIndicatorPosition where = dropIndicatorPosition();

    QTreeWidgetItem* group = nullptr;
    bool onHeader = false;
    if (target) {
        if (!target->parent() && rowLayerIndex(target) < 0) { group = target; onHeader = true; }
        else if (rowLayerIndex(target) >= 0)                 group = target->parent();
        else if (target->parent()) { target = target->parent(); group = target->parent(); }
    }
    if (!group && topLevelItemCount() > 0)      // dropped on empty space below the rows
        group = topLevelItem(topLevelItemCount() - 1);
    if (!group) { e->ignore(); return; }

    // `insertBefore` is the layer index the dragged row must land ABOVE; -1 keeps the list
    // position (pane change only) when the target group has no layers to anchor against.
    int insertBefore = -1;
    if (onHeader || !target) {
        // Dropping onto a pane header (or past the last row) puts the layer on TOP of that
        // pane's block, so it is immediately visible in the pane.
        if (group->childCount() > 0) insertBefore = rowLayerIndex(group->child(0)) + 1;
    } else {
        const int t = rowLayerIndex(target);
        insertBefore = (where == QAbstractItemView::BelowItem) ? t : t + 1;
    }

    // Answer CopyAction, never MoveAction: QAbstractItemView::startDrag() calls
    // clearOrRemove() — which deletes the dragged rows from the view model — whenever
    // QDrag::exec() reports a move that the *base* dropEvent did not perform. We perform the
    // move on the LayerManager and rebuild, so letting Qt also strip rows would corrupt the
    // freshly rebuilt tree.
    e->setDropAction(Qt::CopyAction);
    e->accept();
    emit layerDropRequested(from, group->data(kColName, kPaneIdRole).toULongLong(),
                            insertBefore);
}

void LayerTreeWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete && !selectedItems().isEmpty()) {
        emit deleteRequested();
        e->accept();
        return;
    }
    QTreeWidget::keyPressEvent(e);
}

QMimeData* LayerTreeWidget::mimeData(const QList<QTreeWidgetItem*>& items) const {
    QMimeData* md = QTreeWidget::mimeData(items);
    if (!md) md = new QMimeData();
    // startDrag() resolved the dragged row before invoking the base implementation (which
    // calls us), so m_drag_src is authoritative; fall back to scanning the passed items.
    int idx = m_drag_src;
    if (idx < 0)
        for (auto* it : items)
            if ((idx = rowLayerIndex(it)) >= 0) break;
    if (idx >= 0) md->setData(kLayerMime, QByteArray::number(idx));
    return md;
}

// --- LayerPanel implementation ---

LayerPanel::LayerPanel(QWidget* parent) : QWidget(parent) {
    m_tree = new LayerTreeWidget(this);
    m_tree->setObjectName("layerTree");   // scopes the blank-indicator QSS rule (#4)
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Layer"), tr(""), tr("Vis"), tr("Opacity")});
    m_tree->header()->setSectionResizeMode(kColName,  QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(kColOrder, QHeaderView::Fixed);
    m_tree->header()->setSectionResizeMode(kColVis,   QHeaderView::Fixed);
    m_tree->header()->setSectionResizeMode(kColOp,    QHeaderView::Fixed);
    m_tree->header()->setStretchLastSection(false);
    resizeHeaderColumns();
    m_tree->setRootIsDecorated(true);    // pane groups are collapsible drop-downs (Phase 18 #6)
    m_tree->setIndentation(14);
    // Shift/Ctrl multi-select; the per-row checkbox mirrors this same selection (Phase 18 #8).
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Drops are interpreted by LayerTreeWidget::dropEvent, not by Qt's item mover.
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::CopyAction);   // see LayerTreeWidget::dropEvent
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    // Double-click is reserved for "bring this layer's pane to the front" (Phase 18 #1b),
    // so inline renaming is on F2 / the context menu instead of a double-click.
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setItemDelegate(new PaneColorDelegate(m_tree));   // per-pane row colouring

    // Up / Down / Delete action buttons
    m_btn_up   = new SvgIconButton(this);
    m_btn_down = new SvgIconButton(this);
    m_btn_del  = new SvgIconButton(this);
    updateArrowIcons();
    for (auto* b : {m_btn_up, m_btn_down, m_btn_del}) {
        b->setObjectName("layerMoveBtn");
        b->setFixedSize(20, 20);   // == the pane gear, so SvgIconButton's 14 px icon box
        b->setEnabled(false);      // (and therefore the stroke weight) matches exactly
    }
    m_btn_up->setToolTip(tr("Move layer up within its pane"));
    m_btn_down->setToolTip(tr("Move layer down within its pane"));
    m_btn_del->setToolTip(tr("Remove the selected layers (Del)"));

    auto* btnBar = new QHBoxLayout();
    // The panel sits flush against the dock separator (a 5 px QMainWindow::separator /
    // QSplitter::handle), so a 2 px right margin left the trash button visually touching
    // — and on hover bleeding into — the drag bar. Inset the bar past the separator width
    // so the buttons read as belonging to the panel. The tree below keeps its own edge.
    btnBar->setContentsMargins(4, 4, 8, 4);
    btnBar->setSpacing(2);
    btnBar->addStretch();
    btnBar->addWidget(m_btn_up);
    btnBar->addWidget(m_btn_down);
    btnBar->addWidget(m_btn_del);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(btnBar);
    lay->addWidget(m_tree);

    // ▲/▼ reorder a layer against its PANE siblings — layers of other panes never render
    // together, so only same-pane neighbours define a meaningful up/down (FR-LYR-2).
    auto moveBy = [this](int dir) {
        if (!m_mgr) return;
        const auto sel = selectedLayerIndices();
        if (sel.size() != 1) return;
        const int to = fvPaneNeighbourIndex(m_mgr->layers(), sel.front(), dir);
        if (to >= 0) m_mgr->moveLayer(sel.front(), to);
    };
    connect(m_btn_up,   &QToolButton::clicked, this, [moveBy] { moveBy(+1); });
    connect(m_btn_down, &QToolButton::clicked, this, [moveBy] { moveBy(-1); });
    connect(m_btn_del,  &QToolButton::clicked, this, &LayerPanel::deleteSelection);

    connect(m_tree, &QTreeWidget::itemChanged,
            this, &LayerPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::itemSelectionChanged,
            this, &LayerPanel::onSelectionChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &LayerPanel::onContextMenu);
    connect(m_tree, &LayerTreeWidget::deleteRequested,
            this, &LayerPanel::deleteSelection);
    connect(m_tree, &LayerTreeWidget::dragActiveChanged,
            this, &LayerPanel::dragActiveChanged);
    connect(m_tree, &LayerTreeWidget::layerDropRequested,
            this, &LayerPanel::applyLayerDrop);
    // Phase 18 #1b: double-clicking a row is the DELIBERATE gesture that promotes a pane to
    // the front of its stacked region — a single click must never steal the ribbon.
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        const quint64 pid = paneIdOf(it);
        if (pid) emit paneFocusRequested(pid);
    });
}

void LayerPanel::setLayerManager(LayerManager* mgr) {
    if (m_mgr) disconnect(m_mgr, nullptr, this, nullptr);
    m_mgr = mgr;
    if (!m_mgr) { m_tree->clear(); return; }

    connect(m_mgr, &LayerManager::layerAdded,   this, &LayerPanel::onLayerAdded);
    connect(m_mgr, &LayerManager::layerRemoved, this, &LayerPanel::onLayerRemoved);
    connect(m_mgr, &LayerManager::layerChanged, this, &LayerPanel::onLayerChanged);
    // Follow programmatic active-layer changes (e.g. activating a pane) so the panel's
    // highlight tracks the active layer — Phase 6.0. Suppressed while THIS panel is the
    // one driving the change, so a multi-selection is never collapsed back to one row.
    connect(m_mgr, &LayerManager::activeLayerChanged, this, [this](int idx) {
        if (m_updating || m_selecting || !m_mgr) return;
        m_updating = true;   // suppress onSelectionChanged → setActiveLayer feedback
        if (auto* it = itemForIndex(idx)) {
            // NoUpdate on BOTH paths, with the selection change spelled out separately.
            // setCurrentItem() without a flag resolves one through selectionCommand(index,
            // /*event=*/nullptr), which — having no event to read — falls back to
            // QGuiApplication::keyboardModifiers(). Under ExtendedSelection that is Toggle
            // while Ctrl is held and SelectCurrent while Shift is held, NOT the intended
            // ClearAndSelect: a Ctrl-click that lands here toggled its own row straight back
            // off. The live modifier state is accurate on X11 and stale on Windows, which is
            // why multi-select only fell apart on Linux.
            if (!it->isSelected()) {
                m_tree->clearSelection();
                it->setSelected(true);
            }
            m_tree->setCurrentItem(it, kColName, QItemSelectionModel::NoUpdate);
        } else {
            m_tree->clearSelection();
            m_tree->setCurrentItem(nullptr, kColName, QItemSelectionModel::NoUpdate);
        }
        markActiveRow(idx);   // keep the bold-active-name role (#3) in step
        // setCurrentItem above ran under m_updating, so onSelectionChanged was suppressed —
        // clear any stale pane-selection highlight and re-announce the summary by hand.
        for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
            auto* grp = m_tree->topLevelItem(g);
            const bool sel = grp->isSelected();
            for (int c = 0; c < grp->childCount(); ++c) {
                auto* row = grp->child(c);
                row->setData(kColName, kPaneSelRole, sel);
                for (int k = 0; k < row->childCount(); ++k)
                    row->child(k)->setData(kColName, kPaneSelRole, sel);
            }
        }
        m_tree->viewport()->update();
        m_updating = false;
        updateMoveButtons();
        emit selectionSummaryChanged(static_cast<int>(selectedLayerIndices().size()),
                                     static_cast<int>(selectedPaneIds().size()));
    });
    connect(m_mgr, &LayerManager::layerMoved,
            this, &LayerPanel::rebuildList, Qt::QueuedConnection);
    rebuildList();
}

void LayerPanel::setPaneColorResolver(std::function<QColor(uint64_t)> fn) {
    m_pane_color = std::move(fn);
    if (m_mgr) rebuildList();
}

void LayerPanel::refreshPaneColors() {
    if (m_mgr) rebuildList();
}

void LayerPanel::setPaneListResolver(std::function<std::vector<std::pair<quint64, QString>>()> fn) {
    m_pane_list = std::move(fn);
    if (m_mgr) rebuildList();
}

void LayerPanel::setPaneSyncResolver(std::function<FvPaneSyncInfo(quint64)> fn) {
    m_pane_sync = std::move(fn);
    if (m_mgr) rebuildList();
}

void LayerPanel::refreshPanes() {
    if (m_mgr) rebuildList();
}

QColor LayerPanel::paneColorFor(int layerIndex) const {
    if (!m_pane_color || !m_mgr) return {};
    auto l = m_mgr->layerAt(layerIndex);
    return l ? m_pane_color(l->paneId()) : QColor();
}

bool    LayerPanel::isGroupItem(const QTreeWidgetItem* item) {
    return item && item->data(kColName, kGroupRole).toBool();
}
int     LayerPanel::layerIndexOf(const QTreeWidgetItem* item) { return rowLayerIndex(item); }
quint64 LayerPanel::paneIdOf(const QTreeWidgetItem* item) {
    if (!item) return 0;
    const quint64 pid = item->data(kColName, kPaneIdRole).toULongLong();
    if (pid) return pid;
    return item->parent() ? paneIdOf(item->parent()) : 0;   // subdataset combo grandchild
}

void LayerPanel::rebuildList() {
    // Remember which groups were collapsed and what was selected (rows AND pane headers),
    // so a rebuild — triggered by any layer/pane change — does not silently expand
    // everything or drop the selection the user is about to act on.
    QSet<quint64> collapsed;
    QSet<int>     selected;
    QSet<quint64> selectedPanes;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        auto* grp = m_tree->topLevelItem(g);
        if (!grp->isExpanded())  collapsed.insert(paneIdOf(grp));
        if (grp->isSelected())   selectedPanes.insert(paneIdOf(grp));
        for (int c = 0; c < grp->childCount(); ++c)
            if (grp->child(c)->isSelected()) selected.insert(rowLayerIndex(grp->child(c)));
    }

    m_updating = true;
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    if (!m_mgr) {
        m_tree->setUpdatesEnabled(true);
        m_updating = false;
        updateMoveButtons();
        return;
    }

    // Pane order comes from the layout (so the panel reads top-left → bottom-right like the
    // canvas); every pane gets a group even with no layers, so an empty pane stays visible,
    // droppable and closable (user decision, Phase 18 #6).
    std::vector<uint64_t> paneOrder;
    QHash<quint64, QString> paneLabels;
    if (m_pane_list)
        for (const auto& [pid, label] : m_pane_list()) {
            paneOrder.push_back(pid);
            paneLabels.insert(pid, label);
        }
    const auto groups = fvGroupLayersByPane(m_mgr->layers(), paneOrder);

    for (const auto& grp : groups) {
        const QColor paneCol = m_pane_color ? m_pane_color(grp.paneId) : QColor();
        auto* header = new QTreeWidgetItem();
        header->setText(kColName, paneLabels.value(grp.paneId,
                                                   tr("Pane %1").arg(grp.paneId)));
        header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
        header->setData(kColName, kLayerIndexRole, -1);
        header->setData(kColName, kGroupRole,      true);
        header->setData(kColName, kPaneIdRole,     static_cast<qulonglong>(grp.paneId));
        header->setData(kColName, kPaneColorRole,  paneCol);
        // Sync state drives the badge PaneColorDelegate paints on the header's trailing edge.
        const FvPaneSyncInfo sync = m_pane_sync ? m_pane_sync(grp.paneId) : FvPaneSyncInfo{};
        header->setData(kColName, kSyncRoleRole,  sync.role);
        header->setData(kColName, kSyncColorRole, sync.masterColor);
        m_tree->addTopLevelItem(header);
        header->setFirstColumnSpanned(true);   // header band spans all three columns
        header->setExpanded(!collapsed.contains(grp.paneId));
        header->setSelected(selectedPanes.contains(grp.paneId));
        QStringList headerTips;
        if (grp.layerIndices.empty())
            headerTips << tr("This pane has no layers — drop one here.");
        if (sync.role) headerTips << fvSyncRoleTooltip(sync);
        if (!headerTips.isEmpty())
            header->setToolTip(kColName, headerTips.join('\n'));

        for (auto it = grp.layerIndices.rbegin(); it != grp.layerIndices.rend(); ++it) {
            int i = *it;
            auto layer = m_mgr->layerAt(i);
            if (!layer) continue;
            auto* item = new QTreeWidgetItem(header);
            item->setText(kColName, layer->name());
            QString srcPath = layer->sourceFilePath();
            item->setToolTip(kColName, srcPath.isEmpty() ? layer->name() : srcPath);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                           | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            item->setData(kColName, kLayerIndexRole, i);
            item->setData(kColName, kGroupRole,      false);
            item->setData(kColName, kPaneIdRole,     static_cast<qulonglong>(grp.paneId));
            item->setData(kColName, kPaneColorRole,  paneCol);   // drives PaneColorDelegate
            item->setData(kColName, kActiveRole, i == m_mgr->activeIndex());  // bold if active (#3)
            item->setData(kColName, kPaneSelRole, selectedPanes.contains(grp.paneId));

            // Up / Down arrow buttons to move layers up or down the stack
            auto* orderWrap = new QWidget(m_tree);
            auto* orderLay  = new QHBoxLayout(orderWrap);
            orderLay->setContentsMargins(0, 0, 0, 0);
            orderLay->setSpacing(1);

            auto* btnUp   = new SvgIconButton(orderWrap);
            auto* btnDown = new SvgIconButton(orderWrap);
            btnUp->setObjectName("layerRowUpBtn");
            btnDown->setObjectName("layerRowDownBtn");
            btnUp->setFixedSize(16, 16);
            btnDown->setFixedSize(16, 16);

            bool isDark = palette().base().color().lightness() < 128;
            QString sfx = isDark ? "_dark" : "_light";
            btnUp->setSvgPath(":/icons/arrow_up" + sfx + ".svg");
            btnDown->setSvgPath(":/icons/arrow_down" + sfx + ".svg");

            const int layerIdx = i;
            const bool canUp   = fvPaneNeighbourIndex(m_mgr->layers(), layerIdx, +1) >= 0;
            const bool canDown = fvPaneNeighbourIndex(m_mgr->layers(), layerIdx, -1) >= 0;
            btnUp->setEnabled(canUp);
            btnDown->setEnabled(canDown);
            btnUp->setToolTip(tr("Move layer up in stack"));
            btnDown->setToolTip(tr("Move layer down in stack"));

            connect(btnUp, &QToolButton::clicked, this, [this, layerIdx] {
                if (m_updating || !m_mgr) return;
                int to = fvPaneNeighbourIndex(m_mgr->layers(), layerIdx, +1);
                if (to >= 0) m_mgr->moveLayer(layerIdx, to);
            });
            connect(btnDown, &QToolButton::clicked, this, [this, layerIdx] {
                if (m_updating || !m_mgr) return;
                int to = fvPaneNeighbourIndex(m_mgr->layers(), layerIdx, -1);
                if (to >= 0) m_mgr->moveLayer(layerIdx, to);
            });

            orderLay->addStretch();
            orderLay->addWidget(btnUp);
            orderLay->addWidget(btnDown);
            orderLay->addStretch();
            m_tree->setItemWidget(item, kColOrder, orderWrap);

            // Visibility checkbox as an item widget (so it can be tinted with the pane
            // colour); centred in the column.
            auto* vis = new FvTickCheckBox(m_tree);
            vis->setAccent(paneCol);
            vis->setChecked(layer->visible());
            auto* visWrap = new QWidget(m_tree);
            auto* visLay  = new QHBoxLayout(visWrap);
            visLay->setContentsMargins(0, 0, 0, 0);
            visLay->addStretch();
            visLay->addWidget(vis);
            visLay->addStretch();
            m_tree->setItemWidget(item, kColVis, visWrap);
            const int visIdx = i;
            connect(vis, &QCheckBox::toggled, this, [this, visIdx](bool on) {
                if (m_updating || !m_mgr) return;
                auto l = m_mgr->layerAt(visIdx);
                if (l) { l->setVisible(on); m_mgr->notifyLayerChanged(visIdx); }
            });

            // Opacity slider as item widget
            auto* slider = new QSlider(Qt::Horizontal, m_tree);
            slider->setRange(0, 100);
            slider->setValue(static_cast<int>(layer->opacity() * 100.f));
            const int capturedIdx = i;
            connect(slider, &QSlider::valueChanged, this, [this, capturedIdx](int val) {
                if (m_updating || !m_mgr) return;
                auto l = m_mgr->layerAt(capturedIdx);
                if (!l) return;
                l->setOpacity(val / 100.f);
                // Couple visibility to opacity (Phase 16 #5): opacity 0 ⇔ Vis off, opacity > 0 ⇔
                // Vis on. Unconditional so raising opacity from 0 restores visibility (and drops
                // it at 0). onLayerChanged re-syncs the Vis checkbox from visible().
                const bool vis = (val > 0);
                if (l->visible() != vis) l->setVisible(vis);
                m_mgr->notifyLayerChanged(capturedIdx);
            });
            slider->setToolTip(QString("%1%").arg(slider->value()));
            connect(slider, &QSlider::valueChanged, slider, [slider](int val) {
                slider->setToolTip(QString("%1%").arg(val));
                QToolTip::showText(QCursor::pos(), slider->toolTip(), slider);
            });
            m_tree->setItemWidget(item, kColOp, slider);

            // Tint the opacity slider with the FULL pane colour (Phase 6.2). The Vis
            // checkbox needs no stylesheet — FvTickCheckBox paints its own pane-coloured
            // tick box (Phase 18 follow-up #4), matching the selection indicator.
            if (paneCol.isValid()) {
                slider->setStyleSheet(QString(
                    "QSlider::sub-page:horizontal { background:%1; }"
                    "QSlider::handle:horizontal { background:%1; border:none;"
                    " width:10px; margin:-3px 0; border-radius:5px; }").arg(paneCol.name()));
            }

            // For subdataset layers: add a child row with a variable selector combo
            if (layer->type() == LayerType::Raster) {
                auto* rl = static_cast<RasterLayer*>(layer.get());
                if (rl->hasSubdatasets()) {
                    auto* child = new QTreeWidgetItem(item);
                    child->setFlags(Qt::ItemIsEnabled);   // not selectable/draggable
                    child->setData(kColName, kLayerIndexRole, -1);
                    child->setData(kColName, kGroupRole,     false);
                    child->setData(kColName, kPaneIdRole,    static_cast<qulonglong>(grp.paneId));
                    child->setData(kColName, kPaneColorRole, paneCol);
                    child->setData(kColName, kPaneSelRole,
                                   selectedPanes.contains(grp.paneId));
                    child->setFirstColumnSpanned(true);

                    auto* combo = new FvVariableComboBox(m_tree);
                    for (const auto& [name, desc] : rl->subdatasets()) {
                        combo->addItem(QString::fromStdString(desc.empty() ? name : desc));
                    }
                    combo->setCurrentIndex(rl->subdatasetIndex());
                    m_tree->setItemWidget(child, kColName, combo);
                    item->setExpanded(true);

                    const int layerIdx = i;
                    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                            this, [this, layerIdx](int newSubIdx) {
                        if (m_updating || !m_mgr) return;
                        auto l = m_mgr->layerAt(layerIdx);
                        if (!l || l->type() != LayerType::Raster) return;
                        auto* rl2 = static_cast<RasterLayer*>(l.get());
                        rl2->switchSubdataset(newSubIdx);
                        // Update parent item text
                        auto* parentItem = itemForIndex(layerIdx);
                        if (parentItem) parentItem->setText(kColName, l->name());
                        emit layerDatasetChanged(layerIdx);
                    });
                }
            }
        }
    }

    // Restore the current row, then the selection — in that order and never the other way
    // round. NoUpdate keeps setCurrentItem() from issuing a selection command of its own:
    // without a flag it derives one from QGuiApplication::keyboardModifiers() (Toggle under
    // Ctrl, SelectCurrent under Shift, ClearAndSelect otherwise), so a rebuild landing while
    // the user holds a modifier rewrote the very selection this block exists to preserve.
    if (m_mgr->count() > 0) {
        const int ai = m_mgr->activeIndex();
        if (auto* it = itemForIndex(ai))
            m_tree->setCurrentItem(it, kColName, QItemSelectionModel::NoUpdate);
    }
    for (int i : selected)
        if (auto* it = itemForIndex(i)) it->setSelected(true);
    // Pane headers too. They were selected as they were built above, but a ClearAndSelect
    // from setCurrentItem used to wipe them here while the child rows kept kPaneSelRole —
    // the rows stayed highlighted, selectedPaneIds() went empty, and the trash button then
    // acted on a different set than the panel was showing.
    for (quint64 pid : std::as_const(selectedPanes))
        if (auto* grp = groupItemFor(pid)) grp->setSelected(true);

    m_tree->setUpdatesEnabled(true);
    m_updating = false;
    updateMoveButtons();
}

QTreeWidgetItem* LayerPanel::itemForIndex(int index) const {
    if (index < 0) return nullptr;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        auto* grp = m_tree->topLevelItem(g);
        for (int c = 0; c < grp->childCount(); ++c)
            if (rowLayerIndex(grp->child(c)) == index) return grp->child(c);
    }
    return nullptr;
}

QTreeWidgetItem* LayerPanel::groupItemFor(quint64 paneId) const {
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g)
        if (m_tree->topLevelItem(g)->data(kColName, kPaneIdRole).toULongLong() == paneId)
            return m_tree->topLevelItem(g);
    return nullptr;
}

void LayerPanel::onLayerAdded(int) { rebuildList(); }
void LayerPanel::onLayerRemoved(int) { rebuildList(); }

void LayerPanel::onLayerChanged(int index) {
    if (m_updating || !m_mgr) return;
    auto* item = itemForIndex(index);
    if (!item) return;
    auto layer = m_mgr->layerAt(index);
    if (!layer) return;
    // A layer that moved pane must re-group — a targeted row update cannot express that.
    if (paneIdOf(item) != layer->paneId()) { rebuildList(); return; }
    m_updating = true;
    item->setText(kColName, layer->name());
    if (auto* w = m_tree->itemWidget(item, kColVis))
        if (auto* cb = w->findChild<QCheckBox*>())
            cb->setChecked(layer->visible());
    if (auto* sl = qobject_cast<QSlider*>(m_tree->itemWidget(item, kColOp)))
        sl->setValue(static_cast<int>(layer->opacity() * 100.f));
    m_updating = false;
}

void LayerPanel::onItemChanged(QTreeWidgetItem* item, int col) {
    if (m_updating || !m_mgr || col != kColName || !item) return;
    if (isGroupItem(item)) return;    // header check state is auto-derived from its children
    const int idx = layerIndexOf(item);
    if (idx < 0 || idx >= m_mgr->count()) return;
    auto l = m_mgr->layerAt(idx);
    if (!l) return;

    if (item->text(kColName) != l->name())       // inline rename (F2)
        { l->setName(item->text(kColName)); m_mgr->notifyLayerChanged(idx); }
}

void LayerPanel::onSelectionChanged() {
    if (m_updating) return;
    // Selecting a pane group highlights every layer under it, in that pane's colour. Only
    // the header is in Qt's selection model — the rows follow via kPaneSelRole, so a plain
    // click on a single row still clears everything the way ExtendedSelection expects.
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        auto* grp = m_tree->topLevelItem(g);
        const bool sel = grp->isSelected();
        for (int c = 0; c < grp->childCount(); ++c) {
            auto* row = grp->child(c);
            if (row->data(kColName, kPaneSelRole).toBool() != sel)
                row->setData(kColName, kPaneSelRole, sel);
            for (int k = 0; k < row->childCount(); ++k)   // subdataset combo row
                row->child(k)->setData(kColName, kPaneSelRole, sel);
        }
    }
    m_tree->viewport()->update();

    const auto layers = selectedLayerIndices();
    if (layers.size() == 1) {
        // Exactly one layer selected → it becomes the active layer. m_selecting must cover
        // the emitted signal too: MainWindow bounces activeLayerChanged straight back into
        // LayerManager::setActiveLayer, and the handler below would then ClearAndSelect that
        // one row — wiping a selection that also holds pane groups (e.g. one pane with a
        // layer plus panes with none). MainWindow still activates the owning pane, but
        // WITHOUT bringing it to the front of a stacked region (Phase 18 #1).
        m_selecting = true;
        if (m_mgr) m_mgr->setActiveLayer(layers.front());
        emit activeLayerChanged(layers.front());
        m_selecting = false;
        markActiveRow(layers.front());
    }
    emit selectionSummaryChanged(static_cast<int>(layers.size()),
                                 static_cast<int>(selectedPaneIds().size()));
    updateMoveButtons();
}

// The bold name marks the ACTIVE layer (#3), and has to be re-stamped wherever the active
// layer changes -- including when this panel is what changed it. The `activeLayerChanged`
// handler above cannot cover that case: it returns early under `m_selecting`, the guard that
// stops a panel-driven activation from collapsing a multi-row selection, and the early return
// took the bold with it. So a click in the Layers panel activated the layer everywhere else in
// the application and left its own row unbolded.
void LayerPanel::markActiveRow(int activeIndex) {
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        auto* grp = m_tree->topLevelItem(g);
        for (int c = 0; c < grp->childCount(); ++c) {
            auto* row = grp->child(c);
            const bool active = rowLayerIndex(row) == activeIndex;
            if (row->data(kColName, kActiveRole).toBool() != active)
                row->setData(kColName, kActiveRole, active);
        }
    }
    m_tree->viewport()->update();
}

std::vector<int> LayerPanel::selectedLayerIndices() const {
    // Directly highlighted rows, PLUS every layer of a highlighted pane group — selecting a
    // pane means "this pane and its contents". Panes and layers from other panes mix freely
    // in one Shift/Ctrl selection; duplicates are folded out.
    std::vector<int> out;
    auto add = [&out](int idx) {
        if (idx >= 0 && std::find(out.begin(), out.end(), idx) == out.end()) out.push_back(idx);
    };
    for (auto* it : m_tree->selectedItems()) add(rowLayerIndex(it));
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        auto* grp = m_tree->topLevelItem(g);
        if (!grp->isSelected()) continue;
        for (int c = 0; c < grp->childCount(); ++c) add(rowLayerIndex(grp->child(c)));
    }
    return out;
}

std::vector<quint64> LayerPanel::selectedPaneIds() const {
    std::vector<quint64> out;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        auto* grp = m_tree->topLevelItem(g);
        if (grp->isSelected()) out.push_back(grp->data(kColName, kPaneIdRole).toULongLong());
    }
    return out;
}

void LayerPanel::applyLayerDrop(int from, quint64 targetPaneId, int insertBefore) {
    if (!m_mgr) return;
    auto l = m_mgr->layerAt(from);
    if (!l) return;
    // Pane first (the index is still valid), then the reorder — assignLayerToPane does not
    // touch the list order, so `from` survives it.
    if (targetPaneId && l->paneId() != targetPaneId)
        emit paneAssignmentRequested(from, targetPaneId);
    if (insertBefore >= 0) {
        const int to = fvMoveTargetIndex(from, insertBefore, m_mgr->count());
        if (to != from) { m_mgr->moveLayer(from, to); return; }   // layerMoved → rebuildList
    }
    rebuildList();
}

void LayerPanel::deleteSelection() {
    if (!m_mgr) return;
    // The HIGHLIGHTED rows are the removal set — a selected pane group contributes all of
    // its layers (see selectedLayerIndices), so "select a pane, press Delete" is exactly
    // "remove this pane's contents", and the prompt below decides the pane's own fate.
    const std::vector<int> targets = fvRemovalOrder(selectedLayerIndices());

    // Panes that would end up with no layers at all — plus any selected pane that is
    // already layer-less (nothing to remove there, so "close it" is the only intent).
    // NOTE the element type: `uint64_t`, matching `Layer::paneId()` and the return of
    // fvPanesEmptiedBy — NOT `quint64`. The two are the same width everywhere, but on
    // LP64 (Linux/macOS) `uint64_t` is `unsigned long` while `quint64` is `unsigned long
    // long`: distinct types, so `vector<uint64_t>` does not convert to `vector<quint64>`.
    // MSVC makes both `unsigned long long`, which is why this only broke the GCC build.
    // Individual ids still convert implicitly, so pushing/emitting quint64 below is fine.
    std::vector<uint64_t> emptied = fvPanesEmptiedBy(m_mgr->layers(), targets);
    for (quint64 pid : selectedPaneIds()) {
        auto* grp = groupItemFor(pid);
        if (grp && grp->childCount() == 0
            && std::find(emptied.begin(), emptied.end(), pid) == emptied.end())
            emptied.push_back(pid);
    }
    if (targets.empty() && emptied.empty()) return;

    bool closePanes = false;
    if (!emptied.empty()) {
        // QoL (#2): removing every layer of a pane is ambiguous — the user may want to keep
        // the now-empty pane (to drop something else into) or be rid of it. Ask, don't guess.
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Remove"));
        QPushButton* keep = nullptr;
        if (targets.empty()) {
            box.setText(tr("Close %n empty pane(s)?", "", static_cast<int>(emptied.size())));
        } else {
            box.setText(tr("Remove %n layer(s)?", "", static_cast<int>(targets.size())));
            box.setInformativeText(
                tr("That empties %n pane(s). Keep the empty pane(s), or close them too?",
                   "", static_cast<int>(emptied.size())));
            keep = box.addButton(tr("Remove Layers Only"), QMessageBox::AcceptRole);
        }
        auto* both = box.addButton(targets.empty() ? tr("Close Pane(s)")
                                                   : tr("Remove and Close Pane(s)"),
                                   QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(keep ? keep : both);
        box.exec();
        if (box.clickedButton() == both)      closePanes = true;
        else if (box.clickedButton() != keep) return;      // Cancel / dialog closed
    } else if (targets.size() > 1) {
        if (QMessageBox::question(this, tr("Remove"),
                tr("Remove %n layer(s)?", "", static_cast<int>(targets.size())),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    for (int idx : targets) m_mgr->removeLayer(idx);
    // Closing the LAST pane is refused upstream (MainWindow::closePane clears it instead),
    // so FR-PNE-11's never-zero-panes rule still holds.
    if (closePanes)
        for (quint64 pid : emptied) emit paneCloseRequested(pid);
}

void LayerPanel::updateMoveButtons() {
    const auto layers = selectedLayerIndices();
    const auto panes  = selectedPaneIds();
    m_btn_del->setEnabled(!layers.empty() || !panes.empty());
    // Reordering is a single-layer act: disabled for a multi-selection and whenever a whole
    // pane is selected (there is no meaningful "move this pane up" in the layer stack).
    if (layers.size() != 1 || !panes.empty() || !m_mgr) {
        m_btn_up->setEnabled(false);
        m_btn_down->setEnabled(false);
        return;
    }
    m_btn_up->setEnabled(fvPaneNeighbourIndex(m_mgr->layers(), layers.front(), +1) >= 0);
    m_btn_down->setEnabled(fvPaneNeighbourIndex(m_mgr->layers(), layers.front(), -1) >= 0);
}

void LayerPanel::onContextMenu(const QPoint& pos) {
    if (!m_mgr) return;
    auto* item = m_tree->itemAt(pos);
    if (!item) return;

    // --- Pane-group header menu (Phase 18 #6/#8) ---
    if (isGroupItem(item)) {
        const quint64 pid = paneIdOf(item);
        QMenu menu(this);
        auto* actShow  = menu.addAction(tr("Show This Pane"));
        auto* actClose = menu.addAction(tr("Close Pane"));
        auto* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
        if (!chosen) return;
        if (chosen == actShow)       emit paneFocusRequested(pid);
        else if (chosen == actClose) {
            if (QMessageBox::question(this, tr("Close Pane"),
                    tr("Close pane \"%1\"? Its layers move to another pane.")
                        .arg(item->text(kColName)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                emit paneCloseRequested(pid);
        }
        return;
    }

    int idx = layerIndexOf(item);
    if (idx < 0 && item->parent()) idx = layerIndexOf(item->parent());   // subdataset combo row
    if (idx < 0 || idx >= m_mgr->count()) return;

    // Multi-selection: offer the batch action only, so a right-click cannot silently act on
    // just one of several selected layers.
    const auto selection = selectedLayerIndices();
    if (selection.size() > 1) {
        QMenu menu(this);
        auto* actDel = menu.addAction(tr("Remove %n Selected Layer(s)", "",
                                         static_cast<int>(selection.size())));
        if (menu.exec(m_tree->viewport()->mapToGlobal(pos)) == actDel) deleteSelection();
        return;
    }

    QMenu menu(this);
    auto* actSettings = menu.addAction(tr("Layer Settings…"));
    auto* actRename   = menu.addAction(tr("Rename"));
    auto* actRemove   = menu.addAction(tr("Remove"));
    auto* actFit      = menu.addAction(tr("Fit to Layer"));

    // NOTE: no "Show Colorbar" here — colorbar visibility is a PANE setting, offered by the
    // pane gear menu alone (user decision).

    // "To Pane" submenu (Phase 6.3): reassign this layer to a pane (exclusive; the layer's
    // current pane is tick-marked).
    QHash<QAction*, quint64> paneActions;
    if (m_pane_list) {
        const auto panes = m_pane_list();
        if (!panes.empty()) {
            const quint64 cur = m_mgr->layerAt(idx) ? m_mgr->layerAt(idx)->paneId() : 0;
            auto* sub = menu.addMenu(tr("To Pane"));
            auto* actAll = sub->addAction(tr("All Panes (Global Overlay)"));
            actAll->setCheckable(true);
            actAll->setChecked(cur == kAllPanesId);
            paneActions.insert(actAll, kAllPanesId);
            sub->addSeparator();
            for (const auto& [pid, label] : panes) {
                auto* a = sub->addAction(label);
                a->setCheckable(true);
                a->setChecked(pid == cur);
                paneActions.insert(a, pid);
            }
        }
    }

    auto* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == actSettings) {
        emit layerSettingsRequested(idx);
        return;
    }

    if (auto it = paneActions.constFind(chosen); it != paneActions.constEnd()) {
        emit paneAssignmentRequested(idx, it.value());
        return;
    }

    if (chosen == actRename) {
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"),
            tr("Name:"), QLineEdit::Normal, m_mgr->layerAt(idx)->name(), &ok);
        if (ok && !name.isEmpty()) {
            m_mgr->layerAt(idx)->setName(name);
            m_mgr->notifyLayerChanged(idx);
        }
    } else if (chosen == actRemove) {
        m_mgr->removeLayer(idx);
    } else if (chosen == actFit) {
        emit fitToLayerRequested(idx);
    }
}

void LayerPanel::resizeHeaderColumns() {
    // The Vis section must clear TWO things, and the old `plainAdvance("Vis") + 16` cleared
    // neither reliably:
    //   1. the header label, which the stylesheet renders SEMI-BOLD inside padding
    //      (QHeaderView::section { font-weight: 600; padding: 4px 6px; border-right: 1px }).
    //      header()->font() reports none of that, so measuring with it under-sizes the
    //      section — worse on Linux's wider "Sans Serif" than on Windows.
    //   2. the visibility tick, which needs clear space either side or it reads as part of
    //      the neighbouring (stretched) Layer column instead of owning its own.
    // On Linux the old formula gave 34 px — 20 px of semi-bold label into 21 px of usable
    // width, and only 9 px around the tick, which is what looked "buried" in the name.
    QFont headerFont = m_tree->header()->font();
    headerFont.setWeight(QFont::DemiBold);
    const int kSectionChrome = 2 * 6 + 1;    // QSS horizontal padding + right border
    const int kTickClearance = 13;           // breathing room each side of the tick
    const int headerNeed = QFontMetrics(headerFont).horizontalAdvance(tr("Vis"))
                           + kSectionChrome;
    const int tickNeed   = kFvTickBoxSide + 2 * kTickClearance;
    m_tree->header()->resizeSection(kColOrder, 38);
    m_tree->header()->resizeSection(kColVis, std::max(headerNeed, tickNeed));
    m_tree->header()->resizeSection(kColOp, 80);
}

void LayerPanel::updateArrowIcons() {
    bool isDark = palette().base().color().lightness() < 128;
    QString sfx = isDark ? "_dark" : "_light";
    m_btn_up->setSvgPath(":/icons/arrow_up"   + sfx + ".svg");
    m_btn_down->setSvgPath(":/icons/arrow_down" + sfx + ".svg");
    m_btn_del->setSvgPath(":/icons/trash"     + sfx + ".svg");

    for (auto* btn : m_tree->findChildren<SvgIconButton*>()) {
        if (btn->objectName() == "layerRowUpBtn")
            btn->setSvgPath(":/icons/arrow_up" + sfx + ".svg");
        else if (btn->objectName() == "layerRowDownBtn")
            btn->setSvgPath(":/icons/arrow_down" + sfx + ".svg");
    }
}

void LayerPanel::changeEvent(QEvent* e) {
    if (e->type() == QEvent::PaletteChange) updateArrowIcons();
    if (e->type() == QEvent::StyleChange || e->type() == QEvent::FontChange)
        QMetaObject::invokeMethod(this, &LayerPanel::resizeHeaderColumns, Qt::QueuedConnection);
    QWidget::changeEvent(e);
}
