find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets Charts Network Svg)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(glm REQUIRED)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
endif()

# ---------------------------------------------------------------------------
# GDAL: cmake config (conda-forge / Homebrew / GDAL 3.8+) → pkg-config fallback
# ---------------------------------------------------------------------------
find_package(GDAL CONFIG QUIET)
if(TARGET GDAL::GDAL)
    set(FV_GDAL_TARGET GDAL::GDAL)
    message(STATUS "FlashViewer: GDAL via cmake config (GDAL::GDAL)")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GDAL_PC REQUIRED IMPORTED_TARGET gdal)
    set(FV_GDAL_TARGET PkgConfig::GDAL_PC)
    message(STATUS "FlashViewer: GDAL via pkg-config (PkgConfig::GDAL_PC)")
endif()

# ---------------------------------------------------------------------------
# muParser: cmake config (conda-forge / Homebrew) → pkg-config fallback
# ---------------------------------------------------------------------------
find_package(muparser CONFIG QUIET)
if(TARGET muparser::muparser)
    set(FV_MUPARSER_TARGET muparser::muparser)
    message(STATUS "FlashViewer: muParser via cmake config (muparser::muparser)")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MUPARSER_PC QUIET IMPORTED_TARGET muparser)
    endif()
    if(TARGET PkgConfig::MUPARSER_PC)
        set(FV_MUPARSER_TARGET PkgConfig::MUPARSER_PC)
        message(STATUS "FlashViewer: muParser via pkg-config (PkgConfig::MUPARSER_PC)")
    else()
        message(FATAL_ERROR
            "muParser not found.\n"
            "  Linux:   sudo apt install libmuparser-dev\n"
            "  macOS:   brew install muparser\n"
            "  Windows: conda install -c conda-forge muparser (or add it to environment-windows.yml)\n"
            "  conda (Linux/macOS): provided by environment-linux.yml / environment-macos.yml")
    endif()
endif()

# ---------------------------------------------------------------------------
# Apache Arrow / Arrow Flight (live Arrow Flight georeferencing session --
# see src/live/, docs/LIVE_GEOREF_DEV_PLAN.md). Requires an Arrow C++ build
# with Flight enabled (conda-forge's `libarrow-flight` / `arrow-cpp` with
# ARROW_FLIGHT=ON, or Homebrew's `apache-arrow` + `apache-arrow-flight` on
# recent formulae -- verify Flight is actually compiled in, it is NOT
# enabled by every distro's default Arrow package).
# ---------------------------------------------------------------------------
find_package(Arrow CONFIG QUIET)
find_package(ArrowFlight CONFIG QUIET)
if(TARGET arrow_shared AND TARGET arrow_flight_shared)
    set(FV_ARROW_TARGETS arrow_shared arrow_flight_shared)
    message(STATUS "FlashViewer: Arrow/Arrow Flight via cmake config (arrow_shared, arrow_flight_shared)")
elseif(TARGET Arrow::arrow_shared AND TARGET ArrowFlight::arrow_flight_shared)
    set(FV_ARROW_TARGETS Arrow::arrow_shared ArrowFlight::arrow_flight_shared)
    message(STATUS "FlashViewer: Arrow/Arrow Flight via cmake config (Arrow::/ArrowFlight:: namespaced targets)")
else()
    message(FATAL_ERROR
        "Apache Arrow C++ with Flight not found.\n"
        "  conda (recommended, all platforms): conda install -c conda-forge libarrow-flight\n"
        "  macOS:   brew install apache-arrow apache-arrow-flight\n"
        "  Linux:   see https://arrow.apache.org/install/ for your distro's apt/yum repo\n"
        "  The exact CMake target names (arrow_shared/Arrow::arrow_shared,\n"
        "  arrow_flight_shared/ArrowFlight::arrow_flight_shared) vary by how Arrow was\n"
        "  packaged -- if this still fails after installing, run\n"
        "  `find_package(Arrow CONFIG)` by hand / inspect the installed ArrowConfig.cmake\n"
        "  and adjust FV_ARROW_TARGETS above to match.")
endif()

qt_standard_project_setup()
