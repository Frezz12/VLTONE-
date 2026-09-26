#include "StartupWindow.hpp"

#include "AccountService.hpp"
#include "LocalizationManager.hpp"
#include "Theme.hpp"

#include <algorithm>
#include <limits>
#include <QApplication>
#include <QDesktopServices>
#include <QDebug>
#include <QEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace {

constexpr int kStartupWidth = 440;
constexpr int kCompactHeight = 224;
constexpr int kLoginHeight = 480;
constexpr int kContentMargin = 28;

} // namespace

StartupWindow::StartupWindow(account::Service* service, QWidget* parent)
    : QDialog(parent), m_service(service) {
    setObjectName(QStringLiteral("StartupWindow"));
    setWindowTitle(QStringLiteral("VLTONE"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setFixedSize(kStartupWidth, kCompactHeight);

    m_logo = new QLabel(this);
    m_logo->setObjectName(QStringLiteral("StartupLogo"));
    m_logo->setFixedSize(52, 52);
    m_logo->setAlignment(Qt::AlignCenter);
    m_logo->setAccessibleName(tr("VLTONE logo"));
    const QPixmap logo(QStringLiteral(":/vlt/icon-1024.png"));
    if (!logo.isNull()) {
        const qreal dpr = devicePixelRatioF();
        QPixmap scaled = logo.scaled(QSize(qRound(52 * dpr), qRound(52 * dpr)),
                                     Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        m_logo->setPixmap(scaled);
    }

    auto* title = new QLabel(QStringLiteral("VLTONE"), this);
    title->setObjectName(QStringLiteral("StartupTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(24);
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    title->setFont(titleFont);

    m_product = new QLabel(tr("Digital audio workstation"), this);
    m_product->setObjectName(QStringLiteral("StartupProduct"));
    QFont captionFont = m_product->font();
    captionFont.setPixelSize(12);
    m_product->setFont(captionFont);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("StartupStatus"));
    QFont statusFont = m_status->font();
    statusFont.setPixelSize(13);
    statusFont.setWeight(QFont::DemiBold);
    m_status->setFont(statusFont);

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("StartupDetail"));
    m_detail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_detail->setTextFormat(Qt::PlainText);
    m_detail->setFont(captionFont);
    m_detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_detail->setFixedHeight(32);

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("StartupCount"));
    m_count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_count->setFont(captionFont);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("StartupProgress"));
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->setAccessibleName(tr("Application startup progress"));
    auto progressPolicy = m_progress->sizePolicy();
    progressPolicy.setRetainSizeWhenHidden(true);
    m_progress->setSizePolicy(progressPolicy);

    m_loginPanel = new QFrame(this);
    m_loginPanel->setObjectName(QStringLiteral("StartupLoginPanel"));
    auto* loginColumn = new QVBoxLayout(m_loginPanel);
    loginColumn->setContentsMargins(0, 16, 0, 0);
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
    m_emailLabel->setBuddy(m_email);
    m_passwordLabel->setBuddy(m_password);
    form->addRow(m_emailLabel, m_email);
    form->addRow(m_passwordLabel, m_password);
    loginColumn->addLayout(form);

    m_login = new QPushButton(tr("Sign in"), m_loginPanel);
    m_login->setObjectName(QStringLiteral("StartupLoginButton"));
    m_login->setDefault(true);
    connect(m_login, &QPushButton::clicked, this, &StartupWindow::submit);
    loginColumn->addWidget(m_login);
    m_restore = new QPushButton(m_loginPanel);
    m_restore->setObjectName(QStringLiteral("StartupRestoreButton"));
    m_restore->setAutoDefault(false);
    connect(m_restore, &QPushButton::clicked, service, &account::Service::restoreSavedSession);
    loginColumn->addWidget(m_restore);

    auto* links = new QHBoxLayout;
    links->setContentsMargins(0, 0, 0, 0);
    m_register = new QPushButton(tr("Create account"), m_loginPanel);
    m_register->setFlat(true);
    m_register->setAutoDefault(false);
    connect(m_register, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("register")); });
    m_reset = new QPushButton(tr("Forgot password?"), m_loginPanel);
    m_reset->setFlat(true);
    m_reset->setAutoDefault(false);
    connect(m_reset, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("forgot-password")); });
    links->addWidget(m_register);
    links->addStretch(1);
    links->addWidget(m_reset);
    loginColumn->addLayout(links);
    m_loginPanel->hide();

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(kContentMargin, 28, kContentMargin, 24);
    column->setSpacing(0);
    auto* brand = new QHBoxLayout;
    brand->setSpacing(14);
    brand->addWidget(m_logo);
    auto* wordmark = new QVBoxLayout;
    wordmark->setSpacing(2);
    wordmark->addWidget(title);
    wordmark->addWidget(m_product);
    brand->addLayout(wordmark, 1);
    column->addLayout(brand);
    column->addSpacing(30);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(12);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_count);
    column->addLayout(statusRow);
    column->addSpacing(12);
    column->addWidget(m_progress);
    column->addSpacing(10);
    column->addWidget(m_detail);
    column->addWidget(m_loginPanel);
    column->addStretch(1);

    connect(service, &account::Service::busyChanged, this, [this](bool busy) {
        m_login->setDisabled(busy);
        m_restore->setDisabled(busy);
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
                m_stage = Stage::LicenseConfirmed;
                m_stageDetail = m_service->snapshot().email;
                renderStage();
            });
    connect(service, &account::Service::authenticationRequired, this,
            [this](const QString& reason, bool) {
                revealLogin(reason, !reason.isEmpty() && reason != tr("Sign in to continue."));
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
    m_stage = Stage::LoadingSystem;
    renderStage();
}

void StartupWindow::showPluginScan(std::uint32_t done, std::uint32_t total,
                                   const QString& currentPath) {
    m_loginPanel->hide();
    m_stage = Stage::PluginScan;
    m_scanDone = done;
    m_scanTotal = total;
    m_scanPath = currentPath;
    renderStage();
}

void StartupWindow::showReady(int pluginCount) {
    m_loginPanel->hide();
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
    if (m_email->text().trimmed().isEmpty() || m_password->text().size() < 8) {
        setStatusError(
            tr("Enter your email and a password of at least 8 characters."));
        return;
    }
    m_service->login(m_email->text(), m_password->text());
}

void StartupWindow::revealLogin(const QString& reason, bool error) {
    m_stage = Stage::SignIn;
    m_stageDetail = error ? reason : QString();
    m_stageError = error;
    m_loginPanel->show();
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
                           ? kLoginHeight + std::max(0, m_detail->height() - 54)
                           : kCompactHeight);
    if (size() == target) return;

    // Expanding the sign-in form should not make the dialog jump down and to
    // the right. Preserve the visual centre once the window is on screen.
    const QPoint centre = frameGeometry().center();
    setFixedSize(target);
    if (isVisible()) move(centre - rect().center());
}

void StartupWindow::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() != QEvent::LanguageChange) return;
    retranslateUi();
    renderStage();
}

void StartupWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Theme& t = th();
    QLinearGradient surface(0, 0, 0, height());
    // The lighter transport preset must not restyle the startup window.
    const QColor background = t.id == QLatin1String("dark")
        ? t.transportBackground : t.headerBackground;
    surface.setColorAt(0, mixColors(background, t.surfaceElevated, 0.25));
    surface.setColorAt(1, background);
    painter.setBrush(surface);
    painter.setPen(mixColors(t.separator(), t.textPrimary, 0.12));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            Theme::cornerRadius, Theme::cornerRadius);
}

void StartupWindow::mousePressEvent(QMouseEvent* event) {
    // The brand area replaces the native title bar; let the OS own dragging.
    if (event->button() == Qt::LeftButton && event->position().y() < 100 &&
        windowHandle() && windowHandle()->startSystemMove()) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void StartupWindow::retranslateUi() {
    m_logo->setAccessibleName(tr("VLTONE logo"));
    m_product->setText(tr("Digital audio workstation"));
    m_progress->setAccessibleName(tr("Application startup progress"));
    m_email->setAccessibleName(tr("Email"));
    m_password->setAccessibleName(tr("Password"));
    m_emailLabel->setText(tr("Email"));
    m_passwordLabel->setText(tr("Password"));
    m_login->setText(tr("Sign in"));
    m_restore->setText(tr("Restore saved sign-in"));
    m_register->setText(tr("Create account"));
    m_reset->setText(tr("Forgot password?"));
}

void StartupWindow::renderStage() {
    QString status;
    QString detail;
    bool indeterminate = false;
    bool error = false;
    QString count;
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
                count = QStringLiteral("%1 / %2")
                            .arg(std::min(m_scanDone, m_scanTotal)).arg(m_scanTotal);
                detail = file.isEmpty() ? tr("Looking for installed plugins") : file;
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
    m_count->setText(count);
    m_detail->setAccessibleName(detail);
    const bool signingIn = !m_loginPanel->isHidden();
    const int textWidth = kStartupWidth - 2 * kContentMargin;
    m_detail->setWordWrap(signingIn);
    m_detail->setFixedHeight(signingIn
        ? std::max(32, m_detail->fontMetrics().boundingRect(
              QRect(0, 0, textWidth, 0), Qt::TextWordWrap, detail).height())
        : 32);
    m_detail->setText(signingIn ? detail : m_detail->fontMetrics().elidedText(
                                             detail, Qt::ElideMiddle, textWidth));
    if (m_detail->toolTip().isEmpty() && m_detail->text() != detail)
        m_detail->setToolTip(detail);
    if (m_detail->property("error").toBool() != error) {
        m_detail->setProperty("error", error);
        m_detail->style()->unpolish(m_detail);
        m_detail->style()->polish(m_detail);
    }
    m_progress->setVisible(m_stage != Stage::SignIn);
    if (m_stage == Stage::PluginScan && m_scanTotal > 0) {
        const int maximum = static_cast<int>(std::min<std::uint32_t>(
            m_scanTotal, std::numeric_limits<int>::max()));
        m_progress->setRange(0, maximum);
        m_progress->setValue(static_cast<int>(
            std::uint64_t(std::min(m_scanDone, m_scanTotal)) * maximum / m_scanTotal));
    } else {
        m_progress->setRange(0, indeterminate ? 0 : 1);
        if (!indeterminate)
            m_progress->setValue(m_stage == Stage::Ready ||
                                m_stage == Stage::LicenseConfirmed ? 1 : 0);
    }
    m_progress->setAccessibleDescription(status + QLatin1Char(' ') + count);
    syncWindowSize();
}

void StartupWindow::applyTheme() {
    const Theme& t = th();
    const QColor border = mixColors(t.separator(), t.textPrimary, 0.10);
    setStyleSheet(QString(R"(
#StartupWindow, #StartupWindow QLabel { background: transparent; }
#StartupTitle { color: %1; font-size: 24px; font-weight: 600; }
#StartupProduct, #StartupDetail, #StartupCount { color: %2; }
#StartupStatus { color: %1; font-size: 13px; font-weight: 600; }
#StartupDetail[error="true"] { color: %3; }
#StartupLoginPanel { background: transparent; border: 0; border-top: 1px solid %5; }
#StartupLoginPanel QLabel { color: %2; }
#StartupLoginPanel QLineEdit {
    min-height: 32px; color: %1; background: %6;
    border: 1px solid %5; border-radius: %RADIUS%px; padding: 0 8px;
}
#StartupLoginPanel QLineEdit:focus { border-color: %7; }
#StartupLoginPanel QPushButton { min-height: 28px; }
#StartupRestoreButton { color: %1; background: %4; border: 1px solid %5; border-radius: %RADIUS%px; }
#StartupRestoreButton:hover { border-color: %7; }
#StartupRestoreButton:focus { border-color: %7; }
#StartupLoginPanel QPushButton:flat { color: %2; background: transparent; border: 1px solid transparent; border-radius: %RADIUS%px; padding: 0 4px; }
#StartupLoginPanel QPushButton:flat:hover { color: %1; background: %4; }
#StartupLoginPanel QPushButton:flat:focus { border-color: %7; }
#StartupLoginButton {
    min-height: 34px; color: %ACCENT_TEXT%; background: %7;
    border: 1px solid %7; border-radius: %RADIUS%px; font-weight: 600;
}
#StartupLoginButton:hover, #StartupLoginButton:focus { background: %8; border-color: %8; }
#StartupLoginButton:disabled { color: %2; background: %6; border-color: %5; }
#StartupProgress { background: %6; border: 0; border-radius: 3px; }
#StartupProgress::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %7, stop:1 %8); border-radius: 3px; }
)").replace("%RADIUS%", QString::number(Theme::cornerRadius))
        .replace("%ACCENT_TEXT%", t.accentText().name())
        .arg(t.textPrimary.name(),
             t.textSecondary.name(), Theme::record().name(),
             t.surfaceElevated.name(), border.name(), t.well().name(),
             t.accent.name(), t.accentHighlight.name()));
    update();
}

bool StartupWindow::checkForTest() {
    // Deliver real resize/layout events without displaying a test window.
    setAttribute(Qt::WA_DontShowOnScreen);
    show();
    const QPoint centre = frameGeometry().center();
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
                         m_logo->size() == QSize(52, 52) &&
                         m_loginPanel->isHidden() &&
                         !findChild<QWidget*>(QStringLiteral("StartupLanguage"));

    showPluginScan(3, 8, QStringLiteral("/Plugins/Example.vst3"));
    layout()->activate();
    const QRect progressRect = m_progress->geometry();
    const QRect detailRect = m_detail->geometry();
    const bool progressTracksScan = m_progress->minimum() == 0 &&
        m_progress->maximum() == 8 && m_progress->value() == 3 &&
        m_count->text() == QStringLiteral("3 / 8");
    const QString longPath = QStringLiteral("/Plugins/") +
        QStringLiteral("Long plugin name ").repeated(20) + QStringLiteral(".vst3");
    showPluginScan(7, 8, longPath);
    layout()->activate();
    const bool stableLayout = m_progress->geometry() == progressRect &&
        m_detail->geometry() == detailRect && size() == QSize(kStartupWidth, kCompactHeight) &&
        m_detail->toolTip() == longPath &&
        m_detail->accessibleName() == QFileInfo(longPath).fileName() &&
        m_detail->fontMetrics().horizontalAdvance(m_detail->text()) <= m_detail->width();
    showPluginScan(9, 8, {});
    const bool completeScan = m_progress->value() == m_progress->maximum();
    showPluginScan(0, 0, {});
    const bool discovery = m_progress->minimum() == 0 && m_progress->maximum() == 0 &&
                           m_count->text().isEmpty();

    revealLogin({}, false);
    layout()->activate();
    m_loginPanel->layout()->activate();
    const bool loginFits = size() == QSize(kStartupWidth, kLoginHeight) &&
        layout()->minimumSize().height() <= height() && m_progress->isHidden() &&
        m_email->width() >= 180 && windowFlags().testFlag(Qt::FramelessWindowHint) &&
        frameGeometry().center() == centre;
    submit(); // Empty credentials must report the local error without a request.
    layout()->activate();
    const bool errorFits = m_detail->property("error").toBool() &&
        layout()->minimumSize().height() <= height() &&
        m_detail->height() >= m_detail->fontMetrics().boundingRect(
            QRect(0, 0, m_detail->width(), 0), Qt::TextWordWrap, m_detail->text()).height();
    showSystemLoading();
    const bool reset = m_loginPanel->isHidden() && !m_progress->isHidden() &&
        size() == QSize(kStartupWidth, kCompactHeight) &&
        !m_detail->property("error").toBool() && m_count->text().isEmpty() &&
        frameGeometry().center() == centre;
    showReady(8);
    const bool ready = m_progress->maximum() == 1 && m_progress->value() == 1;
    hide();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(this, &escape);
    const bool passed = compact && progressTracksScan && stableLayout && completeScan &&
                        discovery && loginFits && errorFits && reset && ready && cancelled();
    if (!passed)
        qWarning() << "startup:" << compact << progressTracksScan << stableLayout
                   << completeScan << discovery << loginFits << errorFits << reset << ready
                   << "size" << size() << "minimum" << layout()->minimumSize()
                   << "email width" << m_email->width();
    return passed;
}
