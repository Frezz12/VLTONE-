#include "PluginStyle.hpp"
#include "PluginEditorWindow.hpp"

#include "Controls.hpp"
#include "EqualizerPanel.hpp"
#include "CompressorPanel.hpp"
#include "Cla2aPanel.hpp"
#include "DelayPanel.hpp"
#include "GraphitPanel.hpp"
#include "PitchCorrectorPanel.hpp"
#include "ModulationPanel.hpp"
#include "GravityPanel.hpp"
#include "InternalEditorFrame.hpp"
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"
#include <QPushButton>
#include <QDir>
#include "SamplerPanel.hpp"
#include "SlicerPanel.hpp"
#include "Theme.hpp"
#include "graphics/ScenePaintSource.hpp"
#if defined(Q_OS_MACOS)
#include "PluginEditorWindowMac.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <QCloseEvent>
#include <QEvent>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QHideEvent>
#include <cstdio>
#include <cstdlib>
#include <QApplication>
#include <QGridLayout>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QStyleOption>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QFormLayout>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Vendor HWNDs never deliver QWidget key events. A hook on this UI thread
// catches them before TranslateMessage, including vendor nested message loops.
// Only descendants of our live plugin containers participate; this is not a
// system-wide keyboard hook. See Win32 GetMsgProc (PM_REMOVE / WM_NULL).
class PluginEditorNativeKeyboard {
public:
    explicit PluginEditorNativeKeyboard(PluginEditorWindow* editor) {
#ifdef Q_OS_WIN
        m_editor = editor;
        editors().append(editor);
        if (!hook()) hook() = SetWindowsHookExW(WH_GETMESSAGE, intercept, nullptr,
                                              GetCurrentThreadId());
#endif
    }
    ~PluginEditorNativeKeyboard() {
#ifdef Q_OS_WIN
        editors().removeAll(m_editor);
        if (editors().isEmpty() && hook()) {
            UnhookWindowsHookEx(hook());
            hook() = nullptr;
        }
#endif
    }
private:
#ifdef Q_OS_WIN
    static QList<PluginEditorWindow*>& editors() {
        static QList<PluginEditorWindow*> list;
        return list;
    }
    static HHOOK& hook() { static HHOOK value = nullptr; return value; }
    static LRESULT CALLBACK intercept(int code, WPARAM removed, LPARAM data) {
        if (code != HC_ACTION || removed != PM_REMOVE)
            return CallNextHookEx(hook(), code, removed, data);
        auto* message = reinterpret_cast<MSG*>(data);
        const bool down = message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN;
        const bool up = message->message == WM_KEYUP || message->message == WM_SYSKEYUP;
        const bool click = message->message == WM_LBUTTONDOWN || message->message == WM_RBUTTONDOWN;
        if (down || up || click) {
            for (auto* editor : editors()) {
                if (editor->m_closing || !editor->isVisible() || !editor->m_container) continue;
                const HWND host = reinterpret_cast<HWND>(editor->m_container->effectiveWinId());
                if (!host || (host != message->hwnd && !IsChild(host, message->hwnd))) continue;
                if (click) {
                    emit editor->keyboardFocusReceived();
                    break;
                }
                Qt::KeyboardModifiers modifiers;
                if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= Qt::ShiftModifier;
                if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= Qt::ControlModifier;
                if (GetKeyState(VK_MENU) & 0x8000) modifiers |= Qt::AltModifier;
                if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
                    modifiers |= Qt::MetaModifier;
                const auto bits = static_cast<quintptr>(message->lParam);
                const int key = message->wParam == VK_SPACE ? Qt::Key_Space : int(message->wParam);
                QKeyEvent event(down ? QEvent::KeyPress : QEvent::KeyRelease,
                                key, modifiers, (bits >> 16) & 0x1ff,
                                quint32(message->wParam), 0, QString(),
                                down && (bits & (quintptr(1) << 30)));
                wchar_t name[128]{};
                GetClassNameW(message->hwnd, name, 128);
                const QString control = QString::fromWCharArray(name);
                const auto* nativeWidget = QWidget::find(WId(message->hwnd));
                // Alien Qt children share their parent's HWND. Inspect the
                // focused child only when this message is addressed to that
                // same native surface; a vendor HWND can retain stale Qt focus.
                if (const auto* focus = QApplication::focusWidget(); focus &&
                    focus->effectiveWinId() == WId(message->hwnd))
                    nativeWidget = focus;
                const bool textEntry = control.compare(QStringLiteral("Edit"), Qt::CaseInsensitive) == 0 ||
                    control.startsWith(QStringLiteral("RichEdit"), Qt::CaseInsensitive) ||
                    (nativeWidget && nativeWidget->testAttribute(Qt::WA_InputMethodEnabled));
                if (editor->routeHostKey(&event, textEntry)) {
                    message->message = WM_NULL;
                    message->wParam = 0;
                    message->lParam = 0;
                }
                break;
            }
        }
        return CallNextHookEx(hook(), code, removed, data);
    }
    PluginEditorWindow* m_editor = nullptr;
#endif
};

namespace {

/// Sliders are integers; parameters are doubles. One range for all of them, so
/// a 0…1 mix control and a 20…20000 Hz frequency get the same drag resolution.
constexpr int kSliderSteps = 1000;

/// How wide the parameter dock stands. Two knobs across, which is what fits
/// beside a plugin's own view without pushing it off the screen.
constexpr int kDockWidth = 208;

/// How often the fallback panel re-reads the plugin. Only for plugins with no
/// GUI of their own, and only while the window is open.
constexpr int kPollMs = 200;

// Legacy VST editors advance their own animations from effEditIdle, even
// without automation callbacks or a running transport (e.g. groove preview).
constexpr int kEditorIdleMs = 20;

/// Plugin instances are constructed synchronously, but CLAP/VST3/AU may queue
/// main-thread work while their controller and preset settle. Let that work
/// drain before asking them to manufacture a native GUI.
constexpr int kPluginSettleMs = 80;

/// QWidget::winId() can exist one event turn before the NSView/HWND is mapped
/// into the visible hierarchy. Do not give that orphan handle to the plugin.
constexpr int kNativeMapRetryMs = 16;
constexpr int kNativeMapMaxAttempts = 45;

/// Keep the native title bar and a little desktop visible on every side. Apart
/// from looking like a plugin window instead of a second application, this
/// guarantees there is always somewhere to grab the window or resize it.
constexpr int kScreenInset = 28;

// Keep native combo-box input, accessibility and popups, but draw the closed
// controls as one compact routing strip in both QWidget and GPU workspaces.
class PluginHeaderCombo : public QComboBox, public ui::graphics::ScenePaintSource {
public:
    PluginHeaderCombo(icons::Glyph glyph, int preferredWidth, int minimumWidth,
                      QWidget* parent)
        : QComboBox(parent), m_glyph(glyph), m_preferredWidth(preferredWidth),
          m_minimumWidth(minimumWidth) {
        setFixedHeight(20);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    QSize sizeHint() const override { return {m_preferredWidth, 20}; }
    QSize minimumSizeHint() const override { return {m_minimumWidth, 20}; }
    void paintScene(QPainter& painter, const QRegion&) override {
        const auto& t = th();
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const bool focus = hasFocus() && (option.state & QStyle::State_KeyboardFocusChange);
        const bool connected = m_glyph == icons::Glyph::Link &&
                               !currentData().toString().isEmpty() && isEnabled();
        if ((isEnabled() && (underMouse() || (option.state & QStyle::State_On))) || focus) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(mixColors(t.well(), t.textPrimary, 0.07));
            painter.drawRoundedRect(bounds, 4, 4);
        }
        const QColor ink = isEnabled() ? t.textPrimary : t.textSecondary;
        const QColor iconInk = connected ? t.accentHighlight : t.textSecondary;
        icons::Glyph glyph = m_glyph;
        if (glyph == icons::Glyph::StereoRings &&
            currentData().toInt() == int(daw::PluginChannelMode::Mono))
            glyph = icons::Glyph::MonoRing;
        icons::paint(painter, glyph, QRectF(7, 3, 14, 14), iconInk);
        painter.setPen(ink);
        const QRect textRect(27, 0, std::max(0, width() - 46), height());
        const QString label = m_glyph == icons::Glyph::StereoRings &&
                              currentData().toInt() == int(daw::PluginChannelMode::DualMono)
            ? QStringLiteral("2 × ") + itemText(findData(int(daw::PluginChannelMode::Mono)))
            : currentText();
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(label, Qt::ElideRight,
                                                  textRect.width()));
        icons::paint(painter, icons::Glyph::Chevron,
                     QRectF(width() - 16, 5, 10, 10), iconInk);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintScene(painter, QRegion(rect()));
    }
private:
    icons::Glyph m_glyph;
    int m_preferredWidth;
    int m_minimumWidth;
};

// Toggle checkboxes without dismissing the source picker. Native menu focus,
// scrolling, accessibility, Escape and outside-click dismissal remain intact.
class SidechainSourceMenu final : public QMenu {
public:
    using QMenu::QMenu;
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return ||
                key->key() == Qt::Key_Enter) {
                event->accept();
                return true;
            }
        }
        return QMenu::event(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        auto* action = actionAt(event->pos());
        if (event->button() == Qt::LeftButton && toggle(action)) {
            event->accept();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
             event->key() == Qt::Key_Enter) && toggle(activeAction())) {
            event->accept();
            return;
        }
        QMenu::keyPressEvent(event);
    }
private:
    bool toggle(QAction* action) {
        if (!action || !action->isEnabled() || !action->isCheckable()) return false;
        action->trigger();
        update();
        return true;
    }
};

class PluginSidechainCombo final : public PluginHeaderCombo {
public:
    explicit PluginSidechainCombo(QWidget* parent)
        : PluginHeaderCombo(icons::Glyph::Link, 186, 52, parent),
          m_menu(new SidechainSourceMenu(this)) {
        m_menu->setObjectName(QStringLiteral("PluginSidechainMenu"));
        m_menu->setToolTipsVisible(true);
        connect(m_menu, &QMenu::aboutToHide, this, [this] {
            QComboBox::hidePopup();
            update();
        });
    }
    std::function<void(const std::vector<std::string>&)> selectionChanged;
    std::function<std::vector<daw::EngineController::SidechainSource>()> availableSources;

    void setSources(const std::vector<daw::EngineController::SidechainSource>& sources,
                    const std::vector<std::string>& selected) {
        m_sources = sources;
        const bool selectionChanged = m_selected != selected;
        m_selected = selected;
        QStringList names;
        for (const auto& id : selected) {
            const auto found = std::find_if(m_sources.begin(), m_sources.end(),
                [&](const auto& source) { return source.id == id; });
            if (found != m_sources.end()) names << QString::fromStdString(found->name);
            else {
                const auto name = PluginEditorWindow::tr("Missing source (%1)")
                    .arg(QString::fromStdString(id).left(8));
                m_sources.push_back({id, name.toStdString()});
                names << name;
            }
        }
        const QString label = selected.empty() ? PluginEditorWindow::tr("Side Chain: Off")
            : selected.size() == 1 ? PluginEditorWindow::tr("Side Chain: %1").arg(names.front())
            : PluginEditorWindow::tr("Side Chain: %1 sources").arg(selected.size());
        if (count() != 1) { clear(); addItem(label); }
        else if (itemText(0) != label) setItemText(0, label);
        const QString connected = selected.empty() ? QString() : QStringLiteral("connected");
        if (itemData(0).toString() != connected) setItemData(0, connected);
        if (currentIndex() != 0) setCurrentIndex(0);
        const QString tooltip = (names.isEmpty() ? label : names.join(QLatin1Char('\n'))) + QLatin1Char('\n') +
            PluginEditorWindow::tr("Select one or more sources. Their post-fader signals are summed into the sidechain input.");
        if (toolTip() != tooltip) setToolTip(tooltip);
        if (selectionChanged) syncChecks();
    }
    void showPopup() override {
        if (!isEnabled() || m_menu->isVisible()) return;
        // Walking the routing graph belongs to opening the picker, not the
        // 200 ms editor poll. Keep selected/missing sources available to remove.
        auto sources = availableSources ? availableSources() : m_sources;
        for (const auto& source : m_sources) {
            if (std::none_of(sources.begin(), sources.end(),
                [&](const auto& candidate) { return candidate.id == source.id; }))
                sources.push_back(source);
        }
        m_menu->clear();
        auto* off = m_menu->addAction(PluginEditorWindow::tr("Side Chain: Off"));
        off->setCheckable(true);
        connect(off, &QAction::triggered, this, [this] {
            if (selectionChanged) selectionChanged({});
        });
        m_menu->addSeparator();
        for (const auto& source : sources) {
            const QString name = QString::fromStdString(source.name);
            auto* action = m_menu->addAction(fontMetrics().elidedText(name, Qt::ElideRight, 320));
            action->setToolTip(name);
            action->setData(QString::fromStdString(source.id));
            action->setCheckable(true);
            connect(action, &QAction::triggered, this, [this, id = source.id](bool checked) {
                auto selected = m_selected;
                std::erase(selected, id);
                if (checked) selected.push_back(id);
                if (selectionChanged) selectionChanged(selected);
            });
        }
        syncChecks();
        m_menu->setMinimumWidth(std::max(240, width()));
        m_menu->popup(mapToGlobal(QPoint(0, height() + 3)));
        for (auto* action : m_menu->actions()) {
            if (action->isChecked()) { m_menu->setActiveAction(action); break; }
        }
    }
    void hidePopup() override {
        m_menu->hide();
        QComboBox::hidePopup();
    }
private:
    void syncChecks() {
        for (auto* action : m_menu->actions()) {
            if (!action->isCheckable()) continue;
            const auto id = action->data().toString().toStdString();
            const bool selected = id.empty() ? m_selected.empty()
                : std::find(m_selected.begin(), m_selected.end(), id) != m_selected.end();
            action->setChecked(selected);
            action->setEnabled(id.empty() || selected || m_selected.size() < daw::kMaxPluginSidechainSources);
        }
    }
    SidechainSourceMenu* m_menu;
    std::vector<daw::EngineController::SidechainSource> m_sources;
    std::vector<std::string> m_selected;
};

class PluginHeaderName final : public QLabel, public ui::graphics::ScenePaintSource {
public:
    using QLabel::QLabel;
    void paintScene(QPainter& painter, const QRegion&) override {
        painter.setPen(isEnabled() ? th().textPrimary : th().textSecondary);
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight,
                                                  contentsRect().width()));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintScene(painter, QRegion(rect()));
    }
};

QSize boundedToAvailable(QSize requested, const QSize& available) {
    if (!requested.isValid()) requested = QSize(720, 480);
    if (!available.isValid()) return requested;
    requested.setWidth(std::clamp(requested.width(), 1, available.width()));
    requested.setHeight(std::clamp(requested.height(), 1, available.height()));
    return requested;
}

} // namespace

PluginEditorWindow::PluginEditorWindow(daw::EngineController* controller,
                                       QString channelId, QString insertId,
                                       QWidget* parent)
    : QWidget(parent), m_controller(controller),
      m_channelId(std::move(channelId)), m_insertId(std::move(insertId)),
      m_channelKey(m_channelId.toStdString()),
      m_insertKey(m_insertId.toStdString()) {
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("dawPluginEditor", true);
    setFocusPolicy(Qt::StrongFocus);
    m_nativeKeyboard = std::make_unique<PluginEditorNativeKeyboard>(this);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    buildWrapper();

    // Everything below the wrapper lives in one row: the plugin's own view (or
    // the fallback panel) on the left, our parameter dock on the right. A row
    // rather than a single slot because the dock has to be able to stand
    // *beside* a native view we are not allowed to draw into.
    m_content = new QWidget(this);
    m_contentRow = new QHBoxLayout(m_content);
    m_contentRow->setContentsMargins(0, 0, 0, 0);
    m_contentRow->setSpacing(0);
    m_layout->addWidget(m_content, 1);

    // Keep the shell operable while the event loop gets one frame to present
    // it. The native GUI is attached only afterwards, when reparenting can no
    // longer invalidate the NSView/HWND handed to the plugin.
    showLoadingState();
    resize(720, 480);

    // The document already owns the slot identity. The first process readout
    // may still be pending; an empty UID would make the orphan sweep close
    // this valid editor before its asynchronous initialization can finish.
    if (const auto* plugin = m_controller
            ? m_controller->insertModel(m_channelKey, m_insertKey) : nullptr) {
        setWindowTitle(QString::fromStdString(plugin->name));
        m_pluginUid = QString::fromStdString(plugin->uid);
        setProperty("vlt.pitchChrome", plugin->format == daw::PluginFormat::Internal &&
                    plugin->uid == "daw.pitch-corrector");
    } else {
        setWindowTitle(tr("Plugin"));
    }
    if (const auto plugin = editorSnapshot()) m_contentIdentity = plugin->identity;

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this,
            &PluginEditorWindow::pollEditorState);

    m_editorIdle = new QTimer(this);
    m_editorIdle->setTimerType(Qt::PreciseTimer);
    m_editorIdle->setInterval(kEditorIdleMs);
    connect(m_editorIdle, &QTimer::timeout, this, [this] {
        if (m_closing || !m_embedded || !isVisible() || isMinimized() ||
            !m_controller || !m_controller->pumpInsertEditor(
                m_channelKey, m_insertKey, m_openedOn)) {
            m_editorIdle->stop();
        }
    });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &PluginEditorWindow::applyTheme);
    applyTheme();
}

PluginEditorWindow::~PluginEditorWindow() {
    qApp->removeEventFilter(this);
    m_nativeKeyboard.reset();
    // The plugin must let go of the view before Qt destroys it, or the plugin
    // is left drawing into freed memory.
    detachFromPlugin();
}

void PluginEditorWindow::initializeEditor() {
    if (m_editorInitialized || m_closing) return;
    m_editorInitialized = true;
    scheduleEditorInitialization(kPluginSettleMs);
}

void PluginEditorWindow::prepareNativeHostHierarchy() {
    if (!requiresNativeSurface()) return;
    // Headless tests intentionally have no real native-window hierarchy. A
    // forced one changes their focus/shortcut routing without exercising any
    // vendor editor, so leave those platform plugins as ordinary widgets.
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") ||
        platform == QLatin1String("minimal")) {
        return;
    }
    // `m_content` is the eventual parent of the vendor surface. Mark only the
    // chain inside its InternalEditorFrame. The frame deliberately carries
    // WA_DontCreateNativeAncestors: promoting the workspace host or its central
    // children would leave permanent native siblings around the Quick surface
    // after this editor is destroyed.
    //
    // The attribute itself matters beyond creating the handle: QWidget::raise()
    // only reorders the *native* view of a widget that carries it, and the
    // frame around this editor has to be able to come to the front of the
    // workspace once a foreign view lives inside it.
    if (auto* frame = qobject_cast<InternalEditorFrame*>(parentWidget()))
        frame->prepareForNativeSurface();
    QWidget* nativeOverlay = nullptr;
    for (QWidget* widget = m_content; widget && !widget->isWindow();
         widget = widget->parentWidget()) {
        widget->setAttribute(Qt::WA_NativeWindow);
        if (widget->property("dawInternalEditor").toBool()) {
            nativeOverlay = widget;
            break;
        }
    }
    // A foreign NSView/HWND cannot be copied into the Qt Quick scene. Keep the
    // whole frameless editor as one native child above that scene instead. It
    // remains clipped to and positioned inside the DAW workspace; only its
    // pixels bypass scene recording while the timeline stays on the GPU.
    if (nativeOverlay) {
        nativeOverlay->setProperty("vlt.nativeOverlay", true);
        nativeOverlay->raise();
    }
}

bool PluginEditorWindow::requiresNativeSurface() const {
    if ((m_controller && !m_controller->sharedEditingAllowed()) || (m_editAccessCheck && !m_editAccessCheck())) return false;
    const auto plugin = editorSnapshot();
    return plugin && plugin->hasEditor;
}

bool PluginEditorWindow::canResizeNativeEditor() const {
    const auto size = m_controller
        ? m_controller->insertEditorSize(m_channelKey, m_insertKey, m_openedOn)
        : std::nullopt;
    return !size || size->resizable;
}

void PluginEditorWindow::scheduleEditorInitialization(int delayMs) {
    if (m_closing) return;
    const std::uint64_t generation = ++m_loadGeneration;
    m_editorReady = false;
    m_pendingEditor = {};
    showLoadingState();
    QTimer::singleShot(std::max(0, delayMs), this, [this, generation] {
        if (generation != m_loadGeneration || !m_editorInitialized) return;
        // Plugins can request main-thread callbacks during construction and
        // state restore. This is the real loading turn, not merely a label.
        if (m_controller) (void)m_controller->pumpPluginEvents();
        rebuildEditorContent();
    });
}

void PluginEditorWindow::showLoadingState() {
    if (!m_loading) {
        auto* loading = new QLabel(tr("Loading plugin editor…"), m_content);
        loading->setObjectName(QStringLiteral("PluginEditorLoading"));
        loading->setAlignment(Qt::AlignCenter);
        loading->setAccessibleName(tr("Loading plugin editor"));
        // The frame header remains available for close/maximize, but an editor
        // area that is not ready must never pass clicks into the arrangement
        // behind it and look like a dead translucent window.
        loading->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        m_loading = loading;
    }
    m_loading->setGeometry(m_content->rect());
    m_loading->show();
    m_loading->raise();
}

void PluginEditorWindow::hideLoadingState() {
    if (m_loading) m_loading->hide();
}

void PluginEditorWindow::buildWrapper() {
    m_wrapper = new QWidget(this);
    m_wrapper->setObjectName(QStringLiteral("PluginWrapper"));
    m_wrapper->setFixedHeight(28);
    auto* row = new QHBoxLayout(m_wrapper);
    row->setContentsMargins(6, 2, 6, 3);
    row->setSpacing(6);

    m_power = new ui::IconButton(icons::Glyph::Power,
                                 tr("Enable or bypass this plugin"), m_wrapper);
    m_power->setObjectName(QStringLiteral("PluginPower"));
    m_power->setCheckable(true);
    m_power->setButtonSize(24, 22);
    m_power->setProperty("consoleButton", true);
    m_power->setFocusPolicy(Qt::StrongFocus);
    m_power->setAccessibleName(tr("Plugin enabled"));
    row->addWidget(m_power);

    m_pluginName = new PluginHeaderName(tr("Plugin"), m_wrapper);
    m_pluginName->setObjectName(QStringLiteral("PluginWrapperName"));
    m_pluginName->setMinimumWidth(0);
    m_pluginName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(m_pluginName, 1);

    m_dockToggle = new QToolButton(m_wrapper);
    m_dockToggle->setObjectName(QStringLiteral("PluginParameters"));
    m_dockToggle->setCheckable(true);
    m_dockToggle->setFixedSize(24, 22);
    m_dockToggle->setIconSize(QSize(16, 16));
    m_dockToggle->setFocusPolicy(Qt::StrongFocus);
    m_dockToggle->setCursor(Qt::PointingHandCursor);
    m_dockToggle->setToolTip(
        tr("Show this plugin's parameters — right-click a knob to create automation"));
    m_dockToggle->setAccessibleName(tr("Show parameter panel"));
    m_dockToggle->setVisible(false);
    connect(m_dockToggle, &QToolButton::toggled, this,
            [this](bool on) { setParameterDockVisible(on); });
    row->addWidget(m_dockToggle);
    if (const auto* track=m_controller->project().findTrack(m_channelKey); track && track->instrument.id==m_insertKey) {
        auto* slide=new QToolButton(m_wrapper);slide->setText(tr("Slide"));slide->setAccessibleName(tr("Slide delivery settings"));row->addWidget(slide);
        auto* statusTimer=new QTimer(slide);statusTimer->setInterval(250);
        const auto refresh=[this,slide]{auto node=m_controller->instrumentSlideStatus(m_channelKey);
            using D=daw::plugins::SlideDelivery;QString state;
            switch(node.mode){case D::NoteExpression:state=tr("Per-note pitch");break;case D::MPE:state=tr("MPE · channels 2–16");break;case D::PitchBend:state=tr("Pitch Bend · affects the entire channel");break;default:state=tr("Slide unavailable or disabled");break;}
            if(node.overloaded)state=tr("MPE overloaded: new voice skipped");else if(node.clipped)state=tr("Slide exceeds the instrument bend range");
            slide->setText(node.overloaded||node.clipped?tr("Slide !"):tr("Slide")+QStringLiteral(" · ")+state);slide->setToolTip(state);
        };connect(statusTimer,&QTimer::timeout,slide,refresh);statusTimer->start();refresh();
        connect(slide,&QToolButton::clicked,this,[this]{
            const auto* track=m_controller->project().findTrack(m_channelKey);if(!track)return;const auto& slot=track->instrument;
            auto* dialog=new QDialog(this,Qt::Tool);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("Slide delivery"));auto* form=new QFormLayout(dialog);
            auto* mode=new QComboBox(dialog);mode->addItems({tr("Auto"),tr("Per note"),tr("MPE"),tr("Pitch Bend"),tr("Off")});mode->setCurrentIndex(slot.slideDelivery);form->addRow(tr("Delivery"),mode);
            auto* range=new QDoubleSpinBox(dialog);range->setRange(1,96);range->setValue(slot.slideBendRange);range->setSuffix(tr(" st"));form->addRow(tr("Bend range ±"),range);
            auto* tail=new QDoubleSpinBox(dialog);tail->setRange(0,20);tail->setValue(slot.slideReleaseReserve);tail->setSuffix(tr(" s"));form->addRow(tr("Unknown release reserve"),tail);
            auto* status=new QLabel(dialog);status->setWordWrap(true);status->setMaximumWidth(360);form->addRow(status);
            auto update=[this,mode,range,tail,status]{if(!m_controller->canEditSlideNotes()){mode->setEnabled(false);range->setEnabled(false);tail->setEnabled(false);status->setText(tr("Slide editing requires collaboration protocol 5. Reconnect to an updated session."));return;}m_controller->setInstrumentSlideSettings(m_channelKey,mode->currentIndex(),range->value(),tail->value());auto node=m_controller->instrumentSlideStatus(m_channelKey);auto actual=node.mode;status->setText(actual==daw::plugins::SlideDelivery::PitchBend?tr("Affects the entire channel. Match the bend range in the instrument."):actual==daw::plugins::SlideDelivery::MPE?tr("Channel 1 is common. Channels 2–16 retain pitch through release. Match the member bend range in the instrument."):actual==daw::plugins::SlideDelivery::NoteExpression?tr("Independent pitch for each sounding note."):tr("The requested mode is unsupported or disabled."));};
            connect(mode,&QComboBox::activated,dialog,[mode,range,update](int i){if(i==2)range->setValue(48);update();});connect(range,&QDoubleSpinBox::editingFinished,dialog,update);connect(tail,&QDoubleSpinBox::editingFinished,dialog,update);update();dialog->show();
        });
    }


    auto* routing = new QWidget(m_wrapper);
    routing->setObjectName(QStringLiteral("PluginRouting"));
    routing->setFixedHeight(22);
    // Share scarce width with the title, but never stretch past the controls'
    // natural width on a large editor.
    routing->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* routingRow = new QHBoxLayout(routing);
    routingRow->setContentsMargins(1, 0, 1, 0);
    routingRow->setSpacing(0);
    row->addWidget(routing, 1);

    m_channelMode = new PluginHeaderCombo(icons::Glyph::StereoRings, 108, 52, routing);
    m_channelMode->setObjectName(QStringLiteral("PluginMode"));
    m_channelMode->setAccessibleName(tr("Plugin channel mode"));
    m_channelMode->setToolTip(
        tr("Choose automatic, mono, stereo, or two independent mono instances"));
    m_channelMode->addItem(tr("Auto"), int(daw::PluginChannelMode::Auto));
    m_channelMode->addItem(tr("Mono"), int(daw::PluginChannelMode::Mono));
    m_channelMode->addItem(tr("Stereo"), int(daw::PluginChannelMode::Stereo));
    m_channelMode->addItem(tr("Dual Mono"), int(daw::PluginChannelMode::DualMono));
    routingRow->addWidget(m_channelMode);

    m_leftChannel = new QToolButton(routing);
    m_rightChannel = new QToolButton(routing);
    for (QToolButton* button : {m_leftChannel, m_rightChannel}) {
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFixedSize(22, 20);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setCursor(Qt::PointingHandCursor);
    }
    m_leftChannel->setText(QStringLiteral("L"));
    m_leftChannel->setToolTip(tr("Edit the left mono instance"));
    m_leftChannel->setAccessibleName(tr("Edit left dual-mono channel"));
    m_rightChannel->setText(QStringLiteral("R"));
    m_rightChannel->setToolTip(tr("Edit the right mono instance"));
    m_rightChannel->setAccessibleName(tr("Edit right dual-mono channel"));
    routingRow->addWidget(m_leftChannel);
    routingRow->addWidget(m_rightChannel);

    auto* divider = new QWidget(routing);
    divider->setObjectName(QStringLiteral("PluginRoutingDivider"));
    divider->setFixedSize(1, 12);
    routingRow->addWidget(divider, 0, Qt::AlignVCenter);

    auto* sidechain = new PluginSidechainCombo(routing);
    m_sidechain = sidechain;
    m_sidechain->setObjectName(QStringLiteral("PluginSidechain"));
    m_sidechain->setAccessibleName(tr("Sidechain sources"));
    routingRow->addWidget(m_sidechain);
    m_layout->addWidget(m_wrapper);

    connect(m_power, &QAbstractButton::clicked, this, [this](bool enabled) {
        if (!m_controller || m_refreshingWrapper) return;
        m_controller->setInsertBypassed(m_channelKey, m_insertKey, !enabled);
        emit projectEdited();
    });
    connect(m_channelMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_controller || m_refreshingWrapper || index < 0) return;
        const auto mode = daw::PluginChannelMode(
            m_channelMode->itemData(index).toInt());
        detachFromPlugin();
        if (m_controller->setInsertChannelMode(m_channelKey, m_insertKey, mode)) {
            emit projectEdited();
        } else {
            m_channelMode->setToolTip(
                tr("This plugin cannot use the selected channel mode"));
        }
        scheduleEditorInitialization(kPluginSettleMs);
        refreshWrapper();
    });
    auto selectChannel = [this](daw::PluginEditorChannel channel, bool checked) {
        if (!checked || !m_controller || m_refreshingWrapper) return;
        detachFromPlugin();
        m_controller->setInsertEditorChannel(m_channelKey, m_insertKey, channel);
        emit projectEdited();
        scheduleEditorInitialization(kPluginSettleMs);
        refreshWrapper();
    };
    connect(m_leftChannel, &QToolButton::toggled, this,
            [selectChannel](bool checked) {
                selectChannel(daw::PluginEditorChannel::Left, checked);
            });
    connect(m_rightChannel, &QToolButton::toggled, this,
            [selectChannel](bool checked) {
                selectChannel(daw::PluginEditorChannel::Right, checked);
            });
    sidechain->selectionChanged = [this](const std::vector<std::string>& sources) {
        if (!m_controller || m_refreshingWrapper || !m_sidechain->isEnabled()) return;
        if (m_controller->setInsertSidechainSources(m_channelKey, m_insertKey, sources)) {
            emit projectEdited();
        }
        refreshWrapper();
    };
    sidechain->availableSources = [this] {
        return m_controller->insertSidechainSources(m_channelKey);
    };
    refreshWrapper();
}

void PluginEditorWindow::clearEditorContent() {
    detachFromPlugin();
    m_nativeEditorSize = {};
    setProperty("vlt.nativeEditor", false);
    m_layout->setSizeConstraint(QLayout::SetDefaultConstraint);
    m_genericControls.clear();
    m_genericValues.clear();
    m_dockControls.clear();
    m_dockValues.clear();
    m_dockCells.clear();
    m_dockActive.clear();
    if (m_dock) {
        m_contentRow->removeWidget(m_dock);
        delete m_dock;
        m_dock = nullptr;
        m_dockGrid = nullptr;
    }



    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    if (m_container) {
        m_contentRow->removeWidget(m_container);
        delete m_container;
        m_container = nullptr;
    }
    if (m_generic) {
        m_contentRow->removeWidget(m_generic);
        delete m_generic;
        m_generic = nullptr;
    }
}

void PluginEditorWindow::rebuildEditorContent() {
    if (m_rebuildingEditorContent) return;
    m_rebuildingEditorContent = true;
    clearEditorContent();
    // clearEditorContent() invalidates the turn that entered this function.
    // The following generation belongs specifically to the native container
    // created below and is the only one allowed to attach to it.
    const std::uint64_t generation = m_loadGeneration;
    m_fallbackContentSize = {};
    m_nativeEditorFailed = false;
    const auto plugin = editorSnapshot();
    m_contentIdentity = plugin ? plugin->identity : daw::PluginIdentity{};
    QString title = tr("Plugin");
    if (plugin) {
        title = QString::fromStdString(plugin->name);
        m_pluginUid = QString::fromStdString(plugin->uid);
    }
    setWindowTitle(title);

    m_readOnly = (m_controller && !m_controller->sharedEditingAllowed()) || (m_editAccessCheck && !m_editAccessCheck());
    if (m_readOnly) {
        buildGenericEditor();
        if (m_generic) m_generic->setEnabled(false);
        if (m_wrapper) m_wrapper->setEnabled(false);
        if (m_dock) m_dock->setEnabled(false);
        setToolTip(tr("Plugin parameters are read-only until editing permission and the shared edit lock are available."));
        finishEditorContent();
        return;
    }
    if (m_wrapper) m_wrapper->setEnabled(true);
    if (m_dock) m_dock->setEnabled(true);
    setToolTip({});

    const bool hasNativeEditor = plugin && plugin->hasEditor;
    const bool trustedInternal =
        plugin && plugin->format == daw::plugins::Format::Internal;
    const std::string descriptorUid =
        plugin ? plugin->uid : std::string{};
    if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr,
                     "editor for '%s' (%s): instance %s, hasEditor %s\n",
                     title.toUtf8().constData(),
                     plugin ? std::string(daw::plugins::toString(
                                              plugin->format)).c_str()
                            : "-",
                     plugin ? "yes" : "NO",
                     hasNativeEditor ? "yes" : "no");
    }

    if (hasNativeEditor) {
        // A capability snapshot may arrive after the initial presentation.
        // Protect the workspace before promoting the final native hierarchy.
        prepareNativeHostHierarchy();
        m_container = new QWidget(m_content);
        m_container->setProperty("vlt.foreignSurface", true);
        m_container->setAttribute(Qt::WA_NativeWindow);
        // A plugin may report a very large natural size. Do not let that size
        // hint force the top-level window to the desktop dimensions: the host
        // viewport is allowed to clip a fixed-size native view, and a resizable
        // plugin will be offered the bounded viewport in resizeEvent().
        m_container->setMinimumSize(0, 0);
        m_container->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_contentRow->insertWidget(0, m_container, 1);
        // The outer internal frame is already visible at this point. Make the
        // final native child part of that live hierarchy before exporting its
        // handle; some Cocoa/Win32 plugin toolkits return a blank view when
        // attached to an unmapped parent and never recover afterwards.
        m_contentRow->activate();
        m_container->show();
        (void)m_container->winId();
        m_pendingEditor = plugin->identity;
        m_rebuildingEditorContent = false;
        showLoadingState();
        QTimer::singleShot(kNativeMapRetryMs, this,
                           [this, generation] {
                               tryAttachNativeEditor(generation, 0);
                           });
        return;
    }

    if (trustedInternal && descriptorUid == "daw.equalizer") {
        auto* equalizerPanel =
            new EqualizerPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = equalizerPanel;
        connect(equalizerPanel, &EqualizerPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(equalizerPanel, &EqualizerPanel::automationRequested, this,
                [this](const QString& parameterId) {
                    emit automationRequested(m_channelId, m_insertId, parameterId);
                });
        m_contentRow->insertWidget(0, m_generic, 1);
        emit builtInPanelReady(equalizerPanel, QStringLiteral("equalizer"));
        // The branded EQ view itself is 820x520 minimum; this host widget also
        // owns the 39 px plugin header above it.
        setMinimumSize(820, 559);
        m_fallbackContentSize = QSize(1040, 680);
        resize(m_fallbackContentSize);
    } else if (trustedInternal &&
               (daw::plugins::modulation::isModulationUid(descriptorUid) ||
                descriptorUid == "daw.modulation")) {
        auto* panel = new ModulationPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = panel;
        connect(panel, &ModulationPanel::projectEdited, this, &PluginEditorWindow::projectEdited);
        connect(panel, &ModulationPanel::automationRequested, this,
                [this](const QString& id) { emit automationRequested(m_channelId, m_insertId, id); });
        connect(panel, &ModulationPanel::eqExpansionChanged, this, [this, collapsedHeight = 0](bool expanded) mutable {
            const int limit = screen() ? screen()->availableGeometry().height() - 60 : 1000;
            if (expanded) { collapsedHeight = height(); resize(width(), std::min(limit, height() + 270)); }
            else if (collapsedHeight) resize(width(), collapsedHeight);
        });
        m_contentRow->insertWidget(0, panel, 1);
        emit builtInPanelReady(panel, QString::fromStdString(descriptorUid).mid(4));
        const bool rack = descriptorUid == "daw.modulation";
        const bool pro = descriptorUid == "daw.doubler-pro";
        setMinimumSize(rack ? 800 : 360, pro ? 659 : 529);
        m_fallbackContentSize = QSize(rack ? 1040 : 440, pro ? 700 : 570);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.delay") {
        auto* panel = new DelayPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = panel;
        connect(panel, &DelayPanel::projectEdited, this, &PluginEditorWindow::projectEdited);
        connect(panel, &DelayPanel::automationRequested, this,
                [this](const QString& id) { emit automationRequested(m_channelId, m_insertId, id); });
        m_contentRow->insertWidget(0, panel, 1);
        emit builtInPanelReady(panel, QStringLiteral("delay"));
        setMinimumSize(960, 459);
        m_fallbackContentSize = QSize(1100, 499);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.cla2a") {
        auto* panel = new Cla2aPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = panel;
        connect(panel, &Cla2aPanel::projectEdited, this, &PluginEditorWindow::projectEdited);
        connect(panel, &Cla2aPanel::automationRequested, this,
                [this](const QString& id) { emit automationRequested(m_channelId, m_insertId, id); });
        m_contentRow->insertWidget(0, panel, 1);
        emit builtInPanelReady(panel, QStringLiteral("cla2a"));
        setMinimumSize(688, 325);
        m_fallbackContentSize = QSize(820, 349);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.compressor") {
        auto* panel = new CompressorPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = panel;
        connect(panel, &CompressorPanel::projectEdited, this, &PluginEditorWindow::projectEdited);
        connect(panel, &CompressorPanel::automationRequested, this,
                [this](const QString& id) { emit automationRequested(m_channelId, m_insertId, id); });
        m_contentRow->insertWidget(0, panel, 1);
        emit builtInPanelReady(panel, QStringLiteral("compressor"));
        setMinimumSize(800, 379);
        m_fallbackContentSize = QSize(920, 399);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.pitch-corrector") {
        auto* panel = new PitchCorrectorPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = panel;
        connect(panel, &PitchCorrectorPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(panel, &PitchCorrectorPanel::automationRequested, this,
                [this](const QString& id) {
                    emit automationRequested(m_channelId, m_insertId, id);
                });
        m_contentRow->insertWidget(0, panel, 1);
        emit builtInPanelReady(panel, QStringLiteral("pitch-corrector"));
        setMinimumSize(560, 399);
        m_fallbackContentSize = QSize(640, 449);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.graphit") {
        auto* graphitPanel =
            new GraphitPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = graphitPanel;
        connect(graphitPanel, &GraphitPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(graphitPanel, &GraphitPanel::automationRequested, this,
                [this](const QString& parameterId) {
                    emit automationRequested(m_channelId, m_insertId, parameterId);
                });
        m_contentRow->insertWidget(0, m_generic, 1);
        emit builtInPanelReady(graphitPanel, QStringLiteral("graphit"));
        setMinimumSize(440, 499);
        m_fallbackContentSize = QSize(480, 539);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.gravity") {
        auto* gravityPanel =
            new GravityPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = gravityPanel;
        connect(gravityPanel, &GravityPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(gravityPanel, &GravityPanel::automationRequested, this,
                [this](const QString& parameterId) {
                    emit automationRequested(m_channelId, m_insertId, parameterId);
                });
        m_contentRow->insertWidget(0, m_generic, 1);
        emit builtInPanelReady(gravityPanel, QStringLiteral("gravity"));
        setMinimumSize(820, 638);
        m_fallbackContentSize = QSize(920, 718);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.sampler") {
        auto* samplerPanel =
            new SamplerPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = samplerPanel;
        connect(samplerPanel, &SamplerPanel::pluginEditorRequested, this,
                &PluginEditorWindow::nestedPluginEditorRequested);
        connect(samplerPanel, &SamplerPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(samplerPanel, &SamplerPanel::automationRequested, this,
                [this](const QString& parameterId) {
                    emit automationRequested(m_channelId, m_insertId, parameterId);
                });
        m_contentRow->insertWidget(0, m_generic, 1);
        emit builtInPanelReady(samplerPanel, QStringLiteral("sampler"));
        setMinimumSize(860, 558);
        m_fallbackContentSize = QSize(1040, 760);
        resize(m_fallbackContentSize);
    } else if (trustedInternal && descriptorUid == "daw.slicer") {
        auto* slicerPanel = new SlicerPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = slicerPanel;
        connect(slicerPanel, &SlicerPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(slicerPanel, &SlicerPanel::automationRequested, this,
                [this](const QString& parameterId) {
                    emit automationRequested(m_channelId, m_insertId, parameterId);
                });
        m_contentRow->insertWidget(0, m_generic, 1);
        emit builtInPanelReady(slicerPanel, QStringLiteral("slicer"));
        setMinimumSize(780, 426);
        m_fallbackContentSize = QSize(980, 450);
        resize(m_fallbackContentSize);
    } else {
        buildGenericEditor();
    }
    finishEditorContent();
}

void PluginEditorWindow::tryAttachNativeEditor(std::uint64_t generation,
                                               int attempt) {
    if (generation != m_loadGeneration || !m_container) return;
    if ((m_controller && !m_controller->sharedEditingAllowed()) || (m_editAccessCheck && !m_editAccessCheck())) {
        rebuildEditorContent();
        return;
    }

    if (!m_controller || m_controller->insertIdentity(m_channelKey, m_insertKey) != m_pendingEditor) {
        // The slot was replaced while its old GUI was loading. Never call a
        // retired instance; start again against the live slot instead.
        scheduleEditorInitialization(0);
        return;
    }

    const WId handle = m_container->winId();
    bool nativeParentReady = handle != 0 && m_container->isVisible();
    QWidget* topLevel = m_container->window();
    nativeParentReady = nativeParentReady && topLevel &&
                        topLevel->windowHandle() &&
                        topLevel->windowHandle()->isExposed();
#if defined(Q_OS_MACOS)
    nativeParentReady = nativeParentReady &&
                        pluginEditorContainerReadyForMac(m_container);
#endif
    if (!nativeParentReady && attempt < kNativeMapMaxAttempts) {
        QTimer::singleShot(kNativeMapRetryMs, this,
                           [this, generation, attempt] {
                               tryAttachNativeEditor(generation, attempt + 1);
                           });
        return;
    }

    if (nativeParentReady) {
        // Drain callbacks queued by the format-specific editor probe before
        // entering vendor code. GUI creation itself must remain on this thread.
        if (m_controller) (void)m_controller->pumpPluginEvents();
        m_embedded = m_controller->openInsertEditor(
            m_channelKey, m_insertKey, m_pendingEditor, reinterpret_cast<void*>(handle), this);
    }

    if (m_embedded) {
        m_openedOn = m_pendingEditor;
        finishNativeEditorOpen();
    } else {
        m_nativeEditorFailed = true;
        if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
            std::fprintf(stderr,
                         "editor attach failed after %d native-map turns\n",
                         attempt);
        }
        m_contentRow->removeWidget(m_container);
        delete m_container;
        m_container = nullptr;
        buildGenericEditor();
    }
    finishEditorContent();
}

void PluginEditorWindow::finishNativeEditorOpen() {
    if (!m_controller || !m_container) return;
    const auto size = m_controller->insertEditorSize(m_channelKey, m_insertKey, m_openedOn);
    if (!size || size->width == 0 || size->height == 0) return;
    auto width = size->width, height = size->height;

    // The wrapper may elide its labels, but must never widen a small native
    // GUI. The plugin's reported size is authoritative for both host modes.
    setProperty("vlt.nativeEditor", true);
    m_layout->setSizeConstraint(QLayout::SetNoConstraint);
    setMinimumSize(0, 0);
    QSize wanted(int(width) + dockWidth(),
                 int(height) + m_wrapper->height());
    QSize bounded = boundedWindowSize(wanted);

    // If the GUI supports resizing, give it the compact content size before
    // its first uncovered frame. Fixed-size GUIs retain their natural pixels.
    if (size->resizable && bounded != wanted) {
        std::uint32_t boundedWidth = std::uint32_t(
            std::max(1, bounded.width() - dockWidth()));
        std::uint32_t boundedHeight = std::uint32_t(
            std::max(1, bounded.height() - m_wrapper->height()));
        if (const auto accepted = m_controller->resizeInsertEditor(
                m_channelKey, m_insertKey, m_openedOn, {boundedWidth, boundedHeight})) {
            width = accepted->width;
            height = accepted->height;
            wanted = QSize(int(width) + dockWidth(),
                           int(height) + m_wrapper->height());
            bounded = boundedWindowSize(wanted);
        }
    }
    m_applyingPluginSize = true;
    m_nativeEditorSize = QSize(int(width), int(height));
    m_container->resize(int(width), int(height));
    resize(bounded);
    m_applyingPluginSize = false;
}

void PluginEditorWindow::finishEditorContent() {
    // Native editors use the dock; generic editors already show parameters.
    if (m_dockToggle) m_dockToggle->setVisible(m_embedded);
    if (!m_embedded) {
        m_dockToggle->setChecked(false);
    }
    if (m_dockToggle->isChecked()) setParameterDockVisible(true);
    refreshWrapper();
    m_pendingEditor = {};
    m_editorReady = true;
    m_rebuildingEditorContent = false;
    hideLoadingState();
    prepareForPresentation();
    syncPollTimer();
}

void PluginEditorWindow::refreshWrapper() {
    if (!m_controller || !m_wrapper) return;
    const daw::InsertModel* model =
        m_controller->insertModel(m_channelKey, m_insertKey);
    if (!model) return;
    m_refreshingWrapper = true;

    m_power->setChecked(!model->bypassed);
    m_pluginName->setEnabled(!model->bypassed);
    const int modeIndex = m_channelMode->findData(int(model->channelMode));
    if (modeIndex >= 0) m_channelMode->setCurrentIndex(modeIndex);
    const bool dual = model->channelMode == daw::PluginChannelMode::DualMono;
    m_leftChannel->setVisible(dual);
    m_rightChannel->setVisible(dual);
    m_leftChannel->setChecked(model->editorChannel ==
                              daw::PluginEditorChannel::Left);
    m_rightChannel->setChecked(model->editorChannel ==
                               daw::PluginEditorChannel::Right);

    m_pluginName->setText(QString::fromStdString(model->name));
    m_pluginName->setToolTip(m_pluginName->text());

    const bool supports =
        m_controller->insertSupportsSidechain(m_channelKey, m_insertKey);
    if (!supports) {
        if (m_sidechain->isEnabled() || m_sidechain->count() == 0) {
            m_sidechain->hidePopup();
            m_sidechain->clear();
            m_sidechain->addItem(tr("Side Chain: %1").arg(QStringLiteral("—")));
        }
        m_sidechain->setEnabled(false);
        m_sidechain->setToolTip(
            tr("This plugin exposes no auxiliary audio input"));
        m_refreshingWrapper = false;
        return;
    }

    // Only selected names are needed for the closed header. Discover valid
    // routes on demand when its popup opens, even in large playing projects.
    std::vector<daw::EngineController::SidechainSource> sources;
    sources.reserve(model->sidechainTrackIds.size());
    for (const auto& id : model->sidechainTrackIds) {
        if (const auto* source = m_controller->project().findTrack(id))
            sources.push_back({source->id, source->name});
    }
    m_sidechain->setEnabled(true);
    static_cast<PluginSidechainCombo*>(m_sidechain)->setSources(sources, model->sidechainTrackIds);
    m_refreshingWrapper = false;
}

void PluginEditorWindow::detachFromPlugin() {
    if (m_editorIdle) m_editorIdle->stop();
    ++m_loadGeneration;
    m_editorReady = false;
    const auto opened = m_openedOn;
    const auto probe = m_pendingEditor ? m_pendingEditor : m_contentIdentity;
    m_openedOn = {};
    m_pendingEditor = {};
    m_contentIdentity = {};
    m_embedded = false;
    if (!m_controller) return;
    // Clear our tokens before vendor callbacks. A late detach must neither
    // close a successor's window nor discard its pending VST3 capability probe.
    // The content token is a probe too: closeEvent and the deferred destructor
    // both detach, and a replacement may still be waiting to open.
    if (opened) m_controller->closeInsertEditor(m_channelKey, m_insertKey, opened);
    else if (probe) m_controller->closeInsertEditor(m_channelKey, m_insertKey, probe, true);
}

std::optional<daw::PluginEditorSnapshot> PluginEditorWindow::editorSnapshot() const {
    if (!m_controller) return std::nullopt;
    return m_controller->insertEditorSnapshot(m_channelKey, m_insertKey);
}

void PluginEditorWindow::pollEditorState() {
    if (m_controller) {
        const auto health = m_controller->insertRuntimeStatus(m_channelKey, m_insertKey);
        if (health.state == daw::AudioPluginRuntimeState::Faulted ||
            health.state == daw::AudioPluginRuntimeState::AwaitingRecovery ||
            health.state == daw::AudioPluginRuntimeState::Recovering) {
            m_poll->stop();
            m_editorIdle->stop();
            m_content->setEnabled(false);
            return;
        }
    }
    refreshAccessPolicy();

    if (m_editorReady && m_controller &&
        m_contentIdentity != m_controller->insertIdentity(m_channelKey, m_insertKey)) {
        scheduleEditorInitialization(0);
        return;
    }
    refreshWrapper();
    refreshGenericEditor();
    refreshParameterDock();
}

void PluginEditorWindow::refreshAccessPolicy() {
    const bool readOnly = (m_controller && !m_controller->sharedEditingAllowed()) || (m_editAccessCheck && !m_editAccessCheck());
    if (m_readOnly != readOnly && !m_closing) {
        m_readOnly = readOnly;
        rebuildEditorContent();
    }
}

void PluginEditorWindow::syncPollTimer() {
    if (!m_poll) return;
    const bool shouldPoll = isVisible() && !isMinimized();
    const auto plugin = editorSnapshot();
    const bool needsIdle = shouldPoll && !m_closing && m_editorReady &&
        m_embedded && plugin && plugin->identity == m_openedOn && plugin->open &&
        plugin->format == daw::plugins::Format::Vst;
    // This is deliberately independent of pumpPluginEvents' wake-generation
    // fast path and of the slow wrapper/parameter poll. Only the live VST
    // editor needs a periodic turn; the rest of the project stays event-driven.
    if (needsIdle) {
        if (!m_editorIdle->isActive()) m_editorIdle->start();
    } else {
        m_editorIdle->stop();
    }
    if (!shouldPoll) {
        m_poll->stop();
        return;
    }
    if (m_poll->isActive()) return;

    // A hidden editor may have missed automation or routing changes. Refresh
    // before its first restored frame, then resume the low-rate follow-up.
    pollEditorState();
    m_poll->start();
}

bool PluginEditorWindow::routeHostKey(QKeyEvent* event, bool textEntry) {
    if (m_closing || !isVisible()) return false;
    if (event->type() != QEvent::KeyRelease &&
        (QApplication::activeModalWidget() || QApplication::activePopupWidget())) return false;
    if (event->type() == QEvent::KeyPress) emit keyboardFocusReceived();
    return m_hostKeyHandler && m_hostKeyHandler(event, textEntry);
}

bool PluginEditorWindow::eventFilter(QObject* watched, QEvent* event) {
    if (m_closing || !isVisible()) return false;
    switch (event->type()) {
    case QEvent::FocusIn:
    case QEvent::MouseButtonPress:
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        break;
    default:
        return false;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    auto* frame = qobject_cast<InternalEditorFrame*>(parentWidget());
    const bool belongs = widget && (widget == this || isAncestorOf(widget) ||
        (frame && (widget == frame || frame->isAncestorOf(widget))));
    if (!belongs) return false;
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress)
        emit keyboardFocusReceived();
    if (event->type() == QEvent::MouseButtonPress && m_controller &&
        static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        const QString parameter = widget->property("parameterId").toString();
        if (!parameter.isEmpty())
            m_controller->touchInsertParameter(m_channelKey, m_insertKey,
                                               parameter.toStdString());
    }
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        auto* key = static_cast<QKeyEvent*>(event);
        bool textEntry = false;
        for (auto* current = widget; current && current != this; current = current->parentWidget()) {
            const auto* combo = qobject_cast<QComboBox*>(current);
            textEntry = textEntry || qobject_cast<QLineEdit*>(current) ||
                qobject_cast<QAbstractSpinBox*>(current) || qobject_cast<QTextEdit*>(current) ||
                qobject_cast<QPlainTextEdit*>(current) || (combo && combo->isEditable()) ||
                current->testAttribute(Qt::WA_InputMethodEnabled);
        }
        return routeHostKey(key, textEntry);
    }
    return false;
}

void PluginEditorWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) syncPollTimer();
}

void PluginEditorWindow::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
    if (!event->isAccepted() || m_closing) return;
    m_closing = true;
    m_editorInitialized = false;
    // WA_DeleteOnClose is deferred and may wait through a native nested event
    // loop. Release the plugin GUI before the registry permits another open;
    // otherwise VST refuses it, or AU/CLAP/VST3 reuse the old window's view.
    detachFromPlugin();
    emit closing(m_channelId, m_insertId);
}

void PluginEditorWindow::hideEvent(QHideEvent* event) {
    qApp->removeEventFilter(this);
    if (m_poll) m_poll->stop();
    if (m_editorIdle) m_editorIdle->stop();
    QWidget::hideEvent(event);
}

void PluginEditorWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_loading) {
        m_loading->setGeometry(m_content->rect());
        if (m_loading->isVisible()) m_loading->raise();
    }
    if (!m_embedded || m_applyingPluginSize || !m_container) return;
    // Reparenting into a hidden InternalEditorFrame causes provisional layout
    // passes before the frame has restored/fitted itself. They are host
    // bookkeeping, not a user resize, and must not be offered back to a
    // resizable plugin as its new preferred size.
    if (m_rebuildingEditorContent || (!isWindow() && !m_hasBeenPresented)) return;
    if (!m_controller) return;

    // Offer the new size; the plugin snaps it to something it can draw and
    // writes back what it settled on, which the container then takes.
    const auto accepted = m_controller->resizeInsertEditor(
        m_channelKey, m_insertKey, m_openedOn,
        {std::uint32_t(m_container->width()), std::uint32_t(m_container->height())});
    if (!accepted) return;
    const auto width = accepted->width, height = accepted->height;
    m_nativeEditorSize = QSize(int(width), int(height));
    if (int(width) == m_container->width() && int(height) == m_container->height()) {
        return;
    }
    m_applyingPluginSize = true;
    m_container->resize(int(width), int(height));
    m_applyingPluginSize = false;
}

void PluginEditorWindow::showEvent(QShowEvent* event) {
    qApp->installEventFilter(this);
    QWidget::showEvent(event);
    syncPollTimer();
    if (!isWindow()) {
        m_hasBeenPresented = true;
        return;
    }
#if defined(Q_OS_MACOS)
    configurePluginEditorWindowForMac(this);
#endif
    if (windowState().testFlag(Qt::WindowFullScreen) ||
        windowState().testFlag(Qt::WindowMaximized)) {
        QTimer::singleShot(0, this, &PluginEditorWindow::prepareForPresentation);
    }
    if (m_hasBeenPresented) return;
    m_hasBeenPresented = true;
    constrainToScreen(true);
    // The native frame is only reliable after showEvent. Run the containment
    // pass once more so even an unusually thick title bar remains reachable.
    QTimer::singleShot(0, this, [this] { constrainToScreen(false); });
}

void PluginEditorWindow::prepareForPresentation() {
    if(auto* panel=findChild<SamplerPanel*>())panel->showDefaultPage();
    if (!isWindow()) {
        // Geometry persistence belongs to the host frame, but the pixels a
        // foreign GUI needs belong to the plugin. Re-apply that native size on
        // every presentation so an old compact frame can never reopen it as a
        // cropped square.
        if (m_embedded && m_nativeEditorSize.isValid()) {
            m_applyingPluginSize = true;
            m_container->resize(m_nativeEditorSize);
            applyRequestedContentSize(requestedContentSize());
            m_applyingPluginSize = false;
        } else if (m_editorReady && m_fallbackContentSize.isValid()) {
            applyRequestedContentSize(requestedContentSize());
        }
        syncPollTimer();
        return;
    }
    const bool wasVisible = isVisible();
    if (windowState().testFlag(Qt::WindowFullScreen) ||
        windowState().testFlag(Qt::WindowMaximized)) {
        setWindowState(Qt::WindowNoState);
        if (wasVisible) showNormal();
    }
#if defined(Q_OS_MACOS)
    // `winId()` exists by the time MainWindow presents us. Apply the native
    // collection behaviour again because Cocoa can replace the backing
    // NSWindow while its parent enters or leaves fullscreen.
    configurePluginEditorWindowForMac(this);
#endif
    constrainToScreen(!m_hasBeenPresented);
}

int PluginEditorWindow::dockWidth() const {
    return m_dock && m_dock->isVisible() ? kDockWidth : 0;
}

QSize PluginEditorWindow::requestedContentSize() const {
    if (!m_nativeEditorSize.isValid()) {
        if (!m_fallbackContentSize.isValid()) return size();
        return QSize(m_fallbackContentSize.width() + dockWidth(),
                     m_fallbackContentSize.height());
    }
    return QSize(m_nativeEditorSize.width() + dockWidth(),
                 m_nativeEditorSize.height() +
                     (m_wrapper ? m_wrapper->height() : 0));
}

void PluginEditorWindow::onEditorResized(std::uint32_t width,
                                         std::uint32_t height) noexcept {
    const auto identity = m_openedOn ? m_openedOn : m_pendingEditor;
    if (!m_container || width == 0 || height == 0 || !identity || !m_controller ||
        m_controller->insertIdentity(m_channelKey, m_insertKey) != identity) return;
    m_applyingPluginSize = true;
    m_nativeEditorSize = QSize(int(width), int(height));
    m_container->resize(int(width), int(height));
    applyRequestedContentSize(requestedContentSize());
    m_applyingPluginSize = false;
}

QSize PluginEditorWindow::boundedWindowSizeForTest(const QSize& requested,
                                                    const QSize& available) {
    return boundedToAvailable(requested, available);
}

QSize PluginEditorWindow::boundedWindowSize(const QSize& requested) const {
    if (auto* frame = qobject_cast<InternalEditorFrame*>(parentWidget())) {
        // Internal editors are bounded by the DAW workspace, not by the whole
        // monitor. Giving a resizable plugin the monitor size and shrinking
        // only its host frame leaves the plugin drawing a larger image through
        // a smaller viewport — the exact cropped/incorrect-scale failure.
        return boundedToAvailable(requested, frame->maximumContentSize());
    }
    QScreen* target = parentWidget() ? parentWidget()->screen() : screen();
    if (!target) target = QGuiApplication::primaryScreen();
    if (!target) return requested;
    const QRect usable = target->availableGeometry().adjusted(
        kScreenInset, kScreenInset, -kScreenInset, -kScreenInset);
    return boundedToAvailable(requested, usable.size());
}

void PluginEditorWindow::applyRequestedContentSize(const QSize& requested) {
    if (auto* frame = qobject_cast<InternalEditorFrame*>(parentWidget())) {
        frame->resizeForContent(requested);
        return;
    }
    resize(boundedWindowSize(requested));
}

void PluginEditorWindow::constrainToScreen(bool centerOnParent) {
    QScreen* target = parentWidget() ? parentWidget()->screen() : screen();
    if (!target) target = QGuiApplication::primaryScreen();
    if (!target) return;

    const QRect usable = target->availableGeometry().adjusted(
        kScreenInset, kScreenInset, -kScreenInset, -kScreenInset);
    if (usable.isEmpty()) return;

    // `resize()` addresses the client area, while the screen bounds include
    // the native title bar and border. Once shown, subtract those margins so
    // the complete frame — not merely the plugin pixels — fits the screen.
    const QRect client = geometry();
    const QRect frameBefore = frameGeometry();
    const QSize frameMargins(
        std::max(0, frameBefore.width() - client.width()),
        std::max(0, frameBefore.height() - client.height()));
    const QSize clientLimit(std::max(1, usable.width() - frameMargins.width()),
                            std::max(1, usable.height() - frameMargins.height()));
    const QSize wanted = boundedToAvailable(size(), clientLimit);
    m_applyingPluginSize = true;
    setMaximumSize(clientLimit);
    if (wanted != size()) resize(wanted);

    QRect frame = frameGeometry();
    QPoint topLeft = frame.topLeft();
    if (centerOnParent && parentWidget()) {
        topLeft = parentWidget()->frameGeometry().center() -
                  QPoint(frame.width() / 2, frame.height() / 2);
    }
    topLeft.setX(std::clamp(topLeft.x(), usable.left(),
                            std::max(usable.left(), usable.right() - frame.width() + 1)));
    topLeft.setY(std::clamp(topLeft.y(), usable.top(),
                            std::max(usable.top(), usable.bottom() - frame.height() + 1)));
    move(topLeft);
    m_applyingPluginSize = false;
}

void PluginEditorWindow::onEditorClosed() noexcept {
    // The plugin closed itself. Tearing the window down from inside a plugin
    // callback would destroy the very object still on the stack, so it is
    // deferred to the event loop.
    const auto generation = m_loadGeneration;
    const auto identity = m_openedOn ? m_openedOn : m_pendingEditor;
    QMetaObject::invokeMethod(this, [this, generation, identity] {
        if (!m_closing && identity && identity == m_openedOn &&
            generation == m_loadGeneration && m_controller &&
            m_controller->insertIdentity(m_channelKey, m_insertKey) == identity) close();
    }, Qt::QueuedConnection);
}

double PluginEditorWindow::contentScaleFactor() const noexcept {
    // The foreign view is attached to the container, so that native surface's
    // screen decides its scale. Reading the outer widget before it is shown is
    // what can report the scale of the previous/primary screen instead.
    return m_container ? m_container->devicePixelRatioF() : devicePixelRatioF();
}

// ── Fallback panel ─────────────────────────────────────────────────────────

QWidget* PluginEditorWindow::buildParameterDock() {
    auto* dock = new QWidget(m_content);
    dock->setObjectName(QStringLiteral("PluginParamDock"));
    dock->setFixedWidth(kDockWidth);
    auto* outer = new QVBoxLayout(dock);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);
    outer->addWidget(ui::sectionLabel(tr("PARAMETERS"), dock));

    auto* hint = new QLabel(
        tr("Right-click a knob to create automation, or enable automation creation in the toolbar and double-click"), dock);
    hint->setObjectName(QStringLiteral("PluginHint"));
    hint->setWordWrap(true);
    outer->addWidget(hint);

    auto* scroll = new QScrollArea(dock);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* grid = new QWidget(scroll);
    m_dockGrid = new QGridLayout(grid);
    m_dockGrid->setContentsMargins(0, 0, 0, 0);
    m_dockGrid->setHorizontalSpacing(4);
    m_dockGrid->setVerticalSpacing(6);

    const std::vector<daw::plugins::ParameterInfo> parameters =
        m_controller ? m_controller->insertParameters(m_channelKey, m_insertKey)
                     : std::vector<daw::plugins::ParameterInfo>{};
    for (const daw::plugins::ParameterInfo& parameter : parameters) {
        // A parameter the plugin says cannot be automated has no business on a
        // panel whose whole purpose is automating things.
        if (!parameter.isAutomatable) continue;
        const QString parameterId = QString::fromStdString(parameter.id);
        const std::string parameterKey = parameter.id;
        const auto parameterIndex = std::int32_t(parameter.index);

        // One cell per parameter: a badge that says when the plugin is moving
        // this one, the knob, and the value in the plugin's own words. The
        // badge keeps its height while it is empty, so a parameter becoming
        // active does not shuffle the grid under the pointer.
        auto* cell = new QWidget(grid);
        cell->setObjectName(QStringLiteral("PluginParamCell"));
        cell->setProperty("active", false);
        cell->setProperty("parameterId", parameterId);
        auto* column = new QVBoxLayout(cell);
        column->setContentsMargins(3, 2, 3, 3);
        column->setSpacing(1);

        auto* badge = new QLabel(cell);
        badge->setObjectName(QStringLiteral("PluginParamBadge"));
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedHeight(11);
        column->addWidget(badge);

        auto* knob = new ui::Knob(QString::fromStdString(parameter.name), cell);
        knob->setRange(parameter.minValue, parameter.maxValue);
        knob->setDefaultValue(parameter.defaultValue);
        knob->setStepped(parameter.isStepped);
        knob->setCompact(true);
        knob->setAutomatable(true);
        // The caption elides at this width, so the full name has to be
        // reachable some other way.
        knob->setToolTip(QString::fromStdString(parameter.name));
        knob->setProperty("parameterId", parameterId);
        if (const auto* track = m_controller->project().findTrack(m_channelKey);
            track && track->instrument.id == m_insertKey) {
            knob->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(knob, &QWidget::customContextMenuRequested, this, [this, knob, parameterId, parameterKey](const QPoint& pos) {
                QMenu menu(this);
                auto* automate = menu.addAction(tr("Automate"));
                auto* learn = menu.addAction(tr("Назначить MIDI-ручку"));
                auto* cancel = menu.addAction(tr("Отменить обучение"));
                cancel->setEnabled(m_controller->isMidiLearning());
                auto* remove = menu.addAction(tr("Удалить назначение"));
                const auto* picked = menu.exec(knob->mapToGlobal(pos));
                if (picked == automate) emit automationRequested(m_channelId, m_insertId, parameterId);
                if (picked == learn) m_controller->beginMidiLearn(m_channelKey, parameterKey);
                if (picked == cancel) m_controller->cancelMidiLearn();
                if (picked == remove) m_controller->removeMidiLearn(m_channelKey, parameterKey);
            });
        }
        knob->setValue(m_controller->insertParameter(m_channelKey, m_insertKey, parameterKey));
        // The plugin's own words for the value — "440 Hz", "2:1" — rather than
        // a raw number, which is often meaningless.
        knob->setFormatter([this, parameterKey, parameterIndex](double plain) -> QString {
            return parameterText(parameterKey, plain, parameterIndex);
        });
        connect(knob, &ui::Knob::valueChanged, this,
                [this, parameterKey](double plain) {
                    if (!m_controller) return;
                    m_controller->setInsertParameter(
                        m_channelKey, m_insertKey, parameterKey, plain);
                });
        connect(knob, &ui::Knob::editFinished, this, [this] { emit projectEdited(); });
        connect(knob, &ui::Knob::automateRequested, this, [this, parameterId] {
            emit automationRequested(m_channelId, m_insertId, parameterId);
        });
        column->addWidget(knob);

        auto* value = new QLabel(cell);
        value->setObjectName(QStringLiteral("PluginParamValue"));
        value->setAlignment(Qt::AlignCenter);
        value->setText(parameterText(parameterKey, knob->value(), parameterIndex));
        column->addWidget(value);

        m_dockCells.append(cell);
        m_dockControls.push_back(
            DockControl{cell, knob, value, badge, parameterId, knob->value()});
        m_dockValues.push_back({parameterKey, parameterIndex, knob->value(), true});
    }
    if (m_dockCells.isEmpty()) {
        auto* empty = new QLabel(tr("Nothing here can be automated."), grid);
        empty->setObjectName(QStringLiteral("PluginHint"));
        empty->setWordWrap(true);
        m_dockGrid->addWidget(empty, 0, 0, 1, 2);
    }
    layOutParameterDock();
    scroll->setWidget(grid);
    outer->addWidget(scroll, 1);
    return dock;
}

QString PluginEditorWindow::parameterText(const std::string& parameterId,
    double plain, std::int32_t indexHint) const {
    if (m_controller) {
        const auto text = m_controller->insertParameterText(
            m_channelKey, m_insertKey, parameterId, plain, indexHint);
        if (!text.empty()) return QString::fromStdString(text);
    }
    return QString::number(plain, 'g', 4);
}

void PluginEditorWindow::layOutParameterDock() {
    if (!m_dockGrid) return;
    for (QWidget* cell : m_dockCells) m_dockGrid->removeWidget(cell);
    int at = 0;
    for (QWidget* cell : m_dockCells) {
        m_dockGrid->addWidget(cell, at / 2, at % 2);
        ++at;
    }
    m_dockGrid->setRowStretch(m_dockGrid->rowCount(), 1);
}

void PluginEditorWindow::refreshParameterDock() {
    if (!m_dock || !m_dock->isVisible() || !m_controller) return;
    m_controller->readInsertParameters(m_channelKey, m_insertKey, m_dockValues);

    QString touched;
    for (std::size_t i = 0; i < m_dockControls.size(); ++i) {
        auto& control = m_dockControls[i];
        const auto& value = m_dockValues[i];
        if (control.knob->isEditing()) continue;
        if (!value.available) continue;
        const double plain = value.value;
        const bool knobChanged =
            std::abs(plain - control.knob->value()) > 1e-9;
        const bool valueChanged = !std::isfinite(control.lastPlain) ||
                                  std::abs(plain - control.lastPlain) > 1e-9;
        if (!knobChanged && !valueChanged) continue;
        if (knobChanged) {
            // The value moved and it was not this knob that moved it — so it
            // was moved in the plugin's own GUI. That is the parameter the user
            // has their hand on, and the one worth putting at the top.
            touched = control.parameterId;
            QSignalBlocker block(control.knob);
            control.knob->setValue(plain);
        }
        // Some vendor formatters are surprisingly expensive. Ask only for a
        // parameter whose plain value actually changed (or whose control had
        // to be corrected), not for every parameter on every 200 ms poll.
        const QString text = parameterText(value.id, plain, value.index);
        if (control.value->text() != text) {
            control.value->setText(text);
        }
        control.lastPlain = plain;
    }
    if (touched.isEmpty() || touched == m_dockActive) return;

    m_dockActive = touched;
    for (const DockControl& control : m_dockControls) {
        const bool active = control.parameterId == touched;
        if (control.cell->property("active").toBool() == active) continue;
        control.cell->setProperty("active", active);
        control.badge->setText(active ? tr("ACTIVE") : QString());
        // A dynamic property only reaches the stylesheet after a repolish.
        control.cell->style()->unpolish(control.cell);
        control.cell->style()->polish(control.cell);
    }
    // To the front, so the one being turned is the one in view — a plugin with
    // eighty parameters is otherwise a scroll hunt every time.
    for (int i = 0; i < m_dockCells.size(); ++i) {
        if (m_dockCells.at(i)->property("parameterId").toString() != touched) continue;
        m_dockCells.move(i, 0);
        break;
    }
    layOutParameterDock();
}

void PluginEditorWindow::showParameterPanel() {
    if (m_dockToggle) m_dockToggle->setChecked(true);
}

void PluginEditorWindow::setParameterDockVisibleForTest(bool visible) {
    setParameterDockVisible(visible);
    if (m_dockToggle) {
        QSignalBlocker block(m_dockToggle);
        m_dockToggle->setChecked(visible);
    }
}

void PluginEditorWindow::pollForTest() {
    pollEditorState();
}

bool PluginEditorWindow::checkIdleForTest(daw::EngineController& controller,
                                         const std::string& fixturePath) {
    auto* factory = daw::plugins::factoryFor(daw::plugins::Format::Vst);
    if (!factory) return false;
    const auto descriptors = factory->inspect(fixturePath);
    const auto found = std::find_if(descriptors.begin(), descriptors.end(),
        [](const auto& d) { return d.uid == "54465831"; });
    if (found == descriptors.end()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Editor idle test");
    const auto slot = controller.addInsert(track, *found);
    auto* plugin = controller.insertInstance(track, slot);
    if (!plugin) return false;
    auto* editor = new PluginEditorWindow(&controller, QString::fromStdString(track),
                                          QString::fromStdString(slot));
    const QString placementKey = QStringLiteral("tests/pluginEditorSize/") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QWidget workspace;
    workspace.resize(1000, 700);
    workspace.show();
    InternalEditorFrame frame(placementKey, &workspace);
    frame.setContent(editor);
    frame.present();
    const auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto calls = [&] { return int(std::lround(plugin->parameterValue(4) * 1000)); };
    editor->initializeEditor();
    wait(1000);
    bool ok = editor->isEmbedded() && editor->m_editorIdle->isActive();
    const auto fitsNativeSize = [&](const QSize& nativeSize) {
        const QSize contentSize(nativeSize.width() + editor->dockWidth(),
                                nativeSize.height() + editor->m_wrapper->height());
        return editor->size() == contentSize &&
               editor->m_container && editor->m_container->size() == nativeSize &&
               frame.size() == contentSize + QSize(2, 24);
    };
    const bool smallEditorFits = fitsNativeSize(QSize(320, 120));
    // Simulate the same host callback a plugin sends when it changes its GUI.
    editor->onEditorResized(240, 100);
    wait(20);
    const bool smallerEditorFits = fitsNativeSize(QSize(240, 100));
    editor->setParameterDockVisibleForTest(true);
    wait(20);
    const bool dockFits = fitsNativeSize(QSize(240, 100));
    editor->setParameterDockVisibleForTest(false);
    wait(20);
    const bool dockClosedFits = fitsNativeSize(QSize(240, 100));
    frame.hide();
    frame.present();
    editor->prepareForPresentation();
    wait(20);
    const bool reopenedFits = fitsNativeSize(QSize(240, 100));
    editor->onEditorResized(320, 120);
    wait(20);
    ok &= smallEditorFits && smallerEditorFits && dockFits && dockClosedFits && reopenedFits;
    std::fprintf(stderr,
        "%s plugin editor native size: small=%d, resize=%d, dock=%d/%d, reopen=%d\n",
        ok ? "PASS" : "FAIL", int(smallEditorFits), int(smallerEditorFits),
        int(dockFits), int(dockClosedFits), int(reopenedFits));

    // Same stopped-transport scenario as a plugin's own groove preview. With
    // the dedicated timer paused, only the old generation-gated path remains.
    editor->m_editorIdle->stop();
    int before = calls();
    wait(400);
    const int oldPathCalls = calls() - before;
    editor->syncPollTimer();
    before = calls();
    const auto scans = controller.pluginEventScanCountForTest();
    wait(400);
    const int visibleCalls = calls() - before;
    const auto fullScans = controller.pluginEventScanCountForTest() - scans;
    ok &= visibleCalls >= 10 && fullScans <= 2;

    editor->hide();
    before = calls();
    wait(200);
    const int hiddenCalls = calls() - before;
    ok &= !editor->m_editorIdle->isActive() && hiddenCalls <= 1;
    editor->show();
    before = calls();
    wait(400);
    const int restoredCalls = calls() - before;
    ok &= restoredCalls >= 10;
    editor->detachFromPlugin();
    before = calls();
    wait(100);
    ok &= !editor->m_editorIdle->isActive() && calls() == before;

    std::fprintf(stderr,
        "%s plugin editor idle: old=%d/400ms, visible=%d/400ms, hidden=%d/200ms, "
        "restored=%d/400ms, full slot scans=%llu\n", ok ? "PASS" : "FAIL",
        oldPathCalls, visibleCalls, hiddenCalls, restoredCalls,
        static_cast<unsigned long long>(fullScans));

    // Keep an old wrapper alive across remove/Undo, then open the recreated
    // slot in a new wrapper. A late close/idle/resize from the old host must
    // never reach the new native view, even if the allocator reused an address.
    editor->scheduleEditorInitialization(0);
    wait(250);
    editor->m_poll->stop();
    editor->m_editorIdle->stop();
    const auto retired = editor->m_openedOn;
    editor->onEditorClosed();
    controller.removeInsert(track, slot);
    controller.undo();
    ok &= controller.setInsertChannelMode(track, slot, daw::PluginChannelMode::DualMono);
    auto* successor = new PluginEditorWindow(&controller, QString::fromStdString(track),
                                              QString::fromStdString(slot));
    successor->show();
    successor->initializeEditor();
    wait(500);
    const auto current = controller.insertEditorSnapshot(track, slot);
    const bool rejectsRetired = retired && current && retired != current->identity &&
        successor->isEmbedded() && current->open && !editor->isClosing() &&
        !controller.closeInsertEditor(track, slot, retired) &&
        !controller.pumpInsertEditor(track, slot, retired) &&
        !controller.insertEditorSize(track, slot, retired) &&
        !controller.resizeInsertEditor(track, slot, retired, {600, 400}) &&
        !controller.openInsertEditor(track, slot, retired);
    editor->detachFromPlugin();
    const auto afterLateDetach = controller.insertEditorSnapshot(track, slot);
    const bool keepsSuccessor = afterLateDetach && afterLateDetach->open && successor->isEmbedded();
    ok &= rejectsRetired && keepsSuccessor;
    std::fprintf(stderr, "%s retired editor cannot close or service its successor\n",
                 rejectsRetired && keepsSuccessor ? "PASS" : "FAIL");
    auto* leftInstance = controller.insertInstance(track, slot);
    controller.setInsertEditorChannel(track, slot, daw::PluginEditorChannel::Right);
    successor->detachFromPlugin();
    const bool closesOwnSide = leftInstance && !leftInstance->isEditorOpen();
    ok &= closesOwnSide;
    std::fprintf(stderr, "%s native editor detaches its own side after selection changes\n",
                 closesOwnSide ? "PASS" : "FAIL");
    delete successor;
    delete editor;
    QSettings().remove(placementKey);
    controller.removeTrack(track);
    return ok;
}

QStringList PluginEditorWindow::parameterDockOrderForTest() const {
    QStringList order;
    for (const QWidget* cell : m_dockCells) {
        order << cell->property("parameterId").toString();
    }
    return order;
}

void PluginEditorWindow::setParameterDockVisible(bool visible) {
    // Thousands of parameter widgets have no place on the critical path that
    // opens the plugin's own GUI. Build them only when the user asks for the
    // host panel.
    if (visible && !m_dock) {
        m_dock = buildParameterDock();
        m_contentRow->addWidget(m_dock);
        m_dock->setVisible(false);
    }
    if (!m_dock || m_dock->isVisible() == visible) return;

    const QSize before = size();
    const int fallbackStep = visible ? kDockWidth : -kDockWidth;
    m_applyingPluginSize = true;
    m_dock->setVisible(visible);
    // Grow around the plugin's own accepted size, not around the possibly
    // constrained viewport Qt just laid out. Otherwise each dock toggle can
    // make a fixed-size native editor progressively smaller.
    const QSize requested =
        (m_nativeEditorSize.isValid() || m_fallbackContentSize.isValid())
            ? requestedContentSize()
            : QSize(std::max(200, before.width() + fallbackStep),
                    before.height());
    applyRequestedContentSize(requested);
    m_applyingPluginSize = false;

    if (visible) refreshParameterDock();
}

void PluginEditorWindow::buildGenericEditor() {
    m_generic = new QWidget(this);
    if (const auto plugin = editorSnapshot(); plugin && plugin->format == daw::plugins::Format::Internal) {
        pluginStyle::bind(m_generic);
        m_generic->setAutoFillBackground(true);
    }
    auto* outer = new QVBoxLayout(m_generic);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    const bool loaded = m_controller && m_controller->hasInsert(m_channelKey, m_insertKey);
    const std::vector<daw::plugins::ParameterInfo> parameters =
        m_controller ? m_controller->insertParameters(m_channelKey, m_insertKey)
                     : std::vector<daw::plugins::ParameterInfo>{};

    auto* header = ui::sectionLabel(
        m_nativeEditorFailed
            ? tr("PLUGIN GUI FAILED — GENERIC CONTROLS")
            : (loaded ? tr("NO EDITOR — GENERIC CONTROLS")
                      : tr("PLUGIN NOT LOADED")),
        m_generic);
    outer->addWidget(header);

    if (m_nativeEditorFailed) {
        auto* hint = new QLabel(
            tr("The native editor could not attach. These controls remain "
               "available; close the window and try opening it again."),
            m_generic);
        hint->setObjectName(QStringLiteral("PluginHint"));
        hint->setWordWrap(true);
        outer->addWidget(hint);
    }

    if (parameters.empty()) {
        auto* empty = new QLabel(tr("This plugin exposes no parameters."), m_generic);
        empty->setObjectName(QStringLiteral("PluginHint"));
        outer->addWidget(empty);
        outer->addStretch(1);
        m_contentRow->insertWidget(0, m_generic, 1);
        m_fallbackContentSize = QSize(420, 200);
        resize(m_fallbackContentSize);
        return;
    }

    auto* scroll = new QScrollArea(m_generic);
    scroll->setWidgetResizable(true);
    auto* rows = new QWidget(scroll);
    auto* grid = new QGridLayout(rows);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);

    int row = 0;
    for (const daw::plugins::ParameterInfo& parameter : parameters) {
        const QString parameterId = QString::fromStdString(parameter.id);
        const std::string parameterKey = parameter.id;
        auto* name = new QLabel(QString::fromStdString(parameter.name), rows);
        auto* slider = new ui::GlassSlider(Qt::Horizontal, rows);
        slider->setRange(0, kSliderSteps);
        auto* value = new QLabel(rows);
        value->setObjectName(QStringLiteral("PluginHint"));
        value->setMinimumWidth(90);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        // The id, not the index: a plugin can renumber its parameters between
        // versions, and the id is what the document stores.
        slider->setProperty("parameterId", parameterId);
        slider->setProperty("minValue", parameter.minValue);
        slider->setProperty("maxValue", parameter.maxValue);
        slider->setProperty("valueLabel", QVariant::fromValue<QObject*>(value));

        const auto parameterIndex = std::int32_t(parameter.index);
        const double current = m_controller->insertParameter(m_channelKey, m_insertKey, parameterKey);
        value->setText(parameterText(parameterKey, current, parameterIndex));
        const double span = parameter.maxValue - parameter.minValue;
        const double fraction = span > 0.0 ? (current - parameter.minValue) / span : 0.0;
        slider->setValue(int(fraction * kSliderSteps));

        connect(slider, &QSlider::valueChanged, this,
                [this, value, parameterKey, parameterIndex, minimum = parameter.minValue,
                 maximum = parameter.maxValue](int position) {
                    if (!m_controller) return;
                    const double plain = minimum + (maximum - minimum) *
                                                       (double(position) /
                                                        kSliderSteps);
                    m_controller->setInsertParameter(
                        m_channelKey, m_insertKey, parameterKey, plain);
                    const auto text = parameterText(parameterKey, plain, parameterIndex);
                    if (value->text() != text) value->setText(text);
                });
        // Undo gets one entry per gesture, not one per pixel of drag — the
        // same split `commitLaneEdit` uses for automation.
        connect(slider, &QSlider::sliderPressed, this,
                [this, slider, parameterKey] {
                    slider->setProperty(
                        "beforeValue",
                        m_controller ? m_controller->insertParameter(
                                           m_channelKey, m_insertKey,
                                           parameterKey)
                                     : 0.0);
                });
        connect(slider, &QSlider::sliderReleased, this,
                [this, slider, parameterKey] {
                    if (!m_controller) return;
                    const QVariant before = slider->property("beforeValue");
                    if (!before.isValid()) return;
                    m_controller->commitInsertParameterEdit(
                        m_channelKey, m_insertKey, parameterKey,
                        before.toDouble(), "Change Plugin Parameter");
                });

        // The dock is hidden in this branch — it would only repeat the list
        // that is already on screen — so the automate gesture gets a control of
        // its own on each row instead.
        auto* automate =
            new ui::IconButton(icons::Glyph::Automation,
                               tr("Automate this parameter"), rows);
        automate->setButtonSize(22, 22);
        automate->setEnabled(parameter.isAutomatable);
        connect(automate, &QAbstractButton::clicked, this, [this, parameterId] {
            emit automationRequested(m_channelId, m_insertId, parameterId);
        });

        grid->addWidget(name, row, 0);
        grid->addWidget(slider, row, 1);
        grid->addWidget(value, row, 2);
        grid->addWidget(automate, row, 3);
        m_genericControls.push_back(GenericControl{
            slider, value, parameter.minValue, parameter.maxValue, current});
        m_genericValues.push_back({parameterKey, parameterIndex, current, true});
        ++row;
    }

    scroll->setWidget(rows);
    outer->addWidget(scroll, 1);
    m_contentRow->insertWidget(0, m_generic, 1);
    m_fallbackContentSize = QSize(520, std::min(640, 120 + row * 30));
    resize(m_fallbackContentSize);
    refreshGenericEditor();
}

void PluginEditorWindow::refreshGenericEditor() {
    if (!m_generic || !m_controller) return;
    m_controller->readInsertParameters(m_channelKey, m_insertKey, m_genericValues);

    for (std::size_t i = 0; i < m_genericControls.size(); ++i) {
        auto& control = m_genericControls[i];
        const auto& value = m_genericValues[i];
        // Never fight the user: a slider being dragged owns its own value.
        if (control.slider->isSliderDown() || !value.available) continue;
        const double plain = value.value;
        const double span = control.maximum - control.minimum;
        const int position =
            span > 0.0
                ? int(((plain - control.minimum) / span) * kSliderSteps)
                : 0;
        const bool sliderChanged = position != control.slider->value();
        const bool valueChanged = !std::isfinite(control.lastPlain) ||
                                  std::abs(plain - control.lastPlain) > 1e-9;
        if (!sliderChanged && !valueChanged) continue;
        if (sliderChanged) {
            const QSignalBlocker block(control.slider);
            control.slider->setValue(position);
        }
        const auto text = parameterText(value.id, plain, value.index);
        if (control.value->text() != text) control.value->setText(text);
        control.lastPlain = plain;
    }
}

void PluginEditorWindow::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
PluginEditorWindow { background: %BG%; }
#PluginWrapper {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %TOP%,stop:1 %HEADER%);
    border-bottom: 1px solid %SEPARATOR%;
}
#PluginWrapperName { color: %TEXT%; font-size: 12px; font-weight: 600; }
#PluginRouting {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %WELL_TOP%,stop:1 %WELL%);
    border: 1px solid %SEPARATOR%; border-top-color: %WELL_EDGE%; border-bottom-color: %LIGHT%;
    border-radius: 6px;
}
#PluginRoutingDivider { background: %SEPARATOR%; }
#PluginMode, #PluginSidechain {
    color: %TEXT%;
    background: transparent;
    border: none;
    padding: 0;
    min-height: 0;
    font-size: 11px;
    font-weight: 500;
}
#PluginWrapper QToolButton {
    color: %TEXT2%;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 4px;
    padding: 0;
    font-size: 10px;
    font-weight: 600;
}
#PluginWrapper QToolButton:hover { color: %TEXT%; background: %HOVER%; }
#PluginWrapper QToolButton:checked {
    color: %TEXT%;
    background: %SELECTION%;
    border-color: %SEPARATOR%;
}
#PluginEditorLoading {
    color: %TEXT2%;
    background: %BG%;
    font-size: 12px;
}
#PluginHint { color: %TEXT2%; font-size: 11px; }
#PluginRuntimeStatus { color: %TEXT%; font-size: 12px; }
#PluginParamDock {
    background: %SURFACE%;
    border-left: 1px solid %SEPARATOR%;
}
#PluginParamCell {
    background: transparent;
    border: 1px solid transparent;
    border-radius: %RADIUS%px;
}
#PluginParamCell[active="true"] {
    background: %WELL%;
    border: 1px solid %ACCENT%;
}
#PluginParamBadge {
    color: %ACCENT%;
    font-size: 8px;
    font-weight: 700;
    letter-spacing: 1px;
}
#PluginParamValue { color: %TEXT2%; font-size: 9px; }
)").replace("%RADIUS%", QString::number(Theme::cornerRadius))
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%HEADER%", t.headerBackground.name())
        .replace("%TOP%", mixColors(t.headerBackground, t.surfaceElevated, 0.32).name())
        .replace("%WELL_TOP%", t.wellTop().name())
        .replace("%WELL_EDGE%", t.edgeDark(t.well()).name())
        .replace("%LIGHT%", t.edgeLight(t.headerBackground).name())
        .replace("%HOVER%", mixColors(t.headerBackground, t.textPrimary, 0.08).name())
        .replace("%ELEVATED%", t.surfaceElevated.name())
        .replace("%WELL%", t.well().name())
        .replace("%SEPARATOR%", t.separator().name())
        .replace("%SELECTION%", t.selection.name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%TEXT2%", t.textSecondary.name()));
    m_dockToggle->setIcon(icons::icon(icons::Glyph::Automation, t.textSecondary, 16));
    if (property("vlt.pitchChrome").toBool()) {
        setStyleSheet(styleSheet() + QStringLiteral(
            "PluginEditorWindow { background: transparent; }"));
    }
}
