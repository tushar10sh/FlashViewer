#include "panels/NumericDumpPanel.hpp"
#include "app/Application.hpp"
#include "core/LayerManager.hpp"
#include "core/RasterLayer.hpp"
#include "gis/CrsUtil.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidget>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QClipboard>
#include <cmath>

namespace {
enum class ViewChannelMode {
    Composite = 0,
    RedOnly,
    GreenOnly,
    BlueOnly,
    SingleGray
};

QString formatPixelVal(double val) {
    if (std::isnan(val)) return "N/A";
    if (std::floor(val) == val && std::abs(val) < 1e9) {
        return QString::number(static_cast<long long>(val));
    }
    return QString::number(val, 'g', 5);
}
} // namespace

#include "app/Settings.hpp"

class NumericDumpDelegate : public QStyledItemDelegate {
public:
    explicit NumericDumpDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void setSample(const PixelPatchSample* sample, ViewChannelMode mode) {
        m_sample = sample;
        m_mode = mode;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        int r = index.row();
        int c = index.column();
        bool isCenter = (r == 5 && c == 5);

        // Determine dark theme dynamically from palette lightness
        bool isDark = (option.palette.base().color().lightness() < 128) ||
                      (option.palette.window().color().lightness() < 128);

        // 1. Draw Cell Background
        QRect rect = option.rect;
        if (isCenter) {
            // Subtle warm highlight for center selected pixel
            painter->fillRect(rect, isDark ? QColor(80, 55, 15, 230) : QColor(255, 243, 205));
        } else if (option.state & QStyle::State_Selected) {
            painter->fillRect(rect, option.palette.highlight());
        } else {
            painter->fillRect(rect, isDark ? QColor(13, 17, 23) : option.palette.base());
        }

        // Cell Border
        if (isCenter) {
            painter->setPen(QPen(isDark ? QColor(240, 136, 62) : QColor(230, 81, 0), 2)); // Bold amber border for center
            painter->drawRect(rect.adjusted(1, 1, -1, -1));
        } else {
            painter->setPen(QPen(isDark ? QColor(48, 54, 61) : QColor(220, 224, 230), 1));
            painter->drawRect(rect);
        }

        // 2. Draw Numeric Text
        if (!m_sample || !m_sample->valid || m_sample->channels.empty()) {
            painter->restore();
            return;
        }

        QFont font = option.font;
        int fontSize = Settings::instance().numericDumpFontSize();
        font.setPointSize(fontSize);
        if (isCenter) {
            font.setBold(true);
        } else {
            font.setBold(false);
        }
        painter->setFont(font);

        // Theme-aware value colors:
        // Dark theme: light colors (off-white for gray, light bright red, light green, light blue)
        // Light theme: dark colors (dark black for gray, dark red, dark green, dark blue)
        const QColor colNa = isDark ? QColor(139, 148, 158) : QColor(140, 140, 140);
        const QColor colR  = isDark ? QColor(255, 123, 114) : QColor(183, 28, 28);
        const QColor colG  = isDark ? QColor(126, 231, 135) : QColor(27, 94, 32);
        const QColor colB  = isDark ? QColor(121, 192, 255) : QColor(13, 71, 161);
        const QColor colGray = isDark ? QColor(240, 246, 252) : QColor(17, 17, 17);

        if (m_sample->is_rgb && m_sample->channels.size() >= 3) {
            const auto& cellR = m_sample->channels[0][static_cast<size_t>(r)][static_cast<size_t>(c)];
            const auto& cellG = m_sample->channels[1][static_cast<size_t>(r)][static_cast<size_t>(c)];
            const auto& cellB = m_sample->channels[2][static_cast<size_t>(r)][static_cast<size_t>(c)];

            if (!cellR.is_valid && !cellG.is_valid && !cellB.is_valid) {
                painter->setPen(colNa);
                painter->drawText(rect, Qt::AlignCenter, "N/A");
            } else if (m_mode == ViewChannelMode::Composite) {
                // Stack 3 values vertically
                int h3 = rect.height() / 3;
                QRect rR(rect.x() + 1, rect.y(), rect.width() - 2, h3);
                QRect rG(rect.x() + 1, rect.y() + h3, rect.width() - 2, h3);
                QRect rB(rect.x() + 1, rect.y() + 2 * h3, rect.width() - 2, rect.height() - 2 * h3);

                painter->setPen(cellR.is_nodata ? colNa : colR);
                painter->drawText(rR, Qt::AlignCenter, formatPixelVal(cellR.value));

                painter->setPen(cellG.is_nodata ? colNa : colG);
                painter->drawText(rG, Qt::AlignCenter, formatPixelVal(cellG.value));

                painter->setPen(cellB.is_nodata ? colNa : colB);
                painter->drawText(rB, Qt::AlignCenter, formatPixelVal(cellB.value));
            } else if (m_mode == ViewChannelMode::RedOnly) {
                painter->setPen(cellR.is_nodata ? colNa : colR);
                painter->drawText(rect, Qt::AlignCenter, formatPixelVal(cellR.value));
            } else if (m_mode == ViewChannelMode::GreenOnly) {
                painter->setPen(cellG.is_nodata ? colNa : colG);
                painter->drawText(rect, Qt::AlignCenter, formatPixelVal(cellG.value));
            } else if (m_mode == ViewChannelMode::BlueOnly) {
                painter->setPen(cellB.is_nodata ? colNa : colB);
                painter->drawText(rect, Qt::AlignCenter, formatPixelVal(cellB.value));
            }
        } else {
            // Grayscale / single-channel
            const auto& cell = m_sample->channels[0][static_cast<size_t>(r)][static_cast<size_t>(c)];
            if (!cell.is_valid || cell.is_nodata) {
                painter->setPen(colNa);
                painter->drawText(rect, Qt::AlignCenter, "N/A");
            } else {
                painter->setPen(colGray);
                painter->drawText(rect, Qt::AlignCenter, formatPixelVal(cell.value));
            }
        }

        painter->restore();
    }

private:
    const PixelPatchSample* m_sample{nullptr};
    ViewChannelMode         m_mode{ViewChannelMode::Composite};
};

NumericDumpPanel::NumericDumpPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void NumericDumpPanel::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(6, 6, 6, 6);
    mainLay->setSpacing(6);

    // 1. Controls header: Layer selector, Mode selector, Copy button
    auto* topBar = new QHBoxLayout();
    topBar->setSpacing(6);

    auto* layerLbl = new QLabel(tr("Layer:"), this);
    m_layer_combo = new QComboBox(this);
    m_layer_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_layer_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NumericDumpPanel::onLayerChanged);

    auto* modeLbl = new QLabel(tr("View:"), this);
    m_mode_combo = new QComboBox(this);
    m_mode_combo->addItem(tr("RGB Composite"), static_cast<int>(ViewChannelMode::Composite));
    connect(m_mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NumericDumpPanel::onViewModeChanged);

    m_copy_btn = new QPushButton(tr("Copy Matrix"), this);
    m_copy_btn->setToolTip(tr("Copy the 11x11 numeric matrix to clipboard as TSV/CSV"));
    connect(m_copy_btn, &QPushButton::clicked, this, &NumericDumpPanel::copyToClipboard);

    auto* fontLbl = new QLabel(tr("Font:"), this);
    auto* fontSpin = new QSpinBox(this);
    fontSpin->setRange(6, 16);
    fontSpin->setValue(Settings::instance().numericDumpFontSize());
    fontSpin->setSuffix(tr(" pt"));
    fontSpin->setToolTip(tr("Matrix cell font size (configurable in Preferences)"));
    connect(fontSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int pt) {
        Settings::instance().setNumericDumpFontSize(pt);
        m_table->viewport()->update();
    });

    topBar->addWidget(layerLbl);
    topBar->addWidget(m_layer_combo);
    topBar->addWidget(modeLbl);
    topBar->addWidget(m_mode_combo);
    topBar->addWidget(fontLbl);
    topBar->addWidget(fontSpin);
    topBar->addWidget(m_copy_btn);
    mainLay->addLayout(topBar);

    // 2. Center info / coordinates header
    m_info_label = new QLabel(tr("Center Pixel: Col --, Row --"), this);
    m_info_label->setStyleSheet("font-weight: bold; font-size: 12px;");
    m_coord_label = new QLabel(tr("Click on map to inspect 11x11 numeric patch"), this);
    m_coord_label->setStyleSheet("color: #888888; font-size: 11px;");
    mainLay->addWidget(m_info_label);
    mainLay->addWidget(m_coord_label);

    // 3. 11x11 Matrix Table
    m_table = new QTableWidget(11, 11, this);
    m_delegate = new NumericDumpDelegate(m_table);
    m_table->setItemDelegate(m_delegate);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setDefaultSectionSize(45);
    m_table->verticalHeader()->setDefaultSectionSize(35);

    // Initialize empty items
    for (int r = 0; r < 11; ++r) {
        for (int c = 0; c < 11; ++c) {
            auto* item = new QTableWidgetItem();
            item->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(r, c, item);
        }
    }

    connect(m_table, &QTableWidget::cellClicked, this, [this](int r, int c) {
        if (!m_patch_sample.valid) return;
        int absCol = m_patch_sample.start_col + c;
        int absRow = m_patch_sample.start_row + r;
        if (m_patch_sample.is_rgb && m_patch_sample.channels.size() >= 3) {
            double vR = m_patch_sample.channels[0][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
            double vG = m_patch_sample.channels[1][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
            double vB = m_patch_sample.channels[2][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
            m_status_label->setText(tr("Selected: [Col %1, Row %2] -> R=%3  G=%4  B=%5")
                                    .arg(absCol).arg(absRow)
                                    .arg(formatPixelVal(vR))
                                    .arg(formatPixelVal(vG))
                                    .arg(formatPixelVal(vB)));
        } else if (!m_patch_sample.channels.empty()) {
            double v = m_patch_sample.channels[0][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
            m_status_label->setText(tr("Selected: [Col %1, Row %2] -> Value=%3")
                                    .arg(absCol).arg(absRow).arg(formatPixelVal(v)));
        }
    });

    mainLay->addWidget(m_table);

    // 4. Status bar footer
    m_status_label = new QLabel(tr("Center pixel is highlighted in bold [5, 5]."), this);
    m_status_label->setStyleSheet("color: #757575; font-size: 11px;");
    mainLay->addWidget(m_status_label);
}

void NumericDumpPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        if (m_table && m_table->viewport()) {
            m_table->viewport()->update();
        }
    }
}

void NumericDumpPanel::inspectGroups(double geo_x, double geo_y, const std::string& geoWkt,
                                     const QVector<InspectPaneGroup>& groups) {
    m_geo_x = geo_x;
    m_geo_y = geo_y;
    m_geo_wkt = geoWkt;

    m_cached_layers.clear();
    m_layer_combo->blockSignals(true);
    m_layer_combo->clear();

    for (const auto& g : groups) {
        int layerNumInPane = 1;
        for (const auto& l : g.layers) {
            if (l.layer) {
                m_cached_layers.push_back(l);
                QString itemLabel;
                if (!g.paneLabel.isEmpty()) {
                    itemLabel = QString("[%1 #%2] %3").arg(g.paneLabel).arg(layerNumInPane).arg(l.name);
                } else {
                    itemLabel = QString("[#%1] %2").arg(layerNumInPane).arg(l.name);
                }
                m_layer_combo->addItem(itemLabel);
                QString fullPath = l.layer->sourceFilePath();
                if (fullPath.isEmpty()) fullPath = l.name;
                m_layer_combo->setItemData(m_layer_combo->count() - 1, fullPath, Qt::ToolTipRole);
                layerNumInPane++;
            }
        }
    }
    m_layer_combo->blockSignals(false);

    if (m_cached_layers.empty()) {
        clear();
        return;
    }

    if (m_selected_layer_idx >= m_cached_layers.size()) {
        m_selected_layer_idx = 0;
    }
    m_layer_combo->setCurrentIndex(m_selected_layer_idx);
    if (m_selected_layer_idx >= 0 && m_selected_layer_idx < m_cached_layers.size()) {
        auto* rl = m_cached_layers[m_selected_layer_idx].layer;
        if (rl) {
            QString path = rl->sourceFilePath();
            m_layer_combo->setToolTip(path.isEmpty() ? rl->name() : path);
        }
    }

    refresh();
}

void NumericDumpPanel::clear() {
    m_patch_sample = PixelPatchSample{};
    m_info_label->setText(tr("Center Pixel: Col --, Row --"));
    m_coord_label->setText(tr("No raster data inspected"));
    m_status_label->setText(tr("Click on map to inspect pixel values"));
    m_layer_combo->setToolTip(QString{});
    updateTable();
}

void NumericDumpPanel::refresh() {
    if (m_selected_layer_idx < 0 || m_selected_layer_idx >= m_cached_layers.size()) {
        clear();
        return;
    }

    auto* rl = m_cached_layers[m_selected_layer_idx].layer;
    if (!rl) {
        clear();
        return;
    }

    QString path = rl->sourceFilePath();
    m_layer_combo->setToolTip(path.isEmpty() ? rl->name() : path);

    bool ok = fvSamplePixelPatch(rl, m_geo_x, m_geo_y, m_geo_wkt, 11, m_patch_sample);
    if (!ok || !m_patch_sample.valid) {
        clear();
        return;
    }

    // Update coordinates info
    auto fmt = fvFormatCoordinates(m_geo_x, m_geo_y, m_geo_wkt);
    m_info_label->setText(tr("Center Pixel: Col %1, Row %2 (Raster: %3 x %4)")
                          .arg(m_patch_sample.center_col)
                          .arg(m_patch_sample.center_row)
                          .arg(m_patch_sample.raster_w)
                          .arg(m_patch_sample.raster_h));
    m_coord_label->setText(fmt.single_line);

    // Update Mode Combo box items
    m_mode_combo->blockSignals(true);
    int currMode = m_mode_combo->currentData().toInt();
    m_mode_combo->clear();

    if (m_patch_sample.is_rgb) {
        m_mode_combo->addItem(tr("RGB Composite"), static_cast<int>(ViewChannelMode::Composite));
        m_mode_combo->addItem(tr("Red Channel (Band %1)").arg(m_patch_sample.r_band), static_cast<int>(ViewChannelMode::RedOnly));
        m_mode_combo->addItem(tr("Green Channel (Band %2)").arg(m_patch_sample.g_band), static_cast<int>(ViewChannelMode::GreenOnly));
        m_mode_combo->addItem(tr("Blue Channel (Band %3)").arg(m_patch_sample.b_band), static_cast<int>(ViewChannelMode::BlueOnly));

        int idx = m_mode_combo->findData(currMode);
        m_mode_combo->setCurrentIndex(idx >= 0 ? idx : 0);
    } else {
        m_mode_combo->addItem(tr("Grayscale (Band %1)").arg(m_patch_sample.r_band), static_cast<int>(ViewChannelMode::SingleGray));
        m_mode_combo->setCurrentIndex(0);
    }
    m_mode_combo->blockSignals(false);

    updateTable();
}

void NumericDumpPanel::onLayerChanged(int index) {
    if (index >= 0 && index < m_cached_layers.size()) {
        m_selected_layer_idx = index;
        refresh();
    }
}

void NumericDumpPanel::onViewModeChanged(int /*index*/) {
    updateTable();
}

void NumericDumpPanel::updateTable() {
    // Set headers
    if (m_patch_sample.valid) {
        for (int c = 0; c < 11; ++c) {
            int colNum = m_patch_sample.start_col + c;
            m_table->setHorizontalHeaderItem(c, new QTableWidgetItem(QString::number(colNum)));
        }
        for (int r = 0; r < 11; ++r) {
            int rowNum = m_patch_sample.start_row + r;
            m_table->setVerticalHeaderItem(r, new QTableWidgetItem(QString::number(rowNum)));
        }
    } else {
        for (int c = 0; c < 11; ++c) {
            m_table->setHorizontalHeaderItem(c, new QTableWidgetItem(QString("C%1").arg(c - 5)));
        }
        for (int r = 0; r < 11; ++r) {
            m_table->setVerticalHeaderItem(r, new QTableWidgetItem(QString("R%1").arg(r - 5)));
        }
    }

    auto mode = static_cast<ViewChannelMode>(m_mode_combo->currentData().toInt());
    if (m_delegate) {
        m_delegate->setSample(&m_patch_sample, mode);
    }

    // Adjust row height for multi-line RGB composite
    int targetRowHeight = (m_patch_sample.is_rgb && mode == ViewChannelMode::Composite) ? 46 : 28;
    for (int r = 0; r < 11; ++r) {
        m_table->setRowHeight(r, targetRowHeight);
    }

    m_table->viewport()->update();
}

void NumericDumpPanel::copyToClipboard() {
    if (!m_patch_sample.valid || m_patch_sample.channels.empty()) return;

    QString text;
    // Header
    text += "Row/Col\t";
    for (int c = 0; c < 11; ++c) {
        text += QString::number(m_patch_sample.start_col + c) + (c == 10 ? "\n" : "\t");
    }

    auto mode = static_cast<ViewChannelMode>(m_mode_combo->currentData().toInt());

    for (int r = 0; r < 11; ++r) {
        text += QString::number(m_patch_sample.start_row + r) + "\t";
        for (int c = 0; c < 11; ++c) {
            if (m_patch_sample.is_rgb && m_patch_sample.channels.size() >= 3) {
                double rVal = m_patch_sample.channels[0][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
                double gVal = m_patch_sample.channels[1][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
                double bVal = m_patch_sample.channels[2][static_cast<size_t>(r)][static_cast<size_t>(c)].value;

                if (mode == ViewChannelMode::Composite) {
                    text += QString("(%1, %2, %3)").arg(formatPixelVal(rVal), formatPixelVal(gVal), formatPixelVal(bVal));
                } else if (mode == ViewChannelMode::RedOnly) {
                    text += formatPixelVal(rVal);
                } else if (mode == ViewChannelMode::GreenOnly) {
                    text += formatPixelVal(gVal);
                } else if (mode == ViewChannelMode::BlueOnly) {
                    text += formatPixelVal(bVal);
                }
            } else {
                double val = m_patch_sample.channels[0][static_cast<size_t>(r)][static_cast<size_t>(c)].value;
                text += formatPixelVal(val);
            }
            text += (c == 10 ? "\n" : "\t");
        }
    }

    QApplication::clipboard()->setText(text);
    m_status_label->setText(tr("Copied 11x11 matrix to clipboard as TSV table."));
}
