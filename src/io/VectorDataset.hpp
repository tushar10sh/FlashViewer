#pragma once

#include "core/Extent.hpp"
#include <glm/vec2.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct VectorPoint {
    glm::dvec2 pos{0.0, 0.0};
};

struct VectorLineString {
    std::vector<glm::dvec2> points;
    Extent bbox{Extent::invalid()};
};

struct VectorPolygon {
    std::vector<glm::dvec2> exterior;
    std::vector<std::vector<glm::dvec2>> interiors; // holes
    Extent bbox{Extent::invalid()};
};

struct VectorSpatialIndex {
    Extent bounds{Extent::invalid()};
    int gridCols{32};
    int gridRows{32};
    double cellW{1.0};
    double cellH{1.0};
    std::vector<std::vector<size_t>> polyBuckets;
    std::vector<std::vector<size_t>> lineBuckets;
    std::vector<std::vector<size_t>> pointBuckets;

    void build(const std::vector<VectorPolygon>& polys,
               const std::vector<VectorLineString>& lines,
               const std::vector<VectorPoint>& points,
               const Extent& totalExt);

    void queryVisible(const Extent& qExt,
                      const std::vector<VectorPolygon>& polys,
                      const std::vector<VectorLineString>& lines,
                      const std::vector<VectorPoint>& points,
                      std::vector<size_t>& visiblePolys,
                      std::vector<size_t>& visibleLines,
                      std::vector<size_t>& visiblePoints) const;
};

struct VectorGeometryCollection {
    std::vector<VectorPoint> points;
    std::vector<VectorLineString> lines;
    std::vector<VectorPolygon> polygons;
    Extent extent{Extent::invalid()};
    VectorSpatialIndex spatialIndex;
};

class VectorDataset {
public:
    static std::shared_ptr<VectorDataset> open(const std::string& filePath);

    explicit VectorDataset(const std::string& filePath);
    ~VectorDataset() = default;

    bool isValid() const { return m_valid; }
    const std::string& filePath() const { return m_file_path; }
    const std::string& layerName() const { return m_layer_name; }
    const std::string& geometryTypeName() const { return m_geom_type_name; }
    const std::string& crsWkt() const { return m_crs_wkt; }
    const Extent& extent() const { return m_native_geoms.extent; }
    int featureCount() const { return m_feature_count; }

    /// Test whether geometries can be reprojected to target CRS.
    bool canReprojectTo(const std::string& targetCrsWkt, std::string* errorReason = nullptr) const;

    /// Native geometries in source CRS
    const VectorGeometryCollection& nativeGeometries() const { return m_native_geoms; }

    /// Geometries reprojected to target CRS (cached, thread-safe shared_ptr)
    std::shared_ptr<const VectorGeometryCollection> geometriesForCrs(
        const std::string& targetCrsWkt,
        std::function<bool(int current, int total)> progressCallback = nullptr);

private:
    bool loadFromOgr();

    std::string m_file_path;
    std::string m_layer_name;
    std::string m_geom_type_name{"Unknown"};
    std::string m_crs_wkt;
    int m_feature_count{0};
    bool m_valid{false};

    VectorGeometryCollection m_native_geoms;

    std::mutex m_cache_mutex;
    std::unordered_map<std::string, std::shared_ptr<const VectorGeometryCollection>> m_projected_cache;
};
