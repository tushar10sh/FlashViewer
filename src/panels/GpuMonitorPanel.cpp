#include "panels/GpuMonitorPanel.hpp"
#include "util/SystemMetrics.hpp"
#include "util/PerfMetrics.hpp"

#include <QPainter>
#include <QPolygonF>
#include <QLabel>
#include <algorithm>
#include <cmath>

// --------------------------------------------------------------------------
// ResourceCanvas — custom QPainter widget that renders gauges & sparklines
// --------------------------------------------------------------------------

class ResourceCanvas : public QWidget {
public:
    explicit ResourceCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    void setGpuIdentity(const QString& id) {
        if (m_identity != id) {
            m_identity = id;
            update();
        }
    }

    void addSample(const ResourceSample& s) {
        m_latest = s;
        m_samples.push_back(s);
        while (static_cast<int>(m_samples.size()) > kMaxSamples) {
            m_samples.pop_front();
        }

        m_peak_cpu = 1.0;
        m_peak_ram = 1.0;
        m_peak_vram_mb = 1.0;
        m_peak_gpu = 1.0;
        for (const auto& sm : m_samples) {
            m_peak_cpu = std::max(m_peak_cpu, sm.cpu_percent);
            m_peak_ram = std::max(m_peak_ram, sm.ram_percent);
            double vram_mb = static_cast<double>(sm.vram_bytes) / (1024.0 * 1024.0);
            m_peak_vram_mb = std::max(m_peak_vram_mb, vram_mb);
            m_peak_gpu = std::max(m_peak_gpu, sm.gpu_percent);
        }
        update();
    }

    void setConfig(ResourceViewMode mode, bool cpu, bool ram, bool vram, bool gpu) {
        m_view_mode = mode;
        m_show_cpu = cpu;
        m_show_ram = ram;
        m_show_vram = vram;
        m_show_gpu = gpu;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRect full = rect();
        const bool dark = palette().base().color().lightness() < 128;
        p.fillRect(full, palette().base());

        const QColor muted = dark ? QColor(0x8b, 0x94, 0x9e) : QColor(0x65, 0x6d, 0x76);
        const QColor borderCol = dark ? QColor(0x30, 0x36, 0x3d) : QColor(0xd0, 0xd7, 0xde);
        const QColor trackBg = dark ? QColor(0x21, 0x26, 0x2d) : QColor(0xeb, 0xf0, 0xf4);

        // Curated metric colors (Dark / Light Primer palettes)
        const QColor colCpu  = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
        const QColor colRam  = dark ? QColor(0x58, 0xa6, 0xff) : QColor(0x09, 0x69, 0xda);
        const QColor colVram = dark ? QColor(0xbc, 0x8c, 0xff) : QColor(0x82, 0x50, 0xdf);
        const QColor colGpu  = dark ? QColor(0xf0, 0x88, 0x3e) : QColor(0xcf, 0x55, 0x00);

        const int pad = 8;
        int y = pad;

        QFont base = p.font();
        QFont bold = base; bold.setBold(true);
        QFont small = base;
        if (small.pointSize() > 8) small.setPointSize(small.pointSize() - 1);

        // Hardware Identity Subtitle (if available)
        if (!m_identity.isEmpty()) {
            p.setPen(muted);
            p.setFont(small);
            p.drawText(QRect(pad, y, full.width() - 2 * pad, 14), Qt::AlignLeft,
                       p.fontMetrics().elidedText(m_identity, Qt::ElideRight, full.width() - 2 * pad));
            y += 18;
            p.setFont(base);
        }

        // 1. Gauges Section
        if (m_view_mode != ResourceViewMode::GraphsOnly) {
            auto drawGauge = [&](const QString& label, double percent, const QString& detail, const QColor& col) {
                const int gaugeH = 8;
                const int textH = 14;

                // Label and Value
                p.setFont(bold);
                p.setPen(col);
                p.drawText(QRect(pad, y, 45, textH), Qt::AlignLeft | Qt::AlignVCenter, label);

                p.setFont(small);
                p.setPen(palette().windowText().color());
                QString valStr = QString("%1%").arg(percent, 4, 'f', 1);
                p.drawText(QRect(pad + 48, y, 48, textH), Qt::AlignLeft | Qt::AlignVCenter, valStr);

                if (!detail.isEmpty()) {
                    p.setPen(muted);
                    int availW = full.width() - pad - (pad + 100);
                    if (availW > 40) {
                        p.drawText(QRect(pad + 100, y, availW, textH), Qt::AlignRight | Qt::AlignVCenter,
                                   p.fontMetrics().elidedText(detail, Qt::ElideLeft, availW));
                    }
                }
                y += textH + 2;

                // Progress Bar
                QRect track(pad, y, full.width() - 2 * pad, gaugeH);
                p.setPen(Qt::NoPen);
                p.setBrush(trackBg);
                p.drawRoundedRect(track, 3, 3);

                int fillW = static_cast<int>(std::round(track.width() * std::clamp(percent / 100.0, 0.0, 1.0)));
                if (fillW > 0) {
                    QRect fill(track.left(), track.top(), fillW, track.height());
                    p.setBrush(col);
                    p.drawRoundedRect(fill, 3, 3);
                }
                y += gaugeH + 6;
            };

            if (m_show_cpu) {
                drawGauge(tr("CPU"), m_latest.cpu_percent, "", colCpu);
            }
            if (m_show_ram) {
                double procMb = static_cast<double>(m_latest.ram_proc_bytes) / (1024.0 * 1024.0);
                double sysUsedGb = static_cast<double>(m_latest.ram_sys_used) / (1024.0 * 1024.0 * 1024.0);
                double sysTotGb  = static_cast<double>(m_latest.ram_sys_total) / (1024.0 * 1024.0 * 1024.0);
                QString detail;
                if (sysTotGb > 0.0) {
                    detail = tr("App: %1 MB · Sys: %2/%3 GB")
                                 .arg(procMb, 0, 'f', 0)
                                 .arg(sysUsedGb, 0, 'f', 1)
                                 .arg(sysTotGb, 0, 'f', 1);
                } else if (procMb > 0.0) {
                    detail = tr("App: %1 MB").arg(procMb, 0, 'f', 0);
                }
                drawGauge(tr("RAM"), m_latest.ram_percent, detail, colRam);
            }
            if (m_show_vram) {
                double vramMb = static_cast<double>(m_latest.vram_bytes) / (1024.0 * 1024.0);
                double budgetMb = static_cast<double>(m_latest.vram_budget_bytes) / (1024.0 * 1024.0);
                QString detail = tr("%1 MB (%2 tiles · %3 pane%4)")
                                     .arg(vramMb, 0, 'f', 1)
                                     .arg(m_latest.tiles)
                                     .arg(m_latest.panes)
                                     .arg(m_latest.panes == 1 ? "" : "s");
                if (budgetMb > 0.0) {
                    detail = tr("%1 / %2 MB (%3 tiles)")
                                 .arg(vramMb, 0, 'f', 1)
                                 .arg(budgetMb, 0, 'f', 0)
                                 .arg(m_latest.tiles);
                }
                drawGauge(tr("VRAM"), m_latest.vram_percent, detail, colVram);
            }
            if (m_show_gpu) {
                QString detail = tr("Render load estimate");
                drawGauge(tr("GPU"), m_latest.gpu_percent, detail, colGpu);
            }
            y += 2;
        }

        // 2. Sparklines Graph Section
        if (m_view_mode != ResourceViewMode::GaugesOnly) {
            const int legendH = 14;
            int plotH = full.height() - y - pad - legendH - 4;
            if (plotH >= 24 && m_samples.size() >= 2) {
                QRect plot(pad, y, full.width() - 2 * pad, plotH);

                // Frame and Grid
                p.setPen(QPen(borderCol, 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(plot);

                p.setPen(QPen(borderCol, 1, Qt::DotLine));
                int midY = plot.top() + plot.height() / 2;
                p.drawLine(plot.left(), midY, plot.right(), midY);

                const int n = static_cast<int>(m_samples.size());
                auto xAt = [&](int i) {
                    return plot.left() + plot.width() * static_cast<double>(i) / (n - 1);
                };

                auto drawLine = [&](const std::vector<double>& vals, const QColor& col) {
                    QPolygonF poly;
                    for (int i = 0; i < n; ++i) {
                        double v = std::clamp(vals[i], 0.0, 100.0);
                        double py = plot.bottom() - plot.height() * (v / 100.0);
                        poly << QPointF(xAt(i), py);
                    }

                    // Fill under curve
                    QPolygonF fillPoly = poly;
                    fillPoly << QPointF(plot.right(), plot.bottom())
                             << QPointF(plot.left(), plot.bottom());
                    QColor fillC = col;
                    fillC.setAlpha(dark ? 30 : 22);
                    p.setPen(Qt::NoPen);
                    p.setBrush(fillC);
                    p.drawPolygon(fillPoly);

                    // Polyline stroke
                    p.setPen(QPen(col, 1.5));
                    p.setBrush(Qt::NoBrush);
                    p.drawPolyline(poly);
                };

                if (m_show_cpu) {
                    std::vector<double> vals;
                    vals.reserve(n);
                    for (const auto& s : m_samples) vals.push_back(s.cpu_percent);
                    drawLine(vals, colCpu);
                }
                if (m_show_ram) {
                    std::vector<double> vals;
                    vals.reserve(n);
                    for (const auto& s : m_samples) vals.push_back(s.ram_percent);
                    drawLine(vals, colRam);
                }
                if (m_show_vram) {
                    std::vector<double> vals;
                    vals.reserve(n);
                    for (const auto& s : m_samples) vals.push_back(s.vram_percent);
                    drawLine(vals, colVram);
                }
                if (m_show_gpu) {
                    std::vector<double> vals;
                    vals.reserve(n);
                    for (const auto& s : m_samples) vals.push_back(s.gpu_percent);
                    drawLine(vals, colGpu);
                }

                // Legend at bottom
                y += plotH + 4;
                p.setFont(small);
                int legX = pad;
                auto drawLegendItem = [&](const QString& name, double cur, const QColor& col) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(col);
                    p.drawRoundedRect(QRect(legX, y + 3, 8, 8), 2, 2);
                    legX += 12;

                    p.setPen(muted);
                    QString txt = QString("%1: %2%").arg(name).arg(cur, 0, 'f', 0);
                    int tw = p.fontMetrics().horizontalAdvance(txt);
                    p.drawText(QRect(legX, y, tw + 4, legendH), Qt::AlignLeft | Qt::AlignVCenter, txt);
                    legX += tw + 10;
                };

                if (m_show_cpu && legX < full.width() - 40) drawLegendItem("CPU", m_latest.cpu_percent, colCpu);
                if (m_show_ram && legX < full.width() - 40) drawLegendItem("RAM", m_latest.ram_percent, colRam);
                if (m_show_vram && legX < full.width() - 40) drawLegendItem("VRAM", m_latest.vram_percent, colVram);
                if (m_show_gpu && legX < full.width() - 40) drawLegendItem("GPU", m_latest.gpu_percent, colGpu);
            }
        }
    }

private:
    static constexpr int kMaxSamples = 180;

    ResourceViewMode           m_view_mode{ResourceViewMode::Dashboard};
    bool                       m_show_cpu{true};
    bool                       m_show_ram{true};
    bool                       m_show_vram{true};
    bool                       m_show_gpu{true};

    QString                    m_identity;
    std::deque<ResourceSample> m_samples;
    ResourceSample             m_latest;

    double                     m_peak_cpu{1.0};
    double                     m_peak_ram{1.0};
    double                     m_peak_vram_mb{1.0};
    double                     m_peak_gpu{1.0};
};

// --------------------------------------------------------------------------
// GpuMonitorPanel Implementation
// --------------------------------------------------------------------------

GpuMonitorPanel::GpuMonitorPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
    setToolTip(tr("Resource Monitor: Live CPU, GPU, VRAM, and RAM utilization."));
}

void GpuMonitorPanel::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(4, 4, 4, 4);
    mainLay->setSpacing(4);

    // Header Toolbar
    auto* tb = new QWidget(this);
    auto* tbLay = new QHBoxLayout(tb);
    tbLay->setContentsMargins(4, 0, 4, 0);
    tbLay->setSpacing(6);

    auto* modeLbl = new QLabel(tr("View:"), tb);
    m_mode_combo = new QComboBox(tb);
    m_mode_combo->addItem(tr("Dashboard"), static_cast<int>(ResourceViewMode::Dashboard));
    m_mode_combo->addItem(tr("Gauges"), static_cast<int>(ResourceViewMode::GaugesOnly));
    m_mode_combo->addItem(tr("Graphs"), static_cast<int>(ResourceViewMode::GraphsOnly));
    m_mode_combo->setToolTip(tr("Switch between full dashboard, compact gauges, or historical graphs."));

    m_chk_cpu  = new QCheckBox(tr("CPU"), tb);
    m_chk_ram  = new QCheckBox(tr("RAM"), tb);
    m_chk_vram = new QCheckBox(tr("VRAM"), tb);
    m_chk_gpu  = new QCheckBox(tr("GPU"), tb);

    m_chk_cpu->setChecked(m_show_cpu);
    m_chk_ram->setChecked(m_show_ram);
    m_chk_vram->setChecked(m_show_vram);
    m_chk_gpu->setChecked(m_show_gpu);

    m_chk_cpu->setToolTip(tr("Toggle CPU utilization visibility"));
    m_chk_ram->setToolTip(tr("Toggle RAM utilization visibility"));
    m_chk_vram->setToolTip(tr("Toggle VRAM utilization visibility"));
    m_chk_gpu->setToolTip(tr("Toggle GPU utilization visibility"));

    tbLay->addWidget(modeLbl);
    tbLay->addWidget(m_mode_combo);
    tbLay->addSpacing(4);
    tbLay->addWidget(m_chk_cpu);
    tbLay->addWidget(m_chk_ram);
    tbLay->addWidget(m_chk_vram);
    tbLay->addWidget(m_chk_gpu);
    tbLay->addStretch();

    mainLay->addWidget(tb);

    // Canvas Widget
    m_canvas = new ResourceCanvas(this);
    mainLay->addWidget(m_canvas, 1);

    // Wire up signals
    connect(m_mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        setViewMode(static_cast<ResourceViewMode>(m_mode_combo->itemData(idx).toInt()));
    });

    auto onToggled = [this] {
        m_show_cpu  = m_chk_cpu->isChecked();
        m_show_ram  = m_chk_ram->isChecked();
        m_show_vram = m_chk_vram->isChecked();
        m_show_gpu  = m_chk_gpu->isChecked();
        if (m_canvas) {
            m_canvas->setConfig(m_view_mode, m_show_cpu, m_show_ram, m_show_vram, m_show_gpu);
        }
    };

    connect(m_chk_cpu,  &QCheckBox::toggled, this, onToggled);
    connect(m_chk_ram,  &QCheckBox::toggled, this, onToggled);
    connect(m_chk_vram, &QCheckBox::toggled, this, onToggled);
    connect(m_chk_gpu,  &QCheckBox::toggled, this, onToggled);
}

void GpuMonitorPanel::setGpuIdentity(const QString& renderer, const QString& vendor,
                                     const QString& version) {
    QString id = renderer;
    if (!vendor.isEmpty())  id += QStringLiteral(" · ") + vendor;
    if (!version.isEmpty()) id += QStringLiteral(" · GL ") + version;
    if (m_canvas) {
        m_canvas->setGpuIdentity(id);
    }
}

void GpuMonitorPanel::addResourceSample(const ResourceSample& sample) {
    if (m_canvas) {
        m_canvas->addSample(sample);
    }
}

void GpuMonitorPanel::addSample(std::size_t bytes, int tiles, int panes) {
    auto sys_stats = SystemMetrics::instance().sample();
    ResourceSample s;
    s.cpu_percent = sys_stats.cpu_percent;
    s.ram_percent = sys_stats.ram_percent;
    s.ram_proc_bytes = sys_stats.ram_process_bytes;
    s.ram_sys_used = sys_stats.ram_system_used;
    s.ram_sys_total = sys_stats.ram_system_total;

    s.vram_bytes = bytes;
    s.vram_budget_bytes = 1536ULL * 1024 * 1024;
    s.vram_percent = (s.vram_budget_bytes > 0)
        ? std::clamp((static_cast<double>(bytes) / s.vram_budget_bytes) * 100.0, 0.0, 100.0)
        : 0.0;
    s.tiles = tiles;
    s.panes = panes;
    s.gpu_percent = PerfMetrics::instance().sampleGpuUtilization(250.0);

    addResourceSample(s);
}

void GpuMonitorPanel::setViewMode(ResourceViewMode mode) {
    m_view_mode = mode;
    if (m_canvas) {
        m_canvas->setConfig(m_view_mode, m_show_cpu, m_show_ram, m_show_vram, m_show_gpu);
    }
}

void GpuMonitorPanel::setCpuVisible(bool visible) {
    m_show_cpu = visible;
    if (m_chk_cpu) m_chk_cpu->setChecked(visible);
}

void GpuMonitorPanel::setRamVisible(bool visible) {
    m_show_ram = visible;
    if (m_chk_ram) m_chk_ram->setChecked(visible);
}

void GpuMonitorPanel::setVramVisible(bool visible) {
    m_show_vram = visible;
    if (m_chk_vram) m_chk_vram->setChecked(visible);
}

void GpuMonitorPanel::setGpuVisible(bool visible) {
    m_show_gpu = visible;
    if (m_chk_gpu) m_chk_gpu->setChecked(visible);
}
