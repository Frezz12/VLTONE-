#pragma once

#include "CloudProjectClient.hpp"

#include <QDialog>
#include <QString>

#include <memory>

class QCloseEvent;

namespace collab {

/// Owner-facing creator for a single cloud-project invitation.
///
/// The one-time token and the short numeric code never leave this dialog except
/// through an explicit clipboard action by the user.  In particular, neither is
/// exposed as a signal, persisted in settings, or included in diagnostic text.
class CloudProjectInviteDialog final : public QDialog {
    Q_OBJECT
public:
    CloudProjectInviteDialog(CloudProjectClient* projects,
                             const QString& canonicalProjectId,
                             QWidget* parent = nullptr);
    ~CloudProjectInviteDialog() override;

    QString projectId() const;
    bool hasPendingRequest() const noexcept;
    bool hasOneTimeToken() const noexcept;

public slots:
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void applyTheme();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Deterministic fake-port checks for request shaping, synchronous callbacks,
/// stale-response isolation, cancellation, and secret lifetime.  This helper
/// does not construct widgets and therefore only requires QCoreApplication.
bool checkCloudProjectInviteDialogForTest(QString* error = nullptr);

} // namespace collab

