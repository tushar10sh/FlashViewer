#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Performance instrumentation (Phase 12) — makes the SRS §6.1 KPIs (NFR-PERF-1…7)
// measurable and feeds the always-on Performance HUD (FR-APP-14).
//
// Two tiers, mirroring how TileCache::residentBytes() became always-on in Phase 10:
//  - The metrics themselves (frame ring buffer, open-latency, lightweight stall count)
//    are ALWAYS compiled and cheap, so the HUD is meaningful in every build.
//  - Verbose per-op logging and the CI perf-lane detail are gated behind the
//    FV_PERF_INSTRUMENT compile option at the *call sites* (see MainWindow/TileRenderer).
//
// The statistics math lives in free functions so it is unit-testable without a widget
// or a GL context — the same pattern as fvRepaintWithinBudget in MapCanvas.hpp.

// Frame-budget thresholds from SRS §6.1: 33 ms ≈ 30 FPS target; a single frame over
// 100 ms is a hard violation (NFR-PERF-1). A UI-thread stall over 50 ms breaks
// NFR-PERF-2.
inline constexpr double kFrameBudgetSoftMs = 33.0;    // ~30 FPS
inline constexpr double kFrameBudgetHardMs = 100.0;   // NFR-PERF-1 hard cap
inline constexpr double kStallThresholdMs  = 50.0;    // NFR-PERF-2
inline constexpr int    kMaxFrameSamples   = 240;     // ring-buffer depth (~4 s @ 60 FPS)

struct FrameStats {
    double avg_ms{0.0};
    double max_ms{0.0};
    double p99_ms{0.0};    // nearest-rank 99th percentile
    int    over_100ms{0};  // frames exceeding the NFR-PERF-1 hard cap
    int    count{0};
    double fps() const { return avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0; }
};

// --- Pure helpers (no Qt/GL state) — unit-tested directly ---------------------

// Compute avg / max / nearest-rank p99 / >100 ms count over a window of per-frame
// durations (milliseconds). Empty input → a zeroed FrameStats.
FrameStats fvComputeFrameStats(const std::vector<double>& samples_ms);

// True when the observed inter-tick gap exceeds the expected interval by more than
// the NFR-PERF-2 stall threshold (i.e. the UI event loop was blocked).
inline bool fvIsStall(double expected_ms, double actual_ms) {
    return (actual_ms - expected_ms) > kStallThresholdMs;
}

// Elapsed milliseconds between two steady-clock nanosecond timestamps (ready ≥ start).
inline double fvLatencyMs(int64_t start_ns, int64_t ready_ns) {
    return ready_ns >= start_ns ? double(ready_ns - start_ns) / 1.0e6 : 0.0;
}

// --- Process-wide metrics hub -------------------------------------------------

class PerfMetrics {
public:
    static PerfMetrics& instance();

    // Frame timer (NFR-PERF-1) — push one paintGL duration (ms); always-on.
    void       pushFrame(double ms);
    FrameStats frameStats() const;
    double     sampleGpuUtilization(double windowMs = 250.0) const;

    // Open-latency probe (NFR-PERF-3/4). markOpenStart stamps the open; the first
    // markFirstTileReady for that layer records the first-visible-tile latency and
    // clears the pending entry (later tiles are ignored). Thread-safe.
    void   markOpenStart(uint64_t layer_id);
    // Returns true only on the call that actually records the latency (the first ready
    // tile of a pending open), so a gated caller can log it once.
    bool   markFirstTileReady(uint64_t layer_id);
    double lastOpenLatencyMs() const;

    // Main-thread stall detector (NFR-PERF-2). recordStall is called by the UI-loop
    // watchdog when fvIsStall() trips; the HUD reads the running count / last value.
    void   recordStall(const std::string& tag, double delta_ms);
    int    stallCount() const;
    double lastStallMs() const;

    void reset();

private:
    PerfMetrics() = default;

    mutable std::mutex m_mutex;
    std::deque<double> m_frames;                       // rolling per-frame ms
    std::deque<std::pair<int64_t, double>> m_frame_timestamps; // (timestamp_ns, ms)
    std::unordered_map<uint64_t, int64_t> m_open_start_ns;  // layer_id → open-start ns
    double m_last_open_latency_ms{0.0};
    int    m_stall_count{0};
    double m_last_stall_ms{0.0};
};
