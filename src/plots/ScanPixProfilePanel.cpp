#include "plots/ScanPixProfilePanel.hpp"
#include "core/LayerManager.hpp"
#include "core/RasterLayer.hpp"
#include "core/Layer.hpp"
#include "io/RasterDataset.hpp"
#include "core/GeoTransform.hpp"
#include "util/MathUtils.hpp"
#include "widgets/UiKit.hpp"          // fvMakeSection / FvTickCheckBox
#include "widgets/ChartTools.hpp"     // FvChartView / FvChartToolbar — shared with the Spectral Plot
#include "widgets/CurveStyle.hpp"     // fvCurveColor / fvCurveDash — a curve names its pane
#include "app/Settings.hpp"           // the persisted colour-scheme choice (FR-APP-6)
#include "util/Logger.hpp"

#include <QtCharts/QChart>
#include <QGraphicsLayout>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QApplication>
#include <QComboBox>
#include <QCloseEvent>
#include <QEvent>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

ScanPixProfilePanel::ScanPixProfilePanel(LayerManager* mgr, QWidget* parent)
    : QWidget(parent)
    , m_mgr(mgr)
{
    setupUi();
    render();
}

void ScanPixProfilePanel::setRoi(double xmin, double ymin, double xmax, double ymax) {
    m_roi_xmin = xmin;
    m_roi_ymin = ymin;
    m_roi_xmax = xmax;
    m_roi_ymax = ymax;
    m_has_roi  = true;
}

void ScanPixProfilePanel::setupUi() {
    auto* central = this;                 // the panel IS the dock's widget now
    auto* mainLay = new QVBoxLayout(central);
    mainLay->setContentsMargins(6, 6, 6, 6);
    mainLay->setSpacing(6);

    // ---- Controls row ----
    auto* ctrlLay = new QHBoxLayout();

    // Mode group. Section frames, not QGroupBoxes — a group box hangs its title in the
    // widget's top margin, so the heading collided with the frame border.
    QVBoxLayout* modeInner = nullptr;
    auto* modeBox = fvMakeSection(tr("Mode"), modeInner, central);
    auto* modeLay = new QHBoxLayout();
    m_scan_radio  = new QRadioButton(tr("Scan (rows)"),  modeBox);
    m_pixel_radio = new QRadioButton(tr("Pixel (cols)"), modeBox);
    m_scan_radio->setChecked(true);
    modeLay->addWidget(m_scan_radio);
    modeLay->addWidget(m_pixel_radio);
    modeInner->addLayout(modeLay);
    ctrlLay->addWidget(modeBox);

    // Statistic group
    QVBoxLayout* statInner = nullptr;
    auto* statBox = fvMakeSection(tr("Statistic"), statInner, central);
    auto* statLay = new QHBoxLayout();
    m_mean_radio     = new QRadioButton(tr("Mean"),    statBox);
    m_median_radio   = new QRadioButton(tr("Median"),  statBox);
    m_stddev_radio   = new QRadioButton(tr("StdDev"),  statBox);
    m_quantile_radio = new QRadioButton(tr("Quantile"), statBox);
    m_mean_radio->setChecked(true);

    m_p_spin = new QDoubleSpinBox(statBox);
    m_p_spin->setRange(0.0, 1.0);
    m_p_spin->setSingleStep(0.05);
    m_p_spin->setValue(0.9);
    m_p_spin->setDecimals(2);
    m_p_spin->setEnabled(false);

    statLay->addWidget(m_mean_radio);
    statLay->addWidget(m_median_radio);
    statLay->addWidget(m_stddev_radio);
    statLay->addWidget(m_quantile_radio);
    statLay->addWidget(new QLabel(tr("p:"), statBox));
    statLay->addWidget(m_p_spin);
    statInner->addLayout(statLay);
    ctrlLay->addWidget(statBox);

    // No-data masking (FR-ANL-11), the same FvTickCheckBox indicator as everywhere else
    // (FR-APP-15). Left of Compute, because it changes what Compute will produce.
    m_mask_nodata = new FvTickCheckBox(tr("Mask No-Data"), central);
    m_mask_nodata->setChecked(true);
    m_mask_nodata->setToolTip(tr("Exclude each layer's own no-data value (and any non-finite "
                                 "samples) from the statistics. A row or column with nothing "
                                 "left is drawn as a gap."));
    // Deliberately NO recompute here: the toggle states the rule the NEXT Compute will use.
    // Re-running on the toggle re-read every layer of the plot on screen behind a single
    // checkbox click — expensive, and it moved the numbers without the user asking for new
    // ones. Compute is the only thing that reads a raster.
    connect(m_mask_nodata, &QCheckBox::toggled, this, [this] {
        if (m_current && !m_current->curves.isEmpty())
            m_status->setText(tr("Masking changed — press Compute to apply it."));
    });
    // The same gap the chart toolbar puts between its trailing controls, on BOTH sides of the
    // toggle, so the two rows of this panel are spaced alike (FR-APP-15).
    ctrlLay->addSpacing(kFvChartTrailingGap);
    ctrlLay->addWidget(m_mask_nodata);
    ctrlLay->addSpacing(kFvChartTrailingGap);

    // Compute button
    auto* computeBtn = new QPushButton(tr("Compute"), central);
    connect(computeBtn, &QPushButton::clicked, this, &ScanPixProfilePanel::compute);
    ctrlLay->addWidget(computeBtn);
    ctrlLay->addStretch();

    mainLay->addLayout(ctrlLay);

    // Enable p_spin only when quantile selected
    connect(m_quantile_radio, &QRadioButton::toggled, m_p_spin, &QDoubleSpinBox::setEnabled);

    // ---- Chart ----
    m_chart = new QChart();
    m_chart->setTitle(tr("Profile"));
    m_chart->setAnimationOptions(QChart::NoAnimation);
    // Qt's 20 px on every side, its own layout margins and a rounded background inset spent
    // ~80 px of the panel on nothing. These are chart PROPERTIES, so they survive the axis
    // rebuild every render() performs; only the axes need re-doing there.
    m_chart->setMargins(kFvChartMargins);
    m_chart->layout()->setContentsMargins(0, 0, 0, 0);
    m_chart->setBackgroundRoundness(0);
    // QChart's own legend is retired in favour of the shared vertical one (FR-ANL-9).
    m_chart->legend()->setVisible(false);

    m_chart_view = new FvChartView(m_chart, central);

    // The same zoom / pan / home / save row the Spectral Plot carries (FR-APP-15) — a
    // profile over a 1024-sample ROI is exactly where zooming into a feature matters.
    auto* toolbar = new FvChartToolbar(m_chart_view, central);
    toolbar->setSuggestedName(QStringLiteral("scan_pixel_profile"));
    toolbar->setCsvProvider([this] { return profileAsCsv(); });
    connect(toolbar, &FvChartToolbar::editLabelsRequested, this, &ScanPixProfilePanel::editLabels);

    // The colour scheme is shared with the Spectral Plot through Settings, but it is offered
    // here too: since Phase 26.5 a profile plot can hold many curves, so which scheme is in
    // force matters — and reaching it only through the other panel would be a hidden control.
    m_scheme = new QComboBox(central);
    m_scheme->addItem(tr("Colour by pane"));
    m_scheme->addItem(tr("Original palette"));
    m_scheme->setCurrentIndex(Settings::instance().curveColorScheme() == 1 ? 1 : 0);
    m_scheme->setToolTip(tr("Colour by pane: hue names the pane, lightness and line style name "
                            "the layer within it. Original palette: ten fixed hues cycled by "
                            "curve position, carrying no pane meaning."));
    connect(m_scheme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        Settings::instance().setCurveColorScheme(i);
        render();
    });
    toolbar->addTrailingWidget(m_scheme);

    // The same persisted grid toggle the Spectral Plot carries (FR-APP-15). There is no
    // Coords toggle here: a profile's second legend line is its mode and statistic, which is
    // what tells a Mean from a Median.
    m_grid = Settings::instance().plotGrid();
    auto* gridBox = new FvTickCheckBox(tr("Grid"), central);
    gridBox->setChecked(m_grid);
    gridBox->setToolTip(tr("Draw the horizontal and vertical grid lines"));
    connect(gridBox, &QCheckBox::toggled, this, [this](bool on) {
        m_grid = on;
        Settings::instance().setPlotGrid(on);
        render();
    });
    toolbar->addTrailingWidget(gridBox);

    // Off by default: Compute replaces the scope's curves. Ticking it grows the plot on
    // screen instead, so several layers — or several statistics of one layer — can be
    // compared (FR-ANL-12).
    m_persist = new FvTickCheckBox(tr("Persist curves"), central);
    m_persist->setToolTip(tr("Add the next Compute to the plot on screen instead of replacing "
                             "it, so profiles of several layers can be compared"));
    toolbar->addTrailingWidget(m_persist);

    auto* clearBtn = new QPushButton(tr("Clear"), central);
    clearBtn->setToolTip(tr("Discard the plot on screen"));
    connect(clearBtn, &QPushButton::clicked, this, &ScanPixProfilePanel::clearCurrent);
    toolbar->addTrailingWidget(clearBtn);

    auto* barLay = new QHBoxLayout();
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->addWidget(toolbar);
    barLay->addStretch(1);
    mainLay->addLayout(barLay);

    m_status = new QLabel(central);
    m_status->setWordWrap(true);
    mainLay->addWidget(m_status);

    m_chart_view->setMinimumHeight(160);

    // Chart + legend in one container with a divider the user can drag, which is also what
    // Save renders — see FvChartToolbar::setExportWidget; exporting the view alone would drop
    // the legend.
    m_legend = new FvChartLegend(central);
    connect(m_legend, &FvChartLegend::entryRenamed, this,
            [this](const QString& key, const QString& text) {
                // Removing the key IS the reset: nothing stores an empty override, so the
                // automatic label can never be shadowed by a blank one.
                if (text.isEmpty()) m_name_override.remove(key);
                else                m_name_override.insert(key, text);
                render();
            });
    connect(m_legend, &FvChartLegend::entryDeleted,
            this, &ScanPixProfilePanel::deleteLegendRow);
    m_split = new FvChartSplitter(m_chart_view, m_legend, central);
    mainLay->addWidget(m_split, 1);
    toolbar->setExportWidget(m_split);

    applyChartTheme();
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

QString ScanPixProfilePanel::statName(const Curve& c) {
    switch (c.stat) {
        case 0:  return tr("Mean");
        case 1:  return tr("Median");
        case 2:  return tr("StdDev");
        default: return tr("Q(p=%1)").arg(c.p, 0, 'f', 2);
    }
}

QString ScanPixProfilePanel::curveKey(const Curve& c) {
    return QStringLiteral("%1|%2|%3|%4")
        .arg(c.layerId)
        .arg(c.scanMode ? 1 : 0)
        .arg(c.stat)
        .arg(c.p, 0, 'f', 2);
}

QString ScanPixProfilePanel::curveLabel(const Curve& c) const {
    const QString name = m_name_override.value(curveKey(c), c.layerName);
    const bool multiPane = m_current && m_current->panes.size() > 1;
    QString out;
    if (multiPane && !c.paneLabel.isEmpty()) out += c.paneLabel + QStringLiteral(" / ");
    out += name;
    // Newline, not a space: the legend wraps the statistic onto its own line (FR-ANL-9), so a
    // long filename and the mode/statistic never share one row. A rename replaces only the
    // layer-name part, so the statistic still tells a Mean and a Median of one layer apart.
    out += QStringLiteral("\n") + (c.scanMode ? tr("Scan") : tr("Pixel"))
         + QStringLiteral(" — ") + statName(c);
    return out;
}

// Name the plot after the scope that produced it, so the title always says WHOSE profiles are
// drawn: one layer by name, one pane's merge by pane, a sync-group merge by the panes it spans.
static QString buildProfileTitle(const QStringList& paneLabels, int curveCount,
                                 const QString& soleLayerName, const QString& solePaneLabel) {
    if (curveCount == 1)
        return solePaneLabel.isEmpty()
                   ? soleLayerName
                   : ScanPixProfilePanel::tr("%1 — %2").arg(soleLayerName, solePaneLabel);
    if (paneLabels.size() <= 1) {
        const QString pane = paneLabels.isEmpty() ? QString() : paneLabels.front();
        return pane.isEmpty()
                   ? ScanPixProfilePanel::tr("%n profile(s)", "", curveCount)
                   : ScanPixProfilePanel::tr("%1 — %n profile(s)", "", curveCount).arg(pane);
    }
    return ScanPixProfilePanel::tr("Synced %1 — %n profile(s)", "", curveCount)
        .arg(paneLabels.join(QStringLiteral(", ")));
}

void ScanPixProfilePanel::editLabels() {
    const bool scanMode = m_current && !m_current->curves.isEmpty()
                              ? m_current->curves.front().scanMode
                              : m_scan_radio->isChecked();
    const QString dt = !m_current || m_current->title.isEmpty()
                           ? tr("Profile")
                           : tr("Profile: %1").arg(m_current->title);

    FvPlotLabels spec;
    spec.hasPlot = static_cast<bool>(m_current);
    spec.title  = FvLabelSpec{tr("Plot:"), m_current ? m_current->titleOverride : QString(),
                              dt, m_current ? !m_current->titleHidden : true,
                              {}, Qt::SolidLine, false};
    spec.xTitle = FvLabelSpec{tr("X axis:"), m_x_override, scanMode ? tr("Row") : tr("Column"),
                              !m_x_hidden, {}, Qt::SolidLine, false};
    spec.yTitle = FvLabelSpec{tr("Y axis:"), m_y_override, tr("Value"),
                              !m_y_hidden, {}, Qt::SolidLine, false};
    spec.xTicks = m_x_ticks;
    spec.yTicks = m_y_ticks;

    const bool isDark = QApplication::palette().window().color().lightness() < 128;
    const QColor fallbackPane = QApplication::palette().highlight().color();
    QHash<quint64, int> seenInPane;
    if (m_current) {
        for (const Curve& c : m_current->curves) {
            const int idx = seenInPane[c.paneId]++;
            FvLabelSpec row;
            row.text      = m_name_override.value(curveKey(c));
            row.automatic = c.layerName;
            row.shown     = !c.legendHidden;
            row.color     = fvCurveColor(c.paneColor.isValid() ? c.paneColor : fallbackPane,
                                         idx, isDark);
            row.style     = fvCurveDash(idx);
            spec.legend.push_back(row);
        }
    }

    if (!fvEditPlotLabels(this, spec)) return;

    if (m_current) {
        m_current->titleOverride = spec.title.text;
        m_current->titleHidden   = !spec.title.shown;
    }
    m_x_override = spec.xTitle.text;
    m_y_override = spec.yTitle.text;
    m_x_hidden   = !spec.xTitle.shown;
    m_y_hidden   = !spec.yTitle.shown;
    m_x_ticks    = spec.xTicks;
    m_y_ticks    = spec.yTicks;

    if (m_current) {
        // Renames and Show flags first, deletions afterwards and back-to-front: deleting as we
        // go would shift the indices the remaining rows are addressed by.
        for (int i = 0; i < spec.legend.size() && i < m_current->curves.size(); ++i) {
            Curve& c = m_current->curves[i];
            c.legendHidden = !spec.legend[i].shown;
            const QString t = spec.legend[i].text;
            if (t.isEmpty()) m_name_override.remove(curveKey(c));
            else             m_name_override.insert(curveKey(c), t);
        }
        for (int i = spec.legend.size() - 1; i >= 0; --i)
            if (spec.legend[i].deleted) deleteCurve(i);
    }
    render();
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

QString ScanPixProfilePanel::profileAsCsv() const {
    if (!m_current || m_current->curves.isEmpty()) return {};

    // Merged curves can differ in length (layers of different size profiled together), so the
    // table is as long as the longest and short curves end early.
    size_t rows = 0;
    for (const Curve& c : m_current->curves) rows = std::max(rows, c.values.size());
    if (rows == 0) return {};

    auto quote = [](QString s) {
        s.replace('"', QStringLiteral("\"\""));
        return '"' + s + '"';
    };

    const bool scanMode = m_current->curves.front().scanMode;
    // Exports carry what is ON SCREEN, label edits included. The legend's newline is flattened
    // to a space; a CSV field cannot hold one without quoting games at the far end.
    QString out;
    out += "# " + (m_current->titleOverride.isEmpty() ? m_current->title
                                                      : m_current->titleOverride) + "\n";
    out += (m_x_override.isEmpty() ? (scanMode ? tr("Row") : tr("Column")) : m_x_override);
    for (const Curve& c : m_current->curves)
        out += "," + quote(QString(curveLabel(c)).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    out += "\n";

    for (size_t i = 0; i < rows; ++i) {
        out += QString::number(i);
        for (const Curve& c : m_current->curves) {
            out += ",";
            // Past the end, or a line masked out entirely (FR-ANL-11): an EMPTY field. Writing
            // "nan" would be read back as a value by most spreadsheet importers.
            if (i < c.values.size() && !std::isnan(c.values[i]))
                out += QString::number(c.values[i], 'g', 10);
        }
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Computation
// ---------------------------------------------------------------------------

bool ScanPixProfilePanel::profileFor(RasterLayer* rl, const Curve& spec,
                                     std::vector<double>& out) const {
    out.clear();
    if (!rl) return false;
    auto* ds = rl->dataset();
    if (!ds) return false;

    // Determine pixel region to read
    int xoff = 0, yoff = 0, xsize = ds->width(), ysize = ds->height();

    if (m_has_roi) {
        // Convert geo ROI to pixel coords
        auto gt = ds->geoTransform();
        auto p1 = gt.geoToPixel(m_roi_xmin, m_roi_ymax); // top-left (ymax = northern edge)
        auto p2 = gt.geoToPixel(m_roi_xmax, m_roi_ymin); // bottom-right

        int px1 = static_cast<int>(std::floor(std::min(p1.x, p2.x)));
        int py1 = static_cast<int>(std::floor(std::min(p1.y, p2.y)));
        int px2 = static_cast<int>(std::ceil(std::max(p1.x, p2.x)));
        int py2 = static_cast<int>(std::ceil(std::max(p1.y, p2.y)));

        xoff  = std::max(0, px1);
        yoff  = std::max(0, py1);
        xsize = std::min(ds->width(),  px2) - xoff;
        ysize = std::min(ds->height(), py2) - yoff;

        if (xsize <= 0 || ysize <= 0) return false;
    }

    // Cap at 1024 pixels in each dimension
    const int kMaxSize = 1024;
    int dstW = std::min(xsize, kMaxSize);
    int dstH = std::min(ysize, kMaxSize);

    // Read first band only for profile
    TileBuffer buf = ds->readRegion(xoff, yoff, xsize, ysize, dstW, dstH, {1});
    if (!buf.isValid()) return false;

    const float* ptr = buf.bandPtr(0);
    const int W = buf.width;
    const int H = buf.height;
    if (!ptr || W <= 0 || H <= 0) return false;

    // No-data masking (FR-ANL-11). Without it a scene padded with -9999 (or 0, or 65535) drags
    // every aggregate toward the sentinel, and the statistic describes the padding rather than
    // the data. THIS layer's own no-data — a merged plot (FR-ANL-12) masks per layer, since two
    // panes' rasters routinely declare different sentinels. Same relative tolerance as
    // fvSamplePixelBands, so the profile and the Pixel Inspector agree on what counts as
    // no-data. Non-finite samples are dropped whenever masking is on, declared value or not —
    // a NaN poisons mean and stddev outright.
    const bool  mask   = m_mask_nodata && m_mask_nodata->isChecked();
    const bool  has_nd = mask && rl->hasNoData();
    const float nd_v   = has_nd ? rl->noDataValue() : 0.0f;
    const float nd_eps = has_nd ? std::max(std::abs(nd_v) * 1e-5f, 1e-10f) : 0.0f;
    auto keep = [&](float v) {
        if (!mask) return true;
        if (!std::isfinite(v)) return false;
        return !(has_nd && std::abs(v - nd_v) < nd_eps);
    };

    const float p = static_cast<float>(spec.p);
    // A line with nothing left after masking has no statistic. It yields NaN rather than 0 —
    // which would be a value, and would be plotted — and the chart draws a gap there.
    // `line`, not `data` — QWidget has a `data` member, and the shadow is a warning.
    auto aggregate = [&](const std::vector<float>& line) -> double {
        if (line.empty()) return std::numeric_limits<double>::quiet_NaN();
        switch (spec.stat) {
            case 0:  return static_cast<double>(MathUtils::mean(line));
            case 1:  return static_cast<double>(MathUtils::median(line));
            case 2:  return static_cast<double>(MathUtils::stddev(line));
            default: return static_cast<double>(MathUtils::quantile(line, p));
        }
    };

    out.reserve(static_cast<size_t>(spec.scanMode ? H : W));
    int masked_lines = 0;
    if (spec.scanMode) {
        // For each row, compute statistic across all columns
        for (int row = 0; row < H; ++row) {
            std::vector<float> rowData;
            rowData.reserve(static_cast<size_t>(W));
            for (int col = 0; col < W; ++col) {
                const float v = ptr[static_cast<size_t>(row * W + col)];
                if (keep(v)) rowData.push_back(v);
            }
            if (rowData.empty()) ++masked_lines;
            out.push_back(aggregate(rowData));
        }
    } else {
        // For each column, compute statistic across all rows
        for (int col = 0; col < W; ++col) {
            std::vector<float> colData;
            colData.reserve(static_cast<size_t>(H));
            for (int row = 0; row < H; ++row) {
                const float v = ptr[static_cast<size_t>(row * W + col)];
                if (keep(v)) colData.push_back(v);
            }
            if (colData.empty()) ++masked_lines;
            out.push_back(aggregate(colData));
        }
    }
    if (masked_lines > 0)
        FV_INFO("Scan/Pixel Profile: {} of {} lines of '{}' are entirely no-data", masked_lines,
                out.size(), rl->name().toStdString());
    return !out.empty();
}

void ScanPixProfilePanel::compute() {
    if (!m_mgr) return;

    // The scope: MainWindow decides, because only it knows whether the active layer's pane is
    // synced. Without a resolver (or when it declines) the active layer alone is profiled,
    // which is what this panel always did.
    QVector<InspectPaneGroup> scope = m_scope ? m_scope() : QVector<InspectPaneGroup>{};
    if (scope.isEmpty()) {
        auto l = m_mgr->activeLayer();
        if (!l || l->type() != LayerType::Raster) {
            m_status->setText(tr("No raster layer is active."));
            return;
        }
        InspectPaneGroup g;
        g.paneId    = l->paneId();
        g.paneColor = m_pane_color ? m_pane_color(g.paneId) : QColor();
        g.layers.push_back(InspectLayerEntry{ l->name(), static_cast<RasterLayer*>(l.get()) });
        scope.push_back(std::move(g));
    }

    // The statistic every curve of THIS Compute is computed with; it is stored on each curve,
    // so a later re-run reproduces it rather than whatever the radios say by then.
    Curve spec;
    spec.scanMode = m_scan_radio->isChecked();
    spec.stat     = m_mean_radio->isChecked()   ? 0
                  : m_median_radio->isChecked() ? 1
                  : m_stddev_radio->isChecked() ? 2
                                                : 3;
    spec.p        = m_p_spin->value();

    // Profile everything FIRST. A pane whose layers are all unreadable contributes no curve,
    // so it must not appear in the title or widen the scope either.
    struct Hit {
        quint64             paneId{0};
        QString             paneLabel;
        QColor              paneColor;
        QString             layerName;
        quint64             layerId{0};
        std::vector<double> values;
    };
    std::vector<Hit> hits;
    QSet<quint64>    panes;

    for (const auto& g : scope) {
        bool any = false;
        for (const auto& e : g.layers) {
            if (!e.layer) continue;
            std::vector<double> vals;
            if (!profileFor(e.layer, spec, vals)) continue;
            hits.push_back(Hit{g.paneId, g.paneLabel, g.paneColor, e.name,
                               e.layer->layerId(), std::move(vals)});
            any = true;
        }
        if (any) panes.insert(g.paneId);
    }

    if (hits.empty()) {
        // Nothing to profile — say so without destroying what the user is already looking at.
        m_status->setText(tr("Nothing to profile in the current selection."));
        return;
    }

    QSet<quint64> members;
    for (const auto& h : hits) members.insert(h.layerId);

    const bool persist = m_persist && m_persist->isChecked();

    PlotPtr plot;
    if (persist && m_current) {
        // Grow the plot on screen: the newly profiled layers join its scope, so activating any
        // of them brings the comparison back up.
        plot = m_current;
        for (quint64 id : members)
            if (m_by_layer.value(id) != plot) detachLayer(id);
        plot->layers.unite(members);
        plot->panes.unite(panes);
        for (quint64 id : members) m_by_layer.insert(id, plot);
    } else {
        // Re-use the plot when the scope is identical; otherwise the profiled layers leave
        // their old plots and form a new one — the most recent Compute owns them.
        for (const auto& p : m_plots)
            if (p->layers == members && p->panes == panes) { plot = p; break; }
        if (!plot) {
            for (quint64 id : members) detachLayer(id);
            plot = std::make_shared<Plot>();
            plot->layers    = members;
            plot->panes     = panes;
            // A scope spanning panes can only have come from the sync group — the resolver
            // returns one pane otherwise — so this is the merge that a later unsync invalidates.
            plot->syncMerge = panes.size() > 1;
            m_plots.push_back(plot);
            for (quint64 id : members) m_by_layer.insert(id, plot);
        }
        plot->curves.clear();
    }

    for (auto& h : hits) {
        Curve c   = spec;              // mode / statistic / p
        c.layerId   = h.layerId;
        c.paneId    = h.paneId;
        c.paneColor = h.paneColor;
        c.paneLabel = h.paneLabel;     // shown only while the plot spans >1 pane
        c.layerName = h.layerName;
        c.values    = std::move(h.values);
        // Re-running the same layer with the same statistic REPLACES its curve rather than
        // stacking a duplicate on top of itself — otherwise persisting through a Mask No-Data
        // toggle would draw the same profile twice.
        const QString key = curveKey(c);
        bool replaced = false;
        for (Curve& e : plot->curves)
            if (curveKey(e) == key) { e = c; replaced = true; break; }
        if (!replaced) plot->curves.push_back(std::move(c));
    }

    // Title from the FINAL contents, not just this Compute's: with Persist on the plot may
    // already span panes and layers this gesture never touched.
    retitle(plot);

    m_current = plot;
    render();
}

// ---------------------------------------------------------------------------
// Scope bookkeeping — the Spectral Plot's, applied to profiles (FR-ANL-12)
// ---------------------------------------------------------------------------

// By value on purpose: `clearCurrent` passes `m_current`, and the reset below would otherwise
// destroy the very shared_ptr the parameter aliases.
void ScanPixProfilePanel::erasePlot(PlotPtr p) {
    if (!p) return;
    for (quint64 id : p->layers) {
        auto it = m_by_layer.find(id);
        if (it != m_by_layer.end() && *it == p) m_by_layer.erase(it);
    }
    m_plots.erase(std::remove(m_plots.begin(), m_plots.end(), p), m_plots.end());
    if (m_current == p) m_current.reset();
}

void ScanPixProfilePanel::detachLayer(quint64 layerId) {
    auto it = m_by_layer.find(layerId);
    if (it == m_by_layer.end()) return;
    PlotPtr p = *it;
    m_by_layer.erase(it);
    p->layers.remove(layerId);
    p->curves.erase(std::remove_if(p->curves.begin(), p->curves.end(),
                                   [layerId](const Curve& c) { return c.layerId == layerId; }),
                    p->curves.end());
    // A plot with no members left is unreachable — nothing can activate it again.
    if (p->layers.isEmpty()) erasePlot(p);
}

void ScanPixProfilePanel::retitle(const PlotPtr& p) {
    if (!p || p->curves.isEmpty()) return;
    QStringList paneLabels;
    for (const Curve& c : p->curves)
        if (!c.paneLabel.isEmpty() && !paneLabels.contains(c.paneLabel))
            paneLabels.push_back(c.paneLabel);
    p->title = buildProfileTitle(paneLabels, p->curves.size(), p->curves.front().layerName,
                                 p->curves.front().paneLabel);
}

void ScanPixProfilePanel::deleteLegendRow(int legendRow) {
    // The legend's own numbering, which skips curves that draw nothing and rows the user has
    // hidden; the dialog addresses curves directly and calls deleteCurve().
    if (!m_current || legendRow < 0 || legendRow >= m_legend_curve.size()) return;
    deleteCurve(m_legend_curve[legendRow]);
}

void ScanPixProfilePanel::deleteCurve(int idx) {
    if (!m_current || idx < 0 || idx >= m_current->curves.size()) return;

    const quint64 layerId = m_current->curves[idx].layerId;
    PlotPtr plot = m_current;                    // detachLayer/erasePlot may clear m_current
    plot->curves.remove(idx);

    // A layer with no curve left is no longer part of the scope, so activating it falls back
    // to a blank chart rather than to a plot that says nothing about it.
    bool stillHere = false;
    for (const Curve& c : plot->curves) if (c.layerId == layerId) { stillHere = true; break; }
    if (!stillHere) detachLayer(layerId);        // erases the plot if that was its last layer

    if (plot->curves.isEmpty()) erasePlot(plot); // deleting the last curve == pressing Clear
    else                        retitle(plot);
    render();
}

void ScanPixProfilePanel::showLayerPlot(int layerIndex) {
    m_current.reset();
    if (m_mgr) {
        auto l = m_mgr->layerAt(layerIndex);
        if (l && l->type() == LayerType::Raster) {
            auto it = m_by_layer.constFind(static_cast<RasterLayer*>(l.get())->layerId());
            if (it != m_by_layer.constEnd()) m_current = *it;
        }
    }
    render();
}

void ScanPixProfilePanel::forgetLayer(quint64 layerId) {
    if (!m_by_layer.contains(layerId)) return;
    detachLayer(layerId);
    render();
}

void ScanPixProfilePanel::dropMergedPlotsOutside(const QSet<quint64>& syncedPanes) {
    // Only a SYNC merge can be invalidated by a sync change: a single-pane plot is still exactly
    // what its pane shows, and a cross-pane plot the user assembled with Persist is a comparison
    // they asked for, not a statement about panes moving together. This runs on every sync-role
    // change (including a master rename/recolour), so it must keep merges still fully synced.
    std::vector<PlotPtr> doomed;
    for (const auto& p : m_plots) {
        if (!p->syncMerge || p->panes.size() < 2) continue;
        for (quint64 pid : p->panes)
            if (!syncedPanes.contains(pid)) { doomed.push_back(p); break; }
    }
    if (doomed.empty()) return;
    for (const auto& p : doomed) erasePlot(p);
    render();
}

void ScanPixProfilePanel::forgetAll() {
    if (m_plots.empty() && m_by_layer.isEmpty() && m_name_override.isEmpty()
        && m_x_override.isEmpty() && m_y_override.isEmpty())
        return;                      // nothing to drop, and no reason to force a repaint
    m_plots.clear();
    m_by_layer.clear();
    m_current.reset();
    m_name_override.clear();         // label edits belong to the plots they annotated
    m_x_override.clear();
    m_y_override.clear();
    render();
}

void ScanPixProfilePanel::clearCurrent() {
    if (!m_current) return;
    erasePlot(m_current);
    render();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void ScanPixProfilePanel::applyChartTheme() {
    if (!m_chart) return;
    // QChart is painted, not styled by the QSS, so the chrome is tinted from the palette —
    // the same treatment SpectralPlotPanel gives its chart.
    const QPalette pal = QApplication::palette();
    const QColor bg = pal.window().color();
    const QColor fg = pal.windowText().color();
    QColor grid = fg;
    grid.setAlpha(60);

    m_chart->setBackgroundBrush(bg);
    m_chart->setBackgroundPen(Qt::NoPen);
    m_chart->setPlotAreaBackgroundBrush(bg);
    m_chart->setPlotAreaBackgroundVisible(true);
    m_chart->setTitleBrush(fg);
    m_chart->legend()->setLabelColor(fg);
    if (m_chart_view) m_chart_view->setBackgroundBrush(bg);

    for (auto* ax : m_chart->axes()) {
        ax->setLabelsColor(fg);
        ax->setTitleBrush(fg);
        ax->setLinePenColor(grid);
        ax->setGridLineColor(grid);
    }
}

QByteArray ScanPixProfilePanel::saveSplitState() const {
    return m_split ? m_split->saveState() : QByteArray();
}

void ScanPixProfilePanel::restoreSplitState(const QByteArray& state) {
    if (m_split && !state.isEmpty()) m_split->restoreState(state);
}

void ScanPixProfilePanel::render() {
    if (!m_chart) return;

    m_chart->removeAllSeries();
    const auto oldAxes = m_chart->axes();
    for (auto* ax : oldAxes) m_chart->removeAxis(ax);

    const bool haveCurves = m_current && !m_current->curves.isEmpty();

    // Title: the plot's own scope when there is one, otherwise the layer the user just
    // activated — a blank chart still has to name what it would show.
    QString title;
    if (haveCurves && m_current->titleHidden) {
        title.clear();                       // deliberately no title (FR-ANL-10)
    } else if (haveCurves) {
        // An override replaces the whole title, prefix included — a user who typed a title
        // meant that title, not "Profile: " plus that title.
        title = m_current->titleOverride.isEmpty() ? tr("Profile: %1").arg(m_current->title)
                                                   : m_current->titleOverride;
    } else if (m_mgr) {
        auto l = m_mgr->activeLayer();
        title = l ? tr("%1 — no profile computed").arg(l->name()) : tr("Profile");
    } else {
        title = tr("Profile");
    }
    m_chart->setTitle(title);

    if (!haveCurves) {
        m_legend_curve.clear();
        if (m_legend) m_legend->setEntries({});
        m_status->setText(tr("Press Compute to profile the active layer — or, when its pane is "
                             "synced, every visible layer across the sync group."));
        // The axes were just removed, so any captured home names destroyed objects.
        if (m_chart_view) m_chart_view->captureHome();
        applyChartTheme();
        return;
    }

    // A curve's colour names its PANE (FR-ANL-8): hue from the pane colour, a lightness step
    // and a dash pattern for its position among THAT pane's curves. "Original palette" drops
    // all of that and cycles ten fixed hues by curve position.
    static const QColor kLegacyColors[] = {
        QColor(0x1f, 0x77, 0xb4), QColor(0xff, 0x7f, 0x0e), QColor(0x2c, 0xa0, 0x2c),
        QColor(0xd6, 0x27, 0x28), QColor(0x94, 0x67, 0xbd), QColor(0x8c, 0x56, 0x4b),
        QColor(0xe3, 0x77, 0xc2), QColor(0x7f, 0x7f, 0x7f), QColor(0xbc, 0xbd, 0x22),
        QColor(0x17, 0xbe, 0xcf),
    };
    const bool byPane = Settings::instance().curveColorScheme() != 1;
    const bool isDark = QApplication::palette().window().color().lightness() < 128;
    const QColor fallbackPane = QApplication::palette().highlight().color();
    QHash<quint64, int> seenInPane;
    QVector<FvLegendEntry> legend;
    m_legend_curve.clear();
    int drawn = 0;

    double xMax = 1.0;
    double yMin = std::numeric_limits<double>::max();
    double yMax = std::numeric_limits<double>::lowest();

    for (int ci = 0; ci < m_current->curves.size(); ++ci) {
        const Curve& c = m_current->curves[ci];
        // Gather the drawable points BEFORE claiming a colour slot or a legend row: a curve
        // whose every line is masked away draws nothing, so it must contribute nothing — a
        // legend entry for a line that is not on the chart contradicts it.
        QList<QPointF> pts;
        for (size_t i = 0; i < c.values.size(); ++i) {
            const double y = c.values[i];
            if (std::isnan(y)) continue;   // fully-masked line: a gap, not a spike to zero
            pts.append(QPointF(static_cast<double>(i), y));
        }
        if (pts.isEmpty()) continue;

        const int idx = seenInPane[c.paneId]++;
        const QColor col = byPane
            ? fvCurveColor(c.paneColor.isValid() ? c.paneColor : fallbackPane, idx, isDark)
            : kLegacyColors[drawn % 10];
        const Qt::PenStyle dash = byPane ? fvCurveDash(idx) : Qt::SolidLine;
        QPen pen(col);
        pen.setWidth(2);
        pen.setStyle(dash);

        auto* series = new QLineSeries();
        series->setName(curveLabel(c));
        series->setPen(pen);
        series->replace(pts);
        for (const QPointF& pt : pts) {
            xMax = std::max(xMax, pt.x());
            yMin = std::min(yMin, pt.y());
            yMax = std::max(yMax, pt.y());
        }
        m_chart->addSeries(series);
        if (!c.legendHidden) {
            legend.push_back(FvLegendEntry{curveKey(c), curveLabel(c), col, dash, true, true});
            m_legend_curve.push_back(ci);    // this row draws m_current->curves[ci]
        }
        ++drawn;
    }

    if (m_legend) m_legend->setEntries(legend);

    if (m_chart->series().isEmpty()) {
        // Every line of every curve is no-data.
        m_status->setText(tr("Every line is no-data."));
        m_legend_curve.clear();
        if (m_legend) m_legend->setEntries({});
        if (m_chart_view) m_chart_view->captureHome();
        applyChartTheme();
        return;
    }
    m_status->setText(tr("%n curve(s)", "", drawn));

    if (yMin == yMax) { yMin -= 1.0; yMax += 1.0; }

    const bool scanMode = m_current->curves.front().scanMode;
    auto* axisX = new QValueAxis();
    axisX->setTitleText(m_x_hidden ? QString()
                                   : (m_x_override.isEmpty() ? (scanMode ? tr("Row") : tr("Column"))
                                                             : m_x_override));
    axisX->setRange(0.0, xMax);

    auto* axisY = new QValueAxis();
    axisY->setTitleText(m_y_hidden ? QString()
                                   : (m_y_override.isEmpty() ? tr("Value") : m_y_override));
    const double margin = (yMax - yMin) * 0.05;
    axisY->setRange(yMin - margin, yMax + margin);

    // A hand-set range replaces the auto-fit, and captureHome() below then makes it home.
    fvApplyAxisTicks(axisX, m_x_ticks);
    fvApplyAxisTicks(axisY, m_y_ticks);
    axisX->setGridLineVisible(m_grid);
    axisY->setGridLineVisible(m_grid);

    m_chart->addAxis(axisX, Qt::AlignBottom);
    m_chart->addAxis(axisY, Qt::AlignLeft);
    for (auto* s : m_chart->series()) {
        s->attachAxis(axisX);
        s->attachAxis(axisY);
    }

    // These auto-fitted ranges are what the toolbar's home button returns to. The axes are new
    // objects on every replot, so the capture has to happen here — one taken at construction
    // would key ranges to destroyed axes and home would be a no-op after the first refresh.
    if (m_chart_view) m_chart_view->captureHome();
    applyChartTheme();   // the axes are new objects, so they need re-tinting each time
}

void ScanPixProfilePanel::closeEvent(QCloseEvent* e) {
    // The profiles go with the window (FR-ANL-11/12) — the same rule the Spectral Plot has had
    // since Phase 26.4. It was wired only into MainWindow's event filter, so any close that did
    // not pass through that filter left the plots behind and the window reopened showing
    // profiles the user had already dismissed.
    forgetAll();
    QWidget::closeEvent(e);
}

void ScanPixProfilePanel::changeEvent(QEvent* e) {
    // A full re-render, not just applyChartTheme(): the curve colours come from a per-theme
    // lightness band (fvCurveColor), so a theme switch has to restyle the series as well as
    // the chrome. The VALUES do not depend on the theme, so nothing is recomputed.
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange)
        render();
    QWidget::changeEvent(e);
}
