#pragma once
#include "gis/InspectTypes.hpp"
#include "gis/PixelSampler.hpp"
#include <QWidget>
#include <QVector>
#include <string>

class LayerManager;
class QTableWidget;
class QLabel;
class QComboBox;
class QPushButton;

/// NumericDumpPanel displays an 11x11 pixel matrix centered around the inspected pixel.
/// For grayscale imagery, values are displayed in black.
/// For multi-channel (RGB) imagery, values are color-coded in dark red, dark green, and dark blue.
/// The center selected pixel is rendered with bold font and an accent highlight.
class NumericDumpPanel : public QWidget {
    Q_OBJECT
public:
    explicit NumericDumpPanel(QWidget* parent = nullptr);
    ~NumericDumpPanel() override = default;

    void setLayerManager(LayerManager* lm) { m_layer_mgr = lm; }

    /// Feed inspect results from a click or inspect gesture
    void inspectGroups(double geo_x, double geo_y, const std::string& geoWkt,
                       const QVector<InspectPaneGroup>& groups);

    /// Clear the matrix display
    void clear();

    /// Re-sample and refresh the current layer/location
    void refresh();

private slots:
    void onLayerChanged(int index);
    void onViewModeChanged(int index);
    void copyToClipboard();

protected:
    void changeEvent(QEvent* event) override;

private:
    void updateTable();
    void setupUi();

    LayerManager*               m_layer_mgr{nullptr};
    double                      m_geo_x{0.0};
    double                      m_geo_y{0.0};
    std::string                 m_geo_wkt;
    QVector<InspectLayerEntry>  m_cached_layers;
    int                         m_selected_layer_idx{0};
    PixelPatchSample            m_patch_sample;

    QLabel*                     m_info_label{nullptr};
    QLabel*                     m_coord_label{nullptr};
    QComboBox*                  m_layer_combo{nullptr};
    QComboBox*                  m_mode_combo{nullptr};
    QPushButton*                m_copy_btn{nullptr};
    QTableWidget*               m_table{nullptr};
    class NumericDumpDelegate*  m_delegate{nullptr};
    QLabel*                     m_status_label{nullptr};
};
