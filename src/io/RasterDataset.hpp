#pragma once
#include "core/GeoTransform.hpp"
#include "core/Extent.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class GDALDataset;

struct TileBuffer {
    std::vector<float> data;
    int width{0};
    int height{0};
    int bands{0};

    bool isValid() const { return !data.empty() && width > 0 && height > 0 && bands > 0; }

    const float* bandPtr(int b) const {
        return data.data() + static_cast<size_t>(b) * width * height;
    }
    float* bandPtr(int b) {
        return data.data() + static_cast<size_t>(b) * width * height;
    }
};

class RasterDataset {
public:
    ~RasterDataset();

    static std::shared_ptr<RasterDataset> open(const std::string& path);

    int         width()      const { return m_width; }
    int         height()     const { return m_height; }
    int         bandCount()  const { return m_band_count; }
    std::string crsWkt()     const { return m_crs_wkt; }
    std::string filePath()   const { return m_path; }
    // True when the CRS is geographic (lon/lat degrees) rather than projected (linear units).
    // Parsed once from m_crs_wkt via OGR and cached; defaults to true when no CRS is known.
    // Used by the per-pane scale bar (and the CRS status label) to choose distance units.
    bool        isGeographic() const;
    GeoTransform geoTransform() const { return m_geotransform; }
    Extent       extent()       const { return m_extent; }

    // Returns true when the geotransform is the GDAL default (identity: origin=0,0, pixel=1x1).
    // Used to detect files opened without spatial reference.
    bool isGeoTransformIdentity() const;

    // Override the geotransform and CRS after construction (for non-CF NetCDF files
    // where the user manually assigns coordinate arrays via the assignment dialog).
    void setGeoTransformOverride(const double gt[6]);
    void setCrsOverride(const std::string& wkt);

    /// Return the GeoTransform4326 object bridging pixel (col, row), image geo (x, y), and WGS84 (lat, lon).
    std::shared_ptr<class GeoTransform4326> geoTransform4326() const;
    /// Return the cached CrsTransformer for this dataset's projection to EPSG:4326.
    std::shared_ptr<class CrsTransformer>   wgs84Transformer() const;

    std::string bandDescription(int band_1based) const;

    // The band's on-disk GDAL data type, returned as its GDALDataType int value (GDT_*), or -1
    // when out of range. Cached at open(). Kept as int so this header need not include GDAL.
    // Used by the Warp resampling data-aware default (FR-OPS-5): integer types → nearest.
    int  bandDataType(int band_1based) const;
    // True when the band carries a palette/colortable (categorical hint → nearest default).
    bool bandHasColorTable(int band_1based) const;

    /// True when the band has overview pyramids (internal or external .ovr).
    bool hasOverviews(int band_1based = 1) const;
    int  overviewCount(int band_1based = 1) const;

    // FR-CAP-3: true when the raster carries data FlashViewer cannot faithfully
    // represent in its real-valued Float32 pipeline (currently: complex bands, whose
    // imaginary part is dropped). Set at open(); a warning is also logged. Queryable so
    // the report can be surfaced/asserted rather than silently truncating.
    bool hasUnrepresentableData() const { return m_has_unrepresentable; }

    // --- Reprojection helpers (Raster Math cross-CRS evaluation) ----------------
    // Reproject/resample this dataset onto an explicit target grid (dstWkt CRS,
    // dstGt geotransform, dstW×dstH) via GDAL warp, returning an in-memory dataset
    // (caller GDALCloses it; nullptr on failure). All bands are warped, in source
    // order, so a caller reads the band it needs by its 1-based index. Honors any
    // geotransform/CRS override (the warp source carries the effective georef).
    // When hasNoData, src/dst no-data is set so masked regions keep the sentinel.
    // `resampling` is a GDAL `-r` token (near/bilinear/cubic/lanczos).
    GDALDataset* warpToGrid(const std::string& dstWkt, const double dstGt[6],
                            int dstW, int dstH,
                            bool hasNoData, double noDataVal,
                            const std::string& resampling = "bilinear") const;

    // Report the geotransform/size GDAL would produce warping this dataset into
    // dstWkt (auto extent + resolution). Returns false on failure. Used to derive a
    // common target grid for the "reproject to a pane's project CRS" mode.
    bool suggestWarpedGrid(const std::string& dstWkt,
                           double outGt[6], int& outW, int& outH) const;

    // --- On-the-fly display reprojection (Phase 11, FR-CRS-1..5) ----------------
    // Metadata of this dataset viewed reprojected into a pane's Project CRS. The
    // underlying warped VRT is built lazily and cached (keyed by normalized target
    // WKT); pixels are only warped when readWarpedRegion() reads a window.
    struct WarpedView {
        int width{0};
        int height{0};
        GeoTransform gt;       // geotransform in the target (Project) CRS
        Extent       extent;   // extent in the target (Project) CRS
        bool sameAsSource{false}; // target CRS == source CRS (or target empty) → use raw path
        bool failed{false};       // reprojection could not be built (FR-CRS-5)
        bool valid() const { return sameAsSource || (!failed && width > 0 && height > 0); }
    };

    // Return the reprojected view metadata for project_wkt. Empty project_wkt (or a
    // target equal to the source CRS) yields sameAsSource=true (caller uses the raw
    // extent/geotransform). A missing source CRS or an unbuildable warp yields
    // failed=true (caller warns + omits/best-effort per FR-CRS-5). Never throws.
    WarpedView warpedView(const std::string& project_wkt,
                          const std::string& resampling = "bilinear") const;

    // Read a region of this dataset REPROJECTED into project_wkt. Coordinates
    // (xoff,yoff,xsize,ysize) are in the WARPED grid's pixel space (see warpedView());
    // the window is resampled to dstW×dstH. Delegates to readRegion() (source pixels,
    // no interpolation) when the target CRS equals the source or is empty. DISPLAY ONLY
    // — analysis must read source pixels via readRegion()/readTile() (FR-CRS-4). Empty
    // buffer on failure (FR-CRS-5).
    TileBuffer readWarpedRegion(const std::string& project_wkt,
                                int xoff, int yoff, int xsize, int ysize,
                                int dstW, int dstH,
                                const std::vector<int>& bands_1based = {},
                                const std::string& resampling = "bilinear") const;

    TileBuffer readRegion(int xoff, int yoff,
                          int xsize, int ysize,
                          int dstW, int dstH,
                          const std::vector<int>& bands_1based = {}) const;

    TileBuffer readFullPreview(int maxSize = 1024) const;

    TileBuffer readTile(int zoom, int tx, int ty,
                        int tile_size, const std::vector<int>& bands_1based = {}) const;

    struct BandStats { double min{0}, max{0}, mean{0}, stddev{0}; };
    BandStats bandStats(int band_1based) const;

    struct NoDataInfo { bool has_value{false}; double value{0.0}; };
    NoDataInfo noData(int band_1based) const;

    // Returns (lo, hi) stretch values at the given percentile points (0-100).
    // Samples a downscaled preview, excludes nodata, sorts, and returns the
    // values at lo_pct and hi_pct positions.
    std::pair<float, float> computeStretchPercentile(
        int band_1based, double lo_pct, double hi_pct) const;

private:
    RasterDataset() = default;

    // Build a warp source that carries the EFFECTIVE georef (honoring any override):
    // returns m_ds directly when no override is active, else a VRT copy stamped with
    // geoTransform()/crsWkt(). `owned` is set when the caller must GDALClose the result.
    GDALDataset* makeWarpSource(bool& owned) const;

    // A cached, lazily-built warped VRT reprojecting this dataset into one target CRS.
    // `warped` is the VRTWarpedDataset (or nullptr when the build failed / sameAsSource).
    // `owned_src` is the effective-georef VRT source that must outlive `warped` and be
    // closed after it (nullptr when the raw m_ds is the source).
    struct WarpedHandle {
        GDALDataset* warped{nullptr};
        GDALDataset* owned_src{nullptr};
        WarpedView   view;         // cached metadata
        int          alpha_band{0}; // 1-based coverage/alpha band index (0 = none). When the
                                    // source has no declared no-data, the warp adds a -dstalpha
                                    // band so readWarpedRegion can mark UNCOVERED pixels (rotated
                                    // footprint corners) NaN → the shader discards them instead of
                                    // rendering the 0-fill as opaque black.
    };
    // Build/fetch the cached warped handle for a NORMALIZED target WKT. Assumes
    // m_mutex is already held. Returns nullptr only for the sameAsSource/empty case.
    WarpedHandle* ensureWarpedLocked(const std::string& norm_wkt,
                                     const std::string& resampling) const;

    GDALDataset*            m_ds{nullptr};
    mutable std::mutex      m_mutex;
    // Cache of warped VRTs keyed by normalized target WKT (one live target per pane,
    // so this stays small). Cleared in the destructor BEFORE m_ds is closed.
    mutable std::map<std::string, WarpedHandle> m_warp_cache;

    std::string  m_path;
    int          m_width{0};
    int          m_height{0};
    int          m_band_count{0};
    std::string  m_crs_wkt;
    mutable std::optional<bool> m_is_geographic;   // cached isGeographic() result
    GeoTransform m_geotransform;
    Extent       m_extent;

    mutable std::vector<std::optional<BandStats>> m_stats_cache;
    std::vector<NoDataInfo> m_nodata_cache;
    std::vector<int>  m_datatype_cache;      // per-band GDALDataType (as int), filled at open()
    std::vector<bool> m_colortable_cache;    // per-band palette/colortable presence
    bool m_has_unrepresentable{false};       // FR-CAP-3: complex/unrepresentable band present
};
