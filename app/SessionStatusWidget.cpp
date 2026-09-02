#include "SessionStatusWidget.hpp"

#include "CollaborationService.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QToolButton>

namespace collab {
namespace {

QString surfaceLabel(SessionStatusWidget* widget, SurfaceKind surface) {
    switch (surface) {
        case SurfaceKind::Timeline: return widget->tr("Timeline");
        case SurfaceKind::TrackList: return widget->tr("Track list");
        case SurfaceKind::Transport: return widget->tr("Transport");
        case SurfaceKind::Mixer: return widget->tr("Mixer");
        case SurfaceKind::PianoRoll: return widget->tr("Piano roll");
        case SurfaceKind::AutomationEditor: return widget->tr("Automation");
        case SurfaceKind::SampleEditor: return widget->tr("Sample editor");
        case SurfaceKind::BuiltinPlugin: return widget->tr("Built-in plugin");
        case SurfaceKind::Browser: return widget->tr("File browser");
        case SurfaceKind::Web: return widget->tr("Web browser");
        case SurfaceKind::Ai: return widget->tr("AI panel");
        case SurfaceKind::Settings: return widget->tr("Settings");
        case SurfaceKind::Shell: return widget->tr("Workspace");
        default: return {};
    }
}

} // namespace

SessionStatusWidget::SessionStatusWidget(CollaborationService* service,
                                         QWidget* parent)
    : QFrame(parent), m_service(service) {
    setObjectName(QStringLiteral("SessionStatusWidget"));
    setAccessibleName(tr("Collaboration session status"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 1, 4, 1);
    layout->setSpacing(5);
    m_stateLabel = new QLabel(this);
    m_stateLabel->setObjectName(QStringLiteral("SessionStateLabel"));
    m_participantsButton = new QToolButton(this);
    m_participantsButton->setObjectName(QStringLiteral("SessionParticipantsButton"));
    m_participantsButton->setPopupMode(QToolButton::InstantPopup);
    m_participantsButton->setAccessibleName(tr("Session participants"));
    m_participantsMenu = new QMenu(m_participantsButton);
    m_participantsButton->setMenu(m_participantsMenu);
    connect(m_participantsMenu, &QMenu::aboutToShow, this,
            &SessionStatusWidget::rebuildParticipants);
    layout->addWidget(m_stateLabel);
    layout->addWidget(m_participantsButton);

    if (m_service) {
        connect(m_service, &CollaborationService::stateChanged, this,
                [this] { updateState(); });
        connect(m_service->presenceStore(), &PresenceStore::participantsChanged,
                this, &SessionStatusWidget::rebuildParticipants);
    }
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &SessionStatusWidget::applyTheme);
    applyTheme();
    updateState();
    rebuildParticipants();
}

void SessionStatusWidget::updateState() {
    const CollaborationState state = m_service ? m_service->state()
                                               : CollaborationState::Unavailable;
    QString label;
    switch (state) {
        case CollaborationState::LocalOnly: label = tr("Session: Local"); break;
        case CollaborationState::Unavailable:
            label = tr("Session: Authorization needed"); break;
        case CollaborationState::SignedOut: label = tr("Session: Sign in"); break;
        case CollaborationState::NoConnection:
            label = tr("Session: No connection"); break;
        case CollaborationState::Uploading:
            label = tr("Session: Uploading"); break;
        case CollaborationState::Connecting:
            label = tr("Session: Connecting"); break;
        case CollaborationState::Joining: label = tr("Session: Joining"); break;
        case CollaborationState::Synced: label = tr("Session: Synced"); break;
        case CollaborationState::Reconnecting:
            label = tr("Session: Reconnecting"); break;
        case CollaborationState::ReadOnly:
            label = tr("Session: Read-only"); break;
        case CollaborationState::Conflict:
            label = tr("Session: Conflict"); break;
        case CollaborationState::Error: label = tr("Session: Error"); break;
    }
    m_stateLabel->setText(label);
    const QString detail = m_service ? m_service->stateDetail() : QString();
    setToolTip(detail.isEmpty() ? label : label + QStringLiteral(" — ") + detail);
    setAccessibleDescription(toolTip());
}

void SessionStatusWidget::rebuildParticipants() {
    m_participantsMenu->clear();
    const auto participants = m_service
        ? m_service->presenceStore()->participants()
        : QVector<ParticipantIdentity>{};
    m_participantsButton->setText(tr("People %1").arg(participants.size()));
    m_participantsButton->setEnabled(!participants.isEmpty());
    if (participants.isEmpty()) {
        QAction* empty = m_participantsMenu->addAction(tr("No active participants"));
        empty->setEnabled(false);
        return;
    }
    for (const ParticipantIdentity& participant : participants) {
        QString suffix;
        if (participant.host) suffix += tr(" — Host");
        if (!participant.role.isEmpty())
            suffix += tr(" (%1)").arg(participant.role);
        if (const auto surface = m_service->presenceStore()
                                     ->recentSurfaceForParticipant(
                                         participant.participantId)) {
            const QString activity = surfaceLabel(this, *surface);
            if (!activity.isEmpty()) suffix += tr(" — %1").arg(activity);
        }
        QAction* action = m_participantsMenu->addAction(
            safeDisplayName(participant.nickname) + suffix);
        action->setEnabled(false);
        const QColor color = participant.color.isValid()
            ? participant.color
            : PresenceStore::stableParticipantColor(participant.participantId);
        QPixmap swatch(12, 12);
        swatch.fill(color);
        action->setIcon(QIcon(swatch));
    }
}

void SessionStatusWidget::applyTheme() {
    const Theme& theme = th();
    setStyleSheet(QStringLiteral(
        "QFrame#SessionStatusWidget { background: %1; border: 1px solid %2; "
        "border-radius: 5px; } "
        "QLabel#SessionStateLabel { color: %3; } "
        "QToolButton#SessionParticipantsButton { color: %3; border: 0; "
        "padding: 1px 4px; } "
        "QToolButton#SessionParticipantsButton:hover { background: %4; }")
        .arg(theme.surfaceElevated.name(QColor::HexArgb),
             theme.separator().name(QColor::HexArgb),
             theme.textSecondary.name(QColor::HexArgb),
             theme.selection.name(QColor::HexArgb)));
}

} // namespace collab
