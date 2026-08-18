#pragma once
#include <QWidget>
#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <memory>
#include <vector>
#include "core/RasterLayer.hpp"

class SnrToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit SnrToolPanel(QWidget* parent = nullptr);
    ~SnrToolPanel() override = default;

    int windowSize() const { return m_window_size; }
    void setWindowSize(int sz);

    struct SnrBandResult {
        int bandIndex{1};
        QString bandName;
        double mean{0.0};
        double stdDev{0.0};
        double minVal{0.0};
        double maxVal{0.0};
        double snrLinear{0.0};
        double snrDb{0.0};
        int validPixelCount{0};
    };

    static std::vector<SnrBandResult> computeSnr(RasterDataset* ds, int col, int row, int winSize);

    void calculateAndShow(RasterLayer* layer, int col, int row, double geoX, double geoY, const QString& crsWkt);
    void clearResults();

signals:
    void windowSizeChanged(int newSize);

private:
    void setupUi();
    void updateTable(const std::vector<SnrBandResult>& results);
    void copyToClipboard();

    int m_window_size{5};
    QComboBox* m_size_combo{nullptr};
    QLabel* m_info_label{nullptr};
    QTableWidget* m_table{nullptr};
    QPushButton* m_btn_copy{nullptr};
    QPushButton* m_btn_clear{nullptr};

    std::vector<SnrBandResult> m_last_results;

    RasterLayer* m_last_layer{nullptr};
    int m_last_col{0};
    int m_last_row{0};
    double m_last_geo_x{0.0};
    double m_last_geo_y{0.0};
    QString m_last_crs_wkt;
    bool m_has_last_calc{false};
};
