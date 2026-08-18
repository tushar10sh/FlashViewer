#include "panels/VectorLayerPanel.hpp"
#include "core/LayerManager.hpp"
#include "core/VectorLayer.hpp"
#include "gis/CrsUtil.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QColorDialog>
#include <QInputDialog>
#include <QFileInfo>
#include <QScrollArea>

VectorLayerPanel::VectorLayerPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(6, 6, 6, 6);
    mainLay->setSpacing(6);

    // 1. Layer Selection Header & Buttons
    auto* selGroup = new QGroupBox(tr("Vector Layers"), this);
    auto* selLay = new QVBoxLayout(selGroup);
    selLay->setContentsMargins(6, 6, 6, 6);
    selLay->setSpacing(4);

    m_layer_combo = new QComboBox(selGroup);
    connect(m_layer_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VectorLayerPanel::onLayerComboSelected);
    selLay->addWidget(m_layer_combo);

    auto* btnLay = new QHBoxLayout();
    btnLay->setSpacing(4);

    m_btn_add = new QPushButton(tr("＋ Add Shapefile…"), selGroup);
    m_btn_add->setToolTip(tr("Open an ESRI Shapefile (.shp) as a vector layer"));
    connect(m_btn_add, &QPushButton::clicked, this, &VectorLayerPanel::onAddLayerClicked);
    btnLay->addWidget(m_btn_add);

    m_btn_remove = new QPushButton(tr("✕ Remove"), selGroup);
    m_btn_remove->setToolTip(tr("Remove selected vector layer"));
    connect(m_btn_remove, &QPushButton::clicked, this, &VectorLayerPanel::onRemoveLayerClicked);
    btnLay->addWidget(m_btn_remove);

    m_btn_fit = new QPushButton(tr("⊡ Fit"), selGroup);
    m_btn_fit->setToolTip(tr("Fit view to this vector layer"));
    connect(m_btn_fit, &QPushButton::clicked, this, &VectorLayerPanel::onFitToLayerClicked);
    btnLay->addWidget(m_btn_fit);

    selLay->addLayout(btnLay);
    mainLay->addWidget(selGroup);

    // Scroll area for the configurator
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    m_config_container = new QWidget(scrollArea);
    auto* cfgLay = new QVBoxLayout(m_config_container);
    cfgLay->setContentsMargins(0, 0, 0, 0);
    cfgLay->setSpacing(6);

    // 2. Viewport Panel Overlay Target
    auto* paneGroup = new QGroupBox(tr("Viewport Panel Overlay"), m_config_container);
    auto* paneForm = new QFormLayout(paneGroup);
    paneForm->setContentsMargins(6, 6, 6, 6);
    paneForm->setSpacing(6);

    m_combo_target_pane = new QComboBox(paneGroup);
    m_combo_target_pane->setToolTip(tr("Choose which viewport panel to overlay this vector layer on"));
    connect(m_combo_target_pane, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VectorLayerPanel::onTargetPaneChanged);
    paneForm->addRow(tr("Overlay On:"), m_combo_target_pane);

    m_btn_duplicate = new QPushButton(tr("Duplicate to Another Pane…"), paneGroup);
    m_btn_duplicate->setToolTip(tr("Create an overlay copy of this vector on another viewport pane"));
    connect(m_btn_duplicate, &QPushButton::clicked, this, &VectorLayerPanel::onDuplicateToPaneClicked);
    paneForm->addRow(m_btn_duplicate);

    cfgLay->addWidget(paneGroup);

    // 3. Stroke & Outline Styling
    auto* strokeGroup = new QGroupBox(tr("Stroke / Outline (Lines & Polygons)"), m_config_container);
    auto* strokeForm = new QFormLayout(strokeGroup);
    strokeForm->setContentsMargins(6, 6, 6, 6);
    strokeForm->setSpacing(6);

    m_btn_stroke_color = new QPushButton(strokeGroup);
    m_btn_stroke_color->setMinimumHeight(24);
    connect(m_btn_stroke_color, &QPushButton::clicked, this, &VectorLayerPanel::onStrokeColorClicked);
    strokeForm->addRow(tr("Color:"), m_btn_stroke_color);

    m_spin_stroke_width = new QDoubleSpinBox(strokeGroup);
    m_spin_stroke_width->setRange(0.5, 20.0);
    m_spin_stroke_width->setSingleStep(0.5);
    m_spin_stroke_width->setValue(2.0);
    m_spin_stroke_width->setSuffix(tr(" px"));
    connect(m_spin_stroke_width, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &VectorLayerPanel::onStrokeWidthChanged);
    strokeForm->addRow(tr("Width:"), m_spin_stroke_width);

    m_combo_stroke_style = new QComboBox(strokeGroup);
    m_combo_stroke_style->addItem(tr("Solid Line"), static_cast<int>(Qt::SolidLine));
    m_combo_stroke_style->addItem(tr("Dash Line"), static_cast<int>(Qt::DashLine));
    m_combo_stroke_style->addItem(tr("Dot Line"), static_cast<int>(Qt::DotLine));
    m_combo_stroke_style->addItem(tr("Dash Dot Line"), static_cast<int>(Qt::DashDotLine));
    connect(m_combo_stroke_style, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VectorLayerPanel::onStrokeStyleChanged);
    strokeForm->addRow(tr("Style:"), m_combo_stroke_style);

    cfgLay->addWidget(strokeGroup);

    // 4. Fill Styling
    auto* fillGroup = new QGroupBox(tr("Fill (Polygons)"), m_config_container);
    auto* fillLay = new QVBoxLayout(fillGroup);
    fillLay->setContentsMargins(6, 6, 6, 6);
    fillLay->setSpacing(6);

    m_chk_transparent_fill = new QCheckBox(tr("Transparent Fill (Default)"), fillGroup);
    m_chk_transparent_fill->setChecked(true);
    connect(m_chk_transparent_fill, &QCheckBox::toggled, this, &VectorLayerPanel::onTransparentFillToggled);
    fillLay->addWidget(m_chk_transparent_fill);

    auto* fillForm = new QFormLayout();
    fillForm->setContentsMargins(0, 0, 0, 0);
    fillForm->setSpacing(4);

    m_btn_fill_color = new QPushButton(fillGroup);
    m_btn_fill_color->setMinimumHeight(24);
    connect(m_btn_fill_color, &QPushButton::clicked, this, &VectorLayerPanel::onFillColorClicked);
    fillForm->addRow(tr("Fill Color:"), m_btn_fill_color);

    auto* opLay = new QHBoxLayout();
    m_slider_fill_opacity = new QSlider(Qt::Horizontal, fillGroup);
    m_slider_fill_opacity->setRange(0, 100);
    m_slider_fill_opacity->setValue(0);
    m_lbl_fill_opacity = new QLabel(tr("0%"), fillGroup);
    m_lbl_fill_opacity->setFixedWidth(36);
    connect(m_slider_fill_opacity, &QSlider::valueChanged, this, &VectorLayerPanel::onFillOpacityChanged);
    opLay->addWidget(m_slider_fill_opacity);
    opLay->addWidget(m_lbl_fill_opacity);
    fillForm->addRow(tr("Opacity:"), opLay);

    fillLay->addLayout(fillForm);
    cfgLay->addWidget(fillGroup);

    // 4. Point Marker Styling
    auto* pointGroup = new QGroupBox(tr("Point Markers"), m_config_container);
    auto* ptForm = new QFormLayout(pointGroup);
    ptForm->setContentsMargins(6, 6, 6, 6);
    ptForm->setSpacing(6);

    m_spin_point_size = new QSpinBox(pointGroup);
    m_spin_point_size->setRange(2, 40);
    m_spin_point_size->setValue(6);
    m_spin_point_size->setSuffix(tr(" px"));
    connect(m_spin_point_size, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &VectorLayerPanel::onPointSizeChanged);
    ptForm->addRow(tr("Marker Size:"), m_spin_point_size);

    cfgLay->addWidget(pointGroup);

    // 5. Metadata / Layer Summary
    auto* metaGroup = new QGroupBox(tr("Layer Info"), m_config_container);
    auto* metaForm = new QFormLayout(metaGroup);
    metaForm->setContentsMargins(6, 6, 6, 6);
    metaForm->setSpacing(4);

    m_lbl_geom_type = new QLabel(metaGroup);
    metaForm->addRow(tr("Type:"), m_lbl_geom_type);

    m_lbl_features = new QLabel(metaGroup);
    metaForm->addRow(tr("Features:"), m_lbl_features);

    m_lbl_crs = new QLabel(metaGroup);
    m_lbl_crs->setWordWrap(true);
    metaForm->addRow(tr("CRS:"), m_lbl_crs);

    m_lbl_path = new QLabel(metaGroup);
    m_lbl_path->setWordWrap(true);
    metaForm->addRow(tr("Path:"), m_lbl_path);

    cfgLay->addWidget(metaGroup);
    cfgLay->addStretch();

    scrollArea->setWidget(m_config_container);
    mainLay->addWidget(scrollArea, 1);

    m_lbl_no_layer = new QLabel(tr("No vector layer selected.\nClick 'Add Shapefile…' or open a .shp file."), this);
    m_lbl_no_layer->setAlignment(Qt::AlignCenter);
    m_lbl_no_layer->setStyleSheet("color: gray; padding: 20px;");
    mainLay->addWidget(m_lbl_no_layer);

    updateUiFromLayer();
}

void VectorLayerPanel::setLayerManager(LayerManager* mgr) {
    if (m_mgr == mgr) return;
    m_mgr = mgr;
    if (m_mgr) {
        connect(m_mgr, &LayerManager::layerAdded, this, &VectorLayerPanel::onLayerListChanged);
        connect(m_mgr, &LayerManager::layerRemoved, this, &VectorLayerPanel::onLayerListChanged);
        connect(m_mgr, &LayerManager::activeLayerChanged, this, &VectorLayerPanel::onActiveLayerChanged);
        connect(m_mgr, &LayerManager::layerChanged, this, &VectorLayerPanel::onLayerListChanged);
    }
    rebuildLayerCombo();
}

void VectorLayerPanel::configureFor(VectorLayer* layer) {
    m_current_layer = layer;
    updateUiFromLayer();
}

void VectorLayerPanel::onLayerListChanged() {
    rebuildLayerCombo();
}

void VectorLayerPanel::onActiveLayerChanged(int index) {
    if (!m_mgr || index < 0 || index >= m_mgr->count()) {
        configureFor(nullptr);
        return;
    }
    auto layer = m_mgr->layerAt(index);
    if (layer && layer->type() == LayerType::Vector) {
        configureFor(static_cast<VectorLayer*>(layer.get()));
        // Sync combo selection
        m_updating = true;
        for (int i = 0; i < m_layer_combo->count(); ++i) {
            if (m_layer_combo->itemData(i).toInt() == index) {
                m_layer_combo->setCurrentIndex(i);
                break;
            }
        }
        m_updating = false;
    } else {
        // If an active raster is clicked, check if we should keep current vector or clear
        // Don't clear if current layer is still valid in manager
    }
}

void VectorLayerPanel::rebuildLayerCombo() {
    m_updating = true;
    m_layer_combo->clear();

    if (!m_mgr) {
        m_updating = false;
        updateUiFromLayer();
        return;
    }

    int selectedComboIndex = -1;
    for (int i = 0; i < m_mgr->count(); ++i) {
        auto l = m_mgr->layerAt(i);
        if (l && l->type() == LayerType::Vector) {
            m_layer_combo->addItem(QString("⬡ %1").arg(l->name()), i);
            if (l.get() == m_current_layer) {
                selectedComboIndex = m_layer_combo->count() - 1;
            }
        }
    }

    if (m_layer_combo->count() > 0) {
        if (selectedComboIndex >= 0) {
            m_layer_combo->setCurrentIndex(selectedComboIndex);
        } else {
            m_layer_combo->setCurrentIndex(0);
            int lyrIdx = m_layer_combo->currentData().toInt();
            auto l = m_mgr->layerAt(lyrIdx);
            m_current_layer = (l && l->type() == LayerType::Vector) ? static_cast<VectorLayer*>(l.get()) : nullptr;
        }
    } else {
        m_current_layer = nullptr;
    }

    m_updating = false;
    updateUiFromLayer();
}

void VectorLayerPanel::onLayerComboSelected(int comboIndex) {
    if (m_updating || !m_mgr || comboIndex < 0) return;
    int lyrIdx = m_layer_combo->itemData(comboIndex).toInt();
    if (lyrIdx >= 0 && lyrIdx < m_mgr->count()) {
        auto l = m_mgr->layerAt(lyrIdx);
        if (l && l->type() == LayerType::Vector) {
            m_current_layer = static_cast<VectorLayer*>(l.get());
            updateUiFromLayer();
            m_mgr->setActiveLayer(lyrIdx);
        }
    }
}

void VectorLayerPanel::updateUiFromLayer() {
    bool hasLayer = (m_current_layer != nullptr);

    m_config_container->setVisible(hasLayer);
    m_lbl_no_layer->setVisible(!hasLayer);
    m_btn_remove->setEnabled(hasLayer);
    m_btn_fit->setEnabled(hasLayer);

    if (!hasLayer) return;

    m_updating = true;

    // Target Pane Overlay
    m_combo_target_pane->blockSignals(true);
    m_combo_target_pane->clear();
    m_combo_target_pane->addItem(tr("All Panes (Global Overlay)"), static_cast<qulonglong>(kAllPanesId));

    if (m_pane_list) {
        for (const auto& [pid, label] : m_pane_list()) {
            m_combo_target_pane->addItem(label, static_cast<qulonglong>(pid));
        }
    }
    int paneIdx = m_combo_target_pane->findData(static_cast<qulonglong>(m_current_layer->paneId()));
    m_combo_target_pane->setCurrentIndex(paneIdx >= 0 ? paneIdx : 0);
    m_combo_target_pane->blockSignals(false);

    // Stroke
    m_spin_stroke_width->setValue(m_current_layer->strokeWidth());
    int styleIdx = m_combo_stroke_style->findData(static_cast<int>(m_current_layer->strokeStyle()));
    if (styleIdx >= 0) m_combo_stroke_style->setCurrentIndex(styleIdx);

    // Fill
    QColor fill = m_current_layer->fillColor();
    bool isTransparent = (fill.alpha() == 0);
    m_chk_transparent_fill->setChecked(isTransparent);
    m_btn_fill_color->setEnabled(!isTransparent);
    m_slider_fill_opacity->setEnabled(!isTransparent);
    int opPct = static_cast<int>(fill.alphaF() * 100.0f);
    m_slider_fill_opacity->setValue(opPct);
    m_lbl_fill_opacity->setText(QString("%1%").arg(opPct));

    // Point
    m_spin_point_size->setValue(static_cast<int>(m_current_layer->pointSize()));

    // Metadata
    if (auto* ds = m_current_layer->dataset()) {
        m_lbl_geom_type->setText(QString::fromStdString(ds->geometryTypeName()));
        m_lbl_features->setText(QString::number(ds->featureCount()));
        m_lbl_crs->setText(fvCrsShortName(ds->crsWkt()));
        m_lbl_path->setText(QString::fromStdString(ds->filePath()));
    } else {
        m_lbl_geom_type->setText(tr("Unknown"));
        m_lbl_features->setText(tr("0"));
        m_lbl_crs->setText(tr("None"));
        m_lbl_path->setText(tr(""));
    }

    updateColorButtons();

    m_updating = false;
}

void VectorLayerPanel::onTargetPaneChanged(int comboIndex) {
    if (m_updating || !m_current_layer || comboIndex < 0) return;
    uint64_t targetPid = m_combo_target_pane->itemData(comboIndex).toULongLong();
    if (m_current_layer->paneId() != targetPid) {
        m_current_layer->setPaneId(targetPid);
        if (m_mgr) {
            for (int i = 0; i < m_mgr->count(); ++i) {
                if (m_mgr->layerAt(i).get() == m_current_layer) {
                    m_mgr->notifyLayerChanged(i);
                    break;
                }
            }
        }
        emit layerStyleChanged(m_current_layer);
    }
}

void VectorLayerPanel::onDuplicateToPaneClicked() {
    if (!m_current_layer || !m_mgr) return;
    if (!m_pane_list) return;

    auto panes = m_pane_list();
    if (panes.empty()) return;

    QStringList options;
    std::vector<uint64_t> pids;
    options << tr("All Panes (Global Overlay)");
    pids.push_back(kAllPanesId);

    for (const auto& [pid, label] : panes) {
        options << label;
        pids.push_back(pid);
    }

    bool ok = false;
    QString chosen = QInputDialog::getItem(
        this, tr("Duplicate Vector Layer"),
        tr("Choose target pane to overlay a copy of '%1':").arg(m_current_layer->name()),
        options, 0, false, &ok);

    if (ok && !chosen.isEmpty()) {
        int idx = options.indexOf(chosen);
        if (idx >= 0 && idx < static_cast<int>(pids.size())) {
            uint64_t targetPid = pids[static_cast<size_t>(idx)];
            auto clone = std::make_shared<VectorLayer>(m_current_layer->datasetPtr());
            clone->setName(m_current_layer->name() + tr(" (Overlay)"));
            clone->setStrokeColor(m_current_layer->strokeColor());
            clone->setStrokeWidth(m_current_layer->strokeWidth());
            clone->setStrokeStyle(m_current_layer->strokeStyle());
            clone->setFillColor(m_current_layer->fillColor());
            clone->setPointSize(m_current_layer->pointSize());
            clone->setPaneId(targetPid);
            m_mgr->addLayer(clone);
            emit duplicateLayerRequested(m_current_layer, targetPid);
        }
    }
}

void VectorLayerPanel::updateColorButtons() {
    if (!m_current_layer) return;

    QColor sc = m_current_layer->strokeColor();
    m_btn_stroke_color->setStyleSheet(
        QString("background-color: %1; color: %2; font-weight: bold; border-radius: 3px; border: 1px solid #555;")
            .arg(sc.name(QColor::HexRgb))
            .arg(sc.lightness() > 128 ? "#000000" : "#ffffff"));
    m_btn_stroke_color->setText(sc.name(QColor::HexRgb).toUpper());

    QColor fc = m_current_layer->fillColor();
    if (fc.alpha() == 0) {
        m_btn_fill_color->setStyleSheet("background-color: transparent; color: gray; border: 1px dashed #777; border-radius: 3px;");
        m_btn_fill_color->setText(tr("Transparent"));
    } else {
        m_btn_fill_color->setStyleSheet(
            QString("background-color: %1; color: %2; font-weight: bold; border-radius: 3px; border: 1px solid #555;")
                .arg(fc.name(QColor::HexRgb))
                .arg(fc.lightness() > 128 ? "#000000" : "#ffffff"));
        m_btn_fill_color->setText(fc.name(QColor::HexRgb).toUpper());
    }
}

void VectorLayerPanel::onAddLayerClicked() {
    emit openVectorRequested();
}

void VectorLayerPanel::onRemoveLayerClicked() {
    if (!m_mgr || !m_current_layer) return;
    for (int i = 0; i < m_mgr->count(); ++i) {
        if (m_mgr->layerAt(i).get() == m_current_layer) {
            m_mgr->removeLayer(i);
            break;
        }
    }
}

void VectorLayerPanel::onFitToLayerClicked() {
    if (!m_mgr || !m_current_layer) return;
    for (int i = 0; i < m_mgr->count(); ++i) {
        if (m_mgr->layerAt(i).get() == m_current_layer) {
            emit fitToLayerRequested(i);
            break;
        }
    }
}

void VectorLayerPanel::onStrokeColorClicked() {
    if (!m_current_layer) return;
    QColor c = QColorDialog::getColor(m_current_layer->strokeColor(), this, tr("Select Stroke / Outline Color"));
    if (c.isValid()) {
        m_current_layer->setStrokeColor(c);
        updateColorButtons();
        emit layerStyleChanged(m_current_layer);
        if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
    }
}

void VectorLayerPanel::onFillColorClicked() {
    if (!m_current_layer) return;
    QColor initial = m_current_layer->fillColor();
    if (initial.alpha() == 0) initial = QColor(220, 20, 20, 100);
    QColor c = QColorDialog::getColor(initial, this, tr("Select Fill Color"), QColorDialog::ShowAlphaChannel);
    if (c.isValid()) {
        m_current_layer->setFillColor(c);
        m_chk_transparent_fill->setChecked(c.alpha() == 0);
        updateUiFromLayer();
        emit layerStyleChanged(m_current_layer);
        if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
    }
}

void VectorLayerPanel::onStrokeWidthChanged(double val) {
    if (m_updating || !m_current_layer) return;
    m_current_layer->setStrokeWidth(static_cast<float>(val));
    emit layerStyleChanged(m_current_layer);
    if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
}

void VectorLayerPanel::onStrokeStyleChanged(int index) {
    if (m_updating || !m_current_layer || index < 0) return;
    Qt::PenStyle style = static_cast<Qt::PenStyle>(m_combo_stroke_style->currentData().toInt());
    m_current_layer->setStrokeStyle(style);
    emit layerStyleChanged(m_current_layer);
    if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
}

void VectorLayerPanel::onTransparentFillToggled(bool checked) {
    if (m_updating || !m_current_layer) return;
    if (checked) {
        QColor c = m_current_layer->fillColor();
        c.setAlpha(0);
        m_current_layer->setFillColor(c);
    } else {
        QColor c = m_current_layer->fillColor();
        if (c.alpha() == 0) {
            c = m_current_layer->strokeColor();
            c.setAlpha(80);
        }
        m_current_layer->setFillColor(c);
    }
    updateUiFromLayer();
    emit layerStyleChanged(m_current_layer);
    if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
}

void VectorLayerPanel::onFillOpacityChanged(int val) {
    if (m_updating || !m_current_layer) return;
    QColor c = m_current_layer->fillColor();
    c.setAlphaF(val / 100.0f);
    m_current_layer->setFillColor(c);
    m_lbl_fill_opacity->setText(QString("%1%").arg(val));
    if (val == 0 && !m_chk_transparent_fill->isChecked()) {
        m_chk_transparent_fill->setChecked(true);
    } else if (val > 0 && m_chk_transparent_fill->isChecked()) {
        m_chk_transparent_fill->setChecked(false);
    }
    updateColorButtons();
    emit layerStyleChanged(m_current_layer);
    if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
}

void VectorLayerPanel::onPointSizeChanged(int val) {
    if (m_updating || !m_current_layer) return;
    m_current_layer->setPointSize(static_cast<float>(val));
    emit layerStyleChanged(m_current_layer);
    if (m_mgr) m_mgr->notifyLayerChanged(m_mgr->activeIndex());
}
