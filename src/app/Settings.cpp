#include "app/Settings.hpp"
#include <QSettings>

static constexpr const char* kTheme         = "appearance/theme";
static constexpr const char* kGeometry      = "window/geometry";
static constexpr const char* kState         = "window/state";
static constexpr const char* kLayoutVersion = "window/layoutVersion";
static constexpr const char* kOsmTileUrl    = "basemap/tileUrl";
static constexpr const char* kOsmTileUrlDefault =
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
static constexpr const char* kDisplayResampling = "render/displayResampling";
static constexpr const char* kLogExportDir       = "log/exportDir";
static constexpr const char* kPerfHudVisible     = "perf/hudVisible";
static constexpr const char* kCurveColorScheme   = "plots/curveColorScheme";
static constexpr const char* kPlotGrid           = "plots/grid";
static constexpr const char* kLegendCoords       = "plots/legendCoords";
static constexpr const char* kNumericDumpFontSize = "numericDump/fontSize";

Settings::Settings() = default;

Settings& Settings::instance() {
    static Settings inst;
    return inst;
}

Theme Settings::theme() const {
    QSettings s;
    return static_cast<Theme>(s.value(kTheme, static_cast<int>(Theme::Dark)).toInt());
}

void Settings::setTheme(Theme t) {
    QSettings s;
    s.setValue(kTheme, static_cast<int>(t));
}

QString Settings::osmTileUrl() const {
    QSettings s;
    return s.value(kOsmTileUrl, QLatin1String(kOsmTileUrlDefault)).toString();
}

void Settings::setOsmTileUrl(const QString& url) {
    QSettings s;
    s.setValue(kOsmTileUrl, url);
}

int Settings::displayResampling() const {
    QSettings s;
    int v = s.value(kDisplayResampling, 0).toInt();
    return (v < 0 || v > 2) ? 0 : v;   // clamp to {Bilinear, Bicubic2, Bicubic4}
}

void Settings::setDisplayResampling(int mode) {
    QSettings s;
    s.setValue(kDisplayResampling, (mode < 0 || mode > 2) ? 0 : mode);
}

QString Settings::logExportDir() const {
    QSettings s;
    return s.value(kLogExportDir, QString()).toString();
}

void Settings::setLogExportDir(const QString& dir) {
    QSettings s;
    s.setValue(kLogExportDir, dir);
}

bool Settings::perfHudVisible() const {
    QSettings s;
    return s.value(kPerfHudVisible, false).toBool();
}

void Settings::setPerfHudVisible(bool on) {
    QSettings s;
    s.setValue(kPerfHudVisible, on);
}

int Settings::curveColorScheme() const {
    QSettings s;
    return s.value(kCurveColorScheme, 0).toInt();
}

void Settings::setCurveColorScheme(int scheme) {
    QSettings s;
    s.setValue(kCurveColorScheme, scheme);
}

bool Settings::plotGrid() const {
    QSettings s;
    return s.value(kPlotGrid, true).toBool();
}

void Settings::setPlotGrid(bool on) {
    QSettings s;
    s.setValue(kPlotGrid, on);
}

bool Settings::legendCoords() const {
    QSettings s;
    return s.value(kLegendCoords, true).toBool();
}

void Settings::setLegendCoords(bool on) {
    QSettings s;
    s.setValue(kLegendCoords, on);
}

int Settings::numericDumpFontSize() const {
    QSettings s;
    int v = s.value(kNumericDumpFontSize, 8).toInt();
    return (v < 5 || v > 24) ? 8 : v;
}

void Settings::setNumericDumpFontSize(int pt) {
    QSettings s;
    s.setValue(kNumericDumpFontSize, (pt < 5 || pt > 24) ? 8 : pt);
}

QByteArray Settings::plotWindowGeometry(const QString& key) const {
    QSettings s;
    return s.value(QStringLiteral("plots/geometry/") + key).toByteArray();
}

void Settings::setPlotWindowGeometry(const QString& key, const QByteArray& data) {
    QSettings s;
    s.setValue(QStringLiteral("plots/geometry/") + key, data);
}

void Settings::saveGeometry(const QByteArray& data) {
    QSettings s;
    s.setValue(kGeometry, data);
}

QByteArray Settings::loadGeometry() const {
    QSettings s;
    return s.value(kGeometry).toByteArray();
}

void Settings::saveState(const QByteArray& data) {
    QSettings s;
    s.setValue(kState, data);
}

QByteArray Settings::loadState() const {
    QSettings s;
    return s.value(kState).toByteArray();
}

int Settings::layoutVersion() const {
    QSettings s;
    return s.value(kLayoutVersion, -1).toInt();
}

void Settings::setLayoutVersion(int v) {
    QSettings s;
    s.setValue(kLayoutVersion, v);
}

void Settings::clearLayoutState() {
    QSettings s;
    s.remove(kGeometry);
    s.remove(kState);
    s.remove(kLayoutVersion);
}
