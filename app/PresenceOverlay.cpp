#include "PresenceOverlay.hpp"

#include "PresenceStore.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace collab {
namespace {

constexpr qint64 kClickAnimationMs = 420;

QColor readableTextOn(const QColor& background) {
    const qreal luminance = 0.2126 * background.redF() +
                            0.7152 * background.greenF() +
                            0.0722 * background.blueF();
    return luminance > 0.58 ? QColor(20, 22, 26) : Qt::white;
}

} // namespace

PresenceOverlay::PresenceOverlay(QWidget* surface, SurfaceAddress address,
                                 PresenceStore* store, QWidget* parent)
    : QWidget(parent ? parent : surface),
      m_surface(surface),
      m_address(std::move(address)),
      m_store(store),
      m_reduceMotion(QSettings().value(QStringLiteral("ui/reduceMotion"), false)
                         .toBool()) {
    setObjectName(QStringLiteral("CollaborationPresenceOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(tr("Collaborator pointers"));
    if (m_surface) m_surface->installEventFilter(this);
    if (m_store) {
        connect(m_store, &PresenceStore::presenceChanged, this,
                &PresenceOverlay::onPresenceChanged);
    }
    m_animationTimer.setInterval(33);
    m_animationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_animationTimer, &QTimer::timeout, this, [this] { update(); });
    syncGeometry();
    show();
    raise();
}

void PresenceOverlay::setPointMapper(PointMapper mapper) {
    m_mapper = std::move(mapper);
    update();
}

bool PresenceOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_surface && event &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
         event->type() == QEvent::LayoutRequest)) {
        syncGeometry();
    }
    return QWidget::eventFilter(watched, event);
}

std::optional<QPointF> PresenceOverlay::mapPoint(
    const SemanticPoint& point) const {
    if (m_mapper) return m_mapper(point, size());
    return surfacePointFromNormalized(point, size());
}

void PresenceOverlay::syncGeometry() {
    if (!m_surface) return;
    setGeometry(m_surface->rect());
    raise();
}

void PresenceOverlay::onPresenceChanged() {
    raise();
    update();
    if (!m_reduceMotion && !m_animationTimer.isActive())
        m_animationTimer.start();
}

void PresenceOverlay::paintEvent(QPaintEvent*) {
    if (!m_store) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto cursors = m_store->cursorsForSurface(m_address, now);
    bool animationNeeded = false;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics metrics(font());

    for (const PresenceCursorSnapshot& cursor : cursors) {
        animationNeeded = animationNeeded || cursor.interpolating;
        const auto position = mapPoint(cursor.packet.point);
        if (!position) continue;
        painter.save();
        painter.setOpacity(std::clamp(cursor.opacity, qreal(0.0), qreal(1.0)));
        const QColor color = cursor.participant.color.isValid()
                                 ? cursor.participant.color
                                 : PresenceStore::stableParticipantColor(
                                       cursor.participant.participantId);

        const QPointF p(std::clamp(position->x(), 2.0, qreal(width() - 2)),
                        std::clamp(position->y(), 2.0, qreal(height() - 2)));
        QPainterPath arrow;
        arrow.moveTo(p);
        arrow.lineTo(p + QPointF(3.5, 17.0));
        arrow.lineTo(p + QPointF(8.0, 12.5));
        arrow.lineTo(p + QPointF(13.0, 20.0));
        arrow.lineTo(p + QPointF(16.0, 18.0));
        arrow.lineTo(p + QPointF(11.0, 10.5));
        arrow.lineTo(p + QPointF(18.0, 9.0));
        arrow.closeSubpath();
        painter.setPen(QPen(QColor(0, 0, 0, 170), 3.0, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(color);
        painter.drawPath(arrow);
        painter.setPen(QPen(color.lighter(125), 1.0));
        painter.drawPath(arrow);

        const QString nickname = safeDisplayName(cursor.participant.nickname);
        const int labelWidth = metrics.horizontalAdvance(nickname) + 14;
        const int labelHeight = metrics.height() + 6;
        QPointF labelAt = p + QPointF(15.0, 15.0);
        labelAt.setX(std::clamp(labelAt.x(), 2.0,
                                std::max(2.0, qreal(width() - labelWidth - 2))));
        labelAt.setY(std::clamp(labelAt.y(), 2.0,
                                std::max(2.0, qreal(height() - labelHeight - 2))));
        const QRectF label(labelAt, QSizeF(labelWidth, labelHeight));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(label, 5.0, 5.0);
        painter.setPen(readableTextOn(color));
        painter.drawText(label.adjusted(7, 2, -7, -2),
                         Qt::AlignVCenter | Qt::AlignLeft, nickname);

        if (!cursor.packet.selectionIds.isEmpty() || cursor.packet.drag.active) {
            const QString activity = cursor.packet.drag.active
                ? tr("Dragging")
                : tr("%1 selected").arg(cursor.packet.selectionIds.size());
            const int width = metrics.horizontalAdvance(activity) + 12;
            const QRectF badge(label.left(), label.bottom() + 3, width,
                               metrics.height() + 4);
            painter.setBrush(QColor(18, 20, 24, 220));
            painter.setPen(QPen(color, 1.0));
            painter.drawRoundedRect(badge, 4.0, 4.0);
            painter.setPen(Qt::white);
            painter.drawText(badge.adjusted(6, 1, -6, -1),
                             Qt::AlignVCenter | Qt::AlignLeft, activity);
        }

        if (cursor.packet.phase == PointerPhase::Press) {
            const qint64 clickAge = now - cursor.receivedAtMs;
            if (clickAge >= 0 && clickAge <= kClickAnimationMs) {
                const qreal progress = qreal(clickAge) / kClickAnimationMs;
                const qreal radius = m_reduceMotion ? 9.0 : 5.0 + 16.0 * progress;
                QColor ring = color;
                ring.setAlphaF(m_reduceMotion ? 0.8 : 1.0 - progress);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(ring, 2.0));
                painter.drawEllipse(p, radius, radius);
                animationNeeded = animationNeeded || !m_reduceMotion;
            }
        }
        painter.restore();
    }
    if (!animationNeeded) m_animationTimer.stop();
}

} // namespace collab
