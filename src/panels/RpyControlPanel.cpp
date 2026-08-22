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

#include <QPushButton>
#include <QSignalBlocker>

namespace {
constexpr int kDebounceMs = 300;
constexpr double kDeg2Rad = 0.017453292519943295;
constexpr double kRad2Deg = 1.0 / kDeg2Rad;
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

    // --- Execution Mode -------------------------------------------------------
    auto* modeGroup = new QGroupBox(tr("Execution Mode"), this);
    auto* modeLayout = new QHBoxLayout(modeGroup);
    m_live_mode_check = new QCheckBox(tr("Live Updates"), this);
    m_live_mode_check->setChecked(true);
    connect(m_live_mode_check, &QCheckBox::toggled, this, &RpyControlPanel::onLiveModeToggled);

    m_process_btn = new QPushButton(tr("Process / Recompute"), this);
    m_process_btn->setEnabled(false);
    connect(m_process_btn, &QPushButton::clicked, this, &RpyControlPanel::onProcessClicked);

    modeLayout->addWidget(m_live_mode_check);
    modeLayout->addWidget(m_process_btn);
    root->addWidget(modeGroup);

    // --- Attitude Correction ------------------------------------------------
    auto* attGroup = new QGroupBox(tr("Attitude Correction (RPY)"), this);
    auto* attForm = new QFormLayout(attGroup);

    auto makeBiasSpin = [this] {
        auto* sb = new QDoubleSpinBox(this);
        sb->setRange(-5.0, 5.0);
        sb->setDecimals(4);
        sb->setSingleStep(0.001);
        sb->setSuffix(tr(" deg"));
        connect(sb, &QDoubleSpinBox::valueChanged, this, [this](double) { onAnyControlChanged(); });
        return sb;
    };
    m_roll_bias_deg = makeBiasSpin();
    m_pitch_bias_deg = makeBiasSpin();
    m_yaw_bias_deg = makeBiasSpin();
    attForm->addRow(tr("Roll bias"), m_roll_bias_deg);
    attForm->addRow(tr("Pitch bias"), m_pitch_bias_deg);
    attForm->addRow(tr("Yaw bias"), m_yaw_bias_deg);

    auto makeRateSpin = [this] {
        auto* sb = new QDoubleSpinBox(this);
        sb->setRange(-5.0, 5.0);
        sb->setDecimals(6);
        sb->setSingleStep(0.0001);
        sb->setSuffix(tr(" deg/s"));
        connect(sb, &QDoubleSpinBox::valueChanged, this, [this](double) { onAnyControlChanged(); });
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
    connect(m_t_ref, &QDoubleSpinBox::valueChanged, this, [this](double) { onAnyControlChanged(); });
    attForm->addRow(tr("Reference epoch"), m_t_ref);

    m_frame = new QComboBox(this);
    m_frame->addItems({QStringLiteral("hill"), QStringLiteral("body")});
    connect(m_frame, &QComboBox::currentTextChanged, this, [this](const QString&) { onAnyControlChanged(); });
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
    connect(m_cell_locate_method, &QComboBox::currentTextChanged, this, [this](const QString&) { onAnyControlChanged(); });
    geoForm->addRow(tr("Cell locate method"), m_cell_locate_method);

    m_use_local_cell_guess = new QCheckBox(tr("Use local cell guess"), this);
    connect(m_use_local_cell_guess, &QCheckBox::toggled, this, [this](bool) { onAnyControlChanged(); });
    geoForm->addRow(m_use_local_cell_guess);

    m_resample_mode = new QComboBox(this);
    m_resample_mode->addItems({QStringLiteral("bilinear"), QStringLiteral("bicubic"),
                                QStringLiteral("blackman_sinc")});
    m_resample_mode->setCurrentText(QStringLiteral("bicubic"));
    connect(m_resample_mode, &QComboBox::currentTextChanged, this, [this](const QString&) { onAnyControlChanged(); });
    geoForm->addRow(tr("Resample mode"), m_resample_mode);

    root->addWidget(geoGroup);

    // --- Compute ----------------------------------------------------------------
    auto* computeGroup = new QGroupBox(tr("Compute"), this);
    auto* computeForm = new QFormLayout(computeGroup);

    m_device = new QComboBox(this);
    m_device->addItems({QStringLiteral("cpu"), QStringLiteral("cuda")});
    connect(m_device, &QComboBox::currentTextChanged, this, [this](const QString&) { onAnyControlChanged(); });
    computeForm->addRow(tr("Device"), m_device);

    auto makeDtypeCombo = [this] {
        auto* cb = new QComboBox(this);
        cb->addItems({QStringLiteral("float32"), QStringLiteral("float64")});
        cb->setCurrentText(QStringLiteral("float64"));
        connect(cb, &QComboBox::currentTextChanged, this, [this](const QString&) { onAnyControlChanged(); });
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

bool RpyControlPanel::isLiveMode() const {
    return m_live_mode_check ? m_live_mode_check->isChecked() : true;
}

void RpyControlPanel::onLiveModeToggled(bool live) {
    if (m_process_btn) m_process_btn->setEnabled(!live);
    if (live) {
        onAnyControlChanged();
    }
}

void RpyControlPanel::onProcessClicked() {
    // Explicit user request to (re)run now -- bypass sendUpdate()'s no-op
    // guard (unlike onAnyControlChanged()/onDebounceTimeout(), an identical
    // resend here is deliberate, e.g. re-running after a Cancel).
    m_last_sent.reset();
    sendUpdate(/*preview=*/false);
}

void RpyControlPanel::setControlsLocked(bool locked) {
    if (m_roll_bias_deg) m_roll_bias_deg->setEnabled(!locked);
    if (m_pitch_bias_deg) m_pitch_bias_deg->setEnabled(!locked);
    if (m_yaw_bias_deg) m_yaw_bias_deg->setEnabled(!locked);
    if (m_roll_rate_deg) m_roll_rate_deg->setEnabled(!locked);
    if (m_pitch_rate_deg) m_pitch_rate_deg->setEnabled(!locked);
    if (m_yaw_rate_deg) m_yaw_rate_deg->setEnabled(!locked);
    if (m_t_ref) m_t_ref->setEnabled(!locked);
    if (m_frame) m_frame->setEnabled(!locked);
    if (m_stride) m_stride->setEnabled(!locked);
    if (m_cell_locate_method) m_cell_locate_method->setEnabled(!locked);
    if (m_use_local_cell_guess) m_use_local_cell_guess->setEnabled(!locked);
    if (m_resample_mode) m_resample_mode->setEnabled(!locked);
    if (m_device) m_device->setEnabled(!locked);
    if (m_dtype_geo) m_dtype_geo->setEnabled(!locked);
    if (m_dtype_pixel) m_dtype_pixel->setEnabled(!locked);
    if (m_dtype_maps) m_dtype_maps->setEnabled(!locked);
    if (!isLiveMode() && m_process_btn) {
        m_process_btn->setEnabled(!locked);
    }
}

QString RpyControlPanel::overviewResampleMethod() const {
    return m_overview_resample ? m_overview_resample->currentText() : QStringLiteral("NEAREST");
}

void RpyControlPanel::setSession(std::shared_ptr<LiveGeorefSession> session) {
    m_session = std::move(session);
    setEnabled(m_session != nullptr);
    m_last_sent.reset();   // a new session has no baseline yet -- see sendUpdate()'s guard
    if (!m_session) return;
    connect(m_session.get(), &LiveGeorefSession::progressUpdated,
            this, &RpyControlPanel::onProgressUpdated, Qt::QueuedConnection);
    connect(m_session.get(), &LiveGeorefSession::finished,
            this, &RpyControlPanel::onSessionFinished, Qt::QueuedConnection);
    m_status_label->setText(tr("Connected"));
}

void RpyControlPanel::applyRemoteConfig(const LiveConfigUpdate& cfg) {
    // Block each widget's OWN signal, not this whole QObject: onAnyControlChanged()
    // is connected per-widget (see setupUi()), so a blanket QSignalBlocker on `this`
    // wouldn't reach signals emitted BY the child widgets themselves.
    const QSignalBlocker b1(m_roll_bias_deg);
    const QSignalBlocker b2(m_pitch_bias_deg);
    const QSignalBlocker b3(m_yaw_bias_deg);
    const QSignalBlocker b4(m_roll_rate_deg);
    const QSignalBlocker b5(m_pitch_rate_deg);
    const QSignalBlocker b6(m_yaw_rate_deg);
    const QSignalBlocker b7(m_t_ref);
    const QSignalBlocker b8(m_frame);
    const QSignalBlocker b9(m_stride);
    const QSignalBlocker b10(m_cell_locate_method);
    const QSignalBlocker b11(m_use_local_cell_guess);
    const QSignalBlocker b12(m_resample_mode);
    const QSignalBlocker b13(m_device);
    const QSignalBlocker b14(m_dtype_geo);
    const QSignalBlocker b15(m_dtype_pixel);
    const QSignalBlocker b16(m_dtype_maps);

    if (m_roll_bias_deg)  m_roll_bias_deg->setValue(cfg.rpyBiasRad[0] * kRad2Deg);
    if (m_pitch_bias_deg) m_pitch_bias_deg->setValue(cfg.rpyBiasRad[1] * kRad2Deg);
    if (m_yaw_bias_deg)   m_yaw_bias_deg->setValue(cfg.rpyBiasRad[2] * kRad2Deg);
    if (m_roll_rate_deg)  m_roll_rate_deg->setValue(cfg.rpyRateRadPerS[0] * kRad2Deg);
    if (m_pitch_rate_deg) m_pitch_rate_deg->setValue(cfg.rpyRateRadPerS[1] * kRad2Deg);
    if (m_yaw_rate_deg)   m_yaw_rate_deg->setValue(cfg.rpyRateRadPerS[2] * kRad2Deg);
    if (m_t_ref)          m_t_ref->setValue(cfg.rpyTRefS);
    if (m_frame)          m_frame->setCurrentText(cfg.rpyFrame);

    if (m_stride) {
        m_stride->setValue(cfg.stride);
        if (m_stride_label) m_stride_label->setText(QString::number(cfg.stride));
    }
    if (m_cell_locate_method)   m_cell_locate_method->setCurrentText(cfg.cellLocateMethod);
    if (m_use_local_cell_guess) m_use_local_cell_guess->setChecked(cfg.useLocalCellGuess);
    if (m_resample_mode)        m_resample_mode->setCurrentText(cfg.resampleMode);
    if (m_device)      m_device->setCurrentText(cfg.device);
    if (m_dtype_geo)   m_dtype_geo->setCurrentText(cfg.dtypeGeo);
    if (m_dtype_pixel) m_dtype_pixel->setCurrentText(cfg.dtypePixel);
    if (m_dtype_maps)  m_dtype_maps->setCurrentText(cfg.dtypeMaps);

    // New baseline for sendUpdate()'s no-op guard, so the next real user
    // interaction is compared against the server's actual current state
    // rather than whatever this panel had before the restore.
    m_last_sent = cfg;
    m_last_sent->preview = false;
}

void RpyControlPanel::onAnyControlChanged() {
    if (!isLiveMode()) return;
    sendUpdate(/*preview=*/true);
    if (m_debounce) m_debounce->start();
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

    // Suppress a byte-for-byte-identical resend: sendConfigUpdate() always
    // bumps the session generation, which aborts and restarts the ENTIRE
    // scene's compute from scratch (see LiveGeorefSession::sendConfigUpdate /
    // trims.live.session.LiveGeorefSession.apply_config_update) -- fine when
    // the user actually changed something, but any spurious extra call to
    // onAnyControlChanged() (a stray Qt signal from the panel, unrelated to
    // an actual value change) would otherwise silently wipe and restart an
    // already-streaming live session for no reason.
    if (m_last_sent && *m_last_sent == update) return;
    m_last_sent = update;

    if (!preview) {
        setControlsLocked(true);
    }
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
    setControlsLocked(false);
    m_status_label->setText(tr("Up to date"));
    m_progress->setValue(m_progress->maximum());
}
