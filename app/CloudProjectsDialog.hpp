#pragma once

#include "CloudProjectClient.hpp"

#include <QCoreApplication>
#include <QDialog>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace collab {

/// Widget-free list, selection and enablement machine for the cloud project
/// browser.
///
/// Ports are std::function so checkCloudProjectsDialogForTest can drive it with
/// a synchronous fake, the same shape InviteRequestState and JoinFlowState use.
/// Every button's enabled state comes from allows(); the widgets only render it,
/// so the rules are testable and cannot drift between the two.
class CloudProjectListState final {
    // Pinned rather than inferred: lupdate records an unqualified context for a
    // namespaced class while Qt looks up the namespaced one, and the mismatch
    // silently leaves the strings untranslated. See JoinFlowState.
    Q_DECLARE_TR_FUNCTIONS(CloudProjectListState)
public:
    enum class Phase : quint8 { Idle, Loading, Ready, Failed };
    enum class Action : quint8 { Open, Publish, Invite, Archive, Join, Refresh };

    struct Ports {
        std::function<quint64()> list;
        std::function<quint64(const QString& projectId)> archive;
        std::function<bool(quint64)> cancel;
    };

    /// `openProjectId` is the cloud project this window already has open. It is
    /// excluded from destructive actions.
    CloudProjectListState(QString openProjectId, Ports ports);
    ~CloudProjectListState();

    Phase phase() const noexcept;
    const QVector<CloudProjectView>& projects() const noexcept;
    QString selectedProjectId() const;
    const CloudProjectView* selected() const noexcept;
    bool select(const QString& projectId);

    /// The single source of truth for what the user may do right now.
    bool allows(Action action) const;

    const QString& safeMessage() const noexcept;
    bool messageIsError() const noexcept;

    bool refresh();
    bool beginArchive();

    void onListed(quint64 requestId, const QVector<CloudProjectView>& projects);
    void onArchived(quint64 requestId);
    void onFailed(quint64 requestId, CloudRequestKind kind,
                  const CloudClientError& error);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Cloud project browser: what exists, what state it is in, and what can be
/// done with it. Replaces an inline QDialog that rendered every project as one
/// run-on line of text.
class CloudProjectsDialog final : public QDialog {
    Q_OBJECT
public:
    CloudProjectsDialog(CloudProjectClient* projects,
                        const QString& openProjectId,
                        QWidget* parent = nullptr);
    ~CloudProjectsDialog() override;

    /// The project the user chose to open, valid after accept().
    QString chosenProjectId() const;

signals:
    void publishRequested();
    void inviteRequested(const QString& projectId);
    /// Asks the owner to run the join-by-code flow; the dialog does not own it.
    void joinByCodeRequested();

public slots:
    void reject() override;

private:
    void applyTheme();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Deterministic fake-port checks for the enablement rules, stale responses and
/// failure isolation. Builds no widgets, so it needs only QCoreApplication.
bool checkCloudProjectsDialogForTest(QString* error = nullptr);

} // namespace collab
