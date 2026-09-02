#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace daw { class EngineController; }

class FileBrowserTree;
class FileSearchWorker;
class PreviewLoader;
class WaveformStrip;
class QLabel;
class QLineEdit;
class QTimer;
namespace ui { class IconButton; }

/// The file browser: the folders the user chose, a search, and an audition.
///
/// Not a file explorer. It shows the folders that were added to it and nothing
/// else, which is the point — a sample library is a handful of places, and the
/// rest of the disk is noise. Files are dragged straight into the sampler, onto
/// the arrangement, or onto a track's instrument slot; selecting one plays it
/// through the engine's preview node without touching the project.
class FileBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit FileBrowserPanel(daw::EngineController* controller,
                              QWidget* parent = nullptr);
    ~FileBrowserPanel() override;

    /// Re-read the folder list and the preview options from the settings.
    void reloadSettings();

    /// Semantic counterparts of the browser header and audition controls.
    /// They deliberately carry no path: the current tree selection and the
    /// user's folder picker remain the only sources of filesystem authority.
    void requestAddFolder();
    void refreshFolders();
    void togglePreview();
    void setPreviewLoopEnabled(bool enabled);
    void setAutoPreviewEnabled(bool enabled);
    bool hasPreviewableSelection() const;

    /// Scale the browser's own interface — rows, icons, labels and the search
    /// field — without touching the rest of the window. `step` is a multiplier
    /// (1.15 in, 1/1.15 out); `setZoom` takes the factor directly. Persisted.
    void zoomBy(double step);
    void setZoom(double factor);
    double zoom() const { return m_zoom; }

    /// Re-read the scanned plugins and re-file them under the browser's plugin
    /// folder. Called when a scan finishes.
    void reloadPlugins();

    /// Which edge of the window the panel is on. Only the border it draws
    /// changes; the layout order is the shell's business.
    void setOnLeft(bool onLeft);

    /// Headless check only: add `folder` to the browser (without persisting it
    /// when `persist` is false) and select `selectFile` inside it, so a
    /// screenshot or a selftest can show a real tree and a real waveform.
    bool showFolderForTest(const QString& folder, const QString& selectFile = {},
                           bool persist = false);
    bool reloadSelectedPreviewForTest();
    /// Headless check only: open the plugin folders and select the first
    /// plugin, so a grab shows the listing. False when none are scanned.
    bool showPluginsForTest();
    /// Headless check only: has a decode landed and drawn a waveform? The
    /// audition itself cannot be observed without an audio device, so this is
    /// what says the worker → strip → engine chain ran.
    bool hasPreviewWaveformForTest() const;
    /// Headless check only: the file URLs a drag of the current selection would
    /// carry. Empty when nothing draggable is selected.
    QStringList dragUrlsForTest() const;
    /// Headless check only: the whole payload a drag of the current selection
    /// would carry. Caller owns it.
    class QMimeData* dragPayloadForTest() const;
    /// Backward-compatible name used by the plugin browser check.
    class QMimeData* pluginDragForTest() const;
    /// Headless check only: run the real asynchronous library search and wait
    /// for its package-filtered result.
    QStringList searchForTest(const QString& query);
    bool selectedProjectTemplateForTest() const;
    bool activateSelectedProjectTemplateForTest();

signals:
    /// Something worth a line in the status bar.
    void statusMessage(const QString& text);
    /// The gear button: the shell opens the Browser page of Settings.
    void settingsRequested();
    /// Lets a semantic command mirror the Play button's real availability.
    void previewAvailabilityChanged(bool available);
    /// A browser preset was activated and should be applied to the currently
    /// selected mixer channel by the shell.
    void channelStripPresetActivated(const QString& path);
    void projectTemplateActivated(const QString& path);
    void projectTemplateTracksRequested(const QString& path);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    /// Ctrl/Cmd + wheel zooms, which is where a hand already is when a row is
    /// too small to read.
    void wheelEvent(class QWheelEvent* event) override;

private:
    QWidget* buildHeader();
    /// Set the caption under the waveform, elided to the panel's width with the
    /// full text kept as the tooltip.
    void setFileLabel(const QString& text, const QString& tip = {});
    QWidget* buildPreviewBar();
    void applyTheme();

    void addFolder();
    void searchChanged(const QString& query);
    void selectFile(const QString& path);
    void startPreview(const QString& path);
    void stopPreview();
    void refreshPreviewState();

    /// Push `m_zoom` into every part of the panel that has a size.
    void applyZoom();

    double m_zoom = 1.0;
    /// Font sizes as they were before any zoom, so scaling is always measured
    /// from the design size instead of compounding on the last one.
    QHash<QWidget*, int> m_baseFontPx;

    daw::EngineController* m_controller = nullptr;
    FileBrowserTree* m_tree = nullptr;
    FileSearchWorker* m_search = nullptr;
    PreviewLoader* m_loader = nullptr;
    QLineEdit* m_searchField = nullptr;
    WaveformStrip* m_strip = nullptr;
    QLabel* m_fileLabel = nullptr;
    ui::IconButton* m_playButton = nullptr;
    ui::IconButton* m_loopButton = nullptr;
    ui::IconButton* m_autoButton = nullptr;
    /// Debounces typing, so a search starts once the user pauses rather than
    /// per keystroke.
    QTimer* m_searchTimer = nullptr;
    /// Polls the audition position for the playhead — only while one is
    /// playing, so an idle browser costs nothing.
    QTimer* m_playheadTimer = nullptr;

    QString m_selectedPath;
    /// Whether the decode in flight was started in order to be heard. A
    /// selection with auto-preview off still decodes — for the waveform — and
    /// must stay silent.
    bool m_playOnLoad = false;
    /// The caption before eliding, so a resize can re-elide from the original.
    QString m_fileLabelText;
    bool m_onLeft = true;
};
