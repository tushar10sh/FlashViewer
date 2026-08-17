#pragma once
// Shared CRS-string helpers (Phase 11, FR-CRS-*). Header-only + free so the canvas, the
// status bar, and the CRS picker dialog format/compare CRS identically without duplicating
// OGR boilerplate. WKT/EPSG/PROJ strings are accepted uniformly via SetFromUserInput().
#include <ogr_spatialref.h>
#include <cpl_error.h>
#include <QString>
#include <cmath>
#include <string>

// Short human label for a CRS: "EPSG:32643" when an authority code is present, else the
// CRS name, else a fallback. Empty input ⇒ "geographic (lon/lat)" (the identity case).
inline QString fvCrsShortName(const std::string& wkt) {
    if (wkt.empty()) return QStringLiteral("geographic (lon/lat)");
    OGRSpatialReference sr;
    if (sr.SetFromUserInput(wkt.c_str()) != OGRERR_NONE) return QStringLiteral("unknown CRS");
    const char* auth = sr.GetAuthorityName(nullptr);
    const char* code = sr.GetAuthorityCode(nullptr);
    if (auth && code)
        return QString("%1:%2").arg(QString::fromUtf8(auth), QString::fromUtf8(code));
    const char* nm = sr.GetName();
    return nm ? QString::fromUtf8(nm) : QStringLiteral("unknown CRS");
}

// True when two CRS strings denote the same reference system (robust to WKT vs EPSG vs
// PROJ spelling, axis order, whitespace). Empty compares equal only to empty (both are the
// geographic/identity case, so no reprojection is needed between them).
inline bool fvSameCrsWkt(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.empty() || b.empty()) return false;
    OGRSpatialReference sa, sb;
    if (sa.SetFromUserInput(a.c_str()) != OGRERR_NONE) return false;
    if (sb.SetFromUserInput(b.c_str()) != OGRERR_NONE) return false;
    return sa.IsSame(&sb) == TRUE;
}

// Transform a point (x,y) from `fromWkt` to `toWkt` IN PLACE. Empty WKT = geographic
// (EPSG:4326). No-op returning true when the two CRS are the same (raw-equal fast path).
// Returns false and leaves (x,y) unchanged on parse/transform failure or a non-finite
// result. Used to bridge a click in a pane's Project CRS to a layer's SOURCE CRS so
// analysis (inspect, pixel readout, highlight) samples the right source pixel under
// on-the-fly reprojection (Phase 11, FR-CRS-4). OAMS_TRADITIONAL_GIS_ORDER (x=east/lon).
inline bool fvTransformPoint(const std::string& fromWkt, const std::string& toWkt,
                             double& x, double& y) {
    if (fvSameCrsWkt(fromWkt, toWkt)) return true;
    OGRSpatialReference src, dst;
    if (src.SetFromUserInput(fromWkt.empty() ? "EPSG:4326" : fromWkt.c_str()) != OGRERR_NONE)
        return false;
    if (dst.SetFromUserInput(toWkt.empty() ? "EPSG:4326" : toWkt.c_str()) != OGRERR_NONE)
        return false;
    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRCoordinateTransformation* ct = OGRCreateCoordinateTransformation(&src, &dst);
    if (!ct) return false;
    double tx = x, ty = y;
    // Transforming an out-of-domain point (e.g. a cursor readout while the camera sits over a
    // native-CRS-fallback layer) makes PROJ raise a CE_Failure ("utm: Invalid latitude") that
    // the global GDAL handler would surface as a red banner. Suppress it locally — the caller
    // already degrades gracefully on the `false` return (Phase 17 #4). CPLGetLastErrorMsg is
    // still set for any interested logger.
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const int ok = ct->Transform(1, &tx, &ty);
    CPLPopErrorHandler();
    OGRCoordinateTransformation::DestroyCT(ct);
    if (!ok || !std::isfinite(tx) || !std::isfinite(ty)) return false;
    x = tx;
    y = ty;
    return true;
}

struct FormattedCoordinates {
    QString xy_text;         ///< Projected / Geo X and Y (e.g. "X: 500234.1234  Y: 4210982.5678")
    QString latlon_text;     ///< WGS84 Lat and Lon (e.g. "Lat: 38.04512° N  Lon: 122.10543° W")
    QString single_line;     ///< Single line combining X, Y and Lat, Lon (for bottom status bar)
    QString multi_line;      ///< Multi line combining X, Y and Lat, Lon (for Pixel Inspector)
    double  lon{0.0};
    double  lat{0.0};
    bool    has_latlon{false};
};

/// High-performance, thread-safe coordinate transformer between two CRS representations.
/// Reuses an underlying OGRCoordinateTransformation object across frequent queries.
class CrsTransformer {
public:
    explicit CrsTransformer(const std::string& fromWkt, const std::string& toWkt = "EPSG:4326")
        : m_from_wkt(fromWkt), m_to_wkt(toWkt) {
        init();
    }

    ~CrsTransformer() {
        cleanup();
    }

    CrsTransformer(const CrsTransformer&) = delete;
    CrsTransformer& operator=(const CrsTransformer&) = delete;

    bool isValid() const { return m_valid; }
    bool isSame() const { return m_same; }
    bool isGeographic() const { return m_is_geographic; }
    const std::string& fromWkt() const { return m_from_wkt; }
    const std::string& toWkt() const { return m_to_wkt; }

    /// Transform in-place (x, y) from source CRS to destination CRS.
    bool transform(double& x, double& y) const {
        if (m_same) return true;
        if (!m_valid || !m_ct) return false;

        double tx = x, ty = y;
        bool ok = false;
        {
            std::lock_guard lock(m_mutex);
            CPLPushErrorHandler(CPLQuietErrorHandler);
            ok = (m_ct->Transform(1, &tx, &ty) != 0);
            CPLPopErrorHandler();
        }
        if (!ok || !std::isfinite(tx) || !std::isfinite(ty)) return false;
        x = tx;
        y = ty;
        return true;
    }

    /// Forward conversion: Project Geo (x, y) -> WGS84 Lat/Lon (degrees).
    bool toLatLon(double x, double y, double& lat, double& lon) const {
        double tx = x, ty = y;
        if (!transform(tx, ty)) return false;
        lon = tx;
        lat = ty;
        return true;
    }

    /// Reverse conversion: WGS84 Lat/Lon (degrees) -> Project Geo (x, y).
    bool toProjected(double lat, double lon, double& x, double& y) const {
        if (m_same) {
            x = lon;
            y = lat;
            return true;
        }
        double tx = lon, ty = lat;
        if (!fvTransformPoint(m_to_wkt, m_from_wkt, tx, ty)) return false;
        x = tx;
        y = ty;
        return true;
    }

    /// Convert from (x, y) into a FormattedCoordinates struct with real-time Lat/Lon and X/Y.
    FormattedCoordinates format(double x, double y) const {
        FormattedCoordinates res;
        double lon = x;
        double lat = y;
        res.has_latlon = toLatLon(x, y, lat, lon);
        res.lon = lon;
        res.lat = lat;

        if (m_is_geographic) {
            res.xy_text = QString("X: %1  Y: %2")
                .arg(x, 11, 'f', 6).arg(y, 11, 'f', 6);
        } else {
            res.xy_text = QString("X: %1  Y: %2")
                .arg(x, 11, 'f', 3).arg(y, 11, 'f', 3);
        }

        if (res.has_latlon) {
            char latHemi = (lat >= 0.0) ? 'N' : 'S';
            char lonHemi = (lon >= 0.0) ? 'E' : 'W';
            double absLat = std::abs(lat);
            double absLon = std::abs(lon);

            res.latlon_text = QString("Lat: %1° %2  Lon: %3° %4")
                .arg(absLat, 8, 'f', 5).arg(latHemi)
                .arg(absLon, 9, 'f', 5).arg(lonHemi);

            res.single_line = QString("%1  |  %2").arg(res.xy_text.trimmed(), res.latlon_text.trimmed());
            res.multi_line  = QString("%1\n%2").arg(res.xy_text.trimmed(), res.latlon_text.trimmed());
        } else {
            res.single_line = res.xy_text;
            res.multi_line  = res.xy_text;
        }
        return res;
    }

private:
    void init() {
        m_same = fvSameCrsWkt(m_from_wkt, m_to_wkt);
        OGRSpatialReference sr;
        m_is_geographic = m_from_wkt.empty() ||
            (sr.SetFromUserInput(m_from_wkt.c_str()) == OGRERR_NONE && sr.IsGeographic());

        if (m_same) {
            m_valid = true;
            return;
        }

        OGRSpatialReference src, dst;
        if (src.SetFromUserInput(m_from_wkt.empty() ? "EPSG:4326" : m_from_wkt.c_str()) != OGRERR_NONE) return;
        if (dst.SetFromUserInput(m_to_wkt.empty() ? "EPSG:4326" : m_to_wkt.c_str()) != OGRERR_NONE) return;
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        m_ct = OGRCreateCoordinateTransformation(&src, &dst);
        m_valid = (m_ct != nullptr);
    }

    void cleanup() {
        if (m_ct) {
            OGRCoordinateTransformation::DestroyCT(m_ct);
            m_ct = nullptr;
        }
    }

    std::string m_from_wkt;
    std::string m_to_wkt;
    OGRCoordinateTransformation* m_ct{nullptr};
    bool m_same{false};
    bool m_valid{false};
    bool m_is_geographic{true};
    mutable std::mutex m_mutex;
};

/// Thread-safe global pool for cached CrsTransformer objects shared across the application.
class CrsTransformerPool {
public:
    static std::shared_ptr<CrsTransformer> get(const std::string& fromWkt,
                                               const std::string& toWkt = "EPSG:4326") {
        static std::mutex s_pool_mutex;
        static std::unordered_map<std::string, std::shared_ptr<CrsTransformer>> s_pool;

        std::string key = fromWkt + "==>" + toWkt;
        std::lock_guard lock(s_pool_mutex);
        auto it = s_pool.find(key);
        if (it != s_pool.end()) {
            return it->second;
        }
        auto tr = std::make_shared<CrsTransformer>(fromWkt, toWkt);
        s_pool[key] = tr;
        return tr;
    }
};

/// Get a shared CrsTransformer instance from any CRS to EPSG:4326 (WGS84 Lat/Lon).
inline std::shared_ptr<CrsTransformer> fvGetWgs84Transformer(const std::string& fromWkt) {
    return CrsTransformerPool::get(fromWkt, "EPSG:4326");
}

/// Real-time conversion of (x, y) coordinates into formatted strings with both projected
/// units and WGS84 latitude / longitude readouts.
inline FormattedCoordinates fvFormatCoordinates(double x, double y, const std::string& crsWkt) {
    auto tr = fvGetWgs84Transformer(crsWkt);
    if (tr) return tr->format(x, y);

    FormattedCoordinates res;
    res.xy_text = QString("X: %1  Y: %2").arg(x, 11, 'f', 3).arg(y, 11, 'f', 3);
    res.single_line = res.xy_text;
    res.multi_line = res.xy_text;
    return res;
}


