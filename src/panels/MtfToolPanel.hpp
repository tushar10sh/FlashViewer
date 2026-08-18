#pragma once
#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <memory>
#include <vector>
#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include "widgets/ChartTools.hpp"
#include "core/RasterLayer.hpp"

class MtfToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit MtfToolPanel(QWidget* parent = nullptr);
    ~MtfToolPanel() override = default;

    int windowSize() const { return m_window_size; }
    void setWindowSize(int sz);

    struct MtfResult {
        std::vector<QPointF> horizMtf; // (frequency cycles/pixel, MTF 0.0-1.0)
        std::vector<QPointF> vertMtf;  // (frequency cycles/pixel, MTF 0.0-1.0)
        double horizMtf50{0.0};
        double vertMtf50{0.0};
        double horizNyquist{0.0};
        double vertNyquist{0.0};
        bool valid{false};
    };

    static MtfResult computeMtf(RasterDataset* ds, int col, int row, int winSize);

    void calculateAndShow(RasterLayer* layer, int col, int row, double geoX, double geoY, const QString& crsWkt);
    void clearResults();

signals:
    void windowSizeChanged(int newSize);

private:
    void setupUi();
    void updatePlot(const MtfResult& res);

    int m_window_size{11};
    QComboBox* m_size_combo{nullptr};
    QLabel* m_info_label{nullptr};
    QChart* m_chart{nullptr};
    FvChartView* m_chart_view{nullptr};
    QLineSeries* m_series_h{nullptr};
    QLineSeries* m_series_v{nullptr};
    QValueAxis* m_axis_x{nullptr};
    QValueAxis* m_axis_y{nullptr};

    QLabel* m_lbl_h_mtf50{nullptr};
    QLabel* m_lbl_v_mtf50{nullptr};
    QLabel* m_lbl_h_nyq{nullptr};
    QLabel* m_lbl_v_nyq{nullptr};

    MtfResult m_last_result;

    RasterLayer* m_last_layer{nullptr};
    int m_last_col{0};
    int m_last_row{0};
    double m_last_geo_x{0.0};
    double m_last_geo_y{0.0};
    QString m_last_crs_wkt;
    bool m_has_last_calc{false};
};
