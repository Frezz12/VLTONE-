#pragma once

#include "Icons.hpp"
#include "Host/PluginTypes.hpp"

#include <QColor>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <vector>

class QLineEdit;
class QVariantAnimation;
class PluginQuickAdderOverlay;

namespace daw { class EngineController; }

/// Compact plugin finder inside a selected track or audio clip's context-panel
/// island.
///
/// Collapsed, it is only a magnifying-glass glyph at the same height as the
/// other track actions. Expanded, it becomes a one-line search field while the
/// other actions step away. Results are painted by a separate child overlay
/// anchored below the island, so a long list covers the timeline instead of
/// resizing the toolbar or the Context Panel vertically.
///
/// The widget is a pure view over `EngineController`'s plugin manager. It owns
/// no document state: inserting a plugin is delegated to the caller through
/// signals, so the ContextPanel decides where in the chain the new slot lands.
class PluginQuickAdder : public QWidget {
    Q_OBJECT
public:
    PluginQuickAdder(daw::EngineController* controller, QWidget* parent = nullptr);
    ~PluginQuickAdder() override;

    /// The track this adder inserts into. Used to read the current effect count
    /// (for the "+3" badge) and to seed the "Suggested" section.
    void setTrackId(const QString& trackId);
    QString trackId() const { return m_trackId; }

    /// Route the chosen effect into one audio clip's private Clip FX chain.
    /// Instruments are omitted in this mode because Clip FX accepts effects.
    void setClipTarget(const QString& trackId, const QString& clipId);
    QString clipId() const { return m_clipId; }

    /// The accent colour the glass rim and highlights are tinted with.
    void setAccentColor(const QColor& color);

    /// Expand the search and focus the field. Used by the Ctrl+F shortcut.
    void openSearch();
    /// Collapse back to the magnifying-glass button.
    void closeSearch();

    bool isExpanded() const { return m_expanded; }

    /// Kept at toolbar height in both states; only width changes.
    int preferredHeight() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// A plugin was chosen. `insertId` is the new slot id ("" if it failed to
    /// load). `openEditor` is true when the caller should also open the GUI.
    void pluginInserted(const QString& insertId, bool openEditor);
    /// The user asked to open a plugin's editor window directly.
    void editorRequested(const QString& insertId);
    /// The widget changed width, so the context panel should re-layout the plate.
    void sizeChanged();
    /// Search took over (or released) the track-action row. The Context Panel
    /// hides the unrelated action group while this is true.
    void searchStateChanged(bool expanded);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

private:
    struct Entry {
        daw::plugins::PluginDescriptor descriptor;
        bool favorite = false;
        bool recent = false;
        bool suggested = false;
        QString section;   // Instruments / Effects / Utilities / Recently Used / Favorites
    };

    struct VisibleItem {
        const Entry* entry = nullptr;
        QString section;
    };

    struct HitRow {
        enum class Kind { Section, Plugin } kind = Kind::Plugin;
        QRect rect;
        QString section;
        int visibleIndex = -1;
    };

    void rebuildEntries();
    void applyFilter();
    void refreshFavorites();
    void refreshRecent();
    void refreshSuggested();
    void updateGeometryForState();
    void animateTo(bool expanded);
    void collapseImmediatelyForEditor();
    void insertCurrent(bool openEditor, bool keepOpen = false);
    void setHighlight(int index);
    void toggleFavorite(const Entry& entry);
    void rememberRecent(const daw::plugins::PluginDescriptor& descriptor);
    std::vector<HitRow> hitRows() const;
    int maximumScrollOffset() const;
    int sectionIndex(const QString& section) const;
    QString searchText(const Entry& entry) const;
    void scrollHighlightIntoView();
    void showOverlay();
    void hideOverlay();
    void positionOverlay();
    QRect listViewport() const;
    void paintOverlay(QPaintEvent*);
    void overlayMousePress(QMouseEvent*);
    void overlayMouseMove(QMouseEvent*);
    void overlayLeave();
    void overlayWheel(QWheelEvent*);

    /// The plugin at `index` in the visible list, or nullptr.
    const Entry* visibleAt(int index) const;
    int visibleCount() const;

    // ── State ──
    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    QString m_clipId;
    QColor m_accent;
    bool m_expanded = false;
    bool m_hover = false;
    bool m_pressed = false;
    bool m_loading = false;
    int m_highlight = -1;
    int m_scrollOffset = 0;
    double m_expandProgress = 0.0;   // 0 = collapsed, 1 = expanded

    // ── Data ──
    std::vector<Entry> m_all;        // every plugin, unfiltered
    std::vector<VisibleItem> m_visible;   // after the current filter
    QString m_filter;
    QVector<QString> m_favorites;    // plugin uids
    QVector<QString> m_recent;       // plugin uids, most recent first
    QVector<QString> m_suggested;    // plugin uids suggested for this track
    QVector<bool> m_sectionCollapsed;   // per-section collapse state
    int m_confirmIndex = -1;
    double m_confirmProgress = 0.0;

    // ── Sub-widgets ──
    QLineEdit* m_search = nullptr;
    QVariantAnimation* m_expandAnim = nullptr;
    QPointer<QVariantAnimation> m_confirmAnim;
    QWidget* m_overlay = nullptr;

    friend class PluginQuickAdderOverlay;
};
