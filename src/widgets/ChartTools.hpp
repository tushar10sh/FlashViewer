#pragma once
// Shared chart furniture used by BOTH plot windows (Phase 26.1), so the Spectral Plot and
// the Scan/Pixel Profile navigate and export identically — the same reason the check
// indicator and the section frame live in UiKit (FR-APP-15).
//
//  • FvChartView    — a QChartView that pans on left-drag and zooms on the wheel, with a
//                     remembered "home" range to return to.
//  • FvChartToolbar — the ⊕ / ⊖ / ⌂ buttons, the drag-to-pan hint and Save (PNG / SVG / CSV).
//  • FvChartLegend  — the legend, replacing QChart's own. QLegend lays its markers out
//                     horizontally, elides each to a single line, and draws a plain colour
//                     block — none of which survives contact with a label like
//                     "Pane 1 / Landsat_TOA_BT_100m.tif (481337.45, 2173212.10)", or with
//                     FR-ANL-8's dash patterns.
// Every label a plot draws is edited by RIGHT-CLICKING it: the title, each axis title
// (FvChartView::labelContextMenuRequested) and each legend entry (FvChartLegend's own row
// menu). No ✎ buttons: one per legend row crowded the entries off their own column, and a
// toolbar button for "the labels" could not say WHICH label it would edit.

#include <QChartView>
#include <QHash>
#include <QString>
#include <QWidget>
#include <functional>

class QChart;
class QEvent;
class QVBoxLayout;
class QMouseEvent;
class QWheelEvent;
class SvgIconButton;
class QLabel;

/// QChartView with direct navigation. Zoom and pan are applied to the axes' ranges rather
/// than through QChart::zoom()/zoomReset(): the plots rebuild their axes on every refresh,
/// which discards the chart's internal zoom stack, so `zoomReset()` would silently do
/// nothing after a replot. Ranges we captured ourselves survive that.
/// Which label a right-click landed on. The bands around the plot area are what is hit-tested
/// — QChart draws its title and axis titles itself and exposes no items to pick.
enum class FvChartLabel { Title = 0, XAxis = 1, YAxis = 2 };

class FvChartView : public QChartView {
    Q_OBJECT
public:
    explicit FvChartView(QChart* chart, QWidget* parent = nullptr);

    /// Record the axes' current ranges as "home". Call it right after (re)building the axes;
    /// the view keys them per axis, so an axis replaced on the next replot simply has no home
    /// until it is captured again.
    void captureHome();

    /// Multiply the visible span by `factor` (<1 zooms in) about the plot-area centre.
    void zoomBy(double factor);

public slots:
    void zoomIn()    { zoomBy(1.0 / kStep); }
    void zoomOut()   { zoomBy(kStep); }
    void resetZoom();

signals:
    /// Emitted whenever the view is zoomed or panned, and on reset. Lets an owner enable or
    /// disable a "home" button, or annotate that the view is no longer auto-fitted.
    void viewChanged(bool atHome);

    /// The user right-clicked the plot title or an axis title. The owner opens the menu,
    /// because only it knows the automatic text its "Edit" would offer to restore.
    void labelContextMenuRequested(FvChartLabel which, const QPoint& globalPos);

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    static constexpr double kStep = 1.25;   // one wheel notch / one button press

    void panByPixels(double dx, double dy);
    void emitViewChanged();

    struct Home { double min{0}, max{0}; bool valid{false}; };
    QHash<QObject*, Home> m_home;      // axis → its captured range
    QPointF m_last_pos;
    bool    m_dragging{false};
};

/// Horizontal breathing room between the chart toolbar's icon cluster and each trailing
/// control. Exported because the Scan/Pixel Profile separates its own controls by the same
/// amount — one number, so the two panels cannot drift apart (FR-APP-15).
inline constexpr int kFvChartTrailingGap = 8;

/// One curve as the legend shows it.
struct FvLegendEntry {
    QString      key;                    ///< opaque, chosen by the owner; identifies a rename
    QString      text;                   ///< may contain newlines; the label wraps as well
    QColor       color;
    Qt::PenStyle style{Qt::SolidLine};
    bool         renamable{true};        ///< offer Edit in the row's right-click menu
    bool         deletable{true};        ///< offer Delete in the row's right-click menu
};

/// Vertical legend, sitting to the RIGHT of the chart. Each row is a swatch that carries the
/// curve's colour AND pen style (fvPaintCurveSwatch) plus a word-wrapping label; Edit and
/// Delete live in the row's RIGHT-CLICK menu. Scrolls when there are more curves than fit.
///
/// The buttons used to sit in the row. They cost ~46 px of a ~240 px column, which is most of
/// what a two-line "«pane» / «file» (x, y)" entry needs, and squeezing the label that hard is
/// what made the legend unreadable.
class FvChartLegend : public QWidget {
    Q_OBJECT
public:
    explicit FvChartLegend(QWidget* parent = nullptr);
    void setEntries(const QVector<FvLegendEntry>& entries);
    int  entryCount() const { return m_entries.size(); }

public slots:
    /// Delete row `index` exactly as its context menu would. Public because the menu is modal
    /// and cannot be driven headlessly — a test needs the owner's delete path, not QMenu's.
    void deleteEntry(int index);

signals:
    /// The user renamed `key`. An EMPTY string means "reset to the automatic text" — the
    /// only way back once a label has been overridden.
    void entryRenamed(const QString& key, const QString& text);

    /// The user deleted the entry at `index` — its position in the vector last passed to
    /// setEntries(). Deliberately positional rather than keyed: a rename key names the LAYER
    /// (so an edit outlives the next click rebuilding the curves), while a delete has to name
    /// one CURVE, and one layer can contribute several.
    void entryDeleted(int index);

protected:
    void changeEvent(QEvent* e) override;
    /// Re-elides the entries: how much of a long name fits depends on the column's width.
    void resizeEvent(QResizeEvent* e) override;

private:
    void rebuild();
    /// Row `index` was right-clicked — offer Edit / Delete.
    void showRowMenu(int index, const QPoint& globalPos);
    void promptRename(int index);
    /// Width available to a row's label, in pixels.
    int  labelWidth() const;

    QVBoxLayout*          m_rows{nullptr};
    QVector<FvLegendEntry> m_entries;
};

/// Prompt for ONE label. `current` is what the user has typed before (empty when the automatic
/// text is in force) and `automatic` is what clearing the field restores — shown in the prompt,
/// since a single-line QInputDialog has nowhere to put a placeholder. Returns false on Cancel;
/// on OK `current` holds the new value, empty meaning "back to the automatic text".
bool fvPromptChartLabel(QWidget* parent, const QString& what, const QString& automatic,
                        QString& current);

/// The button row above a chart: zoom in / zoom out / home, a hand icon captioning the
/// drag-to-pan gesture, and Save. Labels are edited by right-clicking them, not from here.
/// `chartView` must outlive the toolbar.
///
/// Save always offers PNG and SVG (rendered from the view, so what is saved is what is on
/// screen, theme included). CSV appears only when the owner supplies a provider — the
/// toolbar has no idea what the curves mean, and a CSV of "whatever is plotted" is the one
/// export that cannot be produced generically.
class FvChartToolbar : public QWidget {
    Q_OBJECT
public:
    explicit FvChartToolbar(FvChartView* chartView, QWidget* parent = nullptr);

    /// Text of the CSV to write, produced on demand. Return an empty string to mean "nothing
    /// to export"; leave unset to drop CSV from the save dialog entirely.
    void setCsvProvider(std::function<QString()> provider);

    /// Base name offered in the save dialog (no extension). Defaults to "plot".
    void setSuggestedName(const QString& baseName) { m_suggested = baseName; }

    /// Append a widget after the built-in buttons (e.g. the Spectral Plot's Persist/Clear).
    void addTrailingWidget(QWidget* w);

    /// What PNG/SVG export renders. Defaults to the chart view alone; a panel with a separate
    /// legend widget must pass the CONTAINER holding both, or the exported image silently
    /// loses the legend.
    void setExportWidget(QWidget* w) { m_export = w; }

protected:
    /// Icons are per-theme assets; re-resolve them on a palette switch.
    void changeEvent(QEvent* e) override;

private:
    void refreshIcons();
    void save();

    FvChartView*   m_view{nullptr};
    SvgIconButton* m_zoom_in{nullptr};
    SvgIconButton* m_zoom_out{nullptr};
    SvgIconButton* m_home{nullptr};
    SvgIconButton* m_save{nullptr};
    QWidget*       m_export{nullptr};   // null ⇒ the chart view alone
    QLabel*        m_hand{nullptr};
    class QHBoxLayout* m_layout{nullptr};

    bool m_has_trailing{false};

    std::function<QString()> m_csv;
    QString                  m_suggested{QStringLiteral("plot")};
};
