#include "app/Application.hpp"
#include "app/MainWindow.hpp"
#include "app/CommandLineOptions.hpp"
#include "live/LiveGeorefSession.hpp"
#include "render/SurfaceFormat.hpp"
#include "util/Logger.hpp"

#include <QCoreApplication>
#include <QSurfaceFormat>

int main(int argc, char* argv[]) {
    const auto opts = fv::parseCommandLine(argc, argv);

    if (opts.showHelp) {
        fv::printHelp(argc > 0 ? argv[0] : "FlashViewer");
        return 0;
    }

    if (opts.showVersion) {
        fv::printVersion();
        return 0;
    }

    if (opts.cpuMode) {
        fv::applyCpuRenderingEnvironment();
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }

    QSurfaceFormat::setDefaultFormat(fvDefaultSurfaceFormat(opts.cpuMode));

    // Multi-pane uses several QOpenGLWidgets in one window. Without a shared context group,
    // destroying one QOpenGLWidget (closing a pane) leaves its siblings' composited output
    // black even though they keep painting (Phase 6.4.4). AA_ShareOpenGLContexts puts every
    // widget context in one share group — the Qt-recommended setup for multiple QOpenGLWidgets.
    // Must be set BEFORE the QApplication is constructed.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    qRegisterMetaType<LiveTile>("LiveTile");
    qRegisterMetaType<QVector<double>>("QVector<double>");

    Application app(argc, argv);

    if (opts.cpuMode) {
        FV_INFO("CPU-only / Software OpenGL rendering mode enabled (Mesa llvmpipe)");
    }

    MainWindow win;
    if (opts.cpuMode) {
        win.setWindowTitle(win.windowTitle() + " [CPU Mode]");
    }
    win.show();

    if (!opts.filesToOpen.isEmpty()) {
        win.openFiles(opts.filesToOpen);
    }

    return app.exec();
}

