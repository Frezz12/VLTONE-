#include "PresenceInputRouter.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QChildEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTextEdit>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace collab {
namespace {

constexpr int kMoveIntervalMs = 50; // 20 Hz hard cap.

bool explicitlyHidden(const QWidget* widget) {
    return widget && widget->property("collaborationPresenceHidden").toBool();
}

QString safePropertyId(const QWidget* widget) {
    if (!widget) return {};
    const QString candidate =
        widget->property("collaborationPresenceTarget").toString();
    return isWireSafeSemanticId(candidate) ? candidate : QString();
}

QString safeTargetId(QWidget* widget, QWidget* root) {
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        const QString id = safePropertyId(current);
        if (!id.isEmpty()) return id;
        if (current == root) break;
    }
    return {};
}

bool safeControlOptIn(QWidget* widget, QWidget* root) {
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (current->property("collaborationPresenceSafeControl").toBool() &&
            !safePropertyId(current).isEmpty()) {
            return true;
        }
        if (current == root) break;
    }
    return false;
}

} // namespace

PresenceInputRouter::PresenceInputRouter(QObject* parent) : QObject(parent) {
    m_moveTimer.setSingleShot(true);
    m_moveTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_moveTimer, &QTimer::timeout, this,
            &PresenceInputRouter::flushMove);
    m_lastMoveSent.start();
    if (qApp) qApp->installEventFilter(this);
}

PresenceInputRouter::~PresenceInputRouter() {
    if (qApp) qApp->removeEventFilter(this);
}

void PresenceInputRouter::registerSurface(
    QWidget* surface, PresenceSurfaceRegistration registration) {
    if (!surface) return;
    const bool firstRegistration = !m_surfaces.contains(surface);
    registration.address.instanceId =
        safeSemanticId(registration.address.instanceId);
    registration.address.contextId =
        safeSemanticId(registration.address.contextId);
    m_surfaces.insert(surface, std::move(registration));
    enableMouseTracking(surface);
    if (firstRegistration) {
        connect(surface, &QObject::destroyed, this, [this, surface] {
            m_surfaces.remove(surface);
            if (m_lastSurface == surface) m_lastSurface.clear();
        });
    }
}

void PresenceInputRouter::unregisterSurface(QWidget* surface) {
    m_surfaces.remove(surface);
    if (m_lastSurface == surface) m_lastSurface.clear();
}

void PresenceInputRouter::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    m_pendingMove.reset();
    m_moveTimer.stop();
    if (!enabled && m_lastSurface) {
        const auto it = m_surfaces.constFind(m_lastSurface.data());
        if (it != m_surfaces.cend()) {
            PresencePacket packet;
            packet.phase = PointerPhase::Leave;
            packet.policy = PresencePolicy::Hidden;
            packet.point.surface = it->address;
            sendNow(std::move(packet));
        }
    }
    m_lastSurface.clear();
}

void PresenceInputRouter::publishSelection(
    const SurfaceAddress& surface, const QStringList& semanticIds) {
    if (!m_enabled) return;
    PresencePacket packet;
    packet.policy = PresencePolicy::Exact;
    packet.point.surface = surface;
    packet.selectionChange = true;
    const qsizetype count = std::min<qsizetype>(semanticIds.size(), 256);
    for (qsizetype i = 0; i < count; ++i) {
        if (isWireSafeSemanticId(semanticIds.at(i)))
            packet.selectionIds.push_back(semanticIds.at(i));
    }
    sendNow(std::move(packet));
}

void PresenceInputRouter::publishDragPreview(const DragPreview& preview,
                                             PresencePolicy policy) {
    if (!m_enabled) return;
    PresencePacket packet;
    packet.policy = policy;
    packet.point = sanitizedPoint(preview.destination,
                                  preview.destination.surface);
    packet.drag = preview;
    packet.drag.kind = isWireSafeSemanticId(preview.kind)
                           ? preview.kind
                           : QString();
    packet.drag.objectIds.clear();
    const qsizetype count = std::min<qsizetype>(preview.objectIds.size(), 64);
    for (qsizetype i = 0; i < count; ++i) {
        if (isWireSafeSemanticId(preview.objectIds.at(i)))
            packet.drag.objectIds.push_back(preview.objectIds.at(i));
    }
    packet.drag.destination = packet.point;
    sendNow(std::move(packet));
}

bool PresenceInputRouter::eventFilter(QObject* watched, QEvent* event) {
    auto* target = qobject_cast<QWidget*>(watched);
    if (!target || !event) return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::ChildAdded) {
        auto* childEvent = static_cast<QChildEvent*>(event);
        if (auto* child = qobject_cast<QWidget*>(childEvent->child())) {
            if (matchSurface(target)) enableMouseTracking(child);
        }
        return QObject::eventFilter(watched, event);
    }
    if (!m_enabled) return QObject::eventFilter(watched, event);

    const auto match = matchSurface(target);
    if (!match) return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::Leave && target == match->root) {
        PresencePacket packet;
        packet.phase = PointerPhase::Leave;
        packet.policy = PresencePolicy::Hidden;
        packet.point.surface = match->registration.address;
        sendNow(std::move(packet));
        m_lastSurface.clear();
        return QObject::eventFilter(watched, event);
    }
    if (event->type() != QEvent::MouseMove &&
        event->type() != QEvent::MouseButtonPress &&
        event->type() != QEvent::MouseButtonRelease) {
        return QObject::eventFilter(watched, event);
    }

    auto* mouse = static_cast<QMouseEvent*>(event);
    const QPointF local = QPointF(match->root->mapFromGlobal(
        mouse->globalPosition().toPoint()));
    PointerPhase phase = PointerPhase::Move;
    if (event->type() == QEvent::MouseButtonPress) phase = PointerPhase::Press;
    else if (event->type() == QEvent::MouseButtonRelease)
        phase = PointerPhase::Release;
    PresencePacket packet = packetFor(*match, target, local, phase);
    m_lastSurface = match->root;
    if (phase == PointerPhase::Move) queueMove(std::move(packet));
    else sendNow(std::move(packet));
    return QObject::eventFilter(watched, event);
}

std::optional<PresenceInputRouter::Match> PresenceInputRouter::matchSurface(
    QWidget* widget) const {
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        const auto it = m_surfaces.constFind(current);
        if (it != m_surfaces.cend()) return Match{current, it.value()};
    }
    return std::nullopt;
}

bool PresenceInputRouter::isSensitiveWidget(QWidget* widget, QWidget* root) {
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (explicitlyHidden(current) || qobject_cast<QTextEdit*>(current) ||
            qobject_cast<QPlainTextEdit*>(current) ||
            qobject_cast<QDialog*>(current) ||
            (qobject_cast<QComboBox*>(current) &&
             qobject_cast<QComboBox*>(current)->isEditable()) ||
            current->inherits("QWebEngineView") ||
            current->inherits("PluginEditorWindow")) {
            return true;
        }
        if (auto* lineEdit = qobject_cast<QLineEdit*>(current)) {
            // Passwords and arbitrary text remain hidden even if a parent was
            // accidentally marked safe. A normal single-value control (BPM,
            // pan, gain) may opt in with a reviewed semantic id.
            if (lineEdit->echoMode() != QLineEdit::Normal ||
                !safeControlOptIn(widget, root))
                return true;
        }
        if (qobject_cast<QAbstractSpinBox*>(current) &&
            !safeControlOptIn(widget, root))
            return true;
        if (current == root) break;
    }
    return false;
}

PresencePolicy PresenceInputRouter::effectivePolicy(
    QWidget* target, QWidget* root, PresencePolicy registered) {
    if (registered == PresencePolicy::Hidden ||
        isSensitiveWidget(target, root)) {
        return PresencePolicy::Hidden;
    }
    for (QWidget* current = target; current; current = current->parentWidget()) {
        const QString override =
            current->property("collaborationPresencePolicy").toString();
        if (override == QLatin1String("hidden")) return PresencePolicy::Hidden;
        if (override == QLatin1String("coarse"))
            return PresencePolicy::Coarse;
        if (current == root) break;
    }
    return registered;
}

SemanticPoint PresenceInputRouter::sanitizedPoint(
    const SemanticPoint& point, const SurfaceAddress& fallbackSurface) {
    SemanticPoint safe = point;
    if (safe.surface.kind == SurfaceKind::Unknown) safe.surface = fallbackSurface;
    safe.surface.instanceId = isWireSafeSemanticId(safe.surface.instanceId)
                                  ? safe.surface.instanceId
                                  : safeSemanticId(fallbackSurface.instanceId);
    safe.surface.contextId = isWireSafeSemanticId(safe.surface.contextId)
                                 ? safe.surface.contextId
                                 : safeSemanticId(fallbackSurface.contextId);
    const auto keepSafe = [](const QString& value) {
        return isWireSafeSemanticId(value) ? value : QString();
    };
    safe.targetId = keepSafe(safe.targetId);
    safe.trackId = keepSafe(safe.trackId);
    safe.clipId = keepSafe(safe.clipId);
    safe.parameterId = keepSafe(safe.parameterId);
    if (!std::isfinite(safe.normalized.x()) ||
        !std::isfinite(safe.normalized.y())) {
        safe.normalized = QPointF(-1.0, -1.0);
    } else {
        safe.normalized.setX(std::clamp(safe.normalized.x(), 0.0, 1.0));
        safe.normalized.setY(std::clamp(safe.normalized.y(), 0.0, 1.0));
    }
    if (!std::isfinite(safe.timeSeconds) || safe.timeSeconds < 0.0)
        safe.timeSeconds = -1.0;
    if (!std::isfinite(safe.beat) || safe.beat < 0.0) safe.beat = -1.0;
    if (safe.pitch < 0 || safe.pitch > 127) safe.pitch = -1;
    if (!std::isfinite(safe.laneFraction) || safe.laneFraction < 0.0)
        safe.laneFraction = -1.0;
    else
        safe.laneFraction = std::clamp(safe.laneFraction, 0.0, 1.0);
    return safe;
}

void PresenceInputRouter::enableMouseTracking(QWidget* root) {
    if (!root) return;
    root->setMouseTracking(true);
    const auto children = root->findChildren<QWidget*>();
    for (QWidget* child : children) child->setMouseTracking(true);
}

PresencePacket PresenceInputRouter::packetFor(
    const Match& match, QWidget* target, const QPointF& localPosition,
    PointerPhase phase) const {
    PresencePacket packet;
    packet.phase = phase;
    packet.policy = effectivePolicy(target, match.root,
                                    match.registration.policy);
    packet.point.surface = match.registration.address;
    if (packet.policy == PresencePolicy::Hidden) return packet;

    packet.point.normalized =
        normalizedSurfacePoint(localPosition, match.root->size());
    if (match.registration.encode) {
        if (const auto encoded = match.registration.encode(localPosition))
            packet.point = sanitizedPoint(*encoded, match.registration.address);
    }
    const QString targetId = safeTargetId(target, match.root);
    if (!targetId.isEmpty()) packet.point.targetId = targetId;

    if (packet.policy == PresencePolicy::Coarse) {
        // Coarse means surface activity only. Coordinates — even quantised —
        // can reveal which item, prompt or path a participant is inspecting.
        packet.point.normalized = QPointF(-1.0, -1.0);
        packet.point.targetId.clear();
        packet.point.trackId.clear();
        packet.point.clipId.clear();
        packet.point.parameterId.clear();
        packet.point.timeSeconds = -1.0;
        packet.point.beat = -1.0;
        packet.point.pitch = -1;
        packet.point.laneFraction = -1.0;
    }
    return packet;
}

void PresenceInputRouter::queueMove(PresencePacket packet) {
    const qint64 elapsed = m_lastMoveSent.elapsed();
    if (elapsed >= kMoveIntervalMs && !m_moveTimer.isActive()) {
        sendNow(std::move(packet));
        return;
    }
    m_pendingMove = std::move(packet);
    const int remaining = std::max(1, kMoveIntervalMs - int(elapsed));
    if (!m_moveTimer.isActive()) m_moveTimer.start(remaining);
}

void PresenceInputRouter::sendNow(PresencePacket packet) {
    if (!m_enabled && packet.policy != PresencePolicy::Hidden) return;
    packet.clientSequence = ++m_sequence;
    packet.sentAtMs = QDateTime::currentMSecsSinceEpoch();
    emit presenceReady(packet);
    m_lastMoveSent.restart();
}

void PresenceInputRouter::flushMove() {
    if (!m_pendingMove || !m_enabled) {
        m_pendingMove.reset();
        return;
    }
    PresencePacket packet = std::move(*m_pendingMove);
    m_pendingMove.reset();
    sendNow(std::move(packet));
}

bool PresenceInputRouter::checkSafetyForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QWidget root;

    QDialog dialog(&root);
    QWidget dialogChild(&dialog);
    if (!isSensitiveWidget(&dialogChild, &root))
        return fail(QStringLiteral("dialog inherited an exact presence surface"));

    QLineEdit password(&root);
    password.setEchoMode(QLineEdit::Password);
    password.setProperty("collaborationPresenceSafeControl", true);
    password.setProperty("collaborationPresenceTarget",
                         QStringLiteral("transport.bpm"));
    if (!isSensitiveWidget(&password, &root))
        return fail(QStringLiteral("password control bypassed the safe allowlist"));

    QPlainTextEdit multiline(&root);
    multiline.setProperty("collaborationPresenceSafeControl", true);
    multiline.setProperty("collaborationPresenceTarget",
                          QStringLiteral("transport.bpm"));
    if (!isSensitiveWidget(&multiline, &root))
        return fail(QStringLiteral("multiline text bypassed the safe allowlist"));

    QSpinBox arbitraryNumber(&root);
    if (!isSensitiveWidget(&arbitraryNumber, &root))
        return fail(QStringLiteral("unreviewed numeric input became exact"));

    QSpinBox reviewedScalar(&root);
    reviewedScalar.setProperty("collaborationPresenceSafeControl", true);
    reviewedScalar.setProperty("collaborationPresenceTarget",
                               QStringLiteral("transport.bpm"));
    if (isSensitiveWidget(&reviewedScalar, &root))
        return fail(QStringLiteral("reviewed scalar presence opt-in was ignored"));
    return true;
}

} // namespace collab
