#pragma once
#include <QDialog>
#include <QColor>

class RasterLayer;
class LayerManager;
class MapCanvas;
class QLineEdit;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QTabWidget;

class LayerSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit LayerSettingsDialog(RasterLayer* layer, LayerManager* mgr, MapCanvas* canvas, QWidget* parent = nullptr);
    ~LayerSettingsDialog() override = default;

private slots:
    void applySettings();
    void onAccept();

private:
    void setupUi();
    void loadCurrentSettings();

    RasterLayer*  m_layer{nullptr};
    LayerManager* m_layer_mgr{nullptr};
    MapCanvas*    m_canvas{nullptr};

    QTabWidget*     m_tab_widget{nullptr};

    // General tab controls
    QLineEdit*      m_name_edit{nullptr};
    QSlider*        m_opacity_slider{nullptr};
    QSpinBox*       m_opacity_spin{nullptr};
    QCheckBox*      m_visible_check{nullptr};
    QComboBox*      m_pane_combo{nullptr};

    // Display Filter tab controls
    QComboBox*      m_filter_mode_combo{nullptr};
    QComboBox*      m_filter_size_combo{nullptr};
    QSlider*        m_swipe_x_slider{nullptr};
    QSpinBox*       m_swipe_x_spin{nullptr};
    QSlider*        m_swipe_y_slider{nullptr};
    QSpinBox*       m_swipe_y_spin{nullptr};

    // Band Mapping & Color tab controls
    QComboBox*      m_render_mode_combo{nullptr};
    QComboBox*      m_red_band_combo{nullptr};
    QComboBox*      m_green_band_combo{nullptr};
    QComboBox*      m_blue_band_combo{nullptr};
    QComboBox*      m_colormap_combo{nullptr};
    QCheckBox*      m_invert_colormap_check{nullptr};

    // Contrast & Stretch tab controls
    QDoubleSpinBox* m_stretch_min_spin{nullptr};
    QDoubleSpinBox* m_stretch_max_spin{nullptr};
    QPushButton*    m_stretch_2_98_btn{nullptr};
    QPushButton*    m_stretch_minmax_btn{nullptr};
    QPushButton*    m_stretch_2sigma_btn{nullptr};

    // Resampling & No-Data tab controls
    QComboBox*      m_resample_combo{nullptr};
    QCheckBox*      m_nodata_check{nullptr};
    QDoubleSpinBox* m_nodata_spin{nullptr};
    QPushButton*    m_nodata_color_btn{nullptr};
    QColor          m_nodata_color{0, 0, 0, 0};
};
