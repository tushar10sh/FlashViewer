#pragma once
#include <string>
#include <vector>
#include <functional>
#include <utility>

class GDALDataset;

class PyramidBuilder {
public:
    /// Check if a GDAL dataset already has overviews.
    static bool hasOverviews(GDALDataset* ds);

    /// Check if a raster on disk already has overviews (either internal or external .ovr / .rrd).
    static bool hasOverviews(const std::string& path);

    /// Query raster dimensions without full open.
    static std::pair<int, int> getRasterDimensions(const std::string& path);

    /// Returns true if the file is a local raster that would benefit from pyramids
    /// (e.g. max dimension >= minDim, no existing overviews, and supported format).
    static bool shouldPromptPyramids(const std::string& path, int minDim = 1024);

    /// Remember that the user answered or dismissed the prompt for this path in the current session.
    static void dismissPrompt(const std::string& path);

    /// Compute standard binary pyramid levels for given dimensions (e.g. 2, 4, 8, 16...).
    static std::vector<int> computeOverviewLevels(int width, int height, int minTileDim = 128);

    /// Progress callback: receives fraction complete [0.0, 1.0].
    /// Return true to continue, false to cancel.
    using ProgressFn = std::function<bool(double fraction)>;

    /// Build binary overviews (external .ovr file) for a raster.
    /// Resampling can be "AVERAGE", "NEAREST", "GAUSS", "CUBIC", etc.
    /// Returns true on success, false on error or user cancellation.
    static bool buildPyramids(const std::string& path,
                             const std::string& resampling = "AVERAGE",
                             ProgressFn progressCb = nullptr);
};
