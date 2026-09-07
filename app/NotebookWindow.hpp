#pragma once

#include <QWidget>
#include <QString>

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
class QWebEngineView;

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

public slots:
    void reloadSettings();

signals:
    void settingsRequested();
    void visibilityChanged(bool visible);
    void timedTextChanged();
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
    QWebEngineView* m_view = nullptr;
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
    bool m_contentDirty = false;
    bool m_backgroundPlaying = false;
    bool m_loadingTimedText = false;
};
