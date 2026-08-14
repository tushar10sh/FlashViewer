#pragma once
#include <QWidget>
#include <QColor>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QRadioButton>
#include <QDoubleSpinBox>
#include "gis/InspectTypes.hpp"
#include "widgets/ChartTools.hpp"   // FvAxisTicks
#include <functional>
#include <memory>
#include <vector>

class LayerManager;
class RasterLayer;
class FvChartView;
class FvChartLegend;
class QComboBox;
class QEvent;
class QLabel;

// The Scan/Pixel Profile (FR-ANL-2/3): a top-level window on the same terms as the Spectral
// Plot — above the main window, geometry remembered, closed until `P` or Tools opens it.
//
// Phase 26.5 gives it the Spectral Plot's scope bookkeeping (FR-ANL-12): the panel holds one
// PLOT per COMPUTE SCOPE, not one curve for whatever happens to be active. A scope is the set
// of layers one Compute profiled:
//   * active layer in an unsynced pane → that layer alone
//   * active layer in a SYNCED pane    → every visible raster across the whole sync group,
//                                        merged into one plot
// Every layer of the scope maps to that plot, so activating ANY of them in the Layers panel
// brings the same curves back up; a layer with no plot renders a blank chart titled with its
// own name. "Persist curves" grows the plot on screen instead of replacing it, so profiles of
// several layers — or several statistics of one layer — can be compared side by side.
//
// Each curve carries its OWN layer's no-data (FR-ANL-11), so a merged plot masks per layer
// rather than by whatever happened to be active.
class ScanPixProfilePanel : public QWidget {
    Q_OBJECT
public:
    explicit ScanPixProfilePanel(LayerManager* mgr, QWidget* parent = nullptr);

    // NOTE: no caller sets an ROI today — the profile runs over the whole raster. Kept
    // because the requirement (FR-ANL-2) is written in terms of a user-drawn ROI and the
    // computation already honours one; wiring a rubber-band gesture is the missing half.
    void setRoi(double xmin, double ymin, double xmax, double ymax);

    // Pane colour for a pane id — supplied by MainWindow, which owns the PaneLayout. An
    // invalid/absent colour falls back to the palette highlight.
    void setPaneColorResolver(std::function<QColor(quint64)> r) { m_pane_color = std::move(r); }

    // What one Compute profiles, in the same InspectPaneGroup shape the Pixel Inspector and
    // the Spectral Plot are fed — MainWindow owns the PaneLayout and the Layers-panel
    // selection, so it decides whether the scope is the active layer, the whole sync group, or
    // the layers the user selected. Returning an empty scope (or leaving the resolver unset)
    // falls back to the active layer alone.
    void setScopeResolver(std::function<FvProfileScope()> r) { m_scope = std::move(r); }

    // The Layers panel activated `layerIndex` — show that layer's plot, or a blank chart.
    void showLayerPlot(int layerIndex);
    // A layer is going away: drop its curves and its plot membership.
    void forgetLayer(quint64 layerId);
    // Sync roles changed: `syncedPanes` is the current sync group (empty when nothing is
    // synced). A merged plot spanning panes no longer synced together is discarded — the merge
    // was a statement about panes that move together.
    void dropMergedPlotsOutside(const QSet<quint64>& syncedPanes);

    /// The chart/legend divider, saved with the window's geometry (FR-APP-6) — a divider that
    /// reset on every launch would be worse than one that could not be dragged.
    QByteArray saveSplitState() const;
    void       restoreSplitState(const QByteArray& state);

public slots:
    void compute();
    // Discard the plot on screen (and the scope it belongs to), leaving a blank chart.
    void clearCurrent();
    // Drop EVERY stored plot and every label override. Called when the panel is closed: the
    // profiles are a live working set, not a document (FR-ANL-11/12). The colour scheme is the
    // sole exception — it is a preference and lives in Settings (FR-APP-6).
    void forgetAll();

protected:
    // The curve colour is derived from a per-theme lightness band, so a theme switch has to
    // restyle the series, not just re-tint the chrome.
    void changeEvent(QEvent* e) override;
    // Closing the window discards every plot (FR-ANL-11/12), on the same terms as the Spectral
    // Plot: the profiles are a live working set, not a document. It lives HERE rather than in
    // MainWindow's filter so that it holds however the window is closed — the ✕, Alt+F4, or a
    // caller doing close() — and so a headless test can drive it.
    void closeEvent(QCloseEvent* e) override;

private:
    // One profiled layer. The statistic is recorded WITH the curve, not read from the radios
    // at draw time: with "Persist curves" on, one plot can hold a Mean and a Median of the
    // same layer, so the legend and the CSV have to say which is which long after the radios
    // have moved on, and a re-Compute must replace the matching curve rather than a random one.
    struct Curve {
        quint64 layerId{0};
        quint64 paneId{0};
        QColor  paneColor;        // resolved at compute time, so a later pane recolour cannot
                                  // silently re-attribute an old curve
        QString paneLabel;        // empty unless the plot spans >1 pane
        QString layerName;
        bool    scanMode{true};   // rows (true) or columns
        int     stat{0};          // 0 mean, 1 median, 2 stddev, 3 quantile
        double  p{0.9};           // quantile only
        std::vector<double> values;   // one per row/column; NaN = fully masked (drawn as a gap)
        // The curve is drawn either way; this only removes its LEGEND row (FR-ANL-10).
        bool    legendHidden{false};
    };
    // One plot: the scope that produced it plus the curves computed so far.
    struct Plot {
        QString        title;           // automatic, from the scope
        QString        titleOverride;   // user-edited; empty ⇒ use `title`
        QSet<quint64>  layers;          // scope — every layer that maps to this plot
        QSet<quint64>  panes;           // panes the scope spans (>1 ⇒ a synced merge)
        QVector<Curve> curves;
        // True only for a plot the SYNC GROUP produced. "Persist curves" can also grow a plot
        // across panes, and such a plot is a deliberate comparison the user assembled — it must
        // not be discarded the way a sync merge is when the panes stop moving together.
        bool           syncMerge{false};
        // "No title", which an EMPTY titleOverride cannot express -- that already means
        // "use the automatic text".
        bool           titleHidden{false};
    };
    using PlotPtr = std::shared_ptr<Plot>;

    void setupUi();
    void applyChartTheme();
    /// The toolbar's ✎ — one dialog for the titles, the legend and the axis ticks (FR-ANL-10).
    void editLabels();

    /// Compute one layer's profile with the masking rule in force AT THE TIME OF THE CALL.
    /// False when the layer has nothing readable (no dataset, empty ROI).
    bool profileFor(RasterLayer* rl, const Curve& spec, std::vector<double>& out) const;

    /// Stable identity of a curve for the label overrides and the legend keys: the layer plus
    /// the statistic, so renaming a layer's Mean does not also rename its Median.
    static QString curveKey(const Curve& c);
    /// The automatic legend text: pane prefix (merged plots only), layer name, and the
    /// statistic on its own line so the legend can wrap it (FR-ANL-9).
    QString curveLabel(const Curve& c) const;
    static QString statName(const Curve& c);
    /// The plot on screen as CSV — one row per index, one column per curve.
    QString profileAsCsv() const;

    // Unmap `layerId` from whatever plot owns it and drop its curves; erase the plot once
    // nothing maps to it any more.
    void detachLayer(quint64 layerId);
    void erasePlot(PlotPtr p);          // by value — clearCurrent() passes m_current
    /// Drop the curve the legend's `legendRow`-th entry draws (FR-ANL-13) — the legend's own
    /// numbering, which skips curves that draw nothing and rows the user has hidden.
    void deleteLegendRow(int legendRow);
    /// Drop `m_current->curves[idx]`. A layer left with no curve leaves the scope, and a plot
    /// left with no curves is discarded outright.
    void deleteCurve(int idx);
    /// Rebuild `title` from what the plot NOW holds — Persist grows it after the fact.
    void retitle(const PlotPtr& p);
    void render();

    LayerManager*   m_mgr{nullptr};
    double m_roi_xmin{0}, m_roi_ymin{0}, m_roi_xmax{0}, m_roi_ymax{0};
    bool   m_has_roi{false};

    std::function<QColor(quint64)>                 m_pane_color;
    std::function<FvProfileScope()>                m_scope;

    QRadioButton*   m_scan_radio{nullptr};
    QRadioButton*   m_pixel_radio{nullptr};
    QRadioButton*   m_mean_radio{nullptr};
    QRadioButton*   m_median_radio{nullptr};
    QRadioButton*   m_stddev_radio{nullptr};
    QRadioButton*   m_quantile_radio{nullptr};
    QDoubleSpinBox* m_p_spin{nullptr};
    // FR-ANL-11. On by default: a statistic taken over a scene's -9999 padding describes the
    // padding, not the data, so the useful answer is the masked one.
    class QCheckBox* m_mask_nodata{nullptr};
    class QCheckBox* m_persist{nullptr};
    QComboBox*      m_scheme{nullptr};
    QLabel*         m_status{nullptr};

    QChart*        m_chart{nullptr};
    FvChartView*   m_chart_view{nullptr};
    FvChartLegend* m_legend{nullptr};
    class FvChartSplitter* m_split{nullptr};

    // Label overrides (FR-ANL-10). Keyed by CURVE identity, not by position, so an edit
    // survives the next Compute replacing the curves; an empty entry never exists — clearing
    // the text removes the key, which is what restores the automatic label.
    QHash<QString, QString> m_name_override;
    QString                 m_x_override, m_y_override;
    bool                    m_x_hidden{false}, m_y_hidden{false};
    // Hand-set axis ranges/steps (FR-ANL-14).
    FvAxisTicks             m_x_ticks, m_y_ticks;
    bool                    m_grid{true};

    // Legend row → index into m_current->curves, rebuilt by every render(). A fully-masked
    // curve draws nothing and gets no legend row, so the two are not the same sequence.
    QVector<int>             m_legend_curve;

    std::vector<PlotPtr>     m_plots;
    QHash<quint64, PlotPtr>  m_by_layer;
    PlotPtr                  m_current;
};
