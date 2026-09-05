#include "CloudProjectsDialog.hpp"

#include "CollaborationDialogStyle.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace collab {
namespace {

constexpr int kProjectIdRole = Qt::UserRole;
constexpr int kTitleRole = Qt::UserRole + 1;
constexpr int kStatusRole = Qt::UserRole + 2;
constexpr int kRoleRole = Qt::UserRole + 3;
constexpr int kUpdatedRole = Qt::UserRole + 4;
constexpr int kIsOpenRole = Qt::UserRole + 5;

constexpr int kCardHeight = 58;

QString roleText(CloudProjectRole role) {
    switch (role) {
        case CloudProjectRole::Owner:  return CloudProjectListState::tr("Owner");
        case CloudProjectRole::Editor: return CloudProjectListState::tr("Editor");
        case CloudProjectRole::Viewer: return CloudProjectListState::tr("Viewer");
    }
    return {};
}

QString statusText(CloudProjectStatus status) {
    switch (status) {
        case CloudProjectStatus::Uploading:
            return CloudProjectListState::tr("Uploading");
        case CloudProjectStatus::Active:
            return CloudProjectListState::tr("Active");
        case CloudProjectStatus::ReadOnly:
            return CloudProjectListState::tr("Read-only");
        case CloudProjectStatus::Conflict:
            return CloudProjectListState::tr("Conflict");
        case CloudProjectStatus::Archived:
            return CloudProjectListState::tr("Archived");
    }
    return {};
}

/// Status is a shape and a word, not only a colour: the pill carries text so it
/// stays readable without hue.
QColor statusTint(CloudProjectStatus status) {
    switch (status) {
        case CloudProjectStatus::Active:    return th().accent;
        case CloudProjectStatus::Uploading: return Theme::cycle();
        case CloudProjectStatus::ReadOnly:  return th().textSecondary;
        case CloudProjectStatus::Conflict:  return Theme::record();
        case CloudProjectStatus::Archived:  return th().separator();
    }
    return th().textSecondary;
}

/// Coarse, human relative time. Precision past "yesterday" is noise in a list
/// whose job is to tell recent projects from stale ones.
QString relativeTime(const QDateTime& when) {
    if (!when.isValid()) return {};
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64 seconds = when.secsTo(now);
    if (seconds < 0) return CloudProjectListState::tr("Just now");
    if (seconds < 90) return CloudProjectListState::tr("Just now");
    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return CloudProjectListState::tr("%n minute(s) ago", nullptr, int(minutes));
    const qint64 hours = minutes / 60;
    if (hours < 24)
        return CloudProjectListState::tr("%n hour(s) ago", nullptr, int(hours));
    const qint64 days = hours / 24;
    if (days < 30)
        return CloudProjectListState::tr("%n day(s) ago", nullptr, int(days));
    return QLocale().toString(when.toLocalTime().date(), QLocale::ShortFormat);
}

/// Draws one project as a card: title, a status pill, the role and when it last
/// changed. The old single-line "%1 — %2, %3" made every project look the same.
class ProjectCardDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        return QSize(QStyledItemDelegate::sizeHint(option, index).width(),
                     kCardHeight);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const Theme& theme = th();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const bool selected = option.state & QStyle::State_Selected;
        const QRectF card = QRectF(option.rect).adjusted(3, 2, -3, -2);
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? theme.selection : theme.well());
        painter->drawRoundedRect(card, 7, 7);
        if (selected) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(theme.accent, 1.0));
            painter->drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
        }

        const auto status =
            CloudProjectStatus(index.data(kStatusRole).toInt());
        const auto role = CloudProjectRole(index.data(kRoleRole).toInt());
        const bool isOpen = index.data(kIsOpenRole).toBool();

        QFont titleFont = option.font;
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(theme.textPrimary);
        const QRectF titleRect(card.left() + 12, card.top() + 8,
                               card.width() - 130, 20);
        painter->drawText(
            titleRect, Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(index.data(kTitleRole).toString(),
                                              Qt::ElideRight,
                                              int(titleRect.width())));

        QFont detailFont = option.font;
        detailFont.setPointSizeF(std::max(7.0, option.font.pointSizeF() - 1.0));
        painter->setFont(detailFont);
        painter->setPen(theme.textSecondary);
        QStringList details;
        details << roleText(role);
        if (isOpen) details << CloudProjectListState::tr("Open now");
        const QString updated = relativeTime(index.data(kUpdatedRole).toDateTime());
        if (!updated.isEmpty()) details << updated;
        painter->drawText(QRectF(card.left() + 12, card.top() + 30,
                                 card.width() - 130, 18),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          details.join(QStringLiteral(" · ")));

        // Status pill, right aligned.
        const QString label = statusText(status);
        const int pillWidth = painter->fontMetrics().horizontalAdvance(label) + 18;
        const QRectF pill(card.right() - pillWidth - 12,
                          card.center().y() - 9, pillWidth, 18);
        const QColor tint = statusTint(status);
        painter->setPen(Qt::NoPen);
        painter->setBrush(mixColors(tint, theme.surface, 0.78));
        painter->drawRoundedRect(pill, 9, 9);
        painter->setPen(tint);
        painter->drawText(pill, Qt::AlignCenter, label);
        painter->restore();
    }
};

} // namespace

struct CloudProjectListState::Impl {
    QString openProjectId;
    Ports ports;
    Phase phase = Phase::Idle;
    QVector<CloudProjectView> projects;
    QString selectedId;
    quint64 listRequest = 0;
    quint64 archiveRequest = 0;
    bool issuing = false;
    QString safeMessage;
    bool messageIsError = false;

    Impl(QString open, Ports value)
        : openProjectId(collab::dialog::canonicalUuid(open)),
          ports(std::move(value)) {}

    const CloudProjectView* find(const QString& projectId) const {
        for (const CloudProjectView& view : projects) {
            if (view.project.id == projectId) return &view;
        }
        return nullptr;
    }

    void setInfo(const QString& text) {
        safeMessage = text;
        messageIsError = false;
    }
    void setError(const QString& text) {
        safeMessage = text;
        messageIsError = true;
    }
};

CloudProjectListState::CloudProjectListState(QString openProjectId, Ports ports)
    : m_impl(std::make_unique<Impl>(std::move(openProjectId),
                                    std::move(ports))) {}

CloudProjectListState::~CloudProjectListState() { shutdown(); }

CloudProjectListState::Phase CloudProjectListState::phase() const noexcept {
    return m_impl->phase;
}

const QVector<CloudProjectView>& CloudProjectListState::projects()
    const noexcept {
    return m_impl->projects;
}

QString CloudProjectListState::selectedProjectId() const {
    return m_impl->selectedId;
}

const CloudProjectView* CloudProjectListState::selected() const noexcept {
    return m_impl->selectedId.isEmpty() ? nullptr
                                        : m_impl->find(m_impl->selectedId);
}

bool CloudProjectListState::select(const QString& projectId) {
    if (projectId.isEmpty()) {
        m_impl->selectedId.clear();
        return true;
    }
    if (!m_impl->find(projectId)) return false;
    m_impl->selectedId = projectId;
    return true;
}

bool CloudProjectListState::allows(Action action) const {
    const CloudProjectView* view = selected();
    switch (action) {
        case Action::Refresh:
            return m_impl->phase != Phase::Loading && bool(m_impl->ports.list);
        case Action::Publish:
        case Action::Join:
            // Neither depends on a selection: publishing acts on the local
            // project, and joining is how you reach a project you cannot see
            // in this list yet.
            return true;
        case Action::Open:
            return view != nullptr &&
                   view->project.status != CloudProjectStatus::Archived &&
                   view->project.status != CloudProjectStatus::Uploading;
        case Action::Invite:
            // Inviting needs the right to manage members.
            return view != nullptr &&
                   view->role == CloudProjectRole::Owner &&
                   view->project.status != CloudProjectStatus::Archived;
        case Action::Archive:
            // Owner only, not already archived, and never the project this
            // window has open — archiving what you are editing would leave the
            // session pointing at something nobody can rejoin.
            return view != nullptr && m_impl->archiveRequest == 0 &&
                   view->role == CloudProjectRole::Owner &&
                   view->project.status != CloudProjectStatus::Archived &&
                   view->project.id != m_impl->openProjectId &&
                   bool(m_impl->ports.archive);
    }
    return false;
}

const QString& CloudProjectListState::safeMessage() const noexcept {
    return m_impl->safeMessage;
}

bool CloudProjectListState::messageIsError() const noexcept {
    return m_impl->messageIsError;
}

bool CloudProjectListState::refresh() {
    if (!allows(Action::Refresh)) return false;
    m_impl->phase = Phase::Loading;
    m_impl->setInfo(CloudProjectListState::tr("Loading…"));
    m_impl->issuing = true;
    const quint64 issued = m_impl->ports.list();
    m_impl->issuing = false;
    m_impl->listRequest = issued;
    if (issued == 0) {
        m_impl->phase = Phase::Failed;
        m_impl->setError(CloudProjectListState::tr("Cloud projects could not be loaded."));
        return false;
    }
    return true;
}

bool CloudProjectListState::beginArchive() {
    if (!allows(Action::Archive)) return false;
    const QString projectId = m_impl->selectedId;
    m_impl->issuing = true;
    const quint64 issued = m_impl->ports.archive(projectId);
    m_impl->issuing = false;
    m_impl->archiveRequest = issued;
    if (issued == 0) {
        m_impl->setError(CloudProjectListState::tr("The project could not be archived."));
        return false;
    }
    m_impl->setInfo(CloudProjectListState::tr("Archiving…"));
    return true;
}

void CloudProjectListState::onListed(
    quint64 requestId, const QVector<CloudProjectView>& projects) {
    if (m_impl->issuing || m_impl->listRequest == 0 ||
        requestId != m_impl->listRequest) {
        return;
    }
    m_impl->listRequest = 0;
    m_impl->projects = projects;
    m_impl->phase = Phase::Ready;
    // A selection that no longer exists must not keep enabling buttons.
    if (!m_impl->selectedId.isEmpty() && !m_impl->find(m_impl->selectedId))
        m_impl->selectedId.clear();
    m_impl->setInfo(projects.isEmpty()
                        ? CloudProjectListState::tr("You have no cloud projects yet.")
                        : QString());
}

void CloudProjectListState::onArchived(quint64 requestId) {
    if (m_impl->archiveRequest == 0 || requestId != m_impl->archiveRequest)
        return;
    m_impl->archiveRequest = 0;
    m_impl->setInfo(CloudProjectListState::tr("Project archived."));
    refresh();
}

void CloudProjectListState::onFailed(quint64 requestId, CloudRequestKind kind,
                                     const CloudClientError& error) {
    // listProjects can reject synchronously, before refresh() has stored the
    // id it returned, so the kind is matched as well as the id.
    const bool isList = kind == CloudRequestKind::ListProjects ||
                        (m_impl->listRequest != 0 &&
                         requestId == m_impl->listRequest);
    const bool isArchive = m_impl->archiveRequest != 0 &&
                           requestId == m_impl->archiveRequest;
    if (!isList && !isArchive) return;
    if (isList) {
        m_impl->listRequest = 0;
        m_impl->phase = Phase::Failed;
    }
    if (isArchive) m_impl->archiveRequest = 0;
    m_impl->setError(collab::dialog::boundedSafeMessage(
        error.safeMessage, CloudProjectListState::tr("The cloud request could not be completed.")));
}

void CloudProjectListState::shutdown() {
    const quint64 list = m_impl->listRequest;
    const quint64 archive = m_impl->archiveRequest;
    m_impl->listRequest = 0;
    m_impl->archiveRequest = 0;
    m_impl->issuing = false;
    if (m_impl->ports.cancel) {
        if (list != 0) m_impl->ports.cancel(list);
        if (archive != 0) m_impl->ports.cancel(archive);
    }
}

// ── Dialog ─────────────────────────────────────────────────────────────────

struct CloudProjectsDialog::Impl {
    CloudProjectsDialog* q = nullptr;
    std::unique_ptr<CloudProjectListState> state;
    QPointer<CloudProjectClient> projects;
    QString openProjectId;
    QString chosen;

    QListWidget* list = nullptr;
    QLabel* status = nullptr;
    QPushButton* open = nullptr;
    QPushButton* publish = nullptr;
    QPushButton* invite = nullptr;
    QPushButton* archive = nullptr;
    QPushButton* join = nullptr;
    QPushButton* refresh = nullptr;

    void rebuild() {
        const QString keep = state->selectedProjectId();
        list->clear();
        for (const CloudProjectView& view : state->projects()) {
            auto* item = new QListWidgetItem(list);
            item->setData(kProjectIdRole, view.project.id);
            item->setData(kTitleRole, view.project.title);
            item->setData(kStatusRole, int(view.project.status));
            item->setData(kRoleRole, int(view.role));
            item->setData(kUpdatedRole, view.project.updatedAt);
            item->setData(kIsOpenRole, view.project.id == openProjectId);
            if (view.project.id == keep) list->setCurrentItem(item);
        }
        refreshActions();
    }

    void refreshActions() {
        using Action = CloudProjectListState::Action;
        open->setEnabled(state->allows(Action::Open));
        publish->setEnabled(state->allows(Action::Publish));
        invite->setEnabled(state->allows(Action::Invite));
        archive->setEnabled(state->allows(Action::Archive));
        join->setEnabled(state->allows(Action::Join));
        refresh->setEnabled(state->allows(Action::Refresh));

        status->setText(state->safeMessage());
        status->setObjectName(state->messageIsError()
                                  ? QStringLiteral("CollabError")
                                  : QStringLiteral("CollabSecondary"));
        status->setVisible(!state->safeMessage().isEmpty());
        status->style()->unpolish(status);
        status->style()->polish(status);
    }

    void syncSelection() {
        const QListWidgetItem* item = list->currentItem();
        state->select(item ? item->data(kProjectIdRole).toString() : QString());
        refreshActions();
    }
};

CloudProjectsDialog::CloudProjectsDialog(CloudProjectClient* projects,
                                         const QString& openProjectId,
                                         QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->q = this;
    m_impl->projects = projects;
    m_impl->openProjectId = collab::dialog::canonicalUuid(openProjectId);

    const QPointer<CloudProjectClient> guard(projects);
    CloudProjectListState::Ports ports;
    ports.list = [guard] { return guard ? guard->listProjects() : 0; };
    ports.archive = [guard](const QString& projectId) {
        return guard ? guard->archiveProject(projectId) : 0;
    };
    ports.cancel = [guard](quint64 requestId) {
        return guard && guard->cancel(requestId);
    };
    m_impl->state = std::make_unique<CloudProjectListState>(
        m_impl->openProjectId, std::move(ports));

    setWindowTitle(CloudProjectListState::tr("Cloud Projects"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    resize(620, 460);

    QLabel* title = collab::dialog::titleLabel(CloudProjectListState::tr("Cloud projects"), this);
    auto* description = new QLabel(
        CloudProjectListState::tr("Projects you have published or been invited to. Open one to work "
           "on it with other people."),
        this);
    description->setObjectName(QStringLiteral("CollabSecondary"));
    description->setWordWrap(true);

    m_impl->list = new QListWidget(this);
    m_impl->list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_impl->list->setItemDelegate(new ProjectCardDelegate(m_impl->list));
    m_impl->list->setUniformItemSizes(true);
    m_impl->list->setSpacing(0);

    m_impl->status = new QLabel(this);
    m_impl->status->setTextFormat(Qt::PlainText);
    m_impl->status->setWordWrap(true);

    m_impl->open = new QPushButton(CloudProjectListState::tr("Open"), this);
    m_impl->open->setDefault(true);
    m_impl->publish = new QPushButton(CloudProjectListState::tr("Publish…"), this);
    m_impl->invite = new QPushButton(CloudProjectListState::tr("Invite…"), this);
    m_impl->archive = new QPushButton(CloudProjectListState::tr("Archive"), this);
    m_impl->join = new QPushButton(CloudProjectListState::tr("Join by Code…"), this);
    m_impl->refresh = new QPushButton(CloudProjectListState::tr("Refresh"), this);
    auto* close = new QPushButton(CloudProjectListState::tr("Close"), this);

    // Secondary actions on the left, the primary pair on the right, so the
    // destructive one is nowhere near the button people press by reflex.
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_impl->join);
    buttons->addWidget(m_impl->publish);
    buttons->addWidget(m_impl->invite);
    buttons->addWidget(m_impl->archive);
    buttons->addStretch(1);
    buttons->addWidget(m_impl->refresh);
    buttons->addWidget(close);
    buttons->addWidget(m_impl->open);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(collab::dialog::kMargins);
    column->setSpacing(collab::dialog::kSpacing);
    column->addWidget(title);
    column->addWidget(description);
    column->addWidget(m_impl->list, 1);
    column->addWidget(m_impl->status);
    column->addLayout(buttons);

    connect(m_impl->list, &QListWidget::itemSelectionChanged, this,
            [this] { m_impl->syncSelection(); });
    connect(m_impl->list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) {
                if (m_impl->state->allows(CloudProjectListState::Action::Open))
                    accept();
            });
    connect(m_impl->open, &QPushButton::clicked, this, &QDialog::accept);
    connect(close, &QPushButton::clicked, this, &CloudProjectsDialog::reject);
    connect(m_impl->refresh, &QPushButton::clicked, this, [this] {
        m_impl->state->refresh();
        m_impl->refreshActions();
    });
    connect(m_impl->publish, &QPushButton::clicked, this, [this] {
        emit publishRequested();
        reject();
    });
    connect(m_impl->invite, &QPushButton::clicked, this, [this] {
        emit inviteRequested(m_impl->state->selectedProjectId());
    });
    connect(m_impl->join, &QPushButton::clicked, this, [this] {
        emit joinByCodeRequested();
    });
    connect(m_impl->archive, &QPushButton::clicked, this, [this] {
        const CloudProjectView* view = m_impl->state->selected();
        if (!view) return;
        if (QMessageBox::question(
                this, CloudProjectListState::tr("Archive Cloud Project"),
                CloudProjectListState::tr("Archive “%1”? Nobody will be able to open or join it "
                   "afterwards.")
                    .arg(view->project.title),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        m_impl->state->beginArchive();
        m_impl->refreshActions();
    });

    if (projects) {
        connect(projects, &CloudProjectClient::projectsListed, this,
                [this](quint64 requestId,
                       const QVector<CloudProjectView>& listed) {
                    m_impl->state->onListed(requestId, listed);
                    m_impl->rebuild();
                });
        connect(projects, &CloudProjectClient::operationCompleted, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const QString&) {
                    if (kind != CloudRequestKind::ArchiveProject) return;
                    m_impl->state->onArchived(requestId);
                    m_impl->refreshActions();
                });
        connect(projects, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
                    m_impl->state->onFailed(requestId, kind, error);
                    m_impl->rebuild();
                });
    }

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &CloudProjectsDialog::applyTheme);
    applyTheme();
    m_impl->state->refresh();
    m_impl->refreshActions();
}

CloudProjectsDialog::~CloudProjectsDialog() = default;

QString CloudProjectsDialog::chosenProjectId() const {
    return m_impl->state->selectedProjectId();
}

void CloudProjectsDialog::reject() {
    m_impl->state->shutdown();
    QDialog::reject();
}

void CloudProjectsDialog::applyTheme() {
    setStyleSheet(collab::dialog::styleSheet());
    if (m_impl->list) m_impl->list->viewport()->update();
}

bool checkCloudProjectsDialogForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    using Action = CloudProjectListState::Action;
    const QString openId = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString otherId = QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString viewerId = QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const QString archivedId = QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");

    const auto make = [](const QString& id, CloudProjectRole role,
                         CloudProjectStatus status) {
        CloudProjectView view;
        view.project.id = id;
        view.project.title = QStringLiteral("Project");
        view.project.status = status;
        view.role = role;
        return view;
    };
    const QVector<CloudProjectView> listing{
        make(openId, CloudProjectRole::Owner, CloudProjectStatus::Active),
        make(otherId, CloudProjectRole::Owner, CloudProjectStatus::Active),
        make(viewerId, CloudProjectRole::Viewer, CloudProjectStatus::Active),
        make(archivedId, CloudProjectRole::Owner, CloudProjectStatus::Archived),
    };

    struct Recorder {
        quint64 next = 0;
        int lists = 0;
        int archives = 0;
        int cancels = 0;
        QString archived;
    };
    const auto ports = [](Recorder& log) {
        CloudProjectListState::Ports value;
        value.list = [&log] {
            ++log.lists;
            return ++log.next;
        };
        value.archive = [&log](const QString& projectId) {
            ++log.archives;
            log.archived = projectId;
            return ++log.next;
        };
        value.cancel = [&log](quint64) {
            ++log.cancels;
            return true;
        };
        return value;
    };

    {
        Recorder log;
        CloudProjectListState state(openId, ports(log));
        if (!state.refresh() || state.phase() !=
                                    CloudProjectListState::Phase::Loading) {
            return fail(QStringLiteral("the list request did not start"));
        }
        // A second refresh while one is in flight would race two listings.
        if (state.allows(Action::Refresh) || state.refresh()) {
            return fail(QStringLiteral("a concurrent refresh was allowed"));
        }
        state.onListed(log.next, listing);
        if (state.phase() != CloudProjectListState::Phase::Ready ||
            state.projects().size() != 4) {
            return fail(QStringLiteral("the listing was not stored"));
        }

        // Nothing selected: only the selection-free actions are available.
        if (state.allows(Action::Open) || state.allows(Action::Archive) ||
            state.allows(Action::Invite) || !state.allows(Action::Join) ||
            !state.allows(Action::Publish)) {
            return fail(QStringLiteral("actions ignored the empty selection"));
        }

        // The project this window already has open must not be archivable.
        state.select(openId);
        if (!state.allows(Action::Open) || state.allows(Action::Archive)) {
            return fail(QStringLiteral("the open project was archivable"));
        }
        // Another owned project is.
        state.select(otherId);
        if (!state.allows(Action::Archive) || !state.allows(Action::Invite)) {
            return fail(QStringLiteral("an owned project was not archivable"));
        }
        // A viewer may open but neither invite nor archive.
        state.select(viewerId);
        if (!state.allows(Action::Open) || state.allows(Action::Archive) ||
            state.allows(Action::Invite)) {
            return fail(QStringLiteral("a viewer was given owner actions"));
        }
        // An archived project can be neither opened nor archived again.
        state.select(archivedId);
        if (state.allows(Action::Open) || state.allows(Action::Archive)) {
            return fail(QStringLiteral("an archived project stayed actionable"));
        }

        // Selecting something absent leaves the previous selection alone.
        if (state.select(QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")) ||
            state.selectedProjectId() != archivedId) {
            return fail(QStringLiteral("an unknown selection was accepted"));
        }
    }

    // A selection that disappears from a later listing stops enabling actions.
    {
        Recorder log;
        CloudProjectListState state(openId, ports(log));
        state.refresh();
        state.onListed(log.next, listing);
        state.select(otherId);
        state.refresh();
        state.onListed(log.next, {make(openId, CloudProjectRole::Owner,
                                       CloudProjectStatus::Active)});
        if (!state.selectedProjectId().isEmpty() ||
            state.allows(Action::Archive)) {
            return fail(QStringLiteral("a vanished selection stayed actionable"));
        }
    }

    // Archiving refreshes, and a second archive cannot be started while the
    // first is in flight.
    {
        Recorder log;
        CloudProjectListState state(openId, ports(log));
        state.refresh();
        state.onListed(log.next, listing);
        state.select(otherId);
        if (!state.beginArchive() || log.archived != otherId) {
            return fail(QStringLiteral("archiving did not start"));
        }
        if (state.allows(Action::Archive) || state.beginArchive()) {
            return fail(QStringLiteral("a second archive was allowed"));
        }
        const int listsBefore = log.lists;
        state.onArchived(log.next);
        if (log.lists != listsBefore + 1) {
            return fail(QStringLiteral("archiving did not refresh the list"));
        }
    }

    // A stale listing is ignored, and a synchronous list failure is attributed
    // by kind even though no id was ever stored.
    {
        Recorder log;
        CloudProjectListState state(openId, ports(log));
        state.refresh();
        state.onListed(log.next + 42, listing);
        if (!state.projects().isEmpty()) {
            return fail(QStringLiteral("a stale listing was accepted"));
        }
        CloudClientError error;
        error.safeMessage = QStringLiteral("Server said no");
        state.onFailed(0, CloudRequestKind::ListProjects, error);
        if (state.phase() != CloudProjectListState::Phase::Failed ||
            !state.messageIsError() ||
            !state.safeMessage().contains(QStringLiteral("Server said no"))) {
            return fail(QStringLiteral("a list failure was not reported"));
        }
        // A failure for an unrelated request must not touch this dialog.
        state.refresh();
        state.onFailed(log.next + 500, CloudRequestKind::GetProject, error);
        if (state.phase() != CloudProjectListState::Phase::Loading) {
            return fail(QStringLiteral("an unrelated failure was consumed"));
        }
    }

    // Shutdown cancels whatever is outstanding.
    {
        Recorder log;
        CloudProjectListState state(openId, ports(log));
        state.refresh();
        state.onListed(log.next, listing);
        state.select(otherId);
        state.beginArchive();
        state.shutdown();
        if (log.cancels != 1) {
            return fail(QStringLiteral("shutdown did not cancel the archive"));
        }
    }
    return true;
}

} // namespace collab
