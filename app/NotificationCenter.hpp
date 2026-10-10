#pragma once
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QHash>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <vector>

class QWidget;
class QToolButton;
namespace ui {
struct Notification {
    struct Action { QString label; std::function<void()> invoke; };
    QString id, title, message, detail;
    quint64 incident = 0;
    bool resolved = false;
    std::vector<Action> actions;
};

/// Compact application-owned cards; no desktop notification or focus activation.
class NotificationCenter final : public QObject {
    Q_OBJECT
public:
    explicit NotificationCenter(QWidget* surface);
    ~NotificationCenter() override;
    void showNotification(Notification notification, bool reveal = false);
    void remove(const QString& id);
    void clear();
    int visibleCount() const;
    bool dismissed(const QString& id) const;
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    struct Entry;
    void dismiss(const QString& id);
    void arrange();
    void raiseCards();
    void expire();
    QPointer<QWidget> m_surface;
    QWidget* m_layer = nullptr;
    QHash<QString, Entry*> m_entries;
    QStringList m_order;
    QToolButton* m_more = nullptr;
    QTimer m_expiry;
    QElapsedTimer m_clock;
    bool m_raising = false;
    bool m_raisePending = false;
};
}
