#include "io/VectorDataset.hpp"
#include "gis/CrsUtil.hpp"
#include "util/Logger.hpp"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <cmath>
#include <algorithm>

namespace {

Extent computePointsExtent(const std::vector<glm::dvec2>& pts) {
    if (pts.empty()) return Extent::invalid();
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const auto& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    if (!std::isfinite(minX)) return Extent::invalid();
    return Extent{minX, minY, maxX, maxY};
}

void extractGeometries(OGRGeometry* geom, VectorGeometryCollection& coll) {
    if (!geom) return;

    OGRwkbGeometryType type = wkbFlatten(geom->getGeometryType());

    if (type == wkbPoint) {
        auto* pt = dynamic_cast<OGRPoint*>(geom);
        if (pt) {
            coll.points.push_back({ glm::dvec2(pt->getX(), pt->getY()) });
        }
    } else if (type == wkbMultiPoint) {
        auto* mp = dynamic_cast<OGRMultiPoint*>(geom);
        if (mp) {
            for (int i = 0; i < mp->getNumGeometries(); ++i) {
                auto* pt = dynamic_cast<OGRPoint*>(mp->getGeometryRef(i));
                if (pt) {
                    coll.points.push_back({ glm::dvec2(pt->getX(), pt->getY()) });
                }
            }
        }
    } else if (type == wkbLineString) {
        auto* ls = dynamic_cast<OGRLineString*>(geom);
        if (ls && ls->getNumPoints() > 0) {
            VectorLineString vls;
            int nPts = ls->getNumPoints();
            vls.points.reserve(nPts);
            for (int i = 0; i < nPts; ++i) {
                vls.points.push_back(glm::dvec2(ls->getX(i), ls->getY(i)));
            }
            vls.bbox = computePointsExtent(vls.points);
            coll.lines.push_back(std::move(vls));
        }
    } else if (type == wkbMultiLineString) {
        auto* mls = dynamic_cast<OGRMultiLineString*>(geom);
        if (mls) {
            for (int i = 0; i < mls->getNumGeometries(); ++i) {
                auto* ls = dynamic_cast<OGRLineString*>(mls->getGeometryRef(i));
                if (ls && ls->getNumPoints() > 0) {
                    VectorLineString vls;
                    int nPts = ls->getNumPoints();
                    vls.points.reserve(nPts);
                    for (int j = 0; j < nPts; ++j) {
                        vls.points.push_back(glm::dvec2(ls->getX(j), ls->getY(j)));
                    }
                    vls.bbox = computePointsExtent(vls.points);
                    coll.lines.push_back(std::move(vls));
                }
            }
        }
    } else if (type == wkbPolygon) {
        auto* poly = dynamic_cast<OGRPolygon*>(geom);
        if (poly) {
            auto* extRing = poly->getExteriorRing();
            if (extRing && extRing->getNumPoints() > 0) {
                VectorPolygon vp;
                int nExt = extRing->getNumPoints();
                vp.exterior.reserve(nExt);
                for (int i = 0; i < nExt; ++i) {
                    vp.exterior.push_back(glm::dvec2(extRing->getX(i), extRing->getY(i)));
                }
                vp.bbox = computePointsExtent(vp.exterior);
                int nRings = poly->getNumInteriorRings();
                for (int r = 0; r < nRings; ++r) {
                    auto* inRing = poly->getInteriorRing(r);
                    if (inRing && inRing->getNumPoints() > 0) {
                        std::vector<glm::dvec2> hole;
                        int nHole = inRing->getNumPoints();
                        hole.reserve(nHole);
                        for (int j = 0; j < nHole; ++j) {
                            hole.push_back(glm::dvec2(inRing->getX(j), inRing->getY(j)));
                        }
                        vp.interiors.push_back(std::move(hole));
                    }
                }
                coll.polygons.push_back(std::move(vp));
            }
        }
    } else if (type == wkbMultiPolygon) {
        auto* mpoly = dynamic_cast<OGRMultiPolygon*>(geom);
        if (mpoly) {
            for (int i = 0; i < mpoly->getNumGeometries(); ++i) {
                auto* sub = dynamic_cast<OGRPolygon*>(mpoly->getGeometryRef(i));
                if (sub) {
                    auto* extRing = sub->getExteriorRing();
                    if (extRing && extRing->getNumPoints() > 0) {
                        VectorPolygon vp;
                        int nExt = extRing->getNumPoints();
                        vp.exterior.reserve(nExt);
                        for (int j = 0; j < nExt; ++j) {
                            vp.exterior.push_back(glm::dvec2(extRing->getX(j), extRing->getY(j)));
                        }
                        vp.bbox = computePointsExtent(vp.exterior);
                        int nRings = sub->getNumInteriorRings();
                        for (int r = 0; r < nRings; ++r) {
                            auto* inRing = sub->getInteriorRing(r);
                            if (inRing && inRing->getNumPoints() > 0) {
                                std::vector<glm::dvec2> hole;
                                int nHole = inRing->getNumPoints();
                                hole.reserve(nHole);
                                for (int k = 0; k < nHole; ++k) {
                                    hole.push_back(glm::dvec2(inRing->getX(k), inRing->getY(k)));
                                }
                                vp.interiors.push_back(std::move(hole));
                            }
                        }
                        coll.polygons.push_back(std::move(vp));
                    }
                }
            }
        }
    } else if (type == wkbGeometryCollection) {
        auto* gc = dynamic_cast<OGRGeometryCollection*>(geom);
        if (gc) {
            for (int i = 0; i < gc->getNumGeometries(); ++i) {
                extractGeometries(gc->getGeometryRef(i), coll);
            }
        }
    }
}

Extent computeExtent(const VectorGeometryCollection& coll) {
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    bool hasData = false;

    for (const auto& pt : coll.points) {
        if (!std::isfinite(pt.pos.x) || !std::isfinite(pt.pos.y)) continue;
        minX = std::min(minX, pt.pos.x);
        maxX = std::max(maxX, pt.pos.x);
        minY = std::min(minY, pt.pos.y);
        maxY = std::max(maxY, pt.pos.y);
        hasData = true;
    }
    for (const auto& line : coll.lines) {
        if (line.bbox.isValid()) {
            minX = std::min(minX, line.bbox.xmin);
            maxX = std::max(maxX, line.bbox.xmax);
            minY = std::min(minY, line.bbox.ymin);
            maxY = std::max(maxY, line.bbox.ymax);
            hasData = true;
        }
    }
    for (const auto& poly : coll.polygons) {
        if (poly.bbox.isValid()) {
            minX = std::min(minX, poly.bbox.xmin);
            maxX = std::max(maxX, poly.bbox.xmax);
            minY = std::min(minY, poly.bbox.ymin);
            maxY = std::max(maxY, poly.bbox.ymax);
            hasData = true;
        }
    }

    if (!hasData) return Extent::invalid();
    return Extent{minX, minY, maxX, maxY};
}

} // namespace

void VectorSpatialIndex::build(const std::vector<VectorPolygon>& polys,
                               const std::vector<VectorLineString>& lines,
                               const std::vector<VectorPoint>& points,
                               const Extent& totalExt)
{
    bounds = totalExt;
    if (!bounds.isValid()) return;

    size_t total = polys.size() + lines.size() + points.size();
    if (total == 0) return;

    gridCols = std::clamp(static_cast<int>(std::sqrt(total) / 3.0), 8, 64);
    gridRows = gridCols;
    cellW = bounds.width() / gridCols;
    cellH = bounds.height() / gridRows;
    if (cellW <= 0.0) cellW = 1.0;
    if (cellH <= 0.0) cellH = 1.0;

    polyBuckets.assign(static_cast<size_t>(gridCols * gridRows), {});
    lineBuckets.assign(static_cast<size_t>(gridCols * gridRows), {});
    pointBuckets.assign(static_cast<size_t>(gridCols * gridRows), {});

    auto getCol = [this](double x) {
        int c = static_cast<int>((x - bounds.xmin) / cellW);
        return std::clamp(c, 0, gridCols - 1);
    };
    auto getRow = [this](double y) {
        int r = static_cast<int>((y - bounds.ymin) / cellH);
        return std::clamp(r, 0, gridRows - 1);
    };

    for (size_t i = 0; i < polys.size(); ++i) {
        const auto& p = polys[i];
        if (!p.bbox.isValid()) continue;
        int c0 = getCol(p.bbox.xmin);
        int c1 = getCol(p.bbox.xmax);
        int r0 = getRow(p.bbox.ymin);
        int r1 = getRow(p.bbox.ymax);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                polyBuckets[static_cast<size_t>(r * gridCols + c)].push_back(i);
            }
        }
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];
        if (!l.bbox.isValid()) continue;
        int c0 = getCol(l.bbox.xmin);
        int c1 = getCol(l.bbox.xmax);
        int r0 = getRow(l.bbox.ymin);
        int r1 = getRow(l.bbox.ymax);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                lineBuckets[static_cast<size_t>(r * gridCols + c)].push_back(i);
            }
        }
    }

    for (size_t i = 0; i < points.size(); ++i) {
        const auto& pt = points[i];
        int c = getCol(pt.pos.x);
        int r = getRow(pt.pos.y);
        pointBuckets[static_cast<size_t>(r * gridCols + c)].push_back(i);
    }
}

void VectorSpatialIndex::queryVisible(const Extent& qExt,
                                      const std::vector<VectorPolygon>& polys,
                                      const std::vector<VectorLineString>& lines,
                                      const std::vector<VectorPoint>& points,
                                      std::vector<size_t>& visiblePolys,
                                      std::vector<size_t>& visibleLines,
                                      std::vector<size_t>& visiblePoints) const
{
    visiblePolys.clear();
    visibleLines.clear();
    visiblePoints.clear();

    if (!bounds.isValid() || !qExt.isValid() || !bounds.overlaps(qExt)) return;

    auto getCol = [this](double x) {
        int c = static_cast<int>((x - bounds.xmin) / cellW);
        return std::clamp(c, 0, gridCols - 1);
    };
    auto getRow = [this](double y) {
        int r = static_cast<int>((y - bounds.ymin) / cellH);
        return std::clamp(r, 0, gridRows - 1);
    };

    int c0 = getCol(qExt.xmin);
    int c1 = getCol(qExt.xmax);
    int r0 = getRow(qExt.ymin);
    int r1 = getRow(qExt.ymax);

    std::vector<bool> polySeen(polys.size(), false);
    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            const auto& bucket = polyBuckets[static_cast<size_t>(r * gridCols + c)];
            for (size_t idx : bucket) {
                if (!polySeen[idx]) {
                    polySeen[idx] = true;
                    if (polys[idx].bbox.overlaps(qExt)) {
                        visiblePolys.push_back(idx);
                    }
                }
            }
        }
    }

    std::vector<bool> lineSeen(lines.size(), false);
    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            const auto& bucket = lineBuckets[static_cast<size_t>(r * gridCols + c)];
            for (size_t idx : bucket) {
                if (!lineSeen[idx]) {
                    lineSeen[idx] = true;
                    if (lines[idx].bbox.overlaps(qExt)) {
                        visibleLines.push_back(idx);
                    }
                }
            }
        }
    }

    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            const auto& bucket = pointBuckets[static_cast<size_t>(r * gridCols + c)];
            for (size_t idx : bucket) {
                const auto& pt = points[idx];
                if (pt.pos.x >= qExt.xmin && pt.pos.x <= qExt.xmax &&
                    pt.pos.y >= qExt.ymin && pt.pos.y <= qExt.ymax) {
                    visiblePoints.push_back(idx);
                }
            }
        }
    }
}

std::shared_ptr<VectorDataset> VectorDataset::open(const std::string& filePath) {
    auto ds = std::make_shared<VectorDataset>(filePath);
    if (!ds->isValid()) return nullptr;
    return ds;
}

VectorDataset::VectorDataset(const std::string& filePath)
    : m_file_path(filePath)
{
    loadFromOgr();
}

bool VectorDataset::loadFromOgr() {
    GDALAllRegister();
    OGRRegisterAll();

    GDALDataset* poDS = static_cast<GDALDataset*>(
        GDALOpenEx(m_file_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));

    if (!poDS) {
        FV_WARN("VectorDataset: failed to open vector file '{}'", m_file_path);
        return false;
    }

    if (poDS->GetLayerCount() == 0) {
        FV_WARN("VectorDataset: file '{}' has no layers", m_file_path);
        GDALClose(poDS);
        return false;
    }

    OGRLayer* poLayer = poDS->GetLayer(0);
    if (!poLayer) {
        GDALClose(poDS);
        return false;
    }

    m_layer_name = poLayer->GetName();
    const OGRSpatialReference* srs = poLayer->GetSpatialRef();
    if (srs) {
        char* wkt = nullptr;
        srs->exportToWkt(&wkt);
        if (wkt) {
            m_crs_wkt = wkt;
            CPLFree(wkt);
        }
    }

    // Geometry type name
    OGRwkbGeometryType baseType = wkbFlatten(poLayer->GetGeomType());
    m_geom_type_name = OGRGeometryTypeToName(baseType);

    // Read features and extract geometries
    poLayer->ResetReading();
    m_feature_count = 0;
    while (OGRFeature* poFeature = poLayer->GetNextFeature()) {
        ++m_feature_count;
        OGRGeometry* poGeometry = poFeature->GetGeometryRef();
        if (poGeometry) {
            extractGeometries(poGeometry, m_native_geoms);
        }
        OGRFeature::DestroyFeature(poFeature);
    }

    m_native_geoms.extent = computeExtent(m_native_geoms);
    m_native_geoms.spatialIndex.build(m_native_geoms.polygons, m_native_geoms.lines, m_native_geoms.points, m_native_geoms.extent);

    GDALClose(poDS);

    m_valid = (!m_native_geoms.points.empty() || !m_native_geoms.lines.empty() || !m_native_geoms.polygons.empty());
    FV_INFO("VectorDataset: opened '{}' ({} features, {} points, {} lines, {} polygons)",
            m_file_path, m_feature_count,
            m_native_geoms.points.size(), m_native_geoms.lines.size(), m_native_geoms.polygons.size());
    return m_valid;
}

bool VectorDataset::canReprojectTo(const std::string& targetCrsWkt, std::string* errorReason) const {
    if (targetCrsWkt.empty() || targetCrsWkt == m_crs_wkt || m_crs_wkt.empty()) {
        return true;
    }
    auto transformer = CrsTransformerPool::get(m_crs_wkt, targetCrsWkt);
    if (!transformer || !transformer->isValid()) {
        if (errorReason) {
            *errorReason = "Failed to initialize OGR coordinate transformer between vector CRS and target CRS.";
        }
        return false;
    }
    // Test point transformation on center of extent
    if (m_native_geoms.extent.isValid()) {
        double testX = (m_native_geoms.extent.xmin + m_native_geoms.extent.xmax) * 0.5;
        double testY = (m_native_geoms.extent.ymin + m_native_geoms.extent.ymax) * 0.5;
        if (!transformer->transform(testX, testY)) {
            if (errorReason) {
                *errorReason = "Coordinate conversion test failed (coordinates out of target CRS bounds or incompatible datum/projection).";
            }
            return false;
        }
    }
    return true;
}

std::shared_ptr<const VectorGeometryCollection> VectorDataset::geometriesForCrs(
    const std::string& targetCrsWkt,
    std::function<bool(int current, int total)> progressCallback)
{
    if (targetCrsWkt.empty() || targetCrsWkt == m_crs_wkt || m_crs_wkt.empty()) {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_projected_cache.find("");
        if (it != m_projected_cache.end()) {
            return it->second;
        }
        auto sp = std::make_shared<const VectorGeometryCollection>(m_native_geoms);
        m_projected_cache[""] = sp;
        return sp;
    }

    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_projected_cache.find(targetCrsWkt);
        if (it != m_projected_cache.end()) {
            return it->second;
        }
    }

    auto transformer = CrsTransformerPool::get(m_crs_wkt, targetCrsWkt);
    if (!transformer || !transformer->isValid()) {
        return nullptr;
    }

    const size_t totalWork = m_native_geoms.points.size() + m_native_geoms.lines.size() + m_native_geoms.polygons.size();
    size_t currentWork = 0;

    VectorGeometryCollection out;
    out.points.reserve(m_native_geoms.points.size());
    for (const auto& pt : m_native_geoms.points) {
        double px = pt.pos.x;
        double py = pt.pos.y;
        if (transformer->transform(px, py)) {
            out.points.push_back({ glm::dvec2(px, py) });
        }
        ++currentWork;
        if (progressCallback && (currentWork % 1000 == 0)) {
            if (!progressCallback(static_cast<int>(currentWork), static_cast<int>(totalWork))) {
                return nullptr;
            }
        }
    }

    out.lines.reserve(m_native_geoms.lines.size());
    for (const auto& line : m_native_geoms.lines) {
        VectorLineString vls;
        vls.points.reserve(line.points.size());
        for (const auto& p : line.points) {
            double px = p.x;
            double py = p.y;
            if (transformer->transform(px, py)) {
                vls.points.push_back(glm::dvec2(px, py));
            }
        }
        if (vls.points.size() >= 2) {
            vls.bbox = computePointsExtent(vls.points);
            out.lines.push_back(std::move(vls));
        }
        ++currentWork;
        if (progressCallback && (currentWork % 1000 == 0)) {
            if (!progressCallback(static_cast<int>(currentWork), static_cast<int>(totalWork))) {
                return nullptr;
            }
        }
    }

    out.polygons.reserve(m_native_geoms.polygons.size());
    for (const auto& poly : m_native_geoms.polygons) {
        VectorPolygon vp;
        vp.exterior.reserve(poly.exterior.size());
        for (const auto& p : poly.exterior) {
            double px = p.x;
            double py = p.y;
            if (transformer->transform(px, py)) {
                vp.exterior.push_back(glm::dvec2(px, py));
            }
        }
        for (const auto& hole : poly.interiors) {
            std::vector<glm::dvec2> vHole;
            vHole.reserve(hole.size());
            for (const auto& p : hole) {
                double px = p.x;
                double py = p.y;
                if (transformer->transform(px, py)) {
                    vHole.push_back(glm::dvec2(px, py));
                }
            }
            if (vHole.size() >= 3) {
                vp.interiors.push_back(std::move(vHole));
            }
        }
        if (vp.exterior.size() >= 3) {
            vp.bbox = computePointsExtent(vp.exterior);
            out.polygons.push_back(std::move(vp));
        }
        ++currentWork;
        if (progressCallback && (currentWork % 1000 == 0)) {
            if (!progressCallback(static_cast<int>(currentWork), static_cast<int>(totalWork))) {
                return nullptr;
            }
        }
    }

    out.extent = computeExtent(out);
    out.spatialIndex.build(out.polygons, out.lines, out.points, out.extent);

    if (progressCallback) {
        progressCallback(static_cast<int>(totalWork), static_cast<int>(totalWork));
    }

    auto result = std::make_shared<const VectorGeometryCollection>(std::move(out));
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        m_projected_cache[targetCrsWkt] = result;
    }

    return result;
}
