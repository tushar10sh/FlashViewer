#include "render/TileRenderer.hpp"
#include "render/TileShaders.hpp"
#include "core/ColormapRegistry.hpp"
#include "gis/WarpResampling.hpp"   // fvDefaultResampling (display warp token)
#include "util/Logger.hpp"
#include "util/ErrorReporter.hpp"
#include "util/PerfMetrics.hpp"
#include "core/RasterLayer.hpp"   // RasterLayer::layerId() for the open-latency probe

#include <QColor>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>
#include <array>

// Shaders are defined in render/TileShaders.hpp (shared with the render tests).
using tileshaders::kTileVert;
using tileshaders::kRgbFrag;
using tileshaders::kGrayFrag;

// --------------------------------------------------------------------------

TileRenderer::TileRenderer()
    : m_cache(512)
    , m_pool(std::max(2, static_cast<int>(std::thread::hardware_concurrency()) - 1))
{}

void TileRenderer::init(QOpenGLFunctions_4_1_Core& gl) {
    if (m_initialized) return;

    m_shader_rgb  = std::make_unique<GlslProgram>();
    m_shader_gray = std::make_unique<GlslProgram>();

    if (!m_shader_rgb->compile(gl, kTileVert, kRgbFrag))
        FV_ERROR("TileRenderer: RGB shader: {}", m_shader_rgb->lastError());
    if (!m_shader_gray->compile(gl, kTileVert, kGrayFrag))
        FV_ERROR("TileRenderer: Gray shader: {}", m_shader_gray->lastError());

    // Unit quad: a_pos [0,1]^2, a_uv — y=0 at top of texture (GDAL row 0 = top)
    const float verts[] = {
        0.f, 0.f,  0.f, 1.f,    // bottom-left  geo corner
        1.f, 0.f,  1.f, 1.f,    // bottom-right
        1.f, 1.f,  1.f, 0.f,    // top-right
        0.f, 1.f,  0.f, 0.f,    // top-left
    };
    const uint32_t idx[] = {0,1,2, 0,2,3};

    gl.glGenVertexArrays(1, &m_quad_vao);
    gl.glGenBuffers(1, &m_quad_vbo);
    gl.glGenBuffers(1, &m_quad_ebo);
    gl.glBindVertexArray(m_quad_vao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_quad_ebo);
    gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    gl.glEnableVertexAttribArray(1);
    gl.glBindVertexArray(0);

    m_initialized = true;
    // threadCount(), not pendingCount() — the latter is the QUEUE DEPTH, which is
    // naturally 0 at init, so the line always read "0 worker threads".
    FV_INFO("TileRenderer: initialized ({} worker threads)", m_pool.threadCount());
}

void TileRenderer::destroy(QOpenGLFunctions_4_1_Core& gl) {
    m_cache.clear(gl);
    // NOTE (Phase 6.4.4): do NOT destroy ColormapRegistry textures here. The registry is an
    // app-global singleton and QOpenGLWidget contexts share GL resources via Qt's global share
    // context, so tearing them down when ONE pane closes would delete the colormap textures the
    // surviving panes still bind (→ blank pseudocolor render). They are reclaimed at process exit.
    m_colormap_textures.clear();
    if (m_quad_vao) { gl.glDeleteVertexArrays(1, &m_quad_vao); m_quad_vao = 0; }
    if (m_quad_vbo) { gl.glDeleteBuffers(1, &m_quad_vbo); m_quad_vbo = 0; }
    if (m_quad_ebo) { gl.glDeleteBuffers(1, &m_quad_ebo); m_quad_ebo = 0; }
    if (m_shader_rgb)  { m_shader_rgb->destroy(gl);  m_shader_rgb.reset(); }
    if (m_shader_gray) { m_shader_gray->destroy(gl); m_shader_gray.reset(); }
    m_initialized = false;
}

void TileRenderer::invalidateLayer(QOpenGLFunctions_4_1_Core& gl, uint64_t layer_id) {
    m_cache.removeLayer(layer_id, gl);
}

// --------------------------------------------------------------------------
// LOD helpers

int TileRenderer::zoomForDims(int eff_w, int eff_h, double eff_geo_w, const Camera& camera) const {
    if (eff_w <= 0 || eff_h <= 0) return 0;
    int max_zoom = static_cast<int>(std::ceil(std::log2(
        std::max(eff_w, eff_h) / static_cast<double>(kTileSize))));
    max_zoom = std::max(0, max_zoom);

    if (camera.viewportWidth() <= 1 || camera.viewportHeight() <= 1 ||
        camera.scale() <= 0.0 || eff_geo_w <= 0.0) {
        return max_zoom;
    }

    double native_pixel_geo = eff_geo_w / eff_w;
    if (native_pixel_geo <= 0.0) return max_zoom;

    double ratio = camera.scale() / native_pixel_geo;
    if (ratio <= 1.0) return max_zoom;

    int lod_drop = static_cast<int>(std::floor(std::log2(ratio)));
    int lod_zoom = max_zoom - lod_drop;
    return std::clamp(lod_zoom, 0, max_zoom);
}

int TileRenderer::computeZoom(const RasterLayer& layer, const Camera& camera) const {
    auto* ds = layer.dataset();
    if (!ds) return 0;
    return zoomForDims(ds->width(), ds->height(), layer.extent().width(), camera);
}

std::vector<TileKey> TileRenderer::visibleTilesFor(uint64_t layer_id, const Extent& img,
                                                   const Camera& camera, int zoom,
                                                   uint32_t crs_epoch) const {
    const Extent vis = camera.visibleExtent();
    if (img.width() <= 0 || img.height() <= 0 || !img.overlaps(vis)) return {};

    int n = 1 << zoom;  // number of tiles per axis at this zoom level

    // Convert visible geo extent (Project CRS) to tile indices
    double tx_per_geo = n / img.width();
    double ty_per_geo = n / img.height();

    int tx0 = std::max(0, static_cast<int>(std::floor((vis.xmin - img.xmin) * tx_per_geo)));
    int tx1 = std::min(n-1, static_cast<int>(std::floor((vis.xmax - img.xmin) * tx_per_geo)));
    int ty0 = std::max(0, static_cast<int>(std::floor((img.ymax - vis.ymax) * ty_per_geo)));
    int ty1 = std::min(n-1, static_cast<int>(std::floor((img.ymax - vis.ymin) * ty_per_geo)));

    std::vector<TileKey> keys;
    keys.reserve(static_cast<size_t>((tx1-tx0+1) * (ty1-ty0+1)));
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
            keys.push_back({layer_id, zoom, tx, ty, crs_epoch});
    return keys;
}

std::vector<TileKey> TileRenderer::visibleTiles(const RasterLayer& layer,
                                                  const Camera& camera, int zoom) const {
    return visibleTilesFor(layer.layerId(), layer.extent(), camera, zoom, 0);
}

Extent TileRenderer::tileExtentFor(const RasterDataset::WarpedView& wv,
                                   const TileKey& key) const {
    int n     = 1 << key.zoom;
    // Integer ceiling division — must match readTile()/readWarpedRegion() tiling exactly
    int src_w = (wv.width  + n - 1) / n;
    int src_h = (wv.height + n - 1) / n;
    if (src_w <= 0 || src_h <= 0) return {};
    int xoff  = key.tx * src_w;
    int yoff  = key.ty * src_h;
    int act_w = std::min(src_w, wv.width  - xoff);
    int act_h = std::min(src_h, wv.height - yoff);
    const auto& gt = wv.gt;
    double xmin = gt.gt[0] + xoff         * gt.gt[1];
    double xmax = gt.gt[0] + (xoff+act_w) * gt.gt[1];
    double ymax = gt.gt[3] + yoff         * gt.gt[5];  // gt[5]<0, so this is the top geo edge
    double ymin = gt.gt[3] + (yoff+act_h) * gt.gt[5];  // bottom geo edge
    return {xmin, ymin, xmax, ymax};
}

Extent TileRenderer::tileGeoExtent(const RasterLayer& layer, const TileKey& key) const {
    auto* ds = layer.dataset();
    if (!ds) return {};
    RasterDataset::WarpedView sv;
    sv.width = ds->width(); sv.height = ds->height();
    sv.gt = ds->geoTransform(); sv.extent = ds->extent(); sv.sameAsSource = true;
    return tileExtentFor(sv, key);
}

// --------------------------------------------------------------------------
// Tile loading and GPU upload

void TileRenderer::uploadTex2D(QOpenGLFunctions_4_1_Core& gl,
                                const float* data, int w, int h, GLuint& tex_id) {
    if (tex_id == 0) gl.glGenTextures(1, &tex_id);
    gl.glBindTexture(GL_TEXTURE_2D, tex_id);
    while (gl.glGetError() != GL_NO_ERROR) { /* drain stale errors */ }
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, data);
    // VRAM-pressure detection (FR-ERR-5): on GL_OUT_OF_MEMORY the byte-budget /
    // count LRU eviction (TileCache::evict, run each frame) reclaims space next
    // pass; report once so we degrade gracefully instead of terminating.
    if (gl.glGetError() == GL_OUT_OF_MEMORY && !m_vram_warned) {
        m_vram_warned = true;
        ErrorReporter::instance().report(
            3, QStringLiteral("GPU"),
            QStringLiteral("Out of GPU memory uploading a tile — evicting cached "
                           "tiles and falling back to coarser detail."));
    }
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glBindTexture(GL_TEXTURE_2D, 0);
}

GLuint TileRenderer::getOrBuildColormap(QOpenGLFunctions_4_1_Core& gl, int id) {
    return ColormapRegistry::instance().getOrUploadTexture(gl, id);
}

bool TileRenderer::ensureTile(QOpenGLFunctions_4_1_Core& gl,
                               const TileKey& key, RasterLayer* layer,
                               int eff_w, int eff_h,
                               const std::string& project_wkt,
                               const std::string& resampling) {
    auto tile = m_cache.getOrCreate(key);

    // Step 1: Check upload_ready FIRST — handles both initial load AND refresh completion
    if (tile->upload_ready.load(std::memory_order_acquire)) {
        std::lock_guard lock(tile->data_mutex);
        // Describe the tile by what the worker actually DECODED, not by the layer's
        // mapping right now — those can differ if the user changed bands mid-decode,
        // and uploading against the wrong description would show stale texels as fresh.
        const bool gray    = tile->pending_gray;
        tile->grayscale    = gray;
        tile->band_r       = tile->pending_band_r;
        tile->band_g       = tile->pending_band_g;
        tile->band_b       = tile->pending_band_b;
        tile->stretch_min  = layer->stretchMin();
        tile->stretch_max  = layer->stretchMax();
        for (int c = 0; c < 3; ++c) {
            tile->ch_lo[c] = layer->channelStretchMin(c);
            tile->ch_hi[c] = layer->channelStretchMax(c);
        }
        if (!gray) {
            uploadTex2D(gl, tile->cpu_data_r.data(), tile->tile_w, tile->tile_h, tile->texture_r);
            if (!tile->cpu_data_g.empty())
                uploadTex2D(gl, tile->cpu_data_g.data(), tile->tile_w, tile->tile_h, tile->texture_g);
            if (!tile->cpu_data_b.empty())
                uploadTex2D(gl, tile->cpu_data_b.data(), tile->tile_w, tile->tile_h, tile->texture_b);
            tile->cpu_data_r.clear(); tile->cpu_data_g.clear(); tile->cpu_data_b.clear();
        } else {
            uploadTex2D(gl, tile->cpu_data_r.data(), tile->tile_w, tile->tile_h, tile->texture_r);
            tile->cpu_data_r.clear();
        }
        tile->upload_ready.store(false, std::memory_order_release);
        tile->refreshing.store(false, std::memory_order_release);
        tile->state = TileState::Ready;
        // Open-latency probe (NFR-PERF-3/4): the first ready tile of a freshly-opened
        // layer marks first-visible-tiles. Cheap + always-on (feeds the HUD); the
        // detailed log line is gated behind FV_PERF_INSTRUMENT.
        if (PerfMetrics::instance().markFirstTileReady(layer->layerId())) {
#ifdef FV_PERF_INSTRUMENT
            FV_INFO("perf: layer {} first tiles ready in {:.0f} ms (NFR-PERF-3/4)",
                    layer->layerId(), PerfMetrics::instance().lastOpenLatencyMs());
#endif
        }
        return true;
    }

    // Step 2: Check if tile is Ready
    if (tile->state == TileState::Ready) {
        // Matching means the same MODE *and* the same source bands. Comparing only the
        // mode left a re-pick of the R/G/B triple (or of the gray band) drawing the
        // previously decoded bands until an RGB↔Gray round-trip forced a reload.
        if (!fvTileBandsStale(*tile, layer->bandMapping())) return true;

        // Band selection mismatch — schedule a refresh worker (only once)
        if (!tile->refreshing.load(std::memory_order_acquire)) {
            // SHARED ownership, not layer->dataset(): the worker outlives this call, and a
            // layer removed while its decode is in flight would otherwise destroy the
            // GDALDataset — and the mutex a worker may be blocked on — under that worker.
            std::shared_ptr<RasterDataset> ds = layer->datasetPtr();
            if (ds) {
                const auto bm   = layer->bandMapping();
                const bool gray = bm.isGrayscale();
                const int  zoom = key.zoom;
                const int  tx   = key.tx;
                const int  ty   = key.ty;
                auto tile_ref = tile;
                tile->refreshing.store(true, std::memory_order_release);
                m_pool.submit([ds, tile_ref, bm, gray, zoom, tx, ty,
                               eff_w, eff_h, project_wkt, resampling]() mutable {
                    std::vector<int> bands;
                    if (gray) {
                        bands = {bm.grayBand()};
                    } else {
                        bands = {bm.red_idx, bm.green_idx, bm.blue_idx};
                    }
                    // Tiling is against the EFFECTIVE (reprojected) grid; readWarpedRegion warps the
                    // window into the Project CRS (or reads source pixels when sameAsSource).
                    int n     = 1 << zoom;
                    int src_w = (eff_w + n - 1) / n;
                    int src_h = (eff_h + n - 1) / n;
                    int xoff  = tx * src_w;
                    int yoff  = ty * src_h;
                    int act_w = std::min(src_w, eff_w - xoff);
                    int act_h = std::min(src_h, eff_h - yoff);

                    int max_zoom = static_cast<int>(std::ceil(std::log2(
                        std::max(eff_w, eff_h) / static_cast<double>(kTileSize))));
                    max_zoom = std::max(0, max_zoom);
                    int downsample = (zoom >= max_zoom) ? 1 : (1 << (max_zoom - zoom));

                    int dst_act_w = std::max(1, (act_w + downsample - 1) / downsample);
                    int dst_act_h = std::max(1, (act_h + downsample - 1) / downsample);

                    // Read a neighbour apron around the tile, clamped to the dataset
                    int src_hx = kTileApron * downsample;
                    int src_hy = kTileApron * downsample;
                    int rx0 = std::max(0, xoff - src_hx), ry0 = std::max(0, yoff - src_hy);
                    int rx1 = std::min(eff_w, xoff + act_w + src_hx);
                    int ry1 = std::min(eff_h, yoff + act_h + src_hy);
                    int read_w = rx1 - rx0, read_h = ry1 - ry0;

                    int dst_w = std::max(1, (read_w + downsample - 1) / downsample);
                    int dst_h = std::max(1, (read_h + downsample - 1) / downsample);
                    int inner_x = (xoff - rx0) / downsample;
                    int inner_y = (yoff - ry0) / downsample;

                    TileBuffer buf = ds->readWarpedRegion(project_wkt, rx0, ry0, read_w, read_h,
                                                          dst_w, dst_h, bands, resampling);
                    if (!buf.isValid()) {
                        tile_ref->refreshing.store(false, std::memory_order_release);
                        return;
                    }
                    std::lock_guard lock(tile_ref->data_mutex);
                    tile_ref->tile_w = buf.width;
                    tile_ref->tile_h = buf.height;
                    tile_ref->inner_x = inner_x;   tile_ref->inner_y = inner_y;
                    tile_ref->inner_w = dst_act_w; tile_ref->inner_h = dst_act_h;
                    tile_ref->pending_gray   = gray;
                    tile_ref->pending_band_r = gray ? bm.grayBand() : bm.red_idx;
                    tile_ref->pending_band_g = gray ? bm.grayBand() : bm.green_idx;
                    tile_ref->pending_band_b = gray ? bm.grayBand() : bm.blue_idx;
                    if (!gray) {
                        int nb = buf.bands;
                        tile_ref->cpu_data_r.assign(buf.bandPtr(0), buf.bandPtr(0) + buf.width*buf.height);
                        tile_ref->cpu_data_g.assign(buf.bandPtr(std::min(1,nb-1)), buf.bandPtr(std::min(1,nb-1)) + buf.width*buf.height);
                        tile_ref->cpu_data_b.assign(buf.bandPtr(std::min(2,nb-1)), buf.bandPtr(std::min(2,nb-1)) + buf.width*buf.height);
                    } else {
                        tile_ref->cpu_data_r.assign(buf.bandPtr(0), buf.bandPtr(0) + buf.width*buf.height);
                    }
                    tile_ref->upload_ready.store(true, std::memory_order_release);
                });
            }
        }
        // Return true regardless — keep drawing old tile as fallback
        return true;
    }

    // Step 3: Still loading
    if (tile->state == TileState::Loading) return false;

    // Step 4: Empty — schedule initial decode
    tile->state = TileState::Loading;
    // Shared ownership for the same reason as the refresh path above: the decode runs on a
    // pool thread and must keep the dataset alive for as long as it reads through it.
    std::shared_ptr<RasterDataset> ds = layer->datasetPtr();
    if (!ds) { tile->state = TileState::Empty; return false; }

    const auto bm   = layer->bandMapping();
    const bool gray = bm.isGrayscale();
    const int  zoom = key.zoom;
    const int  tx   = key.tx;
    const int  ty   = key.ty;
    auto tile_ref = tile;  // shared_ptr copy keeps the tile alive alongside the dataset

    m_pool.submit([ds, tile_ref, bm, gray, zoom, tx, ty,
                   eff_w, eff_h, project_wkt, resampling]() mutable {
        std::vector<int> bands;
        if (gray) {
            bands = {bm.grayBand()};
        } else {
            bands = {bm.red_idx, bm.green_idx, bm.blue_idx};
        }
        int n     = 1 << zoom;
        int src_w = (eff_w + n - 1) / n;
        int src_h = (eff_h + n - 1) / n;
        int xoff  = tx * src_w;
        int yoff  = ty * src_h;
        int act_w = std::min(src_w, eff_w - xoff);
        int act_h = std::min(src_h, eff_h - yoff);

        int max_zoom = static_cast<int>(std::ceil(std::log2(
            std::max(eff_w, eff_h) / static_cast<double>(kTileSize))));
        max_zoom = std::max(0, max_zoom);
        int downsample = (zoom >= max_zoom) ? 1 : (1 << (max_zoom - zoom));

        int dst_act_w = std::max(1, (act_w + downsample - 1) / downsample);
        int dst_act_h = std::max(1, (act_h + downsample - 1) / downsample);

        // Read a neighbour apron around the tile, clamped to the dataset
        int src_hx = kTileApron * downsample;
        int src_hy = kTileApron * downsample;
        int rx0 = std::max(0, xoff - src_hx), ry0 = std::max(0, yoff - src_hy);
        int rx1 = std::min(eff_w, xoff + act_w + src_hx);
        int ry1 = std::min(eff_h, yoff + act_h + src_hy);
        int read_w = rx1 - rx0, read_h = ry1 - ry0;

        int dst_w = std::max(1, (read_w + downsample - 1) / downsample);
        int dst_h = std::max(1, (read_h + downsample - 1) / downsample);
        int inner_x = (xoff - rx0) / downsample;
        int inner_y = (yoff - ry0) / downsample;

        TileBuffer buf = ds->readWarpedRegion(project_wkt, rx0, ry0, read_w, read_h,
                                              dst_w, dst_h, bands, resampling);
        if (!buf.isValid()) return;

        std::lock_guard lock(tile_ref->data_mutex);
        tile_ref->tile_w = buf.width;
        tile_ref->tile_h = buf.height;
        tile_ref->inner_x = inner_x;   tile_ref->inner_y = inner_y;
        tile_ref->inner_w = dst_act_w; tile_ref->inner_h = dst_act_h;
        tile_ref->pending_gray   = gray;
        tile_ref->pending_band_r = gray ? bm.grayBand() : bm.red_idx;
        tile_ref->pending_band_g = gray ? bm.grayBand() : bm.green_idx;
        tile_ref->pending_band_b = gray ? bm.grayBand() : bm.blue_idx;

        if (!gray) {
            int nb = buf.bands;
            tile_ref->cpu_data_r.assign(buf.bandPtr(0), buf.bandPtr(0) + buf.width*buf.height);
            tile_ref->cpu_data_g.assign(buf.bandPtr(std::min(1,nb-1)), buf.bandPtr(std::min(1,nb-1)) + buf.width*buf.height);
            tile_ref->cpu_data_b.assign(buf.bandPtr(std::min(2,nb-1)), buf.bandPtr(std::min(2,nb-1)) + buf.width*buf.height);
        } else {
            tile_ref->cpu_data_r.assign(buf.bandPtr(0), buf.bandPtr(0) + buf.width*buf.height);
        }
        tile_ref->upload_ready.store(true, std::memory_order_release);
    });

    return false;
}

// --------------------------------------------------------------------------
// Draw

void TileRenderer::drawTile(QOpenGLFunctions_4_1_Core& gl,
                              const TileKey& key,
                              const GpuTile& tile,
                              const glm::mat4& vp,
                              RasterLayer* layer,
                              const RasterDataset::WarpedView& wv,
                              float opacity,
                              bool use_nearest) {
    Extent ext = tileExtentFor(wv, key);
    glm::vec4 tile_ext{
        static_cast<float>(ext.xmin), static_cast<float>(ext.ymin),
        static_cast<float>(ext.xmax), static_cast<float>(ext.ymax)
    };

    // FR-RND-10 display resampling. At magnification past native (use_nearest) ALL
    // modes fall back to bilinear + GL_NEAREST for crisp pixel inspection (parity
    // with bilinear); bicubic applies only when interpolating (minification). The
    // tile apron + u_inner mapping keep bicubic seam-free across tile boundaries.
    const int resample = fvEffectiveResample(use_nearest,
                                             static_cast<int>(layer->displayResampling()));
    const float bleed_guard = use_nearest ? 0.0f : 1.0f;
    const glm::vec4 inner_rect{
        static_cast<float>(tile.inner_x), static_cast<float>(tile.inner_y),
        static_cast<float>(tile.inner_w), static_cast<float>(tile.inner_h)};

    // A tile awaiting its refresh still holds the PREVIOUS bands' texels, so it is drawn
    // with the stretch stamped at its own upload rather than the layer's new one — see
    // fvTileDrawStretch. Live values are used the moment the tile is current again.
    const bool stale = fvTileBandsStale(tile, layer->bandMapping());
    const FvStretch gray_s = fvTileDrawStretch(stale, tile.stretch_min, tile.stretch_max,
                                               layer->stretchMin(), layer->stretchMax());
    FvStretch ch_s[3];
    for (int c = 0; c < 3; ++c)
        ch_s[c] = fvTileDrawStretch(stale, tile.ch_lo[c], tile.ch_hi[c],
                                    layer->channelStretchMin(c), layer->channelStretchMax(c));

    gl.glBindVertexArray(m_quad_vao);

    if (!tile.grayscale) {
        m_shader_rgb->bind(gl);
        m_shader_rgb->setUniform(gl, "u_view_proj",   vp);
        m_shader_rgb->setUniform(gl, "u_tile_extent", tile_ext);
        m_shader_rgb->setUniform(gl, "u_opacity",     opacity);
        // Per-channel stretch: each display channel stretches its mapped band
        // independently (FR-HST-6 / per-band histograms).
        m_shader_rgb->setUniform(gl, "u_min_r", ch_s[0].lo);
        m_shader_rgb->setUniform(gl, "u_max_r", ch_s[0].hi);
        m_shader_rgb->setUniform(gl, "u_min_g", ch_s[1].lo);
        m_shader_rgb->setUniform(gl, "u_max_g", ch_s[1].hi);
        m_shader_rgb->setUniform(gl, "u_min_b", ch_s[2].lo);
        m_shader_rgb->setUniform(gl, "u_max_b", ch_s[2].hi);
        m_shader_rgb->setUniform(gl, "u_band_r", 0);
        m_shader_rgb->setUniform(gl, "u_band_g", 1);
        m_shader_rgb->setUniform(gl, "u_band_b", 2);
        m_shader_rgb->setUniform(gl, "u_has_nodata",   layer->hasNoData()  ? 1.0f : 0.0f);
        m_shader_rgb->setUniform(gl, "u_nodata_value", layer->noDataValue());
        {
            QColor c = layer->nodataColor();
            glm::vec4 nc{c.redF(), c.greenF(), c.blueF(), c.alphaF()};
            m_shader_rgb->setUniform(gl, "u_nodata_color", nc);
        }
        m_shader_rgb->setUniform(gl, "u_bleed_guard", bleed_guard);
        m_shader_rgb->setUniform(gl, "u_resample",    resample);
        m_shader_rgb->setUniform(gl, "u_inner",       inner_rect);
        GLint mag = use_nearest ? GL_NEAREST : GL_LINEAR;
        gl.glActiveTexture(GL_TEXTURE0); gl.glBindTexture(GL_TEXTURE_2D, tile.texture_r);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
        gl.glActiveTexture(GL_TEXTURE1); gl.glBindTexture(GL_TEXTURE_2D, tile.texture_g);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
        gl.glActiveTexture(GL_TEXTURE2); gl.glBindTexture(GL_TEXTURE_2D, tile.texture_b);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
        gl.glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        m_shader_rgb->release(gl);
    } else {
        m_shader_gray->bind(gl);
        m_shader_gray->setUniform(gl, "u_view_proj",   vp);
        m_shader_gray->setUniform(gl, "u_tile_extent", tile_ext);
        m_shader_gray->setUniform(gl, "u_opacity",     opacity);
        m_shader_gray->setUniform(gl, "u_min",         gray_s.lo);
        m_shader_gray->setUniform(gl, "u_max",         gray_s.hi);
        m_shader_gray->setUniform(gl, "u_band",        0);
        m_shader_gray->setUniform(gl, "u_colormap",    1);
        m_shader_gray->setUniform(gl, "u_invert",      layer->colormapInvert() ? 1.0f : 0.0f);
        m_shader_gray->setUniform(gl, "u_has_nodata",   layer->hasNoData()  ? 1.0f : 0.0f);
        m_shader_gray->setUniform(gl, "u_nodata_value", layer->noDataValue());
        {
            QColor c = layer->nodataColor();
            glm::vec4 nc{c.redF(), c.greenF(), c.blueF(), c.alphaF()};
            m_shader_gray->setUniform(gl, "u_nodata_color", nc);
        }
        m_shader_gray->setUniform(gl, "u_bleed_guard", bleed_guard);
        m_shader_gray->setUniform(gl, "u_resample",    resample);
        m_shader_gray->setUniform(gl, "u_inner",       inner_rect);
        GLuint cm_tex = getOrBuildColormap(gl, layer->colormapId());
        gl.glActiveTexture(GL_TEXTURE0); gl.glBindTexture(GL_TEXTURE_2D, tile.texture_r);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, use_nearest ? GL_NEAREST : GL_LINEAR);
        gl.glActiveTexture(GL_TEXTURE1); gl.glBindTexture(GL_TEXTURE_1D, cm_tex);
        gl.glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        m_shader_gray->release(gl);
    }
    gl.glBindVertexArray(0);
    gl.glActiveTexture(GL_TEXTURE0);
}

// --------------------------------------------------------------------------

void TileRenderer::flushPendingUploads(QOpenGLFunctions_4_1_Core&)
{
    // Uploads happen inline in ensureTile on the GL thread; nothing to flush here.
}

// --------------------------------------------------------------------------

bool TileRenderer::render(QOpenGLFunctions_4_1_Core& gl,
                           const Camera& camera,
                           const std::vector<std::shared_ptr<Layer>>& layers,
                           const std::string& project_wkt,
                           uint32_t crs_epoch) {
    if (!m_initialized) return true;

    ++m_frame_counter;
    if (m_frame_counter == 0) ++m_frame_counter;

    flushPendingUploads(gl);

    glm::mat4 vp = camera.viewProjMatrix();
    bool all_ready = true;

    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const auto& layer_ptr = *it;
        if (!layer_ptr->visible() || layer_ptr->type() != LayerType::Raster) continue;
        auto* rl = static_cast<RasterLayer*>(layer_ptr.get());
        auto* ds = rl->dataset();
        if (!ds) continue;

        // On-the-fly reprojection view into the pane's Project CRS (FR-CRS-1). Warp
        // resampling is data-aware (categorical/integer → nearest, continuous → bilinear,
        // FR-ACC-3). sameAsSource ⇒ the raw source path (the base layer never warps).
        const std::string resamp =
            fvDefaultResampling(static_cast<GDALDataType>(ds->bandDataType(1)),
                                ds->bandHasColorTable(1));
        RasterDataset::WarpedView wv = ds->warpedView(project_wkt, resamp);

        const bool reprojecting = !wv.sameAsSource && !wv.failed;
        // Phase 17 #4: when a layer can't be reprojected into the pane CRS, DON'T omit it —
        // fall back to drawing it UNWARPED in its native CRS. The camera stays in the pane
        // CRS, so the layer lands at its native georeferenced coordinates (it may not align
        // with the pane); showReprojectionNotice() tells the user. Its tiles are then read
        // raw (empty read_wkt ⇒ readWarpedRegion delegates to the untouched source read).
        std::string read_wkt = project_wkt;
        bool native_fallback = false;
        if (wv.failed) {
            wv = ds->warpedView(std::string());   // native (sameAsSource) view — never fails
            read_wkt.clear();
            native_fallback = true;
        }
        if (m_reproject_cb) m_reproject_cb(rl->layerId(), reprojecting, native_fallback);
        if (wv.failed) {          // native view unusable too → skip (defensive; FR-CRS-5)
            all_ready = false;
            continue;
        }

        const int eff_w = wv.width;
        const int eff_h = wv.height;
        int zoom = zoomForDims(eff_w, eff_h, wv.extent.width(), camera);
        auto keys = visibleTilesFor(rl->layerId(), wv.extent, camera, zoom, crs_epoch);

        // Use nearest-neighbor magnification when one image pixel ≥ one screen pixel
        double pixel_geo_w = eff_w > 0 ? wv.extent.width() / eff_w : 1.0;
        bool use_nearest = (pixel_geo_w >= camera.scale());

        for (const auto& key : keys) {
            m_cache.touch(key, m_frame_counter);
            if (!ensureTile(gl, key, rl, eff_w, eff_h, read_wkt, resamp)) {
                all_ready = false;
                // Fall back: draw a coarser tile that covers this area ONLY if already resident
                for (int fz = zoom - 1; fz >= 0; --fz) {
                    int diff = zoom - fz;
                    TileKey fallback{key.layer_id, fz,
                        key.tx >> diff, key.ty >> diff, key.crs_epoch};
                    auto ft = m_cache.get(fallback);
                    if (ft && ft->state == TileState::Ready) {
                        m_cache.touch(fallback, m_frame_counter);
                        drawTile(gl, fallback, *ft, vp, rl, wv, layer_ptr->opacity(), use_nearest);
                        break;
                    }
                }
            } else {
                auto tile = m_cache.get(key);
                if (tile && tile->state == TileState::Ready) {
                    drawTile(gl, key, *tile, vp, rl, wv, layer_ptr->opacity(), use_nearest);
                    // Keep repaint timer going while refresh is in progress
                    if (tile->refreshing.load(std::memory_order_acquire))
                        all_ready = false;
                }
            }
        }
    }

    m_cache.evict(gl, m_frame_counter);
    return all_ready;
}
