#include <catch2/catch_test_macros.hpp>
#include "util/SystemMetrics.hpp"
#include "panels/GpuMonitorPanel.hpp"

TEST_CASE("SystemMetrics samples CPU and RAM resources correctly", "[util][metrics]") {
    auto stats = SystemMetrics::instance().sample();

    // System RAM should be positive on all modern OSes
    CHECK(stats.ram_system_total > 0);
    CHECK(stats.ram_percent >= 0.0);
    CHECK(stats.ram_percent <= 100.0);
    CHECK(stats.cpu_percent >= 0.0);
    CHECK(stats.cpu_percent <= 100.0);

    // Subsequent sample after short period
    auto stats2 = SystemMetrics::instance().sample();
    CHECK(stats2.ram_system_total == stats.ram_system_total);
    CHECK(stats2.ram_percent >= 0.0);
    CHECK(stats2.ram_percent <= 100.0);
}

TEST_CASE("GpuMonitorPanel configuration and sampling", "[panels][gpu_monitor]") {
    GpuMonitorPanel panel;

    // Default configuration
    CHECK(panel.viewMode() == ResourceViewMode::Dashboard);
    CHECK(panel.isCpuVisible());
    CHECK(panel.isRamVisible());
    CHECK(panel.isVramVisible());
    CHECK(panel.isGpuVisible());

    // Switch view mode
    panel.setViewMode(ResourceViewMode::GaugesOnly);
    CHECK(panel.viewMode() == ResourceViewMode::GaugesOnly);

    panel.setViewMode(ResourceViewMode::GraphsOnly);
    CHECK(panel.viewMode() == ResourceViewMode::GraphsOnly);

    // Toggle metric visibility
    panel.setCpuVisible(false);
    CHECK_FALSE(panel.isCpuVisible());

    panel.setRamVisible(false);
    CHECK_FALSE(panel.isRamVisible());

    panel.setVramVisible(false);
    CHECK_FALSE(panel.isVramVisible());

    panel.setGpuVisible(false);
    CHECK_FALSE(panel.isGpuVisible());

    // Feed samples without crash
    panel.addSample(1024 * 1024 * 64, 16, 1);

    ResourceSample rs;
    rs.cpu_percent = 25.5;
    rs.ram_percent = 40.0;
    rs.ram_proc_bytes = 100 * 1024 * 1024;
    rs.ram_sys_used = 8ULL * 1024 * 1024 * 1024;
    rs.ram_sys_total = 16ULL * 1024 * 1024 * 1024;
    rs.vram_bytes = 128 * 1024 * 1024;
    rs.vram_budget_bytes = 1536ULL * 1024 * 1024;
    rs.vram_percent = 8.3;
    rs.tiles = 20;
    rs.panes = 2;
    rs.gpu_percent = 31.2;

    panel.addResourceSample(rs);
    panel.setGpuIdentity("Apple M-series", "Apple", "4.1");
}
