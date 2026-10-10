#include "NotificationCenter.hpp"
#include "Theme.hpp"
#include <QApplication>
#include <QCursor>
#include <QDynamicPropertyChangeEvent>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

namespace ui {
namespace {
class NotificationCard final : public QFrame {
public:
    using QFrame::QFrame;
protected:
    void paintEvent(QPaintEvent*) override {
        const auto& theme = ThemeManager::instance().theme();
        QPainter painter(this);
        painter.fillRect(rect(), theme.surfaceElevated);
        painter.setPen(theme.gridLineStrong);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};
void prepareOverlay(QWidget* widget) {
    widget->setAttribute(Qt::WA_DontCreateNativeAncestors);
    widget->setAttribute(Qt::WA_NativeWindow);
    widget->setAttribute(Qt::WA_OpaquePaintEvent);
    widget->setProperty("vlt.nativeOverlay", true);
    widget->setProperty("vlt.notificationOverlay", true);
    widget->setFocusPolicy(Qt::NoFocus);
}
}
struct NotificationCenter::Entry {
    Notification data;
    QFrame* card = nullptr;
    QLabel* title = nullptr;
    QLabel* message = nullptr;
    QHBoxLayout* actions = nullptr;
    std::vector<QPushButton*> buttons;
    bool dismissed = false;
    qint64 remainingMs = 5000;
};

NotificationCenter::NotificationCenter(QWidget* surface) : QObject(surface), m_surface(surface) {
    // One native sibling above plugin hosts, with ordinary QWidget cards
    // inside it. Separate native cards multiply exposure work while a plugin
    // moves underneath them and cannot paint the gaps or disclosure opaquely.
    m_layer = new NotificationCard(surface);
    m_layer->setObjectName(QStringLiteral("AudioNotificationLayer"));
    prepareOverlay(m_layer);
    m_layer->hide();
    connect(&ThemeManager::instance(), &ThemeManager::changed, m_layer, [this] { m_layer->update(); });
    m_more = new QToolButton(m_layer);
    m_more->setObjectName(QStringLiteral("MoreAudioNotifications"));
    m_more->setAutoFillBackground(true);
    m_more->setFocusPolicy(Qt::StrongFocus);
    m_more->hide();
    connect(m_more, &QToolButton::clicked, this, [this] {
        QMenu menu(m_surface);
        for (const auto& id : std::as_const(m_order)) {
            const auto* entry = m_entries.value(id);
            if (!entry || entry->dismissed || entry->card->isVisible()) continue;
            connect(menu.addAction(entry->data.title), &QAction::triggered, this, [this, id] {
                m_order.removeAll(id); m_order.prepend(id); arrange();
            });
        }
        menu.exec(m_more->mapToGlobal(QPoint(0, 0)));
    });
    qApp->installEventFilter(this);
    m_expiry.setInterval(100);
    connect(&m_expiry, &QTimer::timeout, this, &NotificationCenter::expire);
}

NotificationCenter::~NotificationCenter() {
    if (qApp) qApp->removeEventFilter(this);
    clear();
    delete m_layer;
}

void NotificationCenter::showNotification(Notification notification, bool reveal) {
    if (!m_surface) return;
    auto* entry = m_entries.value(notification.id);
    if (!entry) {
        entry = new Entry;
        entry->card = new NotificationCard(m_layer);
        entry->card->setObjectName(QStringLiteral("AudioNotificationCard"));
        entry->card->setProperty("notificationId", notification.id);
        entry->card->setAttribute(Qt::WA_OpaquePaintEvent);
        entry->card->setFocusPolicy(Qt::NoFocus);
        auto* layout = new QVBoxLayout(entry->card);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(8);
        auto* header = new QHBoxLayout;
        entry->title = new QLabel(entry->card);
        entry->title->setTextFormat(Qt::PlainText);
        entry->title->setWordWrap(true);
        auto font = entry->title->font(); font.setBold(true); entry->title->setFont(font);
        header->addWidget(entry->title, 1);
        auto* close = new QToolButton(entry->card);
        close->setObjectName(QStringLiteral("DismissAudioNotification"));
        close->setText(QString::fromUtf8("×"));
        close->setAccessibleName(tr("Dismiss notification"));
        close->setToolTip(tr("Dismiss notification"));
        close->setFixedSize(28, 28);
        header->addWidget(close, 0, Qt::AlignTop);
        layout->addLayout(header);
        entry->message = new QLabel(entry->card);
        entry->message->setTextFormat(Qt::PlainText);
        entry->message->setWordWrap(true);
        layout->addWidget(entry->message);
        entry->actions = new QHBoxLayout;
        layout->addLayout(entry->actions);
        const auto id = notification.id;
        connect(close, &QToolButton::clicked, this, [this, id] { dismiss(id); });
        const auto applyTheme = [card = QPointer<QFrame>(entry->card)] {
            if (!card) return;
            const auto& theme = ThemeManager::instance().theme();
            card->setStyleSheet(QStringLiteral(
                "#AudioNotificationCard { background:%1; color:%2; border:1px solid %3; }"
                "#AudioNotificationCard QLabel { color:%2; background:transparent; border:0; }"
                "#AudioNotificationCard QPushButton, #AudioNotificationCard QToolButton { min-height:24px; }")
                .arg(theme.surfaceElevated.name(), theme.textPrimary.name(), theme.gridLineStrong.name()));
        };
        connect(&ThemeManager::instance(), &ThemeManager::changed, entry->card, applyTheme);
        applyTheme();
        m_entries.insert(id, entry);
        m_order.prepend(id);
    }
    const bool newIncident = entry->data.incident != notification.incident;
    if (reveal || newIncident) entry->dismissed = false;
    const bool newlyResolved = notification.resolved && (!entry->data.resolved || newIncident);
    entry->data = std::move(notification);
    entry->title->setText(entry->data.title);
    entry->message->setText(entry->data.message);
    entry->card->setToolTip(entry->data.detail);
    entry->card->setAccessibleName(entry->data.title);
    entry->card->setAccessibleDescription(entry->data.message);
    while (entry->buttons.size() > entry->data.actions.size()) {
        auto* button = entry->buttons.back();
        entry->buttons.pop_back();
        entry->actions->removeWidget(button);
        button->hide(); button->deleteLater();
    }
    for (std::size_t index = 0; index < entry->data.actions.size(); ++index) {
        const auto& action = entry->data.actions[index];
        if (index == entry->buttons.size()) {
            auto* button = new QPushButton(entry->card);
            button->setMinimumHeight(28);
            button->setAutoDefault(false);
            connect(button, &QPushButton::clicked, this, [this, id = entry->data.id, index] {
                const auto* current = m_entries.value(id);
                if (!current || index >= current->data.actions.size()) return;
                // Invoking an action can update or remove its own card.
                const auto invoke = current->data.actions[index].invoke;
                if (invoke) invoke();
            });
            entry->actions->addWidget(button);
            entry->buttons.push_back(button);
        }
        auto* button = entry->buttons[index];
        button->setText(action.label);
        button->setAccessibleName(action.label);
        // A new child of an already visible card otherwise stays hidden until
        // Qt's queued show event; measuring now would omit its height.
        button->show();
    }
    entry->card->ensurePolished();
    entry->card->layout()->invalidate();
    if (newlyResolved) entry->remainingMs = 5000;
    if (entry->data.resolved && !m_expiry.isActive()) { m_clock.start(); m_expiry.start(); }
    arrange();
}

void NotificationCenter::dismiss(const QString& id) {
    if (auto* entry = m_entries.value(id)) { entry->dismissed = true; entry->card->hide(); }
    arrange();
}
bool NotificationCenter::dismissed(const QString& id) const {
    const auto* entry = m_entries.value(id); return entry && entry->dismissed;
}
void NotificationCenter::remove(const QString& id) {
    if (auto* entry = m_entries.take(id)) {
        entry->card->hide(); entry->card->deleteLater(); delete entry;
    }
    m_order.removeAll(id);
    arrange();
}
void NotificationCenter::clear() {
    for (auto* entry : std::as_const(m_entries)) {
        delete entry->card; delete entry;
    }
    m_entries.clear(); m_order.clear(); m_expiry.stop();
    if (m_more) m_more->hide();
    if (m_layer) m_layer->hide();
}
int NotificationCenter::visibleCount() const {
    int count = 0;
    for (const auto* entry : m_entries) count += entry->card->isVisible();
    return count;
}
void NotificationCenter::arrange() {
    if (!m_surface) return;
    const int preferredWidth = std::max(420, m_surface->fontMetrics().horizontalAdvance(QStringLiteral("M")) * 42);
    const int width = std::min(preferredWidth, std::max(1, m_surface->width() - 32));
    const int x = std::max(0, m_surface->width() - width - 16);
    int remaining = 0;
    for (const auto* entry : m_entries) remaining += !entry->dismissed;
    int capacity = 0, neededHeight = 0;
    for (const auto& id : std::as_const(m_order)) {
        auto* entry = m_entries.value(id);
        if (!entry || entry->dismissed) continue;
        entry->card->setFixedWidth(width);
        const int height = std::max(entry->card->layout()->totalHeightForWidth(width), entry->card->minimumSizeHint().height());
        if (capacity && (capacity == 3 || neededHeight + height + 8 > m_surface->height() - 70)) break;
        neededHeight += height + 8; ++capacity;
    }
    m_more->setVisible(remaining > capacity);
    const int height = std::max(0, neededHeight - (capacity ? 8 : 0)) + (remaining > capacity ? 38 : 0);
    m_layer->setGeometry(x, std::max(0, m_surface->height() - height - 16), width, height);
    int bottom = height;
    if (remaining > capacity) {
        m_more->setText(tr("More notifications (%1)").arg(remaining - capacity));
        m_more->setAccessibleName(m_more->text());
        m_more->setGeometry(0, bottom - 30, width, 30);
        bottom -= 38;
    }
    int visible = 0;
    for (const auto& id : std::as_const(m_order)) {
        auto* entry = m_entries.value(id);
        if (!entry) continue;
        const bool show = !entry->dismissed && visible < capacity;
        if (show) {
            entry->card->setFixedWidth(width);
            const int height = std::max(entry->card->layout()->totalHeightForWidth(width), entry->card->minimumSizeHint().height());
            entry->card->setGeometry(0, std::max(0, bottom - height), width, height);
            bottom -= height + 8; ++visible;
        }
        entry->card->setVisible(show);
    }
    m_layer->setVisible(capacity > 0);
    raiseCards();
}
void NotificationCenter::raiseCards() {
    if (m_raising) return;
    m_raising = true;
    const auto raise = [](QWidget* widget) {
        if (!widget->isVisible()) return;
        widget->raise();
        if (widget->windowHandle()) widget->windowHandle()->raise();
    };
    raise(m_layer);
    m_raising = false;
}
bool NotificationCenter::eventFilter(QObject* object, QEvent* event) {
    switch (event->type()) {
    case QEvent::Resize:
        if (object == m_surface) arrange();
        break;
    case QEvent::DynamicPropertyChange:
        if (static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() != "vlt.nativeOverlay") break;
        [[fallthrough]];
    case QEvent::Show:
        if (object == m_surface) arrange();
        [[fallthrough]];
    case QEvent::ZOrderChange:
        if (!m_raising && m_layer->isVisible()) {
            const auto* widget = qobject_cast<QWidget*>(object);
            if (widget && widget->property("vlt.nativeOverlay").toBool() &&
                !widget->property("vlt.notificationOverlay").toBool() && m_surface && m_surface->isAncestorOf(widget) &&
                !m_raisePending) {
                // The editor also raises its native QWindow after QWidget::raise
                // returns. Restore our layer after that complete operation.
                m_raisePending = true;
                QTimer::singleShot(0, this, [this] {
                    m_raisePending = false;
                    raiseCards();
                });
            }
        }
        break;
    default: break;
    }
    return false;
}
void NotificationCenter::expire() {
    const auto elapsed = m_clock.restart();
    bool pending = false;
    QStringList expired;
    for (const auto& id : std::as_const(m_order)) {
        auto* entry = m_entries.value(id);
        if (!entry || !entry->data.resolved) continue;
        pending = true;
        const auto* focus = QApplication::focusWidget();
        // Native child surfaces may deliver Enter/Leave through a different
        // window; use the pointer position for the complete card and buttons.
        const bool hovered = entry->card->rect().contains(entry->card->mapFromGlobal(QCursor::pos()));
        if (entry->card->isVisible() && !hovered &&
            !(focus && (focus == entry->card || entry->card->isAncestorOf(focus))))
            entry->remainingMs -= elapsed;
        if (entry->dismissed || entry->remainingMs <= 0) expired.push_back(id);
    }
    for (const auto& id : expired) remove(id);
    if (!pending) m_expiry.stop();
}
}
