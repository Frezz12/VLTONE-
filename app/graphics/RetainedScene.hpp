#pragma once
#include "SceneRecorder.hpp"
#include <QPainter>
#include <functional>
#include <unordered_map>

namespace ui::graphics {
// GUI-owned numerical geometry; the render thread receives immutable groups.
// A cache hit changes one GPU transform, without comparing/copying all vertices.
class RetainedScene {
public:
    static constexpr qint64 budget = 32 * 1024 * 1024;
    void clear() { m_entries.clear(); m_bytes = 0; }
    bool paint(QPainter& painter, quint64 key, QSize size, QPointF origin,
               const std::function<void(QPainter&)>& draw) {
        auto* sink = sceneGeometrySink(painter);
        if (!sink) return false;
        auto found = m_entries.find(key);
        if (found == m_entries.end()) {
            SceneRecorder recorder(size, painter.device()->devicePixelRatioF());
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
            while (m_bytes + group->bytes > budget || m_entries.size() >= 64) {
                auto oldest = m_entries.begin();
                for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
                    if (it->second.used < oldest->second.used) oldest = it;
                m_bytes -= oldest->second.group->bytes;
                m_entries.erase(oldest);
            }
            m_bytes += group->bytes;
            found = m_entries.emplace(key, Entry{std::move(group), 0}).first;
            ++m_builds;
        }
        found->second.used = ++m_clock;
        sink->appendGroup(found->second.group, origin);
        return true;
    }
    quint64 builds() const { return m_builds; }
private:
    struct Entry { std::shared_ptr<const SceneMeshGroup> group; quint64 used; };
    std::unordered_map<quint64, Entry> m_entries;
    qint64 m_bytes = 0;
    quint64 m_clock = 0, m_builds = 0;
};
}
