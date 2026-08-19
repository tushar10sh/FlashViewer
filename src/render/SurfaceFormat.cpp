#include "render/SurfaceFormat.hpp"

QSurfaceFormat fvDefaultSurfaceFormat(bool cpuMode) {
    QSurfaceFormat fmt;
    fmt.setVersion(4, 1);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(cpuMode ? 0 : 4);               // MSAA 4× in hardware (FR-RND-6); disabled in CPU mode for fast software rendering
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    // NOTE: the framebuffer keeps its alpha channel; the "whitish tinge / see-through"
    // (#7) is fixed in MapCanvas::initializeGL by pinning the framebuffer alpha to 1 via
    // glBlendFuncSeparate(...,GL_ZERO,GL_ONE) — requesting setAlphaBufferSize(0) here was
    // not honoured on Linux/WSL (the widget stayed translucent), so it is not used.
    return fmt;
}
