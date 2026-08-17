#include "render/PixelHighlightOverlay.hpp"
#include <QPainter>
#include <QPaintEvent>
#include <QPolygonF>
#include <algorithm>

PixelHighlightOverlay::PixelHighlightOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
}

void PixelHighlightOverlay::setHighlight(const std::array<QPointF, 4>& corners_geo,
                                          const Camera* cam) {
    m_corners = corners_geo;
    m_cam     = cam;
    m_active  = true;
    update();
}

void PixelHighlightOverlay::clearHighlight() {
    m_active = false;
    update();
}

void PixelHighlightOverlay::paintEvent(QPaintEvent*) {
    if (!m_active || !m_cam) return;

    // The pixel's four corners are already in the camera's (Project) CRS. Project each to
    // screen and outline the resulting quadrilateral — under reprojection this is a sheared
    // ring, not an axis-aligned box.
    QPolygonF poly;
    poly.reserve(4);
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    double sumX = 0.0, sumY = 0.0;
    for (int i = 0; i < 4; ++i) {
        auto s = m_cam->geoToScreen(m_corners[i].x(), m_corners[i].y());
        if (i == 0) { minx = maxx = s.x; miny = maxy = s.y; }
        else {
            minx = std::min(minx, s.x); maxx = std::max(maxx, s.x);
            miny = std::min(miny, s.y); maxy = std::max(maxy, s.y);
        }
        sumX += s.x;
        sumY += s.y;
        poly << QPointF(s.x, s.y);
    }

    double cx = sumX / 4.0;
    double cy = sumY / 4.0;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    double boxW = maxx - minx;
    double boxH = maxy - miny;

    // If the pixel is large enough on screen, outline the pixel boundary
    if (boxW >= 4.0 && boxH >= 4.0) {
        p.setPen(QPen(QColor(230, 20, 20, 230), 2));
        p.setBrush(QColor(255, 0, 0, 35));
        p.drawPolygon(poly);
    }

    // Always draw a small, crisp red target marker at the selected pixel center
    // 1. Contrast halo / shadow (white outer stroke)
    p.setPen(QPen(QColor(255, 255, 255, 220), 3.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx - 6.0, cy), QPointF(cx + 6.0, cy));
    p.drawLine(QPointF(cx, cy - 6.0), QPointF(cx, cy + 6.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(cx, cy), 3.5, 3.5);

    // 2. Foreground red crosshair + center dot
    p.setPen(QPen(QColor(220, 20, 20, 255), 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx - 6.0, cy), QPointF(cx + 6.0, cy));
    p.drawLine(QPointF(cx, cy - 6.0), QPointF(cx, cy + 6.0));
    p.setBrush(QColor(220, 20, 20, 255));
    p.drawEllipse(QPointF(cx, cy), 2.5, 2.5);
}
