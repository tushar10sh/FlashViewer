#include "panels/MtfToolPanel.hpp"
#include <QApplication>
#include <QClipboard>
#include <QTextStream>
#include <cmath>
#include <algorithm>
#include <numeric>

MtfToolPanel::MtfToolPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void MtfToolPanel::setupUi() {
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
    m_size_combo->setCurrentIndex(3); // 11x11 default

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
    m_info_label = new QLabel(tr("Click on an image region to calculate MTF."), this);
    m_info_label->setWordWrap(true);
    m_info_label->setStyleSheet("font-weight: bold; color: #2196F3;");
    mainLayout->addWidget(m_info_label);

    // Chart Setup
    m_chart = new QChart();
    m_chart->setTitle(tr("Modulation Transfer Function (MTF)"));
    m_chart->setAnimationOptions(QChart::NoAnimation);

    m_series_h = new QLineSeries();
    m_series_h->setName(tr("Horizontal MTF"));
    QPen penH(QColor(33, 150, 243), 2.5);
    m_series_h->setPen(penH);

    m_series_v = new QLineSeries();
    m_series_v->setName(tr("Vertical MTF"));
    QPen penV(QColor(255, 87, 34), 2.5);
    m_series_v->setPen(penV);

    m_chart->addSeries(m_series_h);
    m_chart->addSeries(m_series_v);

    m_axis_x = new QValueAxis();
    m_axis_x->setTitleText(tr("Spatial Frequency (cycles/pixel)"));
    m_axis_x->setRange(0.0, 0.5);
    m_axis_x->setTickCount(6);

    m_axis_y = new QValueAxis();
    m_axis_y->setTitleText(tr("MTF (Response)"));
    m_axis_y->setRange(0.0, 1.05);
    m_axis_y->setTickCount(6);

    m_chart->addAxis(m_axis_x, Qt::AlignBottom);
    m_chart->addAxis(m_axis_y, Qt::AlignLeft);

    m_series_h->attachAxis(m_axis_x);
    m_series_h->attachAxis(m_axis_y);
    m_series_v->attachAxis(m_axis_x);
    m_series_v->attachAxis(m_axis_y);

    m_chart_view = new FvChartView(m_chart, this);
    m_chart_view->setRenderHint(QPainter::Antialiasing);
    m_chart_view->setMinimumHeight(220);
    mainLayout->addWidget(m_chart_view);

    // Stats GroupBox
    auto* statsGroup = new QGroupBox(tr("MTF Summary Statistics"), this);
    auto* statsLayout = new QHBoxLayout(statsGroup);

    m_lbl_h_mtf50 = new QLabel(tr("H MTF₅₀: --"), this);
    m_lbl_v_mtf50 = new QLabel(tr("V MTF₅₀: --"), this);
    m_lbl_h_nyq = new QLabel(tr("H Nyquist: --"), this);
    m_lbl_v_nyq = new QLabel(tr("V Nyquist: --"), this);

    statsLayout->addWidget(m_lbl_h_mtf50);
    statsLayout->addWidget(m_lbl_v_mtf50);
    statsLayout->addWidget(m_lbl_h_nyq);
    statsLayout->addWidget(m_lbl_v_nyq);
    mainLayout->addWidget(statsGroup);

    setWindowTitle(tr("MTF Analysis Tool"));
    resize(620, 480);
}

void MtfToolPanel::setWindowSize(int sz) {
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

// Compute 1D DFT magnitude array and normalize
static std::vector<QPointF> computeDftMtf(const std::vector<double>& signal, double& mtf50Out, double& nyquistOut) {
    std::vector<QPointF> points;
    int N = static_cast<int>(signal.size());
    if (N < 2) return points;

    // Calculate Line Spread Function (LSF): finite difference derivative |s[i+1] - s[i]|
    std::vector<double> lsf;
    lsf.reserve(N);
    double meanSig = std::accumulate(signal.begin(), signal.end(), 0.0) / N;

    // Compute derivative (or s[i] - meanSig if uniform edge/profile)
    for (int i = 0; i < N - 1; ++i) {
        lsf.push_back(std::abs(signal[i + 1] - signal[i]));
    }
    // Handle constant/smooth region fallback
    double sumLsf = std::accumulate(lsf.begin(), lsf.end(), 0.0);
    if (sumLsf < 1e-12) {
        // Fallback to demeaned absolute values
        lsf.clear();
        for (int i = 0; i < N; ++i) {
            lsf.push_back(std::abs(signal[i] - meanSig));
        }
        sumLsf = std::accumulate(lsf.begin(), lsf.end(), 0.0);
    }

    int M = static_cast<int>(lsf.size());
    if (M < 1) return points;

    int numSteps = 50;
    double maxMag = 0.0;
    std::vector<double> mags;
    mags.reserve(numSteps + 1);

    for (int step = 0; step <= numSteps; ++step) {
        double f = (0.5 * step) / numSteps; // freq 0.0 to 0.5
        double re = 0.0, im = 0.0;
        for (int n = 0; n < M; ++n) {
            double angle = 2.0 * M_PI * f * n;
            re += lsf[n] * std::cos(angle);
            im -= lsf[n] * std::sin(angle);
        }
        double mag = std::sqrt(re * re + im * im);
        mags.push_back(mag);
        if (step == 0) maxMag = mag;
    }

    if (maxMag < 1e-12) maxMag = 1.0;

    mtf50Out = -1.0;
    nyquistOut = mags.back() / maxMag;

    for (int step = 0; step <= numSteps; ++step) {
        double f = (0.5 * step) / numSteps;
        double normMtf = mags[step] / maxMag;
        points.emplace_back(f, normMtf);

        if (mtf50Out < 0.0 && normMtf <= 0.5 && step > 0) {
            double prevF = (0.5 * (step - 1)) / numSteps;
            double prevM = mags[step - 1] / maxMag;
            // Linear interpolation for MTF50
            if (std::abs(prevM - normMtf) > 1e-6) {
                mtf50Out = prevF + (0.5 - prevM) * (f - prevF) / (normMtf - prevM);
            } else {
                mtf50Out = f;
            }
        }
    }

    if (mtf50Out < 0.0) mtf50Out = 0.5; // if MTF stays above 0.5 throughout range

    return points;
}

MtfToolPanel::MtfResult MtfToolPanel::computeMtf(RasterDataset* ds, int col, int row, int winSize) {
    MtfResult res;
    if (!ds || winSize < 1) return res;

    int half = winSize / 2;
    int rx0 = std::clamp(col - half, 0, ds->width() - 1);
    int ry0 = std::clamp(row - half, 0, ds->height() - 1);
    int rx1 = std::clamp(col + half, 0, ds->width() - 1);
    int ry1 = std::clamp(row + half, 0, ds->height() - 1);
    int rw = rx1 - rx0 + 1;
    int rh = ry1 - ry0 + 1;

    if (rw <= 0 || rh <= 0) return res;

    TileBuffer buf = ds->readRegion(rx0, ry0, rw, rh, rw, rh, {1});
    if (!buf.isValid()) return res;

    const float* pixels = buf.bandPtr(0);
    auto ndInfo = ds->noData(1);
    bool hasNd = ndInfo.has_value;
    double ndVal = ndInfo.value;
    double eps = std::max(std::abs(ndVal) * 1e-5, 1e-10);

    // Compute mean of valid pixels for missing value substitution
    double validSum = 0.0;
    int validCnt = 0;
    for (int i = 0; i < rw * rh; ++i) {
        float val = pixels[i];
        if (!std::isnan(val) && !std::isinf(val) && (!hasNd || std::abs(val - ndVal) >= eps)) {
            validSum += val;
            validCnt++;
        }
    }
    double fillVal = (validCnt > 0) ? (validSum / validCnt) : 0.0;

    std::vector<std::vector<double>> img(rh, std::vector<double>(rw, fillVal));
    for (int r = 0; r < rh; ++r) {
        for (int c = 0; c < rw; ++c) {
            float val = pixels[r * rw + c];
            if (!std::isnan(val) && !std::isinf(val) && (!hasNd || std::abs(val - ndVal) >= eps)) {
                img[r][c] = val;
            }
        }
    }

    // Horizontal profile: mean across rows for each column c
    std::vector<double> profH(rw, 0.0);
    for (int c = 0; c < rw; ++c) {
        double s = 0.0;
        for (int r = 0; r < rh; ++r) s += img[r][c];
        profH[c] = s / rh;
    }

    // Vertical profile: mean across columns for each row r
    std::vector<double> profV(rh, 0.0);
    for (int r = 0; r < rh; ++r) {
        double s = 0.0;
        for (int c = 0; c < rw; ++c) s += img[r][c];
        profV[r] = s / rw;
    }

    res.horizMtf = computeDftMtf(profH, res.horizMtf50, res.horizNyquist);
    res.vertMtf = computeDftMtf(profV, res.vertMtf50, res.vertNyquist);
    res.valid = !res.horizMtf.empty() && !res.vertMtf.empty();

    return res;
}

void MtfToolPanel::calculateAndShow(RasterLayer* layer, int col, int row, double geoX, double geoY, const QString& crsWkt) {
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

    m_last_result = computeMtf(ds, col, row, m_window_size);
    updatePlot(m_last_result);
}

void MtfToolPanel::updatePlot(const MtfResult& res) {
    m_series_h->clear();
    m_series_v->clear();

    if (!res.valid) {
        m_lbl_h_mtf50->setText(tr("H MTF₅₀: --"));
        m_lbl_v_mtf50->setText(tr("V MTF₅₀: --"));
        m_lbl_h_nyq->setText(tr("H Nyquist: --"));
        m_lbl_v_nyq->setText(tr("V Nyquist: --"));
        return;
    }

    for (const auto& pt : res.horizMtf) {
        m_series_h->append(pt);
    }
    for (const auto& pt : res.vertMtf) {
        m_series_v->append(pt);
    }

    m_lbl_h_mtf50->setText(tr("H MTF₅₀: %1 cyc/px").arg(res.horizMtf50, 0, 'f', 3));
    m_lbl_v_mtf50->setText(tr("V MTF₅₀: %1 cyc/px").arg(res.vertMtf50, 0, 'f', 3));
    m_lbl_h_nyq->setText(tr("H Nyquist: %1").arg(res.horizNyquist, 0, 'f', 3));
    m_lbl_v_nyq->setText(tr("V Nyquist: %1").arg(res.vertNyquist, 0, 'f', 3));
}

void MtfToolPanel::clearResults() {
    m_series_h->clear();
    m_series_v->clear();
    m_last_result = MtfResult();
    m_has_last_calc = false;
    m_last_layer = nullptr;
    m_info_label->setText(tr("Click on an image region to calculate MTF."));
    m_lbl_h_mtf50->setText(tr("H MTF₅₀: --"));
    m_lbl_v_mtf50->setText(tr("V MTF₅₀: --"));
    m_lbl_h_nyq->setText(tr("H Nyquist: --"));
    m_lbl_v_nyq->setText(tr("V Nyquist: --"));
}
