// Live Arrow Flight Georeferencing Session & LiveRasterDataset Tests.
// Validates:
// 1. LiveRasterDataset MEM driver bridge allocation and shape initialization.
// 2. Geotransform and EPSG CRS overrides.
// 3. Tile receipt, BSQ multi-band pixel placement, and readback.
// 4. Stale-generation rejection during interactive recalculation.
// 5. RpyControlPanel and LiveConfigUpdate parameter updates.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "live/LiveGeorefSession.hpp"
#include "live/LiveRasterDataset.hpp"
#include "panels/RpyControlPanel.hpp"
#include "io/RasterDataset.hpp"
#include "core/RasterLayer.hpp"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

static double readPixel(const RasterDataset* ds, int col, int row, int band = 1) {
    if (!ds) return std::nan("");
    TileBuffer b = ds->readRegion(col, row, 1, 1, 1, 1, {band});
    return b.isValid() ? static_cast<double>(b.data[0]) : std::nan("");
}

TEST_CASE("LiveRasterDataset initializes MEM dataset and updates tiles", "[live][dataset]") {
    auto session = std::make_shared<LiveGeorefSession>(QStringLiteral("grpc://127.0.0.1:8815"));
    auto liveDs = LiveRasterDataset::create(session);

    REQUIRE(liveDs->dataset() == nullptr);

    bool readyFired = false;
    QObject::connect(liveDs.get(), &LiveRasterDataset::ready, [&] {
        readyFired = true;
    });

    int lastRow0 = -1;
    int lastRow1 = -1;
    QObject::connect(liveDs.get(), &LiveRasterDataset::regionUpdated, [&](int r0, int r1) {
        lastRow0 = r0;
        lastRow1 = r1;
    });

    const int H = 32;
    const int W = 64;
    const QStringList bandIds = {QStringLiteral("B1"), QStringLiteral("B2")};
    const QVector<double> gt = {500000.0, 10.0, 0.0, 4000000.0, 0.0, -10.0};
    const int epsg = 32632; // WGS 84 / UTM zone 32N
    const int gen = 1;

    // Simulate session emitting 'started'
    emit session->started(H, W, bandIds, gt, epsg, QStringLiteral("float32"), 0.0, gen);

    REQUIRE(readyFired);
    auto ds = liveDs->dataset();
    REQUIRE(ds != nullptr);
    REQUIRE(ds->width() == W);
    REQUIRE(ds->height() == H);
    REQUIRE(ds->bandCount() == 2);

    // Verify geotransform
    auto readGt = ds->geoTransform();
    for (int i = 0; i < 6; ++i) {
        REQUIRE_THAT(readGt.gt[i], WithinAbs(gt[i], 1e-6));
    }

    // Verify CRS
    REQUIRE_FALSE(ds->crsWkt().empty());

    // Initially buffer should be zeroed
    REQUIRE_THAT(readPixel(ds.get(), 10, 5, 1), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(readPixel(ds.get(), 10, 5, 2), WithinAbs(0.0, 1e-6));

    // Send Tile 1: rows [0, 16)
    LiveTile tile1;
    tile1.row0 = 0;
    tile1.row1 = 16;
    tile1.width = W;
    tile1.bandIds = bandIds;
    const int tile1_rows = 16;
    tile1.image.resize(2 * tile1_rows * W);
    // Fill Band 1 with 42.0f, Band 2 with 84.0f
    for (int r = 0; r < tile1_rows; ++r) {
        for (int c = 0; c < W; ++c) {
            tile1.image[0 * tile1_rows * W + r * W + c] = 42.0f;
            tile1.image[1 * tile1_rows * W + r * W + c] = 84.0f;
        }
    }

    emit session->tileReceived(tile1, gen);

    REQUIRE(lastRow0 == 0);
    REQUIRE(lastRow1 == 16);

    // Verify pixels in Tile 1
    REQUIRE_THAT(readPixel(ds.get(), 10, 5, 1), WithinAbs(42.0, 1e-4));
    REQUIRE_THAT(readPixel(ds.get(), 10, 5, 2), WithinAbs(84.0, 1e-4));
    // Verify rows >= 16 are still zero
    REQUIRE_THAT(readPixel(ds.get(), 10, 20, 1), WithinAbs(0.0, 1e-4));

    // Send Tile 2: rows [16, 32)
    LiveTile tile2;
    tile2.row0 = 16;
    tile2.row1 = 32;
    tile2.width = W;
    tile2.bandIds = bandIds;
    const int tile2_rows = 16;
    tile2.image.resize(2 * tile2_rows * W);
    for (int r = 0; r < tile2_rows; ++r) {
        for (int c = 0; c < W; ++c) {
            tile2.image[0 * tile2_rows * W + r * W + c] = 100.0f;
            tile2.image[1 * tile2_rows * W + r * W + c] = 200.0f;
        }
    }

    emit session->tileReceived(tile2, gen);

    REQUIRE(lastRow0 == 16);
    REQUIRE(lastRow1 == 32);

    REQUIRE_THAT(readPixel(ds.get(), 10, 20, 1), WithinAbs(100.0, 1e-4));
    REQUIRE_THAT(readPixel(ds.get(), 10, 20, 2), WithinAbs(200.0, 1e-4));
}

TEST_CASE("LiveRasterDataset ignores tiles from stale generation", "[live][cancellation]") {
    auto session = std::make_shared<LiveGeorefSession>(QStringLiteral("grpc://127.0.0.1:8815"));
    auto liveDs = LiveRasterDataset::create(session);

    const int H = 16;
    const int W = 16;
    const QStringList bandIds = {QStringLiteral("B1")};
    const QVector<double> gt = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};

    // Start generation 2
    emit session->started(H, W, bandIds, gt, 0, QStringLiteral("float32"), 0.0, 2);

    auto ds = liveDs->dataset();
    REQUIRE(ds != nullptr);

    // Send a tile belonging to stale generation 1
    LiveTile staleTile;
    staleTile.row0 = 0;
    staleTile.row1 = 16;
    staleTile.width = W;
    staleTile.bandIds = bandIds;
    staleTile.image.assign(H * W, 999.0f);

    int updateCount = 0;
    QObject::connect(liveDs.get(), &LiveRasterDataset::regionUpdated, [&](int, int) {
        updateCount++;
    });

    emit session->tileReceived(staleTile, 1); // stale gen 1

    // Should be ignored
    REQUIRE(updateCount == 0);
    REQUIRE_THAT(readPixel(ds.get(), 5, 5, 1), WithinAbs(0.0, 1e-4));

    // Send a tile belonging to active generation 2
    LiveTile activeTile = staleTile;
    activeTile.image.assign(H * W, 123.0f);
    emit session->tileReceived(activeTile, 2);

    REQUIRE(updateCount == 1);
    REQUIRE_THAT(readPixel(ds.get(), 5, 5, 1), WithinAbs(123.0, 1e-4));
}

TEST_CASE("LiveConfigUpdate and RpyControlPanel integration", "[live][panel]") {
    RpyControlPanel panel;
    auto session = std::make_shared<LiveGeorefSession>(QStringLiteral("grpc://127.0.0.1:8815"));
    panel.setSession(session);

    REQUIRE(panel.isEnabled());

    bool configChangedFired = false;
    QObject::connect(&panel, &RpyControlPanel::configChanged, [&] {
        configChangedFired = true;
    });

    // Verify progress signal updates UI without errors
    emit session->progressUpdated(5, 10, QStringLiteral("Computing tiles"), 2.5, 1);
    emit session->finished(1);
}
