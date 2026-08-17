#include "gis/PixelSampler.hpp"
#include "gis/CrsUtil.hpp"
#include "core/RasterLayer.hpp"
#include "io/RasterDataset.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

bool fvSamplePixelBands(RasterLayer* rl, double geo_x, double geo_y,
                        const std::string& geoWkt, std::vector<double>& out) {
    out.clear();
    if (!rl) return false;
    auto* ds = rl->dataset();
    if (!ds) return false;

    // The point is in the clicked pane's Project CRS; transform into this layer's SOURCE CRS
    // so analysis reads the correct source pixel under on-the-fly reprojection (FR-CRS-4).
    double sx = geo_x, sy = geo_y;
    fvTransformPoint(geoWkt, ds->crsWkt(), sx, sy);
    auto px = ds->geoTransform().geoToPixel(sx, sy);
    int col = static_cast<int>(std::round(px.x));
    int row = static_cast<int>(std::round(px.y));
    if (col < 0 || row < 0 || col >= ds->width() || row >= ds->height()) return false;

    TileBuffer buf = ds->readRegion(col, row, 1, 1, 1, 1);
    if (!buf.isValid()) return false;

    bool has_nd = rl->hasNoData();
    double nd_v = static_cast<double>(rl->noDataValue());
    double nd_eps = has_nd ? std::max(std::abs(nd_v) * 1e-5, 1e-10) : 0.0;

    for (int b = 0; b < buf.bands; ++b) {
        double v = static_cast<double>(buf.data[static_cast<size_t>(b)]);
        if (has_nd && std::abs(v - nd_v) < nd_eps)
            v = std::numeric_limits<double>::quiet_NaN();
        out.push_back(v);
    }
    return true;
}

bool fvSamplePixelPatch(RasterLayer* rl, double geo_x, double geo_y,
                        const std::string& geoWkt, int window_size, PixelPatchSample& out) {
    out = PixelPatchSample{};
    out.window_size = window_size;
    if (!rl) return false;
    auto* ds = rl->dataset();
    if (!ds) return false;

    double sx = geo_x, sy = geo_y;
    fvTransformPoint(geoWkt, ds->crsWkt(), sx, sy);
    auto px = ds->geoTransform().geoToPixel(sx, sy);
    int col = static_cast<int>(std::round(px.x));
    int row = static_cast<int>(std::round(px.y));

    out.center_col = col;
    out.center_row = row;
    out.raster_w = ds->width();
    out.raster_h = ds->height();
    out.band_count = ds->bandCount();

    if (col < 0 || row < 0 || col >= ds->width() || row >= ds->height()) {
        return false;
    }

    int radius = window_size / 2;
    int start_col = col - radius;
    int start_row = row - radius;
    out.start_col = start_col;
    out.start_row = start_row;

    const BandMapping& bm = rl->bandMapping();
    std::vector<int> bands_to_read;
    if (!bm.isGrayscale() && ds->bandCount() >= 3) {
        out.is_rgb = true;
        out.r_band = bm.red_idx;
        out.g_band = bm.green_idx;
        out.b_band = bm.blue_idx;
        bands_to_read = {out.r_band, out.g_band, out.b_band};
    } else {
        out.is_rgb = false;
        out.r_band = bm.grayBand();
        bands_to_read = {out.r_band};
    }

    // Allocate channels grid
    int num_channels = static_cast<int>(bands_to_read.size());
    out.channels.resize(static_cast<size_t>(num_channels));
    for (int ch = 0; ch < num_channels; ++ch) {
        out.channels[static_cast<size_t>(ch)].resize(static_cast<size_t>(window_size));
        for (int r = 0; r < window_size; ++r) {
            out.channels[static_cast<size_t>(ch)][static_cast<size_t>(r)].resize(static_cast<size_t>(window_size));
        }
    }

    // Calculate valid intersecting rectangle
    int read_col0 = std::max(0, start_col);
    int read_row0 = std::max(0, start_row);
    int read_col1 = std::min(ds->width(), start_col + window_size);
    int read_row1 = std::min(ds->height(), start_row + window_size);
    int read_w = read_col1 - read_col0;
    int read_h = read_row1 - read_row0;

    TileBuffer buf;
    if (read_w > 0 && read_h > 0) {
        buf = ds->readRegion(read_col0, read_row0, read_w, read_h, read_w, read_h, bands_to_read);
    }

    bool has_nd = rl->hasNoData();
    double nd_v = static_cast<double>(rl->noDataValue());
    double nd_eps = has_nd ? std::max(std::abs(nd_v) * 1e-5, 1e-10) : 0.0;

    for (int r = 0; r < window_size; ++r) {
        int cur_r = start_row + r;
        for (int c = 0; c < window_size; ++c) {
            int cur_c = start_col + c;
            bool in_bounds = (cur_c >= 0 && cur_c < ds->width() && cur_r >= 0 && cur_r < ds->height());

            for (int ch = 0; ch < num_channels; ++ch) {
                PixelPatchCell& cell = out.channels[static_cast<size_t>(ch)][static_cast<size_t>(r)][static_cast<size_t>(c)];
                if (!in_bounds || !buf.isValid()) {
                    cell.is_valid = false;
                    cell.value = std::numeric_limits<double>::quiet_NaN();
                } else {
                    int buf_c = cur_c - read_col0;
                    int buf_r = cur_r - read_row0;
                    size_t idx = static_cast<size_t>(ch) * (static_cast<size_t>(read_w) * read_h) +
                                 static_cast<size_t>(buf_r) * read_w + static_cast<size_t>(buf_c);
                    double v = static_cast<double>(buf.data[idx]);
                    if (has_nd && std::abs(v - nd_v) < nd_eps) {
                        cell.is_valid = true;
                        cell.is_nodata = true;
                        cell.value = std::numeric_limits<double>::quiet_NaN();
                    } else if (std::isnan(v)) {
                        cell.is_valid = true;
                        cell.is_nodata = true;
                        cell.value = std::numeric_limits<double>::quiet_NaN();
                    } else {
                        cell.is_valid = true;
                        cell.is_nodata = false;
                        cell.value = v;
                    }
                }
            }
        }
    }

    out.valid = true;
    return true;
}

