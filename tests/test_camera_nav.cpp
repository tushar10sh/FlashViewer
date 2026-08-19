// Phase 3 — Navigation/camera (FR-NAV-1…5) and two inspection-style render
// requirements that need no GL context:
//   TC-NAV-01 pan, TC-NAV-02 zoom-at-cursor, TC-NAV-03 fit-all,
//   TC-NAV-04 fit-single, TC-NAV-05 screen↔geo round-trip,
//   TC-RND-07 bounded repaint budget, TC-RND-09 MSAA 4× surface format.
// All pure logic — no offscreen GL — so these run on every CI lane.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/Camera.hpp"
#include "render/MapCanvas.hpp"      // fvRepaintWithinBudget, kRepaintBudgetMs
#include "render/SurfaceFormat.hpp"  // fvDefaultSurfaceFormat

#include <QSurfaceFormat>

using Catch::Matchers::WithinAbs;
static constexpr double kEps = 1e-6;

// A camera with a known viewport and a known center/scale (set via fitToExtent).
static Camera makeCamera(const Extent& ext, int vp_w = 800, int vp_h = 600) {
    Camera cam;
    cam.setViewportSize(vp_w, vp_h);
    cam.fitToExtent(ext);
    return cam;
}

TEST_CASE("TC-NAV-01 pan shifts the camera center by delta*scale", "[nav][camera]") {
    Camera cam = makeCamera({0, 0, 100, 100}, 100, 100);
    const double scale  = cam.scale();
    const auto   before = cam.center();

    cam.pan(10.0, -4.0);   // +x screen pixels, -y screen pixels
    // pan(): center.x -= dx*scale ; center.y += dy*scale (y flipped)
    REQUIRE_THAT(cam.center().x, WithinAbs(before.x - 10.0 * scale, kEps));
    REQUIRE_THAT(cam.center().y, WithinAbs(before.y + (-4.0) * scale, kEps));
}

TEST_CASE("TC-NAV-02 wheel zoom keeps the geo point under the cursor fixed", "[nav][camera]") {
    Camera cam = makeCamera({-50, -50, 50, 50});
    const double sx = 220.0, sy = 410.0;   // arbitrary cursor inside the viewport

    const double scale_before = cam.scale();
    const auto   geo_before   = cam.screenToGeo(sx, sy);
    cam.zoom(2.0, sx, sy);                  // zoom in centered on the cursor
    const auto   geo_after    = cam.screenToGeo(sx, sy);

    REQUIRE_THAT(geo_after.x, WithinAbs(geo_before.x, 1e-9));
    REQUIRE_THAT(geo_after.y, WithinAbs(geo_before.y, 1e-9));
    REQUIRE(cam.scale() < scale_before);   // geo-units-per-pixel decreased (zoomed in)
}

// TC-NAV-06 — the world view is drawn as a world MAP: prime meridian centred, western
// hemisphere on the left, eastern on the right. Guards the orientation itself (an axis
// flip or an off-centre world extent would break it), independently of how MapCanvas
// happens to apply it.
TEST_CASE("TC-NAV-06 world view centres the prime meridian, west left / east right",
          "[nav][camera]") {
    Camera cam;
    cam.setViewportSize(1200, 700);
    cam.fitToExtent(Extent{-180.0, -85.0, 180.0, 85.0});   // MapCanvas::worldExtent()

    // Centred on lon 0 (Greenwich), not on the antimeridian.
    CHECK_THAT(cam.center().x, Catch::Matchers::WithinAbs(0.0, 1e-9));
    CHECK_THAT(cam.center().y, Catch::Matchers::WithinAbs(0.0, 1e-9));

    const double mid = 1200 / 2.0;
    const double us    = cam.geoToScreen(-98.0,  39.0).x;   // continental US
    const double india = cam.geoToScreen( 78.0,  21.0).x;   // India
    const double gmt   = cam.geoToScreen(  0.0,   0.0).x;

    CHECK(us < gmt);        // negative longitudes to the LEFT of centre
    CHECK(india > gmt);     // positive longitudes to the RIGHT of centre
    CHECK(us < india);
    CHECK_THAT(gmt, Catch::Matchers::WithinAbs(mid, 1e-6));

    // The whole ±180 range is on screen (fitToExtent pads by 5%), so a world view shows
    // one world rather than running off either edge.
    const Extent vis = cam.visibleExtent();
    CHECK(vis.xmin <= -180.0);
    CHECK(vis.xmax >=  180.0);
}

TEST_CASE("TC-NAV-03 fit-all shows the union of all layer extents", "[nav][camera]") {
    const Extent a{0, 0, 10, 10};
    const Extent b{40, 30, 60, 50};
    Camera cam = makeCamera(a.united(b));

    const Extent vis = cam.visibleExtent();
    for (const Extent& e : {a, b}) {
        CHECK(vis.contains(e.xmin, e.ymin));
        CHECK(vis.contains(e.xmax, e.ymax));
    }
}

TEST_CASE("TC-NAV-04 fit-single centers on and fills the viewport with the layer", "[nav][camera]") {
    const Extent ext{100, 200, 300, 400};   // 200×200 square
    Camera cam = makeCamera(ext, 500, 500);  // square viewport

    REQUIRE_THAT(cam.center().x, WithinAbs(ext.center().x, kEps));
    REQUIRE_THAT(cam.center().y, WithinAbs(ext.center().y, kEps));

    // fitToExtent fits with 5% padding, so the view spans extent×1.05.
    const Extent vis = cam.visibleExtent();
    REQUIRE_THAT(vis.width(),  WithinAbs(ext.width()  * 1.05, 1e-6));
    REQUIRE_THAT(vis.height(), WithinAbs(ext.height() * 1.05, 1e-6));
    CHECK(vis.contains(ext.xmin, ext.ymin));
    CHECK(vis.contains(ext.xmax, ext.ymax));
}

TEST_CASE("TC-NAV-05 screen-geo round-trip is exact within 1e-6", "[nav][camera]") {
    Camera cam = makeCamera({-180, -90, 180, 90}, 1024, 512);

    for (auto p : {std::pair{12.0, 34.0}, std::pair{1000.0, 7.0}, std::pair{512.0, 256.0}}) {
        auto geo    = cam.screenToGeo(p.first, p.second);
        auto screen = cam.geoToScreen(geo.x, geo.y);
        REQUIRE_THAT(screen.x, WithinAbs(p.first,  kEps));
        REQUIRE_THAT(screen.y, WithinAbs(p.second, kEps));
    }

    // geo → screen → geo as well
    auto s   = cam.geoToScreen(45.0, -12.0);
    auto geo = cam.screenToGeo(s.x, s.y);
    REQUIRE_THAT(geo.x, WithinAbs(45.0,  kEps));
    REQUIRE_THAT(geo.y, WithinAbs(-12.0, kEps));
}

TEST_CASE("TC-RND-07 progressive refresh respects the bounded budget", "[render][timer]") {
    CHECK(fvRepaintWithinBudget(0));
    CHECK(fvRepaintWithinBudget(kRepaintBudgetMs - 1));
    CHECK(fvRepaintWithinBudget(kRepaintBudgetMs));        // boundary: still within
    CHECK_FALSE(fvRepaintWithinBudget(kRepaintBudgetMs + 1));
    CHECK(kRepaintBudgetMs <= 30000);                      // SRS: ≤ 30 s
}

TEST_CASE("TC-RND-09 default surface format requests MSAA 4x and GL 4.1 Core", "[render][msaa]") {
    const QSurfaceFormat fmt = fvDefaultSurfaceFormat(/*cpuMode=*/false);
    CHECK(fmt.samples() == 4);                             // FR-RND-6
    CHECK(fmt.majorVersion() == 4);
    CHECK(fmt.minorVersion() == 1);
    CHECK(fmt.profile() == QSurfaceFormat::CoreProfile);   // FR-APP-7 / DC-2
    CHECK(fmt.depthBufferSize() >= 24);

    // In CPU-only / software rasterizer mode, samples are 0 (no MSAA) for performance & X11 compatibility
    const QSurfaceFormat cpuFmt = fvDefaultSurfaceFormat(/*cpuMode=*/true);
    CHECK(cpuFmt.samples() == 0);
    CHECK(cpuFmt.majorVersion() == 4);
    CHECK(cpuFmt.profile() == QSurfaceFormat::CoreProfile);
}

#include "app/CommandLineOptions.hpp"

TEST_CASE("CommandLineOptions: parsing --cpu and arguments", "[app][cli]") {
    {
        char* argv[] = { const_cast<char*>("FlashViewer"), const_cast<char*>("--cpu") };
        auto opts = fv::parseCommandLine(2, argv);
        CHECK(opts.cpuMode == true);
        CHECK(opts.showHelp == false);
        CHECK(opts.filesToOpen.isEmpty());
    }

    {
        char* argv[] = { const_cast<char*>("FlashViewer"), const_cast<char*>("-c"), const_cast<char*>("raster.tif") };
        auto opts = fv::parseCommandLine(3, argv);
        CHECK(opts.cpuMode == true);
        CHECK(opts.filesToOpen.size() == 1);
        CHECK(opts.filesToOpen.first() == "raster.tif");
    }

    {
        char* argv[] = { const_cast<char*>("FlashViewer"), const_cast<char*>("--software-gl") };
        auto opts = fv::parseCommandLine(2, argv);
        CHECK(opts.cpuMode == true);
    }

    {
        char* argv[] = { const_cast<char*>("FlashViewer"), const_cast<char*>("--help") };
        auto opts = fv::parseCommandLine(2, argv);
        CHECK(opts.showHelp == true);
    }

    {
        char* argv[] = { const_cast<char*>("FlashViewer"), const_cast<char*>("--version") };
        auto opts = fv::parseCommandLine(2, argv);
        CHECK(opts.showVersion == true);
    }
}

