#pragma once

#include <QString>
#include <QStringList>
#include <QCoreApplication>
#include <iostream>
#include "version.h"

namespace fv {

struct CommandLineOptions {
    bool cpuMode{false};
    bool showHelp{false};
    bool showVersion{false};
    QStringList filesToOpen;
};

/**
 * @brief Parses command line arguments and environment variables for FlashViewer options.
 */
inline CommandLineOptions parseCommandLine(int argc, char* const argv[]) {
    CommandLineOptions options;

    // Check environment variables first (allows setting FLASHVIEWER_CPU_ONLY=1 or LIBGL_ALWAYS_SOFTWARE=1)
    if (qEnvironmentVariableIsSet("FLASHVIEWER_CPU_ONLY") ||
        qEnvironmentVariableIsSet("FLASHVIEWER_SOFTWARE_GL") ||
        qgetenv("LIBGL_ALWAYS_SOFTWARE") == "1") {
        options.cpuMode = true;
    }

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == "--cpu" || arg == "--cpu-only" || arg == "--software-gl" ||
            arg == "--software-opengl" || arg == "-c") {
            options.cpuMode = true;
        } else if (arg == "--help" || arg == "-h" || arg == "-?") {
            options.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            options.showVersion = true;
        } else if (!arg.startsWith("-")) {
            options.filesToOpen.append(arg);
        }
    }

    return options;
}

/**
 * @brief Applies environment variables to force Mesa CPU software rasterization (llvmpipe/swrast).
 */
inline void applyCpuRenderingEnvironment() {
    qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
    qputenv("QT_XCB_FORCE_SOFTWARE_OPENGL", "1");
    qputenv("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe");
    qputenv("GALLIUM_DRIVER", "llvmpipe");
    qputenv("QT_QUICK_BACKEND", "software");
}

/**
 * @brief Prints CLI usage help.
 */
inline void printHelp(const char* progName) {
    std::cout << "FlashViewer " << FLASHVIEWER_VERSION_STRING << " - High-Performance Geospatial Viewer\n\n"
              << "Usage: " << (progName ? progName : "FlashViewer") << " [options] [files...]\n\n"
              << "Options:\n"
              << "  --cpu, --cpu-only, -c    Enable CPU-only / Software OpenGL rendering (Mesa llvmpipe).\n"
              << "                           Recommended for remote X11 redirection (ssh -X / -Y),\n"
              << "                           Docker containers without GPU passthrough, and headless servers.\n"
              << "  --software-gl            Alias for --cpu mode.\n"
              << "  -h, --help               Display this help message and exit.\n"
              << "  -v, --version            Display version information and exit.\n\n"
              << "Arguments:\n"
              << "  files...                 One or more raster (GeoTIFF, ENVI, VRT, etc.) or vector\n"
              << "                           (Shapefile, GeoJSON, etc.) files to open on launch.\n\n"
              << "Environment Variables:\n"
              << "  FLASHVIEWER_CPU_ONLY=1   Force CPU-only software rasterization mode.\n"
              << "  LIBGL_ALWAYS_SOFTWARE=1  Force Mesa software rasterizer (llvmpipe).\n";
}

/**
 * @brief Prints version information.
 */
inline void printVersion() {
    std::cout << "FlashViewer " << FLASHVIEWER_VERSION_STRING << "\n";
}

} // namespace fv
