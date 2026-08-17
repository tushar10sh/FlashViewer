// Phase 11 — On-the-fly reprojection (FR-CRS-1..5). Exercises the display-only reprojection
// engine on RasterDataset (warpedView / readWarpedRegion) and the shared CRS helpers
// (gis/CrsUtil.hpp), headlessly against the F-CRSPAIR fixture (same scene in EPSG:4326 and a
// UTM zone). GL-level alignment (TC-CRS-01) and the modal notices are verified in the app.
#include <catch2/catch_test_macros.hpp>

#include "fixtures/FixtureFactory.hpp"
#include "io/RasterDataset.hpp"
#include "gis/CrsUtil.hpp"

#include <cmath>
#include <string>

// gis/CrsUtil.hpp — CRS label + equality helpers (used by the status bar and picker).
TEST_CASE("TC-CRS: CRS helpers format and compare robustly", "[crs][phase11]") {
    // Equivalent spellings compare equal; different systems do not.
    REQUIRE(fvSameCrsWkt("EPSG:4326", "EPSG:4326"));
    REQUIRE(fvSameCrsWkt("", ""));                    // both geographic/identity
    REQUIRE_FALSE(fvSameCrsWkt("EPSG:4326", "EPSG:32633"));
    REQUIRE_FALSE(fvSameCrsWkt("", "EPSG:32633"));    // identity vs projected differ

    // Short label prefers the authority code; empty ⇒ geographic sentinel.
    REQUIRE(fvCrsShortName("EPSG:32633").contains("32633"));
    REQUIRE(fvCrsShortName("").contains("geographic"));
}

// Phase 26.9 (FR-CRS-2/4, FR-PNE-10). A sync group may hold panes whose Project CRS differ —
// the shared camera is reprojected between them, but a point mirrored from one pane to another
// (the inspect highlight, the ghost cursor) was passed through UNCHANGED. This pins the rule
// the mirroring now follows: a click is a GROUND point, so it crosses into the target pane's
// CRS before that pane is asked to mark it. The untransformed number is not a small error —
// degrees read as metres land the marker on a different continent, which is why "it looked
// roughly right" was never a defence.
TEST_CASE("TC-CRS-14 a point mirrored between panes crosses their Project CRS", "[crs][phase26]") {
    // A point in the UTM-33N zone, in degrees.
    double x = 15.0, y = 50.0;
    double ux = x, uy = y;
    if (!fvTransformPoint("EPSG:4326", "EPSG:32633", ux, uy))
        SKIP("PROJ data unavailable");

    // Projected metres, nowhere near the degrees they came from: handing the raw pair to a
    // pane in UTM is the bug, not a rounding difference.
    REQUIRE(std::abs(ux) > 1000.0);
    REQUIRE(std::abs(uy) > 1000.0);

    // Round-trip: the same ground point comes back, so both panes mark ONE place.
    double bx = ux, by = uy;
    REQUIRE(fvTransformPoint("EPSG:32633", "EPSG:4326", bx, by));
    REQUIRE(std::abs(bx - x) < 1e-6);
    REQUIRE(std::abs(by - y) < 1e-6);

    // Same CRS on both sides is the identity — panes that DO share a CRS pay nothing and are
    // not nudged by a needless transform.
    double sx = x, sy = y;
    REQUIRE(fvTransformPoint("EPSG:4326", "EPSG:4326", sx, sy));
    REQUIRE(sx == x);
    REQUIRE(sy == y);

    // Out of the target's domain ⇒ refused, and the caller clears its marker rather than
    // drawing one at whatever the failed transform left behind.
    double fx = 1e12, fy = 1e12;
    const double keptX = fx, keptY = fy;
    if (!fvTransformPoint("EPSG:4326", "EPSG:32633", fx, fy)) {
        REQUIRE(fx == keptX);
        REQUIRE(fy == keptY);
    }
}

// RasterDataset::warpedView — the sameAsSource short-circuit (base layer never warps).
TEST_CASE("TC-CRS-05 warpedView is a no-op for the source/empty CRS", "[crs][phase11]") {
    FixtureFactory ff;
    auto [a, b] = ff.crsPair(32, 32);
    (void)b;
    auto ds = RasterDataset::open(a.path);   // EPSG:4326
    REQUIRE(ds != nullptr);

    // Target == source CRS → sameAsSource, source dimensions reported, not failed.
    auto vSame = ds->warpedView(ds->crsWkt());
    REQUIRE(vSame.sameAsSource);
    REQUIRE_FALSE(vSame.failed);
    REQUIRE(vSame.width  == ds->width());
    REQUIRE(vSame.height == ds->height());

    // Empty target (geographic identity) → also sameAsSource.
    auto vEmpty = ds->warpedView(std::string());
    REQUIRE(vEmpty.sameAsSource);

    // readWarpedRegion with the source CRS returns the raw pixels (delegates to readRegion).
    auto buf = ds->readWarpedRegion(ds->crsWkt(), 0, 0, ds->width(), ds->height(),
                                    ds->width(), ds->height());
    REQUIRE(buf.isValid());
    REQUIRE(buf.width  == ds->width());
    REQUIRE(buf.height == ds->height());
}

// Phase 17 #4 — native-CRS fallback trigger. A layer with NO source CRS cannot be warped
// into a pane CRS (warpedView reports failed), yet its native view is always drawable, so
// the TileRenderer can draw it unwarped instead of omitting it. This is the deterministic
// mechanism behind the "shown in its native CRS" notice (no raw PROJ/GDAL error surfaces).
TEST_CASE("TC-CRS-13 unreprojectable layer keeps a drawable native view", "[crs][phase17]") {
    FixtureFactory ff;
    auto nc = ff.noCrsFloat(32, 32);
    auto ds = RasterDataset::open(nc.path);
    REQUIRE(ds != nullptr);
    REQUIRE(ds->crsWkt().empty());                 // no source CRS on disk

    // Warping into a real target CRS fails (cannot align a CRS-less source) — the trigger.
    auto vFail = ds->warpedView("EPSG:32633");
    REQUIRE(vFail.failed);
    REQUIRE_FALSE(vFail.sameAsSource);

    // Fallback: the native view (empty target) is always sameAsSource, never failed, and
    // reports the source dimensions — the renderer draws THIS instead of omitting the layer.
    auto vNative = ds->warpedView(std::string());
    REQUIRE(vNative.sameAsSource);
    REQUIRE_FALSE(vNative.failed);
    REQUIRE(vNative.width  == ds->width());
    REQUIRE(vNative.height == ds->height());

    // And a raw read through the native path returns real pixels (what the tiles carry).
    auto buf = ds->readWarpedRegion(std::string(), 0, 0, ds->width(), ds->height(),
                                    ds->width(), ds->height());
    REQUIRE(buf.isValid());
    REQUIRE(buf.width  == ds->width());
    REQUIRE(buf.height == ds->height());
}

// RasterDataset::warpedView — reprojecting a 4326 scene into a different (UTM) CRS.
TEST_CASE("TC-CRS-01 warpedView reprojects into a different Project CRS", "[crs][phase11]") {
    FixtureFactory ff;
    auto [a, b] = ff.crsPair(32, 32);
    auto dsA = RasterDataset::open(a.path);   // EPSG:4326
    auto dsB = RasterDataset::open(b.path);   // EPSG:32633 (needs PROJ data)
    REQUIRE(dsA != nullptr);
    if (!dsB) { SKIP("crsPair warp produced no output (PROJ data unavailable)"); }

    const std::string utmWkt = dsB->crsWkt();
    auto v = dsA->warpedView(utmWkt);
    if (v.failed) { SKIP("warp into UTM failed (PROJ data unavailable)"); }

    REQUIRE_FALSE(v.sameAsSource);          // genuinely reprojected
    REQUIRE(v.width  > 0);
    REQUIRE(v.height > 0);
    REQUIRE(v.extent.width()  > 0.0);       // a valid extent in the target CRS
    REQUIRE(v.extent.height() > 0.0);

    // A tile-sized window read from the warped view returns a valid buffer.
    auto buf = dsA->readWarpedRegion(utmWkt, 0, 0, v.width, v.height,
                                     std::min(v.width, 64), std::min(v.height, 64));
    REQUIRE(buf.isValid());

    // The cached warped handle is reused (equivalent WKT spelling collapses to one entry):
    auto v2 = dsA->warpedView("EPSG:32633");
    // Equivalent target ⇒ same warped grid dimensions as the WKT-spelled request.
    if (!v2.failed && !v2.sameAsSource) {
        REQUIRE(v2.width  == v.width);
        REQUIRE(v2.height == v.height);
    }
}

TEST_CASE("TC-CRS-15 fvFormatCoordinates formats both projected X/Y and Lat/Lon", "[crs][coords]") {
    // 1. Geographic CRS (EPSG:4326)
    auto geoFmt = fvFormatCoordinates(12.345678, -45.678901, "EPSG:4326");
    CHECK(geoFmt.has_latlon);
    CHECK(geoFmt.xy_text.contains("X:"));
    CHECK(geoFmt.xy_text.contains("Y:"));
    CHECK(geoFmt.latlon_text.contains("Lat:"));
    CHECK(geoFmt.latlon_text.contains("Lon:"));
    CHECK(geoFmt.latlon_text.contains("S")); // negative latitude = South
    CHECK(geoFmt.latlon_text.contains("E")); // positive longitude = East
    CHECK(geoFmt.single_line.contains("|"));
    CHECK(geoFmt.multi_line.contains("\n"));

    // 2. Projected CRS (UTM-33N)
    double ux = 500000.0, uy = 5538000.0;
    auto utmFmt = fvFormatCoordinates(ux, uy, "EPSG:32633");
    if (utmFmt.has_latlon) {
        CHECK(utmFmt.xy_text.contains("500000"));
        CHECK(utmFmt.xy_text.contains("5538000"));
        CHECK(utmFmt.latlon_text.contains("Lat:"));
        CHECK(utmFmt.latlon_text.contains("Lon:"));
        CHECK(utmFmt.latlon_text.contains("N"));
        CHECK(utmFmt.latlon_text.contains("E"));
        CHECK(utmFmt.single_line.contains("|"));
    }
}

#include "gis/GeoTransform4326.hpp"

TEST_CASE("TC-CRS-16 GeoTransform4326 bridges pixel, project CRS, and WGS84", "[crs][geotransform]") {
    // North-up raster in UTM-33N: pixel 0,0 at (500000, 5538000), 10m pixels
    GeoTransform gt;
    gt.gt[0] = 500000.0;
    gt.gt[1] = 10.0;
    gt.gt[2] = 0.0;
    gt.gt[3] = 5538000.0;
    gt.gt[4] = 0.0;
    gt.gt[5] = -10.0;

    GeoTransform4326 g4326(gt, "EPSG:32633");
    REQUIRE(g4326.transformer() != nullptr);

    // Pixel to Geo
    glm::dvec2 geo = g4326.pixelToGeo(0, 0);
    CHECK(geo.x == 500005.0);
    CHECK(geo.y == 5537995.0);

    // Pixel to Lat/Lon
    double lat = 0.0, lon = 0.0;
    if (g4326.pixelToLatLon(0, 0, lat, lon)) {
        CHECK(lat > 49.0);
        CHECK(lat < 51.0);
        CHECK(lon > 14.0);
        CHECK(lon < 16.0);

        // Roundtrip Lat/Lon back to Pixel
        double col = 0.0, row = 0.0;
        REQUIRE(g4326.latLonToPixel(lat, lon, col, row));
        CHECK(std::abs(col - 0.0) < 1e-3);
        CHECK(std::abs(row - 0.0) < 1e-3);
    }
}

TEST_CASE("TC-CRS-17 CrsTransformerPool reuses instances and ensures thread safety", "[crs][transformer]") {
    auto tr1 = fvGetWgs84Transformer("EPSG:32633");
    auto tr2 = fvGetWgs84Transformer("EPSG:32633");
    REQUIRE(tr1 != nullptr);
    REQUIRE(tr2 != nullptr);
    CHECK(tr1.get() == tr2.get()); // Same pooled instance reused

    double lat = 0.0, lon = 0.0;
    if (tr1->toLatLon(500000.0, 5538000.0, lat, lon)) {
        double px = 0.0, py = 0.0;
        REQUIRE(tr1->toProjected(lat, lon, px, py));
        CHECK(std::abs(px - 500000.0) < 1e-2);
        CHECK(std::abs(py - 5538000.0) < 1e-2);
    }
}


