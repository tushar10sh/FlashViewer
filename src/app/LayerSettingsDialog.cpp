#include "app/LayerSettingsDialog.hpp"
#include "core/RasterLayer.hpp"
#include "core/LayerManager.hpp"
#include "core/ColormapRegistry.hpp"
#include "render/MapCanvas.hpp"
#include "widgets/UiKit.hpp"
#include <gdal.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QMessageBox>

LayerSettingsDialog::LayerSettingsDialog(RasterLayer* layer, LayerManager* mgr, MapCanvas* canvas, QWidget* parent)
    : QDialog(parent)
    , m_layer(layer)
    , m_layer_mgr(mgr)
    , m_canvas(canvas)
{
    setWindowTitle(tr("Layer Settings — %1").arg(m_layer ? m_layer->name() : tr("No Layer")));
    resize(580, 520);
    setupUi();
    loadCurrentSettings();
}

void LayerSettingsDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);

    m_tab_widget = new QTabWidget(this);

    // --- Tab 1: General & Information ---
    auto* tabGeneral = new QWidget(this);
    auto* genForm = new QFormLayout(tabGeneral);
    genForm->setContentsMargins(16, 16, 16, 16);
    genForm->setSpacing(12);

    m_name_edit = new QLineEdit(this);
    genForm->addRow(tr("Layer Name:"), m_name_edit);

    m_visible_check = new QCheckBox(tr("Visible"), this);
    genForm->addRow(tr("Visibility:"), m_visible_check);

    auto* opacityLayout = new QHBoxLayout();
    m_opacity_slider = new QSlider(Qt::Horizontal, this);
    m_opacity_slider->setRange(0, 100);
    m_opacity_spin = new QSpinBox(this);
    m_opacity_spin->setRange(0, 100);
    m_opacity_spin->setSuffix("%");
    opacityLayout->addWidget(m_opacity_slider);
    opacityLayout->addWidget(m_opacity_spin);
    genForm->addRow(tr("Opacity:"), opacityLayout);

    connect(m_opacity_slider, &QSlider::valueChanged, m_opacity_spin, &QSpinBox::setValue);
    connect(m_opacity_spin, &QSpinBox::valueChanged, m_opacity_slider, &QSlider::setValue);

    m_pane_combo = new QComboBox(this);
    m_pane_combo->addItem(tr("All Viewports (Global Overlay)"), static_cast<qlonglong>(kAllPanesId));
    m_pane_combo->addItem(tr("Viewport Pane 1"), static_cast<qlonglong>(1));
    m_pane_combo->addItem(tr("Viewport Pane 2"), static_cast<qlonglong>(2));
    m_pane_combo->addItem(tr("Viewport Pane 3"), static_cast<qlonglong>(3));
    m_pane_combo->addItem(tr("Viewport Pane 4"), static_cast<qlonglong>(4));
    genForm->addRow(tr("Target Viewport Pane:"), m_pane_combo);

    auto* infoGroup = new QGroupBox(tr("Raster Dataset Metadata"), this);
    auto* infoLayout = new QFormLayout(infoGroup);

    if (m_layer && m_layer->dataset()) {
        auto* ds = m_layer->dataset();
        infoLayout->addRow(tr("Source File:"), new QLabel(QString::fromStdString(ds->filePath()), this));
        infoLayout->addRow(tr("Dimensions:"), new QLabel(QString("%1 × %2 px | %3 band(s)").arg(ds->width()).arg(ds->height()).arg(ds->bandCount()), this));
        infoLayout->addRow(tr("GDAL Data Type:"), new QLabel(QString::fromStdString(GDALGetDataTypeName(static_cast<GDALDataType>(ds->bandDataType(1)))), this));
        infoLayout->addRow(tr("CRS WKT:"), new QLabel(ds->isGeographic() ? tr("Geographic (EPSG:4326 / WGS84)") : tr("Projected CRS"), this));
    }
    genForm->addRow(infoGroup);
    m_tab_widget->addTab(tabGeneral, tr("General"));

    // --- Tab 2: Display Filter ---
    auto* tabFilter = new QWidget(this);
    auto* filterForm = new QFormLayout(tabFilter);
    filterForm->setContentsMargins(16, 16, 16, 16);
    filterForm->setSpacing(12);

    m_filter_mode_combo = new QComboBox(this);
    m_filter_mode_combo->addItem(tr("None (Disabled)"), static_cast<int>(RasterLayer::DisplayFilterMode::None));
    m_filter_mode_combo->addItem(tr("Checkerboard Pattern (Alternate Tiles)"), static_cast<int>(RasterLayer::DisplayFilterMode::Checkerboard));
    m_filter_mode_combo->addItem(tr("Vertical Swipe (X-Axis Boundary)"), static_cast<int>(RasterLayer::DisplayFilterMode::VerticalSwipe));
    m_filter_mode_combo->addItem(tr("Horizontal Swipe (Y-Axis Boundary)"), static_cast<int>(RasterLayer::DisplayFilterMode::HorizontalSwipe));
    filterForm->addRow(tr("Display Filter Mode:"), m_filter_mode_combo);

    m_filter_size_combo = new QComboBox(this);
    m_filter_size_combo->addItem("16 px", 16);
    m_filter_size_combo->addItem("32 px", 32);
    m_filter_size_combo->addItem("64 px (Default)", 64);
    m_filter_size_combo->addItem("128 px", 128);
    m_filter_size_combo->addItem("256 px", 256);
    filterForm->addRow(tr("Checkerboard Tile Size:"), m_filter_size_combo);

    auto* swipeXLayout = new QHBoxLayout();
    m_swipe_x_slider = new QSlider(Qt::Horizontal, this);
    m_swipe_x_slider->setRange(0, 100);
    m_swipe_x_spin = new QSpinBox(this);
    m_swipe_x_spin->setRange(0, 100);
    m_swipe_x_spin->setSuffix("%");
    swipeXLayout->addWidget(m_swipe_x_slider);
    swipeXLayout->addWidget(m_swipe_x_spin);
    filterForm->addRow(tr("Vertical Swipe Position (X):"), swipeXLayout);

    connect(m_swipe_x_slider, &QSlider::valueChanged, m_swipe_x_spin, &QSpinBox::setValue);
    connect(m_swipe_x_spin, &QSpinBox::valueChanged, m_swipe_x_slider, &QSlider::setValue);

    auto* swipeYLayout = new QHBoxLayout();
    m_swipe_y_slider = new QSlider(Qt::Horizontal, this);
    m_swipe_y_slider->setRange(0, 100);
    m_swipe_y_spin = new QSpinBox(this);
    m_swipe_y_spin->setRange(0, 100);
    m_swipe_y_spin->setSuffix("%");
    swipeYLayout->addWidget(m_swipe_y_slider);
    swipeYLayout->addWidget(m_swipe_y_spin);
    filterForm->addRow(tr("Horizontal Swipe Position (Y):"), swipeYLayout);

    connect(m_swipe_y_slider, &QSlider::valueChanged, m_swipe_y_spin, &QSpinBox::setValue);
    connect(m_swipe_y_spin, &QSpinBox::valueChanged, m_swipe_y_slider, &QSlider::setValue);

    auto* filterHelp = new QLabel(tr(
        "<b>Display Filter Behavior:</b><br/>"
        "• <b>Checkerboard:</b> Alternate screen tiles render this layer; background tiles show the underlying layer or dark canvas.<br/>"
        "• <b>Vertical Swipe:</b> Renders this layer up to X%, revealing underlying layers beyond.<br/>"
        "• <b>Horizontal Swipe:</b> Renders this layer down to Y%, revealing underlying layers below."
    ), this);
    filterHelp->setWordWrap(true);
    filterHelp->setStyleSheet("color: #90a4ae; font-size: 11px; padding: 8px; background: rgba(0,0,0,0.15); border-radius: 4px;");
    filterForm->addRow(filterHelp);

    m_tab_widget->addTab(tabFilter, tr("Display Filter"));

    // --- Tab 3: Bands & Color Rendering ---
    auto* tabColor = new QWidget(this);
    auto* colorForm = new QFormLayout(tabColor);
    colorForm->setContentsMargins(16, 16, 16, 16);
    colorForm->setSpacing(12);

    m_render_mode_combo = new QComboBox(this);
    m_render_mode_combo->addItem(tr("Grayscale / Pseudocolor Single-Band"), 0);
    m_render_mode_combo->addItem(tr("RGB Composite Multi-Band"), 1);
    colorForm->addRow(tr("Rendering Mode:"), m_render_mode_combo);

    m_red_band_combo = new QComboBox(this);
    m_green_band_combo = new QComboBox(this);
    m_blue_band_combo = new QComboBox(this);

    if (m_layer && m_layer->dataset()) {
        int bcnt = m_layer->dataset()->bandCount();
        for (int b = 1; b <= bcnt; ++b) {
            QString label = QString("Band %1 (%2)").arg(b).arg(QString::fromStdString(m_layer->dataset()->bandDescription(b)));
            m_red_band_combo->addItem(label, b);
            m_green_band_combo->addItem(label, b);
            m_blue_band_combo->addItem(label, b);
        }
    }
    colorForm->addRow(tr("Red / Gray Band:"), m_red_band_combo);
    colorForm->addRow(tr("Green Band:"), m_green_band_combo);
    colorForm->addRow(tr("Blue Band:"), m_blue_band_combo);

    m_colormap_combo = new QComboBox(this);
    const auto colormaps = ColormapRegistry::instance().all();
    for (const auto& cm : colormaps) {
        m_colormap_combo->addItem(QString::fromStdString(cm.name), cm.id);
    }
    colorForm->addRow(tr("Colormap Palette:"), m_colormap_combo);

    m_invert_colormap_check = new QCheckBox(tr("Invert Colormap Colors"), this);
    colorForm->addRow(tr("Invert Colormap:"), m_invert_colormap_check);

    m_tab_widget->addTab(tabColor, tr("Bands & Colormap"));

    // --- Tab 4: Contrast & Stretch ---
    auto* tabStretch = new QWidget(this);
    auto* stretchForm = new QFormLayout(tabStretch);
    stretchForm->setContentsMargins(16, 16, 16, 16);
    stretchForm->setSpacing(12);

    m_stretch_min_spin = new QDoubleSpinBox(this);
    m_stretch_min_spin->setRange(-1e9, 1e9);
    m_stretch_min_spin->setDecimals(4);
    stretchForm->addRow(tr("Min Stretch Value:"), m_stretch_min_spin);

    m_stretch_max_spin = new QDoubleSpinBox(this);
    m_stretch_max_spin->setRange(-1e9, 1e9);
    m_stretch_max_spin->setDecimals(4);
    stretchForm->addRow(tr("Max Stretch Value:"), m_stretch_max_spin);

    auto* presetBtnLayout = new QHBoxLayout();
    m_stretch_2_98_btn = new QPushButton(tr("2% - 98% Cumulative"), this);
    m_stretch_minmax_btn = new QPushButton(tr("Min - Max Full Range"), this);
    m_stretch_2sigma_btn = new QPushButton(tr("StdDev (2σ)"), this);
    presetBtnLayout->addWidget(m_stretch_2_98_btn);
    presetBtnLayout->addWidget(m_stretch_minmax_btn);
    presetBtnLayout->addWidget(m_stretch_2sigma_btn);
    stretchForm->addRow(tr("Auto-Stretch Presets:"), presetBtnLayout);

    connect(m_stretch_2_98_btn, &QPushButton::clicked, this, [this] {
        if (m_layer && m_layer->dataset()) {
            auto stats = m_layer->dataset()->bandStats(1);
            m_stretch_min_spin->setValue(stats.min);
            m_stretch_max_spin->setValue(stats.max);
        }
    });

    connect(m_stretch_minmax_btn, &QPushButton::clicked, this, [this] {
        if (m_layer && m_layer->dataset()) {
            auto stats = m_layer->dataset()->bandStats(1);
            m_stretch_min_spin->setValue(stats.min);
            m_stretch_max_spin->setValue(stats.max);
        }
    });

    connect(m_stretch_2sigma_btn, &QPushButton::clicked, this, [this] {
        if (m_layer && m_layer->dataset()) {
            auto stats = m_layer->dataset()->bandStats(1);
            double lo = std::max(stats.min, stats.mean - 2.0 * stats.stddev);
            double hi = std::min(stats.max, stats.mean + 2.0 * stats.stddev);
            m_stretch_min_spin->setValue(lo);
            m_stretch_max_spin->setValue(hi);
        }
    });

    m_tab_widget->addTab(tabStretch, tr("Contrast Stretch"));

    // --- Tab 5: Resampling & No-Data ---
    auto* tabResample = new QWidget(this);
    auto* resampleForm = new QFormLayout(tabResample);
    resampleForm->setContentsMargins(16, 16, 16, 16);
    resampleForm->setSpacing(12);

    m_resample_combo = new QComboBox(this);
    m_resample_combo->addItem(tr("Bilinear (Default)"), static_cast<int>(RasterLayer::DisplayResampling::Bilinear));
    m_resample_combo->addItem(tr("Bicubic (2x2 Footprint)"), static_cast<int>(RasterLayer::DisplayResampling::Bicubic2));
    m_resample_combo->addItem(tr("Bicubic (4x4 Footprint)"), static_cast<int>(RasterLayer::DisplayResampling::Bicubic4));
    resampleForm->addRow(tr("Display Resampling Mode:"), m_resample_combo);

    m_nodata_check = new QCheckBox(tr("Override Dataset No-Data Value"), this);
    resampleForm->addRow(tr("No-Data Override:"), m_nodata_check);

    m_nodata_spin = new QDoubleSpinBox(this);
    m_nodata_spin->setRange(-1e9, 1e9);
    m_nodata_spin->setDecimals(4);
    resampleForm->addRow(tr("No-Data Value:"), m_nodata_spin);

    m_nodata_color_btn = new QPushButton(tr("Choose Color..."), this);
    resampleForm->addRow(tr("No-Data Render Color:"), m_nodata_color_btn);

    connect(m_nodata_color_btn, &QPushButton::clicked, this, [this] {
        QColor picked = QColorDialog::getColor(m_nodata_color, this, tr("Select No-Data Pixel Color"));
        if (picked.isValid()) {
            m_nodata_color = picked;
            m_nodata_color_btn->setText(QString("RGBA(%1,%2,%3,%4)")
                .arg(picked.red()).arg(picked.green()).arg(picked.blue()).arg(picked.alpha()));
        }
    });

    m_tab_widget->addTab(tabResample, tr("Resampling & No-Data"));

    mainLayout->addWidget(m_tab_widget);

    // Dialog Button Box
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    mainLayout->addWidget(btnBox);

    connect(btnBox->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, &LayerSettingsDialog::onAccept);
    connect(btnBox->button(QDialogButtonBox::Cancel), &QPushButton::clicked, this, &QDialog::reject);
    connect(btnBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &LayerSettingsDialog::applySettings);
}

void LayerSettingsDialog::loadCurrentSettings() {
    if (!m_layer) return;

    m_name_edit->setText(m_layer->name());
    m_visible_check->setChecked(m_layer->visible());
    int opPct = static_cast<int>(m_layer->opacity() * 100.0f + 0.5f);
    m_opacity_slider->setValue(opPct);
    m_opacity_spin->setValue(opPct);

    quint64 curPane = m_layer->paneId();
    int paneIdx = m_pane_combo->findData(static_cast<qlonglong>(curPane));
    if (paneIdx >= 0) m_pane_combo->setCurrentIndex(paneIdx);

    // Filter
    int fModeIdx = m_filter_mode_combo->findData(static_cast<int>(m_layer->displayFilterMode()));
    if (fModeIdx >= 0) m_filter_mode_combo->setCurrentIndex(fModeIdx);

    int szIdx = m_filter_size_combo->findData(m_layer->displayFilterCheckSize());
    if (szIdx >= 0) m_filter_size_combo->setCurrentIndex(szIdx);

    int sx = static_cast<int>(m_layer->displayFilterSwipeX() * 100.0f + 0.5f);
    int sy = static_cast<int>(m_layer->displayFilterSwipeY() * 100.0f + 0.5f);
    m_swipe_x_slider->setValue(sx);
    m_swipe_x_spin->setValue(sx);
    m_swipe_y_slider->setValue(sy);
    m_swipe_y_spin->setValue(sy);

    // Bands & Color
    const auto& bm = m_layer->bandMapping();
    m_render_mode_combo->setCurrentIndex(bm.isGrayscale() ? 0 : 1);

    int rIdx = m_red_band_combo->findData(bm.red_idx);
    if (rIdx >= 0) m_red_band_combo->setCurrentIndex(rIdx);
    int gIdx = m_green_band_combo->findData(bm.green_idx);
    if (gIdx >= 0) m_green_band_combo->setCurrentIndex(gIdx);
    int bIdx = m_blue_band_combo->findData(bm.blue_idx);
    if (bIdx >= 0) m_blue_band_combo->setCurrentIndex(bIdx);

    int cmIdx = m_colormap_combo->findData(m_layer->colormapId());
    if (cmIdx >= 0) m_colormap_combo->setCurrentIndex(cmIdx);
    m_invert_colormap_check->setChecked(m_layer->colormapInvert());

    // Stretch
    m_stretch_min_spin->setValue(m_layer->stretchMin());
    m_stretch_max_spin->setValue(m_layer->stretchMax());

    // Resampling & No-data
    int rsIdx = m_resample_combo->findData(static_cast<int>(m_layer->displayResampling()));
    if (rsIdx >= 0) m_resample_combo->setCurrentIndex(rsIdx);

    m_nodata_check->setChecked(m_layer->hasNoDataOverride());
    m_nodata_spin->setValue(m_layer->noDataOverrideValue());
    m_nodata_color = m_layer->nodataColor();
    m_nodata_color_btn->setText(QString("RGBA(%1,%2,%3,%4)")
        .arg(m_nodata_color.red()).arg(m_nodata_color.green()).arg(m_nodata_color.blue()).arg(m_nodata_color.alpha()));
}

void LayerSettingsDialog::applySettings() {
    if (!m_layer) return;

    m_layer->setName(m_name_edit->text());
    m_layer->setVisible(m_visible_check->isChecked());
    m_layer->setOpacity(m_opacity_slider->value() / 100.0f);
    m_layer->setPaneId(static_cast<uint64_t>(m_pane_combo->currentData().toLongLong()));

    // Filter
    auto fMode = static_cast<RasterLayer::DisplayFilterMode>(m_filter_mode_combo->currentData().toInt());
    m_layer->setDisplayFilterMode(fMode);
    m_layer->setDisplayFilterCheckSize(m_filter_size_combo->currentData().toInt());
    m_layer->setDisplayFilterSwipeX(m_swipe_x_slider->value() / 100.0f);
    m_layer->setDisplayFilterSwipeY(m_swipe_y_slider->value() / 100.0f);

    // Bands
    BandMapping bm;
    if (m_render_mode_combo->currentIndex() == 1) {
        bm = BandMapping::rgb(
            m_red_band_combo->currentData().toInt(),
            m_green_band_combo->currentData().toInt(),
            m_blue_band_combo->currentData().toInt()
        );
    } else {
        bm = BandMapping::gray(m_red_band_combo->currentData().toInt());
    }
    m_layer->setBandMapping(bm);
    m_layer->setColormapId(m_colormap_combo->currentData().toInt());
    m_layer->setColormapInvert(m_invert_colormap_check->isChecked());

    // Stretch
    m_layer->setStretch(static_cast<float>(m_stretch_min_spin->value()), static_cast<float>(m_stretch_max_spin->value()));

    // Resampling & No-Data
    m_layer->setDisplayResampling(static_cast<RasterLayer::DisplayResampling>(m_resample_combo->currentData().toInt()));
    m_layer->setNoDataOverride(m_nodata_check->isChecked(), static_cast<float>(m_nodata_spin->value()));
    m_layer->setNodataColor(m_nodata_color);

    if (m_canvas) m_canvas->update();
    if (m_layer_mgr) {
        int idx = m_layer_mgr->activeIndex();
        if (idx >= 0) m_layer_mgr->notifyLayerChanged(idx);
    }
}

void LayerSettingsDialog::onAccept() {
    applySettings();
    QDialog::accept();
}
