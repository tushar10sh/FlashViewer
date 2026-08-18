// Phase 5 — OSM basemap: default-off, geographic-span zoom, and basemap reprojection.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/OsmTileRenderer.hpp"

#include <ogr_spatialref.h>
#include <cmath>
#include <numbers>
#include <algorithm>
#include <utility>

// TC-OSM-09 — fvOsmZoomForSpan (pure): wider span ⇒ lower zoom; clamped to [0,19].
TEST_CASE("OSM zoom decreases with wider geographic span", "[osm][TC-OSM-09]") {
    const int vp = 1024;

    // A ~360° span (whole world) should sit at a very low zoom; a tiny span high.
    int z_world = fvOsmZoomForSpan(360.0, vp);
    int z_city  = fvOsmZoomForSpan(0.05, vp);   // ~5 km across
    REQUIRE(z_world < z_city);

    // Monotonic: narrower span never yields a lower zoom.
    int prev = -1;
    for (double w : {180.0, 90.0, 10.0, 1.0, 0.1, 0.01}) {
        int z = fvOsmZoomForSpan(w, vp);
        REQUIRE(z >= prev);
        prev = z;
    }

    // Clamped to [0,19] and degenerate inputs are safe.
    REQUIRE(fvOsmZoomForSpan(1e-9, vp) == 19);
    REQUIRE(fvOsmZoomForSpan(5000.0, vp) == 0);   // span ≫ world ⇒ clamped to 0
    REQUIRE(fvOsmZoomForSpan(0.0, vp) == 0);
    REQUIRE(fvOsmZoomForSpan(10.0, 0) == 0);
}

// TC-OSM-10 — fvOsmWrapCopyRange (pure): which world copies a visible span needs.
// Copy k spans [-180 + 360k, 180 + 360k]; k = 0 is the canonical world.
TEST_CASE("OSM wrap copy range covers spans past the antimeridian", "[osm][TC-OSM-10]") {
    using Catch::Matchers::WithinAbs;

    // Wholly inside the canonical world ⇒ that copy alone.
    REQUIRE(fvOsmWrapCopyRange(-10.0, 10.0)   == std::pair{0, 0});
    REQUIRE(fvOsmWrapCopyRange(-180.0, 180.0) == std::pair{0, 1});   // touches the k=1 seam

    // Panning EAST past +180 pulls in the copy to the east; 0..360 spans both.
    REQUIRE(fvOsmWrapCopyRange(170.0, 190.0) == std::pair{0, 1});
    REQUIRE(fvOsmWrapCopyRange(0.0,   360.0) == std::pair{0, 1});
    REQUIRE(fvOsmWrapCopyRange(200.0, 300.0) == std::pair{1, 1});    // entirely in copy 1

    // Panning WEST past -180 pulls in the copy to the west; 0..-360 spans both. This is
    // the case plain truncation gets wrong: -190 must floor into copy -1, not copy 0.
    REQUIRE(fvOsmWrapCopyRange(-190.0, -170.0) == std::pair{-1, 0});
    REQUIRE(fvOsmWrapCopyRange(-360.0,    0.0) == std::pair{-1, 0});
    REQUIRE(fvOsmWrapCopyRange(-300.0, -200.0) == std::pair{-1, -1});

    // A span crossing BOTH edges asks for all three copies.
    REQUIRE(fvOsmWrapCopyRange(-200.0, 200.0) == std::pair{-1, 1});

    // Degenerate / non-finite spans are safe and never widen the loop.
    REQUIRE(fvOsmWrapCopyRange(10.0, 10.0)  == std::pair{0, 0});   // empty
    REQUIRE(fvOsmWrapCopyRange(10.0, -10.0) == std::pair{0, 0});   // inverted
    REQUIRE(fvOsmWrapCopyRange(std::nan(""), 10.0) == std::pair{0, 0});

    // Absurdly wide spans are capped, so one frame can never draw unbounded copies.
    const auto huge = fvOsmWrapCopyRange(-100000.0, 100000.0);
    REQUIRE(huge.second - huge.first + 1 == kFvOsmMaxWrapCopies);

    // Every returned range must actually contain the span's own copies (spot-check the
    // seam: +180 belongs to copy 1, and -180 to copy 0).
    REQUIRE(fvOsmWrapCopyRange(180.0, 181.0).first == 1);
    REQUIRE(fvOsmWrapCopyRange(-180.0, -179.0).first == 0);
}

// TC-OSM-11 — Mercator latitude tessellation. A tile drawn as ONE quad interpolates its
// texture linearly across a latitude range Web Mercator does not cover linearly, which at
// coarse zoom slid the basemap off a geographic raster's coastlines. This checks the row
// count actually delivers the sub-pixel budget it claims, by measuring the real
// interpolation error against the exact inverse-Mercator.
TEST_CASE("OSM tile row tessellation keeps Mercator error sub-pixel", "[osm][TC-OSM-11]") {
    // Exact latitude at normalised Mercator position t in [0,1] (t=0 is the north edge).
    auto latAt = [](double t) {
        return std::atan(std::sinh(std::numbers::pi * (1.0 - 2.0 * t))) * 180.0 / std::numbers::pi;
    };

    // Coarser zoom must never ask for fewer rows than finer zoom, and the count is bounded.
    int prev = kFvOsmMaxTileRows + 1;
    for (int z = 0; z <= 19; ++z) {
        const int rows = fvOsmTileRows(z);
        REQUIRE(rows >= 1);
        REQUIRE(rows <= kFvOsmMaxTileRows);
        REQUIRE(rows <= prev);            // monotonically non-increasing with zoom
        prev = rows;
    }
    REQUIRE(fvOsmTileRows(0) > 1);        // z=0 spans ~170 deg of latitude - must subdivide
    REQUIRE(fvOsmTileRows(19) == 1);      // a sliver per tile - one quad is already exact

    // The real check: worst-case displacement, in PIXELS, between the linear interpolation
    // a sub-cell performs and the true latitude, at the zoom fvOsmZoomForSpan would pick.
    for (int z = 0; z <= 10; ++z) {
        const double n     = std::pow(2.0, z);
        const int    rows  = fvOsmTileRows(z);
        const double deg_px = 360.0 / (256.0 * n);   // geographic camera at ~256 px/tile
        double worst_px = 0.0;

        // Sample every tile row, and every sub-cell within it, across the whole grid.
        for (int ty = 0; ty < static_cast<int>(n); ++ty) {
            for (int r = 0; r < rows; ++r) {
                const double t0 = (ty + double(r)     / rows) / n;
                const double t1 = (ty + double(r + 1) / rows) / n;
                const double lat0 = latAt(t0), lat1 = latAt(t1);
                for (int s = 1; s < 16; ++s) {           // interior of the sub-cell
                    const double f = s / 16.0;
                    const double drawn = lat0 + f * (lat1 - lat0);   // what the quad does
                    const double truth = latAt(t0 + f * (t1 - t0));  // where it belongs
                    worst_px = std::max(worst_px, std::abs(drawn - truth) / deg_px);
                }
            }
        }
        INFO("zoom " << z << " rows " << rows << " worst " << worst_px << " px");
        REQUIRE(worst_px < 1.0);
    }

    // And the regression itself: with a SINGLE quad per tile, z=0 is wildly off - which is
    // exactly the bug. Confirms the test above would catch a revert.
    double single_quad_px = 0.0;
    for (int s = 1; s < 16; ++s) {
        const double f = s / 16.0;
        const double drawn = latAt(0.0) + f * (latAt(1.0) - latAt(0.0));
        single_quad_px = std::max(single_quad_px,
                                  std::abs(drawn - latAt(f)) / (360.0 / 256.0));
    }
    REQUIRE(single_quad_px > 10.0);
}

// TC-OSM-07 — the basemap is OFF by default on a fresh renderer.
TEST_CASE("OSM basemap is disabled by default", "[osm][TC-OSM-07]") {
    OsmTileRenderer r(nullptr);
    REQUIRE(r.isEnabled() == false);
    r.setEnabled(true);
    REQUIRE(r.isEnabled() == true);

    // setProjectCrs must accept empty / geographic / projected WKT without crashing.
    r.setProjectCrs(std::string());                 // geographic (identity)
    OGRSpatialReference wgs84; wgs84.SetWellKnownGeogCS("WGS84");
    char* geoWkt = nullptr; wgs84.exportToWkt(&geoWkt);
    r.setProjectCrs(geoWkt ? geoWkt : "");
    CPLFree(geoWkt);
    OGRSpatialReference utm; utm.importFromEPSG(32633);   // UTM 33N
    char* utmWkt = nullptr; utm.exportToWkt(&utmWkt);
    r.setProjectCrs(utmWkt ? utmWkt : "");
    CPLFree(utmWkt);
    SUCCEED();
}

// TC-OSM-08 — reprojection round-trip lon/lat ↔ projected (EPSG:32633, UTM 33N).
// This mirrors what OsmTileRenderer does per tile corner.
TEST_CASE("OSM basemap reprojection round-trips into a projected CRS", "[osm][TC-OSM-08]") {
    OGRSpatialReference geo;  geo.SetWellKnownGeogCS("WGS84");
    OGRSpatialReference proj; proj.importFromEPSG(32633);
    geo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    proj.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRCoordinateTransformation* g2p = OGRCreateCoordinateTransformation(&geo, &proj);
    OGRCoordinateTransformation* p2g = OGRCreateCoordinateTransformation(&proj, &geo);
    REQUIRE(g2p != nullptr);
    REQUIRE(p2g != nullptr);

    // 15°E lies on the central meridian of UTM 33N → easting ≈ 500000 at the equator.
    double x = 15.0, y = 0.0;
    REQUIRE(g2p->Transform(1, &x, &y));
    REQUIRE_THAT(x, Catch::Matchers::WithinAbs(500000.0, 1.0));   // central-meridian easting
    REQUIRE_THAT(y, Catch::Matchers::WithinAbs(0.0, 1.0));        // northing ≈ 0 at the equator

    // Round-trip back to lon/lat.
    double lon = x, lat = y;
    REQUIRE(p2g->Transform(1, &lon, &lat));
    REQUIRE_THAT(lon, Catch::Matchers::WithinAbs(15.0, 1e-6));
    REQUIRE_THAT(lat, Catch::Matchers::WithinAbs(0.0, 1e-6));

    OGRCoordinateTransformation::DestroyCT(g2p);
    OGRCoordinateTransformation::DestroyCT(p2g);
}

TEST_CASE("OsmTileProvider validates tile URL templates correctly", "[osm][validation]") {
    QString reason;

    // Valid templates
    REQUIRE(OsmTileProvider::validateUrlTemplate("https://tile.openstreetmap.org/{z}/{x}/{y}.png", &reason));
    REQUIRE(reason.isEmpty());

    REQUIRE(OsmTileProvider::validateUrlTemplate("http://a.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png", &reason));
    REQUIRE(reason.isEmpty());

    // Empty URL
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("", &reason));
    REQUIRE_FALSE(reason.isEmpty());

    // Missing placeholders
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("https://tile.openstreetmap.org/{x}/{y}.png", &reason));
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("https://tile.openstreetmap.org/{z}/{y}.png", &reason));
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("https://tile.openstreetmap.org/{z}/{x}.png", &reason));

    // Invalid schemes
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("ftp://tile.openstreetmap.org/{z}/{x}/{y}.png", &reason));
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("file:///path/{z}/{x}/{y}.png", &reason));

    // SSRF blocked addresses
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("http://127.0.0.1/{z}/{x}/{y}.png", &reason));
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("http://192.168.1.1/{z}/{x}/{y}.png", &reason));
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("http://10.0.0.1/{z}/{x}/{y}.png", &reason));
    REQUIRE_FALSE(OsmTileProvider::validateUrlTemplate("http://169.254.169.254/{z}/{x}/{y}.png", &reason));
}

TEST_CASE("OsmTileProvider connection test rejects invalid URLs fast", "[osm][connection]") {
    auto res1 = OsmTileProvider::testConnection("ftp://invalid/{z}/{x}/{y}.png", 100);
    REQUIRE_FALSE(res1.ok);
    REQUIRE_FALSE(res1.errorString.isEmpty());

    auto res2 = OsmTileProvider::testConnection("http://127.0.0.1/{z}/{x}/{y}.png", 100);
    REQUIRE_FALSE(res2.ok);
    REQUIRE_FALSE(res2.errorString.isEmpty());
}

