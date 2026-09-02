#pragma once

#include <QFrame>

class QLabel;
class QMenu;
class QToolButton;

namespace collab {

class CollaborationService;

/// Compact status-bar entry plus participant popover. Text always accompanies
/// colour so connection and identity remain understandable without hue.
class SessionStatusWidget final : public QFrame {
    Q_OBJECT
public:
    explicit SessionStatusWidget(CollaborationService* service,
                                 QWidget* parent = nullptr);

private:
    void updateState();
    void rebuildParticipants();
    void applyTheme();

    CollaborationService* m_service = nullptr;
    QLabel* m_stateLabel = nullptr;
    QToolButton* m_participantsButton = nullptr;
    QMenu* m_participantsMenu = nullptr;
};

} // namespace collab

