#include "panels/RpyControlPanel.hpp"
#include "live/LiveGeorefSession.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QProgressBar>
#include <QLabel>
#include <QTimer>

namespace {
constexpr int kDebounceMs = 300;
constexpr double kDeg2Rad = 0.017453292519943295;
}

RpyControlPanel::RpyControlPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void RpyControlPanel::setupUi() {
    setWindowTitle(tr("Live Georeferencing Control"));

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &RpyControlPanel::onDebounceTimeout);

    auto* root = new QVBoxLayout(this);

    // --- Attitude Correction ------------------------------------------------
    auto* attGroup = new QGroupBox(tr("Attitude Correction (RPY)"), this);
    auto* attForm = new QFormLayout(attGroup);

    auto makeBiasSpin = [this] {
        auto* sb = new QDoubleSpinBox(this);
        sb->setRange(-5.0, 5.0);
        sb->setDecimals(4);
        sb->setSingleStep(0.001);
        sb->setSuffix(tr(" deg"));
        connect(sb, &QDoubleSpinBox::valueChanged, this, &RpyControlPanel::onAnyControlChanged);
        return sb;
    };
    m_roll_bias_deg = makeBiasSpin();
    m_pitch_bias_deg = makeBiasSpin();
    m_yaw_bias_deg = makeBiasSpin();
    attForm->addRow(tr("Roll bias"), m_roll_bias_deg);
    attForm->addRow(tr("Pitch bias"), m_pitch_bias_deg);
    attForm->addRow(tr("Yaw bias"), m_yaw_bias_deg);

    // deg/s, not rad/s -- unified with the bias fields above (both degree-based
    // on screen); converted to rad/s in sendUpdate() same as bias is converted
    // to rad, since LiveConfigUpdate/GeoreferencerConfig.initial_rpy_rate_rad_per_s
    // are radian-based on the wire either way. Range/step sized for typical
    // real drift terms (order 1e-4 rad/s ~= 0.006 deg/s, per satellites/liss3
    // attitude bias fits) while still allowing a much larger nudge.
    auto makeRateSpin = [this] {
        auto* sb = new QDoubleSpinBox(this);
        sb->setRange(-5.0, 5.0);
        sb->setDecimals(6);
        sb->setSingleStep(0.0001);
        sb->setSuffix(tr(" deg/s"));
        connect(sb, &QDoubleSpinBox::valueChanged, this, &RpyControlPanel::onAnyControlChanged);
        return sb;
    };
    m_roll_rate_deg = makeRateSpin();
    m_pitch_rate_deg = makeRateSpin();
    m_yaw_rate_deg = makeRateSpin();
    attForm->addRow(tr("Roll rate"), m_roll_rate_deg);
    attForm->addRow(tr("Pitch rate"), m_pitch_rate_deg);
    attForm->addRow(tr("Yaw rate"), m_yaw_rate_deg);

    m_t_ref = new QDoubleSpinBox(this);
    m_t_ref->setRange(-1e9, 1e9);
    m_t_ref->setDecimals(3);
    m_t_ref->setSuffix(tr(" s (J2000)"));
    connect(m_t_ref, &QDoubleSpinBox::valueChanged, this, &RpyControlPanel::onAnyControlChanged);
    attForm->addRow(tr("Reference epoch"), m_t_ref);

    m_frame = new QComboBox(this);
    m_frame->addItems({QStringLiteral("hill"), QStringLiteral("body")});
    connect(m_frame, &QComboBox::currentTextChanged, this, &RpyControlPanel::onAnyControlChanged);
    attForm->addRow(tr("Correction frame"), m_frame);

    root->addWidget(attGroup);

    // --- Georeferencing -------------------------------------------------------
    auto* geoGroup = new QGroupBox(tr("Georeferencing"), this);
    auto* geoForm = new QFormLayout(geoGroup);

    auto* strideRow = new QWidget(this);
    auto* strideLayout = new QHBoxLayout(strideRow);
    strideLayout->setContentsMargins(0, 0, 0, 0);
    m_stride = new QSlider(Qt::Horizontal, this);
    m_stride->setRange(2, 128);
    m_stride->setValue(32);
    m_stride_label = new QLabel(QStringLiteral("32"), this);
    connect(m_stride, &QSlider::valueChanged, this, [this](int v) {
        m_stride_label->setText(QString::number(v));
        onAnyControlChanged();
    });
    strideLayout->addWidget(m_stride);
    strideLayout->addWidget(m_stride_label);
    geoForm->addRow(tr("Stride"), strideRow);

    m_cell_locate_method = new QComboBox(this);
    // Full set from BicubicGridInterpolant::_local_cell_guess's dispatch
    // (trims/grid/bicubic_interpolant.py) -- GeoreferencerConfig.cell_locate_method's
    // own inline comment only lists the first three; poly_newton/poly_global/
    // the stencil-walk and hash-grid variants are real, later-added methods
    // that comment was never updated for.
    m_cell_locate_method->addItems({
        QStringLiteral("affine_index"),
        QStringLiteral("grid_walk"),
        QStringLiteral("bilinear_global"),
        QStringLiteral("stencil_walk"),
        QStringLiteral("stencil_walk_affine"),
        QStringLiteral("hash_grid"),
        QStringLiteral("poly_newton"),
        QStringLiteral("poly_global"),
    });
    connect(m_cell_locate_method, &QComboBox::currentTextChanged, this, &RpyControlPanel::onAnyControlChanged);
    geoForm->addRow(tr("Cell locate method"), m_cell_locate_method);

    m_use_local_cell_guess = new QCheckBox(tr("Use local cell guess"), this);
    connect(m_use_local_cell_guess, &QCheckBox::toggled, this, &RpyControlPanel::onAnyControlChanged);
    geoForm->addRow(m_use_local_cell_guess);

    m_resample_mode = new QComboBox(this);
    m_resample_mode->addItems({QStringLiteral("bilinear"), QStringLiteral("bicubic"),
                                QStringLiteral("blackman_sinc")});
    m_resample_mode->setCurrentText(QStringLiteral("bicubic"));
    connect(m_resample_mode, &QComboBox::currentTextChanged, this, &RpyControlPanel::onAnyControlChanged);
    geoForm->addRow(tr("Resample mode"), m_resample_mode);

    root->addWidget(geoGroup);

    // --- Compute ----------------------------------------------------------------
    auto* computeGroup = new QGroupBox(tr("Compute"), this);
    auto* computeForm = new QFormLayout(computeGroup);

    m_device = new QComboBox(this);
    m_device->addItems({QStringLiteral("cpu"), QStringLiteral("cuda")});
    connect(m_device, &QComboBox::currentTextChanged, this, &RpyControlPanel::onAnyControlChanged);
    computeForm->addRow(tr("Device"), m_device);

    auto makeDtypeCombo = [this] {
        auto* cb = new QComboBox(this);
        cb->addItems({QStringLiteral("float32"), QStringLiteral("float64")});
        cb->setCurrentText(QStringLiteral("float64"));
        connect(cb, &QComboBox::currentTextChanged, this, &RpyControlPanel::onAnyControlChanged);
        return cb;
    };
    m_dtype_geo = makeDtypeCombo();
    m_dtype_pixel = makeDtypeCombo();
    m_dtype_pixel->setCurrentText(QStringLiteral("float32"));
    m_dtype_maps = makeDtypeCombo();
    computeForm->addRow(tr("Geodetic dtype"), m_dtype_geo);
    computeForm->addRow(tr("Pixel dtype"), m_dtype_pixel);
    computeForm->addRow(tr("Map dtype"), m_dtype_maps);

    root->addWidget(computeGroup);

    // --- Display (client-side only -- never sent to the server) -----------------
    auto* displayGroup = new QGroupBox(tr("Display"), this);
    auto* displayForm = new QFormLayout(displayGroup);

    m_overview_resample = new QComboBox(this);
    m_overview_resample->addItems({QStringLiteral("NEAREST"), QStringLiteral("AVERAGE"),
                                    QStringLiteral("GAUSS"), QStringLiteral("CUBIC"),
                                    QStringLiteral("CUBICSPLINE"), QStringLiteral("LANCZOS"),
                                    QStringLiteral("MODE")});
    // NEAREST default: preserves exact sensor DN in the in-memory pyramid
    // rather than blending neighboring pixels -- matters for real,
    // integer-quantized imagery (e.g. LISS-3's uint16 DN, see
    // ArrowTileWriter's band_dtype) where you want to see the real value,
    // not an averaged one, at zoomed-out overview levels.
    m_overview_resample->setCurrentText(QStringLiteral("NEAREST"));
    displayForm->addRow(tr("Overview resampling"), m_overview_resample);

    m_display_resample = new QComboBox(this);
    m_display_resample->addItems({QStringLiteral("bilinear"), QStringLiteral("bicubic2"),
                                   QStringLiteral("bicubic4")});
    connect(m_display_resample, &QComboBox::currentTextChanged, this,
            &RpyControlPanel::displayResamplingChanged);
    displayForm->addRow(tr("Zoom (GPU) resampling"), m_display_resample);

    root->addWidget(displayGroup);

    // --- Progress ----------------------------------------------------------------
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_status_label = new QLabel(tr("Not connected"), this);
    root->addWidget(m_status_label);
    root->addWidget(m_progress);
    root->addStretch(1);

    setEnabled(false);   // inert until setSession() attaches a live session
}

QString RpyControlPanel::overviewResampleMethod() const {
    return m_overview_resample ? m_overview_resample->currentText() : QStringLiteral("NEAREST");
}

void RpyControlPanel::setSession(std::shared_ptr<LiveGeorefSession> session) {
    m_session = std::move(session);
    setEnabled(m_session != nullptr);
    if (!m_session) return;
    // Qt::QueuedConnection: see LiveRasterDataset::create()'s comment --
    // LiveGeorefSession emits from a background thread whose QObject
    // affinity is still the GUI thread, so AutoConnection would otherwise
    // pick DirectConnection and run these slots off the GUI thread.
    connect(m_session.get(), &LiveGeorefSession::progressUpdated,
            this, &RpyControlPanel::onProgressUpdated, Qt::QueuedConnection);
    connect(m_session.get(), &LiveGeorefSession::finished,
            this, &RpyControlPanel::onSessionFinished, Qt::QueuedConnection);
    m_status_label->setText(tr("Connected"));
}

void RpyControlPanel::onAnyControlChanged() {
    // Immediate coarse preview, then a debounced full-resolution recompute
    // once the user stops adjusting -- the plan's two-tier interactivity
    // design, implemented entirely client-side against one ConfigUpdate
    // message shape (LiveGeorefSession::sendConfigUpdate).
    sendUpdate(/*preview=*/true);
    if (m_debounce) m_debounce->start();   // restarts if already running
}

void RpyControlPanel::onDebounceTimeout() {
    sendUpdate(/*preview=*/false);
}

void RpyControlPanel::sendUpdate(bool preview) {
    if (!m_session) return;
    LiveConfigUpdate update;
    update.rpyBiasRad[0] = m_roll_bias_deg->value() * kDeg2Rad;
    update.rpyBiasRad[1] = m_pitch_bias_deg->value() * kDeg2Rad;
    update.rpyBiasRad[2] = m_yaw_bias_deg->value() * kDeg2Rad;
    update.rpyRateRadPerS[0] = m_roll_rate_deg->value() * kDeg2Rad;
    update.rpyRateRadPerS[1] = m_pitch_rate_deg->value() * kDeg2Rad;
    update.rpyRateRadPerS[2] = m_yaw_rate_deg->value() * kDeg2Rad;
    update.rpyTRefS = m_t_ref->value();
    update.rpyFrame = m_frame->currentText();

    update.stride = m_stride->value();
    update.cellLocateMethod = m_cell_locate_method->currentText();
    update.useLocalCellGuess = m_use_local_cell_guess->isChecked();
    update.resampleMode = m_resample_mode->currentText();

    update.device = m_device->currentText();
    update.dtypeGeo = m_dtype_geo->currentText();
    update.dtypePixel = m_dtype_pixel->currentText();
    update.dtypeMaps = m_dtype_maps->currentText();

    update.preview = preview;
    m_session->sendConfigUpdate(update);
    emit configChanged();
}

void RpyControlPanel::onProgressUpdated(int current, int total, QString stage, double etaS, int /*generation*/) {
    if (total > 0) {
        m_progress->setRange(0, total);
        m_progress->setValue(current);
    }
    QString text = stage;
    if (etaS >= 0.0) text += tr("  (ETA %1s)").arg(etaS, 0, 'f', 1);
    m_status_label->setText(text);
}

void RpyControlPanel::onSessionFinished(int /*generation*/) {
    m_status_label->setText(tr("Up to date"));
    m_progress->setValue(m_progress->maximum());
}
