#include "render/Camera.hpp"
#include <algorithm>
#include <cmath>

void Camera::fitToExtent(const Extent& ext) {
    if (!ext.isValid()) return;

    auto c = ext.center();
    m_center = c;

    // Choose scale so the full extent fits in the viewport with 5% padding
    double pad     = 1.05;
    double scaleX  = ext.width()  * pad / std::max(m_vp_w,  1);
    double scaleY  = ext.height() * pad / std::max(m_vp_h, 1);
    m_scale = std::max(scaleX, scaleY);
}

void Camera::pan(double dx_pix, double dy_pix) {
    // Screen x increases right (east), screen y increases down (south in pixel space)
    m_center.x -= dx_pix * m_scale;
    m_center.y += dy_pix * m_scale;   // y is flipped (geo y increases north)
}

void Camera::zoom(double factor, double sx, double sy) {
    // Geo coords under cursor before zoom
    auto geo_before = screenToGeo(sx, sy);

    m_scale /= factor;
    m_scale  = std::max(m_scale, 1e-12);   // prevent collapse

    // Recompute where that geo point now lands and adjust centre so it stays fixed
    double vp_cx = m_vp_w * 0.5;
    double vp_cy = m_vp_h * 0.5;
    m_center.x = geo_before.x - (sx - vp_cx) * m_scale;
    m_center.y = geo_before.y + (sy - vp_cy) * m_scale;
}

glm::mat4 Camera::viewProjMatrix() const {
    double half_w = m_vp_w * 0.5 * m_scale;
    double half_h = m_vp_h * 0.5 * m_scale;

    float left   = static_cast<float>(m_center.x - half_w);
    float right  = static_cast<float>(m_center.x + half_w);
    float bottom = static_cast<float>(m_center.y - half_h);
    float top    = static_cast<float>(m_center.y + half_h);

    return glm::ortho(left, right, bottom, top, -1.0f, 1.0f);
}

glm::mat4 Camera::relativeViewProjMatrix() const {
    double half_w = m_vp_w * 0.5 * m_scale;
    double half_h = m_vp_h * 0.5 * m_scale;

    float left   = static_cast<float>(-half_w);
    float right  = static_cast<float>(half_w);
    float bottom = static_cast<float>(-half_h);
    float top    = static_cast<float>(half_h);

    return glm::ortho(left, right, bottom, top, -1.0f, 1.0f);
}

Extent Camera::visibleExtent() const {
    double half_w = m_vp_w * 0.5 * m_scale;
    double half_h = m_vp_h * 0.5 * m_scale;
    return {
        m_center.x - half_w, m_center.y - half_h,
        m_center.x + half_w, m_center.y + half_h
    };
}

glm::dvec2 Camera::screenToGeo(double sx, double sy) const {
    double vp_cx = m_vp_w * 0.5;
    double vp_cy = m_vp_h * 0.5;
    return {
        m_center.x + (sx - vp_cx) * m_scale,
        m_center.y - (sy - vp_cy) * m_scale   // y flipped
    };
}

glm::dvec2 Camera::geoToScreen(double gx, double gy) const {
    return {
        m_vp_w * 0.5 + (gx - m_center.x) / m_scale,
        m_vp_h * 0.5 - (gy - m_center.y) / m_scale   // y flipped
    };
}
