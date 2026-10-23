#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>

class QCloseEvent;
class QHideEvent;
class QMoveEvent;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

/// An owned tool window. The OS supplies its title, borders, move and resize
/// behavior; the main application remains its owner and task-switching entry.
class InternalEditorFrame final : public QFrame {
    Q_OBJECT
public:
    explicit InternalEditorFrame(QString settingsKey, QWidget* parent = nullptr);
    ~InternalEditorFrame() override;

    void setContent(QWidget* content);
    QWidget* content() const { return m_content; }
    void setAccessoryWidget(QWidget* accessory);
    void setWorkspaceArea(QWidget* area);
    void prepareForNativeSurface();

    void present();
    void raiseEditor();
    void activateEditor();
    void deactivateEditor();
    bool isEditorActive() const { return m_active; }
    bool isInteracting() const { return m_interacting; }
    void setExpansionEnabled(bool enabled);
    void setMaximized(bool maximized);

    void resizeForContent(const QSize& contentSize);
    QSize maximumContentSize() const;
    /// Screen coordinates, like QWidget::move for a window.
    void moveWithinWorkspace(const QPoint& position);
    static bool checkPlacementForTest();

signals:
    void closeRequested();
    void activeChanged(bool active);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void applyTheme();
    void setEditorActive(bool active);
    void restorePlacement();
    void savePlacement();
    void constrainToScreen();
    QRect availableRect() const;
    bool belongsToFrame(const QWidget* widget) const;
    bool belongsToAccessory(const QWidget* widget) const;

    QString m_settingsKey;
    QVBoxLayout* m_column = nullptr;
    QPointer<QWidget> m_content;
    QPointer<QWidget> m_accessory;
    QPointer<QWidget> m_workspaceArea;
    QPointer<QWidget> m_lastContentFocus;
    QSize m_preferredContentSize{1100, 640};
    bool m_active = false;
    bool m_interacting = false;
    bool m_expansionEnabled = true;
    bool m_placementRestored = false;
};
