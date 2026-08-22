#pragma once
#include <QWidget>
#include <QString>
#include <memory>
#include <optional>

#include "live/LiveGeorefSession.hpp"   // LiveConfigUpdate is a value member below, not just a pointer

class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QSlider;
class QProgressBar;
class QPushButton;
class QLabel;
class QTimer;

class RpyControlPanel : public QWidget {
    Q_OBJECT
public:
    explicit RpyControlPanel(QWidget* parent = nullptr);
    ~RpyControlPanel() override = default;

    // Panel is inert (controls disabled) until a session is attached.
    void setSession(std::shared_ptr<LiveGeorefSession> session);

    QString overviewResampleMethod() const;
    bool isLiveMode() const;
    void setControlsLocked(bool locked);

    // Initializes every control from a session's CURRENT effective config
    // (see SceneInfo::currentConfig / LiveGeorefSession's scene_info
    // handling) -- e.g. after (re)connecting to a server a previous client
    // had already configured. Blocks each widget's own signals while
    // setting values so this never itself triggers a config_update send
    // (that would just redundantly re-tell the server what it already told
    // us). `stride` is also applied, but note the server may have
    // coarsened it for a `preview=true` in-flight update -- see
    // LiveConfigUpdate::preview's doc comment -- so this can show a
    // temporarily-inflated stride value until the next real (non-preview)
    // update settles it back down.
    void applyRemoteConfig(const LiveConfigUpdate& cfg);

signals:
    void configChanged();   // emitted whenever a preview or full update is sent
    void displayResamplingChanged(QString method);

private slots:
    void onAnyControlChanged();
    void onDebounceTimeout();
    void onLiveModeToggled(bool live);
    void onProcessClicked();
    void onProgressUpdated(int current, int total, QString stage, double etaS, int generation);
    void onSessionFinished(int generation);

private:
    void setupUi();
    void sendUpdate(bool preview);

    std::shared_ptr<LiveGeorefSession> m_session;
    QTimer* m_debounce{nullptr};
    // The last LiveConfigUpdate actually sent to m_session (see sendUpdate()'s
    // no-op guard) -- unset (nullopt) until the first real send of a new session.
    std::optional<LiveConfigUpdate> m_last_sent;

    QCheckBox* m_live_mode_check{nullptr};
    QPushButton* m_process_btn{nullptr};

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
