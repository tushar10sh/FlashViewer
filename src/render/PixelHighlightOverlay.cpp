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
    // Handled synchronously directly inside MapCanvas::paintGL() to prevent compositing lag
}
