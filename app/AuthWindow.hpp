#pragma once

#include <QDialog>

namespace account { class Service; }
class QLabel;
class QLineEdit;
class QPushButton;

class AuthWindow final : public QDialog {
    Q_OBJECT
public:
    explicit AuthWindow(account::Service* service, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void submit();
    void updateTexts();
    void setStatus(const QString& message, bool error);
    void openSite(const QString& path);

    account::Service* m_service = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_copy = nullptr;
    QLabel* m_emailLabel = nullptr;
    QLabel* m_passwordLabel = nullptr;
    QLabel* m_status = nullptr;
    QLineEdit* m_email = nullptr;
    QLineEdit* m_password = nullptr;
    QPushButton* m_login = nullptr;
    QPushButton* m_register = nullptr;
    QPushButton* m_reset = nullptr;
    QPushButton* m_language = nullptr;
    QString m_locale;
    bool m_restoreStarted = false;
};
