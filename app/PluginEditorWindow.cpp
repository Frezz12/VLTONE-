#include "PluginEditorWindow.hpp"

#include "Controls.hpp"
#include "EqualizerPanel.hpp"
#include "GravityPanel.hpp"
#include "InternalEditorFrame.hpp"
#include "EngineController.hpp"
#include "SamplerPanel.hpp"
#include "Theme.hpp"
#if defined(Q_OS_MACOS)
#include "PluginEditorWindowMac.hpp"
#endif

#include "Internal/SamplerInstance.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <algorithm>
#include <cmath>
#include <QCloseEvent>
#include <QEvent>
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
#include <QStyle>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

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

    if (daw::plugins::PluginInstance* plugin = instance()) {
        setWindowTitle(QString::fromStdString(plugin->descriptor().name));
        m_pluginUid = QString::fromStdString(plugin->descriptor().uid);
    } else {
        setWindowTitle(tr("Plugin"));
    }

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this,
            &PluginEditorWindow::pollEditorState);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &PluginEditorWindow::applyTheme);
    applyTheme();
}

PluginEditorWindow::~PluginEditorWindow() {
    // The plugin must let go of the view before Qt destroys it, or the plugin
    // is left drawing into freed memory.
    detachFromPlugin();
}

void PluginEditorWindow::initializeEditor() {
    if (m_editorInitialized) return;
    m_editorInitialized = true;
    scheduleEditorInitialization(kPluginSettleMs);
}

void PluginEditorWindow::prepareNativeHostHierarchy() {
    // Headless tests intentionally have no real native-window hierarchy. A
    // forced one changes their focus/shortcut routing without exercising any
    // vendor editor, so leave those platform plugins as ordinary widgets.
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") ||
        platform == QLatin1String("minimal")) {
        return;
    }
    // `m_content` is the eventual parent of the vendor surface. Mark every
    // parent through the workspace host before any one of them is shown. Qt's
    // native-child contract relies on this for correct clipping, z-order and
    // hit testing. In particular, do not let the first plugin become a lone
    // NSView/HWND punched through an otherwise alien-widget hierarchy.
    //
    // The attribute itself matters beyond creating the handle: QWidget::raise()
    // only reorders the *native* view of a widget that carries it, and the
    // frame around this editor has to be able to come to the front of the
    // workspace once a foreign view lives inside it.
    for (QWidget* widget = m_content; widget && !widget->isWindow();
         widget = widget->parentWidget()) {
        widget->setAttribute(Qt::WA_NativeWindow);
    }
}

void PluginEditorWindow::scheduleEditorInitialization(int delayMs) {
    const std::uint64_t generation = ++m_loadGeneration;
    m_editorReady = false;
    m_pendingEditorPlugin = nullptr;
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
    m_wrapper->setFixedHeight(38);
    auto* row = new QHBoxLayout(m_wrapper);
    row->setContentsMargins(7, 4, 7, 4);
    row->setSpacing(6);

    m_power = new ui::IconButton(icons::Glyph::Power,
                                 tr("Enable or bypass this plugin"), m_wrapper);
    m_power->setObjectName(QStringLiteral("PluginPower"));
    m_power->setCheckable(true);
    m_power->setButtonSize(28, 28);
    m_power->setAccessibleName(tr("Plugin enabled"));
    row->addWidget(m_power);

    auto* identity = new QWidget(m_wrapper);
    auto* identityLayout = new QVBoxLayout(identity);
    identityLayout->setContentsMargins(2, 0, 4, 0);
    identityLayout->setSpacing(0);
    m_pluginName = new QLabel(tr("Plugin"), identity);
    m_pluginName->setObjectName(QStringLiteral("PluginWrapperName"));
    m_pluginFormat = new QLabel(identity);
    m_pluginFormat->setObjectName(QStringLiteral("PluginWrapperFormat"));
    identityLayout->addWidget(m_pluginName);
    identityLayout->addWidget(m_pluginFormat);
    row->addWidget(identity, 1);

    m_dockToggle = new QToolButton(m_wrapper);
    m_dockToggle->setCheckable(true);
    m_dockToggle->setFixedSize(27, 27);
    m_dockToggle->setIcon(icons::icon(icons::Glyph::Automation, th().textPrimary));
    m_dockToggle->setToolTip(
        tr("Show this plugin's parameters — Alt/Option-double-click or "
           "right-click a knob to automate it"));
    m_dockToggle->setAccessibleName(tr("Show parameter panel"));
    m_dockToggle->setVisible(false);
    connect(m_dockToggle, &QToolButton::toggled, this,
            [this](bool on) { setParameterDockVisible(on); });
    row->addWidget(m_dockToggle);

    m_channelMode = new QComboBox(m_wrapper);
    m_channelMode->setObjectName(QStringLiteral("PluginMode"));
    m_channelMode->setAccessibleName(tr("Plugin channel mode"));
    m_channelMode->setToolTip(
        tr("Choose automatic, mono, stereo, or two independent mono instances"));
    m_channelMode->addItem(tr("Auto"), int(daw::PluginChannelMode::Auto));
    m_channelMode->addItem(tr("Mono"), int(daw::PluginChannelMode::Mono));
    m_channelMode->addItem(tr("Stereo"), int(daw::PluginChannelMode::Stereo));
    m_channelMode->addItem(tr("Dual Mono"), int(daw::PluginChannelMode::DualMono));
    row->addWidget(m_channelMode);

    m_leftChannel = new QToolButton(m_wrapper);
    m_rightChannel = new QToolButton(m_wrapper);
    for (QToolButton* button : {m_leftChannel, m_rightChannel}) {
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFixedSize(27, 27);
    }
    m_leftChannel->setText(QStringLiteral("L"));
    m_leftChannel->setToolTip(tr("Edit the left mono instance"));
    m_leftChannel->setAccessibleName(tr("Edit left dual-mono channel"));
    m_rightChannel->setText(QStringLiteral("R"));
    m_rightChannel->setToolTip(tr("Edit the right mono instance"));
    m_rightChannel->setAccessibleName(tr("Edit right dual-mono channel"));
    row->addWidget(m_leftChannel);
    row->addWidget(m_rightChannel);

    m_sidechain = new QComboBox(m_wrapper);
    m_sidechain->setObjectName(QStringLiteral("PluginSidechain"));
    m_sidechain->setAccessibleName(tr("Sidechain source"));
    m_sidechain->setMinimumWidth(150);
    row->addWidget(m_sidechain);
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
    connect(m_sidechain, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_controller || m_refreshingWrapper || index < 0 ||
            !m_sidechain->isEnabled()) {
            return;
        }
        const std::string source = m_sidechain->itemData(index).toString().toStdString();
        if (m_controller->setInsertSidechainSource(m_channelKey, m_insertKey,
                                                   source)) {
            emit projectEdited();
        }
        refreshWrapper();
    });
    refreshWrapper();
}

void PluginEditorWindow::clearEditorContent() {
    detachFromPlugin();
    m_nativeEditorSize = {};
    m_genericControls.clear();
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
    daw::plugins::PluginInstance* plugin = instance();
    QString title = tr("Plugin");
    if (plugin) {
        title = QString::fromStdString(plugin->descriptor().name);
        m_pluginUid = QString::fromStdString(plugin->descriptor().uid);
    }
    setWindowTitle(title);

    const bool hasNativeEditor = plugin && plugin->hasEditor();
    if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr,
                     "editor for '%s' (%s): instance %s, hasEditor %s\n",
                     title.toUtf8().constData(),
                     plugin ? std::string(daw::plugins::toString(
                                              plugin->descriptor().format)).c_str()
                            : "-",
                     plugin ? "yes" : "NO",
                     hasNativeEditor ? "yes" : "no");
    }

    if (hasNativeEditor) {
        m_container = new QWidget(m_content);
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
        m_pendingEditorPlugin = plugin;
        m_rebuildingEditorContent = false;
        showLoadingState();
        QTimer::singleShot(kNativeMapRetryMs, this,
                           [this, generation] {
                               tryAttachNativeEditor(generation, 0);
                           });
        return;
    }

    if (dynamic_cast<daw::plugins::equalizer::EqualizerInstance*>(plugin)) {
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
        // The branded EQ view itself is 820x520 minimum; this host widget also
        // owns the 39 px plugin header above it.
        setMinimumSize(820, 559);
        m_fallbackContentSize = QSize(1040, 680);
        resize(m_fallbackContentSize);
    } else if (dynamic_cast<daw::plugins::gravity::GravityInstance*>(plugin)) {
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
        setMinimumSize(820, 638);
        m_fallbackContentSize = QSize(920, 718);
        resize(m_fallbackContentSize);
    } else if (dynamic_cast<daw::plugins::sampler::SamplerInstance*>(plugin)) {
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
        setMinimumSize(860, 558);
        m_fallbackContentSize = QSize(1080, 688);
        resize(m_fallbackContentSize);
    } else {
        buildGenericEditor();
    }
    finishEditorContent();
}

void PluginEditorWindow::tryAttachNativeEditor(std::uint64_t generation,
                                               int attempt) {
    if (generation != m_loadGeneration || !m_container) return;

    daw::plugins::PluginInstance* plugin = instance();
    if (!plugin || plugin != m_pendingEditorPlugin) {
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
        m_embedded = plugin->openEditor(reinterpret_cast<void*>(handle), this);
    }

    if (m_embedded) {
        m_openedOn = plugin;
        finishNativeEditorOpen(plugin);
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

void PluginEditorWindow::finishNativeEditorOpen(
    daw::plugins::PluginInstance* plugin) {
    if (!plugin || !m_container) return;
    std::uint32_t width = 0, height = 0;
    if (!plugin->editorSize(width, height) || width == 0 || height == 0) return;

    QSize wanted(int(width) + dockWidth(),
                 int(height) + m_wrapper->height());
    QSize bounded = boundedWindowSize(wanted);

    // If the GUI supports resizing, give it the compact content size before
    // its first uncovered frame. Fixed-size GUIs retain their natural pixels.
    if (plugin->editorCanResize() && bounded != wanted) {
        std::uint32_t boundedWidth = std::uint32_t(
            std::max(1, bounded.width() - dockWidth()));
        std::uint32_t boundedHeight = std::uint32_t(
            std::max(1, bounded.height() - m_wrapper->height()));
        if (plugin->setEditorSize(boundedWidth, boundedHeight)) {
            width = boundedWidth;
            height = boundedHeight;
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
    // The dock exists to reach parameters *through* a foreign native view. With
    // no such view there is nothing it can do that the panel on screen does not
    // already do, so it is not offered.
    if (m_dockToggle) m_dockToggle->setVisible(m_embedded);
    if (!m_embedded && m_dock) {
        m_dockToggle->setChecked(false);
        m_dock->setVisible(false);
    }
    refreshWrapper();
    m_pendingEditorPlugin = nullptr;
    m_editorReady = true;
    m_rebuildingEditorContent = false;
    hideLoadingState();
    prepareForPresentation();
}

void PluginEditorWindow::refreshWrapper() {
    if (!m_controller || !m_wrapper) return;
    const daw::InsertModel* model =
        m_controller->insertModel(m_channelKey, m_insertKey);
    if (!model) return;
    m_refreshingWrapper = true;

    m_power->setChecked(!model->bypassed);
    const int modeIndex = m_channelMode->findData(int(model->channelMode));
    if (modeIndex >= 0) m_channelMode->setCurrentIndex(modeIndex);
    const bool dual = model->channelMode == daw::PluginChannelMode::DualMono;
    m_leftChannel->setVisible(dual);
    m_rightChannel->setVisible(dual);
    m_leftChannel->setChecked(model->editorChannel ==
                              daw::PluginEditorChannel::Left);
    m_rightChannel->setChecked(model->editorChannel ==
                               daw::PluginEditorChannel::Right);

    if (daw::plugins::PluginInstance* plugin = instance()) {
        m_pluginName->setText(QString::fromStdString(plugin->descriptor().name));
        m_pluginFormat->setText(QString::fromStdString(
            std::string(daw::plugins::toString(plugin->descriptor().format))));
    } else {
        m_pluginName->setText(QString::fromStdString(model->name));
        m_pluginFormat->setText(tr("not loaded"));
    }

    const bool supports =
        m_controller->insertSupportsSidechain(m_channelKey, m_insertKey);
    if (!supports) {
        const QString signature = QStringLiteral("unsupported");
        if (m_sidechainSignature != signature) {
            m_sidechain->clear();
            m_sidechain->addItem(tr("No sidechain input"));
            m_sidechainSignature = signature;
        }
        m_sidechain->setEnabled(false);
        m_sidechain->setToolTip(
            tr("This plugin exposes no auxiliary audio input"));
        m_refreshingWrapper = false;
        return;
    }

    // Source discovery checks the complete routing graph for feedback. It is
    // needed only for plugins that actually expose an auxiliary input.
    const auto sources = m_controller->insertSidechainSources(m_channelKey);
    QStringList signature{QStringLiteral("supported")};
    for (const auto& source : sources) {
        signature << QString::fromStdString(source.id);
    }
    const QString signatureText = signature.join(QLatin1Char('|'));
    if (m_sidechainSignature != signatureText) {
        m_sidechain->clear();
        m_sidechain->addItem(tr("Side Chain: Off"), QString());
        for (const auto& source : sources) {
            m_sidechain->addItem(
                tr("Side Chain: %1").arg(QString::fromStdString(source.name)),
                QString::fromStdString(source.id));
        }
        m_sidechainSignature = signatureText;
    }
    m_sidechain->setEnabled(true);
    const QString current = QString::fromStdString(model->sidechainTrackId);
    int sourceIndex = m_sidechain->findData(current);
    if (sourceIndex < 0 && !current.isEmpty()) {
        m_sidechain->addItem(tr("Side Chain: missing source"), current);
        sourceIndex = m_sidechain->count() - 1;
    }
    m_sidechain->setCurrentIndex(std::max(sourceIndex, 0));
    m_sidechain->setToolTip(
        tr("Choose the post-fader signal sent to the plugin's auxiliary input"));
    m_refreshingWrapper = false;
}

void PluginEditorWindow::detachFromPlugin() {
    ++m_loadGeneration;
    m_pendingEditorPlugin = nullptr;
    m_editorReady = false;
    if (!m_embedded) return;
    m_embedded = false;
    // Only the instance the view was opened on may be told to close it. After a
    // Replace the slot holds a different plugin, and `closeEditor` on that one
    // would be a call about a window it never opened.
    if (daw::plugins::PluginInstance* plugin = instance();
        plugin && plugin == m_openedOn) {
        plugin->closeEditor();
    }
    m_openedOn = nullptr;
}

daw::plugins::PluginInstance* PluginEditorWindow::instance() const {
    if (!m_controller) return nullptr;
    return m_controller->insertInstance(m_channelKey, m_insertKey);
}

void PluginEditorWindow::pollEditorState() {
    refreshWrapper();
    refreshGenericEditor();
    refreshParameterDock();
}

void PluginEditorWindow::syncPollTimer() {
    if (!m_poll) return;
    const bool shouldPoll = isVisible() && !isMinimized();
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

void PluginEditorWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) syncPollTimer();
}

void PluginEditorWindow::closeEvent(QCloseEvent* event) {
    emit closing(m_channelId, m_insertId);
    QWidget::closeEvent(event);
}

void PluginEditorWindow::hideEvent(QHideEvent* event) {
    if (m_poll) m_poll->stop();
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
    daw::plugins::PluginInstance* plugin = instance();
    if (!plugin || !plugin->editorCanResize()) return;

    // Offer the new size; the plugin snaps it to something it can draw and
    // writes back what it settled on, which the container then takes.
    std::uint32_t width = std::uint32_t(m_container->width());
    std::uint32_t height = std::uint32_t(m_container->height());
    if (!plugin->setEditorSize(width, height)) return;
    m_nativeEditorSize = QSize(int(width), int(height));
    if (int(width) == m_container->width() && int(height) == m_container->height()) {
        return;
    }
    m_applyingPluginSize = true;
    m_container->resize(int(width), int(height));
    m_applyingPluginSize = false;
}

void PluginEditorWindow::showEvent(QShowEvent* event) {
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
    if (!m_container || width == 0 || height == 0) return;
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
    QMetaObject::invokeMethod(this, [this] { close(); }, Qt::QueuedConnection);
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
        tr("Alt/Option-double-click or right-click a knob to automate it"), dock);
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

    daw::plugins::PluginInstance* live = instance();
    const std::vector<daw::plugins::ParameterInfo> parameters =
        m_controller ? m_controller->insertParameters(m_channelKey, m_insertKey)
                     : std::vector<daw::plugins::ParameterInfo>{};
    for (const daw::plugins::ParameterInfo& parameter : parameters) {
        // A parameter the plugin says cannot be automated has no business on a
        // panel whose whole purpose is automating things.
        if (!parameter.isAutomatable) continue;
        const QString parameterId = QString::fromStdString(parameter.id);
        const std::string parameterKey = parameter.id;
        const std::int32_t parameterIndex =
            live ? live->parameterIndexForId(parameterKey) : -1;

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
        knob->setValue(parameterIndex >= 0
                           ? live->parameterValue(std::uint32_t(parameterIndex))
                           : parameter.defaultValue);
        // The plugin's own words for the value — "440 Hz", "2:1" — rather than
        // a raw number, which is often meaningless.
        knob->setFormatter([this, parameterId](double plain) -> QString {
            return parameterText(parameterId, plain);
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
        value->setText(parameterText(live, parameterIndex, knob->value()));
        column->addWidget(value);

        m_dockCells.append(cell);
        m_dockControls.push_back(
            DockControl{cell, knob, value, badge, parameterId, parameterKey,
                        parameterIndex, knob->value()});
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

QString PluginEditorWindow::parameterText(const QString& parameterId,
                                          double plain) const {
    if (daw::plugins::PluginInstance* live = instance()) {
        const std::int32_t index =
            live->parameterIndexForId(parameterId.toStdString());
        return parameterText(live, index, plain);
    }
    return QString::number(plain, 'g', 4);
}

QString PluginEditorWindow::parameterText(
    daw::plugins::PluginInstance* live, std::int32_t parameterIndex,
    double plain) const {
    if (live && parameterIndex >= 0) {
        const std::string text =
            live->parameterText(std::uint32_t(parameterIndex), plain);
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
    daw::plugins::PluginInstance* live = instance();

    QString touched;
    for (DockControl& control : m_dockControls) {
        if (control.knob->isEditing()) continue;

        std::int32_t index = -1;
        if (live) {
            const auto parameters = live->parameters();
            const bool cachedIndexValid =
                control.parameterIndex >= 0 &&
                std::size_t(control.parameterIndex) < parameters.size() &&
                parameters[std::size_t(control.parameterIndex)].id ==
                    control.parameterKey;
            if (!cachedIndexValid) {
                control.parameterIndex =
                    live->parameterIndexForId(control.parameterKey);
            }
            index = control.parameterIndex;
        }
        const double plain =
            index >= 0 ? live->parameterValue(std::uint32_t(index)) : 0.0;
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
        const QString text = parameterText(live, index, plain);
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
    auto* outer = new QVBoxLayout(m_generic);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    daw::plugins::PluginInstance* plugin = instance();
    const std::vector<daw::plugins::ParameterInfo> parameters =
        m_controller ? m_controller->insertParameters(m_channelKey, m_insertKey)
                     : std::vector<daw::plugins::ParameterInfo>{};

    auto* header = ui::sectionLabel(
        m_nativeEditorFailed
            ? tr("PLUGIN GUI FAILED — GENERIC CONTROLS")
            : (plugin ? tr("NO EDITOR — GENERIC CONTROLS")
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
        auto* slider = new QSlider(Qt::Horizontal, rows);
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

        const std::int32_t parameterIndex =
            plugin ? plugin->parameterIndexForId(parameterKey) : -1;
        const double current = parameterIndex >= 0
                                   ? plugin->parameterValue(
                                         std::uint32_t(parameterIndex))
                                   : parameter.defaultValue;
        const double span = parameter.maxValue - parameter.minValue;
        const double fraction = span > 0.0 ? (current - parameter.minValue) / span : 0.0;
        slider->setValue(int(fraction * kSliderSteps));

        connect(slider, &QSlider::valueChanged, this,
                [this, value, parameterKey, minimum = parameter.minValue,
                 maximum = parameter.maxValue](int position) {
                    if (!m_controller) return;
                    const double plain = minimum + (maximum - minimum) *
                                                       (double(position) /
                                                        kSliderSteps);
                    m_controller->setInsertParameter(
                        m_channelKey, m_insertKey, parameterKey, plain);
                    if (daw::plugins::PluginInstance* live = instance()) {
                        const std::int32_t index =
                            live->parameterIndexForId(parameterKey);
                        if (index >= 0) {
                            const QString text = QString::fromStdString(
                                live->parameterText(std::uint32_t(index), plain));
                            if (value->text() != text) value->setText(text);
                        }
                    }
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
            slider, value, parameterKey, parameter.minValue,
            parameter.maxValue, parameterIndex, current});
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
    daw::plugins::PluginInstance* plugin = instance();
    if (!plugin) return;

    for (GenericControl& control : m_genericControls) {
        // Never fight the user: a slider being dragged owns its own value.
        if (control.slider->isSliderDown()) continue;
        const auto parameters = plugin->parameters();
        const bool cachedIndexValid =
            control.parameterIndex >= 0 &&
            std::size_t(control.parameterIndex) < parameters.size() &&
            parameters[std::size_t(control.parameterIndex)].id ==
                control.parameterKey;
        if (!cachedIndexValid) {
            control.parameterIndex =
                plugin->parameterIndexForId(control.parameterKey);
        }
        const std::int32_t index = control.parameterIndex;
        const double plain = index >= 0
                                 ? plugin->parameterValue(std::uint32_t(index))
                                 : 0.0;
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
        if (index >= 0) {
            const QString text = QString::fromStdString(
                plugin->parameterText(std::uint32_t(index), plain));
            if (control.value->text() != text) control.value->setText(text);
        }
        control.lastPlain = plain;
    }
}

void PluginEditorWindow::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
PluginEditorWindow { background: %BG%; }
#PluginWrapper {
    background: %SURFACE%;
    border-bottom: 1px solid %SEPARATOR%;
}
#PluginWrapperName { color: %TEXT%; font-size: 12px; font-weight: 600; }
#PluginWrapperFormat { color: %TEXT2%; font-size: 9px; text-transform: uppercase; }
#PluginMode, #PluginSidechain {
    color: %TEXT%;
    background: %WELL%;
    border: 1px solid %SEPARATOR%;
    border-radius: 5px;
    padding: 3px 24px 3px 8px;
    min-height: 20px;
}
#PluginMode:hover, #PluginSidechain:hover { border-color: %ACCENT%; }
#PluginMode:focus, #PluginSidechain:focus { border: 1px solid %ACCENT%; }
#PluginMode QAbstractItemView, #PluginSidechain QAbstractItemView {
    color: %TEXT%;
    background: %ELEVATED%;
    border: 1px solid %SEPARATOR%;
    selection-background-color: %SELECTION%;
    selection-color: %TEXT%;
    outline: none;
}
#PluginWrapper QToolButton {
    color: %TEXT2%;
    background: %WELL%;
    border: 1px solid %SEPARATOR%;
    border-radius: 5px;
    font-weight: 700;
}
#PluginWrapper QToolButton:hover { color: %TEXT%; border-color: %ACCENT%; }
#PluginWrapper QToolButton:checked {
    color: %TEXT%;
    background: %SELECTION%;
    border-color: %ACCENT%;
}
#PluginEditorLoading {
    color: %TEXT2%;
    background: %BG%;
    font-size: 12px;
}
#PluginHint { color: %TEXT2%; font-size: 11px; }
#PluginParamDock {
    background: %SURFACE%;
    border-left: 1px solid %SEPARATOR%;
}
#PluginParamCell {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
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
)")
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%ELEVATED%", t.surfaceElevated.name())
        .replace("%WELL%", t.well().name())
        .replace("%SEPARATOR%", t.separator().name())
        .replace("%SELECTION%", t.selection.name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%TEXT2%", t.textSecondary.name()));
}
