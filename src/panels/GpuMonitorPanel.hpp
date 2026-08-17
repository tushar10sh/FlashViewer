#pragma once
#include <QWidget>
#include <QString>
#include <QComboBox>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <cstddef>
#include <deque>

enum class ResourceViewMode {
    Dashboard = 0,   // Gauges on top, rolling sparklines below
    GaugesOnly = 1,  // Compact progress gauges only
    GraphsOnly = 2   // Historical sparklines only
};

struct ResourceSample {
    double      cpu_percent{0.0};
    double      ram_percent{0.0};
    std::size_t ram_proc_bytes{0};
    std::size_t ram_sys_used{0};
    std::size_t ram_sys_total{0};
    std::size_t vram_bytes{0};
    std::size_t vram_budget_bytes{0};
    double      vram_percent{0.0};
    int         tiles{0};
    int         panes{0};
    double      gpu_percent{0.0};
};

class ResourceCanvas;

// Live System & GPU Resource Monitor Panel.
// Shows configurable CPU, RAM, VRAM, and GPU utilization with compact gauges,
// rolling time-series sparklines, and hardware identity readouts.
class GpuMonitorPanel : public QWidget {
    Q_OBJECT
public:
    explicit GpuMonitorPanel(QWidget* parent = nullptr);

    void setGpuIdentity(const QString& renderer, const QString& vendor,
                        const QString& version);

    // Add a full resource sample
    void addResourceSample(const ResourceSample& sample);

    // Backwards-compatible overload
    void addSample(std::size_t bytes, int tiles, int panes);

    // View configurations
    void setViewMode(ResourceViewMode mode);
    ResourceViewMode viewMode() const { return m_view_mode; }

    void setCpuVisible(bool visible);
    void setRamVisible(bool visible);
    void setVramVisible(bool visible);
    void setGpuVisible(bool visible);

    bool isCpuVisible() const { return m_show_cpu; }
    bool isRamVisible() const { return m_show_ram; }
    bool isVramVisible() const { return m_show_vram; }
    bool isGpuVisible() const { return m_show_gpu; }

    QSize sizeHint() const override { return {280, 260}; }
    QSize minimumSizeHint() const override { return {180, 120}; }

private:
    void setupUi();

    ResourceViewMode m_view_mode{ResourceViewMode::Dashboard};
    bool m_show_cpu{true};
    bool m_show_ram{true};
    bool m_show_vram{true};
    bool m_show_gpu{true};

    ResourceCanvas* m_canvas{nullptr};
    QComboBox*      m_mode_combo{nullptr};
    QCheckBox*      m_chk_cpu{nullptr};
    QCheckBox*      m_chk_ram{nullptr};
    QCheckBox*      m_chk_vram{nullptr};
    QCheckBox*      m_chk_gpu{nullptr};
};
