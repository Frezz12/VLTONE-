#pragma once

#include <QDialog>

#include <cstdint>

namespace account { class Service; }
class QFrame;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

/// The one visible surface while VLT Studio Pro boots.
///
/// It restores the account silently first and reveals login controls only when
/// they are actually required. Once authenticated, the same window reports
/// audio/UI initialization and the incremental plugin scan, so startup never
/// becomes a chain of unrelated dialogs.
class StartupWindow final : public QDialog {
    Q_OBJECT
public:
    explicit StartupWindow(account::Service* service,
                           QWidget* parent = nullptr);

    /// Show the window and keep processing events until a stored entitlement
    /// is restored, the user signs in, or the window is cancelled.
    bool runAuthentication();
    void showSystemLoading();
    void showPluginScan(std::uint32_t done, std::uint32_t total,
                        const QString& currentPath);
    void showReady(int pluginCount);
    bool cancelled() const { return m_cancelled; }

    /// Headless smoke check for the logo/status/progress hierarchy.
    bool checkForTest();

public slots:
    void reject() override;

protected:
    void changeEvent(QEvent* event) override;

private:
    enum class Stage {
        Preparing,
        CheckingLicense,
        LicenseConfirmed,
        LoadingSystem,
        PluginScan,
        Ready,
        SignIn,
    };

    void submit();
    void revealLogin(const QString& reason, bool error);
    void setStatusError(const QString& message);
    void openSite(const QString& path);
    void retranslateUi();
    void renderStage();
    /// Keep the loading state compact, expanding vertically only when the
    /// sign-in form is actually needed while preserving the window centre.
    void syncWindowSize();
    void applyTheme();

    account::Service* m_service = nullptr;
    QComboBox* m_language = nullptr;
    QLabel* m_logo = nullptr;
    QLabel* m_product = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_detail = nullptr;
    QLabel* m_emailLabel = nullptr;
    QLabel* m_passwordLabel = nullptr;
    QProgressBar* m_progress = nullptr;
    QFrame* m_loginPanel = nullptr;
    QLineEdit* m_email = nullptr;
    QLineEdit* m_password = nullptr;
    QPushButton* m_login = nullptr;
    QPushButton* m_register = nullptr;
    QPushButton* m_reset = nullptr;
    Stage m_stage = Stage::Preparing;
    QString m_stageDetail;
    QString m_scanPath;
    std::uint32_t m_scanDone = 0;
    std::uint32_t m_scanTotal = 0;
    int m_pluginCount = 0;
    bool m_stageError = false;
    bool m_restoreStarted = false;
    bool m_cancelled = false;
};
