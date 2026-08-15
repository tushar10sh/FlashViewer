#pragma once

// Window chrome shared by the two plot panels (FR-ANL-1/2): what kind of top-level window a
// plot IS.
//
// A plot is a tool the user opens over the map and works beside — it has to stay above the main
// window while it is open, and it must never float above a browser they switch to. Qt's name
// for that is a TOOL window, owned by the main window:
//
//   Windows : WS_EX_TOOLWINDOW, owned — the shell keeps it above its owner, as before.
//   macOS   : an NSPanel, which floats above the application's own windows. Without this a
//             parented Qt::Window is not an owned window at all there: it sank behind the main
//             window on any click outside it. WA_MacAlwaysShowToolWindow additionally keeps it
//             visible while the application is inactive, instead of Qt's default of hiding
//             tool windows with the app.
//   X11     : _NET_WM_WINDOW_TYPE_UTILITY plus transient-for, which a compliant window manager
//             stacks above the owner while still letting the owner take focus.
//
// Known limitation, measured with a purpose-built probe: WSLg's Weston window manager honours
// none of this — a utility window there is stacked BELOW its owner and is given no proxy
// window, so it cannot be clicked from the Windows side. WSLg is a development convenience,
// not a target platform, and the trade was made deliberately.

#include <QWidget>

inline void fvApplyPlotWindowFlags(QWidget* w) {
    if (!w) return;
    w->setWindowFlags(Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::WindowSystemMenuHint);
#ifdef Q_OS_MACOS
    w->setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
#endif
}
