#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "panels/SnrToolPanel.hpp"
#include "panels/MtfToolPanel.hpp"
#include "core/RasterLayer.hpp"
#include "render/TileRenderer.hpp"
#include "render/MapCanvas.hpp"
#include "io/DatasetFactory.hpp"
#include "fixtures/FixtureFactory.hpp"

TEST_CASE("SNR tool calculation on synthetic dataset", "[snr][tool]") {
    FixtureFactory ff;
    auto fix = ff.gradientFloat(24, 24);
    auto ds = DatasetFactory::open(fix.path);
    REQUIRE(ds != nullptr);

    SECTION("SNR calculation with 5x5 window") {
        auto results = SnrToolPanel::computeSnr(ds.get(), 10, 10, 5);
        REQUIRE(results.size() == 1);

        const auto& r = results[0];
        REQUIRE(r.bandIndex == 1);
        REQUIRE(r.validPixelCount == 25);
        REQUIRE(r.mean > 0.0);
        REQUIRE(r.stdDev >= 0.0);
        REQUIRE(r.snrLinear >= 0.0);
    }

    SECTION("SNR calculation with 11x11 window") {
        auto results = SnrToolPanel::computeSnr(ds.get(), 10, 10, 11);
        REQUIRE(results.size() == 1);

        const auto& r = results[0];
        REQUIRE(r.validPixelCount == 121);
        REQUIRE(r.mean > 0.0);
    }
}

TEST_CASE("MTF tool calculation on synthetic dataset", "[mtf][tool]") {
    FixtureFactory ff;
    auto fix = ff.gradientFloat(32, 32);
    auto ds = DatasetFactory::open(fix.path);
    REQUIRE(ds != nullptr);

    SECTION("MTF calculation with 11x11 window") {
        auto res = MtfToolPanel::computeMtf(ds.get(), 16, 16, 11);
        REQUIRE(res.valid == true);
        REQUIRE(!res.horizMtf.empty());
        REQUIRE(!res.vertMtf.empty());

        REQUIRE_THAT(res.horizMtf[0].y(), Catch::Matchers::WithinAbs(1.0, 1e-4));
        REQUIRE_THAT(res.vertMtf[0].y(), Catch::Matchers::WithinAbs(1.0, 1e-4));

        REQUIRE(res.horizMtf50 >= 0.0);
        REQUIRE(res.vertMtf50 >= 0.0);
    }
}

TEST_CASE("DisplayFilterMode and FilterParams structure", "[filter]") {
    FilterParams params;
    REQUIRE(params.filterMode == 0);
    REQUIRE(params.checkSize == 64.0f);

    params.filterMode = static_cast<int>(MapCanvas::DisplayFilterMode::Checkerboard);
    params.checkSize = 32.0f;
    params.swipePos = 120.0f;

    REQUIRE(params.filterMode == 1);
    REQUIRE(params.checkSize == 32.0f);
    REQUIRE(params.swipePos == 120.0f);
}

TEST_CASE("Per-layer DisplayFilterMode on RasterLayer", "[rasterlayer][filter]") {
    FixtureFactory ff;
    auto fix = ff.gradientFloat(16, 16);
    auto ds = DatasetFactory::open(fix.path);
    REQUIRE(ds != nullptr);

    RasterLayer layer(ds);
    REQUIRE(layer.displayFilterMode() == RasterLayer::DisplayFilterMode::None);
    REQUIRE(layer.displayFilterCheckSize() == 64);

    layer.setDisplayFilterMode(RasterLayer::DisplayFilterMode::Checkerboard);
    layer.setDisplayFilterCheckSize(32);
    layer.setDisplayFilterSwipeX(0.4f);
    layer.setDisplayFilterSwipeY(0.6f);

    REQUIRE(layer.displayFilterMode() == RasterLayer::DisplayFilterMode::Checkerboard);
    REQUIRE(layer.displayFilterCheckSize() == 32);
    REQUIRE_THAT(layer.displayFilterSwipeX(), Catch::Matchers::WithinAbs(0.4f, 1e-4));
    REQUIRE_THAT(layer.displayFilterSwipeY(), Catch::Matchers::WithinAbs(0.6f, 1e-4));
}
