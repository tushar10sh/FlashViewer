#pragma once
#include <QWidget>
#include <QColor>
#include <QString>
#include <QVector>
#include "gis/InspectTypes.hpp"   // InspectLayerEntry / InspectPaneGroup (shared, Phase 26)
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

class LayerManager;
class RasterLayer;
class QVBoxLayout;
class QToolButton;
class QEvent;

class AttributeInspector : public QWidget {
    Q_OBJECT
public:
    explicit AttributeInspector(QWidget* parent = nullptr);
    void setLayerManager(LayerManager* mgr);

    // Sample every group's layers at (geo_x, geo_y) and show one collapsible drop-down per
    // pane group (Phase 6.8). MainWindow chooses the groups (clicked pane, or its sync group).
    // geoWkt is the CRS of (geo_x,geo_y) — the clicked pane's Project CRS — so each layer's
    // SOURCE pixel is sampled by transforming the point into the layer's source CRS (FR-CRS-4).
    void inspectGroups(double geo_x, double geo_y, const std::string& geoWkt,
                       const QVector<InspectPaneGroup>& groups);

    // Clear the inspector display
    void clear();

    // Drop the drop-down for a pane whose inspected data is no longer valid (e.g. its active
    // layer was removed). No-op if that pane has no section shown.
    void removePaneGroup(uint64_t paneId);

protected:
    // Re-style live drop-down headers on a theme switch (their foreground is theme-dependent).
    void changeEvent(QEvent* e) override;

private:
    // Remove all pane-group sections (before rebuilding for a new click).
    void clearGroups();
    // Apply the pane-coloured, theme-compliant style to a drop-down header (pane colour stored
    // on the button so a theme switch can re-style it without re-sampling).
    void styleHeader(QToolButton* header, const QColor& paneColor);

    LayerManager* m_mgr{nullptr};
    class QLabel* m_coord_label{nullptr};
    QWidget*      m_groups{nullptr};      // container for the per-pane collapsible sections
    QVBoxLayout*  m_groups_layout{nullptr};
};
