#include "StartupWindow.hpp"

#include "AccountService.hpp"
#include "LocalizationManager.hpp"
#include "Theme.hpp"

#include <algorithm>
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kStartupWidth = 460;
constexpr int kCompactHeight = 380;
constexpr int kLoginHeight = 500;

} // namespace

StartupWindow::StartupWindow(account::Service* service, QWidget* parent)
    : QDialog(parent), m_service(service) {
    setObjectName(QStringLiteral("StartupWindow"));
    setWindowTitle(QStringLiteral("VLT Studio Pro"));
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                   Qt::WindowCloseButtonHint);
    setModal(true);
    setFixedSize(kStartupWidth, kCompactHeight);

    m_language = new QComboBox(this);
    m_language->setObjectName(QStringLiteral("StartupLanguage"));
    for (const ui::LanguageInfo& language :
         ui::LocalizationManager::instance().languages()) {
        m_language->addItem(
            QStringLiteral("%1 (%2)").arg(language.languageName,
                                           language.locale),
            language.locale);
    }
    const int activeLanguage = m_language->findData(
        ui::LocalizationManager::instance().activeLocale());
    m_language->setCurrentIndex(activeLanguage >= 0 ? activeLanguage : 0);
    connect(m_language, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (index < 0) return;
                const QString locale = m_language->itemData(index).toString();
                if (locale == ui::LocalizationManager::instance().activeLocale())
                    return;
                QString error;
                if (!ui::LocalizationManager::instance().activateLanguage(
                        locale, true, &error)) {
                    QSignalBlocker blocker(m_language);
                    const int current = m_language->findData(
                        ui::LocalizationManager::instance().activeLocale());
                    m_language->setCurrentIndex(current);
                    setStatusError(error);
                    return;
                }
                retranslateUi();
                renderStage();
            });

    m_logo = new QLabel(this);
    m_logo->setObjectName(QStringLiteral("StartupLogo"));
    m_logo->setFixedSize(76, 76);
    m_logo->setAlignment(Qt::AlignCenter);
    m_logo->setAccessibleName(tr("VLT Studio Pro logo"));
    const QPixmap logo(QStringLiteral(":/vlt/icon-1024.png"));
    if (!logo.isNull()) {
        m_logo->setPixmap(logo.scaled(QSize(60, 60), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
    }

    auto* title = new QLabel(QStringLiteral("VLT STUDIO PRO"), this);
    title->setObjectName(QStringLiteral("StartupTitle"));
    title->setAlignment(Qt::AlignCenter);
    QFont titleFont = title->font();
    titleFont.setPixelSize(20);
    titleFont.setBold(true);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    title->setFont(titleFont);

    m_product = new QLabel(tr("Digital audio workstation"), this);
    m_product->setObjectName(QStringLiteral("StartupProduct"));
    m_product->setAlignment(Qt::AlignCenter);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("StartupStatus"));
    m_status->setAlignment(Qt::AlignCenter);
    QFont statusFont = m_status->font();
    statusFont.setPixelSize(14);
    statusFont.setBold(true);
    m_status->setFont(statusFont);

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("StartupDetail"));
    m_detail->setAlignment(Qt::AlignCenter);
    m_detail->setWordWrap(true);
    m_detail->setMinimumHeight(32);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("StartupProgress"));
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->setAccessibleName(tr("Application startup progress"));

    m_loginPanel = new QFrame(this);
    m_loginPanel->setObjectName(QStringLiteral("StartupLoginPanel"));
    auto* loginColumn = new QVBoxLayout(m_loginPanel);
    loginColumn->setContentsMargins(16, 14, 16, 14);
    loginColumn->setSpacing(10);

    m_email = new QLineEdit(m_loginPanel);
    m_email->setObjectName(QStringLiteral("StartupEmail"));
    m_email->setInputMethodHints(Qt::ImhEmailCharactersOnly);
    m_email->setClearButtonEnabled(true);
    m_email->setAccessibleName(tr("Email"));
    m_password = new QLineEdit(m_loginPanel);
    m_password->setObjectName(QStringLiteral("StartupPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(tr("Password"));
    connect(m_password, &QLineEdit::returnPressed, this, &StartupWindow::submit);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    m_emailLabel = new QLabel(tr("Email"), m_loginPanel);
    m_passwordLabel = new QLabel(tr("Password"), m_loginPanel);
    form->addRow(m_emailLabel, m_email);
    form->addRow(m_passwordLabel, m_password);
    loginColumn->addLayout(form);

    m_login = new QPushButton(tr("Sign in"), m_loginPanel);
    m_login->setObjectName(QStringLiteral("StartupLoginButton"));
    m_login->setDefault(true);
    connect(m_login, &QPushButton::clicked, this, &StartupWindow::submit);
    loginColumn->addWidget(m_login);

    auto* links = new QHBoxLayout;
    links->setContentsMargins(0, 0, 0, 0);
    m_register = new QPushButton(tr("Create account"), m_loginPanel);
    m_register->setFlat(true);
    connect(m_register, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("register")); });
    m_reset = new QPushButton(tr("Forgot password?"), m_loginPanel);
    m_reset->setFlat(true);
    connect(m_reset, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("forgot-password")); });
    links->addWidget(m_register);
    links->addStretch(1);
    links->addWidget(m_reset);
    loginColumn->addLayout(links);
    m_loginPanel->hide();

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(32, 22, 32, 24);
    column->setSpacing(8);
    column->addWidget(m_language, 0, Qt::AlignRight);
    column->addStretch(1);
    column->addWidget(m_logo, 0, Qt::AlignHCenter);
    column->addWidget(title);
    column->addWidget(m_product);
    column->addSpacing(8);
    column->addWidget(m_status);
    column->addWidget(m_detail);
    column->addWidget(m_progress);
    column->addSpacing(2);
    column->addWidget(m_loginPanel);
    column->addStretch(1);

    connect(service, &account::Service::busyChanged, this, [this](bool busy) {
        m_login->setDisabled(busy);
        m_email->setDisabled(busy);
        m_password->setDisabled(busy);
        if (busy) {
            m_stage = Stage::CheckingLicense;
            renderStage();
        }
    });
    connect(service, &account::Service::authenticatedChanged, this,
            [this](bool authenticated) {
                if (!authenticated) return;
                m_loginPanel->hide();
                syncWindowSize();
                m_stage = Stage::LicenseConfirmed;
                m_stageDetail = m_service->snapshot().email;
                renderStage();
            });
    connect(service, &account::Service::authenticationRequired, this,
            [this](const QString& reason, bool) {
                revealLogin(reason, reason != tr("Sign in to continue."));
            });
    connect(service, &account::Service::errorOccurred, this,
            [this](const QString&, const QString& message) {
                revealLogin(message, true);
            });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &StartupWindow::applyTheme);

    m_stage = Stage::Preparing;
    retranslateUi();
    renderStage();
    syncWindowSize();
    applyTheme();
}

bool StartupWindow::runAuthentication() {
    if (!m_service) return false;
    m_cancelled = false;
    show();
    raise();
    activateWindow();

    if (m_service->authenticated()) return true;
    QEventLoop wait;
    const QMetaObject::Connection authenticated = connect(
        m_service, &account::Service::authenticatedChanged, &wait,
        [&wait](bool ready) { if (ready) wait.quit(); });
    const QMetaObject::Connection rejected = connect(
        this, &QDialog::rejected, &wait, &QEventLoop::quit);
    if (!m_restoreStarted) {
        m_restoreStarted = true;
        QTimer::singleShot(0, m_service, &account::Service::beginRestore);
    }
    wait.exec();
    disconnect(authenticated);
    disconnect(rejected);
    return !m_cancelled && m_service->authenticated();
}

void StartupWindow::showSystemLoading() {
    m_loginPanel->hide();
    syncWindowSize();
    m_stage = Stage::LoadingSystem;
    renderStage();
}

void StartupWindow::showPluginScan(std::uint32_t done, std::uint32_t total,
                                   const QString& currentPath) {
    m_loginPanel->hide();
    syncWindowSize();
    m_stage = Stage::PluginScan;
    m_scanDone = done;
    m_scanTotal = total;
    m_scanPath = currentPath;
    renderStage();
}

void StartupWindow::showReady(int pluginCount) {
    m_stage = Stage::Ready;
    m_pluginCount = pluginCount;
    renderStage();
}

void StartupWindow::reject() {
    m_cancelled = true;
    QDialog::reject();
}

void StartupWindow::submit() {
    if (!m_service) return;
    if (m_email->text().trimmed().isEmpty() || m_password->text().size() < 12) {
        setStatusError(
            tr("Enter your email and a password of at least 12 characters."));
        return;
    }
    m_service->login(m_email->text(), m_password->text());
}

void StartupWindow::revealLogin(const QString& reason, bool error) {
    m_stage = Stage::SignIn;
    m_stageDetail = error ? reason : QString();
    m_stageError = error;
    m_loginPanel->show();
    syncWindowSize();
    renderStage();
    m_email->setFocus(Qt::OtherFocusReason);
}

void StartupWindow::setStatusError(const QString& message) {
    m_stage = Stage::SignIn;
    m_stageDetail = message;
    m_stageError = true;
    renderStage();
}

void StartupWindow::openSite(const QString& path) {
    QString origin = qEnvironmentVariable(
        "VLT_PUBLIC_ORIGIN", QStringLiteral("https://vltstudio.ru"));
    while (origin.endsWith('/')) origin.chop(1);
    QDesktopServices::openUrl(
        QUrl(origin + QLatin1Char('/') +
             ui::LocalizationManager::instance().websiteLocale() +
             QLatin1Char('/') + path));
}

void StartupWindow::syncWindowSize() {
    const QSize target(kStartupWidth,
                       m_loginPanel && !m_loginPanel->isHidden()
                           ? kLoginHeight
                           : kCompactHeight);
    if (size() == target) return;

    // Expanding the sign-in form should not make the dialog jump down and to
    // the right. Preserve the visual centre once the window is on screen.
    const QPoint centre = frameGeometry().center();
    setFixedSize(target);
    if (isVisible()) move(centre.x() - width() / 2, centre.y() - height() / 2);
}

void StartupWindow::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() != QEvent::LanguageChange) return;
    retranslateUi();
    renderStage();
}

void StartupWindow::retranslateUi() {
    m_language->setAccessibleName(tr("Application language"));
    m_logo->setAccessibleName(tr("VLT Studio Pro logo"));
    m_product->setText(tr("Digital audio workstation"));
    m_progress->setAccessibleName(tr("Application startup progress"));
    m_email->setAccessibleName(tr("Email"));
    m_password->setAccessibleName(tr("Password"));
    m_emailLabel->setText(tr("Email"));
    m_passwordLabel->setText(tr("Password"));
    m_login->setText(tr("Sign in"));
    m_register->setText(tr("Create account"));
    m_reset->setText(tr("Forgot password?"));

    QSignalBlocker blocker(m_language);
    const int active = m_language->findData(
        ui::LocalizationManager::instance().activeLocale());
    if (active >= 0) m_language->setCurrentIndex(active);
}

void StartupWindow::renderStage() {
    QString status;
    QString detail;
    bool indeterminate = false;
    bool error = false;
    m_detail->setToolTip(QString());

    switch (m_stage) {
        case Stage::Preparing:
            status = tr("Preparing startup…");
            detail = tr("Checking saved license");
            indeterminate = true;
            break;
        case Stage::CheckingLicense:
            status = tr("Checking license…");
            detail = tr("Restoring the saved account session");
            indeterminate = true;
            break;
        case Stage::LicenseConfirmed:
            status = tr("License confirmed");
            detail = m_stageDetail.isEmpty() ? tr("Account ready")
                                             : m_stageDetail;
            break;
        case Stage::LoadingSystem:
            status = tr("Loading system…");
            detail = tr("Preparing the audio engine and interface");
            indeterminate = true;
            break;
        case Stage::PluginScan: {
            status = tr("Checking plugins…");
            if (m_scanTotal == 0) {
                detail = tr("Looking for installed plugins");
                indeterminate = true;
            } else {
                const QString file = m_scanPath.isEmpty()
                                         ? QString()
                                         : QFileInfo(m_scanPath).fileName();
                detail = tr("Checked: %1 of %2%3")
                             .arg(m_scanDone)
                             .arg(m_scanTotal)
                             .arg(file.isEmpty()
                                      ? QString()
                                      : QStringLiteral("  ·  ") + file);
                m_detail->setToolTip(m_scanPath);
            }
            break;
        }
        case Stage::Ready:
            status = tr("Ready");
            detail = tr("Plugins available: %1").arg(m_pluginCount);
            break;
        case Stage::SignIn:
            status = tr("Sign-in required");
            detail = m_stageDetail.isEmpty() ? tr("Sign in to continue.")
                                             : m_stageDetail;
            error = m_stageError;
            break;
    }

    m_status->setText(status);
    m_detail->setText(detail);
    m_detail->setProperty("error", error);
    m_detail->style()->unpolish(m_detail);
    m_detail->style()->polish(m_detail);
    m_progress->setRange(0, indeterminate ? 0 : 1);
    if (!indeterminate) m_progress->setValue(m_stage == Stage::Ready ? 1 : 0);
}

void StartupWindow::applyTheme() {
    const Theme& t = th();
    const QColor border = mixColors(t.separator(), t.textPrimary, 0.10);
    setStyleSheet(QString(R"(
#StartupWindow { background: %1; }
#StartupLogo { background: %5; border: 1px solid %6; border-radius: 15px; }
#StartupTitle { color: %2; }
#StartupProduct, #StartupDetail { color: %3; }
#StartupStatus { color: %2; }
#StartupDetail[error="true"] { color: %4; }
#StartupLoginPanel { background: %5; border: 1px solid %6; border-radius: 10px; }
#StartupLoginPanel QLabel { color: %3; }
#StartupLoginPanel QLineEdit {
    min-height: 28px; color: %2; background: %7;
    border: 1px solid %6; border-radius: 7px; padding: 0 8px;
}
#StartupLoginPanel QLineEdit:focus { border-color: %8; }
#StartupLoginButton {
    min-height: 30px; color: white; background: %8;
    border: 1px solid %8; border-radius: 8px; font-weight: 600;
}
#StartupLoginButton:disabled { color: %3; background: %7; border-color: %6; }
#StartupProgress { background: %7; border: 0; border-radius: 3px; }
#StartupProgress::chunk { background: %8; border-radius: 3px; }
)")
        .arg(t.headerBackground.name(), t.textPrimary.name(),
             t.textSecondary.name(), Theme::record().name(),
             t.surfaceElevated.name(), border.name(), t.well().name(),
             t.accent.name()));
}

bool StartupWindow::checkForTest() {
    const QPixmap logo = m_logo
                             ? m_logo->pixmap(Qt::ReturnByValue)
                             : QPixmap();
    const bool hierarchy = m_logo && m_status && m_detail && m_progress &&
                           m_loginPanel && !logo.isNull() &&
                           !m_logo->accessibleName().isEmpty() &&
                           !m_status->text().isEmpty() &&
                           !m_progress->accessibleName().isEmpty();
    const bool compact = hierarchy &&
                         size() == QSize(kStartupWidth, kCompactHeight) &&
                         m_logo->size() == QSize(76, 76) &&
                         m_loginPanel->isHidden();

    // The same smoke check covers the only taller state: the sign-in panel
    // must expand the dialog, then return cleanly to the loading footprint.
    m_loginPanel->show();
    syncWindowSize();
    if (layout()) layout()->activate();
    const bool loginFits = size() == QSize(kStartupWidth, kLoginHeight) &&
                           layout() && layout()->minimumSize().height() <= height();
    m_loginPanel->hide();
    syncWindowSize();
    return compact && loginFits &&
           size() == QSize(kStartupWidth, kCompactHeight);
}
