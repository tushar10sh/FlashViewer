include(Catch)

add_executable(flashviewer_tests
    # Custom Catch2 main (creates the shared QApplication)
    test_main.cpp

    # Existing unit tests
    test_settings.cpp
    test_geo_transform.cpp
    test_colormap.cpp
    test_tile_cache.cpp
    test_vector.cpp

    # Phase 0 — test-infrastructure self-tests
    test_phase0_harness.cpp

    # Phase 1 — application shell (licenses dialog / FR-APP-8)
    test_app_licenses.cpp

    # Phase 2 — raster opening/formats + no-data/NaN transparency
    test_io_formats.cpp
    test_render_nodata.cpp

    # Phase 3 — rendering & navigation
    test_camera_nav.cpp
    test_render_pipeline.cpp
    test_lod.cpp

    # Phase 3A/3B — no-data edge-bleed fix + selectable display resampling
    test_render_resampling.cpp

    # Phase 4 — percentile conversions, auto-stretch stats, colorbar re-anchor
    test_percentile.cpp

    # Phase 5 — OSM basemap: default-off, span→zoom, basemap reprojection
    test_osm.cpp

    # Phase 6.0 — per-pane layer assignment foundation (FR-PNE-4)
    test_pane.cpp

    # Phase 7 — GIS overlays: scale-bar distance + metres-per-pixel helpers (FR-GIS-1/3)
    test_gis.cpp

    # Phase 8 — Raster Math expression engine (FR-MTH)
    test_math.cpp

    # Phase 9 — GDAL ops resampling policy + no-data warp (FR-OPS-2/5, FR-ACC-3)
    test_ops.cpp

    # Phase 10 — security (UrlGuard SSRF) + error routing (ErrorReporter)
    test_security.cpp
    test_error.cpp

    # Phase 11 — on-the-fly reprojection (warpedView / readWarpedRegion + CRS helpers)
    test_crs_reproject.cpp

    # Phase 12 — performance instrumentation (frame stats / stall / latency helpers) + FR-CAP-3
    test_perf.cpp

    # Phase 18 — Layer Panel: pane grouping, multi-select removal, drag-drop reorder
    test_layer_panel.cpp

    # Phase 19 — NetCDF/HDF combined multi-band layer (-separate band-stack VRT)
    test_band_stack.cpp

    # Phase 26.5 — plot-panel scope bookkeeping and, chiefly, layer removal while a panel
    # is open (FR-ANL-4/12). Partial T-PLOT-1.
    test_plots.cpp

    # Phase 24 — coordinate-array georeferencing: fill-value masking, 1-D axis fit,
    # 2-D geolocation-array warp (FR-IO-14/FR-IO-15)
    test_geoloc.cpp

    # Resource and system metrics
    test_system_metrics.cpp
    test_snr_mtf_filter.cpp

    # Shared test harnesses / fixtures
    fixtures/FixtureFactory.cpp
    harness/GdalOracle.cpp
    harness/MockNam.cpp
    harness/GlTestHarness.cpp

    # Sources under test (no shared lib yet)
    "${CMAKE_SOURCE_DIR}/src/app/Settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/AboutLicensesDialog.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/ColormapRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/TileCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/RasterLayer.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/VectorLayer.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/LayerManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/panels/HistogramPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/panels/GpuMonitorPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/panels/VectorLayerPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/panels/SnrToolPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/panels/MtfToolPanel.cpp"
    # LayerPanel + its icon button: TC-LYR-18/19 drive the real panel widget, because the
    # selection bookkeeping they pin lives in rebuildList, not in LayerGrouping's free
    # functions.
    "${CMAKE_SOURCE_DIR}/src/panels/LayerPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/panels/NumericDumpPanel.cpp"
    # Both plot panels + the chart furniture they share: test_plots.cpp drives the real
    # widgets, because the crash it pins lives in the removal path, not in a free function.
    "${CMAKE_SOURCE_DIR}/src/plots/SpectralPlotPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/plots/ScanPixProfilePanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/widgets/ChartTools.cpp"
    "${CMAKE_SOURCE_DIR}/src/gis/PixelSampler.cpp"
    "${CMAKE_SOURCE_DIR}/src/widgets/SvgIconButton.cpp"
    "${CMAKE_SOURCE_DIR}/src/widgets/UiKit.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/SyncGroup.cpp"
    "${CMAKE_SOURCE_DIR}/src/math/ExpressionEngine.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/RasterDataset.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/VectorDataset.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/DatasetFactory.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/PyramidBuilder.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/CloudReader.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/UrlGuard.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/BinaryRasterParser.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/BandStackVrt.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/GeolocArrays.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/GlslProgram.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/Camera.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/TileRenderer.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/VectorRenderer.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/SurfaceFormat.cpp"
    "${CMAKE_SOURCE_DIR}/src/render/OsmTileRenderer.cpp"
    "${CMAKE_SOURCE_DIR}/src/io/OsmTileProvider.cpp"
    "${CMAKE_SOURCE_DIR}/src/util/Logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/util/ErrorReporter.cpp"
    "${CMAKE_SOURCE_DIR}/src/util/PerfMetrics.cpp"
    "${CMAKE_SOURCE_DIR}/src/util/SystemMetrics.cpp"
)

# Phase 12 — mirror the app's instrumentation gate so the perf lane can build the
# tests with FV_PERF_INSTRUMENT=ON. The [perf][logic] cases run unconditionally.
if(FV_PERF_INSTRUMENT)
    target_compile_definitions(flashviewer_tests PRIVATE FV_PERF_INSTRUMENT)
endif()

# Phase 24 — lets the HIDDEN real-data case (TC-IO-26, tag `[.geoloc-realdata]`) find
# `sample_data/` regardless of where the test binary is run from. Hidden means neither CI
# nor a plain `ctest` depends on the sample file; it is opt-in via the tag.
target_compile_definitions(flashviewer_tests PRIVATE
    FV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# FR-APP-8 — embed the license manifest into the test binary so
# AboutLicensesDialog::licenseManifest() can load it from the qrc (TC-APP-08).
qt_add_resources(flashviewer_tests "test_licenses"
    PREFIX "/licenses"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/THIRD_PARTY_LICENSES.md"
)

target_include_directories(flashviewer_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}"        # tests/  → "fixtures/..", "harness/.."
)

target_link_libraries(flashviewer_tests PRIVATE
    Catch2::Catch2                       # NOT WithMain — test_main.cpp provides main()
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Svg                             # UiKit's sync-badge glyph renderer
    Qt6::Charts                          # the plot panels under test_plots.cpp
    spdlog::spdlog
    glm::glm
    nlohmann_json::nlohmann_json
    ${FV_GDAL_TARGET}
    ${FV_MUPARSER_TARGET}
)

set_target_properties(flashviewer_tests PROPERTIES
    AUTOMOC ON
)

# SKIP_RETURN_CODE must be set explicitly — do not rely on Catch2 setting it.
#
# catch_discover_tests() registers ONE CTest test per Catch2 test case, so a case
# that SKIPs is a run in which *every* selected test was skipped. Catch2 exits 4
# ("no tests ran") for such a run. Catch2's own Catch.cmake only started emitting
# `SKIP_RETURN_CODE 4` on the discovered tests in v3.5.0; on older Catch2 the
# property is absent and CTest scores that exit 4 as a hard failure.
#
# That is exactly what broke the Linux lanes: Ubuntu 24.04 ships catch2 3.4.0, so
# the three unconditionally-skipped [perf][rhp] cases (TC-PERF-01/07, 03/04, 05,
# which need FV_PERF_RHP) reported ***Failed, while Windows/macOS — on Catch2
# >= 3.5 via conda-forge/Homebrew — reported them as Skipped and stayed green.
#
# Setting it here fixes every conditional SKIP in the suite too (offscreen GL,
# netCDF driver, PROJ data), which would fail the same way on a runner lacking
# those. On Catch2 >= 3.5 this merely restates the value Catch2 already sets.
catch_discover_tests(flashviewer_tests
    PROPERTIES SKIP_RETURN_CODE 4
)
