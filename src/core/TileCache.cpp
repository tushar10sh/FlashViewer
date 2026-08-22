#include "core/TileCache.hpp"
#include "util/Logger.hpp"

TileCache::TileCache(int capacity) : m_capacity(capacity) {}

std::shared_ptr<GpuTile> TileCache::getOrCreate(const TileKey& key) {
    std::lock_guard lock(m_mutex);
    auto it = m_map.find(key);
    if (it != m_map.end()) {
        m_order.splice(m_order.begin(), m_order, it->second.second);
        return it->second.first;
    }
    auto tile = std::make_shared<GpuTile>();
    m_order.push_front(key);
    m_map[key] = {tile, m_order.begin()};
    return tile;
}

std::shared_ptr<GpuTile> TileCache::get(const TileKey& key) {
    std::lock_guard lock(m_mutex);
    auto it = m_map.find(key);
    if (it == m_map.end()) return nullptr;
    m_order.splice(m_order.begin(), m_order, it->second.second);
    return it->second.first;
}

void TileCache::touch(const TileKey& key, uint64_t frame) {
    std::lock_guard lock(m_mutex);
    auto it = m_map.find(key);
    if (it != m_map.end()) {
        it->second.first->last_touch_frame = frame;
    }
}

void TileCache::evict(QOpenGLFunctions_4_1_Core& gl, uint64_t active_frame) {
    std::lock_guard lock(m_mutex);
    // Primary: count-based LRU cap. Secondary: byte-ceiling (FR-ERR-5) — keep
    // evicting the oldest tiles while resident bytes exceed the budget (but never
    // drop the single most-recently-used tile or tiles actively in use this frame).
    auto overCount = [&] { return static_cast<int>(m_map.size()) > m_capacity; };
    auto overBytes = [&] {
        return m_byte_budget != 0 && m_map.size() > 1
            && residentBytesLocked() > m_byte_budget;
    };

    for (auto it = m_order.rbegin(); it != m_order.rend(); ) {
        if (!overCount() && !overBytes()) break;
        TileKey lru = *it;
        auto mapIt = m_map.find(lru);
        if (mapIt == m_map.end()) {
            it = decltype(it)(m_order.erase(std::next(it).base()));
            continue;
        }

        // Active frame protection: do NOT evict tiles referenced in the active render pass
        if (active_frame > 0 && mapIt->second.first->last_touch_frame == active_frame) {
            ++it;
            continue;
        }

        deleteGpuTile(gl, *mapIt->second.first);
        auto listIt = mapIt->second.second;
        m_map.erase(mapIt);
        it = decltype(it)(m_order.erase(listIt));
    }
}

void TileCache::removeLayer(uint64_t layer_id, QOpenGLFunctions_4_1_Core& gl) {
    std::lock_guard lock(m_mutex);
    for (auto it = m_map.begin(); it != m_map.end(); ) {
        if (it->first.layer_id == layer_id) {
            deleteGpuTile(gl, *it->second.first);
            m_order.erase(it->second.second);
            it = m_map.erase(it);
        } else {
            ++it;
        }
    }
}

void TileCache::markLayerDirty(uint64_t layer_id) {
    std::lock_guard lock(m_mutex);
    for (auto& [key, pair] : m_map) {
        if (key.layer_id == layer_id) {
            pair.first->data_dirty.store(true, std::memory_order_release);
        }
    }
}

void TileCache::clear(QOpenGLFunctions_4_1_Core& gl) {
    std::lock_guard lock(m_mutex);
    for (auto& [key, pair] : m_map)
        deleteGpuTile(gl, *pair.first);
    m_map.clear();
    m_order.clear();
}

int TileCache::size() const {
    std::lock_guard lock(m_mutex);
    return static_cast<int>(m_map.size());
}

std::size_t TileCache::tileBytes(const GpuTile& t) {
    // Count only textures actually allocated on the GPU. GL_R32F = 4 B/texel.
    int textures = (t.texture_r ? 1 : 0) + (t.texture_g ? 1 : 0) + (t.texture_b ? 1 : 0);
    return static_cast<std::size_t>(textures)
         * static_cast<std::size_t>(t.tile_w)
         * static_cast<std::size_t>(t.tile_h) * 4u;
}

std::size_t TileCache::residentBytesLocked() const {
    std::size_t total = 0;
    for (const auto& [key, pair] : m_map)
        total += tileBytes(*pair.first);
    return total;
}

std::size_t TileCache::residentBytes() const {
    std::lock_guard lock(m_mutex);
    return residentBytesLocked();
}

void TileCache::deleteGpuTile(QOpenGLFunctions_4_1_Core& gl, GpuTile& t) {
    if (t.texture_r)     { gl.glDeleteTextures(1, &t.texture_r); t.texture_r = 0; }
    if (t.texture_g)     { gl.glDeleteTextures(1, &t.texture_g); t.texture_g = 0; }
    if (t.texture_b)     { gl.glDeleteTextures(1, &t.texture_b); t.texture_b = 0; }
}
