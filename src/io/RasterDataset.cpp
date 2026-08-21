#include "io/RasterDataset.hpp"
#include "io/PyramidBuilder.hpp"
#include "gis/GeoTransform4326.hpp"
#include "util/Logger.hpp"
#include "util/Percentile.hpp"

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

RasterDataset::~RasterDataset() {
    // Warped VRTs (and any owned effective-georef source) reference m_ds, so tear them
    // down BEFORE closing m_ds. Close the warped VRT first, then its owned source.
    for (auto& [wkt, h] : m_warp_cache) {
        if (h.warped)    GDALClose(h.warped);
        if (h.owned_src) GDALClose(h.owned_src);
    }
    m_warp_cache.clear();
    if (m_ds) {
        GDALClose(m_ds);
        m_ds = nullptr;
    }
}

std::shared_ptr<RasterDataset> RasterDataset::open(const std::string& path) {
    GDALAllRegister();

    GDALDataset* ds = static_cast<GDALDataset*>(
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));

    if (!ds) {
        FV_ERROR("RasterDataset::open failed for '{}'", path);
        return nullptr;
    }

    // Malformed/untrusted-data containment (FR-SEC-4 / FR-ERR-6): reject rasters
    // whose declared dimensions or band count are implausible before we allocate
    // any tile buffers, so a corrupt header cannot drive a huge/UB allocation.
    {
        const long long w = ds->GetRasterXSize();
        const long long h = ds->GetRasterYSize();
        const int       n = ds->GetRasterCount();
        constexpr long long kMaxDim   = 1'000'000;      // 1e6 px per side
        constexpr long long kMaxPix   = 100'000'000'000LL; // 1e11 px total
        constexpr int       kMaxBands = 16384;
        if (w <= 0 || h <= 0 || n < 0 || w > kMaxDim || h > kMaxDim ||
            n > kMaxBands || w * h > kMaxPix) {
            FV_ERROR("RasterDataset::open rejected implausible dims {}x{}x{} for '{}'",
                     w, h, n, path);
            GDALClose(ds);
            return nullptr;
        }
    }

    auto rd = std::shared_ptr<RasterDataset>(new RasterDataset());
    rd->m_ds         = ds;
    rd->m_path       = path;
    rd->m_width      = ds->GetRasterXSize();
    rd->m_height     = ds->GetRasterYSize();
    rd->m_band_count = ds->GetRasterCount();

    double gt[6];
    if (ds->GetGeoTransform(gt) == CE_None) {
        std::memcpy(rd->m_geotransform.gt, gt, sizeof(gt));
    }
    rd->m_extent = rd->m_geotransform.extent(rd->m_width, rd->m_height);

    const char* wkt = ds->GetProjectionRef();
    rd->m_crs_wkt   = wkt ? wkt : "";

    rd->m_stats_cache.resize(static_cast<size_t>(rd->m_band_count));
    rd->m_nodata_cache.resize(static_cast<size_t>(rd->m_band_count));
    rd->m_datatype_cache.assign(static_cast<size_t>(rd->m_band_count), GDT_Unknown);
    rd->m_colortable_cache.assign(static_cast<size_t>(rd->m_band_count), false);
    for (int b = 1; b <= rd->m_band_count; ++b) {
        GDALRasterBand* band = ds->GetRasterBand(b);
        int ok = 0;
        double v = band->GetNoDataValue(&ok);
        if (ok) rd->m_nodata_cache[static_cast<size_t>(b-1)] = {true, v};
        rd->m_datatype_cache[static_cast<size_t>(b-1)]   = static_cast<int>(band->GetRasterDataType());
        rd->m_colortable_cache[static_cast<size_t>(b-1)] = (band->GetColorTable() != nullptr);
    }

    // FR-CAP-3: report — never silently truncate — data FlashViewer cannot faithfully
    // represent. The display/analysis pipeline is real-valued Float32, so a complex
    // band's imaginary component is dropped; surface that to the user via the log
    // (Log dock) rather than showing the real part with no notice.
    {
        int complex_bands = 0;
        for (int b = 0; b < rd->m_band_count; ++b)
            if (GDALDataTypeIsComplex(
                    static_cast<GDALDataType>(rd->m_datatype_cache[static_cast<size_t>(b)])))
                ++complex_bands;
        if (complex_bands > 0) {
            rd->m_has_unrepresentable = true;
            FV_WARN("RasterDataset::open '{}': {} complex band(s) shown as their real "
                    "component only — the imaginary part is not represented (FR-CAP-3).",
                    path, complex_bands);
        }
    }

    FV_INFO("Opened '{}': {}x{}x{} bands", path,
            rd->m_width, rd->m_height, rd->m_band_count);
    return rd;
}

bool RasterDataset::isGeoTransformIdentity() const {
    const double* g = m_geotransform.gt;
    return (std::abs(g[0]) < 1e-10 && std::abs(g[1] - 1.0) < 1e-10
         && std::abs(g[2]) < 1e-10 && std::abs(g[3]) < 1e-10
         && std::abs(g[4]) < 1e-10 && std::abs(std::abs(g[5]) - 1.0) < 1e-10);
}

void RasterDataset::setGeoTransformOverride(const double gt[6]) {
    std::memcpy(m_geotransform.gt, gt, 6 * sizeof(double));
    m_extent = m_geotransform.extent(m_width, m_height);
    FV_INFO("RasterDataset: geotransform override applied to '{}'", m_path);
}

void RasterDataset::setCrsOverride(const std::string& wkt) {
    m_crs_wkt = wkt;
    m_is_geographic.reset();   // re-evaluate on next isGeographic()
    FV_INFO("RasterDataset: CRS override applied to '{}'", m_path);
}

bool RasterDataset::isGeographic() const {
    if (!m_is_geographic.has_value()) {
        bool geo = true;   // default: assume geographic (degrees) when no CRS is known
        if (!m_crs_wkt.empty()) {
            OGRSpatialReference srs;
            if (srs.importFromWkt(m_crs_wkt.c_str()) == OGRERR_NONE)
                geo = (srs.IsGeographic() != 0);
        }
        m_is_geographic = geo;
    }
    return *m_is_geographic;
}

std::shared_ptr<GeoTransform4326> RasterDataset::geoTransform4326() const {
    return std::make_shared<GeoTransform4326>(m_geotransform, m_crs_wkt);
}

std::shared_ptr<CrsTransformer> RasterDataset::wgs84Transformer() const {
    return fvGetWgs84Transformer(m_crs_wkt);
}

std::string RasterDataset::bandDescription(int band_1based) const {
    std::lock_guard lock(m_mutex);
    if (!m_ds || band_1based < 1 || band_1based > m_band_count)
        return {};
    const char* desc = m_ds->GetRasterBand(band_1based)->GetDescription();
    return desc ? desc : ("Band " + std::to_string(band_1based));
}

int RasterDataset::bandDataType(int band_1based) const {
    if (band_1based < 1 || band_1based > static_cast<int>(m_datatype_cache.size()))
        return -1;
    return m_datatype_cache[static_cast<size_t>(band_1based - 1)];
}

bool RasterDataset::bandHasColorTable(int band_1based) const {
    if (band_1based < 1 || band_1based > static_cast<int>(m_colortable_cache.size()))
        return false;
    return m_colortable_cache[static_cast<size_t>(band_1based - 1)];
}

bool RasterDataset::hasOverviews(int band_1based) const {
    return overviewCount(band_1based) > 0;
}

int RasterDataset::overviewCount(int band_1based) const {
    std::lock_guard lock(m_mutex);
    if (!m_ds || band_1based < 1 || band_1based > m_band_count) return 0;
    GDALRasterBand* band = m_ds->GetRasterBand(band_1based);
    return band ? band->GetOverviewCount() : 0;
}

// --------------------------------------------------------------------------
// Reprojection helpers (Raster Math cross-CRS evaluation)

GDALDataset* RasterDataset::makeWarpSource(bool& owned) const {
    owned = false;
    if (!m_ds) return nullptr;

    // Does the open handle already carry the effective georef? (No override applied.)
    double gtDs[6];
    const bool gtMatches = (m_ds->GetGeoTransform(gtDs) == CE_None) &&
                           std::memcmp(gtDs, m_geotransform.gt, sizeof(gtDs)) == 0;
    const char* projDs = m_ds->GetProjectionRef();
    const bool projMatches = projDs && (m_crs_wkt == projDs);
    if (gtMatches && projMatches) return m_ds;   // warp the raw handle directly

    // An override is active → wrap m_ds in a VRT stamped with the effective georef so
    // the warp uses the user-assigned geotransform/CRS (e.g. non-CF NetCDF).
    GDALDriver* vrtDrv = GetGDALDriverManager()->GetDriverByName("VRT");
    if (!vrtDrv) return m_ds;
    GDALDataset* vrt = vrtDrv->CreateCopy("", m_ds, FALSE, nullptr, nullptr, nullptr);
    if (!vrt) return m_ds;
    vrt->SetGeoTransform(const_cast<double*>(m_geotransform.gt));
    if (!m_crs_wkt.empty()) vrt->SetProjection(m_crs_wkt.c_str());
    owned = true;
    return vrt;
}

namespace {
std::string fvFmt(double v) { char b[64]; std::snprintf(b, sizeof b, "%.17g", v); return b; }

// Canonicalize a WKT/user CRS string so equivalent spellings (EPSG code vs full WKT,
// axis-order/whitespace differences) collapse to one cache key. Returns "" when the
// input is empty or cannot be parsed (caller treats "" as identity/geographic).
std::string fvNormalizeWkt(const std::string& wkt) {
    if (wkt.empty()) return {};
    OGRSpatialReference srs;
    if (srs.SetFromUserInput(wkt.c_str()) != OGRERR_NONE) return {};
    char* out = nullptr;
    if (srs.exportToWkt(&out) != OGRERR_NONE || !out) { CPLFree(out); return {}; }
    std::string s(out);
    CPLFree(out);
    return s;
}

// True when two CRS strings denote the same reference system (robust to spelling).
bool fvSameCrs(const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (a.empty() || b.empty()) return false;
    OGRSpatialReference sa, sb;
    if (sa.SetFromUserInput(a.c_str()) != OGRERR_NONE) return false;
    if (sb.SetFromUserInput(b.c_str()) != OGRERR_NONE) return false;
    return sa.IsSame(&sb) == TRUE;
}
}  // namespace

GDALDataset* RasterDataset::warpToGrid(const std::string& dstWkt, const double dstGt[6],
                                       int dstW, int dstH,
                                       bool hasNoData, double noDataVal,
                                       const std::string& resampling) const {
    std::lock_guard lock(m_mutex);
    if (dstWkt.empty() || dstW < 1 || dstH < 1) return nullptr;
    bool owned = false;
    GDALDataset* src = makeWarpSource(owned);
    if (!src) return nullptr;

    GeoTransform tgt; std::memcpy(tgt.gt, dstGt, sizeof(tgt.gt));
    const Extent te = tgt.extent(dstW, dstH);
    const std::string ralg = resampling.empty() ? std::string("bilinear") : resampling;

    std::vector<std::string> a = {
        "-of", "MEM", "-t_srs", dstWkt,
        "-te", fvFmt(te.xmin), fvFmt(te.ymin), fvFmt(te.xmax), fvFmt(te.ymax),
        "-ts", std::to_string(dstW), std::to_string(dstH),
        "-r", ralg
    };
    if (hasNoData) {
        // Register src/dst no-data AND keep no-data out of the resampling kernel so cubic/
        // lanczos (and bilinear) do not smear the sentinel into valid pixels at swath edges —
        // the CPU-warp analogue of the GPU display footprint guard (FR-ACC-3, SDD §7.3).
        a.insert(a.end(), {"-srcnodata", fvFmt(noDataVal), "-dstnodata", fvFmt(noDataVal),
                           "-wo", "UNIFIED_SRC_NODATA=YES", "-wo", "INIT_DEST=NO_DATA"});
    }
    std::vector<char*> argv;
    for (auto& s : a) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    GDALWarpAppOptions* opts = GDALWarpAppOptionsNew(argv.data(), nullptr);
    GDALDatasetH srcH = static_cast<GDALDatasetH>(src);
    GDALDatasetH out = GDALWarp("", nullptr, 1, &srcH, opts, nullptr);
    GDALWarpAppOptionsFree(opts);
    if (owned) GDALClose(src);
    return static_cast<GDALDataset*>(out);
}

bool RasterDataset::suggestWarpedGrid(const std::string& dstWkt,
                                      double outGt[6], int& outW, int& outH) const {
    std::lock_guard lock(m_mutex);
    if (dstWkt.empty()) return false;
    bool owned = false;
    GDALDataset* src = makeWarpSource(owned);
    if (!src) return false;

    // -of VRT is lazy: GDAL computes the output extent/size without warping any pixels.
    std::vector<std::string> a = {"-of", "VRT", "-t_srs", dstWkt, "-r", "bilinear"};
    std::vector<char*> argv;
    for (auto& s : a) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    GDALWarpAppOptions* opts = GDALWarpAppOptionsNew(argv.data(), nullptr);
    GDALDatasetH srcH = static_cast<GDALDatasetH>(src);
    GDALDatasetH out = GDALWarp("", nullptr, 1, &srcH, opts, nullptr);
    GDALWarpAppOptionsFree(opts);
    if (owned) GDALClose(src);
    if (!out) return false;

    auto* od = static_cast<GDALDataset*>(out);
    bool ok = (od->GetGeoTransform(outGt) == CE_None);
    outW = od->GetRasterXSize();
    outH = od->GetRasterYSize();
    GDALClose(out);
    return ok && outW > 0 && outH > 0;
}

// --- On-the-fly display reprojection (Phase 11) -----------------------------------

RasterDataset::WarpedHandle*
RasterDataset::ensureWarpedLocked(const std::string& norm_wkt,
                                  const std::string& resampling) const {
    auto it = m_warp_cache.find(norm_wkt);
    if (it != m_warp_cache.end()) return &it->second;

    WarpedHandle h;
    bool owned = false;
    GDALDataset* src = makeWarpSource(owned);
    if (src) {
        // -of VRT keeps the warp lazy (a VRTWarpedDataset computes output per RasterIO),
        // so only the tiles actually read are reprojected — the tile-scoped behaviour
        // NFR-PERF-7 relies on. The no-data options keep the sentinel out of the
        // resampling kernel (CPU-warp analogue of the GPU footprint guard, FR-ACC-3),
        // mirroring warpToGrid() exactly.
        const std::string ralg = resampling.empty() ? std::string("bilinear") : resampling;
        const bool hasND = !m_nodata_cache.empty() && m_nodata_cache[0].has_value;
        const double ndVal = hasND ? m_nodata_cache[0].value : 0.0;

        std::vector<std::string> a = {"-of", "VRT", "-t_srs", norm_wkt, "-r", ralg};
        if (hasND) {
            // Declared no-data: fill uncovered/masked output with the sentinel (the shader
            // discards it) and keep it out of the resampling kernel.
            a.insert(a.end(), {"-srcnodata", fvFmt(ndVal), "-dstnodata", fvFmt(ndVal),
                               "-wo", "UNIFIED_SRC_NODATA=YES", "-wo", "INIT_DEST=NO_DATA"});
        } else {
            // No declared no-data: add a coverage (alpha) band so readWarpedRegion can NaN-out
            // the uncovered footprint corners — otherwise GDAL's 0-fill renders as opaque black
            // (or white under an inverted colormap). Works for any pixel type.
            a.insert(a.end(), {"-dstalpha"});
        }
        std::vector<char*> argv;
        for (auto& s : a) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        GDALWarpAppOptions* opts = GDALWarpAppOptionsNew(argv.data(), nullptr);
        GDALDatasetH srcH = static_cast<GDALDatasetH>(src);
        // Suppress GDAL/PROJ errors during the warp build. A layer that cannot be reprojected
        // into the pane CRS (e.g. "utm: Invalid latitude") is handled by the native-CRS
        // fallback + a clean, non-modal notice (Phase 17 #4); the raw CPLError must NOT leak to
        // the global error banner. CPLGetLastErrorMsg stays available for the FV_WARN below.
        CPLPushErrorHandler(CPLQuietErrorHandler);
        GDALDatasetH out  = GDALWarp("", nullptr, 1, &srcH, opts, nullptr);
        CPLPopErrorHandler();
        GDALWarpAppOptionsFree(opts);

        if (out) {
            auto* od = static_cast<GDALDataset*>(out);
            double gt[6];
            if (od->GetGeoTransform(gt) == CE_None &&
                od->GetRasterXSize() > 0 && od->GetRasterYSize() > 0) {
                h.warped    = od;
                h.owned_src = owned ? src : nullptr;   // must outlive the lazy warped VRT
                // -dstalpha appends a coverage band after the data bands; note its index.
                h.alpha_band = (od->GetRasterCount() > m_band_count) ? od->GetRasterCount() : 0;
                h.view.width  = od->GetRasterXSize();
                h.view.height = od->GetRasterYSize();
                std::memcpy(h.view.gt.gt, gt, sizeof(gt));
                h.view.extent = h.view.gt.extent(h.view.width, h.view.height);
            } else {
                GDALClose(out);
            }
        }
    }

    if (!h.warped) {
        if (owned && src) GDALClose(src);
        h.view.failed = true;
        FV_WARN("RasterDataset::warpedView could not reproject '{}': {}",
                m_path, CPLGetLastErrorMsg());
    }
    auto res = m_warp_cache.emplace(norm_wkt, h);
    return &res.first->second;
}

RasterDataset::WarpedView
RasterDataset::warpedView(const std::string& project_wkt,
                          const std::string& resampling) const {
    WarpedView v;
    const std::string norm = fvNormalizeWkt(project_wkt);

    // No target CRS (or target == source) → draw raw; report source georef.
    if (norm.empty() || fvSameCrs(m_crs_wkt, norm)) {
        v.sameAsSource = true;
        v.width  = m_width;
        v.height = m_height;
        v.gt     = m_geotransform;
        v.extent = m_extent;
        return v;
    }
    // Target requested but this layer has no source CRS → cannot align (FR-CRS-5).
    if (m_crs_wkt.empty()) {
        v.failed = true;
        return v;
    }
    std::lock_guard lock(m_mutex);
    WarpedHandle* h = ensureWarpedLocked(norm, resampling);
    return h->view;
}

TileBuffer RasterDataset::readWarpedRegion(const std::string& project_wkt,
                                           int xoff, int yoff, int xsize, int ysize,
                                           int dstW, int dstH,
                                           const std::vector<int>& bands_1based,
                                           const std::string& resampling) const {
    const std::string norm = fvNormalizeWkt(project_wkt);
    // Identity / same-CRS → the untouched raw source read (no reprojection interpolation).
    if (norm.empty() || fvSameCrs(m_crs_wkt, norm))
        return readRegion(xoff, yoff, xsize, ysize, dstW, dstH, bands_1based);
    if (m_crs_wkt.empty()) return {};   // unreprojectable (failed view)

    std::lock_guard lock(m_mutex);
    WarpedHandle* h = ensureWarpedLocked(norm, resampling);
    if (!h->warped || dstW < 1 || dstH < 1) return {};
    GDALDataset* wds = h->warped;

    std::vector<int> band_list = bands_1based;
    if (band_list.empty()) {
        band_list.resize(static_cast<size_t>(m_band_count));
        std::iota(band_list.begin(), band_list.end(), 1);
    }
    const int nb = static_cast<int>(band_list.size());

    TileBuffer buf;
    buf.width  = dstW;
    buf.height = dstH;
    buf.bands  = nb;
    buf.data.resize(static_cast<size_t>(dstW) * dstH * nb, 0.0f);

    const CPLErr err = wds->RasterIO(
        GF_Read, xoff, yoff, xsize, ysize,
        buf.data.data(), dstW, dstH, GDT_Float32,
        nb, band_list.data(), 0, 0,
        static_cast<GSpacing>(dstW) * dstH * sizeof(float), nullptr);
    if (err != CE_None) {
        FV_ERROR("RasterDataset::readWarpedRegion RasterIO failed (err={})", static_cast<int>(err));
        return {};
    }

    // Coverage mask: where the warp produced no source coverage (the rotated footprint's
    // corners), the alpha band is ~0 — mark those pixels NaN so the shader discards them
    // (FR-RND-8) instead of drawing GDAL's 0-fill as opaque black. Only when there is no
    // declared no-data (the declared-no-data path already fills corners with the sentinel).
    if (h->alpha_band > 0) {
        std::vector<float> alpha(static_cast<size_t>(dstW) * dstH, 0.0f);
        int aidx = h->alpha_band;
        const CPLErr aerr = wds->RasterIO(
            GF_Read, xoff, yoff, xsize, ysize,
            alpha.data(), dstW, dstH, GDT_Float32,
            1, &aidx, 0, 0, 0, nullptr);
        if (aerr == CE_None) {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            const size_t px = static_cast<size_t>(dstW) * dstH;
            for (size_t i = 0; i < px; ++i) {
                if (alpha[i] < 128.0f) {          // < 50% coverage → treat as uncovered
                    for (int b = 0; b < nb; ++b)
                        buf.data[static_cast<size_t>(b) * px + i] = nan;
                }
            }
        }
    }
    return buf;
}

TileBuffer RasterDataset::readRegion(int xoff, int yoff,
                                      int xsize, int ysize,
                                      int dstW, int dstH,
                                      const std::vector<int>& bands_1based) const {
    std::lock_guard lock(m_mutex);
    if (!m_ds) return {};

    std::vector<int> band_list = bands_1based;
    if (band_list.empty()) {
        band_list.resize(static_cast<size_t>(m_band_count));
        std::iota(band_list.begin(), band_list.end(), 1);
    }
    int nb = static_cast<int>(band_list.size());

    TileBuffer buf;
    buf.width  = dstW;
    buf.height = dstH;
    buf.bands  = nb;
    buf.data.resize(static_cast<size_t>(dstW) * dstH * nb, 0.0f);

    CPLErr err = m_ds->RasterIO(
        GF_Read,
        xoff, yoff, xsize, ysize,
        buf.data.data(),
        dstW, dstH,
        GDT_Float32,
        nb, band_list.data(),
        0,
        0,
        static_cast<GSpacing>(dstW) * dstH * sizeof(float),
        nullptr);

    if (err != CE_None) {
        FV_ERROR("RasterDataset::readRegion RasterIO failed (err={})", static_cast<int>(err));
        return {};
    }
    return buf;
}

TileBuffer RasterDataset::readFullPreview(int maxSize) const {
    int dstW = m_width;
    int dstH = m_height;
    if (dstW > maxSize || dstH > maxSize) {
        double factor = std::min(
            static_cast<double>(maxSize) / dstW,
            static_cast<double>(maxSize) / dstH);
        dstW = std::max(1, static_cast<int>(dstW * factor));
        dstH = std::max(1, static_cast<int>(dstH * factor));
    }
    return readRegion(0, 0, m_width, m_height, dstW, dstH);
}

TileBuffer RasterDataset::readTile(int zoom, int tx, int ty,
                                    int tile_size,
                                    const std::vector<int>& bands_1based) const {
    int n_tiles_axis = 1 << zoom;

    int src_w = (m_width  + n_tiles_axis - 1) / n_tiles_axis;
    int src_h = (m_height + n_tiles_axis - 1) / n_tiles_axis;

    int xoff = tx * src_w;
    int yoff = ty * src_h;

    if (xoff >= m_width || yoff >= m_height) return {};
    int actual_w = std::min(src_w, m_width  - xoff);
    int actual_h = std::min(src_h, m_height - yoff);

    int out_w = std::max(1, tile_size * actual_w / src_w);
    int out_h = std::max(1, tile_size * actual_h / src_h);

    return readRegion(xoff, yoff, actual_w, actual_h, out_w, out_h, bands_1based);
}

RasterDataset::NoDataInfo RasterDataset::noData(int band_1based) const {
    size_t idx = static_cast<size_t>(band_1based - 1);
    if (idx >= m_nodata_cache.size()) return {};
    return m_nodata_cache[idx];
}

RasterDataset::BandStats RasterDataset::bandStats(int band_1based) const {
    const int idx = band_1based - 1;
    {
        std::lock_guard lock(m_mutex);
        if (idx < 0 || idx >= static_cast<int>(m_stats_cache.size()))
            return {};
        if (m_stats_cache[static_cast<size_t>(idx)].has_value())
            return *m_stats_cache[static_cast<size_t>(idx)];
    }

    BandStats st;
    {
        std::lock_guard lock(m_mutex);
        if (!m_ds) return {};
        GDALRasterBand* band = m_ds->GetRasterBand(band_1based);
        band->ComputeStatistics(TRUE, &st.min, &st.max, &st.mean, &st.stddev,
                                nullptr, nullptr);
        m_stats_cache[static_cast<size_t>(idx)] = st;
    }
    FV_DEBUG("Band {} stats: min={:.3f} max={:.3f} mean={:.3f}",
             band_1based, st.min, st.max, st.mean);
    return st;
}

void RasterDataset::invalidateStatsCache() {
    std::lock_guard lock(m_mutex);
    for (auto& entry : m_stats_cache) entry.reset();
}

void RasterDataset::flushCache() {
    std::lock_guard lock(m_mutex);
    if (!m_ds) return;
    m_ds->FlushCache(false);
}

namespace {
struct RasterDatasetProgressCtx {
    RasterDataset::ProgressFn fn;
};

int CPL_STDCALL rasterDatasetProgressBridge(double dfComplete, const char*, void* pArg) {
    if (!pArg) return TRUE;
    auto* ctx = static_cast<RasterDatasetProgressCtx*>(pArg);
    if (!ctx->fn) return TRUE;
    return ctx->fn(dfComplete) ? TRUE : FALSE;
}
}  // namespace

bool RasterDataset::buildOverviews(const std::string& resampling, ProgressFn progressCb) {
    std::lock_guard lock(m_mutex);
    if (!m_ds) return false;

    auto levels = PyramidBuilder::computeOverviewLevels(m_width, m_height);
    if (levels.empty()) return true;  // small enough that no overviews are needed

    RasterDatasetProgressCtx ctx{std::move(progressCb)};
    const CPLErr err = m_ds->BuildOverviews(
        resampling.c_str(), static_cast<int>(levels.size()), levels.data(),
        0, nullptr,
        ctx.fn ? rasterDatasetProgressBridge : nullptr,
        ctx.fn ? &ctx : nullptr);
    if (err != CE_None) {
        FV_WARN("RasterDataset::buildOverviews failed for '{}' (err={})", m_path, static_cast<int>(err));
        return false;
    }
    FV_DEBUG("RasterDataset::buildOverviews: built {} levels for '{}'", levels.size(), m_path);
    return true;
}

void RasterDataset::setNoDataOverride(double value) {
    std::lock_guard lock(m_mutex);
    if (!m_ds) return;
    for (int b = 1; b <= m_band_count; ++b) {
        GDALRasterBand* band = m_ds->GetRasterBand(b);
        if (!band) continue;
        band->SetNoDataValue(value);
        m_nodata_cache[static_cast<size_t>(b - 1)] = {true, value};
    }
}

std::pair<float, float> RasterDataset::computeStretchPercentile(
    int band_1based, double lo_pct, double hi_pct) const
{
    // Read a small preview (up to 512px) to keep this cheap.
    const int kMaxPrev = 512;
    int dstW = m_width, dstH = m_height;
    if (dstW > kMaxPrev || dstH > kMaxPrev) {
        double f = std::min(static_cast<double>(kMaxPrev) / dstW,
                            static_cast<double>(kMaxPrev) / dstH);
        dstW = std::max(1, static_cast<int>(dstW * f));
        dstH = std::max(1, static_cast<int>(dstH * f));
    }
    TileBuffer buf = readRegion(0, 0, m_width, m_height, dstW, dstH, {band_1based});
    if (!buf.isValid() || buf.bands < 1) {
        FV_DEBUG("computeStretchPercentile band {}: readRegion({}x{} decimated from {}x{}) "
                 "returned {}", band_1based, dstW, dstH, m_width, m_height,
                 buf.isValid() ? "0 bands" : "invalid buffer");
        return {0.0f, 1.0f};
    }

    NoDataInfo nd = noData(band_1based);
    const float* src = buf.bandPtr(0);
    const int n = dstW * dstH;

    std::vector<float> vals;
    vals.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        float v = src[i];
        if (!std::isfinite(v)) continue;
        if (nd.has_value && std::abs(static_cast<double>(v) - nd.value) < 1e-6) continue;
        vals.push_back(v);
    }
    // Diagnostic for the "MEM live session reads all-zero stats despite real
    // pixel-inspect values" investigation: pins down whether the decimated
    // readRegion() call itself returned real data (rawMin/rawMax over ALL n
    // samples, before the nodata filter) or whether the nodata filter is
    // discarding everything that got read.
    if (n > 0) {
        float rawMin = src[0], rawMax = src[0];
        for (int i = 1; i < n; ++i) { rawMin = std::min(rawMin, src[i]); rawMax = std::max(rawMax, src[i]); }
        FV_DEBUG("computeStretchPercentile band {}: n={} valid={} nd.has_value={} nd.value={:.3f} "
                 "raw[min={:.3f},max={:.3f}]", band_1based, n, vals.size(), nd.has_value, nd.value,
                 rawMin, rawMax);
    }
    if (vals.empty()) return {0.0f, 1.0f};

    // Single percentile algorithm shared with HistogramPanel (FR-HST-5).
    std::sort(vals.begin(), vals.end());
    float lo = fvPercentileToValue(vals, lo_pct);
    float hi = fvPercentileToValue(vals, hi_pct);
    if (lo >= hi) hi = lo + 1.0f;
    return {lo, hi};
}
