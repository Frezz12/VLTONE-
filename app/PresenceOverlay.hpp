#pragma once

#include "CollaborationTypes.hpp"

#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

namespace collab {

class PresenceStore;

/// Transparent, mouse-pass-through layer attached to one application-owned Qt
/// surface. Native third-party plugin editors are intentionally never overlaid.
class PresenceOverlay final : public QWidget {
    Q_OBJECT
public:
    /// A semantic mapper owns the complete decision. Returning nullopt hides a
    /// point for another clip/slot instead of falling back to coordinates that
    /// belong to that other context.
    using PointMapper =
        std::function<std::optional<QPointF>(const SemanticPoint&, const QSize&)>;

    PresenceOverlay(QWidget* surface, SurfaceAddress address,
                    PresenceStore* store, QWidget* parent = nullptr);

    void setPointMapper(PointMapper mapper);
    SurfaceAddress surfaceAddress() const { return m_address; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::optional<QPointF> mapPoint(const SemanticPoint& point) const;
    void syncGeometry();
    void onPresenceChanged();

    QWidget* m_surface = nullptr;
    SurfaceAddress m_address;
    PresenceStore* m_store = nullptr;
    PointMapper m_mapper;
    QTimer m_animationTimer;
    bool m_reduceMotion = false;
};

} // namespace collab
