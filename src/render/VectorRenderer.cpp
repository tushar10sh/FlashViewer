#include "render/VectorRenderer.hpp"
#include "core/VectorLayer.hpp"

#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>

void VectorRenderer::render(QPainter& painter,
                            const Camera& camera,
                            const std::vector<std::shared_ptr<Layer>>& layers,
                            const std::string& projectCrsWkt)
{
    const int vpW = camera.viewportWidth();
    const int vpH = camera.viewportHeight();
    if (vpW <= 0 || vpH <= 0) return;

    const Extent visibleGeoExt = camera.visibleExtent();
    if (!visibleGeoExt.isValid()) return;

    // Margin around visible viewport in geo coordinates
    const double marginX = (visibleGeoExt.xmax - visibleGeoExt.xmin) * 0.05;
    const double marginY = (visibleGeoExt.ymax - visibleGeoExt.ymin) * 0.05;
    const Extent queryExt{visibleGeoExt.xmin - marginX, visibleGeoExt.ymin - marginY,
                          visibleGeoExt.xmax + marginX, visibleGeoExt.ymax + marginY};

    const QRectF viewportRect(-10, -10, vpW + 20, vpH + 20);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Scratch buffers for zero heap allocation in frame render loop
    std::vector<size_t> visiblePolys;
    std::vector<size_t> visibleLines;
    std::vector<size_t> visiblePoints;
    QPolygonF screenExtScratch;
    QPolygonF screenHoleScratch;

    for (const auto& l : layers) {
        if (!l || !l->visible() || l->type() != LayerType::Vector) continue;

        auto* vl = static_cast<VectorLayer*>(l.get());
        if (!vl->dataset()) continue;

        const float layerOpacity = std::clamp(vl->opacity(), 0.0f, 1.0f);
        if (layerOpacity <= 0.0f) continue;

        QColor strokeColor = vl->strokeColor();
        strokeColor.setAlphaF(strokeColor.alphaF() * layerOpacity);

        QColor fillColor = vl->fillColor();
        fillColor.setAlphaF(fillColor.alphaF() * layerOpacity);

        QPen pen(strokeColor, vl->strokeWidth(), vl->strokeStyle(), Qt::RoundCap, Qt::RoundJoin);
        QBrush brush(fillColor);
        const bool hasFill = (fillColor.alpha() > 0);

        auto geoms = vl->dataset()->geometriesForCrs(projectCrsWkt);
        if (!geoms || !geoms->extent.isValid() || !geoms->extent.overlaps(queryExt)) continue;

        // Query spatial index for ONLY features overlapping viewport
        geoms->spatialIndex.queryVisible(queryExt, geoms->polygons, geoms->lines, geoms->points,
                                         visiblePolys, visibleLines, visiblePoints);

        const double geoScale = camera.scale(); // geo units per pixel

        // 1. Render Visible Polygons
        for (size_t polyIdx : visiblePolys) {
            const auto& poly = geoms->polygons[polyIdx];
            if (poly.exterior.size() < 3) continue;

            // Screen space bounding size check
            double wPx = (poly.bbox.xmax - poly.bbox.xmin) / geoScale;
            double hPx = (poly.bbox.ymax - poly.bbox.ymin) / geoScale;

            // Sub-pixel LOD: if smaller than 1 screen pixel, draw a simple point
            if (wPx < 1.2 && hPx < 1.2) {
                glm::dvec2 s = camera.geoToScreen((poly.bbox.xmin + poly.bbox.xmax) * 0.5,
                                                  (poly.bbox.ymin + poly.bbox.ymax) * 0.5);
                if (viewportRect.contains(s.x, s.y)) {
                    painter.setPen(pen);
                    painter.drawPoint(QPointF(s.x, s.y));
                }
                continue;
            }

            screenExtScratch.clear();
            screenExtScratch.reserve(static_cast<qsizetype>(poly.exterior.size()));
            for (const auto& pt : poly.exterior) {
                glm::dvec2 s = camera.geoToScreen(pt.x, pt.y);
                screenExtScratch.append(QPointF(s.x, s.y));
            }

            if (poly.interiors.empty()) {
                if (hasFill) {
                    painter.setPen(pen);
                    painter.setBrush(brush);
                    painter.drawPolygon(screenExtScratch);
                } else {
                    // Transparent fill: drawing polygon with NoBrush is very fast
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(pen);
                    painter.drawPolygon(screenExtScratch);
                }
            } else {
                if (hasFill) {
                    QPainterPath path;
                    path.setFillRule(Qt::OddEvenFill);
                    path.addPolygon(screenExtScratch);

                    for (const auto& hole : poly.interiors) {
                        if (hole.size() < 3) continue;
                        screenHoleScratch.clear();
                        screenHoleScratch.reserve(static_cast<qsizetype>(hole.size()));
                        for (const auto& pt : hole) {
                            glm::dvec2 s = camera.geoToScreen(pt.x, pt.y);
                            screenHoleScratch.append(QPointF(s.x, s.y));
                        }
                        path.addPolygon(screenHoleScratch);
                    }

                    painter.setPen(pen);
                    painter.setBrush(brush);
                    painter.drawPath(path);
                } else {
                    // Transparent fill: just draw outlines of exterior and interior rings
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(pen);
                    painter.drawPolygon(screenExtScratch);
                    for (const auto& hole : poly.interiors) {
                        if (hole.size() < 3) continue;
                        screenHoleScratch.clear();
                        screenHoleScratch.reserve(static_cast<qsizetype>(hole.size()));
                        for (const auto& pt : hole) {
                            glm::dvec2 s = camera.geoToScreen(pt.x, pt.y);
                            screenHoleScratch.append(QPointF(s.x, s.y));
                        }
                        painter.drawPolygon(screenHoleScratch);
                    }
                }
            }
        }

        // 2. Render Visible Lines
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        for (size_t lineIdx : visibleLines) {
            const auto& line = geoms->lines[lineIdx];
            if (line.points.size() < 2) continue;

            screenExtScratch.clear();
            screenExtScratch.reserve(static_cast<qsizetype>(line.points.size()));
            for (const auto& pt : line.points) {
                glm::dvec2 s = camera.geoToScreen(pt.x, pt.y);
                screenExtScratch.append(QPointF(s.x, s.y));
            }
            painter.drawPolyline(screenExtScratch);
        }

        // 3. Render Visible Points
        const float ptRadius = std::max(2.0f, vl->pointSize() / 2.0f);
        painter.setPen(pen);
        painter.setBrush(hasFill ? brush : QBrush(strokeColor));

        for (size_t ptIdx : visiblePoints) {
            const auto& pt = geoms->points[ptIdx];
            glm::dvec2 s = camera.geoToScreen(pt.pos.x, pt.pos.y);
            if (viewportRect.contains(s.x, s.y)) {
                painter.drawEllipse(QPointF(s.x, s.y), ptRadius, ptRadius);
            }
        }
    }

    painter.restore();
}
