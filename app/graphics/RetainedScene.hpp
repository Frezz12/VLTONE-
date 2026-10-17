#pragma once
#include "SceneRecorder.hpp"
#include <QPainter>
#include <functional>
#include <list>
#include <unordered_map>

namespace ui::graphics {
// GUI-owned numerical geometry; the render thread receives immutable groups.
// A cache hit changes one GPU transform, without comparing/copying all vertices.
class RetainedScene {
public:
    static constexpr qint64 budget = 32 * 1024 * 1024;
    void clear() { m_entries.clear(); m_lru.clear(); m_bytes = 0; }
    template<class Predicate> void invalidate(Predicate affected) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (affected(it->first)) {
                m_bytes -= it->second.group->bytes;
                m_lru.erase(it->second.recency);
                it = m_entries.erase(it);
            }
            else ++it;
        }
    }
    bool paint(QPainter& painter, quint64 key, QSize size, QPointF origin,
               const std::function<void(QPainter&)>& draw) {
        auto* sink = sceneGeometrySink(painter);
        if (!sink) return false;
        const qreal dpr = painter.device()->devicePixelRatioF();
        if (m_dpr != dpr) { clear(); m_dpr = dpr; }
        auto found = m_entries.find(key);
        if (found == m_entries.end()) {
            SceneRecorder recorder(size, dpr);
            QPainter local(&recorder);
            local.setFont(painter.font());
            draw(local);
            local.end();
            if (!recorder.supported()) { sink->appendGroup({}, {}); return true; }
            auto group = std::make_shared<SceneMeshGroup>();
            group->meshes = recorder.takeMeshes();
            for (const auto& mesh : group->meshes) {
                group->bytes += qint64(mesh.vertices.size()) * sizeof(SceneVertex);
                if (mesh.clip) group->bytes += qint64(mesh.clip->triangles.size()) * sizeof(SceneVertex);
                if (mesh.group) group->bytes += mesh.group->bytes;
            }
            // Oversized visible groups still render once, without entering the
            // cache. SceneRecorder and WorkspaceSurface enforce admission too.
            if (group->bytes > budget) { sink->appendGroup(std::move(group), origin); return true; }
            while (m_bytes + group->bytes > budget || m_entries.size() >= 512) {
                const auto oldest = m_entries.find(m_lru.front());
                m_bytes -= oldest->second.group->bytes;
                m_entries.erase(oldest);
                m_lru.pop_front();
            }
            m_bytes += group->bytes;
            m_lru.push_back(key);
            found = m_entries.emplace(key, Entry{std::move(group), std::prev(m_lru.end())}).first;
            ++m_builds;
        }
        m_lru.splice(m_lru.end(), m_lru, found->second.recency);
        sink->appendGroup(found->second.group, origin);
        return true;
    }
    quint64 builds() const { return m_builds; }
private:
    struct Entry { std::shared_ptr<const SceneMeshGroup> group; std::list<quint64>::iterator recency; };
    std::list<quint64> m_lru;
    std::unordered_map<quint64, Entry> m_entries;
    qint64 m_bytes = 0;
    qreal m_dpr = 0;
    quint64 m_builds = 0;
};
}
