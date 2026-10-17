#pragma once
#include <QQuickWindow>
#include <QSGTexture>
#include <QImage>
#include <memory>
#include <list>
#include <unordered_map>

namespace ui::graphics {
// Each window owns this cache in its render thread. Active nodes hold shared
// references; unused textures are evicted in LRU order before allocating more.
class TextureStore {
    struct Entry { qint64 key; std::shared_ptr<QSGTexture> texture; qint64 bytes; };
    std::list<Entry> lru;
    std::unordered_map<qint64, std::list<Entry>::iterator> entries;
    qint64 bytes = 0;
    const qint64 byteBudget;
public:
    static constexpr qint64 budget = 128 * 1024 * 1024;
    explicit TextureStore(qint64 byteBudget = budget) : byteBudget(byteBudget) {}
    struct Statistics { quint64 hits = 0, misses = 0, evictionVisits = 0, evictions = 0; } statistics;
    qint64 retainedBytes() const { return bytes; }
    std::size_t retainedCount() const { return entries.size(); }
    static std::shared_ptr<TextureStore> forWindow(QQuickWindow* window) {
        static thread_local std::unordered_map<QQuickWindow*, std::weak_ptr<TextureStore>> stores;
        if (auto store = stores[window].lock()) return store;
        for (auto it = stores.begin(); it != stores.end();)
            if (it->second.expired()) it = stores.erase(it); else ++it;
        auto store = std::make_shared<TextureStore>();
        stores[window] = store;
        return store;
    }
    std::shared_ptr<QSGTexture> get(const QImage& image, QQuickWindow* window) {
        return get(image, [window, &image] {
            return std::shared_ptr<QSGTexture>(window->createTextureFromImage(image, QQuickWindow::TextureCanUseAtlas));
        });
    }
    template<class Create> std::shared_ptr<QSGTexture> get(const QImage& image, Create create) {
        const auto key = image.cacheKey();
        if (auto found = entries.find(key); found != entries.end()) {
            ++statistics.hits;
            lru.splice(lru.end(), lru, found->second);
            return found->second->texture;
        }
        ++statistics.misses;
        const qint64 cost = qint64(image.width()) * image.height() * 4;
        if (cost <= 0 || cost > byteBudget) return {};
        // Once full, replacing a stale texture must not scan the whole cache.
        // Live scene nodes pin their textures. Move those to the recent end so
        // the next replacement does not rescan the same pinned prefix.
        auto remaining = lru.size();
        while (bytes + cost > byteBudget && remaining-- > 0) {
            auto oldest = lru.begin();
            ++statistics.evictionVisits;
            if (oldest->texture.use_count() > 1) {
                lru.splice(lru.end(), lru, oldest);
                continue;
            }
            bytes -= oldest->bytes;
            entries.erase(oldest->key);
            lru.erase(oldest);
            ++statistics.evictions;
        }
        if (bytes + cost > byteBudget) return {};
        // Small glyphs and controls can share one GPU texture and therefore
        // one material batch. Large images retain their own texture.
        auto texture = create();
        if (texture) {
            texture->setFiltering(QSGTexture::Linear);
            lru.push_back({key, texture, cost});
            entries.emplace(key, std::prev(lru.end()));
            bytes += cost;
        }
        return texture;
    }
};
}
