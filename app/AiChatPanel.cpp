#include "AiChatPanel.hpp"

#include "AiPrefs.hpp"
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
#include "ai/AiSession.hpp"
#include "ai/AiTools.hpp"
#include "ai/ContentCatalog.hpp"
#include "ai/CompositionEngine.hpp"
#include "ai/MusicGen.hpp"
#include "platform/AudioFileDecoder.hpp"

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
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QFontMetrics>
#include <QConicalGradient>
#include <QMenu>
#include <QToolButton>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
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

// Three rows: the title strip, the model line, and the mode switch.
constexpr int kHeaderHeight = 88;
constexpr int kAttachmentsMaxHeight = 70;

// ── Transcript furniture, shared by both modes ───────────────────────────────
//
// Free functions rather than lambdas inside one renderer: the music mode draws
// the same cards, and two transcripts that drifted apart in spacing and
// silhouette would read as two different programs.

QLabel* cardText(QWidget* parent, const QString& text, const char* objectName,
                 bool secondary = false) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
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
    setMinimumWidth(240);
    setAcceptDrops(true);
    setCornerRadius(22);
    setShadowMargin(6);
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
    // Keep every child inside the painted glass plate. The six-pixel outer
    // gutter belongs to the soft shadow and animated rim, not to the content.
    column->setContentsMargins(10, 8, 10, 10);
    column->setSpacing(0);
    column->addWidget(buildHeader());

    m_stack = new QStackedWidget(this);
    column->addWidget(m_stack, 1);

    auto* chat = new QWidget(m_stack);
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
    m_transcriptLayout->setContentsMargins(8, 10, 8, 12);
    m_transcriptLayout->setSpacing(8);
    m_transcript->setWidget(m_transcriptBody);
    chatColumn->addWidget(m_transcript, 1);

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
        connect(account, &account::Service::snapshotChanged, this,
                &AiChatPanel::updateUsageLabel);
    }

    // A very slow spectral pass around the rim is the panel's one deliberate
    // piece of motion. It keeps the assistant feeling alive without animating
    // content or competing with the playhead. Reduced-transparency mode is
    // also the quiet-motion fallback.
    m_edgeAnimation = new QVariantAnimation(this);
    m_edgeAnimation->setDuration(12000);
    m_edgeAnimation->setLoopCount(-1);
    m_edgeAnimation->setStartValue(0.0);
    m_edgeAnimation->setEndValue(1.0);
    connect(m_edgeAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_edgePhase = value.toDouble();
                // Fifteen visual frames a second is ample for a twelve-second
                // light pass and keeps the full-height surface cheap in a DAW.
                if ((++m_edgeFrame % 4) == 0) update();
            });
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
    column->setContentsMargins(8, 6, 4, 5);
    column->setSpacing(2);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(5);

    auto* mark = new QLabel(QStringLiteral("AI"), header);
    mark->setObjectName("AiMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(27, 27);
    row->addWidget(mark);

    m_titleLabel = new QLabel(tr("AI CHAT"), header);
    m_titleLabel->setObjectName("AiTitle");
    row->addWidget(m_titleLabel);
    row->addStretch(1);

    auto* clear = new ui::IconButton(icons::Glyph::Trash,
                                     tr("Start a new conversation"), header);
    clear->setButtonSize(22, 20);
    connect(clear, &QAbstractButton::clicked, this, [this] {
        m_session->clear();
        renderTranscript();
        updateUsageLabel();
    });
    row->addWidget(clear);

    auto* rules = new ui::IconButton(
        icons::Glyph::NoteStyle,
        tr("Standing instructions for this project"), header);
    rules->setButtonSize(22, 20);
    connect(rules, &QAbstractButton::clicked, this,
            &AiChatPanel::editInstructions);
    row->addWidget(rules);

    auto* gear = new ui::IconButton(icons::Glyph::Gear,
                                    tr("Assistant settings"), header);
    gear->setButtonSize(22, 20);
    connect(gear, &QAbstractButton::clicked, this,
            &AiChatPanel::settingsRequested);
    row->addWidget(gear);
    column->addLayout(row);

    auto* meta = new QHBoxLayout;
    meta->setContentsMargins(32, 0, 4, 0);
    meta->setSpacing(6);
    // The model is a *choice*, not a caption: one key and one URL cover every
    // model a provider offers, and which one suits the request is decided here
    // rather than in a settings window.
    m_modelLabel = new QToolButton(header);
    m_modelLabel->setObjectName("AiModel");
    m_modelLabel->setPopupMode(QToolButton::InstantPopup);
    m_modelLabel->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_modelLabel->setCursor(Qt::PointingHandCursor);
    m_modelLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* modelMenu = new QMenu(m_modelLabel);
    connect(modelMenu, &QMenu::aboutToShow, this,
            [this, modelMenu] { populateModelMenu(modelMenu); });
    m_modelLabel->setMenu(modelMenu);
    meta->addWidget(m_modelLabel);
    meta->addStretch(1);

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
    auto* column = new QVBoxLayout(composer);
    column->setContentsMargins(10, 8, 10, 9);
    column->setSpacing(6);

    m_attachHint = new QLabel(tr("Drop samples here to let the assistant use them"),
                              composer);
    m_attachHint->setObjectName("AiHint");
    m_attachHint->setWordWrap(true);
    column->addWidget(m_attachHint);

    m_attachments = new QListWidget(composer);
    m_attachments->setObjectName("AiAttachments");
    m_attachments->setFrameShape(QFrame::NoFrame);
    m_attachments->setMaximumHeight(kAttachmentsMaxHeight);
    m_attachments->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_attachments->setToolTip(
        tr("Files the assistant may load. Select and press Backspace to remove."));
    m_attachments->hide();
    column->addWidget(m_attachments);

    m_input = new QPlainTextEdit(composer);
    m_input->setObjectName("AiInput");
    m_input->setPlaceholderText(
        tr("Make a piano part, write the chords, mix the channel…"));
    m_input->setFixedHeight(82);
    // Nothing may hold the keyboard until it is clicked, or the transport keys
    // stop working — the same rule the tempo field follows.
    m_input->setFocusPolicy(Qt::ClickFocus);
    m_input->installEventFilter(this);
    column->addWidget(m_input);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(4);

    auto* attach = new ui::IconButton(icons::Glyph::Plus,
                                      tr("Attach a sample"), composer);
    attach->setButtonSize(28, 28);
    connect(attach, &QAbstractButton::clicked, this, [this] {
        emit statusMessage(tr("Drag a sample from the browser into the assistant"));
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
    return composer;
}

QWidget* AiChatPanel::buildEmptyState() {
    auto* page = new QWidget(m_stack);
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(16, 24, 16, 16);
    column->setSpacing(10);
    column->addStretch(2);

    auto* mark = new QLabel(QStringLiteral("AI"), page);
    mark->setObjectName("AiEmptyMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(58, 58);
    column->addWidget(mark, 0, Qt::AlignCenter);

    auto* kicker = new QLabel(tr("STUDIO INTELLIGENCE"), page);
    kicker->setObjectName("AiEmptyKicker");
    kicker->setAlignment(Qt::AlignCenter);
    column->addWidget(kicker);

    auto* headline = new QLabel(tr("Choose an AI model"), page);
    headline->setObjectName("AiEmptyTitle");
    headline->setAlignment(Qt::AlignCenter);
    column->addWidget(headline);

    auto* blurb = new QLabel(
        tr("Use a model provided with VLT Studio, or add your own compatible "
           "endpoint in AI Settings."),
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
    auto* capabilities = new QLabel(tr("CREATE  ·  ARRANGE  ·  MIX"), page);
    capabilities->setObjectName("AiEmptyKicker");
    capabilities->setAlignment(Qt::AlignCenter);
    column->addWidget(capabilities);
    column->addStretch(3);
    return page;
}

QWidget* AiChatPanel::buildMusicPage() {
    auto* page = new QWidget(m_stack);
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
    if (ui::GlassPanel::reduceTransparency()) m_edgePhase = 0.0;
    syncEdgeAnimation();

    auto css = [](QColor color) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red()).arg(color.green()).arg(color.blue())
            .arg(QString::number(color.alphaF(), 'f', 3));
    };
    QColor headerLine = mixColors(t.separator(), t.accent, 0.32);
    headerLine.setAlphaF(t.dark ? 0.62 : 0.48);
    QColor markFill = t.accent;
    markFill.setAlphaF(t.dark ? 0.18 : 0.12);
    QColor modelFill = t.surfaceElevated;
    modelFill.setAlphaF(t.dark ? 0.48 : 0.64);
    QColor composerFill = t.surfaceElevated;
    composerFill.setAlphaF(t.dark ? 0.58 : 0.76);
    QColor inputFill = t.well();
    inputFill.setAlphaF(t.dark ? 0.72 : 0.80);
    QColor inputBorder = mixColors(t.separator(), t.accent, 0.20);
    inputBorder.setAlphaF(0.78);
    QColor userTop = mixColors(t.surfaceElevated, t.accent, 0.25);
    userTop.setAlphaF(t.dark ? 0.90 : 0.82);
    QColor userBottom = mixColors(t.surface, t.accent, 0.14);
    userBottom.setAlphaF(t.dark ? 0.86 : 0.78);
    QColor userBorder = mixColors(t.separator(), t.accentHighlight, 0.58);
    userBorder.setAlphaF(0.90);
    QColor assistantTop = t.surfaceElevated;
    assistantTop.setAlphaF(t.dark ? 0.72 : 0.84);
    QColor assistantBottom = mixColors(t.surfaceElevated, t.background, 0.22);
    assistantBottom.setAlphaF(t.dark ? 0.66 : 0.80);
    QColor assistantBorder = mixColors(t.separator(), t.accent, 0.26);
    assistantBorder.setAlphaF(0.78);
    QColor actionTop = mixColors(t.well(), t.accent, 0.08);
    actionTop.setAlphaF(t.dark ? 0.90 : 0.82);
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
#AiHeader { background: transparent; border-bottom: 1px solid %HEADER_LINE%; }
#AiMark { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
          border-radius: 9px; color: %TEXT1%; font-size: 10px;
          font-weight: 800; letter-spacing: 0.8px; }
#AiTitle { color: %TEXT1%; font-size: 12px; font-weight: 750;
           letter-spacing: 1.1px; }
/* Room on the right for the menu caret QToolButton draws itself, or it lands
   on the last letter of the model's name. */
#AiModel { background: %MODEL_FILL%; border: 1px solid %SEP%;
           border-radius: 6px; color: %TEXT2%; font-size: 9px;
           padding: 1px 16px 1px 7px; }
#AiModel:hover { border-color: %ACCENT_SOFT%; color: %TEXT1%; }
#AiModel::menu-indicator { subcontrol-position: right center;
                           subcontrol-origin: padding; right: 4px; }
#AiUsage { color: %TEXT2%; font-size: 9px; }
#AiIndexStatus { color: %TEXT2%; font-size: 9px; font-weight: 650; }
#AiHint { color: %TEXT2%; font-size: 10px; }
#AiEmptyMark { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
               border-radius: 29px; color: %TEXT1%; font-size: 15px;
               font-weight: 800; letter-spacing: 1px; }
#AiEmptyKicker { color: %ACCENT_SOFT%; font-size: 9px; font-weight: 700;
                 letter-spacing: 1.3px; }
#AiEmptyTitle { color: %TEXT1%; font-size: 14px; font-weight: 650; }
#AiOpenSettings { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
                  border-radius: 9px; color: %TEXT1%; padding: 6px 14px; }
#AiOpenSettings:hover { background: %MODEL_FILL%; }
#AiComposer { background: %COMPOSER_FILL%; border: 1px solid %HEADER_LINE%;
              border-radius: 14px; }
#AiTranscript, #AiTranscriptBody { background: transparent; border: none; }
#AiUserCard {
    background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                                stop:0 %USER_TOP%, stop:1 %USER_BOTTOM%);
    border: 1px solid %USER_BORDER%; border-radius: 15px;
}
#AiAssistantCard, #AiLiveCard {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
                                stop:0 %ASSISTANT_TOP%, stop:1 %ASSISTANT_BOTTOM%);
    border: 1px solid %ASSISTANT_BORDER%; border-radius: 15px;
}
#AiActionCard {
    background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                                stop:0 %ACTION_TOP%, stop:1 %ACTION_BOTTOM%);
    border: 1px solid %ACTION_BORDER%; border-radius: 11px;
}
#AiErrorCard { background: %ERROR_FILL%; border: 1px solid %ERROR%;
               border-radius: 12px; }
#AiThinkingCard { background: %MARK_FILL%; border: 1px solid %USER_BORDER%;
                  border-radius: 12px; }
#AiMessageText { color: %TEXT1%; font-size: 11px; }
#AiMessageSecondary { color: %TEXT2%; font-size: 10px; }
#AiUserRole, #AiAssistantRole, #AiActionRole, #AiLiveRole {
    font-size: 8px; font-weight: 750; letter-spacing: 1.1px;
}
#AiUserRole { color: %TEXT2%; }
#AiAssistantRole, #AiLiveRole { color: %ACCENT_SOFT%; }
#AiActionRole { color: %TEXT2%; }
#AiActionStatusOk, #AiActionStatusWarn, #AiActionStatusError {
    border-radius: 5px; padding: 2px 5px; font-family: "%MONO%";
    font-size: 8px; font-weight: 700;
}
#AiActionStatusOk { background: %STATUS_FILL%; color: %ACCENT_SOFT%; }
#AiActionStatusWarn { background: %MARK_FILL%; color: %TEXT1%; }
#AiActionStatusError { background: %ERROR_FILL%; color: %ERROR%; }
#AiActionText { color: %TEXT2%; font-family: "%MONO%"; font-size: 9px; }
#AiCandidateCard { background: %ASSISTANT_TOP%; border: 1px solid %ACTION_BORDER%;
                   border-radius: 9px; }
#AiCandidateTitle { color: %TEXT1%; font-size: 9px; font-weight: 750;
                    letter-spacing: 0.7px; }
#AiCandidateScore { color: %TEXT2%; font-size: 9px; }
#AiCandidateButton { background: %MARK_FILL%; border: 1px solid %ACCENT_SOFT%;
                     border-radius: 6px; color: %TEXT1%; font-size: 8px;
                     font-weight: 700; padding: 3px 7px; }
#AiCandidateButton:hover { background: %MODEL_FILL%; }
#AiRevertButton { background: transparent; border: none; color: %TEXT2%;
                  font-size: 8px; font-weight: 700; letter-spacing: 0.7px;
                  padding: 2px 0; text-align: left; }
#AiRevertButton:hover { color: %ACCENT_SOFT%; }
#AiInput, #AiAttachments { background: %INPUT_FILL%; border: 1px solid %INPUT_BORDER%;
                           border-radius: 10px; color: %TEXT1%;
                           font-size: 11px; padding: 7px; }
#AiInput:focus { border: 1px solid %ACCENT_SOFT%; }
#AiAttachments::item:selected { background: %SELECT%; color: %TEXT1%; }
)")
                      .replace("%MONO%", fixedFamily)
                      .replace("%HEADER_LINE%", css(headerLine))
                      .replace("%MARK_FILL%", css(markFill))
                      .replace("%MODEL_FILL%", css(modelFill))
                      .replace("%COMPOSER_FILL%", css(composerFill))
                      .replace("%INPUT_FILL%", css(inputFill))
                      .replace("%INPUT_BORDER%", css(inputBorder))
                      .replace("%USER_TOP%", css(userTop))
                      .replace("%USER_BOTTOM%", css(userBottom))
                      .replace("%USER_BORDER%", css(userBorder))
                      .replace("%ASSISTANT_TOP%", css(assistantTop))
                      .replace("%ASSISTANT_BOTTOM%", css(assistantBottom))
                      .replace("%ASSISTANT_BORDER%", css(assistantBorder))
                      .replace("%ACTION_TOP%", css(actionTop))
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
    renderTranscript();
}

void AiChatPanel::onReduceTransparencyChanged() {
    ui::GlassPanel::onReduceTransparencyChanged();
    // The preference also governs the slow glass rim animation, so update its
    // running state at the same time as the material itself.
    applyTheme();
}

void AiChatPanel::syncEdgeAnimation() {
    if (!m_edgeAnimation) return;
    const bool shouldRun = isVisible() &&
                           !ui::GlassPanel::reduceTransparency();
    if (shouldRun) {
        if (m_edgeAnimation->state() != QAbstractAnimation::Running)
            m_edgeAnimation->start();
    } else if (m_edgeAnimation->state() != QAbstractAnimation::Stopped) {
        m_edgeAnimation->stop();
    }
}

void AiChatPanel::showEvent(QShowEvent* event) {
    ui::GlassPanel::showEvent(event);
    syncEdgeAnimation();
    startContentIndex(/*force=*/false);
    // A generation may have continued while the panel was hidden. Refresh
    // its one live field immediately without recreating the transcript.
    updateMusicElapsedLabel();
}

void AiChatPanel::startContentIndex(bool force) {
    if (!m_contentCatalog) return;
    std::vector<std::string> roots;
    for (const QString& folder : ui::browserprefs::folders())
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
    // Do not rely on the visibility flag's exact update order around a Qt hide
    // event: this path must unconditionally silence the perpetual animation.
    if (m_edgeAnimation &&
        m_edgeAnimation->state() != QAbstractAnimation::Stopped)
        m_edgeAnimation->stop();
    ui::GlassPanel::hideEvent(event);
}

QRect AiChatPanel::plateRect() const {
    // A docked pane still needs room for its luminous left edge, but it should
    // visually belong to the window on the other three sides. The small right
    // inset preserves the glow without leaving the old floating-card gutter.
    return rect().adjusted(7, 3, -2, -3);
}

QPainterPath AiChatPanel::plateShape() const {
    const QRectF r = QRectF(plateRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (r.isEmpty()) return {};

    // The timeline-facing corners are generous and welcoming; the window-edge
    // corners are tighter, which is what makes the surface read as docked
    // rather than as a card that happens to be cropped by the application.
    const qreal leftRadius = std::min<qreal>(20.0, r.height() / 2.0);
    const qreal rightRadius = std::min<qreal>(7.0, r.height() / 2.0);
    QPainterPath path;
    path.moveTo(r.left() + leftRadius, r.top());
    path.lineTo(r.right() - rightRadius, r.top());
    path.quadTo(r.right(), r.top(), r.right(), r.top() + rightRadius);
    path.lineTo(r.right(), r.bottom() - rightRadius);
    path.quadTo(r.right(), r.bottom(), r.right() - rightRadius, r.bottom());
    path.lineTo(r.left() + leftRadius, r.bottom());
    path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - leftRadius);
    path.lineTo(r.left(), r.top() + leftRadius);
    path.quadTo(r.left(), r.top(), r.left() + leftRadius, r.top());
    path.closeSubpath();
    return path;
}

void AiChatPanel::paintEvent(QPaintEvent* event) {
    ui::GlassPanel::paintEvent(event);
    if (ui::GlassPanel::reduceTransparency()) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    const QRectF plate = QRectF(plateRect()).adjusted(1.25, 1.25, -1.25, -1.25);
    const QPainterPath shape = plateShape();

    // Spectral light travels around the edge rather than washing the whole
    // panel in one brand colour. The glass stays legible; only its thickness
    // catches cyan, violet and the active theme accent.
    QColor cyan = mixColors(t.accentHighlight, QColor(80, 224, 255), 0.62);
    QColor violet = mixColors(t.accent, QColor(176, 104, 255), 0.58);
    QColor clear = t.textPrimary;
    cyan.setAlphaF(t.dark ? 0.82 : 0.52);
    violet.setAlphaF(t.dark ? 0.72 : 0.44);
    clear.setAlphaF(t.dark ? 0.44 : 0.58);

    QConicalGradient spectrum(plate.center(), 360.0 * m_edgePhase);
    spectrum.setColorAt(0.00, cyan);
    spectrum.setColorAt(0.22, clear);
    spectrum.setColorAt(0.45, violet);
    spectrum.setColorAt(0.70, QColor(cyan.red(), cyan.green(), cyan.blue(), 46));
    spectrum.setColorAt(1.00, cyan);
    painter.setBrush(Qt::NoBrush);

    // Wide low-opacity passes are the halo; the final narrow pass is the
    // actual glass rim. All three share one rotating spectrum, so the light
    // travels continuously around the whole panel rather than pulsing corners.
    painter.setOpacity(t.dark ? 0.11 : 0.08);
    painter.setPen(QPen(QBrush(spectrum), 9.0, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawPath(shape);
    painter.setOpacity(t.dark ? 0.24 : 0.17);
    painter.setPen(QPen(QBrush(spectrum), 4.0, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawPath(shape);
    painter.setOpacity(1.0);
    painter.setPen(QPen(QBrush(spectrum), 1.45, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawPath(shape);

    // Two diffused reflections make the pane feel lit from outside the app,
    // not filled with a flat gradient. They remain inside the clipping path.
    painter.save();
    painter.setClipPath(shape);
    QRadialGradient upper(plate.topRight() + QPointF(-18, 18), plate.width() * 0.70);
    QColor glow = cyan;
    glow.setAlphaF(0.13);
    upper.setColorAt(0.0, glow);
    glow.setAlphaF(0.0);
    upper.setColorAt(1.0, glow);
    painter.fillRect(plate, upper);

    QRadialGradient lower(plate.bottomLeft() + QPointF(24, -24),
                          plate.width() * 0.82);
    glow = violet;
    glow.setAlphaF(0.10);
    lower.setColorAt(0.0, glow);
    glow.setAlphaF(0.0);
    lower.setColorAt(1.0, glow);
    painter.fillRect(plate, lower);
    painter.restore();
}

// ── Settings ────────────────────────────────────────────────────────────────

void AiChatPanel::reloadSettings() {
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

bool AiChatPanel::hasKey() const {
    if (!m_client) return false;
    if (m_client->config().transport == ui::LlmConfig::Transport::Direct)
        return !m_client->config().endpoint.isEmpty();
    if (auto* account = account::Service::instance())
        return account->authenticated() && !account->accessToken().isEmpty() &&
               !m_client->config().connectionId.isEmpty();
    return false;
}

void AiChatPanel::updateReadiness() {
    if (!m_stack) return;
    // Music needs no chat key — often no key at all, when the model runs on the
    // user's own server — so the "connect a model" page is the assistant's
    // alone.
    if (m_mode == Mode::Music) {
        m_stack->setCurrentIndex(2);
        if (m_composer) m_composer->show();
        return;
    }
    // The scripted stand-in has no key by design, so a headless run must not be
    // pushed onto the "connect a model" page.
    const bool scripted = dynamic_cast<ScriptedClient*>(m_client.get()) != nullptr;
    const bool ready = hasKey() || scripted;
    m_stack->setCurrentIndex(ready ? 0 : 1);
    // Nothing to type into until there is somewhere to send it.
    if (m_composer) m_composer->setVisible(ready);
}

// ── Attachments ─────────────────────────────────────────────────────────────

void AiChatPanel::addAttachment(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return;
    const QString absolute = info.absoluteFilePath();
    for (int i = 0; i < m_attachments->count(); ++i)
        if (m_attachments->item(i)->data(Qt::UserRole).toString() == absolute)
            return;

    audio::platform::AudioFileInfo probed;
    const bool decodable =
        audio::platform::probeAudioFile(absolute.toStdString(), probed).isOk();

    auto* item = new QListWidgetItem(m_attachments);
    item->setData(Qt::UserRole, absolute);
    item->setText(decodable ? QStringLiteral("%1  ·  %2 s")
                                  .arg(info.fileName())
                                  .arg(probed.durationSeconds(), 0, 'f', 1)
                            : info.fileName());
    item->setToolTip(absolute);
    item->setIcon(icons::icon(decodable ? icons::Glyph::Waveform
                                        : icons::Glyph::Import,
                              th().textSecondary, 12));
    refreshAttachments();
    emit statusMessage(tr("Attached %1").arg(info.fileName()));
}

void AiChatPanel::refreshAttachments() {
    const bool any = m_attachments->count() > 0;
    m_attachments->setVisible(any);
    m_attachHint->setVisible(!any);
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
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool enter =
            key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (enter && !(key->modifiers() & Qt::ShiftModifier)) {
            send();
            return true;
        }
    }
    if (watched == m_attachments && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete) {
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
    if (m_session->running() || !m_client) return;
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;

    if (auto* account = account::Service::instance(); account &&
        m_client->config().accessToken != account->accessToken()) {
        ui::LlmConfig config = m_client->config();
        config.accessToken = account->accessToken();
        m_client->setConfig(std::move(config));
    }

    ai::ToolContext context;
    for (int i = 0; i < m_attachments->count(); ++i) {
        const QString path = m_attachments->item(i)->data(Qt::UserRole).toString();
        audio::platform::AudioFileInfo probed;
        audio::platform::probeAudioFile(path.toStdString(), probed);
        context.attachments.push_back(
            ai::Attachment{QFileInfo(path).fileName().toStdString(),
                           path.toStdString(), probed.durationSeconds(),
                           int(probed.sampleRate), int(probed.channels),
                           "attachment_" + std::to_string(i + 1)});
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
    for (const QString& folder : ui::browserprefs::folders())
        context.sampleFolders.push_back(folder.toStdString());
    startContentIndex(/*force=*/true);
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
    renderTranscript();
    updateBusyState();
    step();
}

void AiChatPanel::step() {
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
        if (m_streamingLabel) {
            m_streamingLabel->setText(m_streaming);
            QTimer::singleShot(0, m_transcript, [this] {
                if (m_transcript)
                    m_transcript->verticalScrollBar()->setValue(
                        m_transcript->verticalScrollBar()->maximum());
            });
        } else {
            renderTranscript();
        }
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
    const ai::AiSession::Step next = m_session->applyReply(reply);
    renderTranscript();
    // The document may have changed under the shell's views, whether or not the
    // run is over — the tracks should appear as they are made, not at the end.
    emit projectChanged();

    if (next == ai::AiSession::Step::NeedsRequest) {
        step();
        return;
    }
    endRun();
}

void AiChatPanel::endRun() {
    updateBusyState();
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
    m_session->applyReply(ai::ModelReply{});
    renderTranscript();
    updateBusyState();
    emit projectChanged();
}

void AiChatPanel::updateBusyState() {
    const bool running = m_mode == Mode::Music ? musicPending()
                                               : m_session->running();
    m_sendButton->setVisible(!running);
    m_stopButton->setVisible(running);
    m_input->setEnabled(!running);
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
    if (m_titleLabel) m_titleLabel->setText(music ? tr("AI MUSIC") : tr("AI CHAT"));
    if (m_instrumentalButton) m_instrumentalButton->setVisible(music);
    if (m_attachHint) m_attachHint->setVisible(!music && m_attachments &&
                                               m_attachments->count() == 0);
    if (m_attachments) m_attachments->setVisible(!music &&
                                                 m_attachments->count() > 0);
    if (m_promptsButton) m_promptsButton->setVisible(!music);
    if (!m_input) return;
    m_input->setPlaceholderText(
        music ? tr("Warm lo-fi beat, dusty piano, brushed drums…")
              : tr("Make a piano part, write the chords, mix the channel…"));
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
    while (QLayoutItem* item = m_transcriptLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    // Which user turns can still be taken back, so the link is only offered
    // where it would actually work.
    QSet<qulonglong> revertable;
    for (const ai::Checkpoint& point : m_session->checkpoints())
        revertable.insert(qulonglong(point.messageIndex));

    const std::vector<ai::Message>& messages = m_session->messages();
    for (std::size_t at = 0; at < messages.size(); ++at) {
        const ai::Message& message = messages[at];
        switch (message.role) {
            case ai::Role::User: {
                const QString mode = QString::fromLatin1(
                    ai::interactionModeName(
                        ai::inferInteractionMode(message.text)));
                auto card = messageCard(
                    m_transcriptBody, "AiUserCard",
                    tr("YOU / %1").arg(mode), "AiUserRole");
                card.second->addWidget(cardText(m_transcriptBody, 
                    QString::fromStdString(message.text), "AiMessageText"));
                if (revertable.contains(qulonglong(at)) && !m_session->running()) {
                    auto* revert = new QPushButton(tr("REVERT REQUEST"), card.first);
                    revert->setObjectName("AiRevertButton");
                    revert->setCursor(Qt::PointingHandCursor);
                    revert->setToolTip(tr("Restore the project to before this request"));
                    connect(revert, &QAbstractButton::clicked, this,
                            [this, at] { revertToMessage(at); });
                    card.second->addWidget(revert, 0, Qt::AlignLeft);
                }
                wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 1, 8, 0);
                break;
            }

            case ai::Role::Assistant: {
                if (!message.text.empty()) {
                    auto card = messageCard(m_transcriptBody, "AiAssistantCard", tr("AI / RESPONSE"),
                                         "AiAssistantRole");
                    card.second->addWidget(cardText(m_transcriptBody, 
                        QString::fromStdString(message.text), "AiMessageText"));
                    wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
                }
                break;
            }

            case ai::Role::Tool: {
                auto card = messageCard(m_transcriptBody, "AiActionCard", tr("AI / ACTIONS"),
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
                            out.ok ? tr("DONE")
                                   : replan ? tr("REPLAN") : tr("ERROR"),
                            actionRow);
                        status->setObjectName(
                            out.ok ? "AiActionStatusOk"
                                   : replan ? "AiActionStatusWarn"
                                            : "AiActionStatusError");
                        status->setAlignment(Qt::AlignCenter);
                        status->setSizePolicy(QSizePolicy::Fixed,
                                              QSizePolicy::Fixed);
                        actionLayout->addWidget(status, 0, Qt::AlignTop);

                        QString detail = QString::fromStdString(out.name);
                        if (!out.ok) {
                            if (!resultError.empty())
                                detail += QStringLiteral(" — ") +
                                          QString::fromStdString(resultError);
                        }
                        auto* action = cardText(m_transcriptBody, detail, "AiActionText", true);
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
                                                tr("/compose Apply composition candidate %1 to the currently selected instrument track.")
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
    }

    if (!m_session->lastError().empty()) {
        auto card = messageCard(m_transcriptBody, "AiErrorCard", tr("AI / ERROR"),
                             "AiActionStatusError");
        card.second->addWidget(cardText(m_transcriptBody, 
            QString::fromStdString(m_session->lastError()), "AiMessageText"));
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
    }

    if (!m_streaming.isEmpty()) {
        auto card = messageCard(m_transcriptBody, "AiLiveCard", tr("AI / LIVE"), "AiLiveRole");
        m_streamingLabel = cardText(m_transcriptBody, m_streaming, "AiMessageText");
        card.second->addWidget(m_streamingLabel);
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
    }

    if (m_session->running()) {
        auto card = messageCard(m_transcriptBody, "AiThinkingCard", tr("AI / THINKING"),
                             "AiAssistantRole");
        card.second->addWidget(cardText(m_transcriptBody, tr("Working…"), "AiMessageSecondary",
                                        true));
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 6, 4);
    }

    if (m_session->messages().empty()) {
        auto card = messageCard(m_transcriptBody, "AiAssistantCard", tr("STUDIO COPILOT"),
                             "AiAssistantRole");
        card.second->addWidget(cardText(m_transcriptBody, 
            tr("Ask for a part and it gets made: \"make a piano, write the "
               "chords, process the channel\"."),
            "AiMessageSecondary", true));
        wrapRow(m_transcriptLayout, m_transcriptBody, card.first, 0, 10, 1);
    }

    m_transcriptLayout->addStretch(1);
    QTimer::singleShot(0, m_transcript, [this] {
        if (!m_transcript) return;
        m_transcript->verticalScrollBar()->setValue(
            m_transcript->verticalScrollBar()->maximum());
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
    setMode(Mode::Assistant, /*persist=*/false);
    m_client.reset(new ScriptedClient(this));
    updateReadiness();

    const std::size_t mark = m_controller->undoDepth();
    const std::size_t tracksBefore = m_controller->project().tracks.size();

    m_input->setPlainText(QStringLiteral("make a piano part"));
    send();

    // The scripted client answers through the event loop, so the run needs the
    // loop pumped rather than a wait.
    for (int i = 0; i < 400 && m_session->running(); ++i)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    if (m_session->running()) return false;

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
