#pragma once
#include <QSurfaceFormat>

// The global default surface format the application requests for every
// QOpenGLWidget: OpenGL 4.1 Core, 4× MSAA, 24-bit depth, 8-bit stencil.
// Realizes FR-RND-6 (anti-aliasing) and FR-APP-7 / DC-2 (GL 4.1 Core).
// Factored out of main() so the configuration is inspectable in tests
// (TC-RND-09) without launching the GUI.
QSurfaceFormat fvDefaultSurfaceFormat(bool cpuMode = false);

