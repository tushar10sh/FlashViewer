#pragma once

#include <QWidget>
#include <QColor>

class LayerManager;
class VectorLayer;
class QComboBox;
class QPushButton;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QSlider;
class QLabel;

class VectorLayerPanel : public QWidget {
    Q_OBJECT
public:
    explicit VectorLayerPanel(QWidget* parent = nullptr);
    ~VectorLayerPanel() override = default;

    void setLayerManager(LayerManager* mgr);
    void setPaneListResolver(std::function<std::vector<std::pair<uint64_t, QString>>()> fn) {
        m_pane_list = std::move(fn);
    }
    void configureFor(VectorLayer* layer);

signals:
    void openVectorRequested();
    void fitToLayerRequested(int layerIndex);
    void layerStyleChanged(VectorLayer* layer);
    void duplicateLayerRequested(VectorLayer* sourceLayer, uint64_t targetPaneId);

private slots:
    void onActiveLayerChanged(int index);
    void onLayerListChanged();
    void onLayerComboSelected(int comboIndex);
    void onTargetPaneChanged(int comboIndex);
    void onDuplicateToPaneClicked();
    void onAddLayerClicked();
    void onRemoveLayerClicked();
    void onFitToLayerClicked();
    void onStrokeColorClicked();
    void onFillColorClicked();
    void onStrokeWidthChanged(double val);
    void onStrokeStyleChanged(int index);
    void onTransparentFillToggled(bool checked);
    void onFillOpacityChanged(int val);
    void onPointSizeChanged(int val);

private:
    void rebuildLayerCombo();
    void updateUiFromLayer();
    void updateColorButtons();

    LayerManager*   m_mgr{nullptr};
    VectorLayer*    m_current_layer{nullptr};
    std::function<std::vector<std::pair<uint64_t, QString>>()> m_pane_list;

    // UI elements
    QComboBox*      m_layer_combo{nullptr};
    QPushButton*    m_btn_add{nullptr};
    QPushButton*    m_btn_remove{nullptr};
    QPushButton*    m_btn_fit{nullptr};
    QComboBox*      m_combo_target_pane{nullptr};
    QPushButton*    m_btn_duplicate{nullptr};

    // Style controls
    QPushButton*    m_btn_stroke_color{nullptr};
    QDoubleSpinBox* m_spin_stroke_width{nullptr};
    QComboBox*      m_combo_stroke_style{nullptr};
    QCheckBox*      m_chk_transparent_fill{nullptr};
    QPushButton*    m_btn_fill_color{nullptr};
    QSlider*        m_slider_fill_opacity{nullptr};
    QLabel*         m_lbl_fill_opacity{nullptr};
    QSpinBox*       m_spin_point_size{nullptr};

    // Info labels
    QLabel*         m_lbl_geom_type{nullptr};
    QLabel*         m_lbl_features{nullptr};
    QLabel*         m_lbl_crs{nullptr};
    QLabel*         m_lbl_path{nullptr};
    QWidget*        m_config_container{nullptr};
    QLabel*         m_lbl_no_layer{nullptr};

    bool            m_updating{false};
};
