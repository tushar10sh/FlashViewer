#include "util/PerfMetrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

FrameStats fvComputeFrameStats(const std::vector<double>& samples_ms) {
    FrameStats st;
    if (samples_ms.empty()) return st;

    st.count = static_cast<int>(samples_ms.size());
    double sum = 0.0;
    for (double ms : samples_ms) {
        sum += ms;
        if (ms > st.max_ms) st.max_ms = ms;
        if (ms > kFrameBudgetHardMs) ++st.over_100ms;
    }
    st.avg_ms = sum / st.count;

    // Nearest-rank p99: index = ceil(0.99 · N) − 1 into the sorted samples.
    std::vector<double> sorted = samples_ms;
    std::sort(sorted.begin(), sorted.end());
    int idx = static_cast<int>(std::ceil(0.99 * st.count)) - 1;
    if (idx < 0) idx = 0;
    if (idx >= st.count) idx = st.count - 1;
    st.p99_ms = sorted[static_cast<size_t>(idx)];
    return st;
}

// --------------------------------------------------------------------------

PerfMetrics& PerfMetrics::instance() {
    static PerfMetrics inst;
    return inst;
}

void PerfMetrics::pushFrame(double ms) {
    const int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.push_back(ms);
    m_frame_timestamps.push_back({now_ns, ms});
    while (static_cast<int>(m_frames.size()) > kMaxFrameSamples)
        m_frames.pop_front();
    const int64_t cutoff_ns = now_ns - static_cast<int64_t>(3.0e9);
    while (!m_frame_timestamps.empty() && m_frame_timestamps.front().first < cutoff_ns)
        m_frame_timestamps.pop_front();
}

FrameStats PerfMetrics::frameStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return fvComputeFrameStats(std::vector<double>(m_frames.begin(), m_frames.end()));
}

double PerfMetrics::sampleGpuUtilization(double windowMs) const {
    if (windowMs <= 0.0) return 0.0;
    const int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t window_ns = static_cast<int64_t>(windowMs * 1.0e6);
    const int64_t cutoff_ns = now_ns - window_ns;

    std::lock_guard<std::mutex> lock(m_mutex);
    double render_time_sum_ms = 0.0;
    for (const auto& [ts, ms] : m_frame_timestamps) {
        if (ts >= cutoff_ns) {
            render_time_sum_ms += ms;
        }
    }
    double load = (render_time_sum_ms / windowMs) * 100.0;
    return std::clamp(load, 0.0, 100.0);
}

void PerfMetrics::markOpenStart(uint64_t layer_id) {
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_open_start_ns[layer_id] = now;
}

bool PerfMetrics::markFirstTileReady(uint64_t layer_id) {
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_open_start_ns.find(layer_id);
    if (it == m_open_start_ns.end()) return false;   // no pending open, or already recorded
    m_last_open_latency_ms = fvLatencyMs(it->second, now);
    m_open_start_ns.erase(it);
    return true;
}

double PerfMetrics::lastOpenLatencyMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_open_latency_ms;
}

void PerfMetrics::recordStall(const std::string& /*tag*/, double delta_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stall_count;
    m_last_stall_ms = delta_ms;
}

int PerfMetrics::stallCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stall_count;
}

double PerfMetrics::lastStallMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_stall_ms;
}

void PerfMetrics::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.clear();
    m_frame_timestamps.clear();
    m_open_start_ns.clear();
    m_last_open_latency_ms = 0.0;
    m_stall_count = 0;
    m_last_stall_ms = 0.0;
}
