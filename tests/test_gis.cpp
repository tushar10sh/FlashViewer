// Phase 7 — GIS overlays & inspection (FR-GIS-1/3). The scale math is factored into free,
// header-only helpers (gis/GeoScale.hpp) so it is unit-testable without a QWidget/GL context.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "gis/GeoScale.hpp"

#include <cmath>

using Catch::Matchers::WithinRel;

TEST_CASE("fvNiceDistance rounds to 1/2/5 x 10^n buckets", "[gis][TC-GIS-01]") {
    // raw = units_per_px * maxPx; result is the largest 1/2/5 x 10^n <= raw.
    REQUIRE(fvNiceDistance(1.2, 100) == 100.0);   // raw 120  -> 100
    REQUIRE(fvNiceDistance(3.1, 100) == 200.0);   // raw 310  -> 200
    REQUIRE(fvNiceDistance(7.0, 100) == 500.0);   // raw 700  -> 500
    REQUIRE(fvNiceDistance(1.0, 100) == 100.0);   // raw 100  -> 100
    REQUIRE(fvNiceDistance(0.005, 100) == 0.5);   // raw 0.5  -> 0.5 (sub-unit)

    // Non-positive / degenerate input is safe (caller guards on 0).
    REQUIRE(fvNiceDistance(0.0, 100) == 0.0);
    REQUIRE(fvNiceDistance(-1.0, 100) == 0.0);
}

TEST_CASE("fvMetersPerPixel converts geographic scales, passes projected through",
          "[gis][TC-GIS-03]") {
    // Projected CRS: linear units assumed metres (L-1) → identity.
    REQUIRE(fvMetersPerPixel(30.0, /*geographic=*/false) == 30.0);
    REQUIRE(fvMetersPerPixel(0.5,  false) == 0.5);

    // Geographic CRS: degrees/px → metres/px via 111.32 km/deg.
    REQUIRE_THAT(fvMetersPerPixel(1.0, /*geographic=*/true),
                 WithinRel(kKmPerDegree * 1000.0));           // 1 deg/px ≈ 111 320 m/px
    REQUIRE_THAT(fvMetersPerPixel(0.001, true),
                 WithinRel(0.001 * kKmPerDegree * 1000.0));   // ≈ 111.32 m/px

    // Geographic conversion is strictly larger than the raw degree value, and monotonic.
    REQUIRE(fvMetersPerPixel(2.0, true) > fvMetersPerPixel(1.0, true));

    // Non-positive input passes through unchanged (guarded).
    REQUIRE(fvMetersPerPixel(0.0, true) == 0.0);
}

#include "gis/GeoMeasurement.hpp"

TEST_CASE("GeoMeasurement: Haversine distance between coordinates", "[gis][measurement]") {
    // 1 degree of longitude along the equator ≈ 111.195 km
    double d1 = fv::haversineDistance(0.0, 0.0, 1.0, 0.0);
    REQUIRE_THAT(d1, Catch::Matchers::WithinAbs(111195.0, 200.0));

    // Distance between London (-0.1278, 51.5074) and Paris (2.3522, 48.8566) ≈ 343.5 km
    double dParisLondon = fv::haversineDistance(-0.1278, 51.5074, 2.3522, 48.8566);
    REQUIRE_THAT(dParisLondon, Catch::Matchers::WithinAbs(343500.0, 1500.0));

    // Same point -> 0 distance
    REQUIRE(fv::haversineDistance(10.0, 20.0, 10.0, 20.0) == 0.0);

    // Polyline calculation across 3 points (A -> B -> A)
    std::vector<QPointF> pts = { QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(0.0, 0.0) };
    double polyDist = fv::calculatePolylineDistance(pts, "EPSG:4326");
    REQUIRE_THAT(polyDist, Catch::Matchers::WithinAbs(2.0 * d1, 10.0));
}

TEST_CASE("GeoMeasurement: Spherical geodesic polygon area", "[gis][measurement]") {
    // A 1x1 degree square at the equator
    std::vector<QPointF> square = {
        QPointF(0.0, 0.0),
        QPointF(1.0, 0.0),
        QPointF(1.0, 1.0),
        QPointF(0.0, 1.0)
    };

    double area = fv::calculatePolygonArea(square, "EPSG:4326");
    // Expected area ≈ 1.23e10 m² (≈ 12,300 km²)
    REQUIRE_THAT(area, Catch::Matchers::WithinAbs(1.23e10, 5e8));

    // Degenerate polygon (< 3 vertices) returns 0
    std::vector<QPointF> degen = { QPointF(0.0, 0.0), QPointF(1.0, 1.0) };
    REQUIRE(fv::calculatePolygonArea(degen, "EPSG:4326") == 0.0);
}

TEST_CASE("GeoMeasurement: Formatting helpers", "[gis][measurement]") {
    REQUIRE(fv::formatDistance(450.5) == QStringLiteral("450.50 m"));
    REQUIRE(fv::formatDistance(1250.0) == QStringLiteral("1.250 km"));

    REQUIRE(fv::formatArea(500.25) == QStringLiteral("500.25 m²"));
    REQUIRE(fv::formatArea(25000.0).startsWith(QStringLiteral("2.50 ha")));
    REQUIRE(fv::formatArea(2500000.0).startsWith(QStringLiteral("2.500 km²")));
}

