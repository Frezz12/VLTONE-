#pragma once

#include <QFrame>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>

#include <array>

class QLabel;
class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

namespace ui {
class IconButton;
}

/// A movable, resizable editor that remains a child of the DAW workspace.
///
/// Unlike a QWidget carrying Qt::Window (or a QDialog), this frame never gets
/// an HWND/NSWindow of its own.  The parent clips it, owns its stacking order,
/// and therefore keeps it inside the application on every platform.  Editors
/// only provide their content; this class owns the shared chrome and placement
/// policy so later editors can adopt the same interaction without duplicating
/// it.
class InternalEditorFrame final : public QFrame {
    Q_OBJECT
public:
    explicit InternalEditorFrame(QString settingsKey,
                                 QWidget* parent = nullptr);
    ~InternalEditorFrame() override;

    void setContent(QWidget* content);
    QWidget* content() const { return m_content; }
    /// A control surface outside the frame which still belongs to this editor.
    /// Mouse-down there must not deactivate the editor before the control gets
    /// its matching release/click.
    void setAccessoryWidget(QWidget* accessory);

    /// Restore the saved placement on first use, show, raise and activate.
    void present();
    void activateEditor();
    bool isEditorActive() const { return m_active; }

    void setMaximized(bool maximized);
    bool isMaximized() const { return m_maximized; }

    /// Resize the outer frame to a content-requested size while keeping it
    /// inside the workspace. Used by resizable plugin UIs and parameter docks.
    void resizeForContent(const QSize& contentSize);
    /// Largest content rectangle the workspace can expose after subtracting
    /// this frame's title bar and one-pixel contour.
    QSize maximumContentSize() const;

signals:
    void closeRequested();
    void activeChanged(bool active);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    enum Edge : int {
        NoEdge = 0,
        LeftEdge = 1,
        TopEdge = 2,
        RightEdge = 4,
        BottomEdge = 8,
    };

    void applyTheme();
    void setEditorActive(bool active);
    void restorePlacement();
    void savePlacement();
    void constrainToParent();
    QRect availableRect() const;
    QRect constrainedGeometry(const QRect& wanted) const;
    void updateResizeHandles();
    void updateMaximizeButton();
    void restoreContentFocus();
    bool belongsToFrame(const QWidget* widget) const;
    bool belongsToAccessory(const QWidget* widget) const;
    void installApplicationEventFilter();
    void uninstallApplicationEventFilter();
    QRect interactiveResizeGeometry(const QPoint& globalPosition) const;
    void queueInteractiveResize(const QRect& geometry);
    void applyPendingInteractiveResize();
    void cancelPendingInteractiveResize();

    QString m_settingsKey;
    QVBoxLayout* m_column = nullptr;
    QWidget* m_titleBar = nullptr;
    QLabel* m_title = nullptr;
    ui::IconButton* m_maximizeButton = nullptr;
    ui::IconButton* m_closeButton = nullptr;
    QPointer<QWidget> m_content;
    QPointer<QWidget> m_accessory;
    QPointer<QWidget> m_lastContentFocus;
    QSize m_preferredContentSize{1100, 640};
    std::array<QWidget*, 8> m_resizeHandles{};

    QRect m_restoreGeometry;
    QRect m_pressGeometry;
    QRect m_pendingResizeGeometry;
    QPoint m_pressGlobal;
    QTimer m_resizeApplyTimer;
    int m_resizeEdges = NoEdge;
    bool m_dragging = false;
    bool m_resizing = false;
    bool m_maximized = false;
    bool m_active = false;
    bool m_placementRestored = false;
    bool m_applicationEventFilterInstalled = false;
};
