#pragma once
// GeoMeasurement.hpp: Great-Circle Haversine distance and spherical geodesic polygon area
// calculations. Transforms coordinates from any Project CRS into WGS84 geographic (EPSG:4326)
// and computes accurate geodesic distances and areas.
#include <QPointF>
#include <QString>
#include <vector>
#include <cmath>
#include <numbers>
#include <algorithm>
#include <utility>
#include "gis/CrsUtil.hpp"

namespace fv {

constexpr double kEarthMeanRadiusMeters = 6371008.8; // IUGG spherical Earth mean radius in meters
constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

/**
 * @brief Computes Great-Circle Haversine distance in meters between two WGS84 geographic points (lon, lat in degrees).
 */
inline double haversineDistance(double lon1, double lat1, double lon2, double lat2) {
    if (!std::isfinite(lon1) || !std::isfinite(lat1) || !std::isfinite(lon2) || !std::isfinite(lat2))
        return 0.0;

    const double phi1 = lat1 * kDegToRad;
    const double phi2 = lat2 * kDegToRad;
    const double dphi = (lat2 - lat1) * kDegToRad;
    const double dlam = (lon2 - lon1) * kDegToRad;

    const double sinHalfDphi = std::sin(dphi * 0.5);
    const double sinHalfDlam = std::sin(dlam * 0.5);

    double a = sinHalfDphi * sinHalfDphi +
               std::cos(phi1) * std::cos(phi2) * sinHalfDlam * sinHalfDlam;
    a = std::clamp(a, 0.0, 1.0);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kEarthMeanRadiusMeters * c;
}

/**
 * @brief Transforms a point from a given Project CRS WKT into WGS84 (lon, lat in degrees).
 * Returns true if transformation succeeded.
 */
inline bool projectToWgs84(double inX, double inY, const std::string& projectWkt, double& outLon, double& outLat) {
    double x = inX;
    double y = inY;
    if (projectWkt.empty()) {
        outLon = inX;
        outLat = inY;
        return true;
    }
    if (!fvTransformPoint(projectWkt, "EPSG:4326", x, y)) {
        outLon = inX;
        outLat = inY;
        return false;
    }
    outLon = x;
    outLat = y;
    return true;
}

/**
 * @brief Calculates total Haversine distance in meters along a polyline given in Project CRS coordinates.
 */
inline double calculatePolylineDistance(const std::vector<QPointF>& points, const std::string& projectWkt) {
    if (points.size() < 2) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        double lon1 = 0, lat1 = 0, lon2 = 0, lat2 = 0;
        projectToWgs84(points[i].x(), points[i].y(), projectWkt, lon1, lat1);
        projectToWgs84(points[i + 1].x(), points[i + 1].y(), projectWkt, lon2, lat2);
        total += haversineDistance(lon1, lat1, lon2, lat2);
    }
    return total;
}

/**
 * @brief Calculates segment distance in meters between two points given in Project CRS.
 */
inline double calculateSegmentDistance(const QPointF& p1, const QPointF& p2, const std::string& projectWkt) {
    double lon1 = 0, lat1 = 0, lon2 = 0, lat2 = 0;
    projectToWgs84(p1.x(), p1.y(), projectWkt, lon1, lat1);
    projectToWgs84(p2.x(), p2.y(), projectWkt, lon2, lat2);
    return haversineDistance(lon1, lat1, lon2, lat2);
}

/**
 * @brief Computes spherical geodesic polygon area in square meters on the Earth sphere
 * for a list of (lon, lat in degrees) coordinates.
 */
inline double sphericalPolygonArea(const std::vector<std::pair<double, double>>& lonLatDegs) {
    const size_t n = lonLatDegs.size();
    if (n < 3) return 0.0;

    double total = 0.0;
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        const double lon1 = lonLatDegs[i].first * kDegToRad;
        const double lat1 = lonLatDegs[i].second * kDegToRad;
        const double lon2 = lonLatDegs[j].first * kDegToRad;
        const double lat2 = lonLatDegs[j].second * kDegToRad;

        double dlon = lon2 - lon1;
        while (dlon > std::numbers::pi) dlon -= 2.0 * std::numbers::pi;
        while (dlon < -std::numbers::pi) dlon += 2.0 * std::numbers::pi;

        total += dlon * (2.0 + std::sin(lat1) + std::sin(lat2));
    }

    double area = std::abs(total) * 0.5 * kEarthMeanRadiusMeters * kEarthMeanRadiusMeters;
    return area;
}

/**
 * @brief Calculates spherical geodesic polygon area in square meters for vertices given in Project CRS.
 */
inline double calculatePolygonArea(const std::vector<QPointF>& points, const std::string& projectWkt) {
    if (points.size() < 3) return 0.0;
    std::vector<std::pair<double, double>> wgs;
    wgs.reserve(points.size());
    for (const auto& pt : points) {
        double lon = 0, lat = 0;
        projectToWgs84(pt.x(), pt.y(), projectWkt, lon, lat);
        wgs.emplace_back(lon, lat);
    }
    return sphericalPolygonArea(wgs);
}

/**
 * @brief Formats a distance in meters to a human-readable string (m, km).
 */
inline QString formatDistance(double meters) {
    if (!std::isfinite(meters) || meters < 0.0) return QStringLiteral("0.00 m");
    if (meters < 1000.0) {
        return QString("%1 m").arg(meters, 0, 'f', 2);
    } else {
        return QString("%1 km").arg(meters / 1000.0, 0, 'f', 3);
    }
}

/**
 * @brief Formats a distance in meters with secondary unit.
 */
inline QString formatDistanceDetailed(double meters) {
    if (!std::isfinite(meters) || meters < 0.0) return QStringLiteral("0.00 m");
    if (meters < 1000.0) {
        return QString("%1 m").arg(meters, 0, 'f', 2);
    } else {
        return QString("%1 km (%2 m)").arg(meters / 1000.0, 0, 'f', 3)
                                     .arg(meters, 0, 'f', 1);
    }
}

/**
 * @brief Formats an area in square meters to a human-readable string (m², ha, km²).
 */
inline QString formatArea(double areaM2) {
    if (!std::isfinite(areaM2) || areaM2 < 0.0) return QStringLiteral("0.00 m²");
    if (areaM2 < 10000.0) {
        return QString("%1 m²").arg(areaM2, 0, 'f', 2);
    } else if (areaM2 < 1000000.0) {
        const double ha = areaM2 / 10000.0;
        return QString("%1 ha (%2 m²)").arg(ha, 0, 'f', 2).arg(areaM2, 0, 'f', 1);
    } else {
        const double km2 = areaM2 / 1000000.0;
        const double ha = areaM2 / 10000.0;
        return QString("%1 km² (%2 ha)").arg(km2, 0, 'f', 3).arg(ha, 0, 'f', 2);
    }
}

} // namespace fv
