qt_add_executable(FlashViewer
    # Entry point
    main.cpp

    # App
    app/Application.hpp  app/Application.cpp
    app/MainWindow.hpp   app/MainWindow.cpp
    app/Settings.hpp     app/Settings.cpp
    app/SettingsDialog.hpp app/SettingsDialog.cpp
    app/GpuInfoDialog.hpp app/GpuInfoDialog.cpp
    app/AboutLicensesDialog.hpp app/AboutLicensesDialog.cpp
    app/CoordAssignDialog.hpp app/CoordAssignDialog.cpp
    app/NewPaneDialog.hpp app/NewPaneDialog.cpp
    app/LayerSettingsDialog.hpp app/LayerSettingsDialog.cpp

    # Core data model
    core/Extent.hpp
    core/GeoTransform.hpp
    core/BandMapping.hpp
    core/Layer.hpp
    core/RasterLayer.hpp  core/RasterLayer.cpp
    core/VectorLayer.hpp  core/VectorLayer.cpp
    core/LayerManager.hpp core/LayerManager.cpp
    core/TileKey.hpp
    core/ThreadPool.hpp   core/ThreadPool.cpp
    core/TileCache.hpp    core/TileCache.cpp
    core/Colormap.hpp
    core/ColormapRegistry.hpp  core/ColormapRegistry.cpp

    # I/O
    io/RasterDataset.hpp    io/RasterDataset.cpp
    io/VectorDataset.hpp    io/VectorDataset.cpp
    io/DatasetFactory.hpp   io/DatasetFactory.cpp
    io/PyramidBuilder.hpp   io/PyramidBuilder.cpp
    io/BinaryRasterParser.hpp io/BinaryRasterParser.cpp
    io/BandStackVrt.hpp     io/BandStackVrt.cpp
    io/GeolocArrays.hpp     io/GeolocArrays.cpp
    io/BinaryImportDialog.hpp io/BinaryImportDialog.cpp
    io/CloudReader.hpp      io/CloudReader.cpp
    io/UrlGuard.hpp         io/UrlGuard.cpp
    io/OsmTileProvider.hpp  io/OsmTileProvider.cpp

    # Rendering
    render/GlslProgram.hpp      render/GlslProgram.cpp
    render/GlslProgramCache.hpp render/GlslProgramCache.cpp
    render/SurfaceFormat.hpp    render/SurfaceFormat.cpp
    render/Camera.hpp           render/Camera.cpp
    render/TileRenderer.hpp     render/TileRenderer.cpp
    render/VectorRenderer.hpp   render/VectorRenderer.cpp
    render/MapCanvas.hpp        render/MapCanvas.cpp
    render/SyncGroup.hpp        render/SyncGroup.cpp
    render/Pane.hpp             render/Pane.cpp
    render/PaneLayoutMode.hpp
    render/PaneSlot.hpp
    render/PaneRegion.hpp       render/PaneRegion.cpp
    render/PaneLayout.hpp       render/PaneLayout.cpp
    render/OsmTileRenderer.hpp        render/OsmTileRenderer.cpp
    render/ColormapLegend.hpp         render/ColormapLegend.cpp
    render/PixelHighlightOverlay.hpp  render/PixelHighlightOverlay.cpp
    render/PaneChrome.hpp             render/PaneChrome.cpp

    # GIS overlays / tools
    gis/ScaleBar.hpp               gis/ScaleBar.cpp
    gis/InspectTypes.hpp
    gis/PixelSampler.hpp           gis/PixelSampler.cpp
    gis/AttributeInspector.hpp     gis/AttributeInspector.cpp
    gis/GdalOpsDialog.hpp          gis/GdalOpsDialog.cpp
    gis/CrsUtil.hpp
    gis/GeoMeasurement.hpp
    gis/GeoTransform4326.hpp
    gis/CrsPickerDialog.hpp        gis/CrsPickerDialog.cpp

    # Raster Math
    math/ExpressionEngine.hpp      math/ExpressionEngine.cpp
    math/RasterMathDialog.hpp      math/RasterMathDialog.cpp

    # Panels
    panels/LayerPanel.hpp          panels/LayerPanel.cpp
    panels/MarqueeLabel.hpp        panels/MarqueeLabel.cpp
    panels/BandSelectorWidget.hpp  panels/BandSelectorWidget.cpp
    panels/ColormapSelectorWidget.hpp panels/ColormapSelectorWidget.cpp
    panels/HistogramPanel.hpp      panels/HistogramPanel.cpp
    panels/RasterInfoPanel.hpp     panels/RasterInfoPanel.cpp
    panels/NoDataWidget.hpp         panels/NoDataWidget.cpp
    panels/GpuMonitorPanel.hpp      panels/GpuMonitorPanel.cpp
    panels/NumericDumpPanel.hpp     panels/NumericDumpPanel.cpp
    panels/VectorLayerPanel.hpp     panels/VectorLayerPanel.cpp
    panels/SnrToolPanel.hpp         panels/SnrToolPanel.cpp
    panels/MtfToolPanel.hpp         panels/MtfToolPanel.cpp

    # Plots
    plots/SpectralPlotPanel.hpp    plots/SpectralPlotPanel.cpp
    plots/ScanPixProfilePanel.hpp  plots/ScanPixProfilePanel.cpp

    # Shared widgets / UI kit (used by both the panels and the dialogs)
    widgets/SvgIconButton.hpp      widgets/SvgIconButton.cpp
    widgets/UiKit.hpp              widgets/UiKit.cpp
    widgets/ChartTools.hpp         widgets/ChartTools.cpp
    widgets/CurveStyle.hpp

    # Utilities
    util/MathUtils.hpp
    util/Logger.hpp util/Logger.cpp
    util/ErrorReporter.hpp util/ErrorReporter.cpp
    util/PerfMetrics.hpp util/PerfMetrics.cpp
    util/SystemMetrics.hpp util/SystemMetrics.cpp
)

# Phase 12 — performance instrumentation. The metrics + Performance HUD (FR-APP-14)
# are always compiled (cheap, like TileCache::residentBytes); FV_PERF_INSTRUMENT only
# enables the verbose per-op logging + CI perf-lane detail at the guarded call sites.
if(FV_PERF_INSTRUMENT)
    target_compile_definitions(FlashViewer PRIVATE FV_PERF_INSTRUMENT)
endif()

qt_add_resources(FlashViewer "themes"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/themes/light.qss"
        "${CMAKE_SOURCE_DIR}/resources/themes/dark.qss"
)

qt_add_resources(FlashViewer "icons"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/icons/arrow_up_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/arrow_down_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/arrow_up_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/arrow_down_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/dock_close_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/dock_close_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/dock_float_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/dock_float_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/gear_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/gear_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/pencil_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/pencil_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/sync_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/sync_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/color_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/color_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/star_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/star_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/mirror_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/mirror_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/trash_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/trash_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/zoom_in_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/zoom_in_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/zoom_out_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/zoom_out_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/home_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/home_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/hand_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/hand_dark.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/save_light.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/save_dark.svg"
)

# FR-APP-8 / C-LIC-5 — embed the third-party license manifest (About → Licenses)
qt_add_resources(FlashViewer "licenses"
    PREFIX "/licenses"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/THIRD_PARTY_LICENSES.md"
)

target_include_directories(FlashViewer PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
)

# FV_GDAL_TARGET and FV_MUPARSER_TARGET are set in cmake/Dependencies.cmake
target_link_libraries(FlashViewer PRIVATE
    Qt6::Core Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Charts Qt6::Network Qt6::Svg
    ${FV_GDAL_TARGET}
    ${FV_MUPARSER_TARGET}
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    glm::glm
)

set_target_properties(FlashViewer PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    WIN32_EXECUTABLE  TRUE
    MACOSX_BUNDLE     TRUE
)
