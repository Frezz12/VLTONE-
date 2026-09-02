#include "CloudProjectInviteDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

#include <functional>
#include <optional>
#include <utility>

namespace collab {
namespace {

constexpr qint64 kMinimumExpirySeconds = 3'600;
constexpr qint64 kMaximumExpirySeconds = 2'592'000;
constexpr qint64 kDefaultExpirySeconds = 604'800;
constexpr int kMaximumEmailCharacters = 320;
constexpr int kMaximumSafeMessageCharacters = 240;
constexpr int kMinimumTokenCharacters = 32;
constexpr int kMaximumTokenCharacters = 256;

QString canonicalUuid(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
        "[0-9a-f]{4}-[0-9a-f]{12}$"));
    const QString text = value.trimmed();
    const QUuid uuid(text);
    if (!pattern.match(text).hasMatch() || uuid.isNull()) return {};
    return uuid.toString(QUuid::WithoutBraces).toLower();
}

QString normalizedEmail(const QString& value) {
    return value.trimmed();
}

bool validEmail(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral("^[^@\\s]+@[^@\\s]+$"));
    const QString email = normalizedEmail(value);
    return email.size() <= kMaximumEmailCharacters &&
           (email.isEmpty() || pattern.match(email).hasMatch());
}

bool validToken(const QString& token) {
    return token.size() >= kMinimumTokenCharacters &&
           token.size() <= kMaximumTokenCharacters &&
           !token.contains(QLatin1Char('\r')) &&
           !token.contains(QLatin1Char('\n'));
}

void wipe(QString& value) {
    if (!value.isEmpty()) value.fill(QChar(0));
    value.clear();
    value.squeeze();
}

QString boundedSafeMessage(const QString& value,
                           const QString& fallback) {
    const QString bounded = value.trimmed().left(kMaximumSafeMessageCharacters);
    return bounded.isEmpty() ? fallback : bounded;
}

class InviteRequestState final {
public:
    enum class Phase : quint8 { Idle, Pending, Ready };

    struct Ports {
        std::function<quint64(const QString&,
                              const CreateCloudProjectInviteInput&)> create;
        std::function<bool(quint64)> cancel;
    };

    InviteRequestState(QString projectId, Ports ports)
        : m_projectId(canonicalUuid(projectId)), m_ports(std::move(ports)) {
        if (m_projectId.isEmpty()) {
            setError(QStringLiteral(
                "This cloud project identifier is invalid. Reopen the project."));
        }
    }

    ~InviteRequestState() { shutdown(); }

    QString projectId() const { return m_projectId; }
    Phase phase() const noexcept { return m_phase; }
    bool isPending() const noexcept { return m_phase == Phase::Pending; }
    bool hasToken() const noexcept {
        return m_phase == Phase::Ready && !m_token.isEmpty();
    }
    const QString& token() const noexcept { return m_token; }
    const QString& safeMessage() const noexcept { return m_safeMessage; }
    bool messageIsError() const noexcept { return m_messageIsError; }

    static bool validInput(const CreateCloudProjectInviteInput& input) {
        return (input.role == CloudMemberRole::Editor ||
                input.role == CloudMemberRole::Viewer) &&
               validEmail(input.targetEmail) &&
               input.expiresInSeconds >= kMinimumExpirySeconds &&
               input.expiresInSeconds <= kMaximumExpirySeconds;
    }

    bool canBegin(const CreateCloudProjectInviteInput& input) const {
        return m_phase == Phase::Idle && !m_projectId.isEmpty() &&
               bool(m_ports.create) && validInput(input);
    }

    bool begin(CreateCloudProjectInviteInput input) {
        if (!canBegin(input)) {
            if (m_projectId.isEmpty()) {
                setError(QStringLiteral(
                    "This cloud project identifier is invalid. Reopen the project."));
            } else if (!validInput(input)) {
                setError(QStringLiteral(
                    "Enter a valid optional email and choose an expiry from "
                    "1 to 720 hours."));
            } else if (!m_ports.create) {
                setError(QStringLiteral(
                    "Cloud invitations are unavailable right now."));
            }
            return false;
        }

        input.targetEmail = normalizedEmail(input.targetEmail);
        clearDeferred();
        m_requestedRole = input.role;
        m_phase = Phase::Pending;
        m_requestId = 0;
        m_issuing = true;
        setInfo(QStringLiteral("Creating invitation…"));

        const quint64 issued = m_ports.create(m_projectId, input);
        m_issuing = false;
        m_requestId = issued;

        auto created = std::move(m_deferredCreated);
        auto failed = std::move(m_deferredFailure);
        m_deferredCreated.reset();
        m_deferredFailure.reset();
        if (created && created->requestId == issued) {
            onCreated(created->requestId, created->result);
        } else if (failed && failed->requestId == issued) {
            onFailed(failed->requestId, failed->safeMessage);
        } else if (issued == 0) {
            m_phase = Phase::Idle;
            setError(QStringLiteral("The invitation request could not be started."));
        }
        // Deferred signals may belong to another request sharing this client.
        // Drop their copies too; in particular, do not retain a crossed token
        // until the next request or dialog shutdown.
        if (created) wipe(created->result.oneTimeToken);
        if (failed) wipe(failed->safeMessage);
        return m_phase != Phase::Idle || issued != 0;
    }

    bool onCreated(quint64 requestId,
                   const CreatedCloudProjectInvite& result) {
        if (m_issuing) {
            clearDeferredCreated();
            m_deferredCreated = DeferredCreated{requestId, result};
            return true;
        }
        if (m_phase != Phase::Pending || m_requestId == 0 ||
            requestId != m_requestId) {
            return false;
        }

        // CloudProjectClient already validates this response. Recheck the
        // request-bound identity and role here so a stale or crossed signal
        // cannot replace the currently awaited invitation.
        if (canonicalUuid(result.invite.projectId) != m_projectId ||
            canonicalUuid(result.invite.id).isEmpty() ||
            result.invite.role != m_requestedRole ||
            !result.invite.expiresAt.isValid() ||
            !validToken(result.oneTimeToken)) {
            return false;
        }

        m_requestId = 0;
        m_phase = Phase::Ready;
        wipe(m_token);
        m_token = result.oneTimeToken;
        setInfo(QStringLiteral(
            "Invitation created. Copy the one-time token before closing."));
        return true;
    }

    bool onFailed(quint64 requestId, const QString& safeMessage) {
        if (m_issuing) {
            m_deferredFailure = DeferredFailure{
                requestId,
                boundedSafeMessage(
                    safeMessage,
                    QStringLiteral("Could not create the invitation."))};
            return true;
        }
        if (m_phase != Phase::Pending || m_requestId == 0 ||
            requestId != m_requestId) {
            return false;
        }
        m_requestId = 0;
        m_phase = Phase::Idle;
        setError(boundedSafeMessage(
            safeMessage, QStringLiteral("Could not create the invitation.")));
        return true;
    }

    void clientUnavailable() {
        m_requestId = 0;
        m_issuing = false;
        m_phase = Phase::Idle;
        clearDeferred();
        wipe(m_token);
        m_ports = {};
        setError(QStringLiteral("Cloud invitations are unavailable right now."));
    }

    void shutdown() {
        const quint64 request = m_requestId;
        m_requestId = 0;
        m_issuing = false;
        m_phase = Phase::Idle;
        clearDeferred();
        wipe(m_token);
        wipe(m_safeMessage);
        if (request != 0 && m_ports.cancel) m_ports.cancel(request);
    }

private:
    struct DeferredCreated {
        quint64 requestId = 0;
        CreatedCloudProjectInvite result;
    };
    struct DeferredFailure {
        quint64 requestId = 0;
        QString safeMessage;
    };

    void setInfo(const QString& message) {
        m_safeMessage = message.left(kMaximumSafeMessageCharacters);
        m_messageIsError = false;
    }

    void setError(const QString& message) {
        m_safeMessage = message.left(kMaximumSafeMessageCharacters);
        m_messageIsError = true;
    }

    void clearDeferredCreated() {
        if (!m_deferredCreated) return;
        wipe(m_deferredCreated->result.oneTimeToken);
        m_deferredCreated.reset();
    }

    void clearDeferred() {
        clearDeferredCreated();
        if (m_deferredFailure) {
            wipe(m_deferredFailure->safeMessage);
            m_deferredFailure.reset();
        }
    }

    QString m_projectId;
    Ports m_ports;
    Phase m_phase = Phase::Idle;
    quint64 m_requestId = 0;
    CloudMemberRole m_requestedRole = CloudMemberRole::Viewer;
    bool m_issuing = false;
    QString m_token;
    QString m_safeMessage;
    bool m_messageIsError = false;
    std::optional<DeferredCreated> m_deferredCreated;
    std::optional<DeferredFailure> m_deferredFailure;
};

} // namespace

struct CloudProjectInviteDialog::Impl {
    CloudProjectInviteDialog* q = nullptr;
    InviteRequestState request;
    QLabel* description = nullptr;
    QComboBox* role = nullptr;
    QLineEdit* email = nullptr;
    QSpinBox* expiryHours = nullptr;
    QLabel* status = nullptr;
    QLabel* tokenLabel = nullptr;
    QLineEdit* token = nullptr;
    QPushButton* copy = nullptr;
    QPushButton* create = nullptr;
    QPushButton* cancel = nullptr;
    QPointer<CloudProjectClient> projects;
    QListWidget* invitations = nullptr;
    QPushButton* refreshInvitations = nullptr;
    QPushButton* revokeInvitation = nullptr;
    quint64 listRequestId = 0;
    quint64 revokeRequestId = 0;

    Impl(CloudProjectInviteDialog* owner, const QString& projectId,
         InviteRequestState::Ports ports)
        : q(owner), request(projectId, std::move(ports)) {}

    CreateCloudProjectInviteInput input() const {
        CreateCloudProjectInviteInput value;
        value.role = role && role->currentData().toInt() ==
                                int(CloudMemberRole::Editor)
            ? CloudMemberRole::Editor
            : CloudMemberRole::Viewer;
        value.targetEmail = email ? email->text() : QString();
        value.expiresInSeconds = expiryHours
            ? qint64(expiryHours->value()) * 3'600
            : kDefaultExpirySeconds;
        return value;
    }

    void refresh() {
        const bool pending = request.isPending();
        const bool ready = request.hasToken();
        const bool valid = request.canBegin(input());
        if (role) role->setDisabled(pending || ready);
        if (email) email->setDisabled(pending || ready);
        if (expiryHours) expiryHours->setDisabled(pending || ready);
        if (create) create->setEnabled(valid);
        if (cancel) cancel->setText(ready ? q->tr("Close") : q->tr("Cancel"));

        if (status) {
            status->setTextFormat(Qt::PlainText);
            status->setText(request.safeMessage());
            status->setStyleSheet(request.messageIsError()
                ? QStringLiteral("QLabel { color: #ff9ba0; }")
                : QStringLiteral("QLabel { color: #8ed7e8; }"));
            status->setVisible(!request.safeMessage().isEmpty());
        }
        if (tokenLabel) tokenLabel->setVisible(ready);
        if (token) {
            if (ready && token->text() != request.token())
                token->setText(request.token());
            else if (!ready && !token->text().isEmpty())
                token->clear();
            token->setVisible(ready);
        }
        if (copy) copy->setVisible(ready);
    }

    void submit() {
        if (request.begin(input())) refresh();
        else refresh();
    }

    void copyToken() {
        if (!request.hasToken()) return;
        if (QClipboard* clipboard = QApplication::clipboard())
            clipboard->setText(request.token(), QClipboard::Clipboard);
        status->setTextFormat(Qt::PlainText);
        status->setText(q->tr("One-time invitation token copied."));
        status->setStyleSheet(QStringLiteral("QLabel { color: #8ed7e8; }"));
        status->show();
    }

    void reloadInvitations() {
        if (!projects || request.projectId().isEmpty()) return;
        if (listRequestId != 0) projects->cancel(listRequestId);
        if (invitations) {
            invitations->clear();
            invitations->addItem(q->tr("Loading…"));
        }
        listRequestId = projects->listInvites(request.projectId());
    }

    void showInvitations(quint64 requestId,
                         const QVector<CloudProjectInvite>& values) {
        if (requestId != listRequestId || !invitations) return;
        listRequestId = 0;
        invitations->clear();
        for (const CloudProjectInvite& invite : values) {
            const QString role = invite.role == CloudMemberRole::Editor
                ? q->tr("Editor") : q->tr("Viewer");
            const QString expiry = invite.expiresAt.isValid()
                ? invite.expiresAt.toLocalTime().toString(
                      QStringLiteral("yyyy-MM-dd HH:mm"))
                : q->tr("unknown expiry");
            auto* item = new QListWidgetItem(
                q->tr("%1 — expires %2").arg(role, expiry), invitations);
            item->setData(Qt::UserRole, invite.id);
        }
        if (invitations->count() == 0)
            invitations->addItem(q->tr("No active invitations"));
        if (revokeInvitation) revokeInvitation->setEnabled(false);
    }

    void revokeSelectedInvitation() {
        if (!projects || !invitations || revokeRequestId != 0) return;
        QListWidgetItem* item = invitations->currentItem();
        const QString inviteId = item
            ? item->data(Qt::UserRole).toString() : QString();
        if (inviteId.isEmpty()) return;
        revokeRequestId =
            projects->revokeInvite(request.projectId(), inviteId);
        if (revokeInvitation) revokeInvitation->setEnabled(false);
    }

    void shutdown() {
        if (projects && listRequestId != 0) projects->cancel(listRequestId);
        if (projects && revokeRequestId != 0)
            projects->cancel(revokeRequestId);
        listRequestId = 0;
        revokeRequestId = 0;
        request.shutdown();
        if (token) token->clear();
        if (email) email->clear();
    }
};

CloudProjectInviteDialog::CloudProjectInviteDialog(
    CloudProjectClient* projects, const QString& canonicalProjectId,
    QWidget* parent)
    : QDialog(parent) {
    QPointer<CloudProjectClient> guard(projects);
    InviteRequestState::Ports ports;
    ports.create = [guard](const QString& projectId,
                           const CreateCloudProjectInviteInput& input) {
        return guard ? guard->createInvite(projectId, input) : quint64(0);
    };
    ports.cancel = [guard](quint64 requestId) {
        return guard && guard->cancel(requestId);
    };
    m_impl = std::make_unique<Impl>(this, canonicalProjectId, std::move(ports));
    m_impl->projects = projects;

    setWindowTitle(tr("Invite People"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    setMinimumWidth(460);

    auto* title = new QLabel(tr("Invite someone to this project"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);

    m_impl->description = new QLabel(
        tr("Choose what the invited person can do. The token is shown only "
           "once and is copied only when you press Copy."),
        this);
    m_impl->description->setTextFormat(Qt::PlainText);
    m_impl->description->setWordWrap(true);

    m_impl->role = new QComboBox(this);
    m_impl->role->addItem(tr("Editor"), int(CloudMemberRole::Editor));
    m_impl->role->addItem(tr("Viewer"), int(CloudMemberRole::Viewer));

    m_impl->email = new QLineEdit(this);
    m_impl->email->setMaxLength(kMaximumEmailCharacters);
    m_impl->email->setInputMethodHints(Qt::ImhEmailCharactersOnly);
    m_impl->email->setClearButtonEnabled(true);
    m_impl->email->setPlaceholderText(tr("Optional"));

    m_impl->expiryHours = new QSpinBox(this);
    m_impl->expiryHours->setRange(
        int(kMinimumExpirySeconds / 3'600),
        int(kMaximumExpirySeconds / 3'600));
    m_impl->expiryHours->setValue(int(kDefaultExpirySeconds / 3'600));
    m_impl->expiryHours->setSuffix(tr(" hours"));

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("Role"), m_impl->role);
    form->addRow(tr("Target email"), m_impl->email);
    form->addRow(tr("Expires after"), m_impl->expiryHours);

    m_impl->status = new QLabel(this);
    m_impl->status->setTextFormat(Qt::PlainText);
    m_impl->status->setWordWrap(true);

    m_impl->tokenLabel = new QLabel(tr("One-time invitation token"), this);
    m_impl->token = new QLineEdit(this);
    m_impl->token->setReadOnly(true);
    m_impl->token->setClearButtonEnabled(false);
    m_impl->copy = new QPushButton(tr("Copy token"), this);

    auto* tokenRow = new QHBoxLayout;
    tokenRow->addWidget(m_impl->token, 1);
    tokenRow->addWidget(m_impl->copy);

    auto* invitationsTitle =
        new QLabel(tr("Active invitations"), this);
    m_impl->invitations = new QListWidget(this);
    m_impl->invitations->setMinimumHeight(110);
    m_impl->refreshInvitations = new QPushButton(tr("Refresh"), this);
    m_impl->revokeInvitation =
        new QPushButton(tr("Revoke selected"), this);
    m_impl->revokeInvitation->setEnabled(false);
    auto* invitationButtons = new QHBoxLayout;
    invitationButtons->addWidget(m_impl->refreshInvitations);
    invitationButtons->addWidget(m_impl->revokeInvitation);
    invitationButtons->addStretch(1);

    m_impl->create = new QPushButton(tr("Create invitation"), this);
    m_impl->create->setDefault(true);
    m_impl->cancel = new QPushButton(tr("Cancel"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_impl->cancel);
    buttons->addWidget(m_impl->create);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(24, 22, 24, 22);
    column->setSpacing(14);
    column->addWidget(title);
    column->addWidget(m_impl->description);
    column->addLayout(form);
    column->addWidget(m_impl->status);
    column->addWidget(m_impl->tokenLabel);
    column->addLayout(tokenRow);
    column->addWidget(invitationsTitle);
    column->addWidget(m_impl->invitations);
    column->addLayout(invitationButtons);
    column->addLayout(buttons);

    connect(m_impl->create, &QPushButton::clicked, this,
            [this] { m_impl->submit(); });
    connect(m_impl->cancel, &QPushButton::clicked, this,
            &CloudProjectInviteDialog::reject);
    connect(m_impl->copy, &QPushButton::clicked, this,
            [this] { m_impl->copyToken(); });
    connect(m_impl->refreshInvitations, &QPushButton::clicked, this,
            [this] { m_impl->reloadInvitations(); });
    connect(m_impl->revokeInvitation, &QPushButton::clicked, this,
            [this] { m_impl->revokeSelectedInvitation(); });
    connect(m_impl->invitations, &QListWidget::itemSelectionChanged, this,
            [this] {
        const QListWidgetItem* item = m_impl->invitations->currentItem();
        m_impl->revokeInvitation->setEnabled(
            item && !item->data(Qt::UserRole).toString().isEmpty() &&
            m_impl->revokeRequestId == 0);
    });
    connect(m_impl->email, &QLineEdit::textChanged, this,
            [this] { m_impl->refresh(); });
    connect(m_impl->role, &QComboBox::currentIndexChanged, this,
            [this] { m_impl->refresh(); });
    connect(m_impl->expiryHours, &QSpinBox::valueChanged, this,
            [this] { m_impl->refresh(); });
    connect(m_impl->email, &QLineEdit::returnPressed, this, [this] {
        if (m_impl->create->isEnabled()) m_impl->submit();
    });

    if (projects) {
        connect(projects, &CloudProjectClient::inviteCreated, this,
                [this](quint64 requestId,
                       const CreatedCloudProjectInvite& result) {
                    if (m_impl->request.onCreated(requestId, result)) {
                        m_impl->refresh();
                        m_impl->reloadInvitations();
                    }
                });
        connect(projects, &CloudProjectClient::invitesListed, this,
                [this](quint64 requestId,
                       const QVector<CloudProjectInvite>& invites) {
            m_impl->showInvitations(requestId, invites);
        });
        connect(projects, &CloudProjectClient::operationCompleted, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const QString&) {
            if (requestId != m_impl->revokeRequestId ||
                kind != CloudRequestKind::RevokeInvite) return;
            m_impl->revokeRequestId = 0;
            m_impl->reloadInvitations();
        });
        connect(projects, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
                    if (kind == CloudRequestKind::ListInvites &&
                        requestId == m_impl->listRequestId) {
                        m_impl->listRequestId = 0;
                        m_impl->status->setText(boundedSafeMessage(
                            error.safeMessage,
                            tr("Could not load invitations.")));
                        return;
                    }
                    if (kind == CloudRequestKind::RevokeInvite &&
                        requestId == m_impl->revokeRequestId) {
                        m_impl->revokeRequestId = 0;
                        m_impl->status->setText(boundedSafeMessage(
                            error.safeMessage,
                            tr("Could not revoke the invitation.")));
                        m_impl->refresh();
                        return;
                    }
                    if (kind == CloudRequestKind::CreateInvite &&
                        m_impl->request.onFailed(requestId,
                                                 error.safeMessage)) {
                        m_impl->refresh();
                    }
                });
        connect(projects, &QObject::destroyed, this, [this] {
            m_impl->request.clientUnavailable();
            m_impl->refresh();
        });
    } else {
        m_impl->request.clientUnavailable();
    }
    m_impl->refresh();
    m_impl->reloadInvitations();
}

CloudProjectInviteDialog::~CloudProjectInviteDialog() {
    if (m_impl) m_impl->shutdown();
}

QString CloudProjectInviteDialog::projectId() const {
    return m_impl ? m_impl->request.projectId() : QString();
}

bool CloudProjectInviteDialog::hasPendingRequest() const noexcept {
    return m_impl && m_impl->request.isPending();
}

bool CloudProjectInviteDialog::hasOneTimeToken() const noexcept {
    return m_impl && m_impl->request.hasToken();
}

void CloudProjectInviteDialog::reject() {
    if (m_impl) m_impl->shutdown();
    QDialog::reject();
}

void CloudProjectInviteDialog::closeEvent(QCloseEvent* event) {
    if (m_impl) m_impl->shutdown();
    QDialog::closeEvent(event);
}

bool checkCloudProjectInviteDialogForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString inviteId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString token = QString(48, QLatin1Char('T'));

    int createCalls = 0;
    int cancelCalls = 0;
    QString capturedProject;
    CreateCloudProjectInviteInput capturedInput;
    InviteRequestState* synchronous = nullptr;
    InviteRequestState::Ports ports;
    ports.create = [&](const QString& requestedProject,
                       const CreateCloudProjectInviteInput& input) {
        ++createCalls;
        capturedProject = requestedProject;
        capturedInput = input;
        CreatedCloudProjectInvite created;
        created.invite.id = inviteId;
        created.invite.projectId = projectId;
        created.invite.role = CloudMemberRole::Editor;
        created.invite.expiresAt =
            QDateTime::currentDateTimeUtc().addSecs(input.expiresInSeconds);
        created.oneTimeToken = token;
        synchronous->onCreated(41, created);
        return quint64(41);
    };
    ports.cancel = [&](quint64) {
        ++cancelCalls;
        return true;
    };
    InviteRequestState state(projectId, ports);
    synchronous = &state;
    CreateCloudProjectInviteInput input;
    input.role = CloudMemberRole::Editor;
    input.targetEmail = QStringLiteral("  collaborator@example.test  ");
    input.expiresInSeconds = kDefaultExpirySeconds;
    if (!state.begin(input) || state.phase() != InviteRequestState::Phase::Ready ||
        state.token() != token || createCalls != 1 || cancelCalls != 0 ||
        capturedProject != projectId ||
        capturedInput.role != CloudMemberRole::Editor ||
        capturedInput.targetEmail !=
            QLatin1String("collaborator@example.test") ||
        capturedInput.expiresInSeconds != kDefaultExpirySeconds) {
        return fail(QStringLiteral(
            "synchronous invite creation changed the canonical request"));
    }

    CreatedCloudProjectInvite stale;
    stale.invite.id = inviteId;
    stale.invite.projectId = projectId;
    stale.invite.role = CloudMemberRole::Editor;
    stale.invite.expiresAt = QDateTime::currentDateTimeUtc().addDays(7);
    stale.oneTimeToken = QString(48, QLatin1Char('S'));
    if (state.onCreated(999, stale) || state.token() != token) {
        return fail(QStringLiteral("a stale response replaced the live token"));
    }
    state.shutdown();
    if (state.hasToken() || !state.token().isEmpty() || cancelCalls != 0) {
        return fail(QStringLiteral("the successful one-time token was retained"));
    }

    quint64 cancelledId = 0;
    InviteRequestState cancelled(
        projectId,
        {[](const QString&, const CreateCloudProjectInviteInput&) {
              return quint64(52);
          },
          [&](quint64 requestId) {
              cancelledId = requestId;
              return true;
          }});
    input.role = CloudMemberRole::Viewer;
    input.targetEmail.clear();
    input.expiresInSeconds = kMinimumExpirySeconds;
    if (!cancelled.begin(input) || !cancelled.isPending())
        return fail(QStringLiteral("pending invitation did not start"));
    cancelled.shutdown();
    if (cancelledId != 52 || cancelled.isPending() || cancelled.hasToken()) {
        return fail(QStringLiteral("pending invitation was not cancelled"));
    }
    if (cancelled.onCreated(52, stale)) {
        return fail(QStringLiteral("a cancelled response was not stale"));
    }

    quint64 crossedCancelId = 0;
    InviteRequestState mismatched(
        projectId,
        {[](const QString&, const CreateCloudProjectInviteInput&) {
              return quint64(63);
          },
          [&](quint64 requestId) {
              crossedCancelId = requestId;
              return true;
          }});
    if (!mismatched.begin(input) || !mismatched.isPending())
        return fail(QStringLiteral("mismatch invitation did not start"));
    CreatedCloudProjectInvite crossed = stale;
    crossed.invite.projectId =
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    crossed.invite.role = CloudMemberRole::Viewer;
    if (mismatched.onCreated(63, crossed) || !mismatched.isPending() ||
        mismatched.hasToken()) {
        return fail(QStringLiteral("cross-project response was accepted"));
    }
    mismatched.shutdown();
    if (crossedCancelId != 63 || mismatched.isPending() ||
        mismatched.hasToken()) {
        return fail(QStringLiteral(
            "cross-project pending request was not safely cancelled"));
    }

    InviteRequestState* synchronousFailure = nullptr;
    InviteRequestState failureState(
        projectId,
        {[&](const QString&, const CreateCloudProjectInviteInput&) {
              synchronousFailure->onFailed(74, QString(500, QLatin1Char('x')));
              return quint64(74);
          },
          {}});
    synchronousFailure = &failureState;
    if (!failureState.begin(input) ||
        failureState.phase() != InviteRequestState::Phase::Idle ||
        !failureState.messageIsError() ||
        failureState.safeMessage().size() != kMaximumSafeMessageCharacters) {
        return fail(QStringLiteral(
            "synchronous bounded failure was not isolated"));
    }

    int validationCalls = 0;
    quint64 validationCancelId = 0;
    CreateCloudProjectInviteInput maximumCaptured;
    InviteRequestState validation(
        projectId,
        {[&](const QString&, const CreateCloudProjectInviteInput& value) {
             ++validationCalls;
             maximumCaptured = value;
             return quint64(85);
         },
         [&](quint64 requestId) {
             validationCancelId = requestId;
             return true;
         }});
    CreateCloudProjectInviteInput invalidInput;
    invalidInput.role = CloudMemberRole::Viewer;
    invalidInput.targetEmail = QString(321, QLatin1Char('a'));
    invalidInput.expiresInSeconds = kDefaultExpirySeconds;
    if (validation.begin(invalidInput) || validationCalls != 0)
        return fail(QStringLiteral("overlong invite email reached the port"));
    invalidInput.targetEmail = QStringLiteral("missing-domain@");
    if (validation.begin(invalidInput) || validationCalls != 0)
        return fail(QStringLiteral("malformed invite email reached the port"));
    invalidInput.targetEmail.clear();
    invalidInput.expiresInSeconds = kMinimumExpirySeconds - 1;
    if (validation.begin(invalidInput) || validationCalls != 0)
        return fail(QStringLiteral("short invite expiry reached the port"));
    invalidInput.expiresInSeconds = kMaximumExpirySeconds + 1;
    if (validation.begin(invalidInput) || validationCalls != 0)
        return fail(QStringLiteral("long invite expiry reached the port"));
    invalidInput.targetEmail = QStringLiteral("max@example.test");
    invalidInput.expiresInSeconds = kMaximumExpirySeconds;
    if (!validation.begin(invalidInput) || validationCalls != 1 ||
        maximumCaptured.role != CloudMemberRole::Viewer ||
        maximumCaptured.targetEmail != QLatin1String("max@example.test") ||
        maximumCaptured.expiresInSeconds != kMaximumExpirySeconds ||
        !validation.isPending()) {
        return fail(QStringLiteral("maximum bounded invite was rejected"));
    }
    validation.shutdown();
    if (validationCancelId != 85)
        return fail(QStringLiteral("maximum bounded invite was not cancelled"));

    InviteRequestState invalid(QStringLiteral("NOT-A-UUID"), ports);
    if (!invalid.projectId().isEmpty() || invalid.canBegin(input) ||
        invalid.begin(input)) {
        return fail(QStringLiteral("invalid project UUID was accepted"));
    }
    if (error) error->clear();
    return true;
}

} // namespace collab
