#pragma once
#include <QString>

enum class Theme { Light, Dark };

class Settings {
public:
    // Bump this whenever the dock widget set, objectNames, or the DEFAULT dock
    // arrangement change (so a stale saved layout is discarded and the new default
    // applies). v2: left column defaults to Layers ≈ half / tabbed
    // Layer-Properties+Histogram group ≈ half.
    // v3: added the GPU Monitor dock + Log-dock action bar.
    // v4: GPU Monitor dock defaulted to a tab with the Log panel (bottom area).
    // v5: GPU Monitor dock defaulted to the right column lower half.
    // v6: fixed that split so GPU Monitor actually lands below the Info/Inspector
    // group (split before tabbing Pixel Inspector), not as a third tab.
    static constexpr int kCurrentLayoutVersion = 7;   // 7: the two plot windows left the dock layout

    static Settings& instance();

    Theme  theme() const;
    void   setTheme(Theme t);

    QString osmTileUrl() const;
    void    setOsmTileUrl(const QString& url);

    // Display-time GPU resampling preference (FR-RND-10): 0=Bilinear, 1=Bicubic
    // (smooth/B-spline), 2=Bicubic (sharp/Catmull-Rom). Matches
    // RasterLayer::DisplayResampling. New layers inherit this default.
    int  displayResampling() const;
    void setDisplayResampling(int mode);

    // Last directory used by Export Logs (FR-APP-10), so the save dialog reopens there.
    QString logExportDir() const;
    void    setLogExportDir(const QString& dir);

    // Performance HUD overlay (FR-APP-14, Phase 12): whether the live frame/latency/VRAM
    // readout is shown on the canvas. Default off; persisted like the other view state.
    bool perfHudVisible() const;
    void setPerfHudVisible(bool on);

    // How plot curves are coloured (FR-ANL-8): 0 = pane hue + lightness + dash (the default,
    // so a curve names the pane its layer lives in), 1 = the original fixed 10-hue series
    // palette, cycled by curve position and carrying no pane meaning.
    int  curveColorScheme() const;
    void setCurveColorScheme(int scheme);

    // Plot grid lines, both plots (FR-ANL-14). Default on.
    bool plotGrid() const;
    void setPlotGrid(bool on);

    // Whether a Spectral Plot legend entry carries the sampled coordinates (FR-ANL-9).
    // Default on; off collapses every entry to one line at once.
    bool legendCoords() const;
    void setLegendCoords(bool on);

    // Geometry of the two plot WINDOWS (FR-ANL-1/2). They are no longer docks, so
    // QMainWindow::saveState no longer carries them and each saves its own frame.
    QByteArray plotWindowGeometry(const QString& key) const;
    void       setPlotWindowGeometry(const QString& key, const QByteArray& data);

    void saveGeometry(const QByteArray& data);
    QByteArray loadGeometry() const;

    void saveState(const QByteArray& data);
    QByteArray loadState() const;

    int  layoutVersion() const;
    void setLayoutVersion(int v);
    void clearLayoutState();

private:
    Settings();
};
