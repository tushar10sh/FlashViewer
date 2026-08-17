#pragma once
#include "core/TileKey.hpp"
#include <QOpenGLFunctions_4_1_Core>
#include <atomic>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

enum class TileState { Empty, Loading, Ready };

struct GpuTile {
    GLuint      texture_r{0};
    GLuint      texture_g{0};
    GLuint      texture_b{0};
    TileState   state{TileState::Empty};
    bool        grayscale{false};
    // The 1-based source bands the RESIDENT textures were decoded from (gray uses
    // band_r only). Tracked so a band change WITHIN a mode — picking a different
    // R/G/B triple, or a different gray band — invalidates the tile just as an
    // RGB↔Gray flip does; comparing the mode flag alone left stale texels on screen.
    // Written only on the UI thread, from the pending_* values, at upload time.
    int         band_r{0}, band_g{0}, band_b{0};
    // The stretch in force when the RESIDENT texels were uploaded — gray, then per display
    // channel. A stale tile stays on screen while its refresh decodes (TileRenderer keeps
    // drawing it as a fallback), so it must be drawn with the stretch that belongs to the
    // bands it actually holds. Drawing the OLD band's texels through the NEW band's range
    // produced one visibly wrong frame on every band change — the reported flicker.
    float       stretch_min{0};
    float       stretch_max{1};
    float       ch_lo[3]{0, 0, 0};
    float       ch_hi[3]{1, 1, 1};
    std::atomic<bool> upload_ready{false};
    std::atomic<bool> refreshing{false};

    // What the in-flight decode worker actually read — written by the worker under
    // data_mutex, committed to grayscale/band_* by the uploader. Keeps the resident
    // description honest even if the layer's mapping changed again mid-decode (the
    // next frame then simply schedules another refresh).
    bool        pending_gray{false};
    int         pending_band_r{0}, pending_band_g{0}, pending_band_b{0};

    std::vector<float> cpu_data_r;
    std::vector<float> cpu_data_g;
    std::vector<float> cpu_data_b;
    int   tile_w{0}, tile_h{0};          // full (aproned) texture size in texels
    // Inner (logical) tile rect within the aproned texture, in texels — the region
    // the quad maps to (FR-RND-10 seamless tiled interpolation). 0 = whole texture.
    int   inner_x{0}, inner_y{0}, inner_w{0}, inner_h{0};
    uint64_t last_touch_frame{0};
    std::mutex data_mutex;
};

class TileCache {
public:
    explicit TileCache(int capacity = 512);

    std::shared_ptr<GpuTile> getOrCreate(const TileKey& key);
    std::shared_ptr<GpuTile> get(const TileKey& key);
    void touch(const TileKey& key, uint64_t frame);

    void evict(QOpenGLFunctions_4_1_Core& gl, uint64_t active_frame = 0);
    void removeLayer(uint64_t layer_id, QOpenGLFunctions_4_1_Core& gl);
    void clear(QOpenGLFunctions_4_1_Core& gl);

    int size() const;
    int capacity() const { return m_capacity; }

    // Estimated resident GPU memory (FR-ERR-5, NFR-PERF-5, GPU Monitor FR-APP-11):
    // sum over resident tiles of (allocated textures) × tile_w × tile_h × 4 bytes
    // (GL_R32F = 4 B/texel). A pure query — no upload-path plumbing needed.
    std::size_t residentBytes() const;

    // Secondary byte-ceiling for LRU eviction (FR-ERR-5). 0 = disabled (count-only).
    // Default sized so the 512-tile count cap dominates at typical tile sizes while
    // the ceiling still contains a runaway before OOM.
    void        setByteBudget(std::size_t bytes) { m_byte_budget = bytes; }
    std::size_t byteBudget() const { return m_byte_budget; }

private:
    using List = std::list<TileKey>;
    using Map  = std::unordered_map<TileKey, std::pair<std::shared_ptr<GpuTile>, List::iterator>>;

    void deleteGpuTile(QOpenGLFunctions_4_1_Core& gl, GpuTile& t);
    // Bytes held by one tile's allocated textures (caller holds m_mutex).
    static std::size_t tileBytes(const GpuTile& t);
    // Sum of tileBytes over all resident tiles (caller holds m_mutex).
    std::size_t residentBytesLocked() const;

    int          m_capacity;
    std::size_t  m_byte_budget{1536ull * 1024 * 1024};  // ~1.5 GiB safety ceiling
    mutable std::mutex m_mutex;
    List         m_order;
    Map          m_map;
};
