#include "ContextPanel.hpp"

#include "Controls.hpp"
#include "Icons.hpp"
#include "PluginQuickAdder.hpp"
#include "SelectionModel.hpp"
#include "Theme.hpp"

#include "RecordingSettingsPage.hpp"

#include "EngineController.hpp"
#include "AudioMusicalAnalysis.hpp"
#include "plugins/PluginConvert.hpp"

#include <QColorDialog>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QSequentialAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegion>
#include <QSettings>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace {

// The island's proportions. It lives inside the 42-pixel tool strip, so every
// number here is chosen to add up to that: shadow + padding + row + padding +
// shadow. Controls are icon-sized; nothing here gets a caption.
constexpr int kRowHeight = 20;    // every control on the island is this tall
constexpr int kButton = 22;       // square icon buttons
constexpr int kPadding = 7;       // plate edge → controls, top and bottom
constexpr int kEndPadding = 14;  // extra breathing room at the rounded ends
constexpr int kShadow = 9;        // room for the shadow and the top flare

constexpr double kMinDb = -60.0;

/// The old row leaves briskly — it is on its way out and nobody is reading it.
constexpr int kSlideOutMs = 160;
/// The new row takes its time arriving, because it is what the user will read.
constexpr int kSlideInMs = 260;
/// The arrival starts before the exit has finished, so the swap reads as one
/// movement rather than two. Paired with the exit curve below, the old row is
/// ~98% of the way out by the time this fires: the two must never both sit
/// legibly on the plate at once, which is exactly the bug this replaced.
constexpr int kSlideInDelayMs = 120;
constexpr int kSpringMs = 320;
/// The sideways ride to a newly selected clip. Slow enough to be followed by
/// eye, quick enough not to be waited for — a swap across the whole window
/// takes about a quarter of a second.
constexpr int kDriftMs = 260;
/// Below this the plate is simply moved. Retargeting an animation for two
/// pixels costs more than it shows, and while a clip is being dragged the
/// target moves every frame.
constexpr int kDriftThreshold = 3;

bool followSelectionEnabled() {
    return QSettings().value("contextPanel/followSelection", true).toBool();
}

/// Continuous context-panel edits used to snapshot the complete ProjectModel.
/// That made grabbing an audio fade proportional to every MIDI note elsewhere
/// in the session. Keep only the four scalar endpoints these controls can
/// actually change and let the controller create its small delta on release.
struct ClipLevelGestureCapture {
    struct State {
        std::string trackId;
        std::string clipId;
        float gain = 1.0f;
        float pan = 0.0f;
    };

    std::vector<State> before;
    daw::EngineController::UndoGroup undoGroup;
    daw::EngineController* groupController = nullptr;

    ~ClipLevelGestureCapture() {
        if (groupController && undoGroup)
            groupController->releaseUndoGroup(undoGroup);
    }

    void capture(daw::EngineController* controller, const std::string& trackId,
                 const std::string& clipId) {
        if (!controller) return;
        if (std::any_of(before.begin(), before.end(), [&](const State& state) {
                return state.trackId == trackId && state.clipId == clipId;
            })) {
            return;
        }
        const daw::ClipModel* clip = controller->audioClip(trackId, clipId);
        if (!clip) return;
        if (before.empty()) {
            undoGroup = controller->beginUndoGroup();
            groupController = controller;
        }
        before.push_back({trackId, clipId, clip->gain, clip->pan});
    }

    void commit(daw::EngineController* controller, const std::string& label) {
        if (!controller || before.empty()) return;
        for (const State& state : before) {
            controller->commitClipFxLevelEdit(
                state.trackId, state.clipId, state.gain, state.pan, label);
        }
        controller->collapseUndo(undoGroup, label);
        undoGroup = {};
        groupController = nullptr;
        before.clear();
    }
};

struct ClipFadeGestureCapture {
    struct State {
        std::string trackId;
        std::string clipId;
        double fadeIn = 0.0;
        double fadeOut = 0.0;
    };

    std::vector<State> before;
    daw::EngineController::UndoGroup undoGroup;
    daw::EngineController* groupController = nullptr;

    ~ClipFadeGestureCapture() {
        if (groupController && undoGroup)
            groupController->releaseUndoGroup(undoGroup);
    }

    void capture(daw::EngineController* controller, const std::string& trackId,
                 const std::string& clipId) {
        if (!controller) return;
        if (std::any_of(before.begin(), before.end(), [&](const State& state) {
                return state.trackId == trackId && state.clipId == clipId;
            })) {
            return;
        }
        const daw::ClipModel* clip = controller->audioClip(trackId, clipId);
        if (!clip) return;
        if (before.empty()) {
            undoGroup = controller->beginUndoGroup();
            groupController = controller;
        }
        before.push_back(
            {trackId, clipId, clip->fadeInSeconds, clip->fadeOutSeconds});
    }

    void commit(daw::EngineController* controller, const std::string& label) {
        if (!controller || before.empty()) return;
        for (const State& state : before) {
            controller->commitClipFadeEdit(state.trackId, state.clipId,
                                           state.fadeIn, state.fadeOut, label);
        }
        controller->collapseUndo(undoGroup, label);
        undoGroup = {};
        groupController = nullptr;
        before.clear();
    }
};

}  // namespace

const std::vector<ContextTool>& contextPanelTools() {
    [[maybe_unused]] static const char* const translatableToolText[] = {
        QT_TRANSLATE_NOOP("ContextPanelTools", "Audio Clip"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Automation Clip"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "MIDI Clip"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Pattern Clip"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Track"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Piano Roll Note"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Recording"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Track colour"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Gain and mute"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Fade in / out"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Detected BPM and key"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Duplicate and delete"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Enable and editor"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Enable and Piano Roll"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Colour"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Mute, solo, mono"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Volume and pan"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Plugin quick-adder"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Colour and mute"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Velocity and pan"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Length and transpose"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Quantize, arpeggiate, chords…"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Layer / overwrite"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Count-in"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "Monitor while recording"),
        QT_TRANSLATE_NOOP("ContextPanelTools", "MIDI trigger (mock)"),
    };
    static const std::vector<ContextTool> tools = {
        {"clip.colour", "Audio Clip", "Track colour"},
        {"clip.level", "Audio Clip", "Gain and mute"},
        {"clip.fades", "Audio Clip", "Fade in / out"},
        {"clip.analysis", "Audio Clip", "Detected BPM and key"},
        {"clip.edit", "Audio Clip", "Duplicate and delete"},
        {"automation.colour", "Automation Clip", "Track colour"},
        {"automation.state", "Automation Clip", "Enable and editor"},
        {"automation.edit", "Automation Clip", "Duplicate and delete"},
        {"midi.colour", "MIDI Clip", "Track colour"},
        {"midi.state", "MIDI Clip", "Enable and Piano Roll"},
        {"midi.edit", "MIDI Clip", "Duplicate and delete"},
        {"pattern.colour", "Pattern Clip", "Track colour"},
        {"pattern.state", "Pattern Clip", "Enable and editor"},
        {"pattern.edit", "Pattern Clip", "Duplicate and delete"},
        {"track.colour", "Track", "Colour"},
        {"track.state", "Track", "Mute, solo, mono"},
        {"track.level", "Track", "Volume and pan"},
        {"track.plugins", "Track", "Plugin quick-adder"},
        {"note.colour", "Piano Roll Note", "Colour and mute"},
        {"note.level", "Piano Roll Note", "Velocity and pan"},
        {"note.timing", "Piano Roll Note", "Length and transpose"},
        {"note.tools", "Piano Roll Note", "Quantize, arpeggiate, chords…"},
        {"note.edit", "Piano Roll Note", "Duplicate and delete"},
        {"record.mode", "Recording", "Layer / overwrite"},
        {"record.countIn", "Recording", "Count-in"},
        {"record.monitor", "Recording", "Monitor while recording"},
        {"record.options", "Recording", "MIDI trigger (mock)"},
    };
    return tools;
}

ContextPanel::ContextPanel(daw::EngineController* controller,
                           ui::SelectionModel* selection, QWidget* parent)
    : ui::GlassPanel(parent), m_controller(controller), m_selection(selection) {
    m_follow = followSelectionEnabled();
    setAccentColor(th().accent);
    setShadowMargin(kShadow);
    // Hangs off the top of the tool strip and flares into it, rather than
    // floating in the middle of it as a separate object.
    setTopAttached(true);
    hide();
    // A selection the *user* made moves the panel on, even from the recording
    // island — engaging Record opens the take settings, it does not lock the
    // panel to them.
    connect(selection, &ui::SelectionModel::changed, this, [this] {
        m_recordPinned = false;
        onSelectionChanged();
    });
}

// ── Context resolution ──

ContextPanel::Context ContextPanel::resolve() const {
    if (!m_enabled || m_suppressed) return Context::None;
    // Engaging Record brings the take settings up straight away, but does not
    // pin the panel there: the next thing the user selects wins, and the
    // settings come back whenever the selection has nothing of its own to show.
    if (m_recordEngaged && m_recordPinned) return Context::Recording;

    const auto& clips = m_selection->clips();
    if (!clips.isEmpty()) {
        std::optional<daw::ClipKind> kind;
        for (const auto& sel : clips) {
            const auto* track =
                m_controller->project().findTrack(sel.trackId.toStdString());
            if (!track) return Context::Other;
            bool found = false;
            for (const auto& clip : track->clips) {
                if (clip.id != sel.clipId.toStdString()) continue;
                if (kind && *kind != clip.kind) return Context::Other;
                kind = clip.kind;
                found = true;
                break;
            }
            if (!found) return Context::Other;
        }
        if (!kind) return Context::Other;
        const bool multi = clips.size() > 1;
        switch (*kind) {
            case daw::ClipKind::Audio:
                return multi ? Context::AudioClipMulti : Context::AudioClip;
            case daw::ClipKind::Automation:
                return multi ? Context::AutomationClipMulti
                             : Context::AutomationClip;
            case daw::ClipKind::Pattern:
                return multi ? Context::PatternClipMulti
                             : Context::PatternClip;
            case daw::ClipKind::Midi:
                return multi ? Context::MidiClipMulti : Context::MidiClip;
        }
        return Context::Other;
    }

    const auto& tracks = m_selection->tracks();
    if (!tracks.isEmpty()) {
        // Only tracks that still exist count; a stale id in the selection must
        // not turn one real track into a "2 tracks" island.
        int live = 0;
        for (const QString& id : tracks) {
            if (m_controller->project().findTrack(id.toStdString())) ++live;
        }
        if (live > 1) return Context::TrackMulti;
        if (live == 1) return Context::Track;
    }
    // Nothing selected and Record engaged: the take settings are the most
    // useful thing the island can hold, so it falls back to them.
    return m_recordEngaged ? Context::Recording : Context::None;
}

QColor ContextPanel::accentFor(Context context) const {
    switch (context) {
        case Context::Recording:
            return Theme::record();
        case Context::AudioClip:
        case Context::AudioClipMulti:
            return Theme::audioAccent();
        case Context::AutomationClip:
        case Context::AutomationClipMulti:
        case Context::MidiClip:
        case Context::MidiClipMulti:
        case Context::PatternClip:
        case Context::PatternClipMulti: {
            if (!m_selection->clips().isEmpty()) {
                const auto* track = m_controller->project().findTrack(
                    m_selection->clips().first().trackId.toStdString());
                if (track) return colorFromRgb(track->color);
            }
            return th().accent;
        }
        case Context::Track:
        case Context::TrackMulti:
            return th().accent;
        default:
            return th().accent;
    }
}

// ── Profiles ──

bool ContextPanel::toolEnabled(const char* toolId) const {
    return QSettings()
        .value(QStringLiteral("contextPanel/%1").arg(QLatin1String(toolId)), true)
        .toBool();
}

// ── Small building blocks ──
//
// Everything on the island is icon-sized. It lives in a 32-pixel strip under
// the transport readout, so a control that needs a caption to be understood
// doesn't belong here — it belongs in the inspector or the mixer, where there
// is room for one.

namespace {

ui::IconButton* islandButton(icons::Glyph glyph, const QString& tip,
                             QWidget* parent) {
    auto* button = new ui::IconButton(glyph, tip, parent);
    button->setButtonSize(kButton, kButton);
    return button;
}

/// A continuous value as a single icon: hovering it slides a slider out, and
/// the icon itself is draggable. See ui::MiniSlider — the island has no room
/// for digits, so the number only appears while the value is being changed.
ui::MiniSlider* islandSlider(icons::Glyph glyph, const QString& tip,
                             QWidget* parent) {
    return new ui::MiniSlider(glyph, tip, parent);
}

QString formatDb(double db) {
    return db <= kMinDb + 0.01 ? QStringLiteral("\u2212\u221E dB")
                               : QString::asprintf("%+.1f dB", db);
}

QString formatMs(double ms) {
    return QString::asprintf("%.0f ms", ms);
}

/// The clickable colour chip that stands in for a name field — the object's
/// colour is the one piece of identity that survives being shrunk to 16 px.
QPushButton* islandSwatch(const QString& tip, QWidget* parent) {
    auto* swatch = new QPushButton(parent);
    swatch->setFixedSize(18, kRowHeight);
    swatch->setCursor(Qt::PointingHandCursor);
    swatch->setToolTip(tip);
    return swatch;
}

void paintSwatch(QPushButton* swatch, const QColor& colour) {
    swatch->setStyleSheet(
        QStringLiteral("background:%1;border:1px solid rgba(0,0,0,110);"
                       "border-radius:5px;")
            .arg(colour.name()));
}

/// Linear gain ↔ decibels. The island shows dB directly rather than a fader
/// position, because a 46-pixel readout has room for a number but not a throw.
double gainToDb(double gain) {
    return gain <= 0.0001 ? kMinDb : 20.0 * std::log10(gain);
}
double dbToGain(double db) {
    return db <= kMinDb + 0.01 ? 0.0 : std::pow(10.0, db / 20.0);
}

QWidget* islandDivider(QWidget* parent) {
    return ui::separatorLine(Qt::Vertical, 16, parent);
}

}  // namespace

QWidget* ContextPanel::newRow(QHBoxLayout*& row) {
    auto* host = new QWidget(this);
    row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    return host;
}

void ContextPanel::afterEdit(bool structural, bool localFileDirty) {
    emit projectEdited(localFileDirty);
    if (structural) emit tracksChanged(localFileDirty);
    invalidateBackdrop();
    refresh();
}

void ContextPanel::forEachSelectedClip(
    const std::function<void(const QString&, const QString&)>& fn) {
    // Copy first: an edit that adds or removes clips can invalidate the
    // selection mid-loop.
    const auto clips = m_selection->clips();
    for (const auto& sel : clips) fn(sel.trackId, sel.clipId);
}

// ── Content: single audio clip ──

QWidget* ContextPanel::buildAudioClip() {
    const ui::ClipSel sel = m_selection->singleClip();
    const std::string trackId = sel.trackId.toStdString();
    const std::string clipId = sel.clipId.toStdString();

    // Resolving the clip on every read rather than holding a pointer: undo
    // replaces whole clip vectors, so any pointer we cached would dangle.
    auto clipOf = [this, trackId, clipId]() -> const daw::ClipModel* {
        const auto* track = m_controller->project().findTrack(trackId);
        if (!track) return nullptr;
        for (const auto& c : track->clips)
            if (c.id == clipId) return &c;
        return nullptr;
    };
    if (!clipOf()) return nullptr;

    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);
    std::vector<std::function<void()>> loaders;

    if (toolEnabled("clip.colour")) {
        // A clip has no colour of its own — it wears its track's. So the chip
        // shows the track colour and picking a new one recolours the track,
        // clips and all.
        auto* swatch = islandSwatch(tr("Track colour"), host);
        connect(swatch, &QAbstractButton::clicked, this, [this, trackId] {
            const auto* t = m_controller->project().findTrack(trackId);
            if (!t) return;
            const QColor picked = QColorDialog::getColor(colorFromRgb(t->color),
                                                         this, tr("Track Colour"));
            if (!picked.isValid()) return;
            m_controller->setTrackColor(trackId,
                                        (uint32_t(picked.red()) << 16) |
                                            (uint32_t(picked.green()) << 8) |
                                            uint32_t(picked.blue()));
            afterEdit(/*structural=*/true);
        });
        row->addWidget(swatch);
        loaders.push_back([this, trackId, swatch] {
            if (const auto* t = m_controller->project().findTrack(trackId))
                paintSwatch(swatch, colorFromRgb(t->color));
        });
    }

    if (toolEnabled("clip.level")) {
        auto* mute = islandButton(icons::Glyph::Power,
                                  tr("Disable / enable this clip"), host);
        mute->setCheckable(true);
        mute->setActiveColor(Theme::mute());
        connect(mute, &QAbstractButton::clicked, this,
                [this, trackId, clipId](bool on) {
                    if (m_updating) return;
                    m_controller->setClipMuted(trackId, clipId, on);
                    afterEdit();
                });

        auto* gain = islandSlider(icons::Glyph::Volume, tr("Clip gain"), host);
        gain->setRange(kMinDb, 12.0);
        gain->setStep(0.25);
        gain->setDefaultValue(0.0);
        gain->setFormatter(formatDb);
        auto gainGesture = std::make_shared<ClipLevelGestureCapture>();
        connect(gain, &ui::MiniSlider::valueChanged, this,
                [this, trackId, clipId, gainGesture](double db) {
                    if (m_updating) return;
                    gainGesture->capture(m_controller, trackId, clipId);
                    m_controller->setClipGain(trackId, clipId, float(dbToGain(db)));
                });
        connect(gain, &ui::MiniSlider::editFinished, this,
                [this, gainGesture] {
                    gainGesture->commit(m_controller, "Set Clip Gain");
                    emit projectEdited();
                });

        row->addWidget(mute);
        row->addWidget(gain);
        loaders.push_back([clipOf, gain, mute] {
            const auto* c = clipOf();
            if (!c) return;
            gain->setValue(gainToDb(c->gain));
            mute->setChecked(c->muted);
        });
    }

    if (toolEnabled("clip.fades")) {
        row->addWidget(islandDivider(host));
        auto* fadeIn = islandSlider(icons::Glyph::FadeIn, tr("Fade in"), host);
        fadeIn->setRange(0.0, 4000.0);
        fadeIn->setStep(4.0);
        fadeIn->setFormatter(formatMs);
        auto* fadeOut = islandSlider(icons::Glyph::FadeOut, tr("Fade out"), host);
        fadeOut->setRange(0.0, 4000.0);
        fadeOut->setStep(4.0);
        fadeOut->setFormatter(formatMs);
        // Drags left, where its ramp points: the two fades mirror each other
        // exactly as the handles on the clip itself do.
        fadeOut->setInverted(true);

        auto fadeGesture = std::make_shared<ClipFadeGestureCapture>();
        auto apply = [this, trackId, clipId, fadeIn, fadeOut, fadeGesture] {
            if (m_updating) return;
            fadeGesture->capture(m_controller, trackId, clipId);
            m_controller->setClipFade(trackId, clipId, fadeIn->value() / 1000.0,
                                      fadeOut->value() / 1000.0);
        };
        connect(fadeIn, &ui::MiniSlider::valueChanged, this, apply);
        connect(fadeOut, &ui::MiniSlider::valueChanged, this, apply);
        // setClipFade clamps the pair against the clip length, so the controls
        // are re-read when the drag ends rather than trusting what was dragged.
        const auto finishFade = [this, fadeGesture] {
            fadeGesture->commit(m_controller, "Set Clip Fade");
            afterEdit();
        };
        connect(fadeIn, &ui::MiniSlider::editFinished, this, finishFade);
        connect(fadeOut, &ui::MiniSlider::editFinished, this, finishFade);

        row->addWidget(fadeIn);
        row->addWidget(fadeOut);
        loaders.push_back([clipOf, fadeIn, fadeOut] {
            const auto* c = clipOf();
            if (!c) return;
            fadeIn->setValue(c->fadeInSeconds * 1000.0);
            fadeOut->setValue(c->fadeOutSeconds * 1000.0);
        });
    }

    if (toolEnabled("clip.analysis")) {
        auto* analysisGroup = new QWidget(host);
        auto* analysisRow = new QHBoxLayout(analysisGroup);
        analysisRow->setContentsMargins(0, 0, 0, 0);
        analysisRow->setSpacing(7);
        analysisRow->addWidget(islandDivider(analysisGroup));
        auto* analysisText = new QLabel(analysisGroup);
        analysisText->setObjectName(QStringLiteral("ClipMusicalAnalysis"));
        analysisText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        analysisText->setStyleSheet(
            QStringLiteral("font-size: 10px; color: palette(midlight);"));
        analysisRow->addWidget(analysisText);
        row->addWidget(analysisGroup);
        loaders.push_back([clipOf, analysisGroup, analysisText] {
            const auto* c = clipOf();
            if (!c || c->musicalAnalysis.empty()) {
                analysisGroup->setVisible(false);
                return;
            }
            QStringList parts;
            QStringList details;
            const auto& measured = c->musicalAnalysis;
            if (measured.tempo.status != daw::MusicalAnalysisStatus::Unavailable &&
                measured.tempo.bpm > 0.0) {
                parts << QObject::tr("%1 BPM").arg(measured.tempo.bpm, 0, 'f', 1);
                details << QObject::tr("Tempo confidence: %1%")
                               .arg(int(std::lround(measured.tempo.confidence * 100.0)));
            }
            if (measured.key.status != daw::MusicalAnalysisStatus::Unavailable &&
                measured.key.root >= 0) {
                daw::analysis::KeyEstimate key;
                key.root = measured.key.root;
                key.scale = measured.key.scale;
                const QString name = QString::fromStdString(
                    daw::analysis::keyDisplayName(key));
                const QString camelot = QString::fromStdString(
                    daw::analysis::camelotName(key.root, key.scale));
                parts << QObject::tr("%1 · %2").arg(name, camelot);
                details << QObject::tr("Key confidence: %1%")
                               .arg(int(std::lround(measured.key.confidence * 100.0)));
            }
            analysisText->setText(parts.join(QStringLiteral("   ")));
            analysisText->setToolTip(details.join(QLatin1Char('\n')));
            analysisGroup->setVisible(!parts.isEmpty());
        });
    }

    if (toolEnabled("clip.edit")) {
        row->addWidget(islandDivider(host));
        // No split here: the Knife tool on the arrangement already does it, at
        // the point you click rather than wherever the playhead happens to be.
        auto* duplicate = islandButton(icons::Glyph::Plus, tr("Duplicate clip"), host);
        auto* remove = islandButton(icons::Glyph::Trash, tr("Delete clip"), host);

        connect(duplicate, &QAbstractButton::clicked, this, [this, trackId, clipId] {
            if (m_controller->duplicateClip(trackId, clipId).empty()) return;
            flashConfirm();
            afterEdit();
        });
        connect(remove, &QAbstractButton::clicked, this, [this, trackId, clipId] {
            m_controller->removeClip(trackId, clipId);
            m_selection->clear();
            afterEdit();
        });

        row->addWidget(duplicate);
        row->addWidget(remove);
    }

    m_applyValues = [loaders = std::move(loaders), this] {
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

// ── Content: several audio clips ──

QWidget* ContextPanel::buildAudioClipMulti() {
    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);

    auto* count = new QLabel(host);
    QFont bold = count->font();
    bold.setPixelSize(11);
    bold.setBold(true);
    count->setFont(bold);
    row->addWidget(count);
    row->addWidget(islandDivider(host));

    if (toolEnabled("clip.level")) {
        // Relative, not absolute: the clips start at different levels and one
        // shared value would flatten them all to the same gain.
        auto nudge = [this](double db) {
            ClipLevelGestureCapture gesture;
            const double factor = std::pow(10.0, db / 20.0);
            forEachSelectedClip([this, factor, &gesture](const QString& trackId,
                                                         const QString& clipId) {
                const std::string owner = trackId.toStdString();
                const std::string id = clipId.toStdString();
                const auto* track =
                    m_controller->project().findTrack(owner);
                if (!track) return;
                for (const auto& c : track->clips) {
                    if (c.id != id) continue;
                    gesture.capture(m_controller, owner, id);
                    m_controller->setClipGain(
                        owner, id,
                        float(std::clamp(double(c.gain) * factor, 0.0,
                                         std::pow(10.0, 12.0 / 20.0))));
                    return;
                }
            });
            gesture.commit(m_controller, "Set Clip Gains");
            afterEdit();
        };
        auto* down = islandButton(icons::Glyph::Minus, tr("Quieter by 1 dB"), host);
        auto* up = islandButton(icons::Glyph::Plus, tr("Louder by 1 dB"), host);
        connect(down, &QAbstractButton::clicked, this, [nudge] { nudge(-1.0); });
        connect(up, &QAbstractButton::clicked, this, [nudge] { nudge(1.0); });

        auto* mute = islandButton(icons::Glyph::Power,
                                  tr("Disable / enable selected clips"), host);
        mute->setCheckable(true);
        mute->setActiveColor(Theme::mute());
        // Not a state, an action: the clips can disagree, so this always mutes
        // and a second press (with the chip lit) unmutes.
        connect(mute, &QAbstractButton::clicked, this, [this, mute](bool on) {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this, on](const QString& trackId,
                                           const QString& clipId) {
                m_controller->setClipMuted(trackId.toStdString(),
                                           clipId.toStdString(), on);
            });
            m_controller->collapseUndo(undoGroup,
                                       on ? "Mute Clips" : "Unmute Clips");
            mute->setToolTip(on ? tr("Unmute all") : tr("Mute all"));
            afterEdit();
        });

        row->addWidget(down);
        row->addWidget(up);
        row->addWidget(mute);
    }

    if (toolEnabled("clip.fades")) {
        row->addWidget(islandDivider(host));
        auto* fadeIn = islandSlider(icons::Glyph::FadeIn, tr("Fade in on all"), host);
        fadeIn->setRange(0.0, 4000.0);
        fadeIn->setStep(4.0);
        fadeIn->setFormatter(formatMs);
        auto* fadeOut = islandSlider(icons::Glyph::FadeOut, tr("Fade out on all"), host);
        fadeOut->setRange(0.0, 4000.0);
        fadeOut->setStep(4.0);
        fadeOut->setFormatter(formatMs);
        fadeOut->setInverted(true);

        auto fadeGesture = std::make_shared<ClipFadeGestureCapture>();
        auto apply = [this, fadeIn, fadeOut, fadeGesture] {
            if (m_updating) return;
            forEachSelectedClip([this, fadeIn, fadeOut, fadeGesture](
                                    const QString& trackId,
                                    const QString& clipId) {
                const std::string owner = trackId.toStdString();
                const std::string id = clipId.toStdString();
                fadeGesture->capture(m_controller, owner, id);
                m_controller->setClipFade(owner, id,
                                          fadeIn->value() / 1000.0,
                                          fadeOut->value() / 1000.0);
            });
        };
        connect(fadeIn, &ui::MiniSlider::valueChanged, this, apply);
        connect(fadeOut, &ui::MiniSlider::valueChanged, this, apply);
        const auto finishFade = [this, fadeGesture] {
            fadeGesture->commit(m_controller, "Set Clip Fades");
            afterEdit();
        };
        connect(fadeIn, &ui::MiniSlider::editFinished, this, finishFade);
        connect(fadeOut, &ui::MiniSlider::editFinished, this, finishFade);
        row->addWidget(fadeIn);
        row->addWidget(fadeOut);
    }

    if (toolEnabled("clip.edit")) {
        row->addWidget(islandDivider(host));
        auto* duplicate = islandButton(icons::Glyph::Plus,
                                       tr("Duplicate all"), host);
        auto* remove = islandButton(icons::Glyph::Trash, tr("Delete all"), host);

        connect(duplicate, &QAbstractButton::clicked, this, [this] {
            forEachSelectedClip([this](const QString& trackId, const QString& clipId) {
                m_controller->duplicateClip(trackId.toStdString(),
                                            clipId.toStdString());
            });
            flashConfirm();
            afterEdit();
        });
        connect(remove, &QAbstractButton::clicked, this, [this] {
            forEachSelectedClip([this](const QString& trackId, const QString& clipId) {
                m_controller->removeClip(trackId.toStdString(),
                                          clipId.toStdString());
            });
            m_selection->clear();
            afterEdit();
        });
        row->addWidget(duplicate);
        row->addWidget(remove);
    }

    m_applyValues = [this, count] {
        count->setText(tr("Clips: %1").arg(m_selection->clips().size()));
    };
    m_applyValues();
    return host;
}

// ── Content: automation clips ──

QWidget* ContextPanel::buildAutomationClip(bool multi) {
    const auto selections = m_selection->clips();
    if (selections.isEmpty()) return nullptr;

    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);
    std::vector<std::function<void()>> loaders;

    if (multi) {
        auto* count = new QLabel(
            tr("Curves: %1").arg(selections.size()), host);
        QFont font = count->font();
        font.setPixelSize(11);
        font.setBold(true);
        count->setFont(font);
        row->addWidget(count);
    }

    const auto colourOwners = [this, selections] {
        std::set<std::string> unique;
        std::vector<std::string> owners;
        for (const auto& sel : selections) {
            const auto* lane = m_controller->project().findTrack(
                sel.trackId.toStdString());
            if (!lane) continue;
            const std::string owner = lane->parentId.empty() ? lane->id
                                                              : lane->parentId;
            if (unique.insert(owner).second) owners.push_back(owner);
        }
        return owners;
    };

    if (toolEnabled("automation.colour")) {
        auto* swatch = islandSwatch(tr("Automation track colour"), host);
        connect(swatch, &QAbstractButton::clicked, this,
                [this, colourOwners] {
                    const auto owners = colourOwners();
                    if (owners.empty()) return;
                    const auto* first =
                        m_controller->project().findTrack(owners.front());
                    if (!first) return;
                    const QColor picked = QColorDialog::getColor(
                        colorFromRgb(first->color), this,
                        tr("Automation Track Colour"));
                    if (!picked.isValid()) return;
                    const uint32_t rgb = (uint32_t(picked.red()) << 16) |
                                         (uint32_t(picked.green()) << 8) |
                                         uint32_t(picked.blue());
                    for (const std::string& owner : owners)
                        m_controller->setTrackColor(owner, rgb);
                    afterEdit(/*structural=*/true);
                });
        row->addWidget(swatch);
        loaders.push_back([this, colourOwners, swatch] {
            const auto owners = colourOwners();
            if (owners.empty()) return;
            if (const auto* owner =
                    m_controller->project().findTrack(owners.front())) {
                paintSwatch(swatch, colorFromRgb(owner->color));
            }
        });
    }

    if (toolEnabled("automation.state")) {
        auto* disable = islandButton(
            icons::Glyph::Power,
            multi ? tr("Disable or enable selected automation clips")
                  : tr("Disable or enable this automation clip"),
            host);
        disable->setCheckable(true);
        disable->setActiveColor(Theme::mute());
        connect(disable, &QAbstractButton::clicked, this,
                [this, disable](bool on) {
                    const auto undoGroup = m_controller->beginUndoGroup();
                    forEachSelectedClip(
                        [this, on](const QString& trackId,
                                   const QString& clipId) {
                            m_controller->setClipMuted(trackId.toStdString(),
                                                       clipId.toStdString(), on);
                        });
                    m_controller->collapseUndo(
                        undoGroup, on ? "Disable Automation Clips"
                                 : "Enable Automation Clips");
                    afterEdit();
                });

        auto* editor = islandButton(
            icons::Glyph::Automation,
            multi ? tr("Open the first selected curve in the Automation Editor")
                  : tr("Open Automation Editor"),
            host);
        connect(editor, &QAbstractButton::clicked, this, [this] {
            if (m_selection->clips().isEmpty()) return;
            const auto sel = m_selection->clips().first();
            emit automationEditorRequested(sel.trackId, sel.clipId);
        });
        row->addWidget(disable);
        row->addWidget(editor);
        loaders.push_back([this, disable] {
            const auto clips = m_selection->clips();
            bool allMuted = !clips.isEmpty();
            for (const auto& sel : clips) {
                const auto* track = m_controller->project().findTrack(
                    sel.trackId.toStdString());
                const daw::ClipModel* clip = nullptr;
                if (track) {
                    for (const auto& candidate : track->clips) {
                        if (candidate.id == sel.clipId.toStdString()) {
                            clip = &candidate;
                            break;
                        }
                    }
                }
                if (!clip || !clip->muted) {
                    allMuted = false;
                    break;
                }
            }
            disable->setChecked(allMuted);
        });
    }

    if (toolEnabled("automation.edit")) {
        row->addWidget(islandDivider(host));
        auto* duplicate = islandButton(
            icons::Glyph::Plus,
            multi ? tr("Duplicate selected automation clips")
                  : tr("Duplicate automation clip"),
            host);
        auto* remove = islandButton(
            icons::Glyph::Trash,
            multi ? tr("Delete selected automation clips")
                  : tr("Delete automation clip"),
            host);
        connect(duplicate, &QAbstractButton::clicked, this, [this] {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this](const QString& trackId,
                                       const QString& clipId) {
                m_controller->duplicateClip(trackId.toStdString(),
                                            clipId.toStdString());
            });
            m_controller->collapseUndo(undoGroup,
                                       "Duplicate Automation Clips");
            afterEdit();
        });
        connect(remove, &QAbstractButton::clicked, this, [this] {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this](const QString& trackId,
                                       const QString& clipId) {
                m_controller->removeClip(trackId.toStdString(),
                                         clipId.toStdString());
            });
            m_controller->collapseUndo(undoGroup,
                                       "Delete Automation Clips");
            m_selection->clear();
            afterEdit();
        });
        row->addWidget(duplicate);
        row->addWidget(remove);
    }

    m_applyValues = [this, loaders = std::move(loaders)] {
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

// ── Content: MIDI clips ──

QWidget* ContextPanel::buildMidiClip(bool multi) {
    const auto selections = m_selection->clips();
    if (selections.isEmpty()) return nullptr;

    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);
    std::vector<std::function<void()>> loaders;

    if (multi) {
        auto* count = new QLabel(
            tr("MIDI clips: %1").arg(selections.size()), host);
        QFont font = count->font();
        font.setPixelSize(11);
        font.setBold(true);
        count->setFont(font);
        row->addWidget(count);
    }

    if (toolEnabled("midi.colour")) {
        auto* swatch = islandSwatch(tr("MIDI track colour"), host);
        connect(swatch, &QAbstractButton::clicked, this,
                [this, selections] {
                    const auto* first = m_controller->project().findTrack(
                        selections.first().trackId.toStdString());
                    if (!first) return;
                    const QColor picked = QColorDialog::getColor(
                        colorFromRgb(first->color), this, tr("MIDI Track Colour"));
                    if (!picked.isValid()) return;
                    const uint32_t rgb = (uint32_t(picked.red()) << 16) |
                                         (uint32_t(picked.green()) << 8) |
                                         uint32_t(picked.blue());
                    std::set<std::string> changed;
                    for (const auto& sel : selections) {
                        const std::string id = sel.trackId.toStdString();
                        if (changed.insert(id).second)
                            m_controller->setTrackColor(id, rgb);
                    }
                    afterEdit(/*structural=*/true);
                });
        row->addWidget(swatch);
        loaders.push_back([this, selections, swatch] {
            if (const auto* track = m_controller->project().findTrack(
                    selections.first().trackId.toStdString())) {
                paintSwatch(swatch, colorFromRgb(track->color));
            }
        });
    }

    if (toolEnabled("midi.state")) {
        auto* disable = islandButton(
            icons::Glyph::Power,
            multi ? tr("Disable or enable selected MIDI clips")
                  : tr("Disable or enable this MIDI clip"),
            host);
        disable->setCheckable(true);
        disable->setActiveColor(Theme::mute());
        connect(disable, &QAbstractButton::clicked, this,
                [this](bool on) {
                    const auto undoGroup = m_controller->beginUndoGroup();
                    forEachSelectedClip([this, on](const QString& trackId,
                                                   const QString& clipId) {
                        m_controller->setClipMuted(trackId.toStdString(),
                                                   clipId.toStdString(), on);
                    });
                    m_controller->collapseUndo(
                        undoGroup,
                        on ? "Disable MIDI Clips" : "Enable MIDI Clips");
                    afterEdit();
                });

        auto* editor = islandButton(
            icons::Glyph::MidiKeys,
            multi ? tr("Open the first selected MIDI clip in Piano Roll")
                  : tr("Open Piano Roll"),
            host);
        editor->setObjectName(QStringLiteral("MidiClipEditorButton"));
        editor->setAccessibleName(tr("Open Piano Roll"));
        connect(editor, &QAbstractButton::clicked, this, [this] {
            if (m_selection->clips().isEmpty()) return;
            const auto sel = m_selection->clips().first();
            emit midiEditorRequested(sel.trackId, sel.clipId);
        });
        row->addWidget(disable);
        row->addWidget(editor);

        loaders.push_back([this, disable] {
            bool allMuted = !m_selection->clips().isEmpty();
            for (const auto& sel : m_selection->clips()) {
                const auto* track = m_controller->project().findTrack(
                    sel.trackId.toStdString());
                const daw::ClipModel* clip = nullptr;
                if (track) {
                    for (const auto& candidate : track->clips) {
                        if (candidate.id == sel.clipId.toStdString()) {
                            clip = &candidate;
                            break;
                        }
                    }
                }
                if (!clip || !clip->muted) {
                    allMuted = false;
                    break;
                }
            }
            disable->setChecked(allMuted);
        });
    }

    if (toolEnabled("midi.edit")) {
        row->addWidget(islandDivider(host));
        auto* duplicate = islandButton(
            icons::Glyph::Plus,
            multi ? tr("Duplicate selected MIDI clips")
                  : tr("Duplicate MIDI clip"),
            host);
        auto* remove = islandButton(
            icons::Glyph::Trash,
            multi ? tr("Delete selected MIDI clips") : tr("Delete MIDI clip"),
            host);
        connect(duplicate, &QAbstractButton::clicked, this, [this] {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this](const QString& trackId,
                                       const QString& clipId) {
                m_controller->duplicateClip(trackId.toStdString(),
                                            clipId.toStdString());
            });
            m_controller->collapseUndo(undoGroup, "Duplicate MIDI Clips");
            afterEdit();
        });
        connect(remove, &QAbstractButton::clicked, this, [this] {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this](const QString& trackId,
                                       const QString& clipId) {
                m_controller->removeClip(trackId.toStdString(),
                                         clipId.toStdString());
            });
            m_controller->collapseUndo(undoGroup, "Delete MIDI Clips");
            m_selection->clear();
            afterEdit();
        });
        row->addWidget(duplicate);
        row->addWidget(remove);
    }

    m_applyValues = [this, loaders = std::move(loaders)] {
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

// ── Content: Pattern clips ──

QWidget* ContextPanel::buildPatternClip(bool multi) {
    const auto selections = m_selection->clips();
    if (selections.isEmpty()) return nullptr;

    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);
    std::vector<std::function<void()>> loaders;

    if (multi) {
        auto* count = new QLabel(
            tr("Patterns: %1").arg(selections.size()), host);
        QFont font = count->font();
        font.setPixelSize(11);
        font.setBold(true);
        count->setFont(font);
        row->addWidget(count);
    }

    if (toolEnabled("pattern.colour")) {
        auto* swatch = islandSwatch(tr("Pattern track colour"), host);
        connect(swatch, &QAbstractButton::clicked, this,
                [this, selections] {
                    const auto* first = m_controller->project().findTrack(
                        selections.first().trackId.toStdString());
                    if (!first) return;
                    const QColor picked = QColorDialog::getColor(
                        colorFromRgb(first->color), this, tr("Pattern Colour"));
                    if (!picked.isValid()) return;
                    const uint32_t rgb = (uint32_t(picked.red()) << 16) |
                                         (uint32_t(picked.green()) << 8) |
                                         uint32_t(picked.blue());
                    std::set<std::string> changed;
                    for (const auto& sel : selections) {
                        const std::string id = sel.trackId.toStdString();
                        if (changed.insert(id).second)
                            m_controller->setTrackColor(id, rgb);
                    }
                    afterEdit(/*structural=*/true);
                });
        row->addWidget(swatch);
        loaders.push_back([this, selections, swatch] {
            if (const auto* track = m_controller->project().findTrack(
                    selections.first().trackId.toStdString())) {
                paintSwatch(swatch, colorFromRgb(track->color));
            }
        });
    }

    if (toolEnabled("pattern.state")) {
        auto* disable = islandButton(
            icons::Glyph::Power,
            multi ? tr("Disable or enable selected Pattern clips")
                  : tr("Disable or enable this Pattern clip"),
            host);
        disable->setCheckable(true);
        disable->setActiveColor(Theme::mute());
        connect(disable, &QAbstractButton::clicked, this,
                [this](bool on) {
                    const auto undoGroup = m_controller->beginUndoGroup();
                    forEachSelectedClip([this, on](const QString& trackId,
                                                   const QString& clipId) {
                        m_controller->setClipMuted(trackId.toStdString(),
                                                   clipId.toStdString(), on);
                    });
                    m_controller->collapseUndo(
                        undoGroup, on ? "Disable Pattern Clips"
                                 : "Enable Pattern Clips");
                    afterEdit();
                });

        auto* editor = islandButton(
            icons::Glyph::Grid,
            multi ? tr("Open the first selected Pattern in the editor")
                  : tr("Open Pattern Editor"),
            host);
        connect(editor, &QAbstractButton::clicked, this, [this] {
            if (m_selection->clips().isEmpty()) return;
            emit patternEditorRequested(m_selection->clips().first().trackId);
        });
        row->addWidget(disable);
        row->addWidget(editor);
        loaders.push_back([this, disable] {
            bool allMuted = !m_selection->clips().isEmpty();
            for (const auto& sel : m_selection->clips()) {
                const auto* track = m_controller->project().findTrack(
                    sel.trackId.toStdString());
                const daw::ClipModel* clip = nullptr;
                if (track) {
                    for (const auto& candidate : track->clips) {
                        if (candidate.id == sel.clipId.toStdString()) {
                            clip = &candidate;
                            break;
                        }
                    }
                }
                if (!clip || !clip->muted) {
                    allMuted = false;
                    break;
                }
            }
            disable->setChecked(allMuted);
        });
    }

    if (toolEnabled("pattern.edit")) {
        row->addWidget(islandDivider(host));
        auto* duplicate = islandButton(
            icons::Glyph::Plus,
            multi ? tr("Duplicate selected Pattern clips")
                  : tr("Duplicate Pattern clip"),
            host);
        auto* remove = islandButton(
            icons::Glyph::Trash,
            multi ? tr("Delete selected Pattern clips")
                  : tr("Delete Pattern clip"),
            host);
        connect(duplicate, &QAbstractButton::clicked, this, [this] {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this](const QString& trackId,
                                       const QString& clipId) {
                m_controller->duplicateClip(trackId.toStdString(),
                                            clipId.toStdString());
            });
            m_controller->collapseUndo(undoGroup,
                                       "Duplicate Pattern Clips");
            afterEdit();
        });
        connect(remove, &QAbstractButton::clicked, this, [this] {
            const auto undoGroup = m_controller->beginUndoGroup();
            forEachSelectedClip([this](const QString& trackId,
                                       const QString& clipId) {
                m_controller->removeClip(trackId.toStdString(),
                                         clipId.toStdString());
            });
            m_controller->collapseUndo(undoGroup, "Delete Pattern Clips");
            m_selection->clear();
            afterEdit();
        });
        row->addWidget(duplicate);
        row->addWidget(remove);
    }

    m_applyValues = [this, loaders = std::move(loaders)] {
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

// ── Content: several tracks at once ──

std::vector<std::string> ContextPanel::selectedTracks() const {
    std::vector<std::string> out;
    for (const QString& id : m_selection->tracks()) {
        if (m_controller->project().findTrack(id.toStdString()))
            out.push_back(id.toStdString());
    }
    return out;
}

QWidget* ContextPanel::buildTrackMulti() {
    const std::vector<std::string> tracks = selectedTracks();
    if (tracks.size() < 2) return nullptr;

    QHBoxLayout* outer = nullptr;
    QWidget* host = newRow(outer);
    auto* actionsHost = new QWidget(host);
    auto* row = new QHBoxLayout(actionsHost);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    outer->addWidget(actionsHost, 0, Qt::AlignVCenter);

    auto* count = new QLabel(host);
    QFont bold = count->font();
    bold.setPixelSize(11);
    bold.setBold(true);
    count->setFont(bold);
    row->addWidget(count);

    // Nothing here reads a value back off the tracks, because there is no one
    // value to read: five tracks at five levels have no shared level to show.
    // Every control is an action.
    const auto forEach = [this, tracks](auto&& act) {
        for (const std::string& id : tracks) {
            if (m_controller->project().findTrack(id)) act(id);
        }
    };

    if (toolEnabled("track.colour")) {
        row->addWidget(islandDivider(host));
        auto* swatch = islandSwatch(tr("Colour every selected track"), host);
        // One swatch for a set of different colours: it shows the first one,
        // which is what the picker will open on.
        const auto* first = m_controller->project().findTrack(tracks.front());
        paintSwatch(swatch, colorFromRgb(first ? first->color : 0x4A90D9));
        connect(swatch, &QAbstractButton::clicked, this,
                [this, forEach, swatch, tracks] {
                    const auto* lead = m_controller->project().findTrack(tracks.front());
                    const QColor picked = QColorDialog::getColor(
                        colorFromRgb(lead ? lead->color : 0x4A90D9), this,
                        tr("Track Colour"));
                    if (!picked.isValid()) return;
                    const uint32_t rgb = (uint32_t(picked.red()) << 16) |
                                         (uint32_t(picked.green()) << 8) |
                                         uint32_t(picked.blue());
                    // A folder among them takes its contents with it — that is
                    // what `setTrackColor` does for a folder.
                    forEach([this, rgb](const std::string& id) {
                        m_controller->setTrackColor(id, rgb);
                    });
                    paintSwatch(swatch, picked);
                    afterEdit(/*structural=*/true);
                });
        row->addWidget(swatch);
    }

    if (toolEnabled("track.state")) {
        row->addWidget(islandDivider(host));
        // Actions, not states: the tracks can disagree, so each chip mutes (or
        // solos) everything and a second press with the chip lit lifts it.
        auto* mute = new ui::MsrButton(
            tr("M"), Theme::mute(), tr("Mute them all"), host);
        connect(mute, &QAbstractButton::clicked, this,
                [this, mute, tracks](bool on) {
                    const auto result =
                        m_controller->setTracksMuted(tracks, on);
                    mute->setToolTip(on ? tr("Unmute them all")
                                        : tr("Mute them all"));
                    afterEdit(/*structural=*/true,
                              daw::collab::marksLocalFileDirty(result));
                });
        row->addWidget(mute);

        const auto stateChip = [&](const QString& letter, const QColor& colour,
                                   const QString& onTip, const QString& offTip,
                                   auto&& setter) {
            auto* chip = new ui::MsrButton(letter, colour, onTip, host);
            connect(chip, &QAbstractButton::clicked, this,
                    [this, chip, onTip, offTip, forEach, setter](bool on) {
                        forEach([&](const std::string& id) { setter(id, on); });
                        chip->setToolTip(on ? offTip : onTip);
                        afterEdit(/*structural=*/true);
                    });
            row->addWidget(chip);
        };
        stateChip(tr("S"), Theme::solo(), tr("Solo them all"),
                  tr("Clear their solos"),
                  [this](const std::string& id, bool on) {
                      m_controller->setTrackSoloed(id, on);
                  });
    }

    if (toolEnabled("track.level")) {
        row->addWidget(islandDivider(host));
        // Relative: the tracks are at different levels, and one shared value
        // would flatten the balance the mix already has.
        const auto nudge = [this, forEach](double db) {
            const auto undoGroup = m_controller->beginUndoGroup();
            const double factor = std::pow(10.0, db / 20.0);
            forEach([this, factor](const std::string& id) {
                const auto* t = m_controller->project().findTrack(id);
                if (!t) return;
                m_controller->setTrackVolume(
                    id, float(std::clamp(double(t->volume) * factor, 0.0, 2.0)));
            });
            m_controller->collapseUndo(undoGroup, "Set Track Volumes");
            afterEdit(/*structural=*/true);
        };
        auto* down = islandButton(icons::Glyph::Minus, tr("Quieter by 1 dB"), host);
        auto* up = islandButton(icons::Glyph::Plus, tr("Louder by 1 dB"), host);
        connect(down, &QAbstractButton::clicked, this, [nudge] { nudge(-1.0); });
        connect(up, &QAbstractButton::clicked, this, [nudge] { nudge(1.0); });
        row->addWidget(down);
        row->addWidget(up);

        // Pan as an offset, not a position: the slider starts centred and says
        // how far to *shift* everything, so a spread survives being moved.
        auto* pan = islandSlider(icons::Glyph::Pan,
                                 tr("Shift the pan of every selected track"),
                                 host);
        pan->setRange(-100.0, 100.0);
        pan->setStep(1.0);
        pan->setDefaultValue(0.0);
        pan->setFormatter([](double v) {
            return std::abs(v) < 0.5
                       ? QStringLiteral("0")
                       : QString::asprintf("%+.0f", v);
        });
        auto baseline = std::make_shared<std::vector<std::pair<std::string, float>>>();
        connect(pan, &ui::MiniSlider::valueChanged, this,
                [this, tracks, baseline](double shift) {
                    if (m_updating) return;
                    if (baseline->empty()) {
                        for (const std::string& id : tracks) {
                            if (const auto* t = m_controller->project().findTrack(id))
                                baseline->emplace_back(id, t->pan);
                        }
                    }
                    for (const auto& [id, from] : *baseline) {
                        m_controller->setTrackPanLive(
                            id, float(std::clamp(double(from) + shift / 100.0,
                                                 -1.0, 1.0)));
                    }
                    emit liveEdited();
                });
        connect(pan, &ui::MiniSlider::editFinished, this,
                [this, baseline] {
                    m_controller->commitTrackPanEdit(*baseline,
                                                     "Shift Track Pans");
                    baseline->clear();
                    afterEdit(/*structural=*/true);
                });
        row->addWidget(pan);
    }

    if (toolEnabled("track.plugins")) {
        outer->addWidget(islandDivider(host));
        // The adder loads into the first track, then the same plugin is loaded
        // into the rest — one search, a plugin on every selected channel.
        auto* adder = new PluginQuickAdder(m_controller, host);
        adder->setTrackId(QString::fromStdString(tracks.front()));
        adder->setAccentColor(th().accent);
        outer->addWidget(adder, 0, Qt::AlignVCenter);
        m_quickAdder = adder;

        connect(adder, &PluginQuickAdder::pluginInserted, this,
                [this, tracks](const QString& insertId, bool) {
                    if (insertId.isEmpty()) return;
                    // Read back what actually landed, so the copies are the
                    // plugin the user picked rather than a guess at it.
                    const auto* lead = m_controller->project().findTrack(tracks.front());
                    const daw::InsertModel* added = nullptr;
                    if (lead) {
                        for (const auto& insert : lead->inserts) {
                            if (insert.id == insertId.toStdString()) added = &insert;
                        }
                    }
                    if (added) {
                        daw::plugins::PluginDescriptor descriptor;
                        descriptor.format = daw::toHostFormat(added->format);
                        descriptor.uid = added->uid;
                        descriptor.path = added->path;
                        descriptor.name = added->name;
                        descriptor.vendor = added->vendor;
                        for (size_t i = 1; i < tracks.size(); ++i)
                            m_controller->addInsert(tracks[i], descriptor);
                    }
                    emit projectEdited();
                    emit tracksChanged();
                    invalidateBackdrop();
                });
        connect(adder, &PluginQuickAdder::editorRequested, this,
                [this, track = QString::fromStdString(tracks.front())](
                    const QString& insertId) {
                    emit pluginEditorRequested(track, insertId);
                });
        connect(adder, &PluginQuickAdder::searchStateChanged, this,
                [this, host, actionsHost](bool expanded) {
                    actionsHost->setVisible(!expanded);
                    host->resize(host->sizeHint().width(), kRowHeight);
                    if (QWidget* strip = parentWidget()) strip->setFixedHeight(44);
                    setGeometry(targetGeometry());
                    layoutSelf();
                    invalidateBackdrop();
                    update();
                });
        connect(adder, &PluginQuickAdder::sizeChanged, this, [this, host] {
            if (!m_content || m_content != host) return;
            host->resize(host->sizeHint().width(), kRowHeight);
            setGeometry(targetGeometry());
            layoutSelf();
            invalidateBackdrop();
        });
    }

    m_applyValues = [this, count] {
        count->setText(tr("Tracks: %1").arg(selectedTracks().size()));
    };
    m_applyValues();
    return host;
}

// ── Content: track / mixer channel ──

QWidget* ContextPanel::buildTrack() {
    const std::string trackId = m_selection->singleTrack().toStdString();
    auto trackOf = [this, trackId]() -> const daw::TrackModel* {
        return m_controller->project().findTrack(trackId);
    };
    if (!trackOf()) return nullptr;
    const bool automationLane = daw::isAutomationLane(*trackOf());
    const std::string colourTrackId =
        automationLane && !trackOf()->parentId.empty() ? trackOf()->parentId
                                                       : trackId;
    auto colourTrackOf = [this, colourTrackId]() -> const daw::TrackModel* {
        return m_controller->project().findTrack(colourTrackId);
    };

    QHBoxLayout* outer = nullptr;
    QWidget* host = newRow(outer);
    auto* actionsHost = new QWidget(host);
    auto* row = new QHBoxLayout(actionsHost);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    outer->addWidget(actionsHost, 0, Qt::AlignVCenter);
    std::vector<std::function<void()>> loaders;

    if (toolEnabled("track.colour")) {
        auto* swatch = islandSwatch(tr("Track colour"), host);
        connect(swatch, &QAbstractButton::clicked, this,
                [this, colourTrackOf, colourTrackId] {
            const auto* t = colourTrackOf();
            if (!t) return;
            const QColor picked = QColorDialog::getColor(colorFromRgb(t->color),
                                                         this, tr("Track Colour"));
            if (!picked.isValid()) return;
            m_controller->setTrackColor(colourTrackId,
                                        (uint32_t(picked.red()) << 16) |
                                            (uint32_t(picked.green()) << 8) |
                                            uint32_t(picked.blue()));
            afterEdit(/*structural=*/true);
        });
        row->addWidget(swatch);
        loaders.push_back([colourTrackOf, swatch] {
            if (const auto* t = colourTrackOf())
                paintSwatch(swatch, colorFromRgb(t->color));
        });
    }

    // A folder that does not sum has no level, no pan, no channel to fold and
    // nowhere to put a plugin. Its colour and its mute/solo are the whole of
    // what it has.
    const bool hasChannel = daw::carriesAudio(*trackOf());

    if (hasChannel) {
        auto* automation = islandButton(
            icons::Glyph::Automation,
            tr("Show or hide automation for this track"), host);
        automation->setCheckable(true);
        automation->setAccessibleName(
            tr("Show or hide automation for this track"));
        connect(automation, &QAbstractButton::clicked, this,
                [this, trackId] { emit automationToggleRequested(
                    QString::fromStdString(trackId)); });
        row->addWidget(automation);
        loaders.push_back([this, trackOf, automation] {
            const auto* track = trackOf();
            if (!track) return;
            automation->setChecked(
                track->automationExpanded &&
                !m_controller->automationLanesOf(track->id).empty());
        });
    }

    if (toolEnabled("track.state") && !automationLane) {
        auto* mute = new ui::MsrButton(tr("M"), Theme::mute(), tr("Mute"), host);
        auto* solo = new ui::MsrButton(tr("S"), Theme::solo(), tr("Solo"), host);
        auto* mono = islandButton(icons::Glyph::MonoRing, tr("Fold to mono"), host);
        mono->setCheckable(true);
        mono->setVisible(hasChannel);

        if (hasChannel) {
            mute->setAutomatable(true);
            connect(mute, &ui::MsrButton::automateRequested, this,
                    [this, trackId] {
                        emit automateMuteRequested(QString::fromStdString(trackId));
                    });
        }

        connect(mute, &QAbstractButton::clicked, this, [this, trackId](bool on) {
            if (m_updating) return;
            const auto result = m_controller->setTrackMuted(trackId, on);
            afterEdit(/*structural=*/true,
                      daw::collab::marksLocalFileDirty(result));
        });
        connect(solo, &QAbstractButton::clicked, this, [this, trackId](bool on) {
            if (m_updating) return;
            m_controller->setTrackSoloed(trackId, on);
            afterEdit(/*structural=*/true);
        });
        connect(mono, &QAbstractButton::clicked, this, [this, trackId](bool on) {
            if (m_updating) return;
            m_controller->setTrackMono(trackId, on);
            afterEdit(/*structural=*/true);
        });

        row->addWidget(mute);
        row->addWidget(solo);
        row->addWidget(mono);
        loaders.push_back([trackOf, mute, solo, mono] {
            const auto* t = trackOf();
            if (!t) return;
            mute->setChecked(t->muted);
            solo->setChecked(t->soloed);
            mono->setChecked(t->mono);
        });
    }

    if (hasChannel && toolEnabled("track.level")) {
        row->addWidget(islandDivider(host));
        auto* volume = islandSlider(icons::Glyph::Volume, tr("Volume"), host);
        volume->setObjectName(QStringLiteral("ContextPanelTrackVolume"));
        volume->setAutomatable(true);
        connect(volume, &ui::MiniSlider::automateRequested, this,
                [this, trackId] {
                    emit automateControlRequested(QString::fromStdString(trackId),
                                                  false);
                });
        volume->setRange(kMinDb, 6.0);
        volume->setStep(0.25);
        volume->setDefaultValue(0.0);
        volume->setFormatter(formatDb);
        auto volumeStart = std::make_shared<std::optional<float>>();
        connect(volume, &ui::MiniSlider::valueChanged, this,
                [this, trackId, volumeStart](double db) {
                    if (m_updating) return;
                    if (!*volumeStart) {
                        if (const auto* track =
                                m_controller->project().findTrack(trackId))
                            *volumeStart = track->volume;
                    }
                    m_controller->setTrackVolumeLive(trackId,
                                                     float(dbToGain(db)));
                    emit liveEdited();
                });
        connect(volume, &ui::MiniSlider::editFinished, this,
                [this, trackId, volumeStart] {
                    if (*volumeStart) {
                        m_controller->commitTrackVolumeEdit(
                            {{trackId, **volumeStart}});
                        volumeStart->reset();
                    }
                    emit projectEdited();
                });

        auto* pan = islandSlider(icons::Glyph::Pan, tr("Pan"), host);
        pan->setAutomatable(true);
        connect(pan, &ui::MiniSlider::automateRequested, this,
                [this, trackId] {
                    emit automateControlRequested(QString::fromStdString(trackId),
                                                  true);
                });
        pan->setRange(-100.0, 100.0);
        pan->setStep(0.8);
        pan->setDefaultValue(0.0);
        // Pan is a knob on the strip, on the track header and in every other
        // host; a line would be the odd one out. Bipolar, so the arc grows out
        // of the centre rather than out of hard left.
        pan->setRotary(true);
        pan->setBipolar(true);
        pan->setFormatter([](double value) {
            const int amount = int(std::round(std::abs(value)));
            if (amount == 0) return QStringLiteral("Centre");
            return (value < 0 ? QStringLiteral("L") : QStringLiteral("R")) +
                   QString::number(amount);
        });
        auto panStart = std::make_shared<std::optional<float>>();
        connect(pan, &ui::MiniSlider::valueChanged, this,
                [this, trackId, panStart](double value) {
                    if (m_updating) return;
                    if (!*panStart) {
                        if (const auto* track =
                                m_controller->project().findTrack(trackId))
                            *panStart = track->pan;
                    }
                    m_controller->setTrackPanLive(trackId,
                                                  float(value / 100.0));
                    emit liveEdited();
                });
        connect(pan, &ui::MiniSlider::editFinished, this,
                [this, trackId, panStart] {
                    if (*panStart) {
                        m_controller->commitTrackPanEdit(
                            {{trackId, **panStart}});
                        panStart->reset();
                    }
                    emit projectEdited();
                });

        row->addWidget(volume);
        row->addWidget(pan);
        loaders.push_back([trackOf, volume, pan] {
            const auto* t = trackOf();
            if (!t) return;
            volume->setValue(gainToDb(t->volume));
            pan->setValue(t->pan * 100.0);
        });
    }

    if (hasChannel && toolEnabled("track.plugins")) {
        QWidget* pluginDivider = nullptr;
        if (row->count() > 0) {
            pluginDivider = islandDivider(host);
            outer->addWidget(pluginDivider);
        }
        auto* adder = new PluginQuickAdder(m_controller, host);
        adder->setTrackId(QString::fromStdString(trackId));
        adder->setAccentColor(colorFromRgb(trackOf()->color));
        outer->addWidget(adder, 0, Qt::AlignVCenter);
        m_quickAdder = adder;

        connect(adder, &PluginQuickAdder::pluginInserted, this,
                [this](const QString& insertId, bool) {
                    if (insertId.isEmpty()) return;
                    emit projectEdited();
                    emit tracksChanged();
                    invalidateBackdrop();
                });
        connect(adder, &PluginQuickAdder::editorRequested, this,
                [this, track = QString::fromStdString(trackId)](
                    const QString& insertId) {
                    emit pluginEditorRequested(track, insertId);
                });
        connect(adder, &PluginQuickAdder::searchStateChanged, this,
                [this, host, actionsHost, pluginDivider](bool expanded) {
                    actionsHost->setVisible(!expanded);
                    if (pluginDivider) pluginDivider->setVisible(!expanded);
                    host->resize(host->sizeHint().width(), kRowHeight);
                    if (QWidget* strip = parentWidget()) strip->setFixedHeight(44);
                    setGeometry(targetGeometry());
                    layoutSelf();
                    invalidateBackdrop();
                    update();
                });
        connect(adder, &PluginQuickAdder::sizeChanged, this, [this, host] {
            if (!m_content || m_content != host) return;
            host->resize(host->sizeHint().width(), kRowHeight);
            if (QWidget* strip = parentWidget()) strip->setFixedHeight(44);
            setGeometry(targetGeometry());
            layoutSelf();
            invalidateBackdrop();
            update();
        });
    }

    m_applyValues = [loaders = std::move(loaders), this] {
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

// ── Content: recording setup ──
//
// Shown while Record is engaged but nothing is capturing yet. Pressing Record
// used to start immediately, which meant the count-in, the metronome and the
// target were decided before the gesture or not at all; engaging first puts
// them here, one press away, and Start is the second half of the gesture.

QWidget* ContextPanel::buildRecording() {
    QHBoxLayout* row = nullptr;
    QWidget* host = newRow(row);
    std::vector<std::function<void()>> loaders;

    // Everything here is a *setting*, not a one-shot command: the panel is
    // where the next take is set up, so every control writes through to the
    // recording preferences and is read back from them. Disengaging Record and
    // engaging it again finds the island exactly as it was left.
    if (toolEnabled("record.mode")) {
        auto* layers = islandButton(
            icons::Glyph::Layers, tr("Layer recording (comping)"), host);
        layers->setCheckable(true);
        connect(layers, &QAbstractButton::clicked, this, [this](bool on) {
            if (m_updating) return;
            const auto mode =
                on ? daw::RecordMode::Layers : daw::RecordMode::Overwrite;
            m_controller->setRecordMode(mode);
            RecordingSettingsPage::persistMode(mode);
            emit recordModeChanged();
        });
        row->addWidget(layers);
        loaders.push_back([this, layers] {
            const bool on = m_controller->recordMode() == daw::RecordMode::Layers;
            layers->setChecked(on);
            layers->setEnabled(!m_controller->isRecording());
            layers->setToolTip(on ? tr("Layer recording — every take is kept")
                                  : tr("Overwrite — a new take replaces what "
                                       "is there"));
        });
    }

    if (toolEnabled("record.countIn")) {
        // On/off here, length in Preferences ▸ Recording: the island has room
        // for the decision, not for the number. Switching it off keeps the
        // chosen length aside so switching it back on restores it rather than
        // silently resetting to three.
        auto* countIn =
            islandButton(icons::Glyph::CountIn, tr("Count in before recording"),
                         host);
        countIn->setCheckable(true);
        countIn->setActiveColor(Theme::record());
        connect(countIn, &QAbstractButton::clicked, this, [this](bool on) {
            if (m_updating) return;
            auto prefs = m_controller->recordingPrefs();
            QSettings settings;
            const QString kept = QStringLiteral("recording/countInBeatsPreferred");
            if (on) {
                prefs.countInBeats =
                    std::clamp(settings.value(kept, 3).toInt(), 1, 8);
            } else {
                if (prefs.countInBeats > 0)
                    settings.setValue(kept, prefs.countInBeats);
                prefs.countInBeats = 0;
            }
            m_controller->setRecordingPrefs(prefs);
            RecordingSettingsPage::persistCountInBeats(prefs.countInBeats);
        });
        row->addWidget(countIn);
        loaders.push_back([this, countIn] {
            const int beats = m_controller->recordingPrefs().countInBeats;
            countIn->setChecked(beats > 0);
            countIn->setEnabled(!m_controller->isRecording() &&
                                !m_controller->isCountingIn());
            countIn->setToolTip(
                beats > 0
                    ? tr("Count-in beats: %1, then record").arg(beats)
                    : tr("No count-in — recording starts at once"));
        });
    }

    if (toolEnabled("record.monitor")) {
        // Monitoring belongs to the take, not to the session: on for as long as
        // the recording runs (and the count-in before it), off again after. Off
        // here means recording never touches it, so only a monitor the user
        // opened by hand is heard.
        auto* monitor = islandButton(
            icons::Glyph::Headphones, tr("Monitor while recording"), host);
        monitor->setCheckable(true);
        connect(monitor, &QAbstractButton::clicked, this, [this](bool on) {
            if (m_updating) return;
            auto prefs = m_controller->recordingPrefs();
            prefs.autoMonitorOnRecord = on;
            m_controller->setRecordingPrefs(prefs);
            RecordingSettingsPage::persistAutoMonitor(on);
        });
        row->addWidget(monitor);
        loaders.push_back([this, monitor] {
            const bool on = m_controller->recordingPrefs().autoMonitorOnRecord;
            monitor->setChecked(on);
            monitor->setEnabled(!m_controller->isRecording() &&
                                !m_controller->isCountingIn());
            monitor->setToolTip(
                on ? tr("Input is monitored while recording, then goes back to "
                        "how it was")
                   : tr("Recording leaves monitoring alone — only tracks you "
                        "monitor yourself are heard"));
        });
    }

    if (toolEnabled("record.options")) {
        // Deliberately a mock: it remembers being switched on and says so, but
        // nothing listens for MIDI yet. Better an honest placeholder in the
        // right place than a button that silently does nothing.
        auto* midiTrigger = islandButton(
            icons::Glyph::MidiKeys,
            tr("Start recording when you play (not connected yet)"), host);
        midiTrigger->setCheckable(true);
        connect(midiTrigger, &QAbstractButton::clicked, this,
                [this, midiTrigger](bool on) {
            if (m_updating) return;
            QSettings().setValue(QStringLiteral("recording/midiTriggerStart"), on);
            if (on) {
                // Say it out loud rather than let the button imply a feature
                // that isn't there; the tooltip alone is too easy to miss.
                ui::ValueBubble::showFor(
                    this, midiTrigger->mapTo(this, QPoint(midiTrigger->width() / 2, 0)),
                    tr("Not connected yet"));
                QTimer::singleShot(1400, this, [] { ui::ValueBubble::dismiss(); });
            }
        });
        row->addWidget(midiTrigger);

        loaders.push_back([this, midiTrigger] {
            midiTrigger->setChecked(
                QSettings()
                    .value(QStringLiteral("recording/midiTriggerStart"), false)
                    .toBool());
        });
    }

    row->addWidget(islandDivider(host));
    // The same chip starts and stops — and cancels a count-in, which is the
    // only thing "stop" can mean before there is anything recorded. It does
    // exactly what R does, so the two never disagree.
    auto* start = islandButton(icons::Glyph::Record, tr("Start recording (R)"),
                               host);
    start->setActiveColor(Theme::record());
    start->setProminent(true);
    connect(start, &QAbstractButton::clicked, this,
            [this] { emit recordingToggleRequested(); });
    row->addWidget(start);
    loaders.push_back([this, start] {
        const bool rolling = m_controller->isRecording();
        const bool counting = m_controller->isCountingIn();
        start->setGlyph(rolling || counting ? icons::Glyph::Stop
                                            : icons::Glyph::Record);
        start->setToolTip(counting  ? tr("Cancel the count-in (R)")
                          : rolling ? tr("Stop recording (R)")
                                    : tr("Start recording (R)"));
    });

    m_applyValues = [loaders = std::move(loaders), this] {
        m_updating = true;
        for (const auto& load : loaders) load();
        m_updating = false;
    };
    m_applyValues();
    return host;
}

void ContextPanel::setRecordEngaged(bool engaged) {
    if (m_recordEngaged == engaged) return;
    m_recordEngaged = engaged;
    // Engaging shows the take settings even over a selected clip; releasing
    // drops that claim and the selection has the panel back.
    m_recordPinned = engaged;
    // Same path as a selection change: the context is re-resolved and the
    // content swapped with the usual motion, so engaging Record reads as the
    // panel changing its mind rather than a different widget appearing.
    onSelectionChanged();
}

QWidget* ContextPanel::buildContent(Context context) {
    m_quickAdder = nullptr;
    switch (context) {
        case Context::AudioClip:      return buildAudioClip();
        case Context::AudioClipMulti: return buildAudioClipMulti();
        case Context::AutomationClip: return buildAutomationClip(false);
        case Context::AutomationClipMulti: return buildAutomationClip(true);
        case Context::MidiClip: return buildMidiClip(false);
        case Context::MidiClipMulti: return buildMidiClip(true);
        case Context::PatternClip: return buildPatternClip(false);
        case Context::PatternClipMulti: return buildPatternClip(true);
        case Context::Track:          return buildTrack();
        case Context::TrackMulti:     return buildTrackMulti();
        case Context::Recording:      return buildRecording();
        default:                      return nullptr;
    }
}

void ContextPanel::openPluginSearch() {
    if (resolve() != Context::Track) return;
    if (m_context != Context::Track || !m_quickAdder) {
        rebuild();
        QTimer::singleShot(kSpringMs + 10, this, [this] {
            if (m_quickAdder) m_quickAdder->openSearch();
        });
        return;
    }
    m_quickAdder->openSearch();
}

// ── Selection → content ──

void ContextPanel::onSelectionChanged() {
    const Context next = resolve();

    // The identity of what is selected, so re-selecting the *same* object after
    // an unrelated refresh doesn't rebuild and restart the animation.
    QString key;
    if (next == Context::AudioClip || next == Context::AutomationClip ||
        next == Context::MidiClip || next == Context::PatternClip) {
        key = m_selection->singleClip().clipId;
    } else if (next == Context::Track) {
        key = m_selection->singleTrack();
    } else if (next == Context::AudioClipMulti ||
               next == Context::AutomationClipMulti ||
               next == Context::MidiClipMulti ||
               next == Context::PatternClipMulti) {
        key = QString::number(m_selection->clips().size());
        for (const auto& sel : m_selection->clips()) key += '/' + sel.clipId;
    } else if (next == Context::TrackMulti) {
        key = QString::number(m_selection->tracks().size());
        for (const QString& id : m_selection->tracks()) key += '/' + id;
    }

    if (next == m_context && key == m_contextKey) {
        refresh();   // same object, values may have moved under us
        return;
    }

    m_contextKey = key;
    transitionTo(buildContent(next), next);
}

void ContextPanel::refresh() {
    if (m_applyValues) m_applyValues();
}

void ContextPanel::rebuild() {
    m_context = Context::None;
    m_contextKey.clear();
    onSelectionChanged();
}

void ContextPanel::setPanelEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    rebuild();
}

void ContextPanel::setSuppressed(bool suppressed) {
    if (m_suppressed == suppressed) return;
    m_suppressed = suppressed;
    // A shared strip must never show two islands at once. Hiding the outgoing
    // arrangement context immediately leaves the note island a clean surface;
    // restoring it may use the usual entrance transition.
    if (suppressed) hide();
    rebuild();
}

// ── Motion ──

void ContextPanel::transitionTo(QWidget* next, Context context) {
    // stop() destroys the group (DeleteWhenStopped) and never emits finished,
    // so the interrupted swap's cleanup has to happen right here instead.
    if (m_transition) m_transition->stop();
    // The sideways ride and the content swap both drive `geometry`. The swap
    // knows where it is going, so it wins and the drift is dropped.
    if (m_drift) m_drift->stop();
    m_contentSliding = false;
    if (m_outgoing) {
        m_outgoing->deleteLater();
        m_outgoing = nullptr;
    }

    const bool wasVisible = isVisible() && m_content;
    m_outgoing = m_content;
    m_content = next;
    m_context = next ? context : Context::None;
    if (!next) m_applyValues = nullptr;

    // Needed before the animations are built: the incoming row slides to where
    // it will rest inside the plate's *final* size, not its current one.
    const QRect target = targetGeometry();

    if (m_content) {
        // As tall as its tallest control, not a fixed 20: a child bigger than
        // its host gets clipped by it, and the prominent record chip — a filled
        // circle spanning the whole button — showed that as flattened top and
        // bottom edges. The plate has room to spare either way.
        m_content->resize(m_content->sizeHint().width(),
                          std::max(kRowHeight, m_content->sizeHint().height()));
        if (QWidget* strip = parentWidget()) strip->setFixedHeight(44);
        layoutSelf();
    } else if (QWidget* strip = parentWidget()) {
        strip->setFixedHeight(44);
    }

    auto* group = new QParallelAnimationGroup(this);

    // Where the new row will come to rest, inside the plate's *final* size
    // rather than its current one.
    QPoint rest;
    if (m_content) {
        const QRect plate(kShadow, 0, std::max(1, target.width() - 2 * kShadow),
                          std::max(1, target.height() - kShadow));
        rest = QPoint(plate.center().x() - m_content->width() / 2,
                      plate.center().y() - m_content->height() / 2);
    }

    // The old row leaves the plate entirely rather than nudging aside. A child
    // is clipped to its parent, so once it is past the left edge it is simply
    // gone — no opacity effect needed, and QWidget graphics effects were tried
    // here and rejected: they recursively render sibling trees into the same
    // backing store, which was both expensive and unsafe during a window flush.
    //
    // Nudging was what it used to do, and it was the bug: 20 pixels left the
    // old buttons sitting there in full view while the new ones arrived on top
    // of them, so a swap showed two rows at once.
    if (m_outgoing) {
        const int exitX = -m_outgoing->width() - 8;
        auto* slide = new QPropertyAnimation(m_outgoing, "pos");
        slide->setDuration(kSlideOutMs);
        slide->setStartValue(m_outgoing->pos());
        slide->setEndValue(QPoint(exitX, m_outgoing->y()));
        // Decelerating, NOT accelerating. An ease-in spends its first half
        // barely moving, which left the old row sitting in place until the new
        // one was already arriving — two rows on the plate at once, which is
        // what this transition looked wrong for. Leaving at once is what makes
        // the plate clear in time.
        slide->setEasingCurve(QEasingCurve::OutCubic);
        connect(slide, &QVariantAnimation::valueChanged, this,
                [this] { updateContentMasks(); });
        group->addAnimation(slide);
        // Dropped the moment it is off the plate, not when the whole swap ends.
        // The spring outlives it, and a row kept alive that long can reappear if
        // anything moves the plate underneath it.
        QPointer<QWidget> leaving = m_outgoing;
        connect(slide, &QAbstractAnimation::finished, this, [leaving] {
            if (leaving) leaving->hide();
        });
    }

    // …and the new one comes in from off the right edge, after a short pause so
    // the plate is clear when it appears.
    if (m_content && wasVisible) {
        const QPoint start(target.width() + 8, rest.y());
        // Placed off-plate *before* it is shown, or it would flash at its
        // resting place for the length of the pause.
        m_content->move(start);
        auto* slideIn = new QPropertyAnimation(m_content, "pos");
        slideIn->setDuration(kSlideInMs);
        slideIn->setStartValue(start);
        slideIn->setEndValue(rest);
        slideIn->setEasingCurve(QEasingCurve::OutCubic);
        connect(slideIn, &QVariantAnimation::valueChanged, this,
                [this] { updateContentMasks(); });

        auto* delayed = new QSequentialAnimationGroup(this);
        delayed->addPause(kSlideInDelayMs);
        delayed->addAnimation(slideIn);
        group->addAnimation(delayed);
        // The spring resizes the plate every frame, and each resize would
        // re-centre the row and overwrite this animation's work. This is what
        // holds layoutSelf() off until the arrival is done.
        m_contentSliding = true;
    } else if (m_content) {
        m_content->move(rest);
    }
    updateContentMasks();
    if (m_content) m_content->show();

    // The plate springs to the size the new content needs.
    if (!isVisible() && m_content) {
        // Coming from nothing: start narrow and centred so it opens outwards
        // rather than sliding in from a corner.
        setGeometry(QRect(target.center().x(), target.y(), 1, target.height()));
        show();
        raise();
    }
    if (isVisible()) {
        auto* spring = new QPropertyAnimation(this, "geometry");
        spring->setDuration(kSpringMs);
        spring->setStartValue(geometry());
        spring->setEndValue(target);
        // A bounded ease keeps the plate inside the arrangement even when its
        // target is flush with the right edge. An overshooting spring pushed
        // the whole mask past that boundary and made the arriving controls
        // look as though they were travelling over the panel from outside.
        spring->setEasingCurve(QEasingCurve::OutCubic);
        group->addAnimation(spring);
    }

    // The accent slides between the context colours instead of cutting.
    auto* tint = new QVariantAnimation(this);
    tint->setDuration(kSpringMs);
    tint->setStartValue(accentColor());
    tint->setEndValue(accentFor(m_context));
    connect(tint, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) { setAccentColor(v.value<QColor>()); });
    group->addAnimation(tint);

    connect(group, &QAbstractAnimation::finished, this, [this, wasVisible] {
        setBackdropFrozen(false);
        m_contentSliding = false;
        if (m_outgoing) {
            m_outgoing->deleteLater();
            m_outgoing = nullptr;
        }
        if (!m_content) {
            hide();
        } else {
            // A child can expand while this context-swap spring is still
            // running (Ctrl+F immediately after selecting a track). The spring
            // still owns geometry until this callback, so settle once more
            // against the child's current size instead of restoring the stale
            // collapsed target it captured at the start.
            const QRect settled = targetGeometry();
            if (geometry() != settled) setGeometry(settled);
            layoutSelf();
        }
        invalidateBackdrop();
        update();
        Q_UNUSED(wasVisible);
    });

    m_transition = group;
    setBackdropFrozen(true);
    group->start(QAbstractAnimation::DeleteWhenStopped);

    if (!m_content && !wasVisible) hide();
}

QRect ContextPanel::targetGeometry() const {
    QWidget* host = parentWidget();
    if (!host) return geometry();
    if (!m_content) {
        // Collapsed: keep the centre so the close reads as a shrink back into
        // the bar rather than a slide off to one side.
        const QRect current = geometry();
        return QRect(current.center().x(), 0, 1, current.height());
    }

    // The island is only as wide as its controls need and always as tall as the
    // strip it sits in.
    const int contentWidth = std::max(m_content->width(), m_content->sizeHint().width());
    const int width = std::min(contentWidth +
                                   2 * (kShadow + kEndPadding),
                               host->width() - 24);
    // Height is the plate plus the shadow below it; the top is flush with the
    // strip's own top edge, which is what makes the two read as one surface.
    const int height = kRowHeight + 2 * kPadding + kShadow;

    // Centred is the fallback, and the whole behaviour when the user has pinned
    // it: under the transport's position readout, where it has always been.
    // The plate belongs over the arrangement — that is what it talks about —
    // so its travel is bounded by the arrangement's own edges rather than by
    // the window's. Without a provider it may use the whole strip.
    int limitLeft = 12;
    int limitRight = std::max(12, host->width() - 12);
    int boundsLeft = 0, boundsRight = 0;
    if (m_boundsProvider && m_boundsProvider(boundsLeft, boundsRight) &&
        boundsRight - boundsLeft > 40) {
        limitLeft = boundsLeft;
        limitRight = boundsRight;
    }
    const int rightmost = std::max(limitLeft, limitRight - width);

    int left = std::clamp((host->width() - width) / 2, limitLeft, rightmost);
    int anchorCentreX = 0;
    if (m_follow && m_anchorProvider && m_anchorProvider(anchorCentreX)) {
        // Above the selection, but never past the arrangement: a clip at the
        // far right pulls the plate to that edge and no further, so it stays
        // whole and never drifts over the track headers.
        left = std::clamp(anchorCentreX - width / 2, limitLeft, rightmost);
    }
    return QRect(left, 0, std::max(1, width), height);
}

void ContextPanel::layoutSelf() {
    if (m_content && !m_contentSliding) {
        // The content keeps its own size through the spring and is simply
        // centred, so nothing reflows mid-animation.
        const QRect inner = plateRect();
        const QSize size = m_content->size();
        m_content->move(inner.center().x() - size.width() / 2,
                        inner.center().y() - size.height() / 2);
    }
    updateContentMasks();
}

void ContextPanel::updateContentMasks() {
    const auto clipRow = [this](QWidget* row) {
        if (!row) return;
        // Children are normally clipped only to ContextPanel::rect(), which
        // includes the shadow gutter. Translating the actual glass outline
        // into row coordinates makes controls appear from *inside* the plate
        // and disappear at its rim during both halves of the swap.
        const QPainterPath inRow = plateShape().translated(-row->pos());
        QRegion visible(inRow.toFillPolygon().toPolygon());
        row->setMask(visible.intersected(QRegion(row->rect())));
    };
    clipRow(m_outgoing);
    clipRow(m_content);
}

void ContextPanel::relayout() {
    if (!m_content || !isVisible()) return;
    setGeometry(targetGeometry());
}

void ContextPanel::setAnchorProvider(std::function<bool(int&)> provider) {
    m_anchorProvider = std::move(provider);
}

void ContextPanel::setBoundsProvider(std::function<bool(int&, int&)> provider) {
    m_boundsProvider = std::move(provider);
}

void ContextPanel::followSelection() {
    if (!m_follow || !m_content || !isVisible()) return;
    // A content swap already animates toward the geometry it read for itself;
    // letting the drift retarget underneath it would make the two fight over
    // the same property.
    if (m_transition && m_transition->state() == QAbstractAnimation::Running) return;
    driftTo(targetGeometry());
}

void ContextPanel::driftTo(const QRect& target) {
    if (target == geometry()) return;
    if (std::abs(target.x() - x()) < kDriftThreshold && target.size() == size()) {
        return;
    }
    // Retarget rather than restart: the plate keeps whatever speed it already
    // had, which is what makes a dragged clip feel followed instead of chased.
    if (m_drift && m_drift->state() == QAbstractAnimation::Running &&
        m_drift->endValue().toRect() == target) {
        return;
    }
    if (m_drift) m_drift->stop();

    auto* slide = new QPropertyAnimation(this, "geometry", this);
    slide->setDuration(kDriftMs);
    slide->setStartValue(geometry());
    slide->setEndValue(target);
    // No overshoot here. The plate is tracking something the user is looking
    // at, and a bounce past it would read as imprecision rather than life.
    slide->setEasingCurve(QEasingCurve::InOutCubic);
    m_drift = slide;
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void ContextPanel::reloadFollowSetting() {
    const bool follow = followSelectionEnabled();
    if (m_follow == follow) return;
    m_follow = follow;
    if (m_content && isVisible()) driftTo(targetGeometry());
}

void ContextPanel::resizeEvent(QResizeEvent* ev) {
    ui::GlassPanel::resizeEvent(ev);
    // Fully rounded ends, whatever the height: this is a floating island, not
    // a rounded rectangle.
    setCornerRadius(plateRect().height() / 2);   // fully rounded lower edge
    layoutSelf();
}
