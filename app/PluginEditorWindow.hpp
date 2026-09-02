#pragma once

#include "Host/PluginInstance.hpp"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <limits>
#include <string>
#include <vector>

namespace daw { class EngineController; }

class QEvent;
class QHideEvent;
class QTimer;
class QVBoxLayout;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;
class QShowEvent;
namespace ui {
class IconButton;
class Knob;
}

/// A window holding one plugin's own editor.
///
/// The plugin's view is parented into a native child widget: `QWidget::winId()`
/// is an `NSView*` on macOS and an `HWND` on Windows, which is exactly what
/// CLAP, VST3 and AU each want — so no per-platform source file is needed here.
///
/// The slot is addressed by (channelId, insertId) and **re-resolved on every
/// use**, never cached as a pointer: undo can rewrite the document, and a
/// rebuild can replace the live instance, underneath an open window. Same
/// discipline as PianoRollWindow.
class PluginEditorWindow : public QWidget, public daw::plugins::PluginEditorHost {
    Q_OBJECT
public:
    PluginEditorWindow(daw::EngineController* controller, QString channelId,
                       QString insertId, QWidget* parent = nullptr);
    ~PluginEditorWindow() override;

    const QString& channelId() const { return m_channelId; }
    const QString& insertId() const { return m_insertId; }
    const QString& pluginUid() const { return m_pluginUid; }

    /// False when the plugin has no GUI or refused to open one; the caller
    /// should fall back to the generic parameter panel.
    bool isEmbedded() const { return m_embedded; }

    /// Clear any maximized/full-screen state inherited from the main macOS
    /// Space and restore this editor as a bounded auxiliary window.
    void prepareForPresentation();

    /// Attach the foreign native view after MainWindow has put this widget in
    /// its final InternalEditorFrame and shown that frame. Calling winId()
    /// before that reparent can leave a plugin drawing into a dead NSView/HWND.
    ///
    /// This first marks the complete, final parent chain as native. Qt requires
    /// that chain for reliable stacking, clipping and input to an embedded
    /// native child; it must happen before the frame is shown.
    void prepareNativeHostHierarchy();
    void initializeEditor();
    bool isEditorInitialized() const { return m_editorReady; }

    /// Let go of the plugin's view *now*, because the plugin itself is about to
    /// be destroyed — a Replace, a Remove, an undo, a project being closed.
    ///
    /// Without this the window would keep the plugin's own NSView/HWND embedded
    /// in it past the plugin's lifetime and hand it back on a later close, by
    /// which time the slot holds a different plugin entirely.
    void detachFromPlugin();

    /// Headless checks and screenshots only. The dock's toggle is offered only
    /// where a plugin has a native view of its own, which no offscreen run can
    /// produce — so these open it, pump it, and read back what it is showing.
    void setParameterDockVisibleForTest(bool visible);
    void pollForTest();
    QStringList parameterDockOrderForTest() const;
    QString parameterDockActiveForTest() const { return m_dockActive; }
    /// Exercise the same screen-bound clamp as a real native editor without
    /// needing a third-party plugin in a headless test run.
    static QSize boundedWindowSizeForTest(const QSize& requested,
                                          const QSize& available);

    // ── PluginEditorHost ──
    void onEditorResized(std::uint32_t width, std::uint32_t height) noexcept override;
    void onEditorClosed() noexcept override;
    double contentScaleFactor() const noexcept override;

signals:
    /// The window is going away; the registry drops its entry.
    void closing(const QString& channelId, const QString& insertId);
    void nestedPluginEditorRequested(const QString& channelId,
                                     const QString& insertId);
    void projectEdited();
    /// Emitted only for the three application-owned Qt editors. Native and
    /// generic third-party plugin surfaces deliberately have no presence seam.
    void builtInPanelReady(QWidget* panel, const QString& stableTypeId);
    /// A parameter knob requested automation by gesture or context menu. The window itself
    /// cannot make a lane — that is the document's business — so it says which
    /// parameter and lets the main window do the work.
    void automationRequested(const QString& channelId, const QString& insertId,
                             const QString& parameterId);

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    /// The live plugin behind the slot, or null if it went away.
    daw::plugins::PluginInstance* instance() const;
    /// Build the fallback panel of sliders for a plugin with no GUI.
    void buildGenericEditor();
    void refreshGenericEditor();
    void pollEditorState();
    void syncPollTimer();
    void buildWrapper();
    void refreshWrapper();
    void scheduleEditorInitialization(int delayMs);
    void rebuildEditorContent();
    void tryAttachNativeEditor(std::uint64_t generation, int attempt);
    void finishNativeEditorOpen(daw::plugins::PluginInstance* plugin);
    void finishEditorContent();
    void showLoadingState();
    void hideLoadingState();
    void clearEditorContent();
    /// The column of parameter knobs that can sit *beside* a plugin's own view.
    ///
    /// A third-party GUI is a foreign native window: its mouse events never
    /// reach Qt, so host gestures and host context menus cannot be attached to
    /// the plugin's own controls. This panel is ours, carries the same
    /// parameters, and gives both creation paths somewhere to live.
    QWidget* buildParameterDock();
    void refreshParameterDock();
    /// Re-place the cells in the grid from `m_dockCells`, which is the order
    /// they are shown in — the promotion of a parameter the plugin is moving is
    /// nothing more than that list being reordered.
    void layOutParameterDock();
    /// The plugin's own words for a value — "440 Hz", "2:1" — or the number.
    QString parameterText(const QString& parameterId, double plain) const;
    QString parameterText(daw::plugins::PluginInstance* live,
                          std::int32_t parameterIndex, double plain) const;
    void setParameterDockVisible(bool visible);
    /// How much width the dock is taking right now — nothing when it is
    /// hidden. Every place the window's width is computed back from the
    /// plugin's own view has to add this, or opening the dock crops the plugin.
    int dockWidth() const;
    /// Content size requested by the foreign GUI, plus our wrapper/dock.
    /// Kept separately from QWidget::size(): an internal frame may temporarily
    /// lay the native viewport out smaller before its saved geometry is
    /// replaced, and reading that viewport back would preserve the crop.
    QSize requestedContentSize() const;
    void applyRequestedContentSize(const QSize& requested);
    QSize boundedWindowSize(const QSize& requested) const;
    void constrainToScreen(bool centerOnParent);
    void applyTheme();

    daw::EngineController* m_controller = nullptr;
    QString m_channelId;
    QString m_insertId;
    /// Immutable controller keys. Polling must not allocate two UTF-8 strings
    /// for every lookup, especially when a plugin exposes many parameters.
    std::string m_channelKey;
    std::string m_insertKey;
    QString m_pluginUid;

    QWidget* m_container = nullptr;      // native, holds the plugin's view
    QVBoxLayout* m_layout = nullptr;
    QWidget* m_wrapper = nullptr;
    ui::IconButton* m_power = nullptr;
    QLabel* m_pluginName = nullptr;
    QLabel* m_pluginFormat = nullptr;
    QComboBox* m_channelMode = nullptr;
    QToolButton* m_leftChannel = nullptr;
    QToolButton* m_rightChannel = nullptr;
    QComboBox* m_sidechain = nullptr;
    QWidget* m_generic = nullptr;        // fallback panel, when there is no GUI
    QWidget* m_loading = nullptr;        // visible until the native attach runs
    /// The row the editor content lives in: the plugin's view (or the fallback
    /// panel) and, beside it, our own parameter dock.
    QWidget* m_content = nullptr;
    class QHBoxLayout* m_contentRow = nullptr;
    QWidget* m_dock = nullptr;
    QToolButton* m_dockToggle = nullptr;
    class QGridLayout* m_dockGrid = nullptr;
    /// The dock's cells in display order. Reordered when the plugin moves a
    /// parameter, so the one being turned rises to the top.
    QList<QWidget*> m_dockCells;
    struct DockControl {
        QWidget* cell = nullptr;
        ui::Knob* knob = nullptr;
        QLabel* value = nullptr;
        QLabel* badge = nullptr;
        QString parameterId;
        std::string parameterKey;
        std::int32_t parameterIndex = -1;
        double lastPlain = std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<DockControl> m_dockControls;
    struct GenericControl {
        QSlider* slider = nullptr;
        QLabel* value = nullptr;
        std::string parameterKey;
        double minimum = 0.0;
        double maximum = 0.0;
        std::int32_t parameterIndex = -1;
        double lastPlain = std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<GenericControl> m_genericControls;
    /// Which parameter the plugin last moved by itself.
    QString m_dockActive;
    QString m_sidechainSignature;
    QTimer* m_poll = nullptr;            // refreshes wrapper + fallback panel
    bool m_refreshingWrapper = false;
    bool m_embedded = false;
    /// The last size accepted or reported by the plugin itself. This remains
    /// authoritative across hiding, saved host geometry, and Qt layout passes.
    QSize m_nativeEditorSize;
    /// Preferred size of the built-in/generic fallback. Unlike a top-level
    /// widget, a layout-managed child cannot reliably report a resize it asked
    /// for, so retain the request until the host frame applies it.
    QSize m_fallbackContentSize;
    bool m_editorInitialized = false;
    /// `initializeEditor()` means loading has been requested; readiness means
    /// the vendor view (or a usable generic fallback) is actually on screen.
    bool m_editorReady = false;
    /// Invalidates queued loading/attach turns when a plugin is replaced,
    /// closed, or its dual-mono side changes while an editor is still loading.
    std::uint64_t m_loadGeneration = 0;
    /// Compared only with the freshly re-resolved slot instance. It is never
    /// dereferenced after a queued turn because a project edit can retire it.
    daw::plugins::PluginInstance* m_pendingEditorPlugin = nullptr;
    bool m_nativeEditorFailed = false;
    bool m_rebuildingEditorContent = false;
    /// The instance the view was opened on. Compared, never dereferenced after
    /// the plugin retires: `instance()` re-reads the slot, and after a Replace
    /// that is a *different* plugin, which must not be told to close an editor
    /// it never opened.
    daw::plugins::PluginInstance* m_openedOn = nullptr;
    /// True while the window is resizing itself on the plugin's instruction, so
    /// the resulting resizeEvent is not echoed back as a host-side resize.
    bool m_applyingPluginSize = false;
    /// The first presentation is centred and clamped after native frame
    /// margins exist. Later user resizes are left alone.
    bool m_hasBeenPresented = false;
};
