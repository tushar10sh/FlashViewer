#pragma once

#include "render/Camera.hpp"
#include "core/Layer.hpp"
#include <QPainter>
#include <memory>
#include <string>
#include <vector>

class VectorRenderer {
public:
    VectorRenderer() = default;
    ~VectorRenderer() = default;

    /// Render all visible vector layers onto the given painter and camera view
    void render(QPainter& painter,
                const Camera& camera,
                const std::vector<std::shared_ptr<Layer>>& layers,
                const std::string& projectCrsWkt);
};
