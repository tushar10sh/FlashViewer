#pragma once
#include "core/Extent.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Orthographic 2D camera for geographic-space rendering.
// center: position in dataset/project CRS (x=east, y=north)
// scale:  geo-units per screen-pixel (larger = more zoomed out)
class Camera {
public:
    Camera() = default;

    // Set viewport dimensions (call on every resizeGL)
    void setViewportSize(int w, int h) { m_vp_w = w; m_vp_h = h; }
    int viewportWidth()  const { return m_vp_w; }
    int viewportHeight() const { return m_vp_h; }

    // Fit camera to show a geographic extent
    void fitToExtent(const Extent& ext);

    // Pan by (dx_pix, dy_pix) screen pixels
    void pan(double dx_pix, double dy_pix);

    // Zoom by factor centred on screen point (sx, sy)
    // factor > 1 = zoom in; factor < 1 = zoom out
    void zoom(double factor, double sx, double sy);

    // Orthographic projection matrix ready for the vertex shader
    glm::mat4 viewProjMatrix() const;

    // Camera-relative orthographic projection matrix (origin at camera center, eliminating
    // single-precision floating point jitter on large UTM / projected coordinates)
    glm::mat4 relativeViewProjMatrix() const;

    // Geographic extent currently visible
    Extent visibleExtent() const;

    // Convert screen pixel → geographic coordinate
    glm::dvec2 screenToGeo(double sx, double sy) const;

    // Convert geographic coordinate → screen pixel
    glm::dvec2 geoToScreen(double gx, double gy) const;

    // Accessors
    glm::dvec2 center() const { return m_center; }
    double     scale()  const { return m_scale; }

    // Direct setters — used to re-place the camera when a pane's Project CRS changes
    // (Phase 11, FR-CRS-2): the center is transformed old→new CRS and the scale is
    // rescaled by the local unit ratio so the view stays put across the change.
    void setCenter(glm::dvec2 c) { m_center = c; }
    void setScale(double s)      { if (s > 0.0) m_scale = s; }

    bool operator==(const Camera& o) const {
        return m_center == o.m_center && m_scale == o.m_scale;
    }

private:
    glm::dvec2 m_center{0.0, 0.0};
    double     m_scale{1.0};   // geo-units per pixel
    int        m_vp_w{1};
    int        m_vp_h{1};
};
