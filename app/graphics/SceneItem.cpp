#include "SceneItem.hpp"
#include "TextureStore.hpp"
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGVertexColorMaterial>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGTextureMaterial>
#include <QSGTransformNode>
#include <QThread>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace ui::graphics {
namespace {
// A uniform colour makes every shade a different material/draw call. Vertex
// colours let Qt batch the many channel borders, fills and ticks together.
// Floats preserve QColor's precision, including premultiplied translucent ink.
struct ColorVertex { float x, y, r, g, b, a; };
const QSGGeometry::AttributeSet& colorAttributes() {
    static const QSGGeometry::Attribute attributes[] = {
        QSGGeometry::Attribute::create(0, 2, QSGGeometry::FloatType, true),
        QSGGeometry::Attribute::create(1, 4, QSGGeometry::FloatType)};
    static const QSGGeometry::AttributeSet set{2, sizeof(ColorVertex), attributes};
    return set;
}
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
    QRegion mask;
    QSGNode* content = nullptr;
    std::vector<MeshNode*> meshes;
    LayerNode() {
        content = new QSGNode;
        appendChildNode(content);
    }
};
struct ScrollNode final : QSGClipNode {
    QSGTransformNode* page = new QSGTransformNode;
    ScrollNode() {
        setIsRectangular(true);
        setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4));
        setFlag(QSGNode::OwnsGeometry);
        appendChildNode(page);
    }
};
struct RootNode final : QSGNode {
    std::unordered_map<quint64, LayerNode*> layers;
    std::unordered_map<quint64, ScrollNode*> scrolls;
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
    const QMatrix4x4 matrix(mesh.snapTranslation
        ? pixelAlignedTranslation(mesh.transform, window->devicePixelRatio()) : mesh.transform);
    if (node->transform->matrix() != matrix) node->transform->setMatrix(matrix);
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
            colorAttributes(), 0));
        node->drawing->geometry()->setDrawingMode(QSGGeometry::DrawTriangles);
        node->drawing->setMaterial(textured ? static_cast<QSGMaterial*>(new QSGTextureMaterial) : new QSGVertexColorMaterial);
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
        node->drawing->markDirty(QSGNode::DirtyMaterial);
    }
    if (textured && (newType || node->previous.smoothTexture != mesh.smoothTexture)) {
        static_cast<QSGTextureMaterial*>(node->drawing->material())->setFiltering(
            mesh.smoothTexture ? QSGTexture::Linear : QSGTexture::Nearest);
        node->drawing->markDirty(QSGNode::DirtyMaterial);
    }
    if (newType || changedTexture || node->previous.vertices != mesh.vertices ||
        (!textured && node->previous.color != mesh.color)) {
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
            auto* vertices = static_cast<ColorVertex*>(geometry->vertexData());
            const float alpha = mesh.color.alphaF();
            const float red = mesh.color.redF() * alpha, green = mesh.color.greenF() * alpha,
                        blue = mesh.color.blueF() * alpha;
            for (int i = 0; i < mesh.vertices.size(); ++i)
                vertices[i] = {mesh.vertices[i].x, mesh.vertices[i].y, red, green, blue, alpha};
        }
        node->drawing->markDirty(QSGNode::DirtyGeometry);
    }
    if (!textured && (newType || (node->previous.color.alphaF() < 1.0f) != (mesh.color.alphaF() < 1.0f))) {
        node->drawing->material()->setFlag(QSGMaterial::Blending, mesh.color.alphaF() < 1.0f);
        node->drawing->markDirty(QSGNode::DirtyMaterial);
    }
    if (node->opacity() != mesh.opacity) node->setOpacity(mesh.opacity);
    node->previous = mesh;
    return true;
}
void updateLayerClip(LayerNode* node, const SceneLayer& layer) {
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
    if (node->clip && (node->clip->clipRect() != layer.clip || node->mask != layer.mask)) {
        node->clip->setClipRect(layer.clip);
        node->clip->setIsRectangular(layer.mask.isEmpty());
        auto* geometry = node->clip->geometry();
        if (layer.mask.isEmpty()) {
            geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            geometry->allocate(4);
            QSGGeometry::updateRectGeometry(geometry, layer.clip);
        } else {
            // QWidget masks also clip descendants. Enforce that at the layer
            // boundary, so a child's painter cannot reset its parent's clip.
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometry->allocate(layer.mask.rectCount() * 6);
            auto* vertices = geometry->vertexDataAsPoint2D();
            for (const QRect& rect : layer.mask) {
                const float l = rect.x(), t = rect.y();
                const float r = l + rect.width(), b = t + rect.height();
                (vertices++)->set(l, t); (vertices++)->set(r, t); (vertices++)->set(l, b);
                (vertices++)->set(r, t); (vertices++)->set(r, b); (vertices++)->set(l, b);
            }
        }
        node->mask = layer.mask;
        node->clip->markDirty(QSGNode::DirtyGeometry);
    }
}
bool populate(LayerNode* node, const SceneLayer& layer, QQuickWindow* window, TextureStore& textures) {
    node->revision = layer.revision;
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
    QSGNode* previousInScroll = nullptr;
    ScrollNode* activeScroll = nullptr;
    std::unordered_set<quint64> wantedScrolls;
    const auto placeAfter = [](QSGNode* parent, QSGNode* node, QSGNode* preceding) {
        if (node->parent() == parent && node->previousSibling() == preceding) return;
        if (node->parent()) node->parent()->removeChildNode(node);
        if (preceding) parent->insertChildNodeAfter(node, preceding);
        else parent->prependChildNode(node);
    };
    if (m_snapshot) for (const auto& layer : m_snapshot->layers) {
        ScrollNode* scroll = nullptr;
        QPointF origin = layer->origin;
        if (layer->scrollGroup) {
            const auto& group = *layer->scrollGroup;
            wantedScrolls.insert(group.id);
            auto& entry = root->scrolls[group.id];
            if (!entry) entry = new ScrollNode;
            scroll = entry;
            if (scroll != activeScroll) {
                placeAfter(root, scroll, previous);
                previous = scroll;
                previousInScroll = nullptr;
                if (scroll->clipRect() != group.clip) {
                    scroll->setClipRect(group.clip);
                    QSGGeometry::updateRectGeometry(scroll->geometry(), group.clip);
                    scroll->markDirty(QSGNode::DirtyGeometry);
                }
                const QMatrix4x4 matrix(pixelAlignedTranslation(
                    QTransform::fromTranslate(group.origin.x(), group.origin.y()), window()->devicePixelRatio()));
                if (scroll->page->matrix() != matrix) scroll->page->setMatrix(matrix);
            }
            // Match the old device-aligned absolute placement at fractional DPI.
            const auto aligned = pixelAlignedTranslation(
                QTransform::fromTranslate(origin.x(), origin.y()), window()->devicePixelRatio());
            origin = QPointF(aligned.dx() - scroll->page->matrix()(0, 3),
                             aligned.dy() - scroll->page->matrix()(1, 3));
        }
        activeScroll = scroll;
        auto found = root->layers.find(layer->id);
        LayerNode* node = found == root->layers.end() ? nullptr : found->second;
        if (!node) {
            node = new LayerNode;
            root->layers[layer->id] = node;
        }
        updateLayerClip(node, *layer);
        if (node->revision != layer->revision && !populate(node, *layer, window(), *root->textures)) {
            // Do not leave a material pointing at an unavailable texture.
            delete node; root->layers.erase(layer->id);
            if (!root->failed) {
                root->failed = true;
                emit resourceError(tr("The GPU texture budget was exceeded or a texture could not be created."));
            }
            break;
        }
        const auto translation = QTransform::fromTranslate(origin.x(), origin.y());
        const QMatrix4x4 matrix(scroll ? translation : pixelAlignedTranslation(translation, window()->devicePixelRatio()));
        // setMatrix marks the entire subtree dirty even for an equal matrix.
        // Scrolling one canvas must not invalidate the cached batches of every
        // stationary panel/control in the workspace.
        if (node->matrix() != matrix) node->setMatrix(matrix);
        placeAfter(scroll ? static_cast<QSGNode*>(scroll->page) : root, node,
                   scroll ? previousInScroll : previous);
        if (scroll) previousInScroll = node;
        else previous = node;
    }
    for (auto it = root->scrolls.begin(); it != root->scrolls.end();) {
        if (wantedScrolls.contains(it->first)) { ++it; continue; }
        delete it->second;
        it = root->scrolls.erase(it);
    }
    return root;
}
} // namespace ui::graphics
