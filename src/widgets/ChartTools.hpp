#pragma once
// Shared chart furniture used by BOTH plot windows (Phase 26.1), so the Spectral Plot and
// the Scan/Pixel Profile navigate and export identically — the same reason the check
// indicator and the section frame live in UiKit (FR-APP-15).
//
//  • FvChartView    — a QChartView that pans on left-drag and zooms on the wheel, with a
//                     remembered "home" range to return to.
//  • FvChartToolbar — the ⊕ / ⊖ / ⌂ buttons, the drag-to-pan hint, the ✎ that opens the
//                     labels dialog, Save (PNG / SVG / CSV), and the Grid toggle.
//  • FvChartLegend  — the legend, replacing QChart's own. QLegend lays its markers out
//                     horizontally, elides each to a single line, and draws a plain colour
//                     block — none of which survives contact with a label like
//                     "Pane 1 / Landsat_TOA_BT_100m.tif (481337.45, 2173212.10)", or with
//                     FR-ANL-8's dash patterns.
//  • fvEditPlotLabels — ONE dialog behind the ✎ for everything a plot labels: the title, the
//                     axis titles, the axis tick ranges, and every legend entry. The legend
//                     column itself carries no buttons — a ✎ and a 🗑 per row took ~46 px of a
//                     ~240 px column and the entries became unreadable.

#include <QChartView>
#include <QMargins>
#include <QSplitter>
#include <QHash>
#include <QString>
#include <QWidget>
#include <functional>

class QChart;
QT_FORWARD_DECLARE_CLASS(QValueAxis)
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

protected:
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

/// What both charts set QChart::setMargins to. Qt defaults to 20 on every side, which spent
/// ~80 px of the panel on nothing — most visibly as a gap between the plot and the legend. The
/// RIGHT side keeps a little more than the rest: the splitter handle sits immediately there,
/// and a plot flush against it reads as if it had been cut off.
inline constexpr QMargins kFvChartMargins{4, 4, 10, 4};

/// Legend type size, in points. Absolute rather than a fraction of the panel font so the two
/// plots agree whatever the desktop is set to, and small because a legend annotates the plot
/// rather than competing with it — the smaller face is also what lets a long file name fit.
inline constexpr double kFvLegendPointSize = 7.5;

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

/// Vertical legend, sitting to the RIGHT of the chart: one row per curve, a swatch carrying
/// the curve's colour AND pen style (fvPaintCurveSwatch) beside a word-wrapping label, and
/// nothing else. Renaming, hiding and deleting all live in the ✎ dialog (fvEditPlotLabels) —
/// per-row buttons cost ~46 px of a ~240 px column and left the entries unreadable.
/// Scrolls when there are more curves than fit.
class FvChartLegend : public QWidget {
    Q_OBJECT
public:
    explicit FvChartLegend(QWidget* parent = nullptr);
    void setEntries(const QVector<FvLegendEntry>& entries);
    int  entryCount() const { return m_entries.size(); }
    /// The text of row `index` as the owner supplied it (before elision). Empty when out of
    /// range. Exists so what a legend actually SAYS is testable — a label built from the wrong
    /// pieces looks fine to a row count.
    QString entryText(int index) const {
        return (index >= 0 && index < m_entries.size()) ? m_entries[index].text : QString();
    }

public slots:
    /// Ask the owner to delete the curve row `index` draws. The dialog applies its own
    /// deletions directly; this exists so the owner's delete path has one public entry point
    /// (used by the tests, which cannot drive a modal dialog).
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

private:
    void rebuild();

    QVBoxLayout*          m_rows{nullptr};
    QVector<FvLegendEntry> m_entries;
};

/// One editable label in the ✎ dialog: a plot/axis title, or a legend entry.
struct FvLabelSpec {
    QString      caption;              ///< row heading (titles) — unused for legend rows
    QString      text;                 ///< the user's override; EMPTY means "automatic"
    QString      automatic;            ///< what an empty field restores; shown as placeholder
    bool         shown{true};          ///< unticked ⇒ the label is not drawn at all
    // Legend rows only:
    QColor       color;
    Qt::PenStyle style{Qt::SolidLine};
    bool         deleted{false};       ///< out: the row's 🗑 was pressed (applied on OK)
};

/// A hand-set axis range and tick step. `manual` false ⇒ auto-fit, which is the default.
/// `start` may exceed `end`: that is how a DESCENDING axis is asked for, and `step` must then
/// be negative — a step whose sign disagrees with the range is rejected, not silently fixed.
struct FvAxisTicks {
    bool   manual{false};
    double start{0.0}, end{0.0};
    double step{0.0};                  ///< 0 with manual=true ⇒ automatic tick spacing
};

/// Chart and legend side by side, with a divider the user can drag: the legend was capped at a
/// constant width, so a wider window only ever grew the plot and a long entry stayed elided
/// however much room there was. The cap becomes a fraction of the CONTAINER instead — the
/// legend may take up to half, never more, so neither a long file name nor a stray drag can
/// squeeze the plot to a sliver — and the divider's position is remembered per window.
class FvChartSplitter : public QSplitter {
    Q_OBJECT
public:
    /// `chartView` takes the stretch; `legend` keeps whatever the divider gives it.
    FvChartSplitter(QWidget* chartView, FvChartLegend* legend, QWidget* parent = nullptr);

protected:
    /// Re-derives the legend's ceiling from the new width, and pulls the divider back inside it
    /// when a shrink has left the legend over its share.
    void resizeEvent(QResizeEvent* e) override;

private:
    FvChartLegend* m_legend{nullptr};
};

/// Apply a hand-set range/step to an axis. A no-op when `t.manual` is false, so a caller can
/// hand it every axis unconditionally. The range is always set low→high with `setReverse`
/// carrying the direction, which keeps FvChartView's zoom and pan arithmetic (min < max)
/// working on a descending axis.
void fvApplyAxisTicks(QValueAxis* axis, const FvAxisTicks& t);

/// Everything the ✎ dialog edits, in and out.
struct FvPlotLabels {
    FvLabelSpec          title, xTitle, yTitle;
    QVector<FvLabelSpec> legend;       ///< one per curve, in plot order — hidden ones included
    FvAxisTicks          xTicks, yTicks;
    bool                 hasPlot{true};///< false ⇒ nothing plotted; title and legend disabled
};

/// The ✎ dialog: three collapsible sections (Titles, Legend, Axis ticks). Returns false on
/// Cancel, leaving `labels` untouched; on OK every field is written back, including each
/// legend row's `deleted` flag. Rejects an axis whose step disagrees with its range rather
/// than quietly reinterpreting it.
bool fvEditPlotLabels(QWidget* parent, FvPlotLabels& labels);

/// The button row above a chart: zoom in / zoom out / home, a hand icon captioning the
/// drag-to-pan gesture, the ✎ that opens the labels dialog, and Save. `chartView` must outlive
/// the toolbar.
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

signals:
    /// The ✎ was pressed. The owner opens fvEditPlotLabels() with its own state, since only it
    /// knows the automatic text behind every field.
    void editLabelsRequested();

public:

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
    SvgIconButton* m_edit{nullptr};
    QWidget*       m_export{nullptr};   // null ⇒ the chart view alone
    QLabel*        m_hand{nullptr};
    class QHBoxLayout* m_layout{nullptr};

    bool m_has_trailing{false};

    std::function<QString()> m_csv;
    QString                  m_suggested{QStringLiteral("plot")};
};
