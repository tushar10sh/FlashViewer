#pragma once
#include <QColor>
#include <QString>
#include <QVector>
#include <cstdint>

class RasterLayer;

// The layer selection produced by one inspect gesture, shared by every consumer of that
// gesture (Phase 26). MainWindow::inspectFromPane assembles the groups once — left-click
// ⇒ each pane's representative layer, right-click ⇒ all its visible rasters, aggregated
// across the sync group when the clicked pane is synced — and hands the SAME groups to
// the Pixel Inspector and the Spectral Plot, so the two panels can never disagree about
// what was sampled.

// One layer to sample within a pane group. `name` is the row label shown in the group's
// table (just the layer name — the pane is shown in the group header).
struct InspectLayerEntry {
    QString      name;
    RasterLayer* layer{nullptr};
};

// A pane's worth of layers to inspect (Phase 6.8). The Pixel Inspector renders each as a
// collapsible drop-down whose header shows `paneLabel` on a translucent `paneColor`
// background; the Spectral Plot uses the same labels for its title and legend. Consumers
// stay pane/sync-agnostic — MainWindow chooses the groups.
struct InspectPaneGroup {
    uint64_t                   paneId{0};
    QString                    paneLabel;
    QColor                     paneColor;
    QVector<InspectLayerEntry> layers;
};

// What one Scan/Pixel Profile **Compute** covers (FR-ANL-12), plus the two things the panel
// cannot work out for itself. MainWindow assembles it, because only MainWindow knows the sync
// roles and the Layers-panel selection.
//
// `fromSelection` decides whether a multi-pane plot is a **sync merge** — discarded when the
// panes stop moving together — or a comparison the user assembled by selecting layers, which
// outlives any sync change. Both look identical once the curves are drawn, so the distinction
// has to travel with the scope.
//
// `hiddenSkipped` counts rasters the user selected but which are not visible. They are left
// out, because every scope rule in the app is written in terms of visible rasters — but the
// user picked them deliberately, so silence would look like a bug.
struct FvProfileScope {
    QVector<InspectPaneGroup> groups;
    int                       hiddenSkipped{0};
    bool                      fromSelection{false};

    FvProfileScope() = default;
    // Implicit on purpose: a caller with nothing to say beyond the groups (every test, and the
    // sync/active-layer paths) just returns them.
    FvProfileScope(QVector<InspectPaneGroup> g) : groups(std::move(g)) {}
};
