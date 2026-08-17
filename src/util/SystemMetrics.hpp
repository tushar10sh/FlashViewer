#pragma once
#include <cstddef>
#include <cstdint>

struct SystemResourceStats {
    double      cpu_percent{0.0};          // Total CPU utilization [0.0, 100.0]
    std::size_t ram_process_bytes{0};      // RAM used by current process (RSS / Working Set)
    std::size_t ram_system_used{0};        // System-wide RAM used (bytes)
    std::size_t ram_system_total{0};       // Total system RAM (bytes)
    double      ram_percent{0.0};          // System RAM utilization [0.0, 100.0]
};

class SystemMetrics {
public:
    static SystemMetrics& instance();

    /// Sample current CPU and RAM metrics (call periodically).
    SystemResourceStats sample();

private:
    SystemMetrics() = default;
    ~SystemMetrics() = default;

#if defined(__APPLE__)
    uint64_t m_prev_cpu_user{0};
    uint64_t m_prev_cpu_system{0};
    uint64_t m_prev_cpu_idle{0};
    uint64_t m_prev_cpu_nice{0};
#elif defined(_WIN32)
    uint64_t m_prev_idle{0};
    uint64_t m_prev_kernel{0};
    uint64_t m_prev_user{0};
#else // Linux
    uint64_t m_prev_user{0};
    uint64_t m_prev_nice{0};
    uint64_t m_prev_system{0};
    uint64_t m_prev_idle{0};
    uint64_t m_prev_iowait{0};
    uint64_t m_prev_irq{0};
    uint64_t m_prev_softirq{0};
    uint64_t m_prev_steal{0};
#endif
    bool m_has_prev{false};
};
