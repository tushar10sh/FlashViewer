#include "panels/SnrToolPanel.hpp"
#include <QHeaderView>
#include <QApplication>
#include <QClipboard>
#include <QTextStream>
#include <cmath>
#include <algorithm>
#include <numeric>

SnrToolPanel::SnrToolPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void SnrToolPanel::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // Top control bar
    auto* topLayout = new QHBoxLayout();
    auto* labelSize = new QLabel(tr("Window Size:"), this);
    m_size_combo = new QComboBox(this);
    m_size_combo->addItem(tr("3 x 3"), 3);
    m_size_combo->addItem(tr("5 x 5"), 5);
    m_size_combo->addItem(tr("7 x 7"), 7);
    m_size_combo->addItem(tr("11 x 11"), 11);
    m_size_combo->addItem(tr("15 x 15"), 15);
    m_size_combo->addItem(tr("21 x 21"), 21);
    m_size_combo->addItem(tr("31 x 31"), 31);
    m_size_combo->addItem(tr("51 x 51"), 51);
    m_size_combo->setCurrentIndex(1); // 5x5 default

    connect(m_size_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        int sz = m_size_combo->itemData(idx).toInt();
        if (sz != m_window_size) {
            m_window_size = sz;
            emit windowSizeChanged(m_window_size);
            if (m_has_last_calc && m_last_layer) {
                calculateAndShow(m_last_layer, m_last_col, m_last_row, m_last_geo_x, m_last_geo_y, m_last_crs_wkt);
            }
        }
    });

    topLayout->addWidget(labelSize);
    topLayout->addWidget(m_size_combo);
    topLayout->addStretch();

    mainLayout->addLayout(topLayout);

    // Info Label
    m_info_label = new QLabel(tr("Click on an image region to calculate SNR."), this);
    m_info_label->setWordWrap(true);
    m_info_label->setStyleSheet("font-weight: bold; color: #4CAF50;");
    mainLayout->addWidget(m_info_label);

    // Results Table
    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    QStringList headers = {
        tr("Band"), tr("Mean (μ)"), tr("Std Dev (σ)"),
        tr("Min"), tr("Max"), tr("SNR (Linear)"), tr("SNR (dB)")
    };
    m_table->setHorizontalHeaderLabels(headers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    mainLayout->addWidget(m_table);

    // Bottom Action buttons
    auto* btnLayout = new QHBoxLayout();
    m_btn_copy = new QPushButton(tr("Copy Results"), this);
    m_btn_clear = new QPushButton(tr("Clear"), this);

    connect(m_btn_copy, &QPushButton::clicked, this, &SnrToolPanel::copyToClipboard);
    connect(m_btn_clear, &QPushButton::clicked, this, &SnrToolPanel::clearResults);

    btnLayout->addWidget(m_btn_copy);
    btnLayout->addWidget(m_btn_clear);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    setWindowTitle(tr("SNR Analysis Tool"));
    resize(600, 350);
}

void SnrToolPanel::setWindowSize(int sz) {
    if (sz < 3) sz = 3;
    if (sz % 2 == 0) sz += 1;
    m_window_size = sz;
    for (int i = 0; i < m_size_combo->count(); ++i) {
        if (m_size_combo->itemData(i).toInt() == sz) {
            m_size_combo->setCurrentIndex(i);
            break;
        }
    }
}

std::vector<SnrToolPanel::SnrBandResult> SnrToolPanel::computeSnr(RasterDataset* ds, int col, int row, int winSize) {
    std::vector<SnrBandResult> results;
    if (!ds || winSize < 1) return results;

    int half = winSize / 2;
    int rx0 = std::clamp(col - half, 0, ds->width() - 1);
    int ry0 = std::clamp(row - half, 0, ds->height() - 1);
    int rx1 = std::clamp(col + half, 0, ds->width() - 1);
    int ry1 = std::clamp(row + half, 0, ds->height() - 1);
    int rw = rx1 - rx0 + 1;
    int rh = ry1 - ry0 + 1;

    if (rw <= 0 || rh <= 0) return results;

    int totalBands = ds->bandCount();
    for (int b = 1; b <= totalBands; ++b) {
        TileBuffer buf = ds->readRegion(rx0, ry0, rw, rh, rw, rh, {b});
        if (!buf.isValid()) continue;

        const float* pixels = buf.bandPtr(0);
        int totalPix = rw * rh;
        auto ndInfo = ds->noData(b);
        bool hasNd = ndInfo.has_value;
        double ndVal = ndInfo.value;
        double eps = std::max(std::abs(ndVal) * 1e-5, 1e-10);

        std::vector<double> validValues;
        validValues.reserve(totalPix);

        for (int i = 0; i < totalPix; ++i) {
            float val = pixels[i];
            if (std::isnan(val) || std::isinf(val)) continue;
            if (hasNd && std::abs(val - ndVal) < eps) continue;
            validValues.push_back(static_cast<double>(val));
        }

        SnrBandResult res;
        res.bandIndex = b;
        res.bandName = QString(tr("Band %1")).arg(b);
        res.validPixelCount = static_cast<int>(validValues.size());

        if (validValues.empty()) {
            res.mean = 0.0;
            res.stdDev = 0.0;
            res.minVal = 0.0;
            res.maxVal = 0.0;
            res.snrLinear = 0.0;
            res.snrDb = 0.0;
        } else {
            double sum = std::accumulate(validValues.begin(), validValues.end(), 0.0);
            double mean = sum / validValues.size();
            double sqSum = 0.0;
            double minV = validValues[0];
            double maxV = validValues[0];

            for (double v : validValues) {
                sqSum += (v - mean) * (v - mean);
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
            }

            double variance = (validValues.size() > 1) ? (sqSum / (validValues.size() - 1)) : 0.0;
            double stdDev = std::sqrt(variance);
            double snrLin = (stdDev > 1e-12) ? (mean / stdDev) : 0.0;
            double snrDb = (snrLin > 1e-12) ? (20.0 * std::log10(snrLin)) : 0.0;

            res.mean = mean;
            res.stdDev = stdDev;
            res.minVal = minV;
            res.maxVal = maxV;
            res.snrLinear = snrLin;
            res.snrDb = snrDb;
        }
        results.push_back(res);
    }
    return results;
}

void SnrToolPanel::calculateAndShow(RasterLayer* layer, int col, int row, double geoX, double geoY, const QString& crsWkt) {
    (void)crsWkt;
    if (!layer || !layer->dataset()) {
        clearResults();
        m_info_label->setText(tr("No active raster layer selected."));
        return;
    }

    m_last_layer = layer;
    m_last_col = col;
    m_last_row = row;
    m_last_geo_x = geoX;
    m_last_geo_y = geoY;
    m_last_crs_wkt = crsWkt;
    m_has_last_calc = true;

    auto* ds = layer->dataset();
    m_info_label->setText(tr("Layer: %1 | Location: Col %2, Row %3 | Geo: (%4, %5) [%6x%6 window]")
        .arg(layer->name())
        .arg(col).arg(row)
        .arg(geoX, 0, 'f', 4)
        .arg(geoY, 0, 'f', 4)
        .arg(m_window_size));

    m_last_results = computeSnr(ds, col, row, m_window_size);
    updateTable(m_last_results);
}

void SnrToolPanel::updateTable(const std::vector<SnrBandResult>& results) {
    m_table->setRowCount(0);
    int row = 0;
    for (const auto& r : results) {
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(r.bandName));
        m_table->setItem(row, 1, new QTableWidgetItem(QString::number(r.mean, 'f', 4)));
        m_table->setItem(row, 2, new QTableWidgetItem(QString::number(r.stdDev, 'f', 4)));
        m_table->setItem(row, 3, new QTableWidgetItem(QString::number(r.minVal, 'f', 4)));
        m_table->setItem(row, 4, new QTableWidgetItem(QString::number(r.maxVal, 'f', 4)));
        m_table->setItem(row, 5, new QTableWidgetItem(QString::number(r.snrLinear, 'f', 2)));
        m_table->setItem(row, 6, new QTableWidgetItem(QString::number(r.snrDb, 'f', 2)));
        row++;
    }
}

void SnrToolPanel::clearResults() {
    m_table->setRowCount(0);
    m_last_results.clear();
    m_has_last_calc = false;
    m_last_layer = nullptr;
    m_info_label->setText(tr("Click on an image region to calculate SNR."));
}

void SnrToolPanel::copyToClipboard() {
    if (m_last_results.empty()) return;
    QString text;
    QTextStream out(&text);
    out << "Band\tMean\tStdDev\tMin\tMax\tSNR(Linear)\tSNR(dB)\n";
    for (const auto& r : m_last_results) {
        out << r.bandName << "\t"
            << r.mean << "\t"
            << r.stdDev << "\t"
            << r.minVal << "\t"
            << r.maxVal << "\t"
            << r.snrLinear << "\t"
            << r.snrDb << "\n";
    }
    QApplication::clipboard()->setText(text);
}
