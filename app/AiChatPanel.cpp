#include "AiChatPanel.hpp"

#include "AiPrefs.hpp"
#include "AiChatChecks.hpp"
#include "AccountService.hpp"
#include "BrowserPrefs.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Icons.hpp"
#include "LlmClient.hpp"
#include "MusicClient.hpp"
#include "PromptService.hpp"
#include "SelectionModel.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "ai/AiSession.hpp"
#include "ai/AiTools.hpp"
#include "ai/ContentCatalog.hpp"
#include "ai/CompositionEngine.hpp"
#include "ai/MusicGen.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Internal/SamplerInstance.hpp"

#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QClipboard>
#include <QLineEdit>
#include <QSaveFile>
#include <QShortcut>
#include <QUuid>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QFontMetrics>
#include <QMenu>
#include <QToolButton>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QQuickWindow>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ai = daw::ai;
using json = nlohmann::json;

namespace {

const char* commandRiskName(ShortcutManager::Risk risk) {
    switch (risk) {
        case ShortcutManager::Risk::Safe: return "safe";
        case ShortcutManager::Risk::Reversible: return "reversible";
        case ShortcutManager::Risk::Destructive: return "destructive";
        case ShortcutManager::Risk::ExternalSideEffect: return "external";
        case ShortcutManager::Risk::Unknown: return "unknown";
    }
    return "unknown";
}

bool commandAllowsMode(const ShortcutManager::Metadata& metadata,
                       ai::InteractionMode mode) {
    const ShortcutManager::Mode required =
        mode == ai::InteractionMode::Help ? ShortcutManager::HelpMode
        : mode == ai::InteractionMode::Teach ? ShortcutManager::TeachMode
                                             : ShortcutManager::DoMode;
    return metadata.modes.testFlag(required);
}

// Compact enough to leave the transcript in charge, with one quiet metadata
// line for the model and the project-aware library state.
constexpr int kHeaderHeight = 76;
constexpr int kAttachmentsMaxHeight = 70;

// ── Transcript furniture, shared by both modes ───────────────────────────────
//
// Free functions rather than lambdas inside one renderer: the music mode draws
// the same cards, and two transcripts that drifted apart in spacing and
// silhouette would read as two different programs.

QToolButton* messageAction(QWidget* parent, icons::Glyph glyph, const QString& label) {
    auto* button = new QToolButton(parent);
    button->setObjectName("AiMessageAction");
    button->setProperty("aiGlyph", int(glyph));
    button->setIcon(icons::icon(glyph, th().textSecondary, 16));
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setFixedSize(28, 28);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setAccessibleName(label);
    button->setToolTip(label);
    return button;
}

QLabel* cardText(QWidget* parent, const QString& text, const char* objectName,
                 bool secondary = false) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setFocusPolicy(Qt::ClickFocus);
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    // Keep QLabel's height-for-width flag: clearing it clips long wrapped
    // answers instead of growing the transcript and its vertical scroll range.
    label->setSizePolicy(policy);
    if (secondary)
        label->setAccessibleDescription(
            QObject::tr("Secondary message text"));
    return label;
}

QPair<QWidget*, QVBoxLayout*> messageCard(QWidget* parent,
                                          const char* objectName,
                                          const QString& role,
                                          const char* roleObject) {
    auto* card = new QWidget(parent);
    card->setObjectName(objectName);
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(11, 9, 11, 10);
    layout->setSpacing(5);
    if (!role.isEmpty()) {
        auto* roleLabel = new QLabel(role, card);
        roleLabel->setObjectName(roleObject);
        roleLabel->setTextFormat(Qt::PlainText);
        layout->addWidget(roleLabel);
    }
    return qMakePair(card, layout);
}

// Stretch ratios give each semantic type its own silhouette: user turns sit
// right, prose answers breathe wider on the left, tool activity is a tighter
// technical block. No fixed pixel width, so resizing stays fluid.
void wrapRow(QVBoxLayout* into, QWidget* parent, QWidget* card, int before,
             int cardStretch, int after) {
    auto* row = new QWidget(parent);
    row->setObjectName("AiMessageRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (before > 0) layout->addStretch(before);
    layout->addWidget(card, cardStretch);
    if (after > 0) layout->addStretch(after);
    into->addWidget(row);
}

/// "3:12" — a duration as a musician reads one.
QString formatDuration(double seconds) {
    if (seconds <= 0.0) return {};
    const int whole = int(seconds + 0.5);
    return QStringLiteral("%1:%2")
        .arg(whole / 60)
        .arg(whole % 60, 2, 10, QLatin1Char('0'));
}

/// A stand-in for a provider, used only by the headless check.
///
/// It reads the conversation the same way a real model would — pulling the ids
/// out of the previous tool results — so `--selftest` exercises the actual
/// multi-round loop rather than a single canned call.
class ScriptedClient final : public ui::LlmClient {
public:
    explicit ScriptedClient(QObject* parent)
        : ui::LlmClient(ui::LlmClient::Provider::Anthropic, parent) {}

    void send(const QString&, const std::vector<ai::Message>& messages,
              Reply onReply) override {
        // Answer on the event loop, like a real request, so the panel's busy
        // state and the re-entrancy are exercised too.
        ai::ModelReply reply = script(messages);
        QTimer::singleShot(0, this, [onReply = std::move(onReply),
                                     reply = std::move(reply)]() mutable {
            onReply(std::move(reply));
        });
    }
    void cancel() override {}
    bool busy() const override { return false; }

private:
    /// What the last tool result reported, by key. How the fake learns the ids
    /// the controller minted.
    static std::string lastValue(const std::vector<ai::Message>& messages,
                                 const char* key) {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            if (it->role == ai::Role::Tool)
                for (const ai::ToolOutcome& out : it->outcomes)
                    if (out.result.contains(key))
                        return out.result.value(key, std::string());
        return {};
    }

    ai::ModelReply script(const std::vector<ai::Message>& messages) {
        ai::ModelReply reply;
        switch (m_round++) {
            case 0:
                reply.text = "Making a piano part.";
                reply.calls.push_back({"t1", "add_track",
                                       json{{"kind", "instrument"},
                                            {"name", "AI Piano"}}});
                break;
            case 1:
                reply.calls.push_back(
                    {"t2", "add_midi_clip",
                     json{{"trackId", lastValue(messages, "trackId")},
                          {"startBar", 1},
                          {"lengthBars", 1}}});
                break;
            case 2: {
                json notes = json::array();
                for (int i = 0; i < 3; ++i)
                    notes.push_back(json{{"pitch", 60 + i * 4},
                                         {"start", 0.0},
                                         {"length", 3.5},
                                         {"velocity", 90}});
                reply.calls.push_back(
                    {"t3", "set_clip_notes",
                     json{{"trackId", lastValue(messages, "trackId")},
                          {"clipId", lastValue(messages, "clipId")},
                          {"notes", notes}}});
                break;
            }
            default:
                reply.text = "Done — a C major triad on a new piano track.";
                break;
        }
        return reply;
    }

    // `trackId` has to survive past the round that made the clip, since the
    // reply after it needs both ids at once.
    int m_round = 0;
};

/// A stand-in for a music server, used only by the headless check.
///
/// It writes a real, decodable WAV through the client's own `saveAudio`, so the
/// check exercises the whole path — brief, file on disk, import, undo — and
/// only the socket is missing.
class ScriptedMusicClient final : public ui::MusicClient {
public:
    explicit ScriptedMusicClient(QObject* parent) : ui::MusicClient(parent) {}

    void generate(const daw::ai::MusicBrief&, Done onDone) override {
        Outcome outcome;
        const QString temp = QDir::temp().filePath("daw_scripted_music.wav");
        audio::AudioBuffer tone(2, 48000);   // one second at 48k
        for (audio::BufferSize f = 0; f < 48000; ++f) {
            const float t = float(f) / 48000.0f;
            const float s = 0.4f * std::sin(2.0f * 3.14159265f * 330.0f * t);
            tone.getChannel(0)[f] = s;
            tone.getChannel(1)[f] = s;
        }
        audio::AudioRecorder recorder;
        recorder.initialize(48000, 2);
        recorder.writeWAVFile(temp.toStdString(), tone, 48000);

        QFile file(temp);
        if (file.open(QIODevice::ReadOnly)) {
            QString error;
            outcome.filePath =
                saveAudio(file.readAll(), QStringLiteral("scripted"),
                          QStringLiteral("wav"), error);
            outcome.error = error;
            outcome.seconds = 1.0;
        } else {
            outcome.error = QStringLiteral("could not write the scripted tone");
        }
        // Through the event loop, like a real request, so the panel's busy
        // state is exercised too.
        QTimer::singleShot(0, this, [onDone = std::move(onDone),
                                     outcome = std::move(outcome)]() mutable {
            onDone(std::move(outcome));
        });
    }
    void cancel() override {}
    bool busy() const override { return false; }
};

} // namespace

AiChatPanel::AiChatPanel(daw::EngineController* controller, QWidget* parent)
    : ui::GlassPanel(parent), m_controller(controller) {
    setObjectName("AiPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    // The pane paints every pixel itself. This prevents the host palette from
    // flashing through as a white rectangle while it opens or resizes.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setMinimumWidth(240);
    setAcceptDrops(true);
    setCornerRadius(Theme::cornerRadius);
    setShadowMargin(0);
    setAccentColor(th().accentHighlight);
    // This panel participates in layout, so unlike the floating context plate
    // there is no scene behind it to refract. Freezing the empty backdrop keeps
    // the glass material and rim without repeatedly screen-grabbing itself.
    setBackdropFrozen(true);

    m_session = std::make_unique<ai::AiSession>(*controller);
    m_contentCatalog = std::make_shared<ai::ContentCatalog>();
    m_compositionCandidates =
        std::make_shared<ai::CompositionCandidateStore>();

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(8, 0, 8, 8);
    column->setSpacing(0);
    column->addWidget(buildHeader());

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName("AiStack");
    m_stack->setAttribute(Qt::WA_StyledBackground, true);
    column->addWidget(m_stack, 1);

    auto* chat = new QWidget(m_stack);
    chat->setObjectName("AiChatPage");
    chat->setAttribute(Qt::WA_StyledBackground, true);
    auto* chatColumn = new QVBoxLayout(chat);
    chatColumn->setContentsMargins(0, 0, 0, 0);
    chatColumn->setSpacing(0);

    m_transcript = new QScrollArea(chat);
    m_transcript->setObjectName("AiTranscript");
    m_transcript->setFrameShape(QFrame::NoFrame);
    m_transcript->setWidgetResizable(true);
    m_transcript->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_transcript->viewport()->setAutoFillBackground(false);

    m_transcriptBody = new QWidget(m_transcript);
    m_transcriptBody->setObjectName("AiTranscriptBody");
    m_transcriptBody->setAttribute(Qt::WA_StyledBackground, true);
    m_transcriptLayout = new QVBoxLayout(m_transcriptBody);
    m_transcriptLayout->setContentsMargins(0, 12, 0, 12);
    m_transcriptLayout->setSpacing(12);
    m_transcript->setWidget(m_transcriptBody);
    chatColumn->addWidget(m_transcript, 1);

    m_latestButton = new QToolButton(chat);
    m_latestButton->setObjectName("AiContextButton");
    m_latestButton->setText(tr("Latest message ↓"));
    m_latestButton->hide();
    chatColumn->addWidget(m_latestButton, 0, Qt::AlignHCenter);
    connect(m_latestButton, &QToolButton::clicked, this, [this] {
        m_followOutput = true;
        m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
        m_latestButton->hide();
    });
    connect(m_transcript->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        const auto* scroll = m_transcript->verticalScrollBar();
        m_followOutput = scroll->maximum() - value <= 24;
        m_latestButton->setVisible(!m_followOutput);
    });
    connect(m_transcript->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int maximum) {
        if (!m_followOutput) return;
        // Wrapped text is laid out after setText returns. Follow its final
        // height rather than the previous chunk's scrollbar range.
        const QSignalBlocker block(m_transcript->verticalScrollBar());
        m_transcript->verticalScrollBar()->setValue(maximum);
    });
    // Claim Find before the main window's plugin-search shortcut. Key events
    // from focused children propagate here when their editor does not use them.
    installEventFilter(this);

    m_stack->addWidget(chat);
    m_stack->addWidget(buildEmptyState());
    m_stack->addWidget(buildMusicPage());

    // One composer for both modes, below the stack rather than inside the chat
    // page: the box you type in is the same box, and only what it does with the
    // text changes.
    m_composer = buildComposer();
    column->addWidget(m_composer);

    m_musicClient = std::make_unique<ui::MusicClient>(this);
    m_musicTicker = new QTimer(this);
    m_musicTicker->setInterval(1000);
    connect(m_musicTicker, &QTimer::timeout, this, [this] {
        ++m_musicElapsed;
        updateMusicElapsedLabel();
    });
    m_contentIndexTicker = new QTimer(this);
    m_contentIndexTicker->setInterval(200);
    connect(m_contentIndexTicker, &QTimer::timeout, this,
            &AiChatPanel::updateContentIndexStatus);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &AiChatPanel::applyTheme);
    if (auto* account = account::Service::instance()) {
        connect(account, &account::Service::aiModelsChanged, this,
                &AiChatPanel::reloadSettings);
        connect(account, &account::Service::authenticatedChanged, this,
                &AiChatPanel::reloadSettings);
        connect(account, &account::Service::snapshotChanged, this,
                &AiChatPanel::updateUsageLabel);
    }

    // Chat is a high-frequency editing surface. Nothing in its chrome moves
    // unless a response itself is changing.
    applyTheme();
    // The remembered mode is restored without the switch travelling: nothing
    // the user did not just do should appear to move.
    m_mode = ui::aiprefs::mode();
    if (m_modeSwitch) {
        const QSignalBlocker block(m_modeSwitch);
        m_modeSwitch->setRight(m_mode == Mode::Music, /*animate=*/false);
    }
    reloadSettings();
    updateUsageLabel();
    renderTranscript();
    renderMusicTranscript();
    applyModeToComposer();
    QTimer::singleShot(0, this,
                       [this] { startContentIndex(/*force=*/true); });
}

AiChatPanel::~AiChatPanel() {
    // The clients' callbacks hold `this`; drop them before the session and the
    // turn list they would reach into go away.
    if (m_client) m_client->cancel();
    if (m_musicClient) m_musicClient->cancel();
    if (m_contentCatalog) m_contentCatalog->cancelRefresh();
}

QWidget* AiChatPanel::buildHeader() {
    auto* header = new QWidget(this);
    header->setObjectName("AiHeader");
    header->setFixedHeight(kHeaderHeight);

    auto* column = new QVBoxLayout(header);
    column->setContentsMargins(4, 8, 0, 8);
    column->setSpacing(2);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(5);

    auto* mark = new QLabel(QStringLiteral("AI"), header);
    mark->setObjectName("AiMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(27, 27);
    row->addWidget(mark);

    m_titleLabel = new QLabel(tr("AI chat"), header);
    m_titleLabel->setObjectName("AiTitle");
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(m_titleLabel, 1);

    auto* clear = new ui::IconButton(icons::Glyph::Trash,
                                     tr("Start a new conversation"), header);
    clear->setButtonSize(26, 26);
    clear->setAccessibleName(clear->toolTip());
    clear->setFocusPolicy(Qt::StrongFocus);
    connect(clear, &QAbstractButton::clicked, this, [this] {
        if (m_session->running()) return;
        if (!m_session->messages().empty() && QMessageBox::question(
                this, tr("New conversation"), tr("Clear the conversation? Project edits will be kept.")) != QMessageBox::Yes) return;
        m_session->clear();
        renderTranscript();
        updateUsageLabel();
    });
    row->addWidget(clear);

    auto* history = messageAction(header, icons::Glyph::Search,
                                  tr("Search, copy or export this conversation (Ctrl+F)"));
    history->setFixedSize(26, 26);
    connect(history, &QToolButton::clicked, this, &AiChatPanel::showConversation);
    row->addWidget(history);

    auto* rules = new ui::IconButton(
        icons::Glyph::NoteStyle,
        tr("Standing instructions for this project"), header);
    rules->setButtonSize(26, 26);
    rules->setAccessibleName(rules->toolTip());
    rules->setFocusPolicy(Qt::StrongFocus);
    connect(rules, &QAbstractButton::clicked, this,
            &AiChatPanel::editInstructions);
    row->addWidget(rules);

    auto* gear = new ui::IconButton(icons::Glyph::Gear,
                                    tr("Assistant settings"), header);
    gear->setButtonSize(26, 26);
    gear->setAccessibleName(gear->toolTip());
    gear->setFocusPolicy(Qt::StrongFocus);
    connect(gear, &QAbstractButton::clicked, this,
            &AiChatPanel::settingsRequested);
    row->addWidget(gear);
    column->addLayout(row);

    auto* meta = new QHBoxLayout;
    meta->setContentsMargins(0, 2, 0, 0);
    meta->setSpacing(6);
    // The model is a *choice*, not a caption: one key and one URL cover every
    // model a provider offers, and which one suits the request is decided here
    // rather than in a settings window.
    m_modelLabel = new QToolButton(header);
    m_modelLabel->setObjectName("AiModel");
    m_modelLabel->setAccessibleName(tr("AI model"));
    m_modelLabel->setPopupMode(QToolButton::InstantPopup);
    m_modelLabel->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_modelLabel->setCursor(Qt::PointingHandCursor);
    m_modelLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* modelMenu = new QMenu(m_modelLabel);
    connect(modelMenu, &QMenu::aboutToShow, this,
            [this, modelMenu] { populateModelMenu(modelMenu); });
    m_modelLabel->setMenu(modelMenu);
    meta->addWidget(m_modelLabel, 1);

    m_contentIndexLabel = new QLabel(header);
    m_contentIndexLabel->setObjectName("AiIndexStatus");
    m_contentIndexLabel->setText(tr("LIB —"));
    meta->addWidget(m_contentIndexLabel);

    m_usageLabel = new QLabel(header);
    m_usageLabel->setObjectName("AiUsage");
    meta->addWidget(m_usageLabel);
    column->addLayout(meta);

    // The Music mode is built and works, but the user asked for it to be out of
    // sight until it is ready to show — so the switch is not created and the
    // panel stays in Assistant. `setMode` and everything behind it are
    // untouched; putting the control back is what turns it on again.
    m_modeSwitch = nullptr;
    return header;
}

QWidget* AiChatPanel::buildComposer() {
    auto* composer = new QWidget(this);
    composer->setObjectName("AiComposer");
    composer->setProperty("inputFocused", false);
    composer->setAttribute(Qt::WA_StyledBackground, true);
    auto* column = new QVBoxLayout(composer);
    column->setContentsMargins(10, 8, 10, 8);
    column->setSpacing(6);

    m_contextButton = new QToolButton(composer);
    m_contextButton->setObjectName("AiContextButton");
    m_contextButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_contextButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_contextButton, &QToolButton::clicked, this, &AiChatPanel::showContext);
    column->addWidget(m_contextButton);
    updateSelectionContext();
    m_attachmentButton = new QToolButton(composer);
    m_attachmentButton->setObjectName("AiContextButton");
    m_attachmentButton->setCheckable(true);
    m_attachmentButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_attachmentButton->hide();
    column->addWidget(m_attachmentButton);

    m_attachHint = new QLabel(tr("Drop samples here to let the assistant use them"),
                              composer);
    m_attachHint->setObjectName("AiHint");
    m_attachHint->setWordWrap(true);
    m_attachHint->hide();
    column->addWidget(m_attachHint);

    m_attachments = new QListWidget(composer);
    m_attachments->setObjectName("AiAttachments");
    m_attachments->setFrameShape(QFrame::NoFrame);
    m_attachments->setMaximumHeight(kAttachmentsMaxHeight);
    m_attachments->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_attachments->installEventFilter(this);
    connect(m_attachments, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const QString path = item->data(Qt::UserRole).toString();
        if (!QFileInfo(path).isDir() || !m_contentCatalog) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Attached folder · %1").arg(QFileInfo(path).fileName()));
        dialog.resize(540, 380);
        auto* column = new QVBoxLayout(&dialog);
        const auto files = m_contentCatalog->search({}, std::nullopt, 200, path.toStdString());
        QStringList lines;
        for (const auto& file : files) {
            QString line = QString::fromStdString(file.location + "/" + file.name);
            if (file.audio) line += QStringLiteral(" · %1 s").arg(file.audio->durationSeconds, 0, 'f', 1);
            lines << line;
        }
        auto* note = new QLabel(tr("Indexed files: %1 (up to 200 shown). Refresh the added library to include new files.").arg(files.size()), &dialog);
        note->setWordWrap(true);
        column->addWidget(note);
        auto* list = new QPlainTextEdit(lines.join('\n'), &dialog);
        list->setReadOnly(true);
        column->addWidget(list);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        column->addWidget(close);
        dialog.exec();
    });
    m_attachments->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_attachmentButton, &QToolButton::toggled, this, [this](bool expanded) {
        m_attachments->setVisible(m_mode == Mode::Assistant && expanded &&
                                  m_attachments->count() > 0);
    });
    connect(m_attachments, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        QMenu menu(this);
        menu.addAction(tr("Remove selected attachments"), this, [this] {
            qDeleteAll(m_attachments->selectedItems()); refreshAttachments();
        });
        menu.addAction(tr("Remove all attachments"), this, [this] {
            m_attachments->clear(); refreshAttachments();
        });
        menu.exec(m_attachments->viewport()->mapToGlobal(point));
    });
    m_attachments->setToolTip(
        tr("Files the assistant may load. Select and press Backspace to remove."));
    m_attachments->hide();
    column->addWidget(m_attachments);

    m_input = new QPlainTextEdit(composer);
    m_input->setObjectName("AiInput");
    // Qt's native text caret uses composition modes outside the scene
    // recorder. Cache this small input surface like the other native controls.
    m_input->viewport()->setProperty("vlt.nativeControlAsset", true);
    m_input->setPlaceholderText(
        tr("Write a message…"));
    m_input->setFixedHeight(68);
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->setFocusPolicy(Qt::StrongFocus);
    m_input->setTabChangesFocus(true);
    m_input->setInputMethodHints(Qt::ImhMultiLine);
    m_input->setAccessibleName(tr("Message to the AI assistant"));
    m_input->setAccessibleDescription(
        tr("Type a request. Enter sends; Shift+Enter inserts a new line."));
    m_input->installEventFilter(this);
    // The whole composer reads as one input surface. Forward a click on its
    // padding to the editor instead of leaving a focused-looking dead area.
    composer->setFocusPolicy(Qt::ClickFocus);
    composer->setFocusProxy(m_input);
    setFocusPolicy(Qt::ClickFocus);
    setFocusProxy(m_input);
    column->addWidget(m_input);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(4);

    auto* attach = new ui::IconButton(icons::Glyph::Plus,
                                      tr("Attach a sample"), composer);
    attach->setButtonSize(28, 28);
    connect(attach, &QAbstractButton::clicked, this, [this] {
        QMenu menu(this);
        menu.addAction(tr("Attach samples or MIDI…"), this, [this] {
            for (const auto& file : QFileDialog::getOpenFileNames(this, tr("Attach samples or MIDI"), {},
                    tr("Audio and MIDI (*.wav *.flac *.aif *.aiff *.mp3 *.ogg *.m4a *.mid *.midi)"))) addAttachment(file);
        });
        menu.addAction(tr("Attach a sample folder…"), this, [this] {
            const auto folder = QFileDialog::getExistingDirectory(this, tr("Attach a sample folder"));
            if (!folder.isEmpty()) addAttachment(folder);
        });
        menu.addAction(tr("Attach selected samples"), this, [this] {
            if (!m_selection) return;
            QSet<QString> tracks, clips;
            for (const auto& id : m_selection->tracks()) tracks.insert(id);
            for (const auto& clip : m_selection->clips()) { tracks.insert(clip.trackId); clips.insert(clip.clipId); }
            for (const auto& track : m_controller->project().tracks) {
                if (!tracks.contains(QString::fromStdString(track.id))) continue;
                if (auto* sampler = m_controller->samplerInstance(track.id, track.instrument.id))
                    addAttachment(QString::fromStdString(sampler->samplePath()));
                for (const auto& clip : track.clips)
                    if (clips.isEmpty() || clips.contains(QString::fromStdString(clip.id)))
                        addAttachment(QString::fromStdString(clip.filePath));
            }
        });
        menu.addAction(tr("Refresh added library"), this, [this] { startContentIndex(true); });
        menu.exec(QCursor::pos());
    });
    buttons->addWidget(attach);

    m_promptsButton = new ui::IconButton(icons::Glyph::Star,
                                         tr("Saved prompts"), composer);
    m_promptsButton->setButtonSize(28, 28);
    connect(m_promptsButton, &QAbstractButton::clicked, this,
            &AiChatPanel::showPromptMenu);
    buttons->addWidget(m_promptsButton);

    // Music only: the one choice that changes what comes back enough to be
    // worth a control instead of a word in the request.
    m_instrumentalButton = new ui::IconButton(
        icons::Glyph::Synth, tr("Instrumental — no vocals"), composer);
    m_instrumentalButton->setButtonSize(28, 28);
    m_instrumentalButton->setCheckable(true);
    m_instrumentalButton->setChecked(ui::aiprefs::musicInstrumental());
    connect(m_instrumentalButton, &QAbstractButton::toggled, this, [](bool on) {
        ui::aiprefs::setMusicInstrumental(on);
    });
    buttons->addWidget(m_instrumentalButton);
    buttons->addStretch(1);

    m_stopButton = new ui::IconButton(icons::Glyph::Stop, tr("Stop"), composer);
    m_stopButton->setButtonSize(30, 28);
    m_stopButton->hide();
    connect(m_stopButton, &QAbstractButton::clicked, this, &AiChatPanel::stop);
    buttons->addWidget(m_stopButton);

    m_sendButton = new ui::IconButton(icons::Glyph::ArrowUp, tr("Send"), composer);
    m_sendButton->setButtonSize(30, 28);
    m_sendButton->setProminent(true);
    m_sendButton->setAccentTint(true);
    connect(m_sendButton, &QAbstractButton::clicked, this, &AiChatPanel::send);
    buttons->addWidget(m_sendButton);

    column->addLayout(buttons);
    for (auto* button : composer->findChildren<ui::IconButton*>()) {
        button->setAccessibleName(button->toolTip());
        button->setFocusPolicy(Qt::StrongFocus);
    }
    return composer;
}

QWidget* AiChatPanel::buildEmptyState() {
    auto* page = new QWidget(m_stack);
    page->setObjectName("AiEmptyPage");
    page->setAttribute(Qt::WA_StyledBackground, true);
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(22, 22, 22, 18);
    column->setSpacing(9);
    column->addStretch(3);

    auto* mark = new QLabel(QStringLiteral("AI"), page);
    mark->setObjectName("AiEmptyMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(42, 42);
    column->addWidget(mark, 0, Qt::AlignCenter);

    auto* kicker = new QLabel(tr("VLT AI"), page);
    kicker->setObjectName("AiEmptyKicker");
    kicker->setAlignment(Qt::AlignCenter);
    column->addWidget(kicker);

    auto* headline = new QLabel(tr("Connect an AI model"), page);
    headline->setObjectName("AiEmptyTitle");
    headline->setAlignment(Qt::AlignCenter);
    column->addWidget(headline);

    auto* blurb = new QLabel(
        tr("Choose a VLT model or add a compatible endpoint. You can keep "
           "drafting your request below while settings are open."),
        page);
    blurb->setObjectName("AiHint");
    blurb->setWordWrap(true);
    blurb->setAlignment(Qt::AlignCenter);
    column->addWidget(blurb);

    auto* open = new QPushButton(tr("Open AI Settings…"), page);
    open->setObjectName("AiOpenSettings");
    connect(open, &QAbstractButton::clicked, this,
            &AiChatPanel::settingsRequested);
    column->addWidget(open, 0, Qt::AlignCenter);
    column->addStretch(4);
    return page;
}

QWidget* AiChatPanel::buildMusicPage() {
    auto* page = new QWidget(m_stack);
    page->setObjectName("AiMusicPage");
    page->setAttribute(Qt::WA_StyledBackground, true);
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    m_musicTranscript = new QScrollArea(page);
    m_musicTranscript->setObjectName("AiTranscript");
    m_musicTranscript->setFrameShape(QFrame::NoFrame);
    m_musicTranscript->setWidgetResizable(true);
    m_musicTranscript->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_musicTranscript->viewport()->setAutoFillBackground(false);

    m_musicBody = new QWidget(m_musicTranscript);
    m_musicBody->setObjectName("AiTranscriptBody");
    m_musicBody->setAttribute(Qt::WA_StyledBackground, true);
    m_musicLayout = new QVBoxLayout(m_musicBody);
    m_musicLayout->setContentsMargins(8, 10, 8, 12);
    m_musicLayout->setSpacing(8);
    m_musicTranscript->setWidget(m_musicBody);
    column->addWidget(m_musicTranscript, 1);
    return page;
}

void AiChatPanel::applyTheme() {
    const Theme& t = th();
    setAccentColor(t.accentHighlight);

    auto css = [](QColor color) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red()).arg(color.green()).arg(color.blue())
            .arg(QString::number(color.alphaF(), 'f', 3));
    };
    QColor markFill = t.accent;
    markFill.setAlphaF(t.dark ? 0.16 : 0.10);
    QColor modelFill = t.surfaceElevated;
    modelFill.setAlphaF(t.dark ? 0.78 : 0.88);
    QColor inputFill = t.well();
    inputFill.setAlphaF(t.dark ? 0.78 : 0.86);
    QColor inputBorder = mixColors(t.separator(), t.accent, 0.20);
    inputBorder.setAlphaF(t.dark ? 0.68 : 0.54);
    QColor userBottom = mixColors(t.surface, t.accent, 0.14);
    userBottom.setAlphaF(t.dark ? 0.82 : 0.76);
    QColor userBorder = mixColors(t.separator(), t.accentHighlight, 0.58);
    userBorder.setAlphaF(t.dark ? 0.52 : 0.42);
    QColor assistantTop = t.surfaceElevated;
    assistantTop.setAlphaF(t.dark ? 0.54 : 0.68);
    QColor assistantBorder = mixColors(t.separator(), t.accent, 0.26);
    assistantBorder.setAlphaF(t.dark ? 0.42 : 0.34);
    QColor actionBottom = t.well();
    actionBottom.setAlphaF(t.dark ? 0.82 : 0.76);
    QColor actionBorder = mixColors(t.separator(), t.accentHighlight, 0.18);
    actionBorder.setAlphaF(0.68);
    QColor statusFill = t.accent;
    statusFill.setAlphaF(t.dark ? 0.18 : 0.12);
    QColor errorFill = Theme::mute();
    errorFill.setAlphaF(t.dark ? 0.16 : 0.10);

#ifdef Q_OS_MACOS
    QString fixedFamily = QStringLiteral("Menlo");
#else
    QString fixedFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
#endif
    fixedFamily.replace('"', QStringLiteral("\\\""));

    setStyleSheet(QString(R"(
#AiPanel { background: transparent; }
#AiStack, #AiChatPage, #AiEmptyPage, #AiMusicPage,
#AiTranscript, #AiTranscriptBody, #AiMessageRow {
    background: transparent; border: none;
}
#AiHeader { background: transparent; border: none; border-bottom: 1px solid %SEP%; }
#AiContextButton { background: transparent; border: none; border-radius: 8px; color: %TEXT2%; font-size: 10px; padding: 3px 4px; text-align: left; }
#AiContextButton:hover, #AiContextButton:focus { background: %MODEL_FILL%; color: %TEXT1%; }
#AiMessageAction { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 0; }
#AiMessageAction:hover { background: %MODEL_FILL%; }
#AiMessageAction:pressed { background: %MARK_FILL%; }
#AiMessageAction:focus { border: 1px solid %ACCENT_SOFT%; }
#AiMark { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
          border-radius: 8px; color: %TEXT1%; font-size: 10px;
          font-weight: 750; }
#AiTitle { color: %TEXT1%; font-size: 12px; font-weight: 650; }
/* Room on the right for the menu caret QToolButton draws itself, or it lands
   on the last letter of the model's name. */
#AiModel { background: %MODEL_FILL%; border: 1px solid %SEP%;
           border-radius: 8px; color: %TEXT2%; font-size: 10px;
           padding: 2px 16px 2px 8px; }
#AiModel:hover { border-color: %ACCENT_SOFT%; color: %TEXT1%; }
#AiModel::menu-indicator { subcontrol-position: right center;
                           subcontrol-origin: padding; right: 4px; }
#AiUsage { color: %TEXT2%; font-size: 10px; }
#AiIndexStatus { color: %TEXT2%; font-size: 10px; font-weight: 600; }
#AiHint { color: %TEXT2%; font-size: 11px; }
#AiEmptyMark { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
               border-radius: 8px; color: %TEXT1%; font-size: 12px;
               font-weight: 750; }
#AiEmptyKicker { color: %ACCENT_SOFT%; font-size: 10px; font-weight: 650; }
#AiEmptyTitle { color: %TEXT1%; font-size: 13px; font-weight: 650; }
#AiOpenSettings { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
                  border-radius: 8px; color: %TEXT1%; padding: 6px 14px; }
#AiOpenSettings:hover { background: %MODEL_FILL%; }
#AiComposer { background: %INPUT_FILL%; border: 1px solid %INPUT_BORDER%;
              border-radius: 8px; }
#AiComposer[inputFocused="true"] { border: 1px solid %ACCENT_SOFT%; }
#AiUserCard {
    background: %USER_BOTTOM%;
    border: 1px solid %USER_BORDER%; border-radius: 8px;
}
#AiAssistantCard, #AiLiveCard {
    background: %ASSISTANT_TOP%; border: 1px solid %ASSISTANT_BORDER%; border-radius: 8px;
}
#AiActionCard {
    background: %ACTION_BOTTOM%;
    border: 1px solid %ACTION_BORDER%; border-radius: 8px;
}
#AiErrorCard { background: %ERROR_FILL%; border: 1px solid %ERROR%;
               border-radius: 8px; }
#AiThinkingCard { background: %MARK_FILL%; border: 1px solid %ASSISTANT_BORDER%;
                  border-radius: 8px; }
#AiMessageText { color: %TEXT1%; font-size: 13px; }
#AiMessageSecondary { color: %TEXT2%; font-size: 11px; }
#AiUserRole, #AiAssistantRole, #AiActionRole, #AiLiveRole {
    font-size: 10px; font-weight: 650;
}
#AiUserRole { color: %TEXT2%; }
#AiAssistantRole, #AiLiveRole { color: %ACCENT_SOFT%; }
#AiActionRole { color: %TEXT2%; }
#AiActionStatusOk, #AiActionStatusWarn, #AiActionStatusError {
    border-radius: 8px; padding: 2px 5px; font-family: "%MONO%";
    font-size: 10px; font-weight: 600;
}
#AiActionStatusOk { background: %STATUS_FILL%; color: %ACCENT_SOFT%; }
#AiActionStatusWarn { background: %MARK_FILL%; color: %TEXT1%; }
#AiActionStatusError { background: %ERROR_FILL%; color: %ERROR%; }
#AiActionText { color: %TEXT2%; font-size: 11px; }
#AiCandidateCard { background: %ASSISTANT_TOP%; border: 1px solid %ACTION_BORDER%;
                   border-radius: 8px; }
#AiCandidateTitle { color: %TEXT1%; font-size: 9px; font-weight: 750;
                    letter-spacing: 0.7px; }
#AiCandidateScore { color: %TEXT2%; font-size: 9px; }
#AiCandidateButton { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
                     border-radius: 8px; color: %TEXT1%; font-size: 8px;
                     font-weight: 700; padding: 3px 7px; }
#AiCandidateButton:hover { background: %MODEL_FILL%; }
#AiRevertButton { background: transparent; border: none; color: %TEXT2%;
                  font-size: 9px; font-weight: 650;
                  padding: 2px 0; text-align: left; }
#AiRevertButton:hover { color: %ACCENT_SOFT%; }
#AiInput { background: transparent; border: none; color: %TEXT1%;
           font-size: 12px; padding: 2px; selection-background-color: %SELECT%; }
#AiInput:focus { border: none; }
#AiAttachments { background: %INPUT_FILL%; border: 1px solid %INPUT_BORDER%;
                 border-radius: 8px; color: %TEXT1%; font-size: 11px; padding: 7px; }
#AiAttachments::item:selected { background: %SELECT%; color: %TEXT1%; }
#AiTranscript QScrollBar:vertical { background: transparent; width: 7px; margin: 4px 0; }
#AiTranscript QScrollBar::handle:vertical { background: %SEP%; min-height: 28px;
                                            border-radius: 3px; }
#AiTranscript QScrollBar::add-line:vertical,
#AiTranscript QScrollBar::sub-line:vertical { height: 0px; }
)")
                      .replace("%MONO%", fixedFamily)
                      .replace("%MARK_FILL%", css(markFill))
                      .replace("%MODEL_FILL%", css(modelFill))
                      .replace("%INPUT_FILL%", css(inputFill))
                      .replace("%INPUT_BORDER%", css(inputBorder))
                      .replace("%USER_BOTTOM%", css(userBottom))
                      .replace("%USER_BORDER%", css(userBorder))
                      .replace("%ASSISTANT_TOP%", css(assistantTop))
                      .replace("%ASSISTANT_BORDER%", css(assistantBorder))
                      .replace("%ACTION_BOTTOM%", css(actionBottom))
                      .replace("%ACTION_BORDER%", css(actionBorder))
                      .replace("%STATUS_FILL%", css(statusFill))
                      .replace("%ERROR_FILL%", css(errorFill))
                      .replace("%ERROR%", css(Theme::mute()))
                      .replace("%ACCENT_SOFT%", css(t.accentHighlight))
                      .replace("%SEP%", css(t.separator()))
                      .replace("%SELECT%", css(t.selection))
                      .replace("%TEXT1%", css(t.textPrimary))
                      .replace("%TEXT2%", css(t.textSecondary)));
    for (auto* button : findChildren<QToolButton*>())
        if (button->property("aiGlyph").isValid())
            button->setIcon(icons::icon(icons::Glyph(button->property("aiGlyph").toInt()), t.textSecondary, 16));
    renderTranscript();
}

void AiChatPanel::onReduceTransparencyChanged() {
    ui::GlassPanel::onReduceTransparencyChanged();
    // The preference also governs the slow glass rim animation, so update its
    // running state at the same time as the material itself.
    applyTheme();
}

void AiChatPanel::showEvent(QShowEvent* event) {
    ui::GlassPanel::showEvent(event);
    startContentIndex(/*force=*/false);
    // A generation may have continued while the panel was hidden. Refresh
    // its one live field immediately without recreating the transcript.
    updateMusicElapsedLabel();
}

void AiChatPanel::startContentIndex(bool force) {
    if (!m_contentCatalog) return;
    std::vector<std::string> roots;
    for (const QString& folder : contentPaths())
        roots.push_back(folder.toStdString());
    m_contentCatalog->setBrowserRoots(std::move(roots));
    const ai::CatalogIndexStatus before = m_contentCatalog->status();
    if (force || before.state == ai::CatalogIndexState::Idle ||
        before.state == ai::CatalogIndexState::Cancelled)
        m_contentCatalog->startRefresh();
    updateContentIndexStatus();
}

void AiChatPanel::updateContentIndexStatus() {
    if (!m_contentCatalog || !m_contentIndexLabel) return;
    const ai::CatalogIndexStatus status = m_contentCatalog->status();
    QString text;
    QString tip;
    switch (status.state) {
        case ai::CatalogIndexState::Idle:
            text = tr("LIB —");
            tip = tr("The sound-library index has not started");
            break;
        case ai::CatalogIndexState::Scanning:
            text = tr("LIB SCAN");
            tip = tr("Scanning browser folders: %1 files found")
                      .arg(status.filesDiscovered);
            break;
        case ai::CatalogIndexState::ReadingMetadata: {
            const int percent = int(std::lround(
                status.progress().value_or(0.0) * 100.0));
            text = tr("LIB %1%").arg(percent);
            tip = tr("Reading sound metadata: %1 of %2")
                      .arg(status.filesProcessed)
                      .arg(status.filesDiscovered);
            break;
        }
        case ai::CatalogIndexState::CancelRequested:
            text = tr("LIB STOP");
            tip = tr("Stopping sound-library indexing");
            break;
        case ai::CatalogIndexState::Ready:
            text = tr("LIB %1").arg(status.filesPublished);
            tip = tr("%1 browser files are ready for the assistant")
                      .arg(status.filesPublished);
            break;
        case ai::CatalogIndexState::Cancelled:
            text = tr("LIB PAUSED");
            tip = tr("Sound-library indexing was paused");
            break;
    }
    m_contentIndexLabel->setText(text);
    m_contentIndexLabel->setToolTip(tip);
    if (m_contentIndexTicker) {
        if (status.running() && !m_contentIndexTicker->isActive())
            m_contentIndexTicker->start();
        else if (!status.running())
            m_contentIndexTicker->stop();
    }
}

void AiChatPanel::hideEvent(QHideEvent* event) {
    ui::GlassPanel::hideEvent(event);
}

QRect AiChatPanel::plateRect() const {
    return rect();
}

QPainterPath AiChatPanel::plateShape() const {
    const QRectF r = QRectF(plateRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (r.isEmpty()) return {};
    QPainterPath path;
    path.addRoundedRect(r, Theme::cornerRadius, Theme::cornerRadius);
    return path;
}

void AiChatPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    const QPainterPath shape = plateShape();
    QColor behind = mixColors(t.background, QColor(8, 9, 12),
                              t.dark ? 0.20 : 0.04);
    behind.setAlpha(255);
    painter.fillRect(rect(), behind);

    QColor surface = mixColors(t.surface, t.background, t.dark ? 0.20 : 0.08);
    surface.setAlpha(255);
    painter.fillPath(shape, surface);

    QColor edge = mixColors(t.separator(), t.accentHighlight, 0.12);
    edge.setAlphaF(t.dark ? 0.72 : 0.52);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(edge, 1.0));
    painter.drawPath(shape);
}

// ── Settings ────────────────────────────────────────────────────────────────

void AiChatPanel::reloadSettings() {
    // Account quota/model refreshes can arrive during a generation. Replacing
    // that client would silently cancel the request and leave the session busy.
    if (m_session && m_session->running()) return;
    ui::aiprefs::ModelConnection connection;
    QString active = ui::aiprefs::activeModelId();
    if (!ui::aiprefs::modelById(active, &connection)) {
        const QList<ui::aiprefs::ModelConnection> available =
            ui::aiprefs::availableModels();
        if (!available.isEmpty()) {
            connection = available.front();
            active = connection.id;
            ui::aiprefs::setActiveModelId(active);
        } else {
            active.clear();
        }
    }

    // A scripted client installed by a headless check must survive a settings
    // reload, or the check would be talking to the network instead.
    const bool scripted = dynamic_cast<ScriptedClient*>(m_client.get()) != nullptr;
    if (!scripted) {
        if (active.isEmpty()) {
            if (m_client) m_client->cancel();
            m_client.reset();
        } else {
        const auto wanted = connection.provider == ui::aiprefs::Provider::OpenAi
                                ? ui::LlmClient::Provider::OpenAi
                                : ui::LlmClient::Provider::Anthropic;
        if (!m_client || m_client->provider() != wanted) {
            if (m_client) m_client->cancel();
            m_client.reset(new ui::LlmClient(wanted, this));
        }

        ui::LlmConfig config;
        config.connectionId = connection.id;
        config.displayName = connection.displayName;
        config.model = connection.model;
        config.stream = ui::aiprefs::streaming();
        config.timeoutSeconds = ui::aiprefs::timeoutSeconds();
        config.maxRetries = ui::aiprefs::maxRetries();
        if (connection.source == ui::aiprefs::ModelSource::Managed) {
            config.transport = ui::LlmConfig::Transport::Managed;
            if (auto* account = account::Service::instance())
                config.accessToken = account->accessToken();
        } else {
            config.transport = ui::LlmConfig::Transport::Direct;
            config.endpoint = connection.endpoint;
            config.apiKey = ui::aiprefs::customApiKey(connection.id);
        }
        m_client->setConfig(config);
        }
    }

    if (m_musicClient &&
        dynamic_cast<ScriptedMusicClient*>(m_musicClient.get()) == nullptr) {
        ui::MusicConfig music;
        music.url = ui::aiprefs::musicUrl();
        music.model = ui::aiprefs::musicModel();
        music.format = ui::aiprefs::musicFormat();
        music.folder = ui::aiprefs::musicFolder();
        music.sampleRate = ui::aiprefs::musicSampleRate();
        music.bitrate = ui::aiprefs::musicBitrate();
        music.timeoutSeconds = ui::aiprefs::musicTimeoutSeconds();
        // The stored key is read at send time, like the chat's; the
        // environment costs nothing to read and never prompts.
        music.apiKey = QProcessEnvironment::systemEnvironment().value(
            QStringLiteral("MINIMAX_API_KEY"));
        m_musicClient->setConfig(std::move(music));
    }
    if (m_instrumentalButton)
        m_instrumentalButton->setChecked(ui::aiprefs::musicInstrumental());

    m_session->setMaxIterations(ui::aiprefs::maxIterations());
    m_session->setHistoryLimit(std::size_t(ui::aiprefs::historyLimit()));
    if (m_modelLabel && m_mode == Mode::Music) {
        m_modelLabel->setText(ui::aiprefs::musicModel());
        m_modelLabel->setToolTip(tr("Music model at %1")
                                     .arg(ui::aiprefs::musicUrl()));
    } else if (m_modelLabel && m_client) {
        m_modelLabel->setText(m_client->displayName());
        m_modelLabel->setToolTip(
            tr("%1 · %2 — click to use another model")
                .arg(m_client->provider() == ui::LlmClient::Provider::OpenAi
                         ? tr("GPT-compatible")
                         : tr("Claude-compatible"),
                     m_client->config().model));
    } else if (m_modelLabel) {
        m_modelLabel->setText(tr("No model"));
        m_modelLabel->setToolTip(tr("Open AI Settings to add a model."));
    }
    updateReadiness();
}

void AiChatPanel::populateModelMenu(QMenu* menu) {
    menu->clear();
    const QList<ui::aiprefs::ModelConnection> managed =
        ui::aiprefs::managedModels();
    const QList<ui::aiprefs::ModelConnection> custom =
        ui::aiprefs::customModels();
    const QString active = ui::aiprefs::activeModelId();
    const auto addModels = [this, menu, &active](
                               const QList<ui::aiprefs::ModelConnection>& models) {
        for (const ui::aiprefs::ModelConnection& model : models) {
        QAction* action = menu->addAction(model.displayName);
        action->setCheckable(true);
        action->setChecked(model.id == active);
        connect(action, &QAction::triggered, this, [this, id = model.id] {
            ui::aiprefs::setActiveModelId(id);
            reloadSettings();
        });
        }
    };
    addModels(managed);
    if (!managed.isEmpty() && !custom.isEmpty()) menu->addSeparator();
    addModels(custom);
    if (!managed.isEmpty() || !custom.isEmpty()) menu->addSeparator();
    connect(menu->addAction(tr("Manage models…")), &QAction::triggered, this,
            &AiChatPanel::settingsRequested);
}

void AiChatPanel::updateReadiness() {
    if (!m_stack) return;
    // Music needs no chat key — often no key at all, when the model runs on the
    // user's own server — so the "connect a model" page is the assistant's
    // alone.
    if (m_mode == Mode::Music) {
        m_stack->setCurrentIndex(2);
        if (m_composer) m_composer->show();
        if (m_input) m_input->setEnabled(true);
        return;
    }
    // A selected model is configured even while an account token is being
    // restored. Runtime authorization errors belong in the transcript; they
    // must not masquerade as a missing model and send the user back to setup.
    const bool ready = m_client != nullptr;
    m_stack->setCurrentIndex(ready ? 0 : 1);
    // The composer is still useful before a model is connected: the user can
    // draft or paste a request, then connect a model without losing it.
    if (m_composer) m_composer->show();
    if (m_input) m_input->setEnabled(true);
}

// ── Attachments ─────────────────────────────────────────────────────────────

QStringList AiChatPanel::contentPaths() const {
    QStringList paths = ui::browserprefs::aiContentPaths();
    if (m_attachments)
        for (int i = 0; i < m_attachments->count(); ++i)
            paths << m_attachments->item(i)->data(Qt::UserRole).toString();
    paths.removeDuplicates();
    return paths;
}

void AiChatPanel::setSelectionModel(ui::SelectionModel* selection) {
    if (m_selection) disconnect(m_selection, nullptr, this, nullptr);
    m_selection = selection;
    if (m_selection) connect(m_selection, &ui::SelectionModel::changed, this, &AiChatPanel::updateSelectionContext);
    updateSelectionContext();
}

void AiChatPanel::updateSelectionContext() {
    if (!m_contextButton) return;
    QStringList names;
    if (m_selection) {
        QSet<QString> tracks;
        for (const auto& id : m_selection->tracks()) tracks.insert(id);
        for (const auto& clip : m_selection->clips()) tracks.insert(clip.trackId);
        for (const auto& track : m_controller->project().tracks) {
            if (!tracks.contains(QString::fromStdString(track.id))) continue;
            names << QString::fromStdString(track.name);
        }
    }
    const int clips = m_selection ? int(m_selection->clips().size()) : 0;
    QString text = names.isEmpty() ? tr("Context · project and playhead")
        : tr("Selection · %1 · %2 clips").arg(names.join(", ")).arg(clips);
    if (m_uiContext) {
        const auto state = m_uiContext();
        if (state.contains("pianoRoll")) {
            const auto& piano = state["pianoRoll"];
            const auto count = piano.value("selectedNoteCount", 0);
            if (count > 0) text += tr(" · %1 notes").arg(count);
        }
    }
    m_contextButton->setText(text);
    m_contextButton->setToolTip(text + "\n" + tr("Click to inspect selection, instruments, samples and library access."));
    m_contextButton->setAccessibleName(text);
    if (m_attachments) {
        QHash<QString, QStringList> usage;
        const auto key = [](const QString& path) {
            QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
            normalized = normalized.toCaseFolded();
#endif
            return normalized;
        };
        for (const auto& track : m_controller->project().tracks) {
            const QString name = QString::fromStdString(track.name);
            for (const auto& clip : track.clips)
                if (!clip.filePath.empty()) usage[key(QString::fromStdString(clip.filePath))] << name;
            if (auto* sampler = m_controller->samplerInstance(track.id, track.instrument.id)) {
                const auto path = sampler->samplePath();
                if (!path.empty()) usage[key(QString::fromStdString(path))] << name;
            }
        }
        for (int i = 0; i < m_attachments->count(); ++i) {
            auto* item = m_attachments->item(i);
            const QString path = item->data(Qt::UserRole).toString();
            auto names = usage.value(key(path));
            names.removeDuplicates();
            item->setText(item->data(Qt::UserRole + 2).toString() + (names.isEmpty() ? QString() : tr(" · in project")));
            item->setToolTip(path + (names.isEmpty() ? QString() : "\n" + tr("Used by: %1").arg(names.join(", "))));
        }
    }
}

void AiChatPanel::showContext() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Assistant context"));
    dialog.resize(570, 440);
    auto* layout = new QVBoxLayout(&dialog);
    QString text = tr("Project: %1\nTempo: %2 BPM\n\nSelection is captured when you send a request.\n")
        .arg(QString::fromStdString(m_controller->projectName())).arg(m_controller->tempo());
    QSet<QString> tracks, clips;
    if (m_selection) {
        for (const auto& id : m_selection->tracks()) tracks.insert(id);
        for (const auto& clip : m_selection->clips()) { tracks.insert(clip.trackId); clips.insert(clip.clipId); }
    }
    for (const auto& track : m_controller->project().tracks) {
        if (!tracks.contains(QString::fromStdString(track.id))) continue;
        text += "\n" + QString::fromStdString(track.name) + "\n";
        if (track.instrument.isLoaded()) text += tr("Instrument: %1\n").arg(QString::fromStdString(track.instrument.name));
        if (auto* sampler = m_controller->samplerInstance(track.id, track.instrument.id))
            text += tr("Sampler source: %1\n").arg(QFileInfo(QString::fromStdString(sampler->samplePath())).fileName());
        for (const auto& slot : track.inserts)
            if (slot.isLoaded()) text += tr("Effect: %1\n").arg(QString::fromStdString(slot.name));
        for (const auto& clip : track.clips) {
            if (!clips.isEmpty() && !clips.contains(QString::fromStdString(clip.id))) continue;
            text += tr("Clip: %1").arg(QString::fromStdString(clip.name));
            if (!clip.filePath.empty()) text += tr(" · Sample: %1").arg(QFileInfo(QString::fromStdString(clip.filePath)).fileName());
            text += '\n';
        }
    }
    text += "\n" + tr("Allowed library and attachments:") + "\n";
    const auto paths = contentPaths();
    text += paths.isEmpty() ? tr("No files or folders added.") : paths.join('\n');
    text += "\n\n" + tr("Only added library locations and explicit attachments are indexed. Folder attachments include their subfolders.");
    auto* details = new QPlainTextEdit(text, &dialog);
    details->setReadOnly(true);
    layout->addWidget(details);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(close);
    dialog.exec();
}

QString AiChatPanel::conversationText() const {
    QString text;
    for (const auto& message : m_session->messages()) {
        if (!message.text.empty()) text += (message.role == ai::Role::User ? tr("You") : tr("VLT AI")) + ":\n" + QString::fromStdString(message.text) + "\n\n";
        for (const auto& outcome : message.outcomes)
            text += QString::fromStdString(outcome.name) + ": " + QString::fromStdString(outcome.result.dump(2)) + "\n\n";
    }
    if (!m_streaming.isEmpty()) text += tr("VLT AI · Writing") + ":\n" + m_streaming;
    return text;
}

void AiChatPanel::showConversation() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Conversation"));
    dialog.resize(650, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* search = new QLineEdit(&dialog);
    search->setPlaceholderText(tr("Find in conversation — Enter for next match"));
    search->setAccessibleName(tr("Find in conversation"));
    layout->addWidget(search);
    auto* text = new QPlainTextEdit(conversationText(), &dialog);
    text->setReadOnly(true);
    layout->addWidget(text);
    connect(search, &QLineEdit::returnPressed, &dialog, [text, search] {
        if (!text->find(search->text())) { text->moveCursor(QTextCursor::Start); text->find(search->text()); }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copy = buttons->addButton(tr("Copy all"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    auto* save = buttons->addButton(tr("Export…"), QDialogButtonBox::ActionRole);
    connect(save, &QPushButton::clicked, &dialog, [this, text, &dialog] {
        const auto path = QFileDialog::getSaveFileName(&dialog, tr("Export conversation"), "conversation.txt", tr("Text files (*.txt)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto bytes = text->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(&dialog, tr("Export conversation"), tr("Could not save the conversation."));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    search->setFocus();
    dialog.exec();
}

void AiChatPanel::addMessageActions(QWidget* card, QVBoxLayout* layout, std::size_t index) {
    const auto& message = m_session->messages()[index];
    const QString text = QString::fromStdString(message.text);
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    auto* copy = messageAction(card, icons::Glyph::Copy, tr("Copy message"));
    copy->setObjectName("AiMessageAction");
    copy->setProperty("aiReadOnlyAction", true);
    copy->setAccessibleName(tr("Copy message"));
    connect(copy, &QToolButton::clicked, this, [this, text] {
        QApplication::clipboard()->setText(text); emit statusMessage(tr("Message copied"));
    });
    actions->addWidget(copy);
    if (message.role == ai::Role::User) {
        auto* edit = messageAction(card, icons::Glyph::Edit, tr("Edit as new request"));
        edit->setToolTip(tr("Edit as new request"));
        edit->setObjectName("AiMessageAction");
        edit->setProperty("aiReadOnlyAction", true);
        connect(edit, &QToolButton::clicked, this, [this, text] {
            if (!m_input->toPlainText().trimmed().isEmpty() &&
                QMessageBox::question(this, tr("Edit request"), tr("Replace the current draft?")) != QMessageBox::Yes) return;
            m_input->setPlainText(text); m_input->setFocus();
        });
        actions->addWidget(edit);
    }
    const bool canRevert = std::any_of(m_session->checkpoints().begin(), m_session->checkpoints().end(),
        [index](const auto& checkpoint) { return checkpoint.messageIndex == index; });
    if (message.role == ai::Role::User && canRevert) {
        auto* revert = messageAction(card, icons::Glyph::Undo,
                                    tr("Restore the project to before this request"));
        revert->setEnabled(!m_session->running());
        connect(revert, &QToolButton::clicked, this, [this, index] { revertToMessage(index); });
        actions->addWidget(revert);
    }
    actions->setSpacing(2);
    actions->addStretch();
    layout->addLayout(actions);
}

void AiChatPanel::continueRequest() {
    if (!m_client || !m_session->resume()) return;
    m_followOutput = true;
    renderTranscript(); updateBusyState(); step();
}

void AiChatPanel::addAttachment(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || (!info.isFile() && !info.isDir())) return;
    const QString absolute = info.canonicalFilePath();
    for (int i = 0; i < m_attachments->count(); ++i)
        if (m_attachments->item(i)->data(Qt::UserRole).toString() == absolute)
            return;

    audio::platform::AudioFileInfo probed;
    const bool decodable =
        info.isFile() && audio::platform::probeAudioFile(absolute.toStdString(), probed).isOk();
    const auto suffix = info.suffix().toLower();
    if (!info.isDir() && !decodable && suffix != "mid" && suffix != "midi") {
        emit statusMessage(tr("Attach an audio file, MIDI file or sample folder."));
        return;
    }

    auto* item = new QListWidgetItem(m_attachments);
    item->setData(Qt::UserRole, absolute);
    item->setData(Qt::UserRole + 1, "attachment_" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    item->setText(info.isDir() ? tr("Folder · %1").arg(info.fileName()) : decodable ? QStringLiteral("%1  ·  %2 s")
                                  .arg(info.fileName())
                                  .arg(probed.durationSeconds(), 0, 'f', 1)
                            : info.fileName());
    item->setToolTip(absolute);
    item->setData(Qt::UserRole + 2, item->text());
    item->setIcon(icons::icon(decodable ? icons::Glyph::Waveform
                                        : icons::Glyph::Import,
                              th().textSecondary, 12));
    refreshAttachments();
    updateSelectionContext();
    emit statusMessage(tr("Attached %1").arg(info.fileName()));
}

void AiChatPanel::refreshAttachments() {
    const bool any = m_attachments->count() > 0;
    m_attachmentButton->setVisible(any && m_mode == Mode::Assistant);
    m_attachmentButton->setText(tr("Attachments · %1").arg(m_attachments->count()));
    QStringList names;
    for (int i = 0; i < m_attachments->count(); ++i) names << m_attachments->item(i)->text();
    m_attachmentButton->setToolTip(tr("Click to inspect attachments. Select an item and press Delete to detach.") + "\n" + tr("Double-click a folder to see indexed samples.") + "\n" + names.join('\n'));
    m_attachments->setVisible(any && m_mode == Mode::Assistant &&
                              m_attachmentButton->isChecked());
    startContentIndex(false);
    {
        auto context = m_session->context();
        context.attachments.clear();
        for (int i = 0; i < m_attachments->count(); ++i) {
            const auto* item = m_attachments->item(i);
            const QString path = item->data(Qt::UserRole).toString();
            const QFileInfo info(path);
            audio::platform::AudioFileInfo audio;
            if (info.isFile()) audio::platform::probeAudioFile(path.toStdString(), audio);
            context.attachments.push_back({info.fileName().toStdString(), path.toStdString(), audio.durationSeconds(),
                int(audio.sampleRate), int(audio.channels), item->data(Qt::UserRole + 1).toString().toStdString(), info.isDir()});
        }
        context.sampleFolders.clear();
        for (const auto& path : contentPaths()) context.sampleFolders.push_back(path.toStdString());
        m_session->setContext(std::move(context));
    }
    // The plus control and drag tooltip carry this affordance without keeping
    // a permanent instruction line above every message.
    m_attachHint->hide();
}

void AiChatPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void AiChatPanel::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls())
        if (url.isLocalFile()) addAttachment(url.toLocalFile());
    event->acceptProposedAction();
}

bool AiChatPanel::eventFilter(QObject* watched, QEvent* event) {
    if (m_mode == Mode::Assistant && (event->type() == QEvent::ShortcutOverride ||
                                     event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Find)) {
            event->accept();
            if (event->type() == QEvent::KeyPress) showConversation();
            return true;
        }
    }
    if (watched == m_input) {
        if (event->type() == QEvent::FocusIn ||
            event->type() == QEvent::FocusOut) {
            const bool focused = event->type() == QEvent::FocusIn;
            if (m_composer &&
                m_composer->property("inputFocused").toBool() != focused) {
                m_composer->setProperty("inputFocused", focused);
                m_composer->style()->unpolish(m_composer);
                m_composer->style()->polish(m_composer);
                m_composer->update();
            }
        } else if (event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                event->accept();
        } else if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            const bool enter =
                key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            if (enter && !(key->modifiers() & Qt::ShiftModifier)) {
                send();
                return true;
            }
        }
    }
    if (watched == m_attachments && (event->type() == QEvent::KeyPress ||
                                    event->type() == QEvent::ShortcutOverride)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            qDeleteAll(m_attachments->selectedItems());
            refreshAttachments();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ── Running a turn ──────────────────────────────────────────────────────────

void AiChatPanel::send() {
    if (m_mode == Mode::Music) {
        sendMusic();
        return;
    }
    if (m_session->running()) return;
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;
    if (!m_client) {
        emit statusMessage(tr("Connect an AI model to send this request"));
        emit settingsRequested();
        return;
    }

    if (auto* account = account::Service::instance(); account &&
        m_client->config().accessToken != account->accessToken()) {
        ui::LlmConfig config = m_client->config();
        config.accessToken = account->accessToken();
        m_client->setConfig(std::move(config));
    }

    ai::ToolContext context;
    context.uiContext = m_uiContext;
    for (int i = 0; i < m_attachments->count(); ++i) {
        const QString path = m_attachments->item(i)->data(Qt::UserRole).toString();
        audio::platform::AudioFileInfo probed;
        const bool folder = QFileInfo(path).isDir();
        if (!folder) audio::platform::probeAudioFile(path.toStdString(), probed);
        context.attachments.push_back(
            ai::Attachment{QFileInfo(path).fileName().toStdString(),
                           path.toStdString(), probed.durationSeconds(),
                           int(probed.sampleRate), int(probed.channels),
                           m_attachments->item(i)->data(Qt::UserRole + 1).toString().toStdString(), folder});
    }

    // What the user is looking at, so "this" and "here" mean something. Read at
    // send time rather than held, because the selection moves while they type.
    if (m_selection) {
        const ui::ClipSel clip = m_selection->singleClip();
        context.focus.trackId = clip.trackId.isEmpty()
                                    ? m_selection->singleTrack().toStdString()
                                    : clip.trackId.toStdString();
        context.focus.clipId = clip.clipId.toStdString();
        const auto addTrack = [&context](const QString& id) {
            const std::string value = id.toStdString();
            if (!value.empty() &&
                std::find(context.focus.trackIds.begin(),
                          context.focus.trackIds.end(), value) ==
                    context.focus.trackIds.end())
                context.focus.trackIds.push_back(value);
        };
        for (const QString& id : m_selection->tracks()) addTrack(id);
        for (const ui::ClipSel& selected : m_selection->clips()) {
            addTrack(selected.trackId);
            if (!selected.clipId.isEmpty())
                context.focus.clipIds.push_back(selected.clipId.toStdString());
        }
    }
    // The assistant searches exactly the folders the browser shows, and nothing
    // else — the same promise the browser makes to the user.
    for (const QString& folder : contentPaths())
        context.sampleFolders.push_back(folder.toStdString());
    startContentIndex(/*force=*/false);
    context.contentCatalog = m_contentCatalog;
    context.compositionCandidates = m_compositionCandidates;

    // The instructions in force, which may have been edited on the server
    // since this session started. Read at send time for exactly that reason.
    if (auto* prompts = ui::PromptService::instance())
        context.prompts = &prompts->pack();
    context.projectPath = m_projectPath.toStdString();

    // Everything the assistant does is one Ctrl+Z, so it acts without asking —
    // except for deleting things, where the cost of being wrong is the user's
    // own work and a dialog is worth the interruption.
    context.confirmDestructive = [this](const std::string& what) {
        return QMessageBox::question(
                   this, tr("Assistant"),
                   tr("The assistant wants to %1.\n\nAllow it?")
                       .arg(QString::fromStdString(what)),
                   QMessageBox::Yes | QMessageBox::No,
                   QMessageBox::No) == QMessageBox::Yes;
    };

    context.searchCommands = [this](const std::string& query,
                                    ai::InteractionMode mode) {
        json commands = json::array();
        if (!m_commands) return json{{"commands", std::move(commands)}};
        const QVector<ShortcutManager::Command> matches =
            m_commands->search(QString::fromStdString(query));
        for (const ShortcutManager::Command& command : matches) {
            if (commands.size() >= 60 ||
                !commandAllowsMode(command.metadata, mode))
                continue;
            const QString shortcut =
                m_commands->shortcut(command.id).toString(
                    QKeySequence::NativeText);
            commands.push_back(
                json{{"commandId", command.id.toStdString()},
                     {"label", command.label.toStdString()},
                     {"category", command.category.toStdString()},
                     {"description", command.metadata.description.toStdString()},
                     {"helpId", command.metadata.helpId.toStdString()},
                     {"risk", commandRiskName(command.metadata.risk)},
                     {"shortcut", shortcut.toStdString()},
                     {"enabled", command.action && command.action->isEnabled()},
                     {"visible", command.action && command.action->isVisible()}});
        }
        return json{{"commands", std::move(commands)}};
    };
    context.invokeCommand = [this](const std::string& id,
                                   ai::InteractionMode mode,
                                   std::string& error) {
        if (!m_commands) {
            error = "the program command catalog is unavailable";
            return false;
        }
        const QString commandId = QString::fromStdString(id);
        const ShortcutManager::Command* command =
            m_commands->command(commandId);
        if (!command) {
            error = "no command with id '" + id +
                    "'; call search_commands and use an exact commandId";
            return false;
        }
        if (!commandAllowsMode(command->metadata, mode)) {
            error = "that command is not allowed in the active interaction mode";
            return false;
        }
        if (!command->action || !command->action->isVisible() ||
            !command->action->isEnabled()) {
            error = "command '" + id +
                    "' is not currently visible and enabled in this UI state";
            return false;
        }
        const bool needsConfirmation =
            command->metadata.risk == ShortcutManager::Risk::Unknown ||
            command->metadata.risk == ShortcutManager::Risk::Destructive ||
            command->metadata.risk ==
                ShortcutManager::Risk::ExternalSideEffect;
        if (needsConfirmation &&
            QMessageBox::question(
                this, tr("Assistant command"),
                tr("The assistant wants to run “%1”.\n\nRisk: %2\n\nAllow it?")
                    .arg(command->label,
                         QString::fromLatin1(
                             commandRiskName(command->metadata.risk))),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            error = "the user declined command '" + id + "'";
            return false;
        }
        if (!m_commands->invoke(commandId)) {
            error = "command '" + id + "' became unavailable before it ran";
            return false;
        }
        return true;
    };

    m_session->setContext(std::move(context));

    m_session->begin(text.toStdString());
    m_input->clear();
    m_followOutput = true;
    renderTranscript();
    updateBusyState();
    step();
}

void AiChatPanel::step() {
    m_client->setStatusSink([this](const QString& status) {
        m_requestStatus = status;
        if (m_requestStatusLabel) m_requestStatusLabel->setText(status);
    });
    m_client->setAvailableTools(m_session->availableTools());
    m_client->setUsageSink([this](ai::AiSession::Usage usage) {
        m_session->addUsage(usage);
        updateUsageLabel();
    });
    // Streamed prose is shown as it is written; it is not in the transcript
    // yet, so it is held here and drawn under it until the reply lands.
    m_streaming.clear();
    m_client->setPartialSink([this](const QString& text) {
        m_streaming += text;
        if (m_streamFlushPending) return;
        m_streamFlushPending = true;
        QTimer::singleShot(16, this, [this] {
            m_streamFlushPending = false;
            if (m_streamingLabel) {
                m_streamingLabel->setText(m_streaming);
                if (m_transcript && m_followOutput) m_transcript->verticalScrollBar()->setValue(
                    m_transcript->verticalScrollBar()->maximum());
            } else if (!m_streaming.isEmpty()) renderTranscript();
        });
    });
    // `wireMessages`, not `messages`: the transcript keeps the whole
    // conversation, the request carries only the recent turns.
    m_client->send(QString::fromStdString(m_session->systemPrompt()),
                   m_session->wireMessages(),
                   [this](ai::ModelReply reply) { onReply(std::move(reply)); });
}

void AiChatPanel::updateUsageLabel() {
    if (!m_usageLabel) return;
    const auto* account = account::Service::instance();
    if (!account || !account->authenticated() ||
        account->snapshot().tokenLimit <= 0) {
        m_usageLabel->clear();
        m_usageLabel->setToolTip({});
        return;
    }
    const account::Snapshot& quota = account->snapshot();
    const int percent = std::clamp(
        int(std::lround(double(quota.tokensUsed) * 100.0 /
                        double(quota.tokenLimit))),
        0, 100);
    m_usageLabel->setText(tr("%1%").arg(percent));
    m_usageLabel->setToolTip(
        tr("%1% of the monthly AI allowance used.").arg(percent));
}

void AiChatPanel::onReply(ai::ModelReply reply) {
    m_streaming.clear();
    const auto revision = m_controller->projectRevision();
    const ai::AiSession::Step next = m_session->applyReply(reply);
    renderTranscript();
    // The document may have changed under the shell's views, whether or not the
    // run is over — the tracks should appear as they are made, not at the end.
    if (m_controller->projectRevision() != revision) emit projectChanged();
    updateSelectionContext();

    if (next == ai::AiSession::Step::NeedsRequest) {
        step();
        return;
    }
    endRun();
}

void AiChatPanel::endRun() {
    updateBusyState();
    reloadSettings();
    if (!m_session->lastError().empty())
        emit statusMessage(
            tr("Assistant: %1")
                .arg(QString::fromStdString(m_session->lastError())));
    else
        emit statusMessage(tr("Assistant finished"));
}

void AiChatPanel::stop() {
    if (m_mode == Mode::Music) {
        if (m_musicClient) m_musicClient->cancel();
        m_musicTicker->stop();
        if (!m_musicTurns.empty() && m_musicTurns.back().pending) {
            m_musicTurns.back().pending = false;
            m_musicTurns.back().error = tr("Stopped.");
        }
        renderMusicTranscript();
        updateBusyState();
        emit statusMessage(tr("Music generation stopped"));
        return;
    }
    m_session->cancel();
    if (m_client) m_client->cancel();
    // `cancel` drops the callback, so nothing else will close the run.
    m_session->applyReply(ai::ModelReply{m_streaming.toStdString(), {}, tr("Stopped. You can continue this request.").toStdString()});
    m_streaming.clear();
    renderTranscript();
    updateBusyState();
}

void AiChatPanel::updateBusyState() {
    const bool running = m_mode == Mode::Music ? musicPending()
                                               : m_session->running();
    m_sendButton->setVisible(!running);
    m_stopButton->setVisible(running);
    // Keep the editor live so a follow-up can be drafted or pasted while the
    // response runs. The Stop button replaces Send until this turn finishes.
    m_input->setEnabled(true);
    if (m_modelLabel) m_modelLabel->setEnabled(!running);
}

// ── The music mode ──────────────────────────────────────────────────────────

bool AiChatPanel::musicPending() const {
    return !m_musicTurns.empty() && m_musicTurns.back().pending;
}

void AiChatPanel::setMode(Mode mode, bool persist) {
    if (m_mode == mode) return;
    // A request in flight belongs to the mode that started it: leaving it
    // running while its transcript is hidden would land a track from nowhere.
    if (m_session->running() || musicPending()) stop();

    m_mode = mode;
    if (persist) ui::aiprefs::setMode(mode);
    if (m_modeSwitch) {
        // The switch may be the thing that called this, and a `setRight` that
        // echoed its own signal would run the handler a second time.
        const QSignalBlocker block(m_modeSwitch);
        m_modeSwitch->setRight(mode == Mode::Music);
    }
    applyModeToComposer();
    reloadSettings();          // the model line names a different model now
    updateBusyState();
    updateReadiness();
    emit statusMessage(mode == Mode::Music
                           ? tr("Music mode — requests generate audio")
                           : tr("Assistant mode — requests work the program"));
}

void AiChatPanel::applyModeToComposer() {
    const bool music = m_mode == Mode::Music;
    if (m_titleLabel)
        m_titleLabel->setText(music ? tr("AI music") : tr("AI chat"));
    if (m_instrumentalButton) m_instrumentalButton->setVisible(music);
    if (m_attachHint) m_attachHint->hide();
    if (m_attachmentButton) m_attachmentButton->setVisible(!music &&
                                                            m_attachments->count() > 0);
    if (m_attachments) m_attachments->setVisible(!music && m_attachmentButton->isChecked() &&
                                                 m_attachments->count() > 0);
    if (m_promptsButton) m_promptsButton->setVisible(!music);
    if (!m_input) return;
    m_input->setPlaceholderText(
        music ? tr("Warm lo-fi beat, dusty piano, brushed drums…")
              : tr("Write a message…"));
}

void AiChatPanel::sendMusic() {
    if (musicPending() || !m_musicClient) return;
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;

    // Same rule as the chat: the stored secret is read here and nowhere else.
    // Unlike the chat, an empty key is fine — a server of one's own may want
    // no authorization at all.
    if (m_musicClient->config().apiKey.isEmpty()) {
        ui::MusicConfig config = m_musicClient->config();
        config.apiKey = ui::aiprefs::musicApiKey();
        m_musicClient->setConfig(std::move(config));
    }

    // What the user is looking at, read at send time — the same context the
    // assistant gets, and the only thing that makes "fits this track" mean
    // anything.
    ai::ToolContext context;
    if (m_selection) {
        const ui::ClipSel clip = m_selection->singleClip();
        context.focus.trackId = clip.trackId.isEmpty()
                                    ? m_selection->singleTrack().toStdString()
                                    : clip.trackId.toStdString();
        context.focus.clipId = clip.clipId.toStdString();
    }

    const bool instrumental =
        m_instrumentalButton && m_instrumentalButton->isChecked();
    const ai::MusicBrief brief = ai::buildBrief(*m_controller,
                                                text.toStdString(), context,
                                                instrumental);

    MusicTurn turn;
    turn.request = text;
    turn.prompt = QString::fromStdString(brief.prompt);
    turn.lyrics = QString::fromStdString(brief.lyrics);
    turn.instrumental = instrumental;
    turn.pending = true;
    m_musicTurns.push_back(turn);

    m_input->clear();
    m_musicElapsed = 0;
    m_musicTicker->start();
    renderMusicTranscript();
    updateBusyState();

    m_musicClient->generate(brief, [this](ui::MusicClient::Outcome outcome) {
        m_musicTicker->stop();
        if (m_musicTurns.empty()) return;
        MusicTurn& turn = m_musicTurns.back();
        turn.pending = false;
        if (!outcome.error.isEmpty() || outcome.filePath.isEmpty()) {
            turn.error = outcome.error.isEmpty()
                             ? tr("nothing came back from the music server.")
                             : outcome.error;
            renderMusicTranscript();
            updateBusyState();
            emit statusMessage(tr("Music generation failed: %1").arg(turn.error));
            return;
        }
        turn.filePath = outcome.filePath;
        turn.seconds = outcome.seconds;
        turn.trackName = QString::fromStdString(ai::slug(turn.request.toStdString()));
        if (turn.trackName.isEmpty()) turn.trackName = tr("Generated");
        if (!insertGenerated(turn.filePath, turn.trackName)) {
            turn.error = tr("the file was saved but could not be decoded: %1")
                             .arg(QDir::toNativeSeparators(turn.filePath));
        } else {
            emit statusMessage(tr("Generated audio added on \"%1\"")
                                   .arg(turn.trackName));
        }
        renderMusicTranscript();
        updateBusyState();
    });
}

bool AiChatPanel::insertGenerated(const QString& path,
                                  const QString& trackName) {
    const std::size_t mark = m_controller->undoDepth();
    const std::string track =
        m_controller->addTrack(daw::TrackKind::Audio, trackName.toStdString());
    const std::string clip = m_controller->importAudio(
        path.toStdString(), track, m_controller->positionSeconds());
    if (clip.empty()) {
        // Undo our own `addTrack` rather than leaving an empty lane behind: a
        // failed generation should cost the project nothing.
        m_controller->undo();
        emit projectChanged();
        return false;
    }
    // Two edits the user thinks of as one arrival, so they undo as one.
    m_controller->collapseUndo(mark, "Generate music");
    if (m_selection) m_selection->setTracks({QString::fromStdString(track)});
    emit projectChanged();
    return true;
}

void AiChatPanel::renderMusicTranscript() {
    if (!m_musicLayout || !m_musicBody) return;
    // The label belongs to a card deleted below. Clear it before removing the
    // widgets so a timer tick can never reach a stale pointer.
    m_musicElapsedLabel = nullptr;
    while (QLayoutItem* item = m_musicLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const MusicTurn& turn : m_musicTurns) {
        auto ask = messageCard(m_musicBody, "AiUserCard", tr("YOU"), "AiUserRole");
        ask.second->addWidget(cardText(m_musicBody, turn.request, "AiMessageText"));
        if (turn.instrumental)
            ask.second->addWidget(cardText(m_musicBody, tr("INSTRUMENTAL"),
                                           "AiMessageSecondary", true));
        wrapRow(m_musicLayout, m_musicBody, ask.first, 1, 8, 0);

        if (turn.pending) {
            auto card = messageCard(m_musicBody, "AiThinkingCard",
                                    tr("AI / GENERATING"), "AiAssistantRole");
            auto* elapsed = cardText(m_musicBody, {}, "AiMessageSecondary", true);
            card.second->addWidget(elapsed);
            // sendMusic() permits only one request at a time, therefore only
            // the newest pending turn owns the live elapsed-time label.
            if (&turn == &m_musicTurns.back()) m_musicElapsedLabel = elapsed;
            updateMusicElapsedLabel();
            wrapRow(m_musicLayout, m_musicBody, card.first, 0, 6, 4);
            continue;
        }
        if (!turn.error.isEmpty()) {
            auto card = messageCard(m_musicBody, "AiErrorCard", tr("AI / ERROR"),
                                    "AiActionStatusError");
            card.second->addWidget(
                cardText(m_musicBody, turn.error, "AiMessageText"));
            wrapRow(m_musicLayout, m_musicBody, card.first, 0, 10, 1);
            continue;
        }

        auto card = messageCard(m_musicBody, "AiActionCard", tr("AI / MUSIC"),
                                "AiActionRole");
        card.second->addWidget(cardText(m_musicBody,
                                        QFileInfo(turn.filePath).fileName(),
                                        "AiMessageText"));
        QString detail = tr("added on \"%1\"").arg(turn.trackName);
        const QString length = formatDuration(turn.seconds);
        if (!length.isEmpty()) detail = length + " · " + detail;
        card.second->addWidget(
            cardText(m_musicBody, detail, "AiMessageSecondary", true));
        // What was actually sent, in full: the project's own facts were added
        // to the request, and the user should be able to see what they were.
        auto* brief = cardText(m_musicBody, turn.prompt, "AiActionText", true);
        brief->setToolTip(turn.lyrics.isEmpty()
                              ? turn.prompt
                              : turn.prompt + "\n\n" + turn.lyrics);
        card.second->addWidget(brief);

        auto* again = new QPushButton(tr("INSERT AGAIN"), card.first);
        again->setObjectName("AiRevertButton");
        again->setCursor(Qt::PointingHandCursor);
        again->setToolTip(tr("Put this audio on another new track at the "
                             "playhead"));
        const QString path = turn.filePath;
        const QString name = turn.trackName;
        connect(again, &QAbstractButton::clicked, this, [this, path, name] {
            if (insertGenerated(path, name))
                emit statusMessage(tr("Generated audio added again"));
        });
        card.second->addWidget(again, 0, Qt::AlignLeft);
        wrapRow(m_musicLayout, m_musicBody, card.first, 0, 10, 1);
    }

    if (m_musicTurns.empty()) {
        auto card = messageCard(m_musicBody, "AiAssistantCard",
                                tr("MUSIC GENERATION"), "AiAssistantRole");
        card.second->addWidget(cardText(
            m_musicBody,
            tr("Describe the music and it is generated and dropped on a new "
               "audio track at the playhead. The tempo, key and the tracks you "
               "already have are sent with the request, so what comes back "
               "fits the session."),
            "AiMessageSecondary", true));
        wrapRow(m_musicLayout, m_musicBody, card.first, 0, 10, 1);
    }

    m_musicLayout->addStretch(1);
    QTimer::singleShot(0, m_musicTranscript, [this] {
        if (!m_musicTranscript) return;
        m_musicTranscript->verticalScrollBar()->setValue(
            m_musicTranscript->verticalScrollBar()->maximum());
    });
}

void AiChatPanel::updateMusicElapsedLabel() {
    if (!isVisible() || !m_musicElapsedLabel || !musicPending()) return;
    const QString text = tr("Writing the music… %1 s").arg(m_musicElapsed);
    if (m_musicElapsedLabel->text() != text)
        m_musicElapsedLabel->setText(text);
}

// ── The transcript ──────────────────────────────────────────────────────────

void AiChatPanel::revertToMessage(std::size_t index) {
    // A checkpoint restores the whole document, so anything done after that
    // request goes too. Said plainly rather than discovered after the click.
    if (QMessageBox::question(
            this, tr("Assistant"),
            tr("Put the project back to how it was before that request?\n\n"
               "Anything changed since — by the assistant or by you — is "
               "undone as well. One Ctrl+Z brings it all back."),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!m_session->revertTo(index)) return;
    renderTranscript();
    emit projectChanged();
    emit statusMessage(tr("Reverted to before that request"));
}

void AiChatPanel::renderTranscript() {
    if (!m_transcript || !m_transcriptLayout) return;

    m_streamingLabel = nullptr;
    m_requestStatusLabel = nullptr;
    const bool follow = m_followOutput;
    const int scrollPosition = m_transcript->verticalScrollBar()->value();
    const QSignalBlocker scrollSignals(m_transcript->verticalScrollBar());

    // Which user turns can still be taken back, so the link is only offered
    // where it would actually work.
    QSet<qulonglong> revertable;
    for (const ai::Checkpoint& point : m_session->checkpoints())
        revertable.insert(qulonglong(point.messageIndex));

    const std::vector<ai::Message>& messages = m_session->messages();
    std::vector<std::size_t> hashes;
    hashes.reserve(messages.size());
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        size_t hash = std::hash<std::string>{}(message.text);
        const auto mix = [&](size_t value) { hash ^= value + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2); };
        mix(size_t(message.role)); mix(revertable.contains(qulonglong(i)));
        for (const auto& outcome : message.outcomes) {
            mix(std::hash<std::string>{}(outcome.callId));
            mix(std::hash<std::string>{}(outcome.name)); mix(outcome.ok);
            mix(std::hash<std::string>{}(outcome.result.dump()));
        }
        hashes.push_back(hash);
    }
    size_t prefix = 0;
    while (prefix < hashes.size() && prefix < m_transcriptHashes.size() &&
           hashes[prefix] == m_transcriptHashes[prefix]) ++prefix;
    // The final tool capsule may absorb newly appended tool messages. Keep
    // complete earlier cards, rebuild only that tail and transient live/error UI.
    size_t start = 0;
    int keep = 0;
    while (keep < m_transcriptLayout->count()) {
        auto* widget = m_transcriptLayout->itemAt(keep)->widget();
        if (!widget || !widget->property("aiMessageEnd").isValid()) break;
        const auto end = widget->property("aiMessageEnd").toULongLong();
        if (end > prefix || end >= m_transcriptHashes.size()) break;
        start = size_t(end); ++keep;
        for (auto* button : widget->findChildren<QAbstractButton*>())
            button->setEnabled(button->property("aiReadOnlyAction").toBool() || !m_session->running());
    }
    while (auto* item = m_transcriptLayout->takeAt(keep)) {
        delete item->widget(); delete item;
    }
    m_transcriptHashes = std::move(hashes);
    for (std::size_t at = start; at < messages.size(); ++at) {
        const int beforeItems = m_transcriptLayout->count();
        const ai::Message& message = messages[at];
        switch (message.role) {
            case ai::Role::User: {
                auto card = messageCard(m_transcriptBody, "AiUserCard", tr("You"), "AiUserRole");
                card.second->addWidget(cardText(m_transcriptBody, 
                    QString::fromStdString(message.text), "AiMessageText"));
                addMessageActions(card.first, card.second, at);
                wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 1, 8, 0);
                break;
            }

            case ai::Role::Assistant: {
                if (!message.text.empty()) {
                    auto card = messageCard(m_transcriptBody, "AiAssistantCard", tr("VLT AI"),
                                         "AiAssistantRole");
                    card.second->addWidget(cardText(m_transcriptBody, 
                        QString::fromStdString(message.text), "AiMessageText"));
                    addMessageActions(card.first, card.second, at);
                    wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
                }
                break;
            }

            case ai::Role::Tool: {
                auto card = messageCard(m_transcriptBody, "AiActionCard", tr("Actions"),
                                     "AiActionRole");
                // A model run often sends one tool result per wire message.
                // Consecutive results are one visible activity, so collect
                // them into a single action capsule instead of repeating the
                // same heading three times.
                std::size_t toolAt = at;
                while (toolAt < messages.size()) {
                    // Tool-only model rounds leave an empty assistant wire
                    // message between their results. It has no visible prose
                    // and must not split one activity into several cards.
                    if (messages[toolAt].role == ai::Role::Assistant &&
                        messages[toolAt].text.empty()) {
                        ++toolAt;
                        continue;
                    }
                    if (messages[toolAt].role != ai::Role::Tool) break;
                    for (const ai::ToolOutcome& out : messages[toolAt].outcomes) {
                        auto* actionRow = new QWidget(card.first);
                        auto* actionLayout = new QHBoxLayout(actionRow);
                        actionLayout->setContentsMargins(0, 1, 0, 1);
                        actionLayout->setSpacing(7);

                        const std::string resultError =
                            out.ok ? std::string()
                                   : out.result.value("error", std::string());
                        const bool replan =
                            resultError.find("project changed") !=
                            std::string::npos;
                        auto* status = new QLabel(
                            out.ok ? tr("Done")
                                   : replan ? tr("Replan") : tr("Error"),
                            actionRow);
                        status->setObjectName(
                            out.ok ? "AiActionStatusOk"
                                   : replan ? "AiActionStatusWarn"
                                            : "AiActionStatusError");
                        status->setAlignment(Qt::AlignCenter);
                        status->setSizePolicy(QSizePolicy::Fixed,
                                              QSizePolicy::Fixed);
                        actionLayout->addWidget(status, 0, Qt::AlignTop);

                        const auto activityLabel = [this](const std::string& name) {
                            if (name == "add_track") return tr("Create track");
                            if (name == "add_midi_clip") return tr("Create MIDI clip");
                            if (name == "set_clip_notes") return tr("Write notes");
                            if (name == "compose_candidates") return tr("Prepare musical variations");
                            if (name == "get_project" || name == "get_project_context") return tr("Read project");
                            if (name == "load_sample") return tr("Load sample");
                            if (name == "add_plugin") return tr("Add instrument or effect");
                            if (name == "set_plugin_parameter") return tr("Adjust sound");
                            return tr("Project action");
                        };
                        QString detail = activityLabel(out.name);
                        if (!out.ok) {
                            if (!resultError.empty())
                                detail += QStringLiteral(" — ") +
                                          QString::fromStdString(resultError);
                        }
                        auto* action = cardText(m_transcriptBody, detail, "AiActionText", true);
                        action->setToolTip(QString::fromStdString(out.name));
                        actionLayout->addWidget(action, 1);
                        card.second->addWidget(actionRow);

                        if (out.ok && out.name == "compose_candidates" &&
                            out.result.contains("candidates") &&
                            out.result["candidates"].is_array()) {
                            int choice = 0;
                            for (const json& candidate :
                                 out.result["candidates"]) {
                                if (!candidate.is_object()) continue;
                                auto* preview = new QWidget(card.first);
                                preview->setObjectName("AiCandidateCard");
                                auto* previewLayout = new QVBoxLayout(preview);
                                previewLayout->setContentsMargins(8, 6, 8, 6);
                                previewLayout->setSpacing(3);

                                const QString letter =
                                    QString(QChar('A' + std::min(choice, 25)));
                                const double total =
                                    candidate.value("score", json::object())
                                        .value("total", 0.0);
                                auto* title = new QLabel(
                                    tr("OPTION %1  ·  %2")
                                        .arg(letter,
                                             QString::number(total, 'f', 2)),
                                    preview);
                                title->setObjectName("AiCandidateTitle");
                                previewLayout->addWidget(title);

                                const json& score =
                                    candidate.value("score", json::object());
                                const auto value = [&score](const char* key) {
                                    return score.value(key, json::object())
                                        .value("value", 0.0);
                                };
                                auto* metrics = new QLabel(
                                    tr("Harmony %1  ·  Rhythm %2  ·  Voice %3  ·  %4 notes")
                                        .arg(QString::number(value("harmony"), 'f', 2),
                                             QString::number(value("rhythm"), 'f', 2),
                                             QString::number(value("voiceLeading"), 'f', 2),
                                             QString::number(candidate.value("noteCount", 0))),
                                    preview);
                                metrics->setObjectName("AiCandidateScore");
                                metrics->setWordWrap(true);
                                previewLayout->addWidget(metrics);

                                const QString candidateId =
                                    QString::fromStdString(candidate.value(
                                        "candidateId", std::string()));
                                auto* choose = new QPushButton(
                                    tr("CHOOSE %1").arg(letter), preview);
                                choose->setObjectName("AiCandidateButton");
                                choose->setCursor(Qt::PointingHandCursor);
                                choose->setEnabled(!m_session->running());
                                choose->setToolTip(
                                    tr("Apply this validated MIDI alternative to the selected track"));
                                connect(choose, &QAbstractButton::clicked, this,
                                        [this, candidateId] {
                                            m_input->setPlainText(
                                                tr("/compose Apply composition candidate %1 to the currently selected MIDI track.")
                                                    .arg(candidateId));
                                            m_input->setFocus();
                                            send();
                                        });
                                previewLayout->addWidget(choose, 0,
                                                         Qt::AlignLeft);
                                card.second->addWidget(preview);
                                ++choice;
                            }

                            auto* regenerate = new QPushButton(
                                tr("REGENERATE OPTIONS"), card.first);
                            regenerate->setObjectName("AiRevertButton");
                            regenerate->setCursor(Qt::PointingHandCursor);
                            regenerate->setEnabled(!m_session->running());
                            regenerate->setToolTip(
                                tr("Ask for fresh candidates with a different seed"));
                            connect(regenerate, &QAbstractButton::clicked, this,
                                    [this] {
                                        m_input->setPlainText(
                                            tr("/compose Regenerate the same composition with a different seed and show new alternatives."));
                                        m_input->setFocus();
                                        send();
                                    });
                            card.second->addWidget(regenerate, 0,
                                                   Qt::AlignLeft);
                        }
                    }
                    ++toolAt;
                }
                at = toolAt - 1;
                wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 9, 2);
                break;
            }
        }
        for (int item = beforeItems; item < m_transcriptLayout->count(); ++item)
            if (auto* row = m_transcriptLayout->itemAt(item)->widget())
                row->setProperty("aiMessageEnd", qulonglong(at + 1));
    }

    if (!m_session->lastError().empty()) {
        auto card = messageCard(m_transcriptBody, "AiErrorCard", tr("VLT AI · Error"),
                             "AiActionStatusError");
        card.second->addWidget(cardText(m_transcriptBody, 
            QString::fromStdString(m_session->lastError()), "AiMessageText"));
        auto* retry = messageAction(card.first, icons::Glyph::Reload, tr("Continue request"));
        retry->setObjectName("AiMessageAction");
        retry->setEnabled(!m_session->running() && m_client != nullptr);
        connect(retry, &QToolButton::clicked, this, &AiChatPanel::continueRequest);
        card.second->addWidget(retry, 0, Qt::AlignLeft);
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
    }

    if (!m_streaming.isEmpty()) {
        auto card = messageCard(m_transcriptBody, "AiLiveCard", tr("VLT AI · Writing"), "AiLiveRole");
        m_streamingLabel = cardText(m_transcriptBody, m_streaming, "AiMessageText");
        card.second->addWidget(m_streamingLabel);
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
    }

    if (m_session->running()) {
        auto card = messageCard(m_transcriptBody, "AiThinkingCard", tr("VLT AI"),
                             "AiAssistantRole");
        m_requestStatusLabel = cardText(m_transcriptBody,
            m_requestStatus.isEmpty() ? tr("Working…") : m_requestStatus, "AiMessageSecondary", true);
        card.second->addWidget(m_requestStatusLabel);
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 6, 4);
    }

    if (m_session->messages().empty()) {
        auto card = messageCard(m_transcriptBody, "AiAssistantCard", tr("VLT AI"),
                             "AiAssistantRole");
        card.second->addWidget(cardText(m_transcriptBody, 
            tr("Ask me to create, arrange, edit or explain anything in the open project."),
            "AiMessageSecondary", true));
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);


    }

    m_transcriptLayout->addStretch(1);
    QTimer::singleShot(0, m_transcript, [this, follow, scrollPosition] {
        if (!m_transcript) return;
        const QSignalBlocker block(m_transcript->verticalScrollBar());
        m_transcript->verticalScrollBar()->setValue(follow && m_followOutput
            ? m_transcript->verticalScrollBar()->maximum() : scrollPosition);
    });
}

// ── Headless checks ─────────────────────────────────────────────────────────

void AiChatPanel::editInstructions() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Instructions for this project"));
    dialog.resize(460, 300);

    auto* column = new QVBoxLayout(&dialog);
    auto* blurb = new QLabel(
        tr("Rules the assistant follows for this project, in your own words — "
           "\"always sidechain the bass\", \"keep it under 100 BPM\", \"no "
           "reverb on the drums\". They are saved with the project and "
           "override the assistant's own defaults."),
        &dialog);
    blurb->setWordWrap(true);
    column->addWidget(blurb);

    auto* editor = new QPlainTextEdit(
        QString::fromStdString(m_controller->aiInstructions()), &dialog);
    column->addWidget(editor, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    column->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;
    const auto result = m_controller->setAiInstructions(
        editor->toPlainText().trimmed().toStdString());
    // Part of the document, so the project is now unsaved.
    emit projectChanged(daw::collab::marksLocalFileDirty(result));
    if (result == daw::collab::SharedMutationResult::Blocked) return;
    emit statusMessage(m_controller->aiInstructions().empty()
                           ? tr("Instructions cleared")
                           : tr("Instructions saved with the project"));
}

void AiChatPanel::showPromptMenu() {
    QMenu menu(this);
    const QStringList saved = ui::aiprefs::savedPrompts();

    for (const QString& prompt : saved) {
        // Elided in the menu, whole in the tooltip: a saved prompt is often a
        // paragraph, and a menu item the width of the screen is useless.
        QAction* action = menu.addAction(
            QFontMetrics(menu.font()).elidedText(prompt, Qt::ElideRight, 320));
        action->setToolTip(prompt);
        connect(action, &QAction::triggered, this, [this, prompt] {
            m_input->setPlainText(prompt);
            m_input->setFocus();
        });
    }
    if (!saved.isEmpty()) menu.addSeparator();

    const QString current = m_input->toPlainText().trimmed();
    QAction* keep = menu.addAction(tr("Save what is typed"));
    keep->setEnabled(!current.isEmpty() && !saved.contains(current));
    connect(keep, &QAction::triggered, this, [this, saved, current] {
        QStringList next = saved;
        next.append(current);
        ui::aiprefs::setSavedPrompts(next);
        emit statusMessage(tr("Prompt saved"));
    });

    if (!saved.isEmpty()) {
        QMenu* forget = menu.addMenu(tr("Forget"));
        for (const QString& prompt : saved) {
            QAction* action = forget->addAction(
                QFontMetrics(menu.font()).elidedText(prompt, Qt::ElideRight, 320));
            connect(action, &QAction::triggered, this, [this, saved, prompt] {
                QStringList next = saved;
                next.removeAll(prompt);
                ui::aiprefs::setSavedPrompts(next);
            });
        }
    }
    menu.exec(m_promptsButton->mapToGlobal(
        QPoint(0, -menu.sizeHint().height())));
}

bool AiChatPanel::checkAgentForTest() {
    if (!ui::checkAiTransport()) return false;
    setMode(Mode::Assistant, /*persist=*/false);

    // A managed model remains selected while its short-lived account token is
    // being restored. That state is a chat with a recoverable authorization
    // error, not the first-run "connect a model" screen.
    m_client.reset(new ui::LlmClient(ui::LlmClient::Provider::OpenAi, this));
    ui::LlmConfig selected;
    selected.transport = ui::LlmConfig::Transport::Managed;
    selected.connectionId = QStringLiteral("managed-free-model");
    selected.displayName = QStringLiteral("Free model");
    m_client->setConfig(std::move(selected));
    updateReadiness();
    if (m_stack->currentIndex() != 0) return false;

    m_client.reset(new ScriptedClient(this));
    updateReadiness();

    // Reproduce first-open input through the physical input window. Sending
    // keys straight to m_input would miss the viewport/focus routing bug.
    renderTranscript();
    if (!findChildren<QWidget*>(QStringLiteral("AiSuggestionPanel")).isEmpty()) return false;
    window()->activateWindow();
    QWidget* inputSource = window();
    QWindow* inputWindow = window()->windowHandle();
    for (auto* host = parentWidget(); host; host = host->parentWidget()) {
        auto surfaces = host->findChildren<ui::graphics::WorkspaceSurface*>(QString(), Qt::FindDirectChildrenOnly);
        if (!surfaces.isEmpty()) {
            inputSource = host;
            inputWindow = surfaces.front()->quickWindow();
            break;
        }
    }
    if (inputWindow) inputWindow->requestActivate();
    QApplication::processEvents();
    m_transcript->setFocus();
    m_input->clear();
    const QPointF inputPoint(12, 12);
    const QPointF inputPosition = m_input->viewport()->mapTo(inputSource, inputPoint);
    const QPointF inputGlobal = m_input->viewport()->mapToGlobal(inputPoint);
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent click(type, inputPosition, inputGlobal, Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(inputWindow, &click);
    }
    QApplication::processEvents();
    if (QApplication::focusWidget() != m_input) {
        std::fprintf(stderr, "FAIL AI chat: first viewport click did not focus the editor\n");
        return false;
    }
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, QStringLiteral("h"));
    QApplication::sendEvent(inputWindow, &typed);
    if (m_input->toPlainText() != QStringLiteral("h")) {
        std::fprintf(stderr, "FAIL AI chat: first-open keyboard input was lost\n");
        return false;
    }
    std::fprintf(stderr, "PASS AI chat: first-open click and typing without suggestions\n");

    // Exercise the actual editor path, not just setPlainText(): this catches a
    // disabled composer or a key filter that steals normal text/newlines.
    if (!m_composer || m_composer->isHidden() ||
        m_composer->focusProxy() != m_input || !m_input->isEnabled() ||
        m_input->focusPolicy() != Qt::StrongFocus) {
        return false;
    }
    m_input->clear();
    QKeyEvent letter(QEvent::KeyPress, Qt::Key_V, Qt::NoModifier,
                     QStringLiteral("v"));
    QApplication::sendEvent(m_input, &letter);
    QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier,
                      QStringLiteral("\r"));
    QApplication::sendEvent(m_input, &newline);
    if (m_input->toPlainText() != QStringLiteral("v\n")) return false;
    m_input->clear();

    const std::size_t mark = m_controller->undoDepth();
    const std::size_t tracksBefore = m_controller->project().tracks.size();

    m_input->setPlainText(QStringLiteral("make a piano part"));
    send();
    if (!m_input->isEnabled()) return false;

    // The scripted client answers through the event loop, so the run needs the
    // loop pumped rather than a wait.
    for (int i = 0; i < 400 && m_session->running(); ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    if (m_session->running()) return false;

    const bool wasHidden = isHidden();
    show();
    QApplication::processEvents();
    QTemporaryDir folders;
    if (!folders.isValid() || !QDir(folders.path()).mkdir("Samples")) return false;
    const int attachedBefore = m_attachments->count();
    addAttachment(folders.path());
    addAttachment(folders.path());
    addAttachment(folders.path() + "/Samples");
    applyModeToComposer();
    if (m_attachments->count() != attachedBefore + 2 || !m_attachments->isHidden()) {
        std::fprintf(stderr, "FAIL AI chat: folder deduplication or collapsed list\n"); return false;
    }
    const auto remainingId = m_attachments->item(attachedBefore + 1)->data(Qt::UserRole + 1);
    m_attachmentButton->setChecked(true);
    m_attachments->setFocus();
    m_attachments->clearSelection();
    m_attachments->item(attachedBefore)->setSelected(true);
    QKeyEvent claimRemove(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(m_attachments, &claimRemove);
    if (!claimRemove.isAccepted()) return false;
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(m_attachments, &remove);
    if (m_attachments->count() != attachedBefore + 1 ||
        m_attachments->item(attachedBefore)->data(Qt::UserRole + 1) != remainingId) {
        std::fprintf(stderr, "FAIL AI chat: attachment Delete or stable id\n"); return false;
    }
    delete m_attachments->takeItem(attachedBefore);
    m_attachmentButton->setChecked(false);
    refreshAttachments();
    const auto actions = m_transcriptBody->findChildren<QToolButton*>("AiMessageAction");
    if (actions.isEmpty()) { std::fprintf(stderr, "FAIL AI chat: missing copy action\n"); return false; }
    auto* clipboard = QApplication::clipboard();
    auto* previous = new QMimeData;
    if (const auto* mime = clipboard->mimeData())
        for (const auto& format : mime->formats()) previous->setData(format, mime->data(format));
    actions.front()->click();
    const bool copied = clipboard->text() == QString::fromStdString(m_session->messages().front().text);
    clipboard->setMimeData(previous);
    if (!copied) { std::fprintf(stderr, "FAIL AI chat: copy text\n"); return false; }
    bool foundConversation = false;
    QTimer::singleShot(0, this, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QDialog*>(widget);
            if (dialog && dialog->windowTitle() == tr("Conversation")) {
                foundConversation = true;
                dialog->reject();
            }
        }
    });
    m_input->setFocus();
    const auto findChord = QKeySequence(QKeySequence::Find)[0];
    QKeyEvent findOverride(QEvent::ShortcutOverride, findChord.key(), findChord.keyboardModifiers());
    QApplication::sendEvent(m_input, &findOverride);
    QKeyEvent findPress(QEvent::KeyPress, findChord.key(), findChord.keyboardModifiers());
    QApplication::sendEvent(m_input, &findPress);
    QApplication::processEvents();
    if (!foundConversation) { std::fprintf(stderr, "FAIL AI chat: Find shortcut\n"); return false; }
    // Reading earlier messages must not be interrupted by a streamed update.
    m_session->begin("/help Explain the arrangement");
    m_session->applyReply({std::string(2500, 'a'), {}, {}});
    // Explicit newlines avoid depending on font-dependent long-word wrapping.
    m_session->begin("/help Explain each bar");
    std::string longAnswer;
    for (int i = 0; i < 100; ++i) longAnswer += "A bar of music.\n";
    m_session->applyReply({longAnswer, {}, {}});
    const auto settleLayout = [] {
        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
    };
    m_followOutput = true;
    renderTranscript();
    settleLayout();
    auto* scroll = m_transcript->verticalScrollBar();
    const bool followed = scroll->value() == scroll->maximum();
    scroll->setValue(0);
    m_followOutput = false;
    m_streaming = "A new fragment";
    renderTranscript();
    settleLayout();
    const bool anchored = followed && scroll->maximum() > 0 && scroll->value() == 0 &&
                          m_transcriptBody->width() <= m_transcript->viewport()->width();
    m_streaming.clear();
    setVisible(!wasHidden);
    if (!anchored) {
        std::fprintf(stderr, "FAIL AI chat: scroll/width maximum=%d value=%d body=%dx%d viewport=%dx%d messages=%zu visible=%d\n",
                     scroll->maximum(), scroll->value(), m_transcriptBody->width(), m_transcriptBody->height(),
                     m_transcript->viewport()->width(), m_transcript->viewport()->height(),
                     m_session->messages().size(), int(m_transcript->isVisible()));
        for (const auto* label : m_transcriptBody->findChildren<QLabel*>("AiMessageText"))
            std::fprintf(stderr, "  text=%d height=%d hint=%d minimum=%d hfw=%d\n", int(label->text().size()),
                         label->height(), label->sizeHint().height(), label->minimumSizeHint().height(),
                         label->heightForWidth(label->width()));
        return false;
    }
    std::fprintf(stderr, "PASS AI chat: compact folders, deduplication, Delete, stable ids, message copy and Find\n");
    std::fprintf(stderr, "PASS AI chat: streaming preserves the reader's scroll position\n");

    const daw::TrackModel* made = nullptr;
    for (const daw::TrackModel& track : m_controller->project().tracks)
        if (track.name == "AI Piano") made = &track;
    if (!made || made->clips.size() != 1 || made->clips[0].notes.size() != 3)
        return false;

    // The whole run has to be one entry, and it has to take all of it back.
    if (m_controller->undoDepth() !=
        std::min(mark + 1, m_controller->undoLimit())) return false;
    m_controller->undo();
    if (m_controller->project().tracks.size() != tracksBefore) return false;
    m_controller->redo();
    return m_controller->project().tracks.size() == tracksBefore + 1;
}

bool AiChatPanel::checkMusicForTest() {
    setMode(Mode::Music, /*persist=*/false);
    // Installed *after* the mode switch: `setMode` reloads the settings, and a
    // real config would put the file somewhere the check does not own.
    m_musicClient.reset(new ScriptedMusicClient(this));
    ui::MusicConfig config;
    config.folder = QDir::tempPath() + QStringLiteral("/daw-selftest-music");
    config.format = QStringLiteral("wav");
    m_musicClient->setConfig(std::move(config));

    const std::size_t mark = m_controller->undoDepth();
    const std::size_t tracksBefore = m_controller->project().tracks.size();

    m_input->setPlainText(QStringLiteral("warm lo-fi beat"));
    send();
    for (int i = 0; i < 400 && musicPending(); ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    if (musicPending()) return false;
    if (m_musicTurns.empty() || !m_musicTurns.back().error.isEmpty()) return false;
    if (!QFileInfo::exists(m_musicTurns.back().filePath)) return false;

    // The audio has to have reached a real track as a real clip.
    const daw::TrackModel* made = nullptr;
    for (const daw::TrackModel& track : m_controller->project().tracks)
        if (track.name == "warm-lo-fi-beat") made = &track;
    if (!made || made->clips.size() != 1) return false;
    if (made->kind != daw::TrackKind::Audio) return false;

    // Track and clip arrived together, so they must go together.
    if (m_controller->undoDepth() !=
        std::min(mark + 1, m_controller->undoLimit())) return false;
    m_controller->undo();
    if (m_controller->project().tracks.size() != tracksBefore) return false;
    m_controller->redo();
    return m_controller->project().tracks.size() == tracksBefore + 1;
}

void AiChatPanel::showDemoMusicTranscriptForTest() {
    setMode(Mode::Music, /*persist=*/false);
    m_musicClient.reset(new ScriptedMusicClient(this));
    ui::MusicConfig config;
    config.folder = QDir::tempPath() + QStringLiteral("/daw-shot-music");
    config.format = QStringLiteral("wav");
    m_musicClient->setConfig(std::move(config));

    m_input->setPlainText(
        QStringLiteral("warm lo-fi beat, dusty piano, brushed drums"));
    send();
    for (int i = 0; i < 400 && musicPending(); ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
}

void AiChatPanel::showDemoTranscriptForTest() {
    setMode(Mode::Assistant, /*persist=*/false);
    m_client.reset(new ScriptedClient(this));
    updateReadiness();
    m_input->setPlainText(
        QStringLiteral("make a piano, write the chords, process the channel"));
    send();
    for (int i = 0; i < 400 && m_session->running(); ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    renderTranscript();
}
