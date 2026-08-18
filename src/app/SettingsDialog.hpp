#pragma once

#include <QDialog>

class QComboBox;
class QSpinBox;
class QCheckBox;
class QLineEdit;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override = default;

signals:
    void themeChanged();
    void settingsApplied();

private slots:
    void applySettings();

private:
    void setupUi();
    void loadValues();

    QComboBox*  m_theme_combo{nullptr};
    QCheckBox*  m_chk_perf_hud{nullptr};
    QCheckBox*  m_chk_plot_grid{nullptr};
    QCheckBox*  m_chk_legend_coords{nullptr};

    QSpinBox*   m_spin_dump_font_size{nullptr};

    QComboBox*  m_combo_resampling{nullptr};
    QLineEdit*  m_txt_osm_url{nullptr};
};
