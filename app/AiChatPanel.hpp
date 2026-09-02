#pragma once

#include "AiPrefs.hpp"
#include "GlassPanel.hpp"

#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

namespace daw {
class EngineController;
namespace ai { class AiSession; class ContentCatalog; class CompositionCandidateStore; struct ModelReply; }
} // namespace daw

class QLabel;
class QHideEvent;
class QListWidget;
class QPlainTextEdit;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QVariantAnimation;
class ShortcutManager;
namespace ui {
class IconButton;
class LlmClient;
class ModeSwitch;
class MusicClient;
class SelectionModel;
} // namespace ui

/// The assistant: a chat that operates the program rather than describing it.
///
/// A request goes out with the project's current state and the tool set from
/// `daw::ai`; whatever the model calls runs against the controller as it
/// arrives, so the tracks and notes appear while the run is still going. The
/// whole run folds into one undo entry, so a result that is not wanted costs
/// one Ctrl+Z.
///
/// It has a second mode. The same box, switched over, sends the request to a
/// music model instead: no tools, no agent loop — one brief out, one audio file
/// back, and a new audio track at the playhead. The brief is read off the open
/// project (tempo, key, meter, what tracks exist), so the two modes share the
/// same idea of context even though only one of them talks to a language model.
///
/// Off by default. A managed request gets a fresh server-side quota check and
/// ephemeral provider configuration, then calls the provider from this
/// computer. Custom models call the locally configured endpoint directly.
class AiChatPanel : public ui::GlassPanel {
    Q_OBJECT
public:
    explicit AiChatPanel(daw::EngineController* controller,
                         QWidget* parent = nullptr);
    ~AiChatPanel() override;

    /// Re-read the selected connection and swap the client if its protocol
    /// changed.
    void reloadSettings();

    /// Files the assistant may use, added by dropping them here.
    void addAttachment(const QString& path);

    /// Where the assistant reads "this" and "that track" from. Not owned.
    void setSelectionModel(ui::SelectionModel* selection) {
        m_selection = selection;
    }
    void setCommandManager(ShortcutManager* commands) {
        m_commands = commands;
    }

    /// Where the open project lives on disk, so the assistant can save it when
    /// asked. Empty for a project that has never been saved. MainWindow keeps
    /// this in step; the panel has no business tracking it itself.
    void setProjectPath(const QString& path) { m_projectPath = path; }

    /// Headless check only: run a whole turn against a scripted stand-in for a
    /// provider — no key, no network — and report whether the tool calls really
    /// reached the document and folded into one undo entry.
    bool checkAgentForTest();
    /// Headless check only: put a transcript on screen for a screenshot.
    void showDemoTranscriptForTest();
    /// Headless check only: run one music request against a stand-in that
    /// answers with a local file, and report whether the audio really reached a
    /// new track and folded into one undo entry.
    bool checkMusicForTest();
    /// Headless check only: a finished music transcript, for a screenshot.
    void showDemoMusicTranscriptForTest();

signals:
    /// Something worth a line in the status bar.
    void statusMessage(const QString& text);
    /// The gear button: the shell opens the AI page of Settings.
    void settingsRequested();
    /// Tools changed the document; the shell rebuilds its views.
    void projectChanged(bool localFileDirty = true);

protected:
    void onReduceTransparencyChanged() override;
    QRect plateRect() const override;
    QPainterPath plateShape() const override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    /// Enter sends, Shift+Enter starts a new line — the convention every chat
    /// box has, and not something QPlainTextEdit does on its own.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    using Mode = ui::aiprefs::Mode;

    /// One music request and what became of it. The music mode keeps its own
    /// short history rather than joining `AiSession`'s: these are not turns of
    /// a language model, and mixing them into the conversation would send
    /// nonsense to the chat provider on the next request.
    struct MusicTurn {
        QString request;        ///< what the user typed
        QString prompt;         ///< what was actually sent, project and all
        QString lyrics;
        bool instrumental = false;
        QString filePath;       ///< where the result was saved
        QString trackName;
        double seconds = 0.0;
        QString error;
        bool pending = false;
    };

    QWidget* buildHeader();
    QWidget* buildComposer();
    QWidget* buildEmptyState();
    QWidget* buildMusicPage();

    void applyTheme();
    /// Keep the browser-backed catalog warm without decoding libraries on the
    /// UI thread. `force` re-reads unchanged roots; false only starts an idle
    /// or cancelled catalog.
    void startContentIndex(bool force);
    void updateContentIndexStatus();
    void send();
    void sendMusic();
    void stop();
    /// Switch modes: it swaps the transcript, the placeholder and what the send
    /// button does.
    ///
    /// `persist` off is for the headless checks, which switch modes to exercise
    /// them and must not leave the user's own setting changed — the same trap
    /// a screenshot run once fell into with the panel's visibility.
    void setMode(Mode mode, bool persist = true);
    void renderMusicTranscript();
    /// Update the only changing field of a pending music turn without
    /// rebuilding every card in the history.
    void updateMusicElapsedLabel();
    /// The spectral rim is useful only while the panel can actually paint.
    void syncEdgeAnimation();
    /// True while a music request is in flight.
    bool musicPending() const;
    /// Placeholder, hints and the instrumental switch follow the mode.
    void applyModeToComposer();
    /// Put a generated file on a new audio track at the playhead, as one undo
    /// entry. False when the file could not be decoded, and the empty track it
    /// would have left is taken back.
    bool insertGenerated(const QString& path, const QString& trackName);
    void step();
    void onReply(daw::ai::ModelReply reply);
    void endRun();
    void renderTranscript();
    void refreshAttachments();
    /// The per-project standing instructions, in a small editor.
    void editInstructions();
    /// The saved-prompt menu: insert one, keep this one, or forget one.
    void showPromptMenu();
    /// Show what the conversation has cost so far.
    void updateUsageLabel();
    void updateBusyState();
    void revertToMessage(std::size_t index);
    /// Show either the chat or the "connect a model" state.
    void updateReadiness();
    /// Fill the model picker with managed models followed by local custom
    /// models, tick the active connection and link to model settings.
    void populateModelMenu(class QMenu* menu);
    bool hasKey() const;

    daw::EngineController* m_controller = nullptr;
    ui::SelectionModel* m_selection = nullptr;
    ShortcutManager* m_commands = nullptr;
    QString m_projectPath;
    std::unique_ptr<daw::ai::AiSession> m_session;
    std::shared_ptr<daw::ai::ContentCatalog> m_contentCatalog;
    std::shared_ptr<daw::ai::CompositionCandidateStore> m_compositionCandidates;
    std::unique_ptr<ui::LlmClient> m_client;
    std::unique_ptr<ui::MusicClient> m_musicClient;

    Mode m_mode = Mode::Assistant;
    std::vector<MusicTurn> m_musicTurns;
    ui::ModeSwitch* m_modeSwitch = nullptr;
    ui::IconButton* m_instrumentalButton = nullptr;
    QWidget* m_composer = nullptr;
    QScrollArea* m_musicTranscript = nullptr;
    QWidget* m_musicBody = nullptr;
    QVBoxLayout* m_musicLayout = nullptr;
    /// Ticks the "generating… 12 s" line, because a music request can take a
    /// minute and a still panel looks stuck.
    QTimer* m_musicTicker = nullptr;
    int m_musicElapsed = 0;
    QLabel* m_musicElapsedLabel = nullptr;

    QStackedWidget* m_stack = nullptr;
    QScrollArea* m_transcript = nullptr;
    QWidget* m_transcriptBody = nullptr;
    QVBoxLayout* m_transcriptLayout = nullptr;
    QPlainTextEdit* m_input = nullptr;
    QListWidget* m_attachments = nullptr;
    QLabel* m_titleLabel = nullptr;
    class QToolButton* m_modelLabel = nullptr;
    QLabel* m_contentIndexLabel = nullptr;
    QLabel* m_usageLabel = nullptr;
    QTimer* m_contentIndexTicker = nullptr;
    /// Prose streamed in but not yet part of the transcript.
    QString m_streaming;
    QLabel* m_streamingLabel = nullptr;
    QLabel* m_attachHint = nullptr;
    ui::IconButton* m_promptsButton = nullptr;
    ui::IconButton* m_sendButton = nullptr;
    ui::IconButton* m_stopButton = nullptr;
    QVariantAnimation* m_edgeAnimation = nullptr;
    double m_edgePhase = 0.0;
    int m_edgeFrame = 0;
};
