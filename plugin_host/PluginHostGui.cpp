#include "PluginProcess.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QResizeEvent>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// Keep the complete editor window in this process. Cross-process native
// parent/owner relationships would join Windows input queues and let a vendor
// GUI hang delay the application's own focus and window operations.
class Editor final : public QWidget, public daw::plugins::PluginEditorHost {
public:
    explicit Editor(daw::plugins::PluginInstance& plugin) : m_plugin(plugin) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_QuitOnClose, false);
        setWindowTitle(QString::fromStdString(plugin.descriptor().name));
        resize(640, 400);
        qApp->installEventFilter(this);
#ifdef Q_OS_WIN
        s_editor = this;
        m_keyHook = SetWindowsHookExW(WH_GETMESSAGE, interceptKey, nullptr, GetCurrentThreadId());
#endif
    }
    bool attach() {
        show();
        if (!m_plugin.openEditor(reinterpret_cast<void*>(winId()), this)) { hide(); return false; }
        m_attached = true;
        std::uint32_t width = 0, height = 0;
        if (m_plugin.editorSize(width, height)) onEditorResized(width, height);
        return true;
    }
    ~Editor() override {
        qApp->removeEventFilter(this);
#ifdef Q_OS_WIN
        if (m_keyHook) UnhookWindowsHookEx(m_keyHook);
        s_editor = nullptr;
#endif
        hide();
        detach();
    }
    bool isAttached() const { return m_attached && !m_closeRequested; }
    void deliverPendingClose() {
#ifdef Q_OS_WIN
        const auto window = reinterpret_cast<HWND>(effectiveWinId());
        MSG message{};
        // The control mailbox can wake before the next GUI pump. Honor an
        // already queued close before deciding whether this view can be reused.
        // Retrieve only this window's close messages, without a general event
        // pump that could start an unrelated vendor dialog before the reply.
        while (PeekMessageW(&message, window, WM_CLOSE, WM_CLOSE, PM_NOREMOVE) &&
               message.hwnd == window) {
            if (!PeekMessageW(&message, window, WM_CLOSE, WM_CLOSE, PM_REMOVE)) break;
            DispatchMessageW(&message);
        }
#endif
    }
    void present() {
        if (isMinimized()) showNormal();
        else show();
        raise();
        activateWindow();
    }
    bool automationShortcutEnabled = false;
    std::uint32_t takeAutomationShortcuts() { return std::exchange(m_automationShortcuts, 0); }
    void detach() {
        if (!m_attached) return;
        m_attached = false;
        m_plugin.closeEditor();
    }
    void onEditorResized(std::uint32_t width, std::uint32_t height) noexcept override {
        if (!width || !height || width > 16384 || height > 16384) return;
        m_resizing = true;
        if (m_plugin.editorCanResize()) {
            // VST3 can ask for a size during attached(), before its adapter
            // has the final view needed to report canResize(). Undo that
            // provisional fixed size once the completed view is resizable.
            setMinimumSize(0, 0);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            resize(int(width), int(height));
        } else setFixedSize(int(width), int(height));
        m_resizing = false;
    }
    void onEditorClosed() noexcept override {
        // Never tear down a view while its callback is on the stack.
        // Mark it now so a queued reopen cannot reuse this retiring view.
        m_closeRequested = true;
        QTimer::singleShot(0, this, [this] { close(); });
    }
    double contentScaleFactor() const noexcept override { return devicePixelRatioF(); }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress &&
            event->type() != QEvent::KeyRelease) return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || (widget != this && !isAncestorOf(widget))) return false;
        for (auto* current = widget; current && current != this; current = current->parentWidget()) {
            const auto* combo = qobject_cast<QComboBox*>(current);
            if (qobject_cast<QLineEdit*>(current) || qobject_cast<QAbstractSpinBox*>(current) ||
                qobject_cast<QTextEdit*>(current) || qobject_cast<QPlainTextEdit*>(current) ||
                (combo && combo->isEditable()) || current->testAttribute(Qt::WA_InputMethodEnabled)) return false;
        }
        auto* key = static_cast<QKeyEvent*>(event);
        const bool a = key->key() == Qt::Key_A || key->key() == 0x0424 ||
            (key->nativeScanCode() & 0xffu) == 0x1e;
        if (!a || (key->modifiers() & ~Qt::KeypadModifier) || !canRouteAutomation()) return false;
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) ++m_automationShortcuts;
        key->accept();
        return true;
    }
    void closeEvent(QCloseEvent* event) override { detach(); event->accept(); }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (!m_attached || m_resizing || !m_plugin.editorCanResize()) return;
        auto width = std::uint32_t(event->size().width());
        auto height = std::uint32_t(event->size().height());
        if (m_plugin.setEditorSize(width, height)) onEditorResized(width, height);
    }
private:
    bool canRouteAutomation() const {
        return automationShortcutEnabled && isVisible() &&
            !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
    }
#ifdef Q_OS_WIN
    static LRESULT CALLBACK interceptKey(int code, WPARAM removed, LPARAM data) {
        auto* editor = s_editor;
        if (editor && code == HC_ACTION && removed == PM_REMOVE && editor->canRouteAutomation()) {
            auto* message = reinterpret_cast<MSG*>(data);
            const bool down = message->message == WM_KEYDOWN;
            const bool up = message->message == WM_KEYUP;
            const auto bits = static_cast<quintptr>(message->lParam);
            const HWND host = reinterpret_cast<HWND>(editor->effectiveWinId());
            if ((down || up) && (message->wParam == 'A' || ((bits >> 16) & 0xff) == 0x1e) &&
                (message->hwnd == host || IsChild(host, message->hwnd)) &&
                !((GetKeyState(VK_SHIFT) | GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) |
                   GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)) {
                wchar_t name[128]{};
                GetClassNameW(message->hwnd, name, 128);
                const QString control = QString::fromWCharArray(name);
                const auto* widget = QWidget::find(WId(message->hwnd));
                if (const auto* focus = QApplication::focusWidget(); focus &&
                    focus->effectiveWinId() == WId(message->hwnd))
                    widget = focus;
                const bool textEntry = control.compare("Edit", Qt::CaseInsensitive) == 0 ||
                    control.startsWith("RichEdit", Qt::CaseInsensitive) ||
                    (widget && widget->testAttribute(Qt::WA_InputMethodEnabled));
                if (!textEntry) {
                    if (down && !(bits & (quintptr(1) << 30))) ++editor->m_automationShortcuts;
                    message->message = WM_NULL;
                }
            }
        }
        return CallNextHookEx(editor ? editor->m_keyHook : nullptr, code, removed, data);
    }
    inline static Editor* s_editor = nullptr;
    HHOOK m_keyHook = nullptr;
#endif
    std::uint32_t m_automationShortcuts = 0;
    daw::plugins::PluginInstance& m_plugin;
    bool m_attached = false, m_resizing = false, m_closeRequested = false;
};

class Gui final : public daw::plugins::PluginHostGui {
public:
    bool open(daw::plugins::PluginInstance& plugin) override {
        if (m_editor) m_editor->deliverPendingClose();
        if (m_editor && m_editor->isAttached()) {
            m_editor->present(); return true;
        }
        close();
        m_editor = std::make_unique<Editor>(plugin);
        m_editor->automationShortcutEnabled = m_automationShortcutEnabled;
        if (!m_editor->attach()) { m_editor.reset(); return false; }
        return true;
    }
    void close() override { m_editor.reset(); }
    void pump() override {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    bool isOpen() const override { return m_editor && m_editor->isAttached(); }
    void setHeartbeat(std::function<void()> heartbeat) override {
        m_heartbeat.stop();
        QObject::disconnect(&m_heartbeat, nullptr, nullptr, nullptr);
        if (!heartbeat) return;
        heartbeat();
        QObject::connect(&m_heartbeat, &QTimer::timeout, &m_heartbeat, std::move(heartbeat));
        m_heartbeat.start(100);
    }
    void setAutomationShortcutEnabled(bool enabled) override {
        m_automationShortcutEnabled = enabled;
        if (m_editor) m_editor->automationShortcutEnabled = enabled;
    }
    std::uint32_t takeAutomationShortcuts() override {
        return m_editor ? m_editor->takeAutomationShortcuts() : 0;
    }
private:
    QTimer m_heartbeat;
    bool m_automationShortcutEnabled = false;
    std::unique_ptr<Editor> m_editor;
};
}

int runPluginHostGui(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    Gui gui;
    return daw::plugins::runPluginHost(argc, argv, &gui);
}
