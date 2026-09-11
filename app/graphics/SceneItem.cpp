#include "SceneItem.hpp"
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGTextureMaterial>
#include <QSGTransformNode>
#include <QThread>
#include <unordered_map>
#include <unordered_set>

namespace ui::graphics {
namespace {
// Each window owns this cache in its render thread. Active nodes hold shared
// references; unused textures are evicted in LRU order before allocating more.
class TextureStore {
    struct Entry { std::shared_ptr<QSGTexture> texture; quint64 used; qint64 bytes; };
    std::unordered_map<qint64, Entry> entries;
    qint64 bytes = 0;
    quint64 clock = 0;
public:
    static constexpr qint64 budget = 128 * 1024 * 1024;
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
        const auto key = image.cacheKey();
        if (auto found = entries.find(key); found != entries.end()) {
            found->second.used = ++clock;
            return found->second.texture;
        }
        const qint64 cost = qint64(image.width()) * image.height() * 4;
        while (bytes + cost > budget) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second.texture.use_count() == 1 &&
                    (oldest == entries.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == entries.end()) return {};
            bytes -= oldest->second.bytes;
            entries.erase(oldest);
        }
        // Small glyphs and controls can share one GPU texture and therefore
        // one material batch. Large images retain their own texture.
        auto texture = std::shared_ptr<QSGTexture>(window->createTextureFromImage(image, QQuickWindow::TextureCanUseAtlas));
        if (texture) {
            texture->setFiltering(QSGTexture::Linear);
            entries.emplace(key, Entry{texture, ++clock, cost});
            bytes += cost;
        }
        return texture;
    }
};
struct MeshNode final : QSGOpacityNode {
    QSGGeometryNode* drawing = nullptr;
    QSGClipNode* clip = nullptr;
    QSGTransformNode* transform = nullptr;
    SceneMesh previous;
    std::shared_ptr<QSGTexture> texture;
    std::vector<MeshNode*> children;
    MeshNode() {
        transform = new QSGTransformNode;
        appendChildNode(transform);
    }
};
struct LayerNode final : QSGTransformNode {
    quint64 revision = 0;
    QSGClipNode* clip = nullptr;
    QSGNode* content = nullptr;
    std::vector<MeshNode*> meshes;
    LayerNode() {
        content = new QSGNode;
        appendChildNode(content);
    }
};
struct RootNode final : QSGNode {
    std::unordered_map<quint64, LayerNode*> layers;
    std::shared_ptr<TextureStore> textures;
    explicit RootNode(QQuickWindow* window) : textures(TextureStore::forWindow(window)) {}
    std::shared_ptr<const SceneSnapshot> snapshot;
    bool failed = false;
};
bool updateMesh(MeshNode* node, const SceneMesh& mesh, QQuickWindow* window, TextureStore& textures) {
    if (mesh.clip) {
        if (!node->clip) {
            node->clip = new QSGClipNode;
            node->clip->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0));
            node->clip->setFlag(QSGNode::OwnsGeometry);
            node->removeChildNode(node->transform);
            node->clip->appendChildNode(node->transform);
            node->appendChildNode(node->clip);
        }
        const auto& clip = *mesh.clip;
        const auto& old = node->previous.clip;
        if (!old || old->bounds != clip.bounds || old->rectangular != clip.rectangular || old->triangles != clip.triangles) {
            node->clip->setIsRectangular(clip.rectangular);
            node->clip->setClipRect(clip.bounds);
            auto* geometry = node->clip->geometry();
            if (clip.rectangular) {
                geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
                geometry->allocate(4);
                QSGGeometry::updateRectGeometry(geometry, clip.bounds);
            } else {
                geometry->setDrawingMode(QSGGeometry::DrawTriangles);
                geometry->allocate(int(clip.triangles.size()));
                auto* vertices = geometry->vertexDataAsPoint2D();
                for (int i = 0; i < clip.triangles.size(); ++i)
                    vertices[i].set(clip.triangles[i].x, clip.triangles[i].y);
            }
            node->clip->markDirty(QSGNode::DirtyGeometry);
        }
    } else if (node->clip) {
        node->clip->removeChildNode(node->transform);
        delete node->clip; node->clip = nullptr;
        node->appendChildNode(node->transform);
    }
    if (node->previous.transform != mesh.transform)
        node->transform->setMatrix(QMatrix4x4(mesh.transform));
    if (mesh.group) {
        if (node->drawing) { delete node->drawing; node->drawing = nullptr; node->texture.reset(); }
        if (node->previous.group != mesh.group) {
            while (node->children.size() > mesh.group->meshes.size()) {
                delete node->children.back(); node->children.pop_back();
            }
            for (std::size_t i = 0; i < mesh.group->meshes.size(); ++i) {
                if (i == node->children.size()) {
                    auto* child = new MeshNode;
                    node->transform->appendChildNode(child);
                    node->children.push_back(child);
                }
                if (!updateMesh(node->children[i], mesh.group->meshes[i], window, textures)) return false;
            }
        }
        if (node->opacity() != mesh.opacity) node->setOpacity(mesh.opacity);
        node->previous = mesh;
        return true;
    }
    for (auto* child : node->children) delete child;
    node->children.clear();
    const bool textured = !mesh.texture.isNull();
    const bool newType = !node->drawing || node->previous.texture.isNull() != mesh.texture.isNull();
    if (newType) {
        delete node->drawing;
        node->texture.reset();
        node->drawing = new QSGGeometryNode;
        node->drawing->setGeometry(new QSGGeometry(textured ? QSGGeometry::defaultAttributes_TexturedPoint2D() :
            QSGGeometry::defaultAttributes_Point2D(), 0));
        node->drawing->geometry()->setDrawingMode(QSGGeometry::DrawTriangles);
        node->drawing->setMaterial(textured ? static_cast<QSGMaterial*>(new QSGTextureMaterial) : new QSGFlatColorMaterial);
        node->drawing->setFlag(QSGNode::OwnsGeometry);
        node->drawing->setFlag(QSGNode::OwnsMaterial);
        node->transform->appendChildNode(node->drawing);
    }
    const bool changedTexture = textured && (newType || node->previous.texture.cacheKey() != mesh.texture.cacheKey());
    if (changedTexture) {
        node->texture.reset();
        node->texture = textures.get(mesh.texture, window);
        if (!node->texture) return false;
        auto* material = static_cast<QSGTextureMaterial*>(node->drawing->material());
        material->setTexture(node->texture.get());
        material->setFiltering(QSGTexture::Linear);
        node->drawing->markDirty(QSGNode::DirtyMaterial);
    }
    if (newType || changedTexture || node->previous.vertices != mesh.vertices) {
        auto* geometry = node->drawing->geometry();
        geometry->allocate(int(mesh.vertices.size()));
        if (textured) {
            const auto uv = node->texture->normalizedTextureSubRect();
            auto* vertices = geometry->vertexDataAsTexturedPoint2D();
            for (int i = 0; i < mesh.vertices.size(); ++i) {
                const auto& v = mesh.vertices[i];
                vertices[i].set(v.x, v.y, float(uv.x() + v.u * uv.width()), float(uv.y() + v.v * uv.height()));
            }
        } else {
            auto* vertices = geometry->vertexDataAsPoint2D();
            for (int i = 0; i < mesh.vertices.size(); ++i)
                vertices[i].set(mesh.vertices[i].x, mesh.vertices[i].y);
        }
        node->drawing->markDirty(QSGNode::DirtyGeometry);
    }
    if (!textured && (newType || node->previous.color != mesh.color)) {
        static_cast<QSGFlatColorMaterial*>(node->drawing->material())->setColor(mesh.color);
        node->drawing->markDirty(QSGNode::DirtyMaterial);
    }
    if (node->opacity() != mesh.opacity) node->setOpacity(mesh.opacity);
    node->previous = mesh;
    return true;
}
bool populate(LayerNode* node, const SceneLayer& layer, QQuickWindow* window, TextureStore& textures) {
    node->revision = layer.revision;
    if (layer.clipRequired && !node->clip) {
        node->clip = new QSGClipNode;
        node->clip->setIsRectangular(true);
        node->clip->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4));
        node->clip->setFlag(QSGNode::OwnsGeometry);
        node->removeChildNode(node->content);
        node->clip->appendChildNode(node->content);
        node->appendChildNode(node->clip);
    } else if (!layer.clipRequired && node->clip) {
        node->clip->removeChildNode(node->content);
        delete node->clip; node->clip = nullptr;
        node->appendChildNode(node->content);
    }
    if (node->clip && node->clip->clipRect() != layer.clip) {
        node->clip->setClipRect(layer.clip);
        QSGGeometry::updateRectGeometry(node->clip->geometry(), layer.clip);
        node->clip->markDirty(QSGNode::DirtyGeometry);
    }
    while (node->meshes.size() > layer.meshes.size()) {
        delete node->meshes.back(); node->meshes.pop_back();
    }
    for (std::size_t i = 0; i < layer.meshes.size(); ++i) {
        if (i == node->meshes.size()) {
            auto* mesh = new MeshNode;
            node->content->appendChildNode(mesh);
            node->meshes.push_back(mesh);
        }
        if (!updateMesh(node->meshes[i], layer.meshes[i], window, textures)) return false;
    }
    return true;
}
} // namespace
SceneItem::SceneItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
}
void SceneItem::setSnapshot(std::shared_ptr<const SceneSnapshot> snapshot) {
    Q_ASSERT(QThread::currentThread() == thread());
    m_snapshot = std::move(snapshot);
    update();
}
QSGNode* SceneItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = oldNode ? static_cast<RootNode*>(oldNode) : new RootNode(window());
    if (root->snapshot == m_snapshot) return root;
    root->snapshot = m_snapshot;
    std::unordered_set<quint64> wanted;
    if (m_snapshot) for (const auto& layer : m_snapshot->layers) wanted.insert(layer->id);
    for (auto it = root->layers.begin(); it != root->layers.end();) {
        if (!wanted.contains(it->first)) { delete it->second; it = root->layers.erase(it); }
        else ++it;
    }
    QSGNode* previous = nullptr;
    if (m_snapshot) for (const auto& layer : m_snapshot->layers) {
        auto found = root->layers.find(layer->id);
        LayerNode* node = found == root->layers.end() ? nullptr : found->second;
        if (!node) {
            node = new LayerNode;
            root->layers[layer->id] = node;
        }
        if (node->revision != layer->revision && !populate(node, *layer, window(), *root->textures)) {
            // Do not leave a material pointing at an unavailable texture.
            delete node; root->layers.erase(layer->id);
            if (!root->failed) {
                root->failed = true;
                emit resourceError(tr("The GPU texture budget was exceeded or a texture could not be created."));
            }
            break;
        }
        QMatrix4x4 matrix; matrix.translate(float(layer->origin.x()), float(layer->origin.y()));
        // setMatrix marks the entire subtree dirty even for an equal matrix.
        // Scrolling one canvas must not invalidate the cached batches of every
        // stationary panel/control in the workspace.
        if (node->matrix() != matrix) node->setMatrix(matrix);
        if (node->parent() != root || node->previousSibling() != previous) {
            if (node->parent()) node->parent()->removeChildNode(node);
            if (previous) root->insertChildNodeAfter(node, previous);
            else root->prependChildNode(node);
        }
        previous = node;
    }
    return root;
}
} // namespace ui::graphics
