#include "plots/SpectralPlotPanel.hpp"
#include "gis/PixelSampler.hpp"
#include "core/LayerManager.hpp"
#include "core/RasterLayer.hpp"
#include "core/Layer.hpp"
#include "widgets/UiKit.hpp"        // FvTickCheckBox — one checkbox idiom app-wide (FR-APP-15)
#include "widgets/ChartTools.hpp"   // FvChartView / FvChartToolbar — shared with the Profile
#include "widgets/CurveStyle.hpp"   // fvCurveColor / fvCurveDash — a curve names its pane
#include "app/Settings.hpp"        // the persisted colour-scheme choice (FR-APP-6)

#include <QtCharts/QChart>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

SpectralPlotPanel::SpectralPlotPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
    render();
}

void SpectralPlotPanel::setupUi() {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_chart = new QChart();
    m_chart->setAnimationOptions(QChart::NoAnimation);
    // QChart's own legend is retired (FR-ANL-9): it lays markers out horizontally, elides each
    // to one line, and draws a plain colour block that cannot show FR-ANL-8's dash patterns.
    m_chart->legend()->setVisible(false);

    m_chart_view = new FvChartView(m_chart, this);
    m_chart_view->setMinimumHeight(160);

    // Navigation + Save come from the shared toolbar, so this panel and the Scan/Pixel
    // Profile behave identically (FR-APP-15). The panel's own controls ride along after it.
    auto* toolbar = new FvChartToolbar(m_chart_view, this);
    toolbar->setSuggestedName(QStringLiteral("spectral_plot"));
    toolbar->setCsvProvider([this] { return curvesAsCsv(); });
    // Labels are edited by right-clicking them (FR-ANL-10): the chart view hit-tests the
    // title / axis-title bands, the panel names the label and offers the one action.
    connect(m_chart_view, &FvChartView::labelContextMenuRequested, this,
            [this](FvChartLabel which, const QPoint& globalPos) {
                QMenu menu(this);
                QAction* edit = menu.addAction(tr("Edit Label…"));
                if (menu.exec(globalPos) == edit) editLabel(which);
            });

    // Pane hue (the default) or the original fixed palette — a plot property, so it lives on
    // the toolbar rather than in a preferences dialog, but it persists (FR-APP-6).
    m_scheme = new QComboBox(this);
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

    // Off by default: a click replaces the scope's curves, exactly like the Pixel Inspector
    // table replaces its rows. Ticking it keeps earlier pixels so several can be compared.
    // FvTickCheckBox, the same outlined-box-with-a-stroked-tick the "Override No-Data"
    // checkbox uses — every checkbox in the app draws alike (FR-APP-15).
    m_persist = new FvTickCheckBox(tr("Persist curves"), this);
    m_persist->setToolTip(tr("Keep the curves from earlier clicks instead of replacing them"));
    toolbar->addTrailingWidget(m_persist);
    auto* clearBtn = new QPushButton(tr("Clear"), this);
    clearBtn->setToolTip(tr("Discard the plot on screen"));
    connect(clearBtn, &QPushButton::clicked, this, &SpectralPlotPanel::clearCurrent);
    toolbar->addTrailingWidget(clearBtn);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(toolbar);
    bar->addStretch(1);
    lay->addLayout(bar);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    // Chart and legend live in ONE container, side by side, and that container is what Save
    // renders — exporting the chart view alone would lose the legend entirely.
    auto* plotArea = new QWidget(this);
    auto* plotLay  = new QHBoxLayout(plotArea);
    plotLay->setContentsMargins(0, 0, 0, 0);
    plotLay->setSpacing(4);
    plotLay->addWidget(m_chart_view, 1);
    m_legend = new FvChartLegend(plotArea);
    connect(m_legend, &FvChartLegend::entryRenamed, this,
            [this](const QString& key, const QString& text) {
                bool ok = false;
                const quint64 id = key.toULongLong(&ok);
                if (!ok) return;
                // Removing the key IS the reset: nothing stores an empty override, so the
                // automatic label can never be shadowed by a blank one.
                if (text.isEmpty()) m_name_override.remove(id);
                else                m_name_override.insert(id, text);
                render();
            });
    connect(m_legend, &FvChartLegend::entryDeleted,
            this, &SpectralPlotPanel::deleteLegendRow);
    plotLay->addWidget(m_legend, 0);
    lay->addWidget(plotArea, 1);
    toolbar->setExportWidget(plotArea);
}

QString SpectralPlotPanel::curveLabel(const Curve& c) const {
    const QString name = m_name_override.value(c.layerId, c.layerName);
    // The pane prefix is decided HERE, not stamped into the curve: Persist can add a second
    // pane to a plot after this curve was sampled, and every curve must then say which pane
    // it came from.
    const bool multiPane = m_current && m_current->panes.size() > 1;
    QString out;
    if (multiPane && !c.paneLabel.isEmpty()) out += c.paneLabel + QStringLiteral(" / ");
    out += name;
    // Newline, not a space: the legend label wraps the coordinates onto their own line
    // (FR-ANL-9), so a long filename and a long coordinate pair never share one row.
    if (!c.coord.isEmpty()) out += QStringLiteral("\n") + c.coord;
    return out;
}

void SpectralPlotPanel::defaultLabels(QString& title, QString& x, QString& y) const {
    title = m_current ? m_current->title : tr("Spectral Profile");
    x = tr("Band");
    y = tr("Value");
}

void SpectralPlotPanel::editLabel(FvChartLabel which) {
    QString dt, dx, dy;
    defaultLabels(dt, dx, dy);
    switch (which) {
        case FvChartLabel::Title: {
            // Nothing to title yet: a blank chart's heading is the activated layer's name, and
            // an override would then outlive the plot it was written for.
            if (!m_current) return;
            QString v = m_current->titleOverride;
            if (fvPromptChartLabel(this, tr("Plot Title"), dt, v)) m_current->titleOverride = v;
            break;
        }
        case FvChartLabel::XAxis: {
            QString v = m_x_override;
            if (fvPromptChartLabel(this, tr("X Axis Title"), dx, v)) m_x_override = v;
            break;
        }
        case FvChartLabel::YAxis: {
            QString v = m_y_override;
            if (fvPromptChartLabel(this, tr("Y Axis Title"), dy, v)) m_y_override = v;
            break;
        }
    }
    render();
}

QString SpectralPlotPanel::curvesAsCsv() const {
    if (!m_current || m_current->curves.isEmpty()) return {};

    // Curves of one plot can differ in band count (layers of different depth merged by a
    // right-click), so the table is as wide as the deepest and short curves end early.
    size_t bands = 0;
    for (const Curve& c : m_current->curves) bands = std::max(bands, c.values.size());
    if (bands == 0) return {};

    auto quote = [](QString s) {
        s.replace('"', QStringLiteral("\"\""));
        return '"' + s + '"';
    };

    // Exports carry what is ON SCREEN, edits included — a CSV headed by labels the user has
    // replaced would not match the chart it came from. The legend's newline is flattened to a
    // space; a CSV field cannot hold one without quoting games at the far end.
    QString out;
    out += "# " + (m_current->titleOverride.isEmpty() ? m_current->title
                                                      : m_current->titleOverride) + "\n";
    out += (m_x_override.isEmpty() ? tr("Band") : m_x_override);
    for (const Curve& c : m_current->curves)
        out += "," + quote(QString(curveLabel(c)).replace('\n', ' '));
    out += "\n";

    for (size_t b = 0; b < bands; ++b) {
        out += QString::number(b + 1);
        for (const Curve& c : m_current->curves) {
            out += ",";
            // Past the end, or no-data: an EMPTY field. Writing "nan" would be read back as
            // a value by most spreadsheet importers.
            if (b < c.values.size() && !std::isnan(c.values[b]))
                out += QString::number(c.values[b], 'g', 10);
        }
        out += "\n";
    }
    return out;
}

void SpectralPlotPanel::setLayerManager(LayerManager* mgr) {
    m_mgr = mgr;
    render();
}

// ---------------------------------------------------------------------------
// Scope bookkeeping
// ---------------------------------------------------------------------------

// By value on purpose: `clearCurrent` passes `m_current`, and the reset below would otherwise
// destroy the very shared_ptr the parameter aliases.
void SpectralPlotPanel::erasePlot(PlotPtr p) {
    if (!p) return;
    for (quint64 id : p->layers) {
        auto it = m_by_layer.find(id);
        if (it != m_by_layer.end() && *it == p) m_by_layer.erase(it);
    }
    m_plots.erase(std::remove(m_plots.begin(), m_plots.end(), p), m_plots.end());
    if (m_current == p) m_current.reset();
}

void SpectralPlotPanel::detachLayer(quint64 layerId) {
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

// Name the plot after the gesture that produced it, so the title always says WHOSE spectra
// are drawn: a single layer by name, one pane's merge by pane, a sync-group merge by the
// panes it spans plus the left/right-click layer rule.
static QString buildTitle(const QStringList& paneLabels, bool allLayers, int curveCount,
                          const QString& soleLayerName, const QString& solePaneLabel,
                          bool syncMerge) {
    if (curveCount == 1)
        return solePaneLabel.isEmpty() ? soleLayerName
                                       : SpectralPlotPanel::tr("%1 — %2")
                                             .arg(soleLayerName, solePaneLabel);
    if (paneLabels.size() <= 1) {
        const QString pane = paneLabels.isEmpty() ? QString() : paneLabels.front();
        if (pane.isEmpty()) return SpectralPlotPanel::tr("%n curve(s)", "", curveCount);
        return allLayers ? SpectralPlotPanel::tr("%1 — all layers").arg(pane)
                         : SpectralPlotPanel::tr("%1 — topmost layers").arg(pane);
    }
    // "Synced" only when ONE gesture spanned the panes. Persist can put two UNSYNCED panes on
    // one chart (FR-ANL-5), and calling that synced would state something untrue about the
    // panes rather than about the curves.
    const QString panes = paneLabels.join(QStringLiteral(", "));
    if (!syncMerge) return SpectralPlotPanel::tr("%1 — %n curve(s)", "", curveCount).arg(panes);
    return allLayers ? SpectralPlotPanel::tr("Synced %1 — all layers").arg(panes)
                     : SpectralPlotPanel::tr("Synced %1 — topmost layers").arg(panes);
}

void SpectralPlotPanel::retitle(const PlotPtr& p) {
    if (!p || p->curves.isEmpty()) return;
    QStringList paneLabels;
    for (const Curve& c : p->curves)
        if (!c.paneLabel.isEmpty() && !paneLabels.contains(c.paneLabel))
            paneLabels.push_back(c.paneLabel);
    p->title = buildTitle(paneLabels, p->allLayers, p->curves.size(),
                          p->curves.front().layerName, p->curves.front().paneLabel,
                          p->syncMerge);
}

void SpectralPlotPanel::addInspectResult(double gx, double gy, const std::string& geoWkt,
                                         const QVector<InspectPaneGroup>& groups,
                                         bool allLayers) {
    // Sample everything FIRST. A pane whose layers all fall outside the click contributes no
    // curve, so it must not appear in the title or widen the scope either.
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
    QStringList      paneLabels;   // in click order, for the title

    for (const auto& g : groups) {
        bool any = false;
        for (const auto& e : g.layers) {
            if (!e.layer) continue;
            std::vector<double> vals;
            if (!fvSamplePixelBands(e.layer, gx, gy, geoWkt, vals) || vals.empty()) continue;
            hits.push_back(Hit{g.paneId, g.paneLabel, g.paneColor, e.name,
                               e.layer->layerId(), std::move(vals)});
            any = true;
        }
        if (any && !panes.contains(g.paneId)) {
            panes.insert(g.paneId);
            paneLabels.push_back(g.paneLabel);
        }
    }

    if (hits.empty()) {
        // Nothing to plot here — say so without destroying what the user is already looking at.
        m_status->setText(tr("No data at this location."));
        return;
    }

    QSet<quint64> members;
    for (const auto& h : hits) members.insert(h.layerId);

    const bool persist = m_persist->isChecked();

    PlotPtr plot;
    if (persist && m_current) {
        // Grow the plot on screen, whatever this gesture sampled (FR-ANL-5). Persist used to
        // accumulate only within ONE scope, so comparing a pixel of pane 1 against a pixel of
        // an UNSYNCED pane 2 — the ordinary way to compare two scenes — was impossible: the
        // second click formed its own plot and the first vanished.
        plot = m_current;
        for (quint64 id : members)
            if (m_by_layer.value(id) != plot) detachLayer(id);
        plot->layers.unite(members);
        plot->panes.unite(panes);
        for (quint64 id : members) m_by_layer.insert(id, plot);
    } else {
        // Re-use the plot when the scope is identical; otherwise the sampled layers leave
        // their old plots and form a new one — the most recent gesture owns them.
        for (const auto& p : m_plots)
            if (p->layers == members && p->panes == panes) { plot = p; break; }
        if (!plot) {
            for (quint64 id : members) detachLayer(id);
            plot = std::make_shared<Plot>();
            plot->layers = members;
            plot->panes  = panes;
            // One gesture spanning panes can only have come from a sync group — inspectFromPane
            // targets the clicked pane alone otherwise.
            plot->syncMerge = panes.size() > 1;
            m_plots.push_back(plot);
            for (quint64 id : members) m_by_layer.insert(id, plot);
        }
        plot->curves.clear();
    }
    plot->allLayers = allLayers;

    for (auto& h : hits) {
        Curve c;
        c.layerId   = h.layerId;
        c.paneId    = h.paneId;
        c.paneColor = h.paneColor;
        c.paneLabel = h.paneLabel;     // shown only while the plot spans >1 pane
        c.layerName = h.layerName;
        // The coordinate is what tells two persisted clicks apart in the legend, so it is
        // formatted to 8 significant digits — fixed decimals would round two nearby clicks
        // in a geographic CRS to the same label. A rename never removes it.
        c.coord   = QString("(%1, %2)").arg(QString::number(gx, 'g', 8),
                                            QString::number(gy, 'g', 8));
        c.values  = std::move(h.values);
        plot->curves.push_back(std::move(c));
    }

    // Title from the FINAL contents, not just this gesture's: with Persist on the plot may
    // already span panes and layers this click never touched.
    retitle(plot);

    m_current = plot;
    render();
}

void SpectralPlotPanel::deleteLegendRow(int legendRow) {
    if (!m_current || legendRow < 0 || legendRow >= m_legend_curve.size()) return;
    const int idx = m_legend_curve[legendRow];
    if (idx < 0 || idx >= m_current->curves.size()) return;

    const quint64 layerId = m_current->curves[idx].layerId;
    PlotPtr plot = m_current;                    // detachLayer/erasePlot may clear m_current
    plot->curves.remove(idx);

    // A layer with no curve left is no longer part of the scope, so activating it falls back
    // to a blank chart rather than to someone else's merge.
    bool stillHere = false;
    for (const Curve& c : plot->curves) if (c.layerId == layerId) { stillHere = true; break; }
    if (!stillHere) detachLayer(layerId);        // erases the plot if that was its last layer

    if (plot->curves.isEmpty()) erasePlot(plot); // deleting the last curve == pressing Clear
    else                        retitle(plot);
    render();
}

void SpectralPlotPanel::showLayerPlot(int layerIndex) {
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

void SpectralPlotPanel::forgetLayer(quint64 layerId) {
    if (!m_by_layer.contains(layerId)) return;
    detachLayer(layerId);
    render();
}

void SpectralPlotPanel::dropMergedPlotsOutside(const QSet<quint64>& syncedPanes) {
    // Only cross-pane plots can be invalidated by a sync change; a single-pane plot is still
    // exactly what its pane shows. Note this runs on every sync-role change (including a
    // master rename/recolour), so it must keep merges whose panes are all still synced.
    std::vector<PlotPtr> doomed;
    for (const auto& p : m_plots) {
        if (p->panes.size() < 2) continue;
        for (quint64 pid : p->panes)
            if (!syncedPanes.contains(pid)) { doomed.push_back(p); break; }
    }
    if (doomed.empty()) return;
    for (const auto& p : doomed) erasePlot(p);
    render();
}

void SpectralPlotPanel::forgetAll() {
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

void SpectralPlotPanel::clearCurrent() {
    if (!m_current) return;
    erasePlot(m_current);
    render();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void SpectralPlotPanel::applyChartTheme() {
    if (!m_chart) return;
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

void SpectralPlotPanel::render() {
    if (!m_chart) return;

    m_chart->removeAllSeries();
    const auto oldAxes = m_chart->axes();
    for (auto* ax : oldAxes) m_chart->removeAxis(ax);

    const bool haveCurves = m_current && !m_current->curves.isEmpty();

    // Title: the plot's own scope when there is one, otherwise the layer the user just
    // activated — a blank chart still has to name what it would show.
    QString title;
    if (haveCurves) {
        title = m_current->titleOverride.isEmpty() ? m_current->title
                                                   : m_current->titleOverride;
    } else if (m_mgr) {
        auto l = m_mgr->activeLayer();
        title = l ? tr("%1 — no pixels selected").arg(l->name()) : tr("Spectral Profile");
    } else {
        title = tr("Spectral Profile");
    }
    m_chart->setTitle(title);

    if (!haveCurves) {
        m_legend_curve.clear();
        if (m_legend) m_legend->setEntries({});
        m_status->setText(tr("Inspect mode: left-click samples the topmost layers, "
                             "right-click samples all layers."));
        if (m_chart_view) m_chart_view->captureHome();   // no axes ⇒ nothing to return home to
        applyChartTheme();
        return;
    }
    double xMax = 1.0;
    double yMin = std::numeric_limits<double>::max();
    double yMax = std::numeric_limits<double>::lowest();

    // A curve's colour names its PANE (Phase 26.2): hue from the pane colour, a lightness
    // step and a dash pattern for its position among THAT pane's curves — so a merged plot
    // reads as "these three shades of blue are Pane 1, this orange is Pane 2" at a glance.
    // Counted per pane, not per plot, so pane 2's first curve is its own pane colour exactly
    // rather than a shade of it.
    // "Original palette" (FR-ANL-8) drops all of that and cycles ten fixed hues by curve
    // position — no pane meaning, but familiar and maximally distinct when the pane structure
    // is not what the user is reading.
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
    int drawn = 0;                       // curves that actually reached the chart

    for (int i = 0; i < m_current->curves.size(); ++i) {
        const Curve& c = m_current->curves[i];

        // Gather the drawable points BEFORE claiming a colour slot or a legend row. A curve
        // whose every band is no-data draws nothing, and must therefore contribute nothing:
        // it previously got a legend entry (and consumed a shade index, shifting the next
        // layer's colour) for a line that was never added to the chart.
        QList<QPointF> pts;
        for (size_t b = 0; b < c.values.size(); ++b) {
            const double y = c.values[b];
            if (std::isnan(y)) continue;   // no-data: leave a gap rather than a spike
            pts.append(QPointF(static_cast<double>(b + 1), y));
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
        legend.push_back(FvLegendEntry{QString::number(c.layerId), curveLabel(c), col, dash,
                                       true, true});
        m_legend_curve.push_back(i);     // this row draws m_current->curves[i]
        ++drawn;
    }

    if (m_legend) m_legend->setEntries(legend);

    if (m_chart->series().isEmpty()) {
        // Every sampled band was no-data.
        m_status->setText(tr("All sampled bands are no-data."));
        m_legend_curve.clear();
        if (m_legend) m_legend->setEntries({});
        if (m_chart_view) m_chart_view->captureHome();
        applyChartTheme();
        return;
    }
    // Counted from what was DRAWN: a scope can sample layers that are entirely no-data at
    // this pixel, and reporting those as curves would contradict the chart.
    m_status->setText(tr("%n curve(s)", "", drawn));

    if (yMin == yMax) { yMin -= 1.0; yMax += 1.0; }

    auto* axisX = new QValueAxis();
    axisX->setTitleText(m_x_override.isEmpty() ? tr("Band") : m_x_override);
    axisX->setRange(1.0, xMax);
    axisX->setLabelFormat("%d");
    // One tick per band while that stays readable; beyond that let Qt space them out.
    const int bands = static_cast<int>(xMax);
    axisX->setTickCount(bands <= 16 ? std::max(bands, 2) : 9);

    auto* axisY = new QValueAxis();
    axisY->setTitleText(m_y_override.isEmpty() ? tr("Value") : m_y_override);
    const double margin = (yMax - yMin) * 0.05;
    axisY->setRange(yMin - margin, yMax + margin);

    m_chart->addAxis(axisX, Qt::AlignBottom);
    m_chart->addAxis(axisY, Qt::AlignLeft);
    for (auto* s : m_chart->series()) {
        s->attachAxis(axisX);
        s->attachAxis(axisY);
    }

    // These auto-fitted ranges are what the toolbar's home button returns to. The axes are
    // new objects on every replot, so this must be re-captured here rather than once at
    // construction — a stale capture would key ranges to destroyed axes and home would be a
    // no-op after the first refresh.
    if (m_chart_view) m_chart_view->captureHome();

    applyChartTheme();
}

void SpectralPlotPanel::changeEvent(QEvent* e) {
    // A full re-render, not just applyChartTheme(): the curve colours are derived from a
    // per-theme lightness band (fvCurveColor), so a theme switch has to restyle the series
    // as well as the chrome. Rebuilding also re-captures the home range, which is correct —
    // the data has not moved.
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange)
        render();
    QWidget::changeEvent(e);
}
