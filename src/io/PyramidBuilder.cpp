#include "io/PyramidBuilder.hpp"
#include "util/Logger.hpp"

#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_string.h>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace {
struct ProgressContext {
    PyramidBuilder::ProgressFn fn;
};

int CPL_STDCALL gdalProgressBridge(double dfComplete, const char* /*pszMessage*/, void* pProgressArg) {
    if (!pProgressArg) return TRUE;
    auto* ctx = static_cast<ProgressContext*>(pProgressArg);
    if (!ctx->fn) return TRUE;
    bool proceed = ctx->fn(dfComplete);
    return proceed ? TRUE : FALSE;
}

bool isNetworkOrSpecialPath(const std::string& path) {
    return path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0
        || path.rfind("/vsicurl/", 0) == 0 || path.rfind("/vsis3/", 0) == 0
        || path.rfind("/vsigs/", 0) == 0 || path.rfind("/vsimem/", 0) == 0
        || path.rfind("NETCDF:", 0) == 0 || path.rfind("HDF5:", 0) == 0
        || path.rfind("HDF4_SDS:", 0) == 0;
}

bool hasOverviewFileOnDisk(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::path p(path);

    // Check <path>.ovr (e.g. image.tif.ovr)
    if (std::filesystem::exists(path + ".ovr", ec) || std::filesystem::exists(path + ".OVR", ec)) {
        return true;
    }
    // Check <stem>.ovr (e.g. image.ovr)
    auto pStem = p;
    pStem.replace_extension(".ovr");
    if (std::filesystem::exists(pStem, ec)) {
        return true;
    }
    // Check <path>.rrd (e.g. image.tif.rrd or image.rrd)
    if (std::filesystem::exists(path + ".rrd", ec) || std::filesystem::exists(path + ".RRD", ec)) {
        return true;
    }
    pStem.replace_extension(".rrd");
    if (std::filesystem::exists(pStem, ec)) {
        return true;
    }
    return false;
}

static std::mutex s_dismissed_mutex;
static std::unordered_set<std::string> s_dismissed_paths;
} // namespace

void PyramidBuilder::dismissPrompt(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard lock(s_dismissed_mutex);
    s_dismissed_paths.insert(path);
}

bool PyramidBuilder::hasOverviews(GDALDataset* ds) {
    if (!ds || ds->GetRasterCount() < 1) return false;
    for (int i = 1; i <= ds->GetRasterCount(); ++i) {
        GDALRasterBand* b = ds->GetRasterBand(i);
        if (b && b->GetOverviewCount() > 0) return true;
    }
    return false;
}

bool PyramidBuilder::hasOverviews(const std::string& path) {
    if (path.empty()) return false;

    // 1. Fast filesystem check for external overview files
    if (hasOverviewFileOnDisk(path)) {
        return true;
    }

    // 2. Open via GDAL to inspect internal overviews or driver-handled pyramids
    GDALAllRegister();
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return false;
    bool has = hasOverviews(ds);
    GDALClose(ds);
    return has;
}

std::pair<int, int> PyramidBuilder::getRasterDimensions(const std::string& path) {
    GDALAllRegister();
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return {0, 0};
    int w = ds->GetRasterXSize();
    int h = ds->GetRasterYSize();
    GDALClose(ds);
    return {w, h};
}

std::vector<int> PyramidBuilder::computeOverviewLevels(int width, int height, int minTileDim) {
    std::vector<int> levels;
    int maxDim = std::max(width, height);
    for (int factor = 2; maxDim / factor >= minTileDim; factor *= 2) {
        levels.push_back(factor);
    }
    if (levels.empty() && maxDim > minTileDim) {
        levels.push_back(2);
    }
    return levels;
}

bool PyramidBuilder::shouldPromptPyramids(const std::string& path, int minDim) {
    if (path.empty() || isNetworkOrSpecialPath(path)) return false;

    // Check if dismissed in this session
    {
        std::lock_guard lock(s_dismissed_mutex);
        if (s_dismissed_paths.count(path) > 0) return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return false;
    }

    // Fast check: if overview file exists on disk, never prompt
    if (hasOverviewFileOnDisk(path)) {
        return false;
    }

    // Ignore multi-variable raw container files (.nc, .hdf, .vrt, etc.)
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".nc" || ext == ".hdf" || ext == ".hdf5" || ext == ".h5" || ext == ".vrt") {
        return false;
    }

    GDALAllRegister();
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) return false;

    int w = ds->GetRasterXSize();
    int h = ds->GetRasterYSize();
    bool has = hasOverviews(ds);
    GDALClose(ds);

    if (has) return false;
    return (std::max(w, h) >= minDim);
}

bool PyramidBuilder::buildPyramids(const std::string& path,
                                   const std::string& resampling,
                                   ProgressFn progressCb) {
    if (path.empty()) return false;

    GDALAllRegister();
    // Open in read-only mode so GDAL generates an external .ovr binary file next to the raster
    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    if (!ds) {
        FV_ERROR("PyramidBuilder: could not open '{}' to build pyramids", path);
        return false;
    }

    int w = ds->GetRasterXSize();
    int h = ds->GetRasterYSize();
    auto levels = computeOverviewLevels(w, h);
    if (levels.empty()) {
        GDALClose(ds);
        return true;
    }

    // Configure standard 256x256 tiled blocks for the external TIFF .ovr
    CPLSetConfigOption("GDAL_TIFF_OVR_BLOCKSIZE", "256");

    ProgressContext ctx{std::move(progressCb)};
    FV_INFO("PyramidBuilder: generating {} overview levels for '{}' ({}x{})",
            levels.size(), path, w, h);

    CPLErr err = ds->BuildOverviews(
        resampling.c_str(),
        static_cast<int>(levels.size()),
        levels.data(),
        0,
        nullptr,
        ctx.fn ? gdalProgressBridge : nullptr,
        ctx.fn ? &ctx : nullptr
    );

    GDALClose(ds);

    if (err != CE_None) {
        FV_WARN("PyramidBuilder: BuildOverviews finished with error/cancel code {}", static_cast<int>(err));
        return false;
    }

    FV_INFO("PyramidBuilder: successfully built pyramids for '{}'", path);
    return true;
}
