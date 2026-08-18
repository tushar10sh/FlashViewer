#pragma once
#include <QOpenGLFunctions_4_1_Core>
#include "render/Camera.hpp"
#include "render/GlslProgram.hpp"
#include "core/Layer.hpp"
#include "core/RasterLayer.hpp"
#include "core/TileKey.hpp"
#include "core/TileCache.hpp"
#include "core/ThreadPool.hpp"
#include "io/RasterDataset.hpp"   // RasterDataset::WarpedView (on-the-fly reprojection)

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <array>

static constexpr int kTileSize = 256;  // output tile size in pixels

// Tile neighbour apron (halo), in texels, read around each tile so interpolation
// kernels at tile edges sample real neighbour pixels instead of clamping (prevents
// bicubic tile-boundary seams). H=2 covers the bicubic kernel reach.
static constexpr int kTileApron = 2;

// Effective display-resampling mode for a draw (FR-RND-10). At magnification past
// native (use_nearest), every mode falls back to bilinear + GL_NEAREST for crisp
// pixel inspection — parity with bilinear; bicubic applies only when interpolating
// (minification / zoom-out). Free + inline so the gate is unit-testable.
inline int fvEffectiveResample(bool use_nearest, int layer_resample) {
    return use_nearest ? 0 : layer_resample;   // 0 = bilinear (NEAREST filter applied)
}

// True when a resident tile's texels no longer correspond to the layer's band mapping
// and must be re-decoded. A tile is stale on an RGB↔Gray flip *and* on a change of the
// source bands within a mode: re-picking the R/G/B triple, or picking a different gray
// band, changes the texels even though the mode is unchanged. In gray mode only the
// single band matters — the unused G/B slots are not compared, so a tile is not
// needlessly refreshed for them. Free + inline so the staleness rule is unit-testable.
inline bool fvTileBandsStale(const GpuTile& tile, const BandMapping& bm) {
    const bool gray = bm.isGrayscale();
    if (tile.grayscale != gray) return true;
    if (gray) return tile.band_r != bm.grayBand();
    return tile.band_r != bm.red_idx
        || tile.band_g != bm.green_idx
        || tile.band_b != bm.blue_idx;
}

// The stretch a tile must be DRAWN with. A stale tile is not dropped — TileRenderer keeps
// it on screen as a fallback while its refresh worker decodes the new bands — so pairing it
// with the layer's live stretch renders the OLD band's texels through the NEW band's range
// for one frame. That mismatched frame, followed by the refreshed one, is what reads as a
// flicker when the band changes. A stale tile therefore keeps the stretch stamped at its
// own upload, and adopts the live values the instant its texels match the mapping again —
// which keeps a histogram-handle drag live for every tile that is up to date.
// Free + inline so the rule is unit-testable without a GL context.
struct FvStretch { float lo, hi; };
inline FvStretch fvTileDrawStretch(bool stale, float tile_lo, float tile_hi,
                                   float live_lo, float live_hi) {
    return stale ? FvStretch{tile_lo, tile_hi} : FvStretch{live_lo, live_hi};
}

// GPU-accelerated tile renderer with async LOD loading.
// Phase 2 single-texture is replaced here with a proper tile/LOD system.
class TileRenderer {
public:
    TileRenderer();
    ~TileRenderer() = default;

    void init(QOpenGLFunctions_4_1_Core& gl);
    void destroy(QOpenGLFunctions_4_1_Core& gl);

    // Draw all visible layers using the LOD tile system, reprojecting each layer whose
    // source CRS differs from project_wkt into it on the fly (Phase 11, FR-CRS-1). An
    // empty project_wkt draws every layer in its own (source) CRS — the pre-Phase-11
    // behaviour. crs_epoch is stamped into tile keys so a Project-CRS change invalidates
    // cached tiles (FR-CRS-2). Returns true if all tiles were ready (no loads pending).
    bool render(QOpenGLFunctions_4_1_Core& gl,
                const Camera& camera,
                const std::vector<std::shared_ptr<Layer>>& layers,
                const std::string& project_wkt = {},
                uint32_t crs_epoch = 0);

    // Per-layer reprojection status callback (Phase 11): invoked during render() for each
    // visible raster layer with (layer_id, reprojecting, native_fallback) so the canvas can
    // raise the on-the-fly reprojection notice (requirement #2). native_fallback is true when
    // the layer could not be warped into the pane CRS and is instead shown in its own native
    // CRS (Phase 17 #4) rather than omitted.
    using ReprojectStatusFn = std::function<void(uint64_t layer_id, bool reprojecting, bool native_fallback)>;
    void setReprojectStatusCallback(ReprojectStatusFn fn) { m_reproject_cb = std::move(fn); }

    // Remove cached GPU tiles for a layer (call when layer is removed).
    void invalidateLayer(QOpenGLFunctions_4_1_Core& gl, uint64_t layer_id);

    TileCache& tileCache() { return m_cache; }

    // LOD helpers — pure (no GL state), public so they can be unit-tested
    // directly (TC-RND-05 computeZoom / TC-RND-06 visibleTiles).

    // Compute LOD zoom level for a layer given the current camera
    int computeZoom(const RasterLayer& layer, const Camera& camera) const;

    // Determine which tiles are visible at the given zoom level
    std::vector<TileKey> visibleTiles(const RasterLayer& layer,
                                       const Camera& camera, int zoom) const;

private:
    // Ensure a tile's GPU texture is current; schedules async load if not.
    // Returns true if the tile is ready to draw. eff_w/eff_h are the effective (warped)
    // dataset dimensions and project_wkt/resampling drive the reprojected tile read
    // (both reduce to the raw source read when project_wkt is empty or == source CRS).
    bool ensureTile(QOpenGLFunctions_4_1_Core& gl,
                    const TileKey& key,
                    RasterLayer* layer,
                    int eff_w, int eff_h,
                    const std::string& project_wkt,
                    const std::string& resampling);

    // Zoom / tile math against explicit (possibly warped) dimensions — the CRS-aware
    // core the public source-based helpers delegate to.
    int zoomForDims(int eff_w, int eff_h, double eff_geo_w = 0.0, const Camera& camera = {}) const;
    std::vector<TileKey> visibleTilesFor(uint64_t layer_id, const Extent& img,
                                         const Camera& camera, int zoom,
                                         uint32_t crs_epoch) const;
    Extent tileExtentFor(const RasterDataset::WarpedView& wv, const TileKey& key) const;

    // Upload pending CPU data to GL for any ready tiles (call at top of render)
    void flushPendingUploads(QOpenGLFunctions_4_1_Core& gl);

    // Draw one tile quad. `wv` supplies the (possibly reprojected) grid the tile's
    // geo extent is computed against, so the quad lands in Project-CRS coordinates.
    void drawTile(QOpenGLFunctions_4_1_Core& gl,
                  const TileKey& key,
                  const GpuTile& tile,
                  const glm::mat4& vp,
                  RasterLayer* layer,
                  const RasterDataset::WarpedView& wv,
                  float opacity,
                  bool use_nearest = false,
                  glm::dvec2 camera_center = {0.0, 0.0});

    // Upload a single-channel float buffer to a GL_R32F texture
    void uploadTex2D(QOpenGLFunctions_4_1_Core& gl,
                     const float* data, int w, int h, GLuint& tex_id);

    // Build and cache a gray colormap 1D texture (id=0)
    GLuint getOrBuildColormap(QOpenGLFunctions_4_1_Core& gl, int colormap_id);

    // Geo extent of one tile
    Extent tileGeoExtent(const RasterLayer& layer, const TileKey& key) const;

    // Shaders
    std::unique_ptr<GlslProgram> m_shader_rgb;
    std::unique_ptr<GlslProgram> m_shader_gray;

    // Shared quad geometry (unit [0,1]^2 with UVs)
    GLuint m_quad_vao{0};
    GLuint m_quad_vbo{0};
    GLuint m_quad_ebo{0};

    TileCache  m_cache;
    ThreadPool m_pool;

    // Colormap 1D textures indexed by colormap_id
    std::unordered_map<int, GLuint> m_colormap_textures;

    bool m_initialized{false};
    bool m_vram_warned{false};   // one-shot GL_OUT_OF_MEMORY report gate (FR-ERR-5)
    uint64_t m_frame_counter{0}; // active frame generation index for cache eviction protection

    ReprojectStatusFn m_reproject_cb;   // per-layer reprojection status (Phase 11)
};
