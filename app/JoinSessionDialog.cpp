#include "JoinSessionDialog.hpp"

#include "CollaborationDialogStyle.hpp"
#include "CollaborationService.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "EngineController.hpp"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace collab {
namespace {

using collab::dialog::wipe;

/// Loose bounds rather than a pinned width; see JoinFlowState::validCode.
constexpr int kMinimumCodeDigits = 6;
constexpr int kMaximumCodeDigits = 32;

QString stepName(JoinStep step) {
    switch (step) {
        case JoinStep::ResolveCode:
            return JoinFlowState::tr("Checking the code");
        case JoinStep::VerifyMembership:
            return JoinFlowState::tr("Opening the project");
        case JoinStep::Compatibility:
            return JoinFlowState::tr("Checking compatibility");
        case JoinStep::DownloadSnapshot:
            return JoinFlowState::tr("Downloading the project");
        case JoinStep::HydrateAssets:
            return JoinFlowState::tr("Downloading audio");
        case JoinStep::NegotiatePlugins:
            return JoinFlowState::tr("Matching plugins");
        case JoinStep::Connect:
            return JoinFlowState::tr("Connecting");
    }
    return {};
}

} // namespace

struct JoinFlowState::Impl {
    Ports ports;

    std::array<JoinStepState, kJoinStepCount> states{};
    std::array<QString, kJoinStepCount> details{};
    JoinStep current = JoinStep::ResolveCode;

    QString projectId;
    QString sessionId;
    QString password;
    std::vector<daw::collab::PluginRequirement> pluginRequirements;
    qint64 pluginRequirementsRevision = 0;
    int commandSchemaVersion = daw::collab::kProjectCommandSchemaVersion;
    bool passwordRequired = false;
    bool membershipGranted = false;
    bool done = false;
    bool ok = false;

    quint64 requestId = 0;
    /// Set while a port is being called, so a fake port that answers
    /// synchronously is not mistaken for a stale reply.
    bool issuing = false;

    QString safeMessage;
    bool messageIsError = false;

    Impl(Ports value) : ports(std::move(value)) { reset(); }

    void reset() {
        states.fill(JoinStepState::Pending);
        for (QString& detail : details) detail.clear();
        current = JoinStep::ResolveCode;
    }

    static int index(JoinStep step) { return int(step); }

    void setState(JoinStep step, JoinStepState state) {
        states[index(step)] = state;
    }

    void enter(JoinStep step) {
        current = step;
        setState(step, JoinStepState::Running);
    }

    void complete(JoinStep step) { setState(step, JoinStepState::Done); }

    void setInfo(const QString& text) {
        safeMessage = text;
        messageIsError = false;
    }

    void setError(const QString& text) {
        safeMessage = text;
        messageIsError = true;
    }

    /// A failed step is the only one marked Failed. Everything after it stays
    /// Pending: those steps were never attempted, and painting them red would
    /// tell the user that six things broke when one did.
    void fail(JoinStep step, const QString& message) {
        setState(step, JoinStepState::Failed);
        done = true;
        ok = false;
        requestId = 0;
        setError(message);
    }

    bool issue(JoinStep step, const std::function<quint64()>& port) {
        enter(step);
        issuing = true;
        const quint64 issued = port();
        issuing = false;
        requestId = issued;
        if (issued == 0 && !done) {
            fail(step, JoinFlowState::tr("The request could not be started."));
            return false;
        }
        return !done;
    }

    /// Stale replies are the normal case here: a cancelled request, a second
    /// dialog sharing the client, a response arriving after a failure.
    bool accepts(quint64 incoming, JoinStep step) const {
        return !done && !issuing && requestId != 0 && incoming == requestId &&
               states[index(step)] == JoinStepState::Running;
    }

    void advanceToConnect() {
        if (sessionId.isEmpty()) {
            fail(JoinStep::VerifyMembership,
                 JoinFlowState::tr("The live session is no longer available."));
            return;
        }
        enter(JoinStep::NegotiatePlugins);
        daw::collab::PluginReadinessReport readiness;
        if (ports.inspectPlugins) {
            readiness = ports.inspectPlugins(pluginRequirements,
                                             pluginRequirementsRevision);
        } else if (!pluginRequirements.empty()) {
            fail(JoinStep::NegotiatePlugins,
                 JoinFlowState::tr("Plugin compatibility cannot be checked."));
            return;
        } else {
            readiness.revision = std::max<qint64>(1,
                                                  pluginRequirementsRevision);
        }
        complete(JoinStep::NegotiatePlugins);
        const auto blocked = std::ranges::count_if(
            readiness.plugins, [](const auto& plugin) {
                return plugin.status !=
                       daw::collab::PluginReadinessStatus::Ready;
            });
        details[index(JoinStep::NegotiatePlugins)] = blocked == 0
            ? JoinFlowState::tr("Ready")
            : JoinFlowState::tr("%1 unavailable; joining as viewer")
                  .arg(blocked);
        if (!ports.join) {
            fail(JoinStep::Connect,
                 JoinFlowState::tr("Joining is unavailable right now."));
            return;
        }
        const QString project = projectId;
        const QString session = sessionId;
        const QString secret = password;
        issue(JoinStep::Connect,
              [&] {
                  return ports.join(project, session, secret, readiness,
                                    commandSchemaVersion);
              });
    }
};

JoinFlowState::JoinFlowState(Ports ports)
    : m_impl(std::make_unique<Impl>(std::move(ports))) {}

JoinFlowState::~JoinFlowState() { shutdown(); }

QString JoinFlowState::normalizeCode(const QString& typed) {
    QString digits;
    digits.reserve(typed.size());
    for (const QChar character : typed) {
        if (character.unicode() >= u'0' && character.unicode() <= u'9')
            digits.append(character);
    }
    return digits;
}

bool JoinFlowState::validCode(const QString& normalized) {
    if (normalized.size() < kMinimumCodeDigits ||
        normalized.size() > kMaximumCodeDigits) {
        return false;
    }
    for (const QChar character : normalized) {
        if (character.unicode() < u'0' || character.unicode() > u'9')
            return false;
    }
    return true;
}

bool JoinFlowState::validPassword(const QString& value) {
    if (value.isEmpty()) return true;
    return value.size() >= 6 && value.size() <= 128 &&
           !value.contains(QLatin1Char('\r')) &&
           !value.contains(QLatin1Char('\n'));
}

JoinStepState JoinFlowState::stepState(JoinStep step) const noexcept {
    return m_impl->states[Impl::index(step)];
}

QString JoinFlowState::stepDetail(JoinStep step) const {
    return m_impl->details[Impl::index(step)];
}

JoinStep JoinFlowState::currentStep() const noexcept { return m_impl->current; }
bool JoinFlowState::running() const noexcept {
    return !m_impl->done && m_impl->requestId != 0;
}
bool JoinFlowState::finished() const noexcept { return m_impl->done; }
bool JoinFlowState::succeeded() const noexcept {
    return m_impl->done && m_impl->ok;
}
QString JoinFlowState::projectId() const { return m_impl->projectId; }
QString JoinFlowState::sessionId() const { return m_impl->sessionId; }
bool JoinFlowState::passwordRequired() const noexcept {
    return m_impl->passwordRequired;
}
const QString& JoinFlowState::safeMessage() const noexcept {
    return m_impl->safeMessage;
}
bool JoinFlowState::messageIsError() const noexcept {
    return m_impl->messageIsError;
}

bool JoinFlowState::begin(const QString& code, const QString& password) {
    if (running()) return false;
    if (!validPassword(password)) {
        m_impl->setError(
            JoinFlowState::tr("Use at least 6 characters, or leave the "
                                  "password empty."));
        return false;
    }

    wipe(m_impl->password);
    m_impl->password = password;
    m_impl->done = false;
    m_impl->ok = false;

    // Resuming after a password prompt: membership is already granted and the
    // project is already known, so redeeming the code again would only burn a
    // single-use invitation.
    if (m_impl->membershipGranted && !m_impl->projectId.isEmpty()) {
        m_impl->setInfo(JoinFlowState::tr("Joining the session…"));
        m_impl->advanceToConnect();
        return !m_impl->done || m_impl->ok;
    }

    const QString normalized = normalizeCode(code);
    if (!validCode(normalized)) {
        m_impl->setError(
            JoinFlowState::tr("Enter the invitation code exactly as you "
                                  "received it."));
        return false;
    }
    if (!m_impl->ports.acceptCode) {
        m_impl->setError(
            JoinFlowState::tr("Joining is unavailable right now."));
        return false;
    }
    m_impl->reset();
    m_impl->setInfo(JoinFlowState::tr("Checking the invitation code…"));
    return m_impl->issue(JoinStep::ResolveCode,
                         [&] { return m_impl->ports.acceptCode(normalized); });
}

void JoinFlowState::onCodeAccepted(quint64 requestId,
                                   const CloudProjectView& project) {
    if (!m_impl->accepts(requestId, JoinStep::ResolveCode)) return;
    const QString canonical = collab::dialog::canonicalUuid(project.project.id);
    if (canonical.isEmpty()) {
        m_impl->fail(JoinStep::ResolveCode,
                     JoinFlowState::tr("The invitation is not valid."));
        return;
    }
    m_impl->complete(JoinStep::ResolveCode);
    m_impl->membershipGranted = true;
    m_impl->projectId = canonical;
    m_impl->details[Impl::index(JoinStep::ResolveCode)] =
        collab::dialog::boundedSafeMessage(project.project.title, {});

    if (!m_impl->ports.fetchProject) {
        m_impl->fail(JoinStep::VerifyMembership,
                     JoinFlowState::tr("Joining is unavailable right now."));
        return;
    }
    m_impl->setInfo(JoinFlowState::tr("Opening the project…"));
    m_impl->issue(JoinStep::VerifyMembership,
                  [&] { return m_impl->ports.fetchProject(canonical); });
}

void JoinFlowState::onProjectReceived(quint64 requestId,
                                      const CloudProjectView& project) {
    if (!m_impl->accepts(requestId, JoinStep::VerifyMembership)) return;
    if (collab::dialog::canonicalUuid(project.project.id) != m_impl->projectId) {
        return;  // a crossed response for another project
    }
    if (project.project.status == CloudProjectStatus::Archived) {
        m_impl->fail(JoinStep::VerifyMembership,
                     JoinFlowState::tr("This project has been archived."));
        return;
    }
    m_impl->complete(JoinStep::VerifyMembership);
    m_impl->details[Impl::index(JoinStep::VerifyMembership)] =
        project.role == CloudProjectRole::Viewer
            ? JoinFlowState::tr("You can watch")
            : JoinFlowState::tr("You can edit");

    // Compatibility is enforced by the server on join, and the versions are
    // already known locally, so this step is a statement rather than a request.
    m_impl->enter(JoinStep::Compatibility);
    m_impl->complete(JoinStep::Compatibility);

    if (!m_impl->ports.beginSync) {
        m_impl->fail(JoinStep::DownloadSnapshot,
                     JoinFlowState::tr("Joining is unavailable right now."));
        return;
    }
    m_impl->enter(JoinStep::DownloadSnapshot);
    m_impl->setInfo(JoinFlowState::tr("Downloading the project…"));
    if (!m_impl->ports.beginSync(m_impl->projectId)) {
        m_impl->fail(JoinStep::DownloadSnapshot,
                     JoinFlowState::tr("The project could not be opened."));
    }
}

void JoinFlowState::onSyncPhase(CloudSyncPhase phase) {
    if (m_impl->done) return;
    if (m_impl->states[Impl::index(JoinStep::DownloadSnapshot)] !=
        JoinStepState::Running) {
        return;
    }
    switch (phase) {
        case CloudSyncPhase::FetchingBootstrap:
            m_impl->details[Impl::index(JoinStep::DownloadSnapshot)] =
                JoinFlowState::tr("Fetching");
            break;
        case CloudSyncPhase::DownloadingSnapshot:
            m_impl->details[Impl::index(JoinStep::DownloadSnapshot)] =
                JoinFlowState::tr("Snapshot");
            break;
        case CloudSyncPhase::ReplayingOperations:
        case CloudSyncPhase::ReconcilingPending:
            m_impl->details[Impl::index(JoinStep::DownloadSnapshot)] =
                JoinFlowState::tr("Replaying edits");
            break;
        case CloudSyncPhase::Failed:
            m_impl->fail(
                JoinStep::DownloadSnapshot,
                JoinFlowState::tr("The project could not be downloaded."));
            break;
        case CloudSyncPhase::CheckingLiveSession:
        case CloudSyncPhase::Ready:
            m_impl->complete(JoinStep::DownloadSnapshot);
            m_impl->details[Impl::index(JoinStep::DownloadSnapshot)].clear();
            m_impl->enter(JoinStep::HydrateAssets);
            m_impl->setInfo(JoinFlowState::tr("Downloading audio…"));
            break;
        case CloudSyncPhase::Idle:
            break;
    }
}

void JoinFlowState::onHydrationProgress(qsizetype done, qsizetype total) {
    if (m_impl->done) return;
    if (m_impl->states[Impl::index(JoinStep::HydrateAssets)] !=
        JoinStepState::Running) {
        return;
    }
    m_impl->details[Impl::index(JoinStep::HydrateAssets)] =
        total > 0 ? JoinFlowState::tr("%1 of %2 files").arg(done).arg(total)
                  : QString();
}

void JoinFlowState::onHydrationSettled(bool degraded) {
    if (m_impl->done) return;
    if (m_impl->states[Impl::index(JoinStep::HydrateAssets)] !=
        JoinStepState::Running) {
        return;
    }
    m_impl->complete(JoinStep::HydrateAssets);
    // Missing audio is not a reason to refuse the session: the arrangement,
    // the edits and everyone else are still there, and the files may arrive
    // later. Say so rather than blocking.
    m_impl->details[Impl::index(JoinStep::HydrateAssets)] =
        degraded ? JoinFlowState::tr("Some files are still missing")
                 : QString();
    m_impl->advanceToConnect();
}

void JoinFlowState::onActiveSession(const CloudSessionState& state) {
    if (m_impl->done || state.session.projectId != m_impl->projectId ||
        state.session.status == CloudSessionStatus::Ended) {
        return;
    }
    m_impl->sessionId = collab::dialog::canonicalUuid(state.session.id);
    m_impl->pluginRequirements = state.session.pluginRequirements;
    m_impl->pluginRequirementsRevision =
        state.session.pluginRequirementsRevision;
    m_impl->commandSchemaVersion = state.session.commandSchemaVersion;
    if (m_impl->ports.selectProtocol &&
        !m_impl->ports.selectProtocol(state.session.commandSchemaVersion)) {
        m_impl->fail(JoinStep::Compatibility,
                     JoinFlowState::tr("This session uses an unsupported collaboration protocol."));
    }
}

void JoinFlowState::onSessionState(quint64 requestId,
                                   const CloudSessionState& state) {
    if (!m_impl->accepts(requestId, JoinStep::Connect)) return;
    if (m_impl->ports.selectProtocol &&
        !m_impl->ports.selectProtocol(state.session.commandSchemaVersion)) {
        m_impl->fail(JoinStep::Connect,
                     JoinFlowState::tr("This session uses an unsupported collaboration protocol."));
        return;
    }
    m_impl->complete(JoinStep::Connect);
    m_impl->sessionId = collab::dialog::canonicalUuid(state.session.id);
    m_impl->done = true;
    m_impl->ok = true;
    m_impl->requestId = 0;
    wipe(m_impl->password);
    m_impl->setInfo(JoinFlowState::tr("You are in the session."));
}

void JoinFlowState::onFailed(quint64 requestId, const CloudClientError& error) {
    if (m_impl->done || m_impl->issuing) return;
    if (m_impl->requestId == 0 || requestId != m_impl->requestId) return;

    // A protected session is not a failure: it is a request for one more piece
    // of information, so the flow parks instead of collapsing.
    if (error.httpStatus == 403 &&
        (error.apiCode == QLatin1String("session_password_required") ||
         error.apiCode == QLatin1String("session_password_invalid"))) {
        const bool wrong = error.apiCode == QLatin1String("session_password_invalid");
        requirePassword();
        m_impl->setError(
            wrong ? JoinFlowState::tr("That password is not correct.")
                  : JoinFlowState::tr(
                        "This session is protected. Enter its password."));
        return;
    }
    m_impl->fail(m_impl->current,
                 collab::dialog::boundedSafeMessage(
                     error.safeMessage,
                     JoinFlowState::tr("Could not join the session.")));
}

void JoinFlowState::requirePassword() {
    m_impl->passwordRequired = true;
    m_impl->requestId = 0;
    m_impl->done = false;
    m_impl->ok = false;
    // Park on Connect rather than failing: everything before it succeeded and
    // must not be redone.
    m_impl->setState(JoinStep::Connect, JoinStepState::Pending);
    m_impl->current = JoinStep::Connect;
    wipe(m_impl->password);
}

void JoinFlowState::shutdown() {
    const quint64 request = m_impl->requestId;
    m_impl->requestId = 0;
    m_impl->issuing = false;
    m_impl->done = true;
    wipe(m_impl->password);
    wipe(m_impl->safeMessage);
    if (request != 0 && m_impl->ports.cancel) m_impl->ports.cancel(request);
}

// ── Dialog ─────────────────────────────────────────────────────────────────

namespace {

/// One checklist row: a mark, the step name, and a short detail.
struct StepRow {
    QLabel* mark = nullptr;
    QLabel* name = nullptr;
    QLabel* detail = nullptr;
};

} // namespace

struct JoinSessionDialog::Impl {
    JoinSessionDialog* q = nullptr;
    std::unique_ptr<JoinFlowState> flow;

    QLineEdit* code = nullptr;
    QLabel* passwordLabel = nullptr;
    QLineEdit* password = nullptr;
    QLabel* status = nullptr;
    QPushButton* join = nullptr;
    QPushButton* cancel = nullptr;
    std::array<StepRow, kJoinStepCount> rows{};

    QPointer<CloudProjectClient> projects;
    QString joinedProjectId;

    void refresh() {
        const bool running = flow->running();
        code->setEnabled(!running && !flow->succeeded());
        const bool wantsPassword = flow->passwordRequired();
        passwordLabel->setVisible(wantsPassword);
        password->setVisible(wantsPassword);
        password->setEnabled(wantsPassword && !running);

        join->setEnabled(!running && !flow->succeeded());
        join->setText(flow->succeeded() ? JoinFlowState::tr("Done")
                                        : JoinFlowState::tr("Join"));
        cancel->setText(flow->succeeded() ? JoinFlowState::tr("Close")
                                          : JoinFlowState::tr("Cancel"));

        status->setText(flow->safeMessage());
        status->setObjectName(flow->messageIsError()
                                  ? QStringLiteral("CollabError")
                                  : QStringLiteral("CollabSecondary"));
        status->setVisible(!flow->safeMessage().isEmpty());
        // Re-polish so the object-name swap actually repaints.
        status->style()->unpolish(status);
        status->style()->polish(status);

        for (int index = 0; index < kJoinStepCount; ++index) {
            const auto step = JoinStep(index);
            const JoinStepState state = flow->stepState(step);
            StepRow& row = rows[index];
            row.detail->setText(flow->stepDetail(step));

            QColor tint = th().textSecondary;
            icons::Glyph glyph = icons::Glyph::Spinner;
            switch (state) {
                case JoinStepState::Done:
                    glyph = icons::Glyph::Check;
                    tint = th().accent;
                    break;
                case JoinStepState::Failed:
                    glyph = icons::Glyph::Warning;
                    tint = Theme::record();
                    break;
                case JoinStepState::Skipped:
                    glyph = icons::Glyph::Minus;
                    break;
                case JoinStepState::Running:
                case JoinStepState::Pending:
                    break;
            }
            // Pending is drawn dimmer than running so the eye lands on the step
            // that is actually happening.
            const bool waiting = state == JoinStepState::Pending;
            row.mark->setPixmap(
                icons::icon(glyph, waiting ? th().separator() : tint, 14)
                    .pixmap(14, 14));
            row.name->setEnabled(!waiting);
            row.detail->setEnabled(!waiting);
        }
    }

    void submit() {
        flow->begin(code->text(), password->text());
        // The widget's copy of the secret is not needed once the flow has it.
        password->clear();
        refresh();
    }

    void finish() {
        joinedProjectId = flow->projectId();
        refresh();
    }
};

JoinSessionDialog::JoinSessionDialog(CloudProjectClient* projects,
                                     CloudProjectSyncCoordinator* sync,
                                     CloudProjectAssetHydrator* hydrator,
                                     CollaborationService* service,
                                     daw::EngineController* controller,
                                     QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->q = this;
    m_impl->projects = projects;

    const QPointer<CloudProjectClient> projectGuard(projects);
    const QPointer<CloudProjectSyncCoordinator> syncGuard(sync);
    JoinFlowState::Ports ports;
    ports.acceptCode = [projectGuard](const QString& value) {
        return projectGuard ? projectGuard->acceptInviteCode(value) : 0;
    };
    ports.fetchProject = [projectGuard](const QString& projectId) {
        return projectGuard ? projectGuard->getProject(projectId) : 0;
    };
    ports.beginSync = [syncGuard](const QString& projectId) {
        // connectIfLive is left on: joining a live session is the whole point,
        // and the REST join below is what actually admits this participant.
        return syncGuard && syncGuard->synchronize(projectId) != 0;
    };
    ports.join = [projectGuard](const QString& projectId,
                                const QString& sessionId,
                                const QString& password,
                                const daw::collab::PluginReadinessReport& readiness,
                                int commandSchemaVersion) {
        return projectGuard ? projectGuard->joinSession(projectId, sessionId,
                                                        password, readiness,
                                                        commandSchemaVersion)
                            : 0;
    };
    ports.inspectPlugins = [controller](const auto& requirements,
                                        qint64 revision) {
        if (!controller) {
            daw::collab::PluginReadinessReport report;
            report.revision = revision;
            return report;
        }
        return daw::collab::evaluatePluginReadiness(
            requirements, controller->pluginManager(), revision);
    };
    const QPointer<CollaborationService> serviceGuard(service);
    ports.selectProtocol = [serviceGuard](int version) {
        return serviceGuard && serviceGuard->setCommandSchemaVersion(version);
    };
    ports.cancel = [projectGuard](quint64 requestId) {
        return projectGuard && projectGuard->cancel(requestId);
    };
    m_impl->flow = std::make_unique<JoinFlowState>(std::move(ports));

    setWindowTitle(JoinFlowState::tr("Join a Session"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    setMinimumWidth(collab::dialog::kMinimumWidth);

    QLabel* title =
        collab::dialog::titleLabel(JoinFlowState::tr("Join a session"), this);
    auto* description = new QLabel(
        JoinFlowState::tr(
            "Enter the invitation code you were given, or open the invitation "
            "link. The project, shared assets, and exact plugin versions are "
            "checked before editing is enabled."),
        this);
    description->setObjectName(QStringLiteral("CollabSecondary"));
    description->setWordWrap(true);

    m_impl->code = new QLineEdit(this);
    m_impl->code->setPlaceholderText(JoinFlowState::tr("1234 5678 9012"));
    m_impl->code->setMaxLength(40);
    m_impl->code->setAlignment(Qt::AlignCenter);

    m_impl->passwordLabel = new QLabel(JoinFlowState::tr("Session password"), this);
    m_impl->password = new QLineEdit(this);
    m_impl->password->setEchoMode(QLineEdit::Password);
    m_impl->password->setMaxLength(128);
    m_impl->passwordLabel->hide();
    m_impl->password->hide();

    auto* checklist = new QVBoxLayout;
    checklist->setSpacing(4);
    for (int index = 0; index < kJoinStepCount; ++index) {
        StepRow& row = m_impl->rows[index];
        row.mark = new QLabel(this);
        row.mark->setFixedSize(14, 14);
        row.name = new QLabel(stepName(JoinStep(index)), this);
        row.detail = new QLabel(this);
        row.detail->setObjectName(QStringLiteral("CollabSecondary"));
        auto* line = new QHBoxLayout;
        line->setSpacing(8);
        line->addWidget(row.mark);
        line->addWidget(row.name);
        line->addStretch(1);
        line->addWidget(row.detail);
        checklist->addLayout(line);
    }

    m_impl->status = new QLabel(this);
    m_impl->status->setTextFormat(Qt::PlainText);
    m_impl->status->setWordWrap(true);

    m_impl->join = new QPushButton(JoinFlowState::tr("Join"), this);
    m_impl->join->setDefault(true);
    m_impl->cancel = new QPushButton(JoinFlowState::tr("Cancel"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_impl->cancel);
    buttons->addWidget(m_impl->join);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(collab::dialog::kMargins);
    column->setSpacing(collab::dialog::kSpacing);
    column->addWidget(title);
    column->addWidget(description);
    column->addWidget(m_impl->code);
    column->addWidget(m_impl->passwordLabel);
    column->addWidget(m_impl->password);
    column->addLayout(checklist);
    column->addWidget(m_impl->status);
    column->addLayout(buttons);

    // Regroup as the user types. Twelve digits in a row is hard to check
    // against a code read out loud, and a paste of any spelling normalises to
    // the same thing.
    connect(m_impl->code, &QLineEdit::textEdited, this,
            [this](const QString& typed) {
                const QString digits = JoinFlowState::normalizeCode(typed);
                QString grouped;
                grouped.reserve(digits.size() + digits.size() / 4);
                for (qsizetype index = 0; index < digits.size(); ++index) {
                    if (index > 0 && index % 4 == 0)
                        grouped.append(QLatin1Char(' '));
                    grouped.append(digits.at(index));
                }
                if (grouped == typed) return;
                // Keep the caret where the user is working rather than
                // snapping it to the end on every keystroke.
                const int caret = m_impl->code->cursorPosition();
                const int digitsBefore =
                    JoinFlowState::normalizeCode(typed.left(caret)).size();
                QSignalBlocker blocker(m_impl->code);
                m_impl->code->setText(grouped);
                m_impl->code->setCursorPosition(
                    int(digitsBefore + (digitsBefore > 0
                                            ? (digitsBefore - 1) / 4
                                            : 0)));
            });
    connect(m_impl->join, &QPushButton::clicked, this,
            [this] { m_impl->submit(); });
    connect(m_impl->code, &QLineEdit::returnPressed, this,
            [this] { m_impl->submit(); });
    connect(m_impl->cancel, &QPushButton::clicked, this,
            &JoinSessionDialog::reject);

    if (projects) {
        connect(projects, &CloudProjectClient::inviteAccepted, this,
                [this](quint64 requestId, const CloudProjectView& project) {
                    m_impl->flow->onCodeAccepted(requestId, project);
                    m_impl->refresh();
                });
        connect(projects, &CloudProjectClient::projectReceived, this,
                [this](quint64 requestId, CloudRequestKind,
                       const CloudProjectView& project) {
                    m_impl->flow->onProjectReceived(requestId, project);
                    m_impl->refresh();
                });
        connect(projects, &CloudProjectClient::sessionStateReceived, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudSessionState& state) {
                    if (kind == CloudRequestKind::GetActiveSession) {
                        m_impl->flow->onActiveSession(state);
                        return;
                    }
                    if (kind != CloudRequestKind::JoinSession) return;
                    m_impl->flow->onSessionState(requestId, state);
                    m_impl->finish();
                });
        connect(projects, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind,
                       const CloudClientError& error) {
                    m_impl->flow->onFailed(requestId, error);
                    m_impl->refresh();
                });
    }
    if (sync) {
        connect(sync, &CloudProjectSyncCoordinator::phaseChanged, this,
                [this](CloudSyncPhase phase) {
                    m_impl->flow->onSyncPhase(phase);
                    m_impl->refresh();
                });
    }
    if (hydrator) {
        connect(hydrator, &CloudProjectAssetHydrator::progressChanged, this,
                [this](qsizetype done, qsizetype total) {
                    m_impl->flow->onHydrationProgress(done, total);
                    m_impl->refresh();
                });
        connect(hydrator, &CloudProjectAssetHydrator::stateChanged, this,
                [this](CloudHydrationState state) {
                    if (state != CloudHydrationState::Ready &&
                        state != CloudHydrationState::Degraded) {
                        return;
                    }
                    m_impl->flow->onHydrationSettled(
                        state == CloudHydrationState::Degraded);
                    m_impl->refresh();
                });
    }

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &JoinSessionDialog::applyTheme);
    applyTheme();
    m_impl->refresh();
}

JoinSessionDialog::~JoinSessionDialog() = default;

void JoinSessionDialog::presetCode(const QString& code) {
    const QString normalized = JoinFlowState::normalizeCode(code);
    if (!JoinFlowState::validCode(normalized)) return;
    m_impl->code->setText(normalized);
    // Focus the button, never press it. A link from a web page must not be
    // able to join a room on the user's behalf.
    m_impl->join->setFocus();
}

QString JoinSessionDialog::joinedProjectId() const {
    return m_impl->flow->succeeded() ? m_impl->joinedProjectId : QString();
}

void JoinSessionDialog::reject() {
    m_impl->flow->shutdown();
    m_impl->password->clear();
    QDialog::reject();
}

void JoinSessionDialog::closeEvent(QCloseEvent* event) {
    m_impl->flow->shutdown();
    m_impl->password->clear();
    QDialog::closeEvent(event);
}

void JoinSessionDialog::applyTheme() {
    setStyleSheet(collab::dialog::styleSheet());
    m_impl->refresh();
}

bool checkJoinSessionDialogForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString sessionId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");

    const auto projectView = [&](CloudProjectStatus status) {
        CloudProjectView view;
        view.project.id = projectId;
        view.project.title = QStringLiteral("Session project");
        view.project.status = status;
        view.role = CloudProjectRole::Editor;
        return view;
    };
    CloudSessionState joined;
    joined.session.id = sessionId;
    joined.session.projectId = projectId;

    struct Recorder {
        quint64 next = 0;
        QString lastCode;
        QString lastPassword;
        int codeCalls = 0;
        int joinCalls = 0;
        int syncCalls = 0;
        int cancels = 0;
    };

    const auto makePorts = [](Recorder& log) {
        JoinFlowState::Ports ports;
        ports.acceptCode = [&log](const QString& code) {
            ++log.codeCalls;
            log.lastCode = code;
            return ++log.next;
        };
        ports.fetchProject = [&log](const QString&) { return ++log.next; };
        ports.beginSync = [&log](const QString&) {
            ++log.syncCalls;
            return true;
        };
        ports.join = [&log](const QString&, const QString&,
                            const QString& password,
                            const daw::collab::PluginReadinessReport&, int) {
            ++log.joinCalls;
            log.lastPassword = password;
            return ++log.next;
        };
        ports.cancel = [&log](quint64) {
            ++log.cancels;
            return true;
        };
        ports.inspectPlugins = [](const auto&, qint64 revision) {
            daw::collab::PluginReadinessReport report;
            report.revision = std::max<qint64>(1, revision);
            return report;
        };
        return ports;
    };

    // Happy path: every step runs in order and separators are normalised away.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        if (!flow.begin(QStringLiteral("1234-5678 9012"), {}) ||
            log.lastCode != QStringLiteral("123456789012")) {
            return fail(QStringLiteral("the code was not normalised"));
        }
        if (flow.stepState(JoinStep::ResolveCode) != JoinStepState::Running) {
            return fail(QStringLiteral("the first step did not start"));
        }
        flow.onCodeAccepted(log.next, projectView(CloudProjectStatus::Active));
        flow.onProjectReceived(log.next, projectView(CloudProjectStatus::Active));
        if (flow.stepState(JoinStep::Compatibility) != JoinStepState::Done ||
            log.syncCalls != 1) {
            return fail(QStringLiteral("the flow did not reach downloading"));
        }
        flow.onSyncPhase(CloudSyncPhase::DownloadingSnapshot);
        flow.onSyncPhase(CloudSyncPhase::Ready);
        flow.onHydrationProgress(4, 12);
        if (!flow.stepDetail(JoinStep::HydrateAssets)
                 .contains(QStringLiteral("4"))) {
            return fail(QStringLiteral("hydration detail was not reported"));
        }
        flow.onActiveSession(joined);
        flow.onHydrationSettled(false);
        if (log.joinCalls != 1 ||
            flow.stepState(JoinStep::NegotiatePlugins) !=
                JoinStepState::Done) {
            return fail(QStringLiteral("plugin negotiation did not run"));
        }
        flow.onSessionState(log.next, joined);
        if (!flow.succeeded() || flow.sessionId() != sessionId ||
            flow.projectId() != projectId) {
            return fail(QStringLiteral("a successful join did not settle"));
        }
    }

    // A protected session parks on Connect and resumes without spending the
    // single-use invitation a second time.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        flow.begin(QStringLiteral("123456789012"), {});
        flow.onCodeAccepted(log.next, projectView(CloudProjectStatus::Active));
        flow.onProjectReceived(log.next, projectView(CloudProjectStatus::Active));
        flow.onSyncPhase(CloudSyncPhase::Ready);
        flow.onActiveSession(joined);
        flow.onHydrationSettled(false);

        CloudClientError denied;
        denied.httpStatus = 403;
        denied.apiCode = QStringLiteral("session_password_required");
        flow.onFailed(log.next, denied);
        if (!flow.passwordRequired() || flow.finished()) {
            return fail(QStringLiteral("a password prompt ended the flow"));
        }
        if (flow.stepState(JoinStep::HydrateAssets) != JoinStepState::Done) {
            return fail(QStringLiteral("a password prompt undid earlier work"));
        }

        const int codeCallsBefore = log.codeCalls;
        if (!flow.begin({}, QStringLiteral("jam night"))) {
            return fail(QStringLiteral("the flow did not resume"));
        }
        if (log.codeCalls != codeCallsBefore) {
            return fail(QStringLiteral("resuming redeemed the code again"));
        }
        if (log.lastPassword != QStringLiteral("jam night")) {
            return fail(QStringLiteral("the password did not reach the port"));
        }
        flow.onSessionState(log.next, joined);
        if (!flow.succeeded()) {
            return fail(QStringLiteral("the resumed join did not settle"));
        }
    }

    // A failure marks its own step and leaves the later ones untouched: one
    // thing broke, not six.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        flow.begin(QStringLiteral("123456789012"), {});
        flow.onCodeAccepted(log.next, projectView(CloudProjectStatus::Active));
        flow.onProjectReceived(log.next, projectView(CloudProjectStatus::Active));
        flow.onSyncPhase(CloudSyncPhase::Failed);
        if (flow.stepState(JoinStep::DownloadSnapshot) !=
            JoinStepState::Failed) {
            return fail(QStringLiteral("a sync failure was not recorded"));
        }
        if (flow.stepState(JoinStep::HydrateAssets) != JoinStepState::Pending ||
            flow.stepState(JoinStep::Connect) != JoinStepState::Pending) {
            return fail(QStringLiteral("a failure marked later steps failed"));
        }
        if (!flow.finished() || flow.succeeded()) {
            return fail(QStringLiteral("a failed flow reported success"));
        }
    }

    // An archived project is refused before anything is downloaded.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        flow.begin(QStringLiteral("123456789012"), {});
        flow.onCodeAccepted(log.next, projectView(CloudProjectStatus::Active));
        flow.onProjectReceived(log.next,
                               projectView(CloudProjectStatus::Archived));
        if (flow.stepState(JoinStep::VerifyMembership) !=
                JoinStepState::Failed ||
            log.syncCalls != 0) {
            return fail(QStringLiteral("an archived project was accepted"));
        }
    }

    // Stale replies are ignored rather than advancing the flow.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        flow.begin(QStringLiteral("123456789012"), {});
        flow.onCodeAccepted(log.next + 99,
                            projectView(CloudProjectStatus::Active));
        if (flow.stepState(JoinStep::ResolveCode) != JoinStepState::Running) {
            return fail(QStringLiteral("a stale reply advanced the flow"));
        }
        flow.onSessionState(log.next, joined);
        if (flow.succeeded()) {
            return fail(QStringLiteral("an out-of-order reply completed the join"));
        }
    }

    // Input validation happens before any request is made.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        if (flow.begin(QStringLiteral("12a"), {}) || log.codeCalls != 0) {
            return fail(QStringLiteral("a malformed code was sent"));
        }
        if (flow.begin(QStringLiteral("123456789012"),
                       QStringLiteral("short")) ||
            log.codeCalls != 0) {
            return fail(QStringLiteral("a too-short password was sent"));
        }
        if (!JoinFlowState::validPassword({}) ||
            JoinFlowState::validPassword(QStringLiteral("12345")) ||
            !JoinFlowState::validPassword(QStringLiteral("123456"))) {
            return fail(QStringLiteral("the password policy is wrong"));
        }
        if (JoinFlowState::normalizeCode(QStringLiteral("(1234) 5678.9012")) !=
            QStringLiteral("123456789012")) {
            return fail(QStringLiteral("code normalisation is wrong"));
        }
    }

    // Shutdown cancels what is in flight.
    {
        Recorder log;
        JoinFlowState flow(makePorts(log));
        flow.begin(QStringLiteral("123456789012"), {});
        flow.shutdown();
        if (log.cancels != 1 || !flow.finished() || flow.succeeded()) {
            return fail(QStringLiteral("shutdown did not cancel the request"));
        }
    }
    return true;
}

} // namespace collab
