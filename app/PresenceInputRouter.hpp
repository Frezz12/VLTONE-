#pragma once

#include "CollaborationTypes.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <optional>

class QWidget;

namespace collab {

struct PresenceSurfaceRegistration {
    SurfaceAddress address;
    PresencePolicy policy = PresencePolicy::Hidden;
    std::function<std::optional<SemanticPoint>(const QPointF&)> encode;
};

/// Application event filter that converts pointer events only on explicitly
/// registered surfaces into a fixed semantic schema. It never inspects widget
/// text, object names, URLs, tooltips, clipboard data or native plugin views.
class PresenceInputRouter final : public QObject {
    Q_OBJECT
public:
    explicit PresenceInputRouter(QObject* parent = nullptr);
    ~PresenceInputRouter() override;

    void registerSurface(QWidget* surface,
                         PresenceSurfaceRegistration registration);
    void unregisterSurface(QWidget* surface);
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// Headless privacy regression: dialogs/password/multiline/arbitrary
    /// numeric inputs stay hidden, while one reviewed scalar opt-in is exact.
    static bool checkSafetyForTest(QString* error = nullptr);

    void publishSelection(const SurfaceAddress& surface,
                          const QStringList& semanticIds);
    void publishDragPreview(const DragPreview& preview,
                            PresencePolicy policy = PresencePolicy::Exact);

signals:
    void presenceReady(const collab::PresencePacket& packet);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Match {
        QWidget* root = nullptr;
        PresenceSurfaceRegistration registration;
    };

    std::optional<Match> matchSurface(QWidget* widget) const;
    static bool isSensitiveWidget(QWidget* widget, QWidget* root);
    static PresencePolicy effectivePolicy(QWidget* target, QWidget* root,
                                          PresencePolicy registered);
    static SemanticPoint sanitizedPoint(const SemanticPoint& point,
                                        const SurfaceAddress& fallbackSurface);
    static void enableMouseTracking(QWidget* root);
    PresencePacket packetFor(const Match& match, QWidget* target,
                             const QPointF& localPosition,
                             PointerPhase phase) const;
    void queueMove(QWidget* target, const QPointF& localPosition);
    void sendNow(PresencePacket packet);
    void flushMove();

    /// The raw sample a throttled move is holding. Building the packet is the
    /// expensive half — sensitivity classification walks every ancestor widget
    /// and the point is sanitised field by field — and mouse moves arrive far
    /// faster than the 20 Hz wire budget, so the sample is kept cheap and the
    /// packet is built once, at flush time.
    struct PendingMove {
        QPointer<QWidget> target;
        QPointF localPosition;
    };

    QHash<QWidget*, PresenceSurfaceRegistration> m_surfaces;
    QPointer<QWidget> m_lastSurface;
    std::optional<PendingMove> m_pendingMove;
    QTimer m_moveTimer;
    QElapsedTimer m_lastMoveSent;
    quint64 m_sequence = 0;
    bool m_enabled = true;
};

} // namespace collab
