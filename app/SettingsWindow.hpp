#pragma once

#include "Theme.hpp"

#include <QDialog>
#include <array>
#include <functional>
#include <QHash>
#include <QString>
#include <QPointer>
#include <QVector>

namespace daw { class EngineController; }

class RecordingSettingsPage;
class AudioSettingsPage;
class QuickImportSettingsPage;
class NotebookSettingsPage;
class ShortcutManager;
class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTabBar;
class QKeySequenceEdit;
class QPushButton;
class QLineEdit;
class QComboBox;
class QLabel;
class QShowEvent;
class QCheckBox;
class QSlider;
class QRadioButton;
class QFileSystemWatcher;
class QWidget;
class QTimer;

/// The unified, non-modal settings window: one place for Audio, Themes and
/// Keyboard Shortcuts, titled with the application name. Replaces the standalone
/// Audio Settings dialog and the Settings ▸ Theme submenu.
class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    SettingsWindow(daw::EngineController* controller, ShortcutManager* shortcuts,
                   QWidget* parent = nullptr);
    void refreshTimelineBackgroundSource();

    /// The settings pages, by name. Contextual entry points use these stable
    /// IDs even though the sidebar presents them in semantic groups.
    enum Tab {
        kAudioTab = 0,
        kQuickImportTab,
        kTransportTab,
        kRecordingTab,
        kContextPanelTab,
        kBrowserTab,
        kNotebookTab,
        kAiTab,
        kAccountTab,
        kLanguageTab,
        kRecoveryTab,
        kThemesTab,
        kThemeEditorTab,
        kShortcutsTab,
        kInterfaceTab,
        kMixerTab,
        kBackgroundsTab,
        kTimelineTab,
        kFontsTab,
        kPerformanceTab,
        kPluginsTab,
        kWebBrowserTab,
        kPageCount,
    };

    /// Bring a specific settings page to the front.
    void showTab(int index);

    /// Re-read the recording preferences. The mode also changes from the
    /// transport's Layers button, and an open window has to follow.
    void reloadRecordingPage();
    bool checkAudioPageForTest() const;
    static bool checkWheelRoutingForTest();
    bool checkOrganizationForTest();
    void showQuickImportError(const QString& message);
    /// Import and apply a .vlttheme delivered by a file picker, Finder or
    /// Explorer. The operation owns a private copy before returning success.
    void importThemeFile(const QString& path);
    QString importTrackIconFile();

signals:
    void transportPanelStyleChanged();
    /// A context-panel profile or its transparency setting changed.
    void contextPanelSettingsChanged();
    /// The browser's folders, side or preview options changed.
    void browserSettingsChanged();
    /// The notebook's background, motion or custom fonts changed.
    void notebookSettingsChanged();
    /// The assistant's provider, key, model or step limit changed.
    void aiSettingsChanged();
    /// MainWindow owns the unsaved-project decision before credentials vanish.
    void accountLogoutRequested();
    /// The global recording mode changed on the Recording tab.
    void recordModeChanged();
    /// The optional compact audio-CPU strip changed on the Audio tab.
    void cpuStatusBarVisibilityChanged(bool visible);
    /// How selected tracks are tinted changed on the Themes tab.
    void selectionTintChanged();
    /// Grid, clip and cursor preferences changed; repaint without rebuilding the style.
    void timelineAppearanceChanged();
    /// A local arrangement/header image, GIF, video or presentation changed.
    void themeBackgroundSettingsChanged();
    void restartRequested();

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* m_timelineBackgroundPath = nullptr;
    QPushButton* m_clearTimelineBackground = nullptr;
    QCheckBox* m_enableTimelineBackground = nullptr;
    /// Keep the dialog inside the current monitor's usable area. Every tab is
    /// scrollable, so shrinking the shell never hides a setting.
    void constrainToScreen();
    void ensurePage(int index);
    void refreshCurrentPage();
    std::array<std::function<QWidget*()>, kPageCount> m_pageFactories;
    QWidget* buildThemesTab();
    QWidget* buildBackgroundsTab();
    QWidget* buildTimelineTab();
    QWidget* buildFontsTab();
    QWidget* buildPerformanceTab();
    QWidget* buildInterfaceTab();
    QWidget* buildMixerTab();
    QWidget* buildThemeEditorTab();
    QWidget* buildShortcutsTab();
    QWidget* buildLanguageTab();
    void refreshLanguages();
    void refreshFontStatus();
    void refreshTrackIcons();
    void refreshThemeLibrary();
    void refreshThemeControls();
    void refreshStartupTemplateOptions();
    bool applyInstalledTheme(const QString& filePath, const QString& storageId);
    void saveCurrentThemeToLibrary();
    void exportCurrentTheme();
    void applySelectedSavedTheme();
    void refreshShortcutEditors();
    void refreshNavigationAppearance();
    void refreshAccount();
    void refreshSubtabs(int page);
    void preparePage(QWidget* page);
    void buildSearchIndex();
    void updateSearch();
    void activateSearchResult(QTreeWidgetItem* item);
    void revealSetting(QWidget* target);
    /// Push the working palette to the app (live), persisting it as "custom".
    void applyEditTheme();
    /// Repaint every colour swatch from the working palette.
    void refreshSwatches();

    daw::EngineController* m_controller = nullptr;
    ShortcutManager* m_shortcuts = nullptr;
    QTreeWidget* m_navigation = nullptr;
    QTabBar* m_subtabs = nullptr;
    QPushButton* m_accountButton = nullptr;
    QLabel* m_accountAvatar = nullptr;
    QLabel* m_accountName = nullptr;
    QLabel* m_accountCaption = nullptr;
    QStackedWidget* m_pages = nullptr;
    QStackedWidget* m_content = nullptr;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_searchResults = nullptr;
    QLabel* m_searchStatus = nullptr;
    QLabel* m_pageHint = nullptr;
    QTimer* m_searchTimer = nullptr;
    QPushButton* m_audioApply = nullptr;
    struct SearchEntry {
        int page;
        QString title;
        QString path;
        QString terms;
        QPointer<QWidget> target;
    };
    QVector<SearchEntry> m_searchIndex;
    QHash<int, QTreeWidgetItem*> m_navigationItems;
    QHash<int, QString> m_pageLabels;
    QVector<QVector<int>> m_categoryPages;
    QLabel* m_pageTitle = nullptr;
    QLabel* m_pageDescription = nullptr;
    RecordingSettingsPage* m_recordingPage = nullptr;
    AudioSettingsPage* m_audioPage = nullptr;
    QuickImportSettingsPage* m_quickImportPage = nullptr;
    NotebookSettingsPage* m_notebookPage = nullptr;
    QListWidget* m_themeList = nullptr;
    QListWidget* m_trackIconList = nullptr;
    QWidget* m_trackIconGroup = nullptr;
    QListWidget* m_savedThemeList = nullptr;
    QPushButton* m_applySavedTheme = nullptr;
    QPushButton* m_exportSavedTheme = nullptr;
    QFileSystemWatcher* m_themeLibraryWatcher = nullptr;
    QHash<QString, QKeySequenceEdit*> m_editors;   // command id → editor

    Theme m_editTheme;                             // the working custom palette
    QLineEdit* m_themeNameEdit = nullptr;
    QWidget* m_themePreview = nullptr;
    QLabel* m_themeLiveStatus = nullptr;
    QLabel* m_themeContrastStatus = nullptr;
    QPushButton* m_themeSaveButton = nullptr;
    QComboBox* m_languageList = nullptr;
    QComboBox* m_startupTemplate = nullptr;
    QLabel* m_languageStatus = nullptr;
    QPushButton* m_removeLanguage = nullptr;
    QLabel* m_fontStatus = nullptr;
    QPushButton* m_resetFont = nullptr;
    QWidget* m_timelineFileRow = nullptr;
    QWidget* m_timelineBlurRow = nullptr;
    QComboBox* m_timelinePlacement = nullptr;
    QSlider* m_timelineVisibility = nullptr;
    QLabel* m_timelineVisibilityValue = nullptr;
    QSlider* m_timelineBlur = nullptr;
    QLabel* m_timelineBlurValue = nullptr;
    QCheckBox* m_timelineAnimate = nullptr;
    QLineEdit* m_headerBackgroundPath = nullptr;
    QPushButton* m_clearHeaderBackground = nullptr;
    QCheckBox* m_enableHeaderBackground = nullptr;
    QWidget* m_headerFileRow = nullptr;
    QWidget* m_headerBlurRow = nullptr;
    QComboBox* m_headerPlacement = nullptr;
    QSlider* m_headerVisibility = nullptr;
    QLabel* m_headerVisibilityValue = nullptr;
    QSlider* m_headerBlur = nullptr;
    QLabel* m_headerBlurValue = nullptr;
    QCheckBox* m_headerAnimate = nullptr;
    QSlider* m_gridWidth = nullptr;
    QLabel* m_gridWidthValue = nullptr;
    QSlider* m_gridOpacity = nullptr;
    QLabel* m_gridOpacityValue = nullptr;
    QSlider* m_playheadWidth = nullptr;
    QLabel* m_playheadWidthValue = nullptr;
    QCheckBox* m_playheadTrail = nullptr;
    QRadioButton* m_trackColourTint = nullptr;
    QRadioButton* m_neutralTint = nullptr;
    bool m_applyingInstalledTheme = false;
    QHash<QString, QPushButton*> m_swatches;       // field key → labelled colour button
};
