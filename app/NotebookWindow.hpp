#pragma once

#include <QWidget>
#include <QString>
#include "model/Document.hpp"

class QComboBox;
class QCheckBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QStackedWidget;
class QTabBar;
class QTimer;
namespace ui::graphics { class BrowserSurface; }

namespace daw { class EngineController; }
namespace ui { class IconButton; }

class NotebookWindow final : public QWidget {
    Q_OBJECT
public:
    explicit NotebookWindow(daw::EngineController* controller,
                            QWidget* parent = nullptr);
    ~NotebookWindow() override;
    void setDetached(bool detached);
    bool ownsEditorFocus() const;
    bool handleEditorCommand(const QString& command);
    bool checkTimedTextForTest(QString* error = nullptr);
    bool importedLegacyContent() const { return m_importedLegacyContent; }

public slots:
    void reloadSettings();
    void syncFromProject();

signals:
    void settingsRequested();
    void visibilityChanged(bool visible);
    void timedTextChanged();
    void projectContentChanged();
    void detachRequested();
    void closeRequested();

public:
    Q_INVOKABLE void receiveContent(const QString& html);
    Q_INVOKABLE void importPastedImage(const QString& dataUrl,
                                       const QString& description);
    Q_INVOKABLE void reportMediaError();

protected:
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildToolbar();
    void buildTimedTextPanel();
    void reloadTimedTextTable();
    void saveTimedTextTable();
    void captureCurrentLine();
    void setSelectedCueToPlayhead();
    void deleteSelectedCue();
    void refreshTimedTextFontChoices();
    void updateTimedTextButtons();
    void readCurrentLine();
    void updateTimedTextPosition();
    void renderDocument();
    QString pageHtml() const;
    void applyTheme();
    void runCommand(const QString& command, const QString& value = {});
    void chooseImage();
    void insertImageFile(const QString& path, const QString& description);
    void saveNow();
    void setSaveStatus(const QString& text, bool error = false);
    void updateMotionButton();

    daw::EngineController* m_controller = nullptr;
    ui::graphics::BrowserSurface* m_view = nullptr;
    QWidget* m_toolbar = nullptr;
    QComboBox* m_font = nullptr;
    QComboBox* m_size = nullptr;
    QComboBox* m_block = nullptr;
    QLabel* m_saveStatus = nullptr;
    ui::IconButton* m_motionButton = nullptr;
    ui::IconButton* m_detachButton = nullptr;
    QTabBar* m_tabs = nullptr;
    QStackedWidget* m_pages = nullptr;
    QWidget* m_formatControls = nullptr;
    QCheckBox* m_timedTextPlaybackButton = nullptr;
    QWidget* m_timedTextPanel = nullptr;
    QTableWidget* m_timedTextTable = nullptr;
    QComboBox* m_timedTextFont = nullptr;
    QLabel* m_timedTextStatus = nullptr;
    QPushButton* m_setCueTime = nullptr;
    QPushButton* m_deleteCue = nullptr;
    QPushButton* m_seekCue = nullptr;
    QLineEdit* m_cueText = nullptr;
    QLabel* m_cuePosition = nullptr;
    QLabel* m_cuePreview = nullptr;
    QTimer* m_positionTimer = nullptr;
    QTimer* m_saveTimer = nullptr;
    QTimer* m_reloadTimer = nullptr;
    QString m_content;
    std::vector<daw::NotebookCueModel> m_loadedCues;
    bool m_contentDirty = false;
    bool m_importedLegacyContent = false;
    bool m_backgroundPlaying = false;
    bool m_loadingTimedText = false;
};

// Expose only document commands to Chromium. Publishing the QWidget itself
// serializes its entire property tree and unrelated window signals.
class NotebookWebBridge final : public QObject {
    Q_OBJECT
public:
    explicit NotebookWebBridge(NotebookWindow* notebook)
        : QObject(notebook), m_notebook(notebook) {}
    Q_INVOKABLE void receiveContent(const QString& html) { m_notebook->receiveContent(html); }
    Q_INVOKABLE void importPastedImage(const QString& url, const QString& description) {
        m_notebook->importPastedImage(url, description);
    }
    Q_INVOKABLE void reportMediaError() { m_notebook->reportMediaError(); }
private:
    NotebookWindow* m_notebook;
};
