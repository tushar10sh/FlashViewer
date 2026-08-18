#pragma once

#include "core/Layer.hpp"
#include "core/Extent.hpp"
#include "io/VectorDataset.hpp"

#include <QColor>
#include <QString>
#include <atomic>
#include <memory>

class VectorLayer : public Layer {
public:
    explicit VectorLayer(std::shared_ptr<VectorDataset> ds);
    ~VectorLayer() override = default;

    uint64_t layerId() const { return m_layer_id; }
    LayerType type() const override { return LayerType::Vector; }

    VectorDataset* dataset() const { return m_ds.get(); }
    std::shared_ptr<VectorDataset> datasetPtr() const { return m_ds; }

    Extent extent() const { return m_ds ? m_ds->extent() : Extent::invalid(); }

    // Styling configuration
    QColor strokeColor() const { return m_stroke_color; }
    void setStrokeColor(const QColor& c) { m_stroke_color = c; }

    float strokeWidth() const { return m_stroke_width; }
    void setStrokeWidth(float w) { m_stroke_width = std::max(0.5f, w); }

    Qt::PenStyle strokeStyle() const { return m_stroke_style; }
    void setStrokeStyle(Qt::PenStyle s) { m_stroke_style = s; }

    QColor fillColor() const { return m_fill_color; }
    void setFillColor(const QColor& c) { m_fill_color = c; }

    float pointSize() const { return m_point_size; }
    void setPointSize(float s) { m_point_size = std::max(1.0f, s); }

private:
    static inline std::atomic<uint64_t> s_next_id{100000};
    uint64_t m_layer_id;

    std::shared_ptr<VectorDataset> m_ds;

    // Default configuration as requested:
    // - Fill: transparent by default
    // - Outline / Points / Lines: Red solid line
    QColor        m_stroke_color{220, 20, 20, 255}; // Solid Red
    float         m_stroke_width{2.0f};
    Qt::PenStyle  m_stroke_style{Qt::SolidLine};
    QColor        m_fill_color{0, 0, 0, 0};          // Transparent fill
    float         m_point_size{6.0f};
};
