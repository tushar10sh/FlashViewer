#include "panels/RasterInfoPanel.hpp"
#include "core/LayerManager.hpp"
#include "core/RasterLayer.hpp"
#include "core/Layer.hpp"
#include "io/RasterDataset.hpp"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QScrollArea>
#include <QResizeEvent>
#include <QStringList>
#include <QtMath>

namespace {
// Pretty-print a CRS WKT into an indented tree: put each node keyword on its own line,
// indented by its bracket depth. A "node" is an ALL-CAPS keyword that starts a bracketed
// element — PROJCS[, GEOGCS[, DATUM[, SPHEROID[, PROJECTION[, PARAMETER[, UNIT[, AXIS[,
// AUTHORITY[, ID[, … — detected as a comma followed by [A-Z][A-Z0-9_]* then '['. This
// deliberately does NOT break on all-caps VALUES such as the axis directions EAST/NORTH
// in AXIS["Easting",EAST] (not followed by '['), on numbers (`,6378137,298.25…`), or on
// anything inside a quoted "string". Whitespace between WKT elements is insignificant, so
// the result stays a valid, parseable WKT when copied. (#11 CRS display refinement.)
QString fvPrettyWkt(const QString& wkt) {
    QString out;
    out.reserve(wkt.size() + wkt.size() / 4);
    int depth = 0;
    bool inQuote = false;
    for (int i = 0; i < wkt.size(); ++i) {
        const QChar c = wkt[i];
        if (c == '"') { inQuote = !inQuote; out.append(c); continue; }
        if (inQuote)  { out.append(c); continue; }
        if (c == '[' || c == '(') { ++depth; out.append(c); continue; }
        if (c == ']' || c == ')') { if (depth > 0) --depth; out.append(c); continue; }
        if (c == ',') {
            out.append(c);
            // Look ahead for a node keyword: [A-Z][A-Z0-9_]* immediately followed by '['/'('.
            int k = i + 1;
            if (k < wkt.size() && wkt[k].isLetter() && wkt[k].isUpper()) {
                while (k < wkt.size() && (wkt[k].isUpper() || wkt[k].isDigit() || wkt[k] == '_'))
                    ++k;
                if (k < wkt.size() && (wkt[k] == '[' || wkt[k] == '(')) {
                    out.append('\n');
                    out.append(QString(depth * 2, QChar(' ')));   // indent by nesting depth
                }
            }
            continue;
        }
        out.append(c);
    }
    return out;
}
} // namespace

// Read-only, flat, auto-height value view that wraps long text ANYWHERE (not only at
// whitespace), so space-less machine strings — a CRS WKT (`...,AUTHORITY["EPSG","7030"]]...`)
// or a path (`C:/Users/.../Landsat_TOA_BT_100m.tif`) — wrap to the panel width instead of
// being clipped on the right (#11). Unlike inserting zero-width breaks into a QLabel, the
// wrapping here is layout-only, so selecting/copying the value yields the clean original
// text. The widget looks like a label (no frame, transparent background, palette text
// colour) and grows its height to fit the wrapped content at the current width.
class FvAutoWrapText : public QTextEdit {
public:
    explicit FvAutoWrapText(QWidget* parent = nullptr) : QTextEdit(parent) {
        setReadOnly(true);
        setFrameStyle(QFrame::NoFrame);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Selectable + copyable (read-only keeps the caret hidden); clean clipboard text.
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        // Look like a label: no chrome, transparent so the dock background shows through;
        // text colour still comes from the theme palette.
        setStyleSheet("QTextEdit { background: transparent; border: none; }");
        viewport()->setAutoFillBackground(false);
        document()->setDocumentMargin(0);
        // Ignore the horizontal size hint so a long value never widens the panel; height
        // is fixed to the wrapped-content height (recomputed on width changes).
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void setValue(const QString& text) { setPlainText(text); updateHeight(); }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QTextEdit::resizeEvent(e);
        updateHeight();
    }
    QSize minimumSizeHint() const override { return QSize(0, 0); }

private:
    void updateHeight() {
        document()->setTextWidth(viewport()->width());
        const int h = qCeil(document()->size().height());
        if (height() != h) setFixedHeight(h);   // guard against resize/setFixedHeight recursion
    }
};

RasterInfoPanel::RasterInfoPanel(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);

    auto* content = new QWidget(scroll);
    auto* form    = new QFormLayout(content);
    form->setContentsMargins(6,6,6,6);
    form->setLabelAlignment(Qt::AlignRight);
    // Value fields take the available width and wrap long rows onto their own line.
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    auto makeValue = [&](FvAutoWrapText*& w, const QString& text) {
        w = new FvAutoWrapText(content);
        w->setValue(text);
    };

    makeValue(m_path_lbl,       tr("—"));
    makeValue(m_crs_lbl,        tr("—"));
    makeValue(m_size_lbl,       tr("—"));
    makeValue(m_bands_lbl,      tr("—"));
    makeValue(m_stats_lbl,      tr("—"));
    makeValue(m_nodata_src_lbl, tr("—"));

    form->addRow(tr("File:"),          m_path_lbl);
    form->addRow(tr("CRS:"),           m_crs_lbl);
    form->addRow(tr("Size:"),          m_size_lbl);
    form->addRow(tr("Bands:"),         m_bands_lbl);
    form->addRow(tr("Statistics:"),    m_stats_lbl);
    form->addRow(tr("No-Data (src):"), m_nodata_src_lbl);

    content->setMinimumWidth(0);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0,0,0,0);
    lay->addWidget(scroll);
}

void RasterInfoPanel::setLayerManager(LayerManager* mgr) {
    if (m_mgr) disconnect(m_mgr, nullptr, this, nullptr);
    m_mgr = mgr;
    if (!m_mgr) { clearDisplay(); return; }
    connect(m_mgr, &LayerManager::activeLayerChanged,
            this, &RasterInfoPanel::onActiveLayerChanged);
    connect(m_mgr, &LayerManager::layerRemoved,
            this, &RasterInfoPanel::onActiveLayerChanged);
    connect(m_mgr, &LayerManager::layerAdded,
            this, [this](int idx){ onActiveLayerChanged(idx); });
    // A live Arrow Flight session's bandStats() are cached against an
    // all-zero MEM buffer at layerAdded time (before any tile has arrived)
    // and only become correct later, once
    // MainWindow::openLiveSession's LiveGeorefSession::finished handler
    // calls RasterDataset::invalidateStatsCache() and notifyLayerChanged() --
    // without this connection (unlike HistogramPanel, which already has it)
    // that later notifyLayerChanged() had nothing to make this panel
    // actually re-read the now-valid stats, so it kept showing the stale
    // all-zero snapshot from layerAdded indefinitely.
    connect(m_mgr, &LayerManager::layerChanged,
            this, &RasterInfoPanel::onActiveLayerChanged);
    clearDisplay();
}

void RasterInfoPanel::setSuppressed(bool on) {
    if (m_suppressed == on) return;
    m_suppressed = on;
    if (on) clearDisplay();
    else    onActiveLayerChanged(m_mgr ? m_mgr->activeIndex() : -1);
}

void RasterInfoPanel::onActiveLayerChanged(int index) {
    if (m_suppressed) { clearDisplay(); return; }
    if (!m_mgr || index < 0) { clearDisplay(); return; }
    auto layerPtr = m_mgr->activeLayer();
    if (!layerPtr) layerPtr = m_mgr->layerAt(index);
    if (!layerPtr || layerPtr->type() != LayerType::Raster) { clearDisplay(); return; }
    showLayer(static_cast<RasterLayer*>(layerPtr.get()));
}

void RasterInfoPanel::showLayer(RasterLayer* layer) {
    m_layer = layer;
    auto* ds = layer->dataset();
    if (!ds) { clearDisplay(); return; }

    // Full path and full CRS WKT; both wrap anywhere within the panel (no clipping, #11).
    m_path_lbl->setValue(QString::fromStdString(ds->filePath()));
    m_crs_lbl->setValue(ds->crsWkt().empty()
        ? tr("Unknown CRS")
        : fvPrettyWkt(QString::fromStdString(ds->crsWkt())));
    m_size_lbl->setValue(QString("%1 × %2 px").arg(ds->width()).arg(ds->height()));
    // Band count, plus the GDAL band descriptions when the dataset carries them — a
    // Phase-19 stacked NetCDF/HDF layer names its bands by variable (FR-IO-13), so the
    // row reads "3 (t2m, u10, v10)" instead of a bare count. Wraps like every value.
    const int nb = ds->bandCount();
    QStringList band_names;
    for (int b = 1; b <= nb; ++b) {
        const QString d = QString::fromStdString(ds->bandDescription(b)).trimmed();
        if (!d.isEmpty()) band_names << d;
    }
    m_bands_lbl->setValue(band_names.size() == nb
        ? QString("%1 (%2)").arg(nb).arg(band_names.join(QStringLiteral(", ")))
        : QString::number(nb));

    auto st = ds->bandStats(1);
    m_stats_lbl->setValue(
        QString("Band 1: min=%1 max=%2\nmean=%3 σ=%4")
            .arg(st.min, 0, 'g', 5).arg(st.max, 0, 'g', 5)
            .arg(st.mean, 0, 'g', 5).arg(st.stddev, 0, 'g', 5));

    auto nd = ds->noData(1);
    m_nodata_src_lbl->setValue(nd.has_value
        ? QString::number(nd.value, 'g', 8)
        : tr("(none)"));
}

void RasterInfoPanel::clearDisplay() {
    m_layer = nullptr;
    for (auto* w : {m_path_lbl, m_crs_lbl, m_size_lbl, m_bands_lbl,
                    m_stats_lbl, m_nodata_src_lbl})
        w->setValue(tr("—"));
}
