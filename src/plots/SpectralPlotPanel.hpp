#pragma once
#include <QWidget>
#include <QColor>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include "gis/InspectTypes.hpp"
#include "widgets/ChartTools.hpp"   // FvAxisTicks
#include <memory>
#include <string>
#include <vector>

class LayerManager;
class QChart;
class QCheckBox;
class QLabel;
class QEvent;
class FvChartView;
class FvChartLegend;
class QComboBox;

// The Spectral Plot (FR-ANL-1): a top-level window parented to MainWindow, so it stays above
// it without covering other applications, remembers its own geometry, and is closed until
// `S` or Tools opens it. (It was a QDockWidget payload from Phase 26 until 26.8; SDD §5.5
// records why that was undone.)
//
// The panel holds one PLOT per inspect SCOPE, not one per click. A scope is the exact set
// of layers a single inspect gesture sampled — mirroring the Pixel Inspector's own left /
// right-click rules (FR-GIS-6):
//   * left-click, unsynced pane   → that pane's representative layer  → a single-layer plot
//   * right-click, unsynced pane  → all its visible rasters           → one merged plot
//   * left-click, synced panes    → each pane's representative layer  → one merged plot
//   * right-click, synced panes   → every visible raster in the group → one merged plot
// Every layer in the scope maps to that plot, so activating ANY of them in the Layers panel
// brings the same merged curves back up. A layer belongs to exactly one plot — the most
// recent scope that sampled it — so a narrower click supersedes an earlier merge for the
// layers it re-samples, and leaves the rest of the merge intact.
//
// Activating a layer with no stored plot renders a blank chart titled with that layer's
// name, so the panel always says what it is showing (or would show).
class SpectralPlotPanel : public QWidget {
    Q_OBJECT
public:
    explicit SpectralPlotPanel(QWidget* parent = nullptr);
    void setLayerManager(LayerManager* mgr);

    // One inspect gesture. `groups` is the SAME selection handed to the Pixel Inspector, so
    // the two panels can never disagree; `allLayers` is the left/right-click distinction and
    // only shapes the title. (gx, gy) are in `geoWkt` — the clicked pane's Project CRS.
    // With "Persist curves" off (the default) the scope's plot is replaced by this pixel;
    // with it on the new curves are appended so several pixels can be compared.
    void addInspectResult(double gx, double gy, const std::string& geoWkt,
                          const QVector<InspectPaneGroup>& groups, bool allLayers);

    // The Layers panel activated `layerIndex` — show that layer's plot, or a blank chart.
    void showLayerPlot(int layerIndex);
    // A layer is going away: drop its curves and its plot membership.
    void forgetLayer(quint64 layerId);
    // Sync roles changed: `syncedPanes` is the current sync group (empty when nothing is
    // synced). A merged plot that spans panes no longer synced together is discarded — its
    // layers fall back to blank until inspected again.
    void dropMergedPlotsOutside(const QSet<quint64>& syncedPanes);

    /// The chart/legend divider, saved with the window's geometry (FR-APP-6) — a divider that
    /// reset on every launch would be worse than one that could not be dragged.
    QByteArray saveSplitState() const;
    void       restoreSplitState(const QByteArray& state);

public slots:
    // Discard the plot on screen (and the scope it belongs to), leaving a blank chart.
    void clearCurrent();
    // Drop EVERY stored plot and every label override. Called when the panel is closed: the
    // spectra are a live working set, not a document, and a user who closes the panel and
    // reopens it expects an empty one rather than curves from an hour ago. The colour scheme
    // is the sole exception — it is a preference and lives in Settings (FR-APP-6).
    void forgetAll();

protected:
    // Re-tint the chart on a theme switch — QChart is painted, not styled by the QSS.
    void changeEvent(QEvent* e) override;
    // Closing the window discards every plot (FR-ANL-11). Here rather than in MainWindow's
    // filter so the rule holds however the window is closed, and so a test can drive it.
    void closeEvent(QCloseEvent* e) override;

private:
    struct Curve {
        quint64             layerId{0};
        quint64             paneId{0};   // which pane's colour this curve wears
        QColor              paneColor;   // resolved at sample time, so a later recolour of
                                         // the pane cannot silently re-attribute an old curve
        // The label is kept in PIECES, not pre-baked: a rename overrides only the layer-name
        // part, so the coordinates still tell two persisted clicks on one layer apart.
        // paneLabel is ALWAYS recorded, and shown only while the plot spans >1 pane: with
        // Persist on a plot can gain a second pane long after this curve was sampled.
        QString             paneLabel;
        QString             layerName;
        QString             coord;       // "(x, y)"
        std::vector<double> values;      // one per band; NaN = no-data (drawn as a gap)
        // The curve is drawn either way; this only removes its LEGEND row (FR-ANL-10). Hidden
        // rows are still listed by the labels dialog, or they could never be brought back.
        bool                legendHidden{false};
    };
    // One plot: the scope that produced it plus the curves sampled so far.
    struct Plot {
        QString        title;            // automatic, from the scope
        QString        titleOverride;    // user-edited; empty ⇒ use `title`
        QSet<quint64>  layers;   // scope — every layer that maps to this plot
        QSet<quint64>  panes;    // panes the scope spans
        QVector<Curve> curves;
        // True only for a plot ONE gesture produced across a sync group. "Persist curves" can
        // also grow a plot across panes, and that is a comparison the user assembled — it must
        // not be discarded the way a sync merge is when the panes stop moving together.
        bool           syncMerge{false};
        bool           allLayers{false};   // the click rule the title names
        // "No title", which an EMPTY titleOverride cannot express -- that already means
        // "use the automatic text".
        bool           titleHidden{false};
    };
    using PlotPtr = std::shared_ptr<Plot>;

    void setupUi();
    /// Display text for a curve: the user's override of the layer name if there is one, plus
    /// the pane prefix and the coordinates, which a rename never removes.
    QString curveLabel(const Curve& c) const;
    /// The automatic (un-overridden) title and axis titles, offered by the dialog as what
    /// clearing a field restores.
    void defaultLabels(QString& title, QString& x, QString& y) const;
    /// The toolbar's ✎ — one dialog for the titles, the legend and the axis ticks (FR-ANL-10).
    void editLabels();
    /// The plot on screen as CSV — one row per band, one column per curve — for the shared
    /// toolbar's Save. Empty when there is nothing plotted.
    QString curvesAsCsv() const;
    // Unmap `layerId` from whatever plot owns it and drop its curves; erase the plot once
    // nothing maps to it any more.
    void detachLayer(quint64 layerId);
    void erasePlot(PlotPtr p);
    /// Drop the curve the legend's `legendRow`-th entry draws (FR-ANL-13) — the legend's own
    /// numbering, which skips curves that draw nothing and rows the user has hidden.
    void deleteLegendRow(int legendRow);
    /// Drop `m_current->curves[idx]`. A layer left with no curve leaves the scope, and a plot
    /// left with no curves is discarded outright.
    void deleteCurve(int idx);
    /// Rebuild `title` from what the plot NOW holds — Persist can add panes and layers long
    /// after the gesture that created it.
    void retitle(const PlotPtr& p);
    void render();
    void applyChartTheme();

    LayerManager* m_mgr{nullptr};
    QChart*       m_chart{nullptr};
    FvChartView*  m_chart_view{nullptr};
    FvChartLegend* m_legend{nullptr};
    class FvChartSplitter* m_split{nullptr};
    QCheckBox*    m_persist{nullptr};
    QComboBox*    m_scheme{nullptr};
    QLabel*       m_status{nullptr};

    // User label overrides. Keyed by LAYER, not by curve, so a rename survives the next
    // inspect click replacing the curves (FR-ANL-10); an empty entry never exists — clearing
    // the text removes the key, which is what restores the automatic label.
    QHash<quint64, QString> m_name_override;
    QString                 m_x_override, m_y_override;
    bool                    m_x_hidden{false}, m_y_hidden{false};
    // Hand-set axis ranges/steps (FR-ANL-14). Manual ranges become the ⌂ home, which falls out
    // of captureHome() running after the axes are built.
    FvAxisTicks             m_x_ticks, m_y_ticks;
    bool                    m_grid{true};
    // Persisted (FR-APP-6): whether an entry carries its sampled coordinates. Off collapses
    // every entry to one line, which is what a ten-curve legend usually wants.
    bool                    m_show_coords{true};

    // Legend row → index into m_current->curves, rebuilt by every render(). A curve whose
    // every band is no-data draws nothing and gets no legend row, so the two are not the same
    // sequence and delete cannot use the row number directly.
    QVector<int>             m_legend_curve;

    std::vector<PlotPtr>     m_plots;
    QHash<quint64, PlotPtr>  m_by_layer;
    PlotPtr                  m_current;
};
