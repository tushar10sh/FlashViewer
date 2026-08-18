#pragma once
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

enum class LayerType { Raster, Vector, OSMBasemap };

// The id of the default pane created at startup. Layers default to this pane so
// The id of the default pane created at startup. Layers default to this pane so
// rasters opened before any explicit pane assignment land on the first pane.
inline constexpr uint64_t kDefaultPaneId = 1;

// Special pane ID (0) for layers configured to overlay across ALL viewport panes simultaneously.
inline constexpr uint64_t kAllPanesId = 0;

class Layer {
public:
    virtual ~Layer() = default;
    virtual LayerType type() const = 0;

    QString name()    const { return m_name; }
    bool    visible() const { return m_visible; }
    float   opacity() const { return m_opacity; }
    virtual QString sourceFilePath() const { return {}; }
    // The pane this layer is displayed on (0 = all panes overlay, 1+ = specific pane).
    uint64_t paneId() const { return m_pane_id; }

    void setName(const QString& n) { m_name = n; }
    void setVisible(bool v)        { m_visible = v; }
    void setOpacity(float o)       { m_opacity = o; }
    void setPaneId(uint64_t id)    { m_pane_id = id; }

protected:
    QString  m_name{"Layer"};
    bool     m_visible{true};
    float    m_opacity{1.0f};
    uint64_t m_pane_id{kDefaultPaneId};
};

// Free predicate + filter so the per-pane rendering selection is unit-testable
// without an OpenGL canvas (Phase 6.0 foundation, TC-PNE-04).
inline bool fvLayerInPane(const Layer& l, uint64_t paneId) {
    return l.paneId() == paneId || l.paneId() == kAllPanesId;
}
inline std::vector<std::shared_ptr<Layer>> fvFilterPane(
    const std::vector<std::shared_ptr<Layer>>& layers, uint64_t paneId) {
    std::vector<std::shared_ptr<Layer>> out;
    out.reserve(layers.size());
    for (const auto& l : layers)
        if (l && fvLayerInPane(*l, paneId)) out.push_back(l);
    return out;
}
