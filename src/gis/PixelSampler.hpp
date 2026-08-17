#pragma once
#include <string>
#include <vector>

class RasterLayer;

struct PixelPatchCell {
    double value{std::numeric_limits<double>::quiet_NaN()};
    bool   is_valid{false};
    bool   is_nodata{false};
};

struct PixelPatchSample {
    int center_col{-1};
    int center_row{-1};
    int start_col{0};
    int start_row{0};
    int window_size{11};
    int raster_w{0};
    int raster_h{0};
    int band_count{0};
    bool is_rgb{false};
    int r_band{1};
    int g_band{1};
    int b_band{1};
    // channels[channel_idx][row_idx 0..window_size-1][col_idx 0..window_size-1]
    std::vector<std::vector<std::vector<PixelPatchCell>>> channels;
    bool valid{false};
};

// Read every band of `rl` at one geographic point — the single implementation shared by the
// Pixel Inspector table and the Spectral Plot (Phase 26), so the number a user reads in the
// table is exactly the number the curve is drawn from.
//
// (geo_x, geo_y) are expressed in `geoWkt` — the clicked pane's Project CRS. The point is
// transformed geoWkt → the layer's SOURCE CRS before sampling, so analysis reads the correct
// source pixel under on-the-fly reprojection (FR-CRS-4). An empty `geoWkt` means geographic.
//
// Returns false (leaving `out` empty) when the layer has no dataset, the point falls outside
// its grid, or the read fails. No-data samples come back as quiet NaN so callers can render
// them as gaps rather than spikes.
bool fvSamplePixelBands(RasterLayer* rl, double geo_x, double geo_y,
                        const std::string& geoWkt, std::vector<double>& out);

// Sample an N x N patch (default 11x11) centered around (geo_x, geo_y) in `rl`.
// Reads the mapped display channels (RGB or Gray) and fills out PixelPatchSample.
bool fvSamplePixelPatch(RasterLayer* rl, double geo_x, double geo_y,
                        const std::string& geoWkt, int window_size, PixelPatchSample& out);

