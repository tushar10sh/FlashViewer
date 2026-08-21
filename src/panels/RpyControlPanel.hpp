#pragma once
#include <QWidget>
#include <QString>
#include <memory>

class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QSlider;
class QProgressBar;
class QLabel;
class QTimer;

class LiveGeorefSession;

// Live control panel for one connected LiveGeorefSession: RPY bias/rate/
// frame plus the practically-tunable GeoreferencerConfig subset (stride,
// cell-seeding method, device, float precision, resample mode) named in the
// plan doc's "UI scope" decision, and a progress bar bound to the session's
// progressUpdated signal. Follows the same floating-tool-window pattern as
// MtfToolPanel/SnrToolPanel (see MainWindow's m_mtf_panel/m_snr_panel) rather
// than a QDockWidget -- created once, shown on demand once a live session
// exists.
//
// Interaction model (two-tier preview/full-resolution, per the plan): every
// control change sends a preview=true ConfigUpdate immediately for fast
// coarse feedback, then restarts a 300ms debounce timer that sends the same
// parameters with preview=false once the user stops adjusting.
class RpyControlPanel : public QWidget {
    Q_OBJECT
public:
    explicit RpyControlPanel(QWidget* parent = nullptr);
    ~RpyControlPanel() override = default;

    // Panel is inert (controls disabled) until a session is attached.
    void setSession(std::shared_ptr<LiveGeorefSession> session);

    // GDAL BuildOverviews algorithm name ("NEAREST" | "AVERAGE" | "GAUSS" |
    // "CUBIC" | "CUBICSPLINE" | "LANCZOS" | "MODE"), queried once by
    // MainWindow::openLiveSession's LiveGeorefSession::finished handler at
    // the moment it calls RasterDataset::buildOverviews() -- purely
    // client-side, never sent to the server, so (unlike the RPY/
    // GeoreferencerConfig fields above) there's no live-rebuild on change:
    // set your preference before the session finishes loading.
    QString overviewResampleMethod() const;

signals:
    void configChanged();   // emitted whenever a preview or full update is sent
    // Purely client-side GL display resampling (RasterLayer::DisplayResampling
    // -- "bilinear" | "bicubic2" | "bicubic4"), unlike overviewResampleMethod()
    // this DOES apply live: MainWindow applies it to the active live layer
    // immediately, no reload/rebuild needed, since it's a texture-sampling
    // filter mode read at paint time, not a resample of stored data. This is
    // "the app's resampling" that governs zoomed-in smoothing once GDAL has
    // already picked the best-matching overview/native-res source for the
    // current zoom (see RasterDataset::buildOverviews()'s doc comment).
    void displayResamplingChanged(QString method);

private slots:
    void onAnyControlChanged();
    void onDebounceTimeout();
    void onProgressUpdated(int current, int total, QString stage, double etaS, int generation);
    void onSessionFinished(int generation);

private:
    void setupUi();
    void sendUpdate(bool preview);

    std::shared_ptr<LiveGeorefSession> m_session;
    QTimer* m_debounce{nullptr};

    QDoubleSpinBox* m_roll_bias_deg{nullptr};
    QDoubleSpinBox* m_pitch_bias_deg{nullptr};
    QDoubleSpinBox* m_yaw_bias_deg{nullptr};
    QDoubleSpinBox* m_roll_rate_deg{nullptr};
    QDoubleSpinBox* m_pitch_rate_deg{nullptr};
    QDoubleSpinBox* m_yaw_rate_deg{nullptr};
    QDoubleSpinBox* m_t_ref{nullptr};
    QComboBox* m_frame{nullptr};

    QSlider* m_stride{nullptr};
    QLabel* m_stride_label{nullptr};
    QComboBox* m_cell_locate_method{nullptr};
    QCheckBox* m_use_local_cell_guess{nullptr};
    QComboBox* m_resample_mode{nullptr};

    QComboBox* m_device{nullptr};
    QComboBox* m_dtype_geo{nullptr};
    QComboBox* m_dtype_pixel{nullptr};
    QComboBox* m_dtype_maps{nullptr};

    QComboBox* m_overview_resample{nullptr};
    QComboBox* m_display_resample{nullptr};

    QProgressBar* m_progress{nullptr};
    QLabel* m_status_label{nullptr};
};
