#include "AuthWindow.hpp"

#include "AccountService.hpp"

#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

AuthWindow::AuthWindow(account::Service* service, QWidget* parent)
    : QDialog(parent), m_service(service) {
    setWindowTitle(QStringLiteral("VLT Studio Pro — Account"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setModal(true);
    setMinimumWidth(440);
    setMaximumWidth(560);
    m_locale = QLocale::system().language() == QLocale::Russian
                   ? QStringLiteral("ru") : QStringLiteral("en");

    auto* header = new QHBoxLayout;
    auto* brand = new QLabel(QStringLiteral("VLT  /  STUDIO PRO"), this);
    QFont brandFont = brand->font();
    brandFont.setBold(true);
    brandFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    brand->setFont(brandFont);
    header->addWidget(brand);
    header->addStretch(1);
    m_language = new QPushButton(this);
    m_language->setFlat(true);
    connect(m_language, &QPushButton::clicked, this, [this] {
        m_locale = m_locale == QLatin1String("ru") ? QStringLiteral("en")
                                                   : QStringLiteral("ru");
        updateTexts();
    });
    header->addWidget(m_language);

    m_title = new QLabel(this);
    QFont titleFont = m_title->font();
    titleFont.setPointSize(titleFont.pointSize() + 7);
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_copy = new QLabel(this);
    m_copy->setWordWrap(true);

    m_email = new QLineEdit(this);
    m_email->setInputMethodHints(Qt::ImhEmailCharactersOnly);
    m_email->setClearButtonEnabled(true);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    connect(m_password, &QLineEdit::returnPressed, this, &AuthWindow::submit);
    m_emailLabel = new QLabel(this);
    m_passwordLabel = new QLabel(this);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(m_emailLabel, m_email);
    form->addRow(m_passwordLabel, m_password);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->hide();
    m_login = new QPushButton(this);
    m_login->setDefault(true);
    connect(m_login, &QPushButton::clicked, this, &AuthWindow::submit);
    m_register = new QPushButton(this);
    m_register->setFlat(true);
    connect(m_register, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("register")); });
    m_reset = new QPushButton(this);
    m_reset->setFlat(true);
    connect(m_reset, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("forgot-password")); });
    auto* links = new QHBoxLayout;
    links->addWidget(m_register);
    links->addStretch(1);
    links->addWidget(m_reset);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(28, 24, 28, 26);
    column->setSpacing(16);
    column->addLayout(header);
    column->addSpacing(8);
    column->addWidget(m_title);
    column->addWidget(m_copy);
    column->addLayout(form);
    column->addWidget(m_status);
    column->addWidget(m_login);
    column->addLayout(links);

    connect(service, &account::Service::busyChanged, this, [this](bool busy) {
        m_login->setDisabled(busy);
        m_email->setDisabled(busy);
        m_password->setDisabled(busy);
        if (busy) setStatus(m_locale == QLatin1String("ru")
                                ? QStringLiteral("Проверяем аккаунт и подписку…")
                                : QStringLiteral("Checking account and entitlement…"), false);
    });
    connect(service, &account::Service::authenticatedChanged, this,
            [this](bool authenticated) { if (authenticated) accept(); });
    connect(service, &account::Service::authenticationRequired, this,
            [this](const QString& reason, bool) { setStatus(reason, true); });
    connect(service, &account::Service::errorOccurred, this,
            [this](const QString& code, const QString& message) {
                QString detail = message;
                const bool ru = m_locale == QLatin1String("ru");
                if (ru) {
                    if (code == QLatin1String("network_unavailable")) {
                        detail = QStringLiteral(
                            "Не удалось подключиться к серверу аккаунтов (%1). "
                            "Проверьте интернет-соединение и повторите вход.")
                                     .arg(m_service->apiOrigin());
                    } else if (code == QLatin1String("invalid_server_response")) {
                        detail = QStringLiteral(
                            "Сервер аккаунтов вернул некорректный ответ. "
                            "Повторите попытку чуть позже.");
                    } else if (code == QLatin1String("invalid_credentials")) {
                        detail = QStringLiteral("Неверная почта или пароль.");
                    } else if (code == QLatin1String("login_rate_limited")) {
                        detail = QStringLiteral(
                            "Слишком много попыток входа. Попробуйте позднее.");
                    } else if (code == QLatin1String("account_suspended")) {
                        detail = QStringLiteral("Этот аккаунт заблокирован.");
                    } else if (code == QLatin1String("validation_failed")) {
                        detail = QStringLiteral(
                            "Приложение отправило некорректные данные устройства.");
                    } else if (code == QLatin1String("device_limit_reached")) {
                        detail = QStringLiteral(
                            "Достигнут лимит из двух устройств. Отзовите старое "
                            "устройство в кабинете на сайте.");
                    }
                } else if (code == QLatin1String("device_limit_reached")) {
                    detail += QStringLiteral(
                        " Revoke an old device in your web account.");
                }
                setStatus(detail, true);
            });
    updateTexts();
}

void AuthWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (m_restoreStarted) return;
    m_restoreStarted = true;
    m_service->beginRestore();
}

void AuthWindow::submit() {
    if (m_email->text().trimmed().isEmpty() || m_password->text().size() < 12) {
        setStatus(m_locale == QLatin1String("ru")
                      ? QStringLiteral("Введите почту и пароль не короче 12 символов.")
                      : QStringLiteral("Enter your email and a password of at least 12 characters."), true);
        return;
    }
    m_service->login(m_email->text(), m_password->text());
}

void AuthWindow::updateTexts() {
    const bool ru = m_locale == QLatin1String("ru");
    m_title->setText(ru ? QStringLiteral("Войдите, чтобы продолжить")
                        : QStringLiteral("Sign in to continue"));
    m_copy->setText(ru
        ? QStringLiteral("VLT Studio Pro проверит Demo-доступ. После успешного входа программа сможет работать без сети до 72 часов.")
        : QStringLiteral("VLT Studio Pro will verify Demo access. After a successful sign-in, the app can work offline for up to 72 hours."));
    m_emailLabel->setText(ru ? QStringLiteral("Почта") : QStringLiteral("Email"));
    m_passwordLabel->setText(ru ? QStringLiteral("Пароль") : QStringLiteral("Password"));
    m_login->setText(ru ? QStringLiteral("Войти") : QStringLiteral("Sign in"));
    m_register->setText(ru ? QStringLiteral("Создать аккаунт на сайте")
                           : QStringLiteral("Create an account on the website"));
    m_reset->setText(ru ? QStringLiteral("Восстановить пароль")
                        : QStringLiteral("Reset password"));
    m_language->setText(ru ? QStringLiteral("EN") : QStringLiteral("RU"));
}

void AuthWindow::setStatus(const QString& message, bool error) {
    m_status->setText(message);
    m_status->setStyleSheet(error
        ? QStringLiteral("QLabel { color: #ff9ba0; padding: 8px; border: 1px solid #8e4549; }")
        : QStringLiteral("QLabel { color: #8ed7e8; padding: 8px; border: 1px solid #339eb8; }"));
    m_status->show();
}

void AuthWindow::openSite(const QString& path) {
    QString origin = qEnvironmentVariable("VLT_PUBLIC_ORIGIN", QStringLiteral("http://localhost:3000"));
    while (origin.endsWith('/')) origin.chop(1);
    QDesktopServices::openUrl(QUrl(origin + QLatin1Char('/') + m_locale + QLatin1Char('/') + path));
}
