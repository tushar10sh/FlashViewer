#pragma once
#include "core/GeoTransform.hpp"
#include "gis/CrsUtil.hpp"
#include <memory>

/// GeoTransform4326 combines a raster's 6-parameter affine GeoTransform with a
/// cached CrsTransformer to provide direct 2-way conversion between Pixel (col, row),
/// Projected Image Geo (x, y), and WGS84 Geodetic (lat, lon in degrees).
class GeoTransform4326 {
public:
    GeoTransform4326(const GeoTransform& gt, const std::string& crsWkt)
        : m_gt(gt), m_crs_wkt(crsWkt), m_transformer(fvGetWgs84Transformer(crsWkt)) {}

    const GeoTransform& geoTransform() const { return m_gt; }
    const std::string&  crsWkt()       const { return m_crs_wkt; }
    std::shared_ptr<CrsTransformer> transformer() const { return m_transformer; }

    /// Pixel (col, row) -> Projected Geo (x, y)
    glm::dvec2 pixelToGeo(double col, double row) const {
        return m_gt.pixelToGeo(col, row);
    }

    /// Projected Geo (x, y) -> Pixel (col, row)
    glm::dvec2 geoToPixel(double x, double y) const {
        return m_gt.geoToPixel(x, y);
    }

    /// Projected Geo (x, y) -> WGS84 Geodetic (lat, lon in degrees)
    bool geoToLatLon(double x, double y, double& lat, double& lon) const {
        if (!m_transformer) return false;
        return m_transformer->toLatLon(x, y, lat, lon);
    }

    /// WGS84 Geodetic (lat, lon in degrees) -> Projected Geo (x, y)
    bool latLonToGeo(double lat, double lon, double& x, double& y) const {
        if (!m_transformer) return false;
        return m_transformer->toProjected(lat, lon, x, y);
    }

    /// Pixel (col, row) -> WGS84 Geodetic (lat, lon in degrees)
    bool pixelToLatLon(double col, double row, double& lat, double& lon) const {
        glm::dvec2 g = m_gt.pixelToGeo(col, row);
        return geoToLatLon(g.x, g.y, lat, lon);
    }

    /// WGS84 Geodetic (lat, lon in degrees) -> Pixel (col, row)
    bool latLonToPixel(double lat, double lon, double& col, double& row) const {
        double gx = 0.0, gy = 0.0;
        if (!latLonToGeo(lat, lon, gx, gy)) return false;
        glm::dvec2 p = m_gt.geoToPixel(gx, gy);
        col = p.x;
        row = p.y;
        return true;
    }

    /// Format coordinates at given Projected Geo (x, y)
    FormattedCoordinates formatGeo(double x, double y) const {
        if (m_transformer) return m_transformer->format(x, y);
        return fvFormatCoordinates(x, y, m_crs_wkt);
    }

    /// Format coordinates at given Pixel (col, row)
    FormattedCoordinates formatPixel(double col, double row) const {
        glm::dvec2 g = m_gt.pixelToGeo(col, row);
        return formatGeo(g.x, g.y);
    }

private:
    GeoTransform m_gt;
    std::string  m_crs_wkt;
    std::shared_ptr<CrsTransformer> m_transformer;
};
