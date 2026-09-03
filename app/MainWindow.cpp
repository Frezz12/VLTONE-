#include "MainWindow.hpp"
#include "CompLayout.hpp"
#include "BrowserPrefs.hpp"
#include "ChannelStrip.hpp"
#include "ChannelStripPresets.hpp"
#include "ContextPanel.hpp"
#include "AiChatPanel.hpp"
#include "AiPrefs.hpp"
#include "AudioPreferences.hpp"
#include "AccountService.hpp"
#include "CollaborationService.hpp"
#include "collaboration/ProjectReducer.hpp"
#ifdef DAW_ENABLE_COLLABORATION
#include "AssetCache.hpp"
#include "CloudProjectPublisher.hpp"
#include "CloudProjectSyncCoordinator.hpp"
#include "CloudProjectAssetHydrator.hpp"
#include "CloudSharedAssetMutationBridge.hpp"
#include "CloudSessionLifecycleController.hpp"
#include "CloudProjectInviteDialog.hpp"
#include "CollaborationCommandBridge.hpp"
#include "RecordingLeaseCoordinator.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "cloud/CloudPublicationCapture.hpp"
#include "recovery/CloudRecordingRecovery.hpp"
#endif
#include "PresenceInputRouter.hpp"
#include "PresenceOverlay.hpp"
#include "SessionStatusWidget.hpp"
#include "FileBrowserPanel.hpp"
#include "EqualizerPanel.hpp"
#include "GraphitPanel.hpp"
#include "GravityPanel.hpp"
#include "Controls.hpp"
#include "GlassPanel.hpp"
#include "Icons.hpp"
#include "InspectorWidget.hpp"
#include "InternalEditorFrame.hpp"
#include "MixerWidget.hpp"
#include "MidiInputManager.hpp"
#include "NoteContextPanel.hpp"
#include "PianoRollWindow.hpp"
#include "PatternWindow.hpp"
#include "RecordingSettingsPage.hpp"
#include "PluginEditorWindow.hpp"
#include "AutomationEditorWindow.hpp"
#include "SampleEditorWindow.hpp"
#include "ExportDialog.hpp"
#include "BounceInPlaceDialog.hpp"
#include "OfflineRenderDialog.hpp"
#include "PluginManagerWindow.hpp"
#include "PluginQuickAdder.hpp"
#include "PlatformDiagnostics.hpp"
#include "PluginPickerMenu.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"
#include "ProjectTemplates.hpp"
#include "RecoveryPrefs.hpp"
#include "RecoverySupport.hpp"
#include "plugins/ScanProcess.hpp"
#include "crash/CrashHandler.hpp"
#include "SettingsWindow.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"
#include "TrackListWidget.hpp"
#include "FileTypes.hpp"
#include "plugins/PluginConvert.hpp"
#include "Internal/GravityInstance.hpp"
#include "Internal/GraphitInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "TransportBar.hpp"
#include "TypingKeyboard.hpp"
#include "UiConstants.hpp"
#include "WebBrowserPanel.hpp"
#include "WebPrefs.hpp"

#include <array>

#include <QAction>
#include <QActionGroup>
#include <QAbstractButton>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QInputDialog>
#include <QJsonArray>
#include <QListWidget>
#include <QPushButton>
#include <QProcess>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QUuid>
#include <QScreen>
#include <QScopedValueRollback>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QTextEdit>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QShortcut>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QThread>
#include <QVBoxLayout>
#include <QWindow>

#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"

#include <QDir>
#include <QDoubleSpinBox>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace {
QString displayProjectName(const std::string& rawName) {
    const QString name = QString::fromStdString(rawName);
    return name == QLatin1String("Untitled")
               ? QCoreApplication::translate("MainWindow", "Untitled")
               : name;
}

QString absoluteCleanPath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

#ifdef DAW_ENABLE_COLLABORATION
QString canonicalCloudUuid(const QString& value) {
    const QUuid uuid(value.trimmed());
    return uuid.isNull()
               ? QString()
               : uuid.toString(QUuid::WithoutBraces).toLower();
}
#endif

/// A VLT project is a directory package on disk. macOS presents that package
/// as one document; other file managers may expose its inner Project.vlt file.
/// Normalize both entry points before the controller sees them.
QString packagePathFromSelection(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    const QString name = info.fileName();
    if (info.isFile() &&
        (name.compare(QString::fromLatin1(daw::ProjectSerializer::kProjectFile),
                      Qt::CaseInsensitive) == 0 ||
         name.compare(QStringLiteral("project.json"),
                      Qt::CaseInsensitive) == 0)) {
        return QDir(info.absolutePath()).absolutePath();
    }
    return absoluteCleanPath(path);
}

/// The detached mixer window: closing it docks the mixer again, so the panel
/// can never end up hidden in both places.
class MixerWindow : public QWidget {
public:
    MixerWindow(std::function<void()> onClose, QWidget* parent = nullptr)
        : QWidget(parent), m_onClose(std::move(onClose)) {
        setWindowTitle(QObject::tr("Mixer — %1")
                           .arg(QApplication::applicationDisplayName()));
        resize(900, 520);
    }

protected:
    void closeEvent(QCloseEvent* ev) override {
        if (m_onClose) m_onClose();
        ev->accept();
    }

private:
    std::function<void()> m_onClose;
};

/// Arrangement container: the track list + timeline fill it, while the mixer
/// floats on top as an overlay that can be dragged up over the tracks.
class ArrangementHost : public QWidget {
public:
    explicit ArrangementHost(QWidget* parent = nullptr) : QWidget(parent) {}
    std::function<void(const QSize&)> onResize;

protected:
    void resizeEvent(QResizeEvent* ev) override {
        if (onResize) onResize(ev->size());
        QWidget::resizeEvent(ev);
    }
};

/// The shell is intentionally horizontally compressible. Its children expose
/// large preferred/minimum hints (transport, inspector, two right panels), but
/// MainWindow must still receive a resize event so it can apply the explicit
/// Web-first/AI-second compression policy.
class MainShellHost final : public QWidget {
public:
    using QWidget::QWidget;

    QSize minimumSizeHint() const override {
        QSize hint = QWidget::minimumSizeHint();
        hint.setWidth(0);
        return hint;
    }
};

/// The workspace contains several deliberately wide headers. QVBoxLayout would
/// otherwise publish their full preferred width as an unshrinkable minimum to
/// the outer row, which lets fixed right panels overflow past the window edge.
/// MainWindow installs the one real minimum explicitly in
/// applyRightPanelWidths(): protected columns or the responsive transport,
/// whichever is wider.
class MainWorkspaceHost final : public QWidget {
public:
    using QWidget::QWidget;

    QSize minimumSizeHint() const override {
        QSize hint = QWidget::minimumSizeHint();
        hint.setWidth(0);
        return hint;
    }
};

/// "Which kind of folder?" — the one question that has to be asked before a
/// folder can be made, since the two kinds do completely different things to
/// the sound. Two cards rather than a combo box: the difference is worth a
/// sentence each, and this is where somebody meets it for the first time.
class FolderKindDialog : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(FolderKindDialog)
public:
    explicit FolderKindDialog(int trackCount, QWidget* parent)
        : QDialog(parent) {
        setWindowTitle(tr("New Folder"));
        setModal(true);

        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(20, 18, 20, 16);
        column->setSpacing(12);

        // Spelled out rather than left to %n: with no translator loaded Qt
        // hands back the source string as written, and "1 track(s)" is not
        // something to show anybody.
        const QString title =
            trackCount <= 0  ? tr("Create an empty folder")
            : trackCount == 1 ? tr("Put the selected track into a folder")
                              : tr("Put the %1 selected tracks into a folder")
                                    .arg(trackCount);
        auto* heading = new QLabel(title, this);
        heading->setObjectName("FolderDialogHeading");
        heading->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        column->addWidget(heading);

        const auto card = [&](const QString& title, const QString& blurb,
                              icons::Glyph glyph) {
            auto* button = new QPushButton(this);
            button->setObjectName("FolderCard");
            button->setCursor(Qt::PointingHandCursor);
            button->setMinimumHeight(88);
            // Any height the dialog has over its contents goes into the two
            // cards, which simply look roomier for it — better than a band of
            // dead space above the Cancel button.
            button->setSizePolicy(QSizePolicy::Preferred,
                                  QSizePolicy::MinimumExpanding);
            auto* row = new QHBoxLayout(button);
            row->setContentsMargins(14, 10, 14, 10);
            row->setSpacing(14);
            auto* mark = new QLabel(button);
            mark->setPixmap(icons::icon(glyph, th().accent, 26).pixmap(26, 26));
            mark->setFixedWidth(26);
            row->addWidget(mark, 0, Qt::AlignVCenter);
            auto* text = new QVBoxLayout;
            text->setContentsMargins(0, 0, 0, 0);
            text->setSpacing(3);
            auto* name = new QLabel(title, button);
            name->setObjectName("FolderCardTitle");
            auto* detail = new QLabel(blurb, button);
            detail->setObjectName("FolderCardBlurb");
            detail->setWordWrap(true);
            // Kept together and centred: with the card free to grow, a bare
            // stack of two labels drifts apart until the title and its
            // explanation stop reading as one thing.
            text->addStretch(1);
            text->addWidget(name);
            text->addWidget(detail);
            text->addStretch(1);
            row->addLayout(text, 1);
            column->addWidget(button);
            return button;
        };

        auto* plain = card(tr("Folder"),
                           tr("Groups the tracks so they can be collapsed, "
                              "moved and coloured together. The audio is "
                              "untouched — each track still goes where it "
                              "went before."),
                           icons::Glyph::Folder);
        auto* summing = card(tr("Summing Folder"),
                             tr("The same, plus a channel of its own: every "
                                "track inside is routed through it, so one "
                                "fader, one set of plugins and one mute "
                                "govern the whole group."),
                             icons::Glyph::FolderSum);

        auto* cancel = new QPushButton(tr("Cancel"), this);
        auto* footer = new QHBoxLayout;
        footer->addStretch(1);
        footer->addWidget(cancel);
        column->addLayout(footer);

        connect(plain, &QPushButton::clicked, this, [this] {
            m_summing = false;
            accept();
        });
        connect(summing, &QPushButton::clicked, this, [this] {
            m_summing = true;
            accept();
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

        const Theme& t = th();
        setStyleSheet(QString(R"(
QDialog { background: %SURFACE%; }
#FolderDialogHeading { color: %TEXT%; font-size: 13px; font-weight: 700; }
#FolderCard { background: %WELL%; border: 1px solid %SEP%; border-radius: 10px;
              text-align: left; }
#FolderCard:hover { border-color: %ACCENT%; background: %HOVER%; }
#FolderCardTitle { color: %TEXT%; font-size: 12px; font-weight: 700; }
#FolderCardBlurb { color: %TEXT2%; font-size: 11px; }
)")
            .replace("%SURFACE%", t.surface.name())
            .replace("%WELL%", t.well().name())
            .replace("%SEP%", t.separator().name())
            .replace("%ACCENT%", t.accent.name())
            .replace("%HOVER%", mixColors(t.well(), t.accent, 0.12).name())
            .replace("%TEXT2%", t.textSecondary.name())
            .replace("%TEXT%", t.textPrimary.name()));
        setFixedWidth(420);
    }

    bool summing() const { return m_summing; }

private:
    bool m_summing = false;
};

/// The decision after a supported audio download finishes. A small frameless
/// card gives the modal the same rounded plate as the right-hand panels while
/// retaining native buttons, Tab order, Enter and Escape behaviour.
class DownloadedAudioDialog final : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(DownloadedAudioDialog)
public:
    enum class Choice { None, NewTrack, SelectedTrack };

    DownloadedAudioDialog(const QString& path, double durationSeconds,
                          double sampleRate, int channels,
                          const QString& selectedTrackName,
                          QWidget* parent)
        : QDialog(parent) {
        setWindowTitle(tr("Import downloaded audio"));
        setModal(true);
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setFixedWidth(460);

        auto* shell = new QVBoxLayout(this);
        shell->setContentsMargins(10, 10, 10, 10);
        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("DownloadedAudioCard"));
        card->setAttribute(Qt::WA_StyledBackground, true);
        shell->addWidget(card);

        auto* column = new QVBoxLayout(card);
        column->setContentsMargins(20, 18, 20, 18);
        column->setSpacing(12);

        auto* heading = new QLabel(tr("Audio downloaded"), card);
        heading->setObjectName(QStringLiteral("DownloadedAudioHeading"));
        column->addWidget(heading);

        auto* fileRow = new QHBoxLayout;
        fileRow->setSpacing(12);
        auto* mark = new QLabel(card);
        mark->setPixmap(
            icons::icon(icons::Glyph::Waveform, th().audioAccent(), 28)
                .pixmap(28, 28));
        mark->setFixedWidth(30);
        fileRow->addWidget(mark, 0, Qt::AlignVCenter);
        auto* fileText = new QVBoxLayout;
        fileText->setSpacing(2);
        auto* name = new QLabel(QFileInfo(path).fileName(), card);
        name->setObjectName(QStringLiteral("DownloadedAudioName"));
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        name->setToolTip(path);
        const int minutes = int(durationSeconds) / 60;
        const int seconds = int(durationSeconds) % 60;
        const QString format = QFileInfo(path).suffix().isEmpty()
                                   ? tr("Audio")
                                   : QFileInfo(path).suffix().toUpper();
        auto* detail = new QLabel(
            tr("%1 · %2:%3 · %4 kHz · %5")
                .arg(format)
                .arg(minutes)
                .arg(seconds, 2, 10, QLatin1Char('0'))
                .arg(sampleRate / 1000.0, 0, 'g', 4)
                .arg(channels == 1 ? tr("mono")
                                   : tr("%1 channels").arg(channels)),
            card);
        detail->setObjectName(QStringLiteral("DownloadedAudioDetail"));
        fileText->addWidget(name);
        fileText->addWidget(detail);
        fileRow->addLayout(fileText, 1);
        column->addLayout(fileRow);

        auto* question = new QLabel(
            tr("Where should this file be placed at the current playhead?"),
            card);
        question->setWordWrap(true);
        question->setObjectName(QStringLiteral("DownloadedAudioQuestion"));
        column->addWidget(question);

        auto* analysisBox = new QFrame(card);
        analysisBox->setObjectName(QStringLiteral("DownloadedAudioAnalysis"));
        auto* analysisLayout = new QVBoxLayout(analysisBox);
        analysisLayout->setContentsMargins(12, 9, 12, 9);
        analysisLayout->setSpacing(5);
        auto* analysisTitle = new QLabel(tr("Analyze before import"), analysisBox);
        analysisTitle->setObjectName(QStringLiteral("DownloadedAudioAnalysisTitle"));
        m_detectTempo = new QCheckBox(tr("Detect BPM"), analysisBox);
        m_detectKey = new QCheckBox(tr("Detect key"), analysisBox);
        m_detectTempo->setChecked(false);
        m_detectKey->setChecked(false);
        analysisLayout->addWidget(analysisTitle);
        analysisLayout->addWidget(m_detectTempo);
        analysisLayout->addWidget(m_detectKey);
        column->addWidget(analysisBox);

        auto* newTrack = new QPushButton(tr("Add to New Audio Track"), card);
        newTrack->setObjectName(QStringLiteral("DownloadedAudioPrimary"));
        newTrack->setDefault(true);
        newTrack->setMinimumHeight(34);
        auto* selected = new QPushButton(
            selectedTrackName.isEmpty()
                ? tr("Add to Selected Audio Track")
                : tr("Add to Selected Track — %1").arg(selectedTrackName),
            card);
        selected->setObjectName(QStringLiteral("DownloadedAudioSelected"));
        selected->setMinimumHeight(34);
        selected->setEnabled(!selectedTrackName.isEmpty());
        if (selectedTrackName.isEmpty()) {
            selected->setToolTip(tr("Select an audio track to use this option"));
            selected->setAccessibleDescription(selected->toolTip());
        }
        auto* cancel = new QPushButton(tr("Do Not Import"), card);
        cancel->setMinimumHeight(30);

        column->addWidget(newTrack);
        column->addWidget(selected);
        column->addWidget(cancel);

        connect(newTrack, &QPushButton::clicked, this, [this] {
            m_choice = Choice::NewTrack;
            accept();
        });
        connect(selected, &QPushButton::clicked, this, [this] {
            m_choice = Choice::SelectedTrack;
            accept();
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

        const Theme& t = th();
        setStyleSheet(QString(R"(
#DownloadedAudioCard {
    background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 18px;
}
#DownloadedAudioHeading { color: %TEXT%; font-size: 15px; font-weight: 700; }
#DownloadedAudioName { color: %TEXT%; font-size: 12px; font-weight: 650; }
#DownloadedAudioDetail, #DownloadedAudioQuestion {
    color: %TEXT2%; font-size: 11px;
}
#DownloadedAudioAnalysis {
    background: %WELL%; border: 1px solid %BORDER%; border-radius: 10px;
}
#DownloadedAudioAnalysisTitle { color: %TEXT%; font-size: 11px; font-weight: 650; }
QCheckBox { color: %TEXT2%; font-size: 11px; spacing: 7px; }
#DownloadedAudioPrimary {
    color: white; background: %ACCENT%; border: 1px solid %ACCENT%;
    border-radius: 9px; font-weight: 650; padding: 6px 12px;
}
#DownloadedAudioPrimary:hover { background: %ACCENT_HI%; }
QPushButton { border-radius: 9px; padding: 6px 12px; }
QPushButton:disabled { color: %TEXT2%; background: %WELL%; }
)")
            .replace("%SURFACE%", t.surfaceElevated.name())
            .replace("%BORDER%", t.sectionDivider().name())
            .replace("%TEXT2%", t.textSecondary.name())
            .replace("%TEXT%", t.textPrimary.name())
            .replace("%ACCENT_HI%", t.accentHighlight.name())
            .replace("%ACCENT%", t.accent.name())
            .replace("%WELL%", t.well().name()));
    }

    Choice choice() const { return m_choice; }
    bool detectTempo() const { return m_detectTempo && m_detectTempo->isChecked(); }
    bool detectKey() const { return m_detectKey && m_detectKey->isChecked(); }

private:
    Choice m_choice = Choice::None;
    QCheckBox* m_detectTempo = nullptr;
    QCheckBox* m_detectKey = nullptr;
};

QString analysisPhaseText(std::string_view phase) {
    if (phase == "preparing") return QObject::tr("Preparing audio…");
    if (phase == "decoding") return QObject::tr("Reading audio…");
    if (phase == "tempo_features") return QObject::tr("Finding transients…");
    if (phase == "tempo_done") return QObject::tr("Checking tempo stability…");
    if (phase == "key_profiles") return QObject::tr("Finding the musical key…");
    if (phase == "key_done") return QObject::tr("Checking key confidence…");
    if (phase == "complete") return QObject::tr("Analysis complete");
    return QObject::tr("Analyzing audio…");
}

/// Decode and analyze on the global worker pool while a small modal reports
/// real progress. The nested event loop keeps painting and dispatching input;
/// no audio work ever runs on the GUI thread.
bool runMusicalAnalysis(QWidget* parent, const QString& path,
                        const daw::analysis::MusicalAnalysisRequest& request,
                        daw::analysis::MusicalAnalysisResult& result,
                        QString& error, bool& wasCancelled) {
    struct State {
        daw::analysis::MusicalAnalysisResult result;
        audio::Result outcome = audio::Result::fail(audio::EngineError::Unknown);
    };
    auto state = std::make_shared<State>();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    wasCancelled = false;

    QProgressDialog progress(QObject::tr("Reading audio…"), QObject::tr("Cancel"),
                             0, 1000, parent);
    progress.setWindowTitle(QObject::tr("Audio analysis"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setValue(0);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress,
                     [cancelled, &progress] {
                         cancelled->store(true, std::memory_order_relaxed);
                         progress.setLabelText(QObject::tr("Cancelling…"));
                     });

    QEventLoop wait;
    QPointer<QProgressDialog> progressGuard(&progress);
    QObject* dispatcher = QCoreApplication::instance();
    QThreadPool::globalInstance()->start(
        [state, cancelled, dispatcher, progressGuard, path, request, &wait] {
            state->outcome = daw::analysis::analyzeAudioFile(
                path.toStdString(), request, state->result,
                [cancelled, dispatcher, progressGuard](double amount,
                                                       std::string_view phase) {
                    if (cancelled->load(std::memory_order_relaxed)) return false;
                    const int value = std::clamp(int(std::lround(amount * 1000.0)),
                                                 0, 1000);
                    const QString label = analysisPhaseText(phase);
                    QMetaObject::invokeMethod(
                        dispatcher,
                        [progressGuard, value, label] {
                            if (!progressGuard) return;
                            progressGuard->setLabelText(label);
                            progressGuard->setValue(value);
                        },
                        Qt::QueuedConnection);
                    return !cancelled->load(std::memory_order_relaxed);
                });
            QMetaObject::invokeMethod(dispatcher, [&wait] { wait.quit(); },
                                      Qt::QueuedConnection);
        });
    progress.show();
    wait.exec();
    progress.hide();

    wasCancelled = cancelled->load(std::memory_order_relaxed);
    if (!state->outcome) {
        error = QString::fromStdString(state->outcome.message());
        return false;
    }
    result = std::move(state->result);
    return true;
}

class AudioAnalysisResultDialog final : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(AudioAnalysisResultDialog)
public:
    AudioAnalysisResultDialog(
        const daw::analysis::MusicalAnalysisResult& result,
        const daw::analysis::MusicalAnalysisRequest& request,
        double currentTempo, bool importing, bool autoApplyTempo,
        QWidget* parent)
        : QDialog(parent) {
        setWindowTitle(tr("Audio analysis result"));
        setModal(true);
        setMinimumWidth(390);
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(20, 18, 20, 18);
        column->setSpacing(11);

        auto* heading = new QLabel(tr("Analysis complete"), this);
        heading->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700;"));
        column->addWidget(heading);

        if (request.detectTempo &&
            result.tempo.status != daw::analysis::DetectionStatus::Unavailable) {
            auto* tempoTitle = new QLabel(
                tr("Detected tempo · %1% confidence")
                    .arg(int(std::lround(result.tempo.confidence * 100.0))), this);
            column->addWidget(tempoTitle);
            m_tempo = new QComboBox(this);
            std::vector<double> candidates{result.tempo.bpm};
            candidates.insert(candidates.end(), result.tempo.alternatives.begin(),
                              result.tempo.alternatives.end());
            for (double bpm : candidates) {
                if (bpm <= 0.0) continue;
                bool duplicate = false;
                for (int i = 0; i < m_tempo->count(); ++i)
                    duplicate = duplicate ||
                        std::abs(m_tempo->itemData(i).toDouble() - bpm) < 0.2;
                if (!duplicate)
                    m_tempo->addItem(tr("%1 BPM").arg(bpm, 0, 'f', 1), bpm);
            }
            column->addWidget(m_tempo);
            m_applyTempo = new QCheckBox(
                tr("Set project tempo (currently %1 BPM)")
                    .arg(currentTempo, 0, 'f', 1), this);
            m_applyTempo->setChecked(autoApplyTempo && m_tempo->count() > 0);
            column->addWidget(m_applyTempo);
            if (!result.tempo.highConfidence()) {
                auto* ambiguity = new QLabel(
                    result.tempo.variable
                        ? tr("The tempo changes across this clip. Nothing will be "
                             "applied unless you choose it.")
                        : tr("The meter is ambiguous. Check the BPM before applying it."),
                    this);
                ambiguity->setWordWrap(true);
                ambiguity->setStyleSheet(QStringLiteral("color: palette(mid);"));
                column->addWidget(ambiguity);
            }
        } else if (request.detectTempo) {
            auto* tempoMissing = new QLabel(tr("BPM · not enough rhythmic information"),
                                            this);
            column->addWidget(tempoMissing);
        }

        if (request.detectKey &&
            result.key.status != daw::analysis::DetectionStatus::Unavailable) {
            const QString key = QString::fromStdString(
                daw::analysis::keyDisplayName(result.key));
            const QString camelot = QString::fromStdString(
                daw::analysis::camelotName(result.key.root, result.key.scale));
            auto* keyLabel = new QLabel(
                tr("Key · %1 · Camelot %2 · %3% confidence")
                    .arg(key, camelot)
                    .arg(int(std::lround(result.key.confidence * 100.0))), this);
            keyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            column->addWidget(keyLabel);
        } else if (request.detectKey) {
            column->addWidget(new QLabel(tr("Key · not enough tonal information"),
                                         this));
        }

        auto* buttons = new QDialogButtonBox(
            importing ? QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                      : QDialogButtonBox::Ok,
            this);
        if (auto* ok = buttons->button(QDialogButtonBox::Ok))
            ok->setText(importing ? tr("Import") : tr("Done"));
        if (auto* cancel = buttons->button(QDialogButtonBox::Cancel))
            cancel->setText(tr("Keep File Only"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        column->addWidget(buttons);
    }

    bool appliesTempo() const {
        return m_applyTempo && m_applyTempo->isChecked() && m_tempo &&
               m_tempo->currentIndex() >= 0;
    }
    double selectedTempo() const {
        return m_tempo ? m_tempo->currentData().toDouble() : 0.0;
    }

private:
    QComboBox* m_tempo = nullptr;
    QCheckBox* m_applyTempo = nullptr;
};

#ifdef DAW_ENABLE_COLLABORATION
QString presenceContextId(const QString& ownerId, const QString& objectId) {
    return collab::safeSemanticId(ownerId + QLatin1Char(':') + objectId);
}

collab::SurfaceAddress presenceAddress(collab::SurfaceKind kind,
                                       const QString& instanceId,
                                       const QString& ownerId,
                                       const QString& objectId) {
    return {kind, collab::safeSemanticId(instanceId),
            presenceContextId(ownerId, objectId)};
}

bool presenceContextMatches(const collab::SemanticPoint& point,
                            const QString& ownerId,
                            const QString& objectId) {
    return point.trackId == collab::safeSemanticId(ownerId) &&
           point.clipId == collab::safeSemanticId(objectId);
}
#endif

} // namespace

#ifdef DAW_ENABLE_COLLABORATION
struct MainWindow::PublicationUiState {
    std::unique_ptr<daw::cloud::CloudPublicationCapture> capture;
    QString sourcePath;
    quint64 sourceRevision = 0;
    quint64 generation = 0;
    bool active = false;
};

struct MainWindow::CloudRecordingRuntime {
    enum class Phase { Acquiring, CountingIn, Capturing, FinalizedWriteFailed };

    Phase phase = Phase::Acquiring;
    quint64 generation = 0;
    QString projectId;
    QString sessionId;
    QString runId;
    QString opId;
    QString transactionId;
    qint64 createdAtUnixMs = 0;
    std::vector<std::string> trackIds;
    int countInBeats = 0;
    QHash<QString, QString> leaseIdByTrack;
    QHash<QString, QString> captureIdByTrack;
    QHash<QString, QString> uploadIdByTrack;
    QHash<QString, QString> assetIdByTrack;
    bool recoveryOnly = true;
    bool lostLease = false;
    daw::EngineController::FinalizedRecordingRun finalized;
};
#endif

MainWindow::MainWindow(bool openDevice, QWidget* parent,
                       collab::CollaborationService* collaboration)
    : QMainWindow(parent), m_collaboration(collaboration) {
    const ui::AudioPreferences audioPreferences = openDevice
        ? ui::loadAudioPreferences()
        : ui::AudioPreferences{};
    auto audioStarted = m_controller.initialize(audioPreferences.config,
                                                 openDevice);
    if (!audioStarted && openDevice) {
        audio::AudioDeviceConfig fallback;
        fallback.sampleRate = 48000.0;
        fallback.bufferSize = 512;
        fallback.inputEnabled = false;
        const auto fallbackStarted =
            m_controller.applyAudioConfiguration(fallback);
        const QString message = fallbackStarted
            ? tr("Saved audio device could not be opened; using the system "
                 "output. Your saved choice was kept.")
            : tr("No audio device could be opened. Choose one in Audio "
                 "Settings and press Apply.");
        QTimer::singleShot(0, this, [this, message] {
            statusBar()->showMessage(message, 12000);
        });
    }

    // A plugin about to be destroyed may have its editor open in one of our
    // windows. The controller cannot know that; we cannot know when a plugin
    // goes. This is where the two meet.
    m_controller.setPluginRetiringCallback(
        [this](const std::string& channelId, const std::string& slotId) {
            retirePluginEditor(QString::fromStdString(channelId),
                               QString::fromStdString(slotId));
        });

    using Mode = daw::EngineController::PlaybackMode;
    const int stored =
        QSettings().value(ui::kPlaybackModeSetting, int(Mode::Resume)).toInt();
    m_controller.setPlaybackMode(Mode(std::clamp(stored, 0, int(Mode::Restart))));
    const QString metronomeSample =
        QSettings().value(ui::kMetronomeSampleSetting).toString();
    if (!metronomeSample.isEmpty() &&
        !m_controller.setMetronomeSample(metronomeSample.toStdString())) {
        // A moved or deleted preference must not leave a permanently broken
        // selection: fall back to the built-in knock on this and later runs.
        QSettings().remove(ui::kMetronomeSampleSetting);
    }

    // Before any widget is built, so the transport's Layers button and the
    // track chips come up showing the restored mode rather than the default.
    RecordingSettingsPage::restore(m_controller);

    m_shortcuts = new ShortcutManager(this);
    // Held-key gestures reach us wherever focus is; see eventFilter.
    qApp->installEventFilter(this);

    // The typing keyboard is built before the widgets so the transport button
    // and the menu item can both be wired to it as they are created. Its filter
    // goes on after this window's: the two never want the same key, and the
    // gesture filter above must keep seeing everything.
    m_typingKeyboard = new TypingKeyboard(&m_controller, this);
    m_typingKeyboard->setTargetProvider(
        [this] { return liveInputTarget(); });
    connect(m_typingKeyboard, &TypingKeyboard::noteStateChanged, this,
            &MainWindow::onLiveNoteStateChanged);
    qApp->installEventFilter(m_typingKeyboard);

    m_midiInput = new MidiInputManager(&m_controller, this);
    m_midiInput->setTargetProvider([this] { return liveInputTarget(); });
    connect(m_midiInput, &MidiInputManager::noteStateChanged, this,
            &MainWindow::onLiveNoteStateChanged);
    connect(&m_selection, &ui::SelectionModel::changed, m_midiInput,
            &MidiInputManager::refreshTarget);

    buildLayout();
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration) {
        m_presenceInput = new collab::PresenceInputRouter(this);
        connect(m_presenceInput, &collab::PresenceInputRouter::presenceReady,
                m_collaboration, &collab::CollaborationService::sendPresence);

        const auto registerSurface = [this](
            QWidget* widget, collab::SurfaceAddress address,
            collab::PresencePolicy policy) {
            if (!widget) return;
            collab::PresenceSurfaceRegistration registration;
            registration.address = address;
            registration.policy = policy;
            m_presenceInput->registerSurface(widget, std::move(registration));
            if (policy == collab::PresencePolicy::Exact)
                new collab::PresenceOverlay(
                    widget, address, m_collaboration->presenceStore());
        };

        collab::PresenceSurfaceRegistration timelineRegistration;
        timelineRegistration.address = {collab::SurfaceKind::Timeline,
                                        QStringLiteral("arrangement"), {}};
        timelineRegistration.policy = collab::PresencePolicy::Exact;
        timelineRegistration.encode = [this](const QPointF& position) {
            return std::optional<collab::SemanticPoint>(
                m_timeline->collaborationPresenceAt(position));
        };
        m_presenceInput->registerSurface(m_timeline, timelineRegistration);
        m_timelinePresence = new collab::PresenceOverlay(
            m_timeline, timelineRegistration.address,
            m_collaboration->presenceStore());
        m_timelinePresence->setPointMapper(
            [this](const collab::SemanticPoint& point, const QSize&) {
                return m_timeline->collaborationPositionFor(point);
            });

        registerSurface(m_transport,
                        {collab::SurfaceKind::Transport,
                         QStringLiteral("main"), {}},
                        collab::PresencePolicy::Exact);
        // The track list is described per track, not as a fraction of the
        // column: two participants scrolled differently, or with different row
        // heights, otherwise see each other's pointer on the wrong track.
        collab::PresenceSurfaceRegistration trackListRegistration;
        trackListRegistration.address = {collab::SurfaceKind::TrackList,
                                         QStringLiteral("main"), {}};
        trackListRegistration.policy = collab::PresencePolicy::Exact;
        trackListRegistration.encode =
            [this](const QPointF& position) -> std::optional<collab::SemanticPoint> {
                return m_trackList->collaborationPresenceAt(position);
            };
        m_presenceInput->registerSurface(m_trackList, trackListRegistration);
        m_trackListPresence = new collab::PresenceOverlay(
            m_trackList, trackListRegistration.address,
            m_collaboration->presenceStore());
        m_trackListPresence->setPointMapper(
            [this](const collab::SemanticPoint& point, const QSize&) {
                return m_trackList->collaborationPositionFor(point);
            });
        // Same reasoning as the track list: the console scrolls, so a fraction
        // of the widget points at a different channel on every screen.
        collab::PresenceSurfaceRegistration mixerRegistration;
        mixerRegistration.address = {collab::SurfaceKind::Mixer,
                                     QStringLiteral("main"), {}};
        mixerRegistration.policy = collab::PresencePolicy::Exact;
        mixerRegistration.encode =
            [this](const QPointF& position) -> std::optional<collab::SemanticPoint> {
                return m_mixer->collaborationPresenceAt(position);
            };
        m_presenceInput->registerSurface(m_mixer, mixerRegistration);
        m_mixerPresence = new collab::PresenceOverlay(
            m_mixer, mixerRegistration.address,
            m_collaboration->presenceStore());
        m_mixerPresence->setPointMapper(
            [this](const collab::SemanticPoint& point, const QSize&) {
                return m_mixer->collaborationPositionFor(point);
            });
        registerSurface(m_browser,
                        {collab::SurfaceKind::Browser,
                         QStringLiteral("files"), {}},
                        collab::PresencePolicy::Coarse);
        registerSurface(m_webContainer,
                        {collab::SurfaceKind::Web,
                         QStringLiteral("browser"), {}},
                        collab::PresencePolicy::Coarse);
        registerSurface(m_aiPanel,
                        {collab::SurfaceKind::Ai,
                         QStringLiteral("assistant"), {}},
                        collab::PresencePolicy::Coarse);
        registerSurface(m_workspace,
                        {collab::SurfaceKind::Shell,
                         QStringLiteral("workspace"), {}},
                        collab::PresencePolicy::Coarse);

        // BPM is a reviewed single-value semantic control. All other line
        // edits/spin boxes remain hidden from presence by default.
        if (auto* tempo = m_transport->findChild<QLineEdit*>(
                QStringLiteral("TempoField"))) {
            tempo->setProperty("collaborationPresenceSafeControl", true);
            tempo->setProperty("collaborationPresenceTarget",
                               QStringLiteral("transport.bpm"));
        }
        m_presenceInput->setEnabled(m_collaboration->isOnline());
        connect(m_collaboration, &collab::CollaborationService::stateChanged,
                m_presenceInput,
                [this](collab::CollaborationState state, const QString&) {
                    m_presenceInput->setEnabled(
                        state == collab::CollaborationState::Synced ||
                        state == collab::CollaborationState::ReadOnly);
                });
        connect(m_collaboration->localSessionState(),
                &collab::LocalSessionState::remoteTransportAccepted, this,
                &MainWindow::applySessionTransport);
        connect(&m_selection, &ui::SelectionModel::changed, m_presenceInput,
                [this] {
                    QStringList ids;
                    if (!m_selection.clips().isEmpty()) {
                        for (const ui::ClipSel& clip : m_selection.clips())
                            ids.push_back(clip.clipId);
                    } else {
                        ids = m_selection.tracks();
                    }
                    m_presenceInput->publishSelection(
                        {collab::SurfaceKind::Timeline,
                         QStringLiteral("arrangement"), {}},
                        ids);
                });
    }
#endif
    buildMenus();
    buildSemanticCommands();
    buildStatusBar();

    syncViews();

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshUi);
    m_refreshTimer->start(33);

    // Cursor movement is a pair of narrow dirty strips and can follow the
    // display without also doubling meter aggregation and plugin polling.
    m_playheadTimer = new QTimer(this);
    m_playheadTimer->setTimerType(Qt::PreciseTimer);
    m_playheadTimer->setInterval(16);
    connect(m_playheadTimer, &QTimer::timeout, this,
            &MainWindow::refreshPlayheadFrame);

    // 8 ms: the count-in decides where the take begins, and the 33 ms refresh
    // would put the punch point up to a frame late — audible against a click.
    m_countInTimer = new QTimer(this);
    m_countInTimer->setTimerType(Qt::PreciseTimer);
    m_countInTimer->setInterval(8);
    connect(m_countInTimer, &QTimer::timeout, this, &MainWindow::tickCountIn);

    // The browser starts where it was left; the View action was created with
    // the same value, so this only has to apply it.
    setBrowserVisible(ui::browserprefs::visible());
    setWebVisible(ui::webprefs::visible());
    setAiVisible(ui::aiprefs::visible());

    // Reopen where it was left. Applied last, after every child has had its say
    // about the layout, or the restore fights the initial sizing. Skipped when
    // there is no device — that is the headless screenshot/selftest path, which
    // sets its own size and must not depend on what a person's window happened
    // to be doing.
    if (openDevice) {
        const QByteArray geometry =
            QSettings().value(ui::kMainGeometrySetting).toByteArray();
        if (!geometry.isEmpty()) restoreGeometry(geometry);
    }
    m_persistGeometry = openDevice;

    // Headless recovery tests use a disposable DAW_RECOVERY_ROOT and never
    // show a prompt. Live recovery is deliberately started by main() only
    // after this window is visible and the modal startup dialog is gone.
    if (!openDevice) startRecovery(/*interactive=*/false);

    updateWindowTitle();
}

void MainWindow::completeInteractiveStartup() {
    startRecovery(/*interactive=*/true);
}

#ifdef DAW_ENABLE_COLLABORATION
void MainWindow::setCloudSharedAssetMutationBridge(
    collab::CloudSharedAssetMutationBridge* bridge) {
    if (m_cloudSharedAssetMutationBridge)
        disconnect(m_cloudSharedAssetMutationBridge, nullptr, this, nullptr);
    m_cloudSharedAssetMutationBridge = bridge;

    const auto updateActions = [this](bool active) {
        if (m_cancelAssetMutationAction)
            m_cancelAssetMutationAction->setEnabled(active);
        if (m_retryAssetMutationAction)
            m_retryAssetMutationAction->setEnabled(false);
    };
    updateActions(bridge && bridge->active());
    if (!bridge) return;

    connect(bridge, &collab::CloudSharedAssetMutationBridge::activeChanged,
            this, updateActions);
    connect(bridge, &collab::CloudSharedAssetMutationBridge::progressChanged,
            this, [this](qsizetype complete, qsizetype total) {
                statusBar()->showMessage(
                    tr("Preparing shared asset… %1/%2")
                        .arg(complete)
                        .arg(total));
            });
    connect(bridge, &collab::CloudSharedAssetMutationBridge::failed, this,
            [this](const QString& message, bool retryable) {
                if (m_retryAssetMutationAction)
                    m_retryAssetMutationAction->setEnabled(retryable);
                if (m_cancelAssetMutationAction)
                    m_cancelAssetMutationAction->setEnabled(true);
                statusBar()->showMessage(
                    retryable
                        ? tr("%1 Use Session > Retry Asset Upload.")
                              .arg(message)
                        : message,
                    10000);
            });
    connect(bridge, &collab::CloudSharedAssetMutationBridge::completed, this,
            [this](bool submitted) {
                statusBar()->showMessage(
                    submitted
                        ? tr("Asset verified; shared change submitted.")
                        : tr("Asset verified, but the project changed before "
                             "the shared command could be submitted."),
                    7000);
            });
    connect(bridge, &collab::CloudSharedAssetMutationBridge::cancelled, this,
            [this] {
                statusBar()->showMessage(tr("Shared asset upload cancelled."),
                                         5000);
            });
}

void MainWindow::setCloudPublicationServices(
    collab::CloudProjectPublisher* publisher,
    collab::CloudProjectSyncCoordinator* synchronizer,
    collab::CloudSessionLifecycleController* sessionLifecycle,
    collab::CollaborationCommandBridge* commandBridge,
    collab::CloudProjectClient* projectClient,
    collab::RecordingLeaseCoordinator* recordingLeases,
    collab::CloudProjectAssetHydrator* assetHydrator,
    collab::AssetCache* assetCache) {
    if (m_cloudPublisher)
        disconnect(m_cloudPublisher, nullptr, this, nullptr);
    if (m_cloudProjectSync)
        disconnect(m_cloudProjectSync, nullptr, this, nullptr);
    if (m_cloudSessionLifecycle)
        disconnect(m_cloudSessionLifecycle, nullptr, this, nullptr);
    if (m_collaborationCommandBridge)
        disconnect(m_collaborationCommandBridge, nullptr, this, nullptr);
    if (m_cloudProjectClient)
        disconnect(m_cloudProjectClient, nullptr, this, nullptr);
    if (m_recordingLeases)
        disconnect(m_recordingLeases, nullptr, this, nullptr);
    if (m_cloudAssetHydrator)
        disconnect(m_cloudAssetHydrator, nullptr, this, nullptr);

    m_cloudPublisher = publisher;
    m_cloudProjectSync = synchronizer;
    m_cloudSessionLifecycle = sessionLifecycle;
    m_collaborationCommandBridge = commandBridge;
    m_cloudProjectClient = projectClient;
    m_recordingLeases = recordingLeases;
    m_cloudAssetHydrator = assetHydrator;
    m_collaborationAssetCache = assetCache;
    if (!m_publicationUi)
        m_publicationUi = std::make_unique<PublicationUiState>();

    if (m_cloudPublisher) {
        connect(m_cloudPublisher,
                &collab::CloudProjectPublisher::phaseChanged, this,
                [this](quint64 generation,
                       collab::CloudPublicationPhase phase) {
                    if (!m_publicationUi || !m_publicationUi->active ||
                        generation != m_publicationUi->generation)
                        return;
                    QString message;
                    switch (phase) {
                        case collab::CloudPublicationPhase::Preflight:
                            message = tr("Checking project for cloud publication…");
                            break;
                        case collab::CloudPublicationPhase::CreatingProject:
                            message = tr("Creating cloud project…");
                            break;
                        case collab::CloudPublicationPhase::UploadingAssets:
                            message = tr("Uploading project audio and plugin state…");
                            break;
                        case collab::CloudPublicationPhase::PreparingSnapshot:
                            message = tr("Preparing canonical cloud snapshot…");
                            break;
                        case collab::CloudPublicationPhase::UploadingSnapshot:
                            message = tr("Uploading cloud snapshot…");
                            break;
                        case collab::CloudPublicationPhase::Activating:
                            message = tr("Activating cloud project…");
                            break;
                        case collab::CloudPublicationPhase::Idle:
                        case collab::CloudPublicationPhase::Completed:
                        case collab::CloudPublicationPhase::Failed:
                        case collab::CloudPublicationPhase::Cancelled:
                            break;
                    }
                    if (!message.isEmpty()) statusBar()->showMessage(message);
                });
        connect(m_cloudPublisher,
                &collab::CloudProjectPublisher::progressChanged, this,
                [this](quint64 generation, int complete, int total) {
                    if (!m_publicationUi || !m_publicationUi->active ||
                        generation != m_publicationUi->generation || total <= 0)
                        return;
                    statusBar()->showMessage(
                        tr("Publishing project to cloud… %1/%2")
                            .arg(complete)
                            .arg(total));
                });
        connect(m_cloudPublisher,
                &collab::CloudProjectPublisher::publicationFailed, this,
                [this](quint64 generation, collab::CloudPublicationPhase,
                       const QString&, const QString& safeMessage,
                       bool retryable) {
                    if (!m_publicationUi || !m_publicationUi->active ||
                        generation != m_publicationUi->generation)
                        return;
                    m_publicationUi->active = false;
                    m_publicationUi->capture.reset();
                    m_publicationUi->sourcePath.clear();
                    updateCloudPublicationAction();
                    updateWindowTitle();
                    statusBar()->showMessage(tr("Cloud publication failed"),
                                             6000);
                    QString detail = safeMessage.trimmed();
                    if (detail.isEmpty())
                        detail = tr("The cloud service could not publish this project.");
                    if (detail.size() > 400) detail = detail.left(400);
                    if (retryable)
                        detail += tr("\n\nYou can try publishing again.");
                    QMessageBox::warning(this, tr("Publish Project Failed"),
                                         detail);
                });
        connect(m_cloudPublisher,
                &collab::CloudProjectPublisher::publicationCancelled, this,
                [this](quint64 generation, const QString&) {
                    if (!m_publicationUi || !m_publicationUi->active ||
                        generation != m_publicationUi->generation)
                        return;
                    m_publicationUi->active = false;
                    m_publicationUi->capture.reset();
                    m_publicationUi->sourcePath.clear();
                    updateCloudPublicationAction();
                    updateWindowTitle();
                    statusBar()->showMessage(tr("Cloud publication cancelled"),
                                             4000);
                });
        connect(m_cloudPublisher,
                &collab::CloudProjectPublisher::publicationCompleted, this,
                [this](quint64 generation,
                       const collab::CloudProjectPublicationResult& result) {
                    if (!m_publicationUi || !m_publicationUi->active ||
                        generation != m_publicationUi->generation)
                        return;

                    const QString sourcePath = m_publicationUi->sourcePath;
                    const quint64 sourceRevision =
                        m_publicationUi->sourceRevision;
                    m_publicationUi->active = false;
                    m_publicationUi->capture.reset();
                    m_publicationUi->sourcePath.clear();

                    const QString projectId =
                        result.project.project.id.trimmed();
                    const bool sameDocument =
                        sourceRevision == m_projectRevision && !m_dirty &&
                        !sourcePath.isEmpty() && !m_projectPath.isEmpty() &&
                        absoluteCleanPath(sourcePath) ==
                            absoluteCleanPath(m_projectPath);
                    if (projectId.isEmpty() || !sameDocument) {
                        updateCloudPublicationAction();
                        updateWindowTitle();
                        statusBar()->showMessage(
                            tr("Cloud copy published; local project left open"),
                            7000);
                        QMessageBox::information(
                            this, tr("Cloud Copy Published"),
                            tr("The cloud copy was created, but this local project "
                               "changed while files were uploading. VLT kept the "
                               "current local document open and did not replace "
                               "those newer edits."));
                        return;
                    }

                    // The package that was explicitly saved before publication
                    // becomes the protected backup only after the downloaded
                    // cloud snapshot has been verified and installed.
                    m_candidateCloudProjectId = projectId;
                    m_candidateCloudBackupPath = sourcePath;

                    statusBar()->showMessage(
                        tr("Published. Opening the verified cloud copy…"));
                    if (!openCloudProject(projectId, sourcePath)) {
                        QMessageBox::warning(
                            this, tr("Cloud Project Published"),
                            tr("The project was published, but VLT could not "
                               "start opening the cloud copy. Your original "
                               "local project remains preserved as a backup."));
                    }
                });
    }

    if (m_cloudProjectSync) {
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::phaseChanged, this,
                [this](collab::CloudSyncPhase) {
                    updateCloudSessionActions();
                    handleCloudRecordingContextChange();
                });
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::synchronizedProject,
                this, [this](const QString& projectId, quint64,
                             const QString&, collab::CloudProjectRole,
                             collab::CloudProjectStatus) {
                    if (projectId == m_candidateCloudProjectId) {
                        m_cloudProjectId = projectId;
                        m_cloudLocalBackupPath = m_candidateCloudBackupPath;
                        m_candidateCloudProjectId.clear();
                        m_candidateCloudBackupPath.clear();
                        if (m_cloudSessionLifecycle)
                            m_cloudSessionLifecycle->bindProject(projectId);
                        m_projectPath.clear();
                        m_dirty = false;
                        m_journal.setProjectPath({}, {});
                        m_journalStale = true;
                        persistCloudBinding();
                        updateCloudPublicationAction();
                        updateWindowTitle();
                    } else if (projectId != m_cloudProjectId) {
                        return;
                    }
                    updateCloudSessionActions();
                    statusBar()->showMessage(tr("Cloud project is ready"),
                                             5000);
                });
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::noActiveSession, this,
                [this](const QString& projectId) {
                    if (projectId != m_cloudProjectId) return;
                    statusBar()->showMessage(
                        tr("Cloud project opened read-only; no live session is running"),
                        7000);
                });
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::synchronizationFailed,
                this, [this](quint64 generation,
                             const collab::CloudSyncError& error) {
                    if (!m_cloudProjectSync ||
                        generation != m_cloudProjectSync->generation())
                        return;
                    m_candidateCloudProjectId.clear();
                    m_candidateCloudBackupPath.clear();
                    QString detail = error.safeMessage.trimmed();
                    if (detail.isEmpty())
                        detail = tr("The verified cloud copy could not be opened.");
                    if (detail.size() > 400) detail = detail.left(400);
                    QMessageBox::warning(
                        this, tr("Open Cloud Project Failed"),
                        tr("The cloud project was not opened. The current "
                           "project was preserved.\n\n%1")
                            .arg(detail));
                    statusBar()->showMessage(
                        tr("Cloud project could not be opened"), 6000);
                });
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::snapshotUploadStarted,
                this, [this](const QString&, int attempt, quint64) {
                    statusBar()->showMessage(
                        tr("Saving collaboration snapshot… attempt %1")
                            .arg(attempt));
                });
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::snapshotUploadCompleted,
                this, [this](const QString&, quint64) {
                    statusBar()->showMessage(
                        tr("Collaboration snapshot saved"), 5000);
                });
        connect(m_cloudProjectSync,
                &collab::CloudProjectSyncCoordinator::snapshotUploadFailed,
                this, [this](const QString&, quint64,
                             const QString& message, bool) {
                    statusBar()->showMessage(
                        message.isEmpty()
                            ? tr("Collaboration snapshot could not be saved")
                            : message.left(320),
                        7000);
                });
    }

    if (m_cloudAssetHydrator) {
        connect(m_cloudAssetHydrator,
                &collab::CloudProjectAssetHydrator::progressChanged,
                this, [this](qsizetype complete, qsizetype total) {
                    if (total > 0 && complete < total)
                        statusBar()->showMessage(
                            tr("Downloading cloud assets… %1/%2")
                                .arg(complete)
                                .arg(total));
                });
        connect(m_cloudAssetHydrator,
                &collab::CloudProjectAssetHydrator::assetUnavailable,
                this, [this](const QString&, const QString& message, bool) {
                    statusBar()->showMessage(
                        message.isEmpty() ? tr("A cloud asset is unavailable")
                                          : message.left(320),
                        7000);
                });
    }

    if (m_cloudSessionLifecycle) {
        connect(m_cloudSessionLifecycle,
                &collab::CloudSessionLifecycleController::phaseChanged, this,
                [this](collab::CloudSessionLifecyclePhase) {
                    updateCloudSessionActions();
                    handleCloudRecordingContextChange();
                });
        connect(m_cloudSessionLifecycle,
                &collab::CloudSessionLifecycleController::capabilitiesChanged,
                this, [this](bool, bool, bool) {
                    updateCloudSessionActions();
                });
        connect(m_cloudSessionLifecycle,
                &collab::CloudSessionLifecycleController::userNotice, this,
                [this](const QString& safeMessage, bool error) {
                    const QString message = safeMessage.left(320);
                    statusBar()->showMessage(message, error ? 7000 : 5000);
                    if (error)
                        QMessageBox::warning(this, tr("Collaboration Session"),
                                             message);
                });
        if (!m_cloudProjectId.isEmpty())
            m_cloudSessionLifecycle->bindProject(m_cloudProjectId);
    }

    if (m_collaborationCommandBridge) {
        connect(m_collaborationCommandBridge,
                &collab::CollaborationCommandBridge::mutationBlocked, this,
                [this](const QString& safeMessage) {
                    statusBar()->showMessage(safeMessage.left(320), 6000);
                });
    }

    if (m_cloudProjectClient) {
        connect(m_cloudProjectClient, &QObject::destroyed, this, [this] {
            updateCloudSessionActions();
            handleCloudRecordingContextChange();
        });
    }

    if (m_recordingLeases) {
        connect(m_recordingLeases,
                &collab::RecordingLeaseCoordinator::leasesAcquired, this,
                [this](const QVector<collab::CloudProjectTrackLease>&) {
                    handleRecordingLeasesAcquired();
                });
        connect(m_recordingLeases,
                &collab::RecordingLeaseCoordinator::acquisitionFailed, this,
                [this](const collab::CloudClientError&) {
                    if (!m_cloudRecording ||
                        m_cloudRecording->phase !=
                            CloudRecordingRuntime::Phase::Acquiring)
                        return;
                    m_cloudRecording.reset();
                    statusBar()->showMessage(
                        tr("Recording could not reserve every selected track"),
                        6000);
                    if (m_contextPanel) m_contextPanel->refresh();
                });
        connect(m_recordingLeases,
                &collab::RecordingLeaseCoordinator::leaseLost, this,
                [this](const collab::CloudClientError&,
                       const QVector<collab::CloudProjectTrackLease>&) {
                    if (!m_cloudRecording) return;
                    const auto phase = m_cloudRecording->phase;
                    if (phase == CloudRecordingRuntime::Phase::Capturing ||
                        phase ==
                            CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
                        m_cloudRecording->lostLease = true;
                        m_cloudRecording->recoveryOnly = true;
                        statusBar()->showMessage(
                            tr("Connection changed; this take will be kept for recovery"),
                            7000);
                        return;
                    }
                    cancelPendingCloudRecording(
                        tr("Recording cancelled because its track reservation was lost"));
                });
        connect(m_recordingLeases, &QObject::destroyed, this, [this] {
            m_recordingLeases = nullptr;
            handleCloudRecordingContextChange();
        });
    }

    if (m_collaboration) {
        connect(m_collaboration, &collab::CollaborationService::stateChanged,
                this, &MainWindow::handleCloudRecordingContextChange,
                Qt::UniqueConnection);
        connect(m_collaboration, &collab::CollaborationService::projectChanged,
                this, &MainWindow::handleCloudRecordingContextChange,
                Qt::UniqueConnection);
        connect(m_collaboration,
                &collab::CollaborationService::roomIdentityChanged, this,
                &MainWindow::handleCloudRecordingContextChange,
                Qt::UniqueConnection);
        connect(m_collaboration,
                &collab::CollaborationService::liveSessionEnding, this,
                &MainWindow::handleCloudRecordingContextChange,
                Qt::UniqueConnection);
        connect(m_collaboration,
                &collab::CollaborationService::liveSessionEnded, this,
                &MainWindow::handleCloudRecordingContextChange,
                Qt::UniqueConnection);
    }

    updateCloudPublicationAction();
    updateCloudSessionActions();
    QTimer::singleShot(0, this, &MainWindow::restoreCloudBinding);
}

bool MainWindow::cloudRecordingContextIsWritable(
    const std::vector<std::string>& targets) const {
    if (!m_recordingLeases || !m_cloudProjectSync ||
        !m_cloudSessionLifecycle || !m_collaboration ||
        targets.empty() || targets.size() > 8) {
        return false;
    }

    const QString projectId = canonicalCloudUuid(m_cloudProjectId);
    const QString sessionId =
        canonicalCloudUuid(m_cloudSessionLifecycle->sessionId());
    if (projectId.isEmpty() || sessionId.isEmpty() ||
        projectId != m_cloudProjectId ||
        m_cloudProjectSync->phase() != collab::CloudSyncPhase::Ready ||
        m_cloudProjectSync->projectId() != projectId ||
        m_cloudSessionLifecycle->phase() !=
            collab::CloudSessionLifecyclePhase::Active ||
        m_cloudSessionLifecycle->projectId() != projectId ||
        m_collaboration->projectId() != projectId ||
        m_collaboration->sessionId() != sessionId ||
        !m_collaboration->canSubmitOperations()) {
        return false;
    }

    const auto role = m_cloudSessionLifecycle->role();
    if (role != collab::CloudProjectRole::Owner &&
        role != collab::CloudProjectRole::Editor) {
        return false;
    }

    QSet<QString> unique;
    for (const std::string& rawTrackId : targets) {
        const QString trackId = QString::fromStdString(rawTrackId);
        if (canonicalCloudUuid(trackId) != trackId ||
            unique.contains(trackId)) {
            return false;
        }
        unique.insert(trackId);
    }
    return true;
}

bool MainWindow::ensureCloudRecordingRecoverySession() {
    if (!m_journal.running()) {
        if (!m_journal.start(
                ui::recovery::rootDir().toStdString(),
                QCoreApplication::applicationVersion().toStdString())) {
            return false;
        }
        if (!m_projectPath.isEmpty()) {
            m_journal.setProjectPath(
                m_projectPath.toStdString(),
                QFileInfo(m_projectPath).baseName().toStdString());
        }
    }

    daw::recovery::RecoverySnapshot snapshot =
        m_controller.captureRecoverySnapshot();
    m_recoveryPluginStateFiles.clear();
    m_recoveryPluginStateFiles.reserve(snapshot.pluginStates.size());
    for (const auto& state : snapshot.pluginStates)
        m_recoveryPluginStateFiles.push_back(state.fileName);
    m_journal.requestWrite(std::move(snapshot));
    m_journal.flush();
    m_journalStale = false;
    if (!m_journal.running() || m_journal.sessionDir().empty()) return false;
    return QFileInfo::exists(
        QDir(QString::fromStdString(m_journal.sessionDir()))
            .filePath(QString::fromUtf8(daw::recovery::kJournalFile)));
}

bool MainWindow::cloudRecordingRecoveryExists() const {
    if (!m_journal.running() || m_journal.sessionDir().empty()) return false;
    return daw::recovery::CloudRecordingRecoveryStore(
               m_journal.sessionDir())
        .exists();
}

void MainWindow::startCloudRecording(
    const std::vector<std::string>& targets) {
    if (!m_cloudProjectId.isEmpty()) {
        statusBar()->showMessage(
            tr("Cloud recording is disabled in this version. Make a local copy to record."),
            7000);
        return;
    }
    if (m_cloudRecording) {
        const bool retry =
            m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::FinalizedWriteFailed;
        statusBar()->showMessage(
            retry
                ? tr("Save the finished recovery take with R or Stop before recording again")
                : tr("A cloud recording request is already in progress"),
            6000);
        return;
    }

    // Exact engine preflight happens before the first REST request. A partial
    // target set must never acquire leases and then silently record less.
    if (!m_controller.canStartRecordingTracksExactly(targets)) {
        statusBar()->showMessage(
            tr("Could not start recording — check every selected track and audio input"),
            5000);
        return;
    }
    if (!cloudRecordingContextIsWritable(targets)) {
        statusBar()->showMessage(
            tr("Cloud recording requires an active writable session"), 6000);
        return;
    }
    if (!ensureCloudRecordingRecoverySession()) {
        QMessageBox::warning(
            this, tr("Recording Safety Unavailable"),
            tr("VLT could not create durable local recovery storage. Recording was not started."));
        return;
    }

    const QString projectId = m_cloudProjectId;
    const QString sessionId = m_cloudSessionLifecycle->sessionId();
    daw::recovery::CloudRecordingRecoveryStore recovery(
        m_journal.sessionDir());
    if (recovery.exists()) {
        daw::recovery::CloudRecordingRecoveryManifest existing;
        const auto read = recovery.read(existing);
        if (!read || existing.projectId != projectId.toStdString() ||
            existing.sessionId != sessionId.toStdString()) {
            QMessageBox::warning(
                this, tr("Cloud Recording Recovery Pending"),
                tr("A previous cloud take still needs recovery. VLT will not replace it with a different project or session."));
            return;
        }
    }

    auto intent = std::make_unique<CloudRecordingRuntime>();
    intent->generation = ++m_cloudRecordingGeneration;
    intent->projectId = projectId;
    intent->sessionId = sessionId;
    const auto makeUuid = [] {
        return QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .toLower();
    };
    intent->runId = makeUuid();
    intent->opId = makeUuid();
    intent->transactionId = makeUuid();
    intent->createdAtUnixMs =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    intent->trackIds = targets;
    intent->countInBeats =
        std::max(0, m_controller.recordingPrefs().countInBeats);
    const quint64 generation = intent->generation;
    QStringList leaseTargets;
    leaseTargets.reserve(int(targets.size()));
    for (const std::string& trackId : targets) {
        const QString id = QString::fromStdString(trackId);
        leaseTargets.push_back(id);
        intent->captureIdByTrack.insert(id, makeUuid());
        intent->uploadIdByTrack.insert(id, makeUuid());
        intent->assetIdByTrack.insert(id, makeUuid());
    }
    m_cloudRecording = std::move(intent);

    const bool started = m_recordingLeases->acquire(
        projectId, sessionId, m_cloudSessionLifecycle->role(),
        collab::CloudSessionStatus::Active, leaseTargets);
    if (!started && m_cloudRecording &&
        m_cloudRecording->generation == generation) {
        m_cloudRecording.reset();
    }
    if (started && m_cloudRecording &&
        m_cloudRecording->generation == generation) {
        statusBar()->showMessage(
            tr("Reserving selected tracks for recording…"));
    }
}

void MainWindow::handleRecordingLeasesAcquired() {
    if (!m_cloudRecording || !m_recordingLeases ||
        m_cloudRecording->phase !=
            CloudRecordingRuntime::Phase::Acquiring) {
        return;
    }
    const quint64 generation = m_cloudRecording->generation;
    if (!cloudRecordingContextIsWritable(m_cloudRecording->trackIds) ||
        !m_controller.canStartRecordingTracksExactly(
            m_cloudRecording->trackIds) ||
        m_cloudRecording->projectId != m_cloudProjectId ||
        m_cloudRecording->sessionId !=
            m_cloudSessionLifecycle->sessionId() ||
        m_recordingLeases->state() != collab::RecordingLeaseState::Held ||
        m_recordingLeases->projectId() != m_cloudRecording->projectId ||
        m_recordingLeases->sessionId() != m_cloudRecording->sessionId) {
        cancelPendingCloudRecording(
            tr("Recording cancelled because the cloud session changed"));
        return;
    }

    const QVector<collab::CloudProjectTrackLease> leases =
        m_recordingLeases->leases();
    QHash<QString, QString> byTrack;
    for (const auto& lease : leases) {
        if (lease.kind != collab::CloudProjectLeaseKind::Record ||
            lease.projectId != m_cloudRecording->projectId ||
            lease.sessionId != m_cloudRecording->sessionId ||
            canonicalCloudUuid(lease.id) != lease.id ||
            canonicalCloudUuid(lease.trackId) != lease.trackId ||
            byTrack.contains(lease.trackId)) {
            cancelPendingCloudRecording(
                tr("Recording cancelled because a track reservation was invalid"));
            return;
        }
        byTrack.insert(lease.trackId, lease.id);
    }
    if (byTrack.size() != int(m_cloudRecording->trackIds.size())) {
        cancelPendingCloudRecording(
            tr("Recording cancelled because not every track was reserved"));
        return;
    }
    for (const std::string& target : m_cloudRecording->trackIds) {
        if (!byTrack.contains(QString::fromStdString(target))) {
            cancelPendingCloudRecording(
                tr("Recording cancelled because not every track was reserved"));
            return;
        }
    }
    m_cloudRecording->leaseIdByTrack = std::move(byTrack);

    bool started = false;
    if (m_cloudRecording->countInBeats > 0) {
        started = m_controller.armCountInExactly(
            m_cloudRecording->trackIds, m_cloudRecording->countInBeats);
        if (started && m_cloudRecording &&
            m_cloudRecording->generation == generation) {
            m_cloudRecording->phase =
                CloudRecordingRuntime::Phase::CountingIn;
            setRecordEngaged(true);
            statusBar()->showMessage(
                tr("Counting in… %1").arg(m_cloudRecording->countInBeats),
                2000);
            m_countInClock.start();
            m_countInTimer->start();
            if (m_contextPanel) m_contextPanel->refresh();
            m_timeline->update();
            return;
        }
    } else {
        started = m_controller.startRecordingTracksExactly(
            m_cloudRecording->trackIds);
        if (started && m_cloudRecording &&
            m_cloudRecording->generation == generation) {
            m_cloudRecording->phase =
                CloudRecordingRuntime::Phase::Capturing;
            announceRecordingStarted(m_cloudRecording->trackIds);
            syncPlayheadTimer();
            return;
        }
    }

    cancelPendingCloudRecording(
        tr("Could not start recording — check the audio input in Preferences"));
}

void MainWindow::cancelPendingCloudRecording(const QString& safeNotice) {
    if (!m_cloudRecording) return;
    const auto phase = m_cloudRecording->phase;
    if (phase == CloudRecordingRuntime::Phase::Capturing ||
        phase == CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        m_cloudRecording->recoveryOnly = true;
        if (!safeNotice.isEmpty())
            statusBar()->showMessage(safeNotice.left(320), 6000);
        return;
    }
    if (phase == CloudRecordingRuntime::Phase::CountingIn &&
        m_controller.isCountingIn()) {
        m_countInTimer->stop();
        m_controller.cancelCountIn();
    }
    if (m_recordingLeases) {
        if (m_recordingLeases->state() ==
            collab::RecordingLeaseState::Lost)
            m_recordingLeases->resetAfterLoss();
        else
            m_recordingLeases->releaseAll();
    }
    m_cloudRecording.reset();
    if (!safeNotice.isEmpty())
        statusBar()->showMessage(safeNotice.left(320), 5000);
    if (m_contextPanel) m_contextPanel->refresh();
    m_timeline->update();
}

void MainWindow::handleCloudRecordingContextChange() {
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        stopCloudRecordingNow(/*interactiveError=*/false);
    }
    const QString currentProject = canonicalCloudUuid(m_cloudProjectId);
    const QString currentSession =
        m_cloudSessionLifecycle
            ? canonicalCloudUuid(m_cloudSessionLifecycle->sessionId())
            : QString();
    if (m_cloudRecording) {
        const bool sameIdentity =
            m_cloudRecording->projectId == currentProject &&
            m_cloudRecording->sessionId == currentSession;
        const bool activeEditor =
            sameIdentity && m_cloudSessionLifecycle &&
            m_cloudSessionLifecycle->phase() ==
                collab::CloudSessionLifecyclePhase::Active &&
            (m_cloudSessionLifecycle->role() ==
                 collab::CloudProjectRole::Owner ||
             m_cloudSessionLifecycle->role() ==
                 collab::CloudProjectRole::Editor);
        const auto phase = m_cloudRecording->phase;
        if (phase == CloudRecordingRuntime::Phase::Capturing ||
            phase == CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
            if (!activeEditor) {
                m_cloudRecording->lostLease = true;
                m_cloudRecording->recoveryOnly = true;
            }
        } else if (!sameIdentity ||
                   !cloudRecordingContextIsWritable(
                       m_cloudRecording->trackIds)) {
            cancelPendingCloudRecording(
                tr("Recording cancelled because the cloud session changed"));
        }
    }
    if (m_recordingLeases)
        m_recordingLeases->handleSessionChanged(currentProject,
                                                currentSession);
}

bool MainWindow::prepareCloudRecordingForProjectTransition() {
    if (!m_cloudRecording) return true;
    const auto phase = m_cloudRecording->phase;
    if (phase == CloudRecordingRuntime::Phase::Capturing ||
        phase == CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        stopCloudRecordingNow();
        return !m_cloudRecording;
    }
    cancelPendingCloudRecording();
    return true;
}

void MainWindow::updateCloudPublicationAction() {
    if (!m_publishProjectAction) return;
    if (m_publicationUi && m_publicationUi->active) {
        m_publishProjectAction->setText(tr("Cancel Cloud Publication…"));
        m_publishProjectAction->setEnabled(m_cloudPublisher != nullptr);
        m_publishProjectAction->setToolTip(
            tr("Cancel the current upload. The saved local project is not changed."));
        return;
    }
    m_publishProjectAction->setText(tr("Publish Project to Cloud…"));
    if (!m_cloudProjectId.isEmpty() || !m_candidateCloudProjectId.isEmpty()) {
        m_publishProjectAction->setEnabled(false);
        m_publishProjectAction->setToolTip(
            m_cloudProjectId.isEmpty()
                ? tr("A cloud project is being opened.")
                : tr("This document is already bound to a cloud project."));
        return;
    }
    m_publishProjectAction->setEnabled(m_cloudPublisher && m_cloudProjectSync);
    m_publishProjectAction->setToolTip(
        tr("Save a local project, validate it, and publish an immutable "
           "cloud copy."));
}

void MainWindow::updateCloudSessionActions() {
    const bool available = m_cloudSessionLifecycle != nullptr;
    if (m_cloudProjectsAction)
        m_cloudProjectsAction->setEnabled(m_cloudProjectClient != nullptr);
    if (m_startSessionAction) {
        m_startSessionAction->setEnabled(
            available && m_cloudSessionLifecycle->canStartSession());
        m_startSessionAction->setToolTip(
            available
                ? tr("Start an independent-transport session after the cloud "
                     "project has been verified and no room is active.")
                : tr("Cloud session service is unavailable."));
    }
    if (m_invitePeopleAction) {
        const bool enabled = canInvitePeople();
        m_invitePeopleAction->setEnabled(enabled);
        m_invitePeopleAction->setToolTip(
            enabled
                ? tr("Create an editor or viewer invitation for this cloud project.")
                : tr("Only the owner of a verified cloud project can invite people."));
    }
    if (m_sessionSettingsAction) {
        const bool enabled = canOpenSessionSettings();
        m_sessionSettingsAction->setEnabled(enabled);
        m_sessionSettingsAction->setToolTip(
            enabled
                ? tr("View participants and manage this verified cloud project.")
                : tr("Open a verified cloud project to view session settings."));
    }
    if (m_endSessionAction) {
        const bool ending =
            available &&
            m_cloudSessionLifecycle->phase() ==
                collab::CloudSessionLifecyclePhase::Ending;
        m_endSessionAction->setText(
            ending ? tr("Ending Session…") : tr("End Session…"));
        m_endSessionAction->setEnabled(
            available && m_cloudSessionLifecycle->canEndSession());
        m_endSessionAction->setToolTip(tr(
            "The owner or current host can save the final snapshot and end "
            "the room for everyone."));
    }
    if (m_leaveSessionAction) {
        const bool leaving =
            available &&
            m_cloudSessionLifecycle->phase() ==
                collab::CloudSessionLifecyclePhase::Leaving;
        m_leaveSessionAction->setText(
            leaving ? tr("Leaving Session…") : tr("Leave Session…"));
        m_leaveSessionAction->setEnabled(
            available && m_cloudSessionLifecycle->canLeaveSession());
        m_leaveSessionAction->setToolTip(
            tr("Leave this room without ending it for other participants."));
    }
}

bool MainWindow::canInvitePeople() const {
    if (!m_cloudProjectClient || !m_cloudProjectSync ||
        !m_cloudSessionLifecycle || m_cloudProjectId.isEmpty()) {
        return false;
    }
    const QUuid uuid(m_cloudProjectId);
    if (uuid.isNull() ||
        uuid.toString(QUuid::WithoutBraces).toLower() != m_cloudProjectId) {
        return false;
    }
    if (m_cloudProjectSync->phase() != collab::CloudSyncPhase::Ready ||
        m_cloudProjectSync->projectId() != m_cloudProjectId ||
        m_cloudSessionLifecycle->projectId() != m_cloudProjectId ||
        m_cloudSessionLifecycle->projectStatus() !=
            collab::CloudProjectStatus::Active ||
        m_cloudSessionLifecycle->role() != collab::CloudProjectRole::Owner) {
        return false;
    }
    const auto phase = m_cloudSessionLifecycle->phase();
    return phase == collab::CloudSessionLifecyclePhase::Ready ||
           phase == collab::CloudSessionLifecyclePhase::Active;
}

bool MainWindow::canOpenSessionSettings() const {
    if (!m_cloudProjectClient || !m_cloudProjectSync ||
        !m_cloudSessionLifecycle || m_cloudProjectId.isEmpty() ||
        m_cloudProjectSync->phase() != collab::CloudSyncPhase::Ready ||
        m_cloudProjectSync->projectId() != m_cloudProjectId ||
        m_cloudSessionLifecycle->projectId() != m_cloudProjectId) {
        return false;
    }
    const auto phase = m_cloudSessionLifecycle->phase();
    return phase == collab::CloudSessionLifecyclePhase::Ready ||
           phase == collab::CloudSessionLifecyclePhase::Active ||
           phase == collab::CloudSessionLifecyclePhase::Ending;
}

bool MainWindow::openCloudProject(const QString& requestedProjectId,
                                  const QString& localBackupPath) {
    const QString projectId = canonicalCloudUuid(requestedProjectId);
    if (projectId.isEmpty() || !m_cloudProjectSync ||
        !m_cloudProjectClient) {
        statusBar()->showMessage(tr("Cloud project is unavailable"), 5000);
        return false;
    }
    if (m_collaborationCommandBridge &&
        m_collaborationCommandBridge->pendingOperationCount() != 0) {
        QMessageBox::information(
            this, tr("Cloud Edits Still Synchronizing"),
            tr("Wait for pending cloud edits to be confirmed before opening another project."));
        return false;
    }
    if (m_cloudProjectId.isEmpty() && m_dirty && !maybeSaveChanges())
        return false;

    m_candidateCloudProjectId = projectId;
    m_candidateCloudBackupPath = localBackupPath.isEmpty()
        ? (m_cloudProjectId.isEmpty() ? m_projectPath : QString())
        : absoluteCleanPath(localBackupPath);
    const quint64 generation = m_cloudProjectSync->synchronize(projectId, true);
    if (generation == 0) {
        m_candidateCloudProjectId.clear();
        m_candidateCloudBackupPath.clear();
        return false;
    }
    statusBar()->showMessage(tr("Opening verified cloud project…"));
    return true;
}

void MainWindow::persistCloudBinding() {
    if (!m_cloudProjectClient || m_cloudProjectId.isEmpty()) return;
    const QString userId = m_cloudProjectClient->currentUserId();
    if (userId.isEmpty()) return;
    QSettings settings;
    settings.setAtomicSyncRequired(true);
    settings.beginGroup(QStringLiteral("collaboration/v2/") + userId);
    settings.setValue(QStringLiteral("cacheVersion"), 2);
    settings.setValue(QStringLiteral("projectId"), m_cloudProjectId);
    settings.setValue(QStringLiteral("localBackupPath"),
                      m_cloudLocalBackupPath);
    settings.endGroup();
    settings.sync();
}

void MainWindow::restoreCloudBinding() {
    if (!m_cloudProjectClient || !m_cloudProjectSync ||
        !m_cloudProjectId.isEmpty() || !m_candidateCloudProjectId.isEmpty() ||
        m_dirty || !m_projectPath.isEmpty()) {
        return;
    }
    const QString userId = m_cloudProjectClient->currentUserId();
    if (userId.isEmpty()) return;
    QSettings settings;
    settings.setAtomicSyncRequired(true);
    settings.beginGroup(QStringLiteral("collaboration/v2/") + userId);
    const int cacheVersion = settings.value(QStringLiteral("cacheVersion"))
                                 .toInt();
    const QString projectId = settings.value(QStringLiteral("projectId"))
                                  .toString();
    const QString backup = settings.value(QStringLiteral("localBackupPath"))
                               .toString();
    settings.endGroup();
    if (cacheVersion == 2 && !canonicalCloudUuid(projectId).isEmpty())
        openCloudProject(projectId, backup);
}

void MainWindow::onOpenCloudProjects() {
    if (!m_cloudProjectClient) {
        statusBar()->showMessage(tr("Cloud projects are unavailable"), 5000);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Cloud Projects"));
    dialog.resize(620, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* refresh = buttons->addButton(
        tr("Refresh"), QDialogButtonBox::ActionRole);
    QPushButton* acceptInvite = buttons->addButton(
        tr("Accept Invitation…"), QDialogButtonBox::ActionRole);
    QPushButton* archive = buttons->addButton(
        tr("Archive"), QDialogButtonBox::DestructiveRole);
    QPushButton* open = buttons->addButton(
        tr("Open"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    open->setEnabled(false);
    archive->setEnabled(false);
    connect(list, &QListWidget::itemSelectionChanged, &dialog,
            [list, open, archive] {
                const QListWidgetItem* item = list->currentItem();
                const auto status = item
                    ? static_cast<collab::CloudProjectStatus>(
                          item->data(Qt::UserRole + 1).toInt())
                    : collab::CloudProjectStatus::Archived;
                const auto role = item
                    ? static_cast<collab::CloudProjectRole>(
                          item->data(Qt::UserRole + 2).toInt())
                    : collab::CloudProjectRole::Viewer;
                open->setEnabled(
                    item && status != collab::CloudProjectStatus::Archived &&
                    status != collab::CloudProjectStatus::Uploading);
                archive->setEnabled(
                    item && role == collab::CloudProjectRole::Owner &&
                    status != collab::CloudProjectStatus::Archived);
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(open, &QPushButton::clicked, &dialog, &QDialog::accept);

    quint64 listRequest = 0;
    quint64 acceptRequest = 0;
    quint64 archiveRequest = 0;
    QString acceptedProjectId;
    const auto requestList = [&] {
        list->clear();
        list->addItem(tr("Loading…"));
        listRequest = m_cloudProjectClient->listProjects();
    };
    connect(refresh, &QPushButton::clicked, &dialog, requestList);
    connect(m_cloudProjectClient,
            &collab::CloudProjectClient::projectsListed, &dialog,
            [&](quint64 requestId,
                const QVector<collab::CloudProjectView>& projects) {
                if (requestId != listRequest) return;
                listRequest = 0;
                list->clear();
                for (const collab::CloudProjectView& view : projects) {
                    QString role = tr("Viewer");
                    if (view.role == collab::CloudProjectRole::Owner)
                        role = tr("Owner");
                    else if (view.role == collab::CloudProjectRole::Editor)
                        role = tr("Editor");
                    QString status = tr("Active");
                    if (view.project.status == collab::CloudProjectStatus::ReadOnly)
                        status = tr("Read-only");
                    else if (view.project.status == collab::CloudProjectStatus::Conflict)
                        status = tr("Conflict");
                    else if (view.project.status == collab::CloudProjectStatus::Archived)
                        status = tr("Archived");
                    else if (view.project.status == collab::CloudProjectStatus::Uploading)
                        status = tr("Uploading");
                    auto* item = new QListWidgetItem(
                        tr("%1  —  %2, %3")
                            .arg(view.project.title, role, status),
                        list);
                    item->setData(Qt::UserRole, view.project.id);
                    item->setData(Qt::UserRole + 1,
                                  int(view.project.status));
                    item->setData(Qt::UserRole + 2, int(view.role));
                }
                if (list->count() == 0)
                    list->addItem(tr("No cloud projects"));
            });
    connect(acceptInvite, &QPushButton::clicked, &dialog, [&] {
        bool accepted = false;
        const QString token = QInputDialog::getText(
            &dialog, tr("Accept Invitation"), tr("Invitation token:"),
            QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || token.isEmpty()) return;
        acceptRequest = m_cloudProjectClient->acceptInvite(token);
    });
    connect(m_cloudProjectClient,
            &collab::CloudProjectClient::inviteAccepted, &dialog,
            [&](quint64 requestId, const collab::CloudProjectView& project) {
                if (requestId != acceptRequest) return;
                acceptRequest = 0;
                // Keep the modal result local. Reusing the app-wide candidate
                // slot here could steal the identity of a bootstrap that
                // completes while this dialog's nested event loop is running.
                acceptedProjectId = project.project.id;
                dialog.accept();
            });
    connect(archive, &QPushButton::clicked, &dialog, [&] {
        QListWidgetItem* item = list->currentItem();
        if (!item) return;
        const QString projectId = item->data(Qt::UserRole).toString();
        if (projectId.isEmpty() || projectId == m_cloudProjectId) {
            statusBar()->showMessage(
                tr("Open another project before archiving this one"), 5000);
            return;
        }
        if (QMessageBox::question(
                &dialog, tr("Archive Cloud Project"),
                tr("Archive the selected cloud project?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) return;
        archiveRequest = m_cloudProjectClient->archiveProject(projectId);
    });
    connect(m_cloudProjectClient,
            &collab::CloudProjectClient::operationCompleted, &dialog,
            [&](quint64 requestId, collab::CloudRequestKind kind,
                const QString&) {
                if (requestId == archiveRequest &&
                    kind == collab::CloudRequestKind::ArchiveProject) {
                    archiveRequest = 0;
                    requestList();
                }
            });
    connect(m_cloudProjectClient, &collab::CloudProjectClient::requestFailed,
            &dialog, [&](quint64 requestId, collab::CloudRequestKind kind,
                         const collab::CloudClientError& error) {
                // listProjects can reject synchronously before requestList has
                // stored its returned id. It is the only list caller while
                // this modal connection exists, so match its kind as well.
                if (kind == collab::CloudRequestKind::ListProjects) {
                    listRequest = 0;
                    list->clear();
                    list->addItem(tr("Could not load cloud projects"));
                } else if (requestId == acceptRequest) {
                    acceptRequest = 0;
                } else if (requestId == archiveRequest) {
                    archiveRequest = 0;
                } else {
                    return;
                }
                QMessageBox::warning(
                    &dialog, tr("Cloud Project Request Failed"),
                    error.safeMessage.isEmpty()
                        ? tr("The cloud request could not be completed.")
                        : error.safeMessage.left(400));
            });

    requestList();
    const int result = dialog.exec();
    QString selectedProject;
    if (result == QDialog::Accepted) {
        if (!acceptedProjectId.isEmpty()) {
            selectedProject = acceptedProjectId;
        } else if (list->currentItem()) {
            selectedProject = list->currentItem()->data(Qt::UserRole).toString();
        }
    }
    if (!selectedProject.isEmpty()) openCloudProject(selectedProject);
}

void MainWindow::onMakeLocalCopy() {
    if (m_cloudProjectId.isEmpty() || !m_cloudProjectSync ||
        !m_collaborationCommandBridge || !m_collaborationAssetCache ||
        !m_cloudAssetHydrator ||
        m_cloudProjectSync->phase() != collab::CloudSyncPhase::Ready) {
        statusBar()->showMessage(tr("No verified cloud project is open"), 5000);
        return;
    }
    if (m_collaborationCommandBridge->pendingOperationCount() != 0) {
        QMessageBox::information(
            this, tr("Cloud Edits Still Synchronizing"),
            tr("Wait for all edits to be confirmed before making a local copy."));
        return;
    }
    if (m_cloudAssetHydrator &&
        (m_cloudAssetHydrator->projectId() != m_cloudProjectId ||
         m_cloudAssetHydrator->state() != collab::CloudHydrationState::Ready ||
         m_cloudAssetHydrator->pendingCount() != 0 ||
         m_cloudAssetHydrator->failedCount() != 0)) {
        QMessageBox::information(
            this, tr("Cloud Assets Not Ready"),
            tr("Wait for every cloud asset to download successfully before making a portable local copy."));
        return;
    }

    // Freeze the exact reducer head before opening a nested file dialog.  Modal
    // event loops continue to deliver websocket traffic, so every condition is
    // checked again after the user has chosen a destination and after the
    // potentially long integrity pass.
    const QString frozenProjectId = m_cloudProjectId;
    const quint64 frozenGeneration = m_cloudProjectSync->generation();
    const quint64 frozenSequence =
        m_collaborationCommandBridge->confirmedServerSequence();
    const auto frozen =
        m_collaborationCommandBridge->confirmedSnapshotAt(frozenSequence);
    if (!frozen) {
        QMessageBox::warning(
            this, tr("Local Copy Not Ready"),
            tr("The confirmed cloud head is not available. Reconnect and try again."));
        return;
    }

    QString target = QFileDialog::getSaveFileName(
        this, tr("Make Local Copy"),
        displayProjectName(frozen->project.name) + QStringLiteral(".vlt"),
        tr("VLT Project (*.vlt)"));
    if (target.isEmpty()) return;
    target = packagePathFromSelection(target);
    const QString extension =
        QStringLiteral(".") + QString::fromLatin1(daw::ProjectSerializer::kExtension);
    if (!target.endsWith(extension, Qt::CaseInsensitive)) target += extension;
    target = absoluteCleanPath(target);
    if (target.isEmpty() ||
        (!m_cloudLocalBackupPath.isEmpty() &&
         target == absoluteCleanPath(m_cloudLocalBackupPath))) {
        QMessageBox::warning(
            this, tr("Choose Another Destination"),
            tr("The protected publication backup cannot be replaced. Choose a different file."));
        return;
    }
    const QFileInfo targetInfo(target);
    if (targetInfo.exists() && !targetInfo.isDir()) {
        QMessageBox::warning(
            this, tr("Local Copy Failed"),
            tr("The selected destination is not a VLT project package."));
        return;
    }

    const auto stillFrozen = [&] {
        return m_cloudProjectId == frozenProjectId && m_cloudProjectSync &&
               m_cloudProjectSync->generation() == frozenGeneration &&
               m_cloudProjectSync->phase() == collab::CloudSyncPhase::Ready &&
               m_collaborationCommandBridge &&
               m_collaborationCommandBridge->pendingOperationCount() == 0 &&
               m_collaborationCommandBridge->confirmedServerSequence() ==
                   frozenSequence &&
               m_cloudAssetHydrator &&
               m_cloudAssetHydrator->projectId() == frozenProjectId &&
               m_cloudAssetHydrator->state() ==
                   collab::CloudHydrationState::Ready &&
               m_cloudAssetHydrator->pendingCount() == 0 &&
               m_cloudAssetHydrator->failedCount() == 0;
    };
    if (!stillFrozen()) {
        QMessageBox::information(
            this, tr("Cloud Project Changed"),
            tr("The confirmed cloud head changed while the dialog was open. Start the local copy again."));
        return;
    }

    // Prove that the engine edge-copy contains exactly the frozen shared
    // document. Local transport/layout/device fields and local paths are
    // projected away before comparison.
    const daw::ProjectModel runtimeBefore = m_controller.project();
    const auto projected =
        daw::cloud::projectForCloudSnapshotV1(runtimeBefore);
    std::string projectedBytes;
    std::string confirmedBytes;
    const audio::Result projectedResult = daw::ProjectSerializer::serializeDocument(
        projected.document, projectedBytes, daw::MediaPaths::Absolute);
    const audio::Result confirmedResult = daw::ProjectSerializer::serializeDocument(
        frozen->project, confirmedBytes, daw::MediaPaths::Absolute);
    if (!projectedResult || !confirmedResult || !projected.valid() ||
        projectedBytes != confirmedBytes) {
        QMessageBox::warning(
            this, tr("Local Copy Not Safe"),
            tr("The live project is not the exact confirmed cloud head. Reconnect before exporting a local copy."));
        return;
    }

    const QList<daw::AssetRef> assets =
        collab::collectCloudProjectAssets(frozen->project);
    quint64 totalBytes = 0;
    for (const daw::AssetRef& asset : assets) {
        if (std::numeric_limits<quint64>::max() - totalBytes < asset.byteSize) {
            QMessageBox::warning(this, tr("Local Copy Failed"),
                                 tr("The project asset manifest is too large."));
            return;
        }
        totalBytes += asset.byteSize;
    }

    QProgressDialog verification(tr("Verifying cloud assets…"), tr("Cancel"),
                                 0, 1000, this);
    verification.setWindowTitle(tr("Make Local Copy"));
    verification.setWindowModality(Qt::WindowModal);
    verification.setMinimumDuration(0);
    quint64 verifiedBytes = 0;
    bool integrityFailure = false;
    for (const daw::AssetRef& asset : assets) {
        const QString localPath = m_collaborationAssetCache->resolve(asset);
        QFile file(localPath);
        if (localPath.isEmpty() || !file.open(QIODevice::ReadOnly) ||
            quint64(file.size()) != asset.byteSize) {
            integrityFailure = true;
            break;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
                integrityFailure = true;
                break;
            }
            hash.addData(chunk);
            verifiedBytes += quint64(chunk.size());
            const int value = totalBytes == 0
                ? 1000
                : int(std::min<quint64>(
                      1000, quint64((static_cast<long double>(verifiedBytes) *
                                     1000.0L) /
                                    static_cast<long double>(totalBytes))));
            verification.setValue(value);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            if (verification.wasCanceled()) return;
        }
        const QString expected =
            QString::fromStdString(asset.sha256).trimmed().toLower();
        if (integrityFailure ||
            QString::fromLatin1(hash.result().toHex()) != expected) {
            integrityFailure = true;
            break;
        }
    }
    verification.setValue(1000);
    if (integrityFailure) {
        QMessageBox::warning(
            this, tr("Asset Verification Failed"),
            tr("At least one cloud asset failed its size or SHA-256 check. Retry hydration before making a local copy."));
        return;
    }
    if (!stillFrozen()) {
        QMessageBox::information(
            this, tr("Cloud Project Changed"),
            tr("The confirmed cloud head changed during verification. Start the local copy again."));
        return;
    }

    QDir parent(targetInfo.absolutePath());
    if (!parent.exists() && !QDir().mkpath(targetInfo.absolutePath())) {
        QMessageBox::warning(this, tr("Local Copy Failed"),
                             tr("The destination folder could not be created."));
        return;
    }
    const QString targetName = QFileInfo(target).fileName();
    const QString nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagingName = QStringLiteral(".%1.tmp-%2").arg(targetName, nonce);
    const QString backupName = QStringLiteral(".%1.backup-%2").arg(targetName, nonce);
    const QString staging = parent.absoluteFilePath(stagingName);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    audio::Result saved = m_controller.saveProject(staging.toStdString());
    if (saved) {
        daw::ProjectModel portable;
        saved = daw::ProjectSerializer::load(portable, staging.toStdString());
        if (saved) {
            // The local package owns ordinary paths/state chunks. Cloud asset
            // identities are removed so reopening never depends on account
            // cache state. Sampler sources have already been embedded by the
            // live built-in's saveProjectState path above.
            const auto clearInsert = [](daw::InsertModel& insert) {
                insert.stateAsset = {};
                insert.rightStateAsset = {};
                insert.assetBindings.clear();
            };
            const auto clearInserts = [&](std::vector<daw::InsertModel>& inserts) {
                for (daw::InsertModel& insert : inserts) clearInsert(insert);
            };
            clearInserts(portable.masterInserts);
            for (daw::TrackModel& track : portable.tracks) {
                clearInsert(track.instrument);
                clearInserts(track.samplerFx.inserts);
                clearInserts(track.inserts);
                for (daw::ClipModel& clip : track.clips) {
                    clip.asset = {};
                    clearInserts(clip.inserts);
                    for (daw::TakeModel& take : clip.takes) take.asset = {};
                }
            }
            portable.sampleRate = frozen->project.sampleRate;
            portable.name = QFileInfo(target).completeBaseName().toStdString();
            saved = daw::ProjectSerializer::saveDocument(
                portable,
                QDir(staging)
                    .absoluteFilePath(
                        QString::fromLatin1(daw::ProjectSerializer::kProjectFile))
                    .toStdString(),
                daw::MediaPaths::Basenames);
        }
    }

    bool installed = false;
    bool movedOldTarget = false;
    if (saved) {
        movedOldTarget = !QFileInfo::exists(target) ||
                         parent.rename(targetName, backupName);
        if (movedOldTarget) installed = parent.rename(stagingName, targetName);
        if (!installed && QFileInfo::exists(parent.absoluteFilePath(backupName)))
            parent.rename(backupName, targetName);
    }
    if (installed && QFileInfo::exists(parent.absoluteFilePath(backupName)))
        QDir(parent.absoluteFilePath(backupName)).removeRecursively();
    if (!installed && QFileInfo::exists(staging))
        QDir(staging).removeRecursively();
    QApplication::restoreOverrideCursor();

    if (!saved || !installed) {
        // saveProject captures live built-in state into the model; restore the
        // exact pre-export edge copy if publication did not complete.
        (void)m_controller.materializeCollaborationProject(runtimeBefore, false);
        QMessageBox::warning(
            this, tr("Local Copy Failed"),
            tr("The portable project could not be written atomically. The cloud project remains open."));
        return;
    }

    const audio::Result opened = m_controller.openProject(target.toStdString());
    if (!opened) {
        (void)m_controller.materializeCollaborationProject(runtimeBefore, false);
        QMessageBox::warning(
            this, tr("Local Copy Saved"),
            tr("The portable copy was saved, but could not be opened. The cloud project remains open."));
        return;
    }

    clearCloudProjectBinding(/*cancelPublication=*/false);
    m_projectPath = target;
    m_selectedTrackId.clear();
    m_dirty = false;
    m_journal.setProjectPath(target.toStdString(),
                             QFileInfo(target).baseName().toStdString());
    m_journalStale = true;
    m_transport->syncTempo();
    syncViews();
    if (!m_controller.project().tracks.empty()) {
        selectTrackFromHeader(
            QString::fromStdString(m_controller.project().tracks.front().id));
    }
    updateWindowTitle();
    statusBar()->showMessage(tr("Created local copy %1")
                                 .arg(QFileInfo(target).fileName()),
                             5000);
}

void MainWindow::onInvitePeople() {
    if (!canInvitePeople()) {
        statusBar()->showMessage(
            tr("The verified cloud project is not available for invitations."),
            5000);
        return;
    }
    // Capture the request-bound identity. If account/session state changes
    // while the modal event loop is running, the dialog still cannot drift to
    // another project; the backend remains authoritative for owner rights.
    const QString projectId = m_cloudProjectId;
    collab::CloudProjectInviteDialog dialog(m_cloudProjectClient.data(),
                                             projectId, this);
    if (dialog.projectId() != projectId) {
        statusBar()->showMessage(
            tr("The cloud project identifier is invalid."), 5000);
        return;
    }
    dialog.exec();
    updateCloudSessionActions();
}

void MainWindow::onSessionSettings() {
    if (!canOpenSessionSettings()) {
        statusBar()->showMessage(
            tr("Open a verified cloud project before viewing session settings."),
            5000);
        return;
    }

    const QString projectId = m_cloudProjectId;
    QPointer<collab::CloudProjectClient> client = m_cloudProjectClient;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Session Settings"));
    dialog.resize(720, 440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* status = new QLabel(tr("Loading participants\u2026"), &dialog);
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* participants = new QTreeWidget(&dialog);
    participants->setColumnCount(3);
    participants->setHeaderLabels(
        {tr("Participant"), tr("Role"), tr("Session")});
    participants->setRootIsDecorated(false);
    participants->setAlternatingRowColors(true);
    participants->setSelectionMode(QAbstractItemView::SingleSelection);
    participants->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    participants->header()->setSectionResizeMode(1,
                                                  QHeaderView::ResizeToContents);
    participants->header()->setSectionResizeMode(2,
                                                  QHeaderView::ResizeToContents);
    layout->addWidget(participants, 1);

    auto* management = new QHBoxLayout;
    auto* role = new QComboBox(&dialog);
    role->addItem(tr("Editor"), 1);
    role->addItem(tr("Viewer"), 2);
    auto* applyRole = new QPushButton(tr("Change Role"), &dialog);
    auto* remove = new QPushButton(tr("Remove"), &dialog);
    auto* transfer = new QPushButton(tr("Transfer Ownership\u2026"), &dialog);
    auto* handoff = new QPushButton(tr("Make Session Host"), &dialog);
    management->addWidget(new QLabel(tr("Role:"), &dialog));
    management->addWidget(role);
    management->addWidget(applyRole);
    management->addWidget(remove);
    management->addWidget(transfer);
    management->addStretch(1);
    management->addWidget(handoff);
    layout->addLayout(management);

    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* refresh = buttons->addButton(
        tr("Refresh"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(client.data(), &QObject::destroyed, &dialog, &QDialog::reject);

    constexpr int kUserIdRole = Qt::UserRole;
    constexpr int kRowRole = Qt::UserRole + 1;
    constexpr int kColorRole = Qt::UserRole + 2;
    constexpr int kTargetMemberRole = Qt::UserRole + 3;
    constexpr int kOwnerRow = 0;
    constexpr int kEditorRow = 1;
    constexpr int kViewerRow = 2;

    collab::CloudProjectView project;
    QVector<collab::CloudProjectMember> members;
    collab::CloudSessionState session;
    bool projectLoaded = false;
    bool membersLoaded = false;
    bool sessionLoaded = false;
    bool sessionActive = false;
    bool closing = false;
    QString loadError;
    quint64 generation = 0;
    quint64 projectGeneration = 0;
    quint64 membersGeneration = 0;
    quint64 sessionGeneration = 0;
    quint64 projectRequest = 0;
    quint64 membersRequest = 0;
    quint64 sessionRequest = 0;
    quint64 mutationRequest = 0;

    const auto cancelRequest = [&](quint64& request) {
        const quint64 pending = std::exchange(request, quint64(0));
        if (pending != 0 && client) client->cancel(pending);
    };

    const auto currentOwner = [&] {
        return client && projectLoaded &&
               project.role == collab::CloudProjectRole::Owner &&
               project.project.ownerUserId == client->currentUserId();
    };

    const auto currentHost = [&] {
        if (!m_collaboration || !m_collaboration->isOnline()) return false;
        const QString local = m_collaboration->localParticipantId();
        const QString liveHost = m_collaboration->hostParticipantId();
        const QString host = liveHost.isEmpty()
            ? session.session.hostMemberId
            : liveHost;
        return !local.isEmpty() && local == host;
    };

    const auto updateControls = [&] {
        QTreeWidgetItem* item = participants->currentItem();
        const bool sameProject = client && m_cloudProjectId == projectId &&
                                 canOpenSessionSettings();
        const bool busy = mutationRequest != 0;
        const bool selected = item != nullptr;
        const int selectedRole = selected
            ? item->data(0, kRowRole).toInt()
            : kViewerRow;
        const bool ownerTarget = selectedRole == kOwnerRow;
        const bool mayManage = sameProject && currentOwner() && !busy;
        const QString targetMember = selected
            ? item->data(0, kTargetMemberRole).toString()
            : QString();
        const QString localParticipant = m_collaboration
            ? m_collaboration->localParticipantId()
            : QString();
        const bool mayHandoff =
            sameProject && !busy && selected && !targetMember.isEmpty() &&
            targetMember != localParticipant && selectedRole != kViewerRow &&
            m_cloudSessionLifecycle &&
            m_cloudSessionLifecycle->phase() ==
                collab::CloudSessionLifecyclePhase::Active &&
            (currentOwner() || currentHost());

        role->setEnabled(mayManage && selected && !ownerTarget);
        applyRole->setEnabled(mayManage && selected && !ownerTarget);
        remove->setEnabled(mayManage && selected && !ownerTarget);
        transfer->setEnabled(mayManage && selected &&
                             selectedRole == kEditorRow);
        handoff->setEnabled(mayHandoff);
        refresh->setEnabled(!busy && client);
    };

    const auto render = [&] {
        const QString selectedUser = participants->currentItem()
            ? participants->currentItem()->data(0, kUserIdRole).toString()
            : QString();
        participants->clear();

        const QVector<collab::ParticipantIdentity> presence =
            m_collaboration
                ? m_collaboration->presenceStore()->participants()
                : QVector<collab::ParticipantIdentity>{};
        const QString localParticipant = m_collaboration
            ? m_collaboration->localParticipantId()
            : QString();

        const auto addParticipant = [&](const QString& userId, int rowRole,
                                        int colorIndex) {
            if (userId.isEmpty()) return;
            QString nickname;
            bool online = false;
            bool host = false;
            QString targetMember;

            if (sessionActive) {
                for (const collab::CloudSessionMember& member : session.members) {
                    if (member.userId != userId) continue;
                    online = true;
                    host = host || member.id == session.session.hostMemberId;
                    if (targetMember.isEmpty() ||
                        (targetMember == localParticipant &&
                         member.id != localParticipant)) {
                        targetMember = member.id;
                    }
                }
            }
            for (const collab::ParticipantIdentity& identity : presence) {
                if (identity.userId != userId) continue;
                if (!identity.nickname.trimmed().isEmpty())
                    nickname = collab::safeDisplayName(identity.nickname);
                online = online || identity.online;
                host = host || identity.host ||
                       identity.participantId ==
                           (m_collaboration
                                ? m_collaboration->hostParticipantId()
                                : QString());
                if (identity.online &&
                    (targetMember.isEmpty() ||
                     (targetMember == localParticipant &&
                      identity.participantId != localParticipant))) {
                    targetMember = identity.participantId;
                }
            }

            QString name = nickname.isEmpty() ? userId : nickname;
            if (client && userId == client->currentUserId())
                name += tr(" (You)");
            QString roleName = tr("Viewer");
            if (rowRole == kOwnerRow) roleName = tr("Owner");
            else if (rowRole == kEditorRow) roleName = tr("Editor");
            QString sessionState = online ? tr("Online") : tr("Offline");
            if (host) sessionState = tr("Host \u00b7 Online");

            auto* item = new QTreeWidgetItem(
                participants, {name, roleName, sessionState});
            item->setData(0, kUserIdRole, userId);
            item->setData(0, kRowRole, rowRole);
            item->setData(0, kColorRole, colorIndex);
            item->setData(0, kTargetMemberRole,
                          online ? targetMember : QString());
            item->setToolTip(0, userId);
            if (userId == selectedUser) participants->setCurrentItem(item);
        };

        if (projectLoaded)
            addParticipant(project.project.ownerUserId, kOwnerRow, 0);
        if (membersLoaded) {
            for (const collab::CloudProjectMember& member : members) {
                if (projectLoaded &&
                    member.userId == project.project.ownerUserId) {
                    continue;
                }
                addParticipant(
                    member.userId,
                    member.role == collab::CloudMemberRole::Editor
                        ? kEditorRow
                        : kViewerRow,
                    member.colorIndex);
            }
        }

        if (participants->topLevelItemCount() != 0 &&
            !participants->currentItem()) {
            participants->setCurrentItem(participants->topLevelItem(0));
        }
        if (QTreeWidgetItem* item = participants->currentItem()) {
            const int rowRole = item->data(0, kRowRole).toInt();
            if (rowRole != kOwnerRow)
                role->setCurrentIndex(rowRole == kEditorRow ? 0 : 1);
        }

        if (projectRequest || membersRequest || sessionRequest) {
            status->setText(tr("Refreshing project participants\u2026"));
        } else if (mutationRequest) {
            status->setText(tr("Applying session settings\u2026"));
        } else if (!loadError.isEmpty()) {
            status->setText(loadError);
        } else if (sessionLoaded && sessionActive) {
            status->setText(tr("Live session: %1 participant(s) online.")
                                .arg(session.members.size()));
        } else {
            status->setText(tr("No live session is active. Project membership is still available below."));
        }
        if (projectLoaded && !currentOwner() && !currentHost()) {
            status->setText(
                status->text() +
                tr("\nProject management is read-only for your role."));
        }
        updateControls();
    };

    std::function<void()> requestRefresh;
    requestRefresh = [&] {
        if (!client || closing || m_cloudProjectId != projectId ||
            !canOpenSessionSettings()) {
            dialog.reject();
            return;
        }
        cancelRequest(projectRequest);
        cancelRequest(membersRequest);
        cancelRequest(sessionRequest);
        const quint64 next = ++generation;
        projectGeneration = next;
        membersGeneration = next;
        sessionGeneration = next;
        projectLoaded = false;
        membersLoaded = false;
        sessionLoaded = false;
        sessionActive = false;
        project = {};
        members.clear();
        session = {};
        loadError.clear();
        projectRequest = client->getProject(projectId);
        membersRequest = client->listMembers(projectId);
        sessionRequest = client->getActiveSession(projectId);
        render();
    };

    connect(participants, &QTreeWidget::itemSelectionChanged, &dialog, [&] {
        if (QTreeWidgetItem* item = participants->currentItem()) {
            const int rowRole = item->data(0, kRowRole).toInt();
            if (rowRole != kOwnerRow)
                role->setCurrentIndex(rowRole == kEditorRow ? 0 : 1);
        }
        updateControls();
    });
    connect(refresh, &QPushButton::clicked, &dialog, requestRefresh);

    connect(client.data(), &collab::CloudProjectClient::projectReceived, &dialog,
            [&](quint64 requestId, collab::CloudRequestKind kind,
                const collab::CloudProjectView& view) {
                if (closing || requestId != projectRequest ||
                    projectGeneration != generation ||
                    kind != collab::CloudRequestKind::GetProject ||
                    view.project.id != projectId) {
                    return;
                }
                projectRequest = 0;
                project = view;
                projectLoaded = true;
                render();
            });
    connect(client.data(), &collab::CloudProjectClient::membersListed, &dialog,
            [&](quint64 requestId,
                const QVector<collab::CloudProjectMember>& result) {
                if (closing || requestId != membersRequest ||
                    membersGeneration != generation) {
                    return;
                }
                membersRequest = 0;
                members = result;
                membersLoaded = true;
                render();
            });
    connect(client.data(), &collab::CloudProjectClient::sessionStateReceived,
            &dialog,
            [&](quint64 requestId, collab::CloudRequestKind kind,
                const collab::CloudSessionState& result) {
                if (closing) return;
                if (requestId == sessionRequest &&
                    sessionGeneration == generation &&
                    kind == collab::CloudRequestKind::GetActiveSession &&
                    result.session.projectId == projectId) {
                    sessionRequest = 0;
                    session = result;
                    sessionLoaded = true;
                    sessionActive = true;
                    render();
                    return;
                }
                if (requestId == mutationRequest &&
                    kind == collab::CloudRequestKind::HandoffSession &&
                    result.session.projectId == projectId) {
                    mutationRequest = 0;
                    requestRefresh();
                }
            });
    connect(client.data(), &collab::CloudProjectClient::memberSaved, &dialog,
            [&](quint64 requestId,
                const collab::CloudProjectMember& member) {
                if (closing || requestId != mutationRequest ||
                    member.projectId != projectId) {
                    return;
                }
                mutationRequest = 0;
                requestRefresh();
            });
    connect(client.data(), &collab::CloudProjectClient::ownershipTransferred,
            &dialog,
            [&](quint64 requestId,
                const collab::CloudOwnershipTransfer& result) {
                if (closing || requestId != mutationRequest ||
                    result.project.id != projectId) {
                    return;
                }
                mutationRequest = 0;
                requestRefresh();
            });
    connect(client.data(), &collab::CloudProjectClient::operationCompleted,
            &dialog,
            [&](quint64 requestId, collab::CloudRequestKind kind,
                const QString&) {
                if (closing || requestId != mutationRequest ||
                    kind != collab::CloudRequestKind::RemoveMember) {
                    return;
                }
                mutationRequest = 0;
                requestRefresh();
            });
    connect(client.data(), &collab::CloudProjectClient::requestFailed, &dialog,
            [&](quint64 requestId, collab::CloudRequestKind,
                const collab::CloudClientError& error) {
                if (closing) return;
                if (requestId == sessionRequest && error.httpStatus == 404) {
                    sessionRequest = 0;
                    sessionLoaded = true;
                    sessionActive = false;
                    render();
                    return;
                }
                const QString message = error.safeMessage.isEmpty()
                    ? tr("The cloud request could not be completed.")
                    : error.safeMessage.left(400);
                if (requestId == mutationRequest) {
                    mutationRequest = 0;
                    render();
                    QMessageBox::warning(&dialog,
                                         tr("Session Settings Failed"),
                                         message);
                    return;
                }
                bool matched = false;
                if (requestId == projectRequest) {
                    projectRequest = 0;
                    matched = true;
                }
                if (requestId == membersRequest) {
                    membersRequest = 0;
                    matched = true;
                }
                if (requestId == sessionRequest) {
                    sessionRequest = 0;
                    sessionLoaded = true;
                    sessionActive = false;
                    matched = true;
                }
                if (matched) {
                    if (loadError.isEmpty()) loadError = message;
                    render();
                }
            });

    if (m_collaboration) {
        connect(m_collaboration->presenceStore(),
                &collab::PresenceStore::participantsChanged, &dialog, render);
        connect(m_collaboration, &collab::CollaborationService::roomIdentityChanged,
                &dialog, [&](const QString&, const QString&, const QString&) {
                    render();
                });
    }

    connect(applyRole, &QPushButton::clicked, &dialog, [&] {
        QTreeWidgetItem* item = participants->currentItem();
        if (!item || !currentOwner() || mutationRequest != 0 || !client)
            return;
        collab::PutCloudProjectMemberInput input;
        input.role = role->currentData().toInt() == kEditorRow
            ? collab::CloudMemberRole::Editor
            : collab::CloudMemberRole::Viewer;
        input.colorIndex = item->data(0, kColorRole).toInt();
        mutationRequest = client->putMember(
            projectId, item->data(0, kUserIdRole).toString(), input);
        render();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        QTreeWidgetItem* item = participants->currentItem();
        if (!item || !currentOwner() || mutationRequest != 0 || !client)
            return;
        if (QMessageBox::question(
                &dialog, tr("Remove Project Member"),
                tr("Remove %1 from this cloud project?").arg(item->text(0)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        mutationRequest = client->removeMember(
            projectId, item->data(0, kUserIdRole).toString());
        render();
    });
    connect(transfer, &QPushButton::clicked, &dialog, [&] {
        QTreeWidgetItem* item = participants->currentItem();
        if (!item || !currentOwner() || mutationRequest != 0 || !client ||
            item->data(0, kRowRole).toInt() != kEditorRow) {
            return;
        }
        if (QMessageBox::question(
                &dialog, tr("Transfer Project Ownership"),
                tr("Transfer ownership to %1? You will become an editor, and "
                   "both accounts will reconnect to verify the new roles.")
                    .arg(item->text(0)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        mutationRequest = client->transferOwnership(
            projectId, item->data(0, kUserIdRole).toString());
        render();
    });
    connect(handoff, &QPushButton::clicked, &dialog, [&] {
        QTreeWidgetItem* item = participants->currentItem();
        if (!item || mutationRequest != 0 || !client ||
            !m_cloudSessionLifecycle) {
            return;
        }
        const QString target = item->data(0, kTargetMemberRole).toString();
        const QString sessionId = m_cloudSessionLifecycle->sessionId();
        if (target.isEmpty() || sessionId.isEmpty()) return;
        mutationRequest = client->handoffSession(projectId, sessionId, target);
        render();
    });

    connect(&dialog, &QDialog::finished, &dialog, [&](int) {
        closing = true;
        ++generation;
        cancelRequest(projectRequest);
        cancelRequest(membersRequest);
        cancelRequest(sessionRequest);
        cancelRequest(mutationRequest);
    });

    requestRefresh();
    dialog.exec();
    updateCloudSessionActions();
}

void MainWindow::clearCloudProjectBinding(bool cancelPublication) {
    if (!prepareCloudRecordingForProjectTransition()) return;
    if (m_cloudSharedAssetMutationBridge)
        m_cloudSharedAssetMutationBridge->cancel();
    if (m_publicationUi && m_publicationUi->active) {
        if (m_cloudPublisher) m_cloudPublisher->cancel();
        // cancel() normally emits publicationCancelled synchronously, after
        // its transfers have been cancelled. Keep staging alive until then;
        // this fallback is only for a missing/disconnected publisher.
        if (m_publicationUi->active) {
            m_publicationUi->active = false;
            m_publicationUi->capture.reset();
            m_publicationUi->sourcePath.clear();
        }
    } else if (cancelPublication && m_cloudPublisher) {
        // Also invalidate publisher work whose UI completion was already
        // detached (for example while replacing app-lifetime services).
        m_cloudPublisher->cancel();
    }
    if (m_cloudSessionLifecycle) m_cloudSessionLifecycle->clearBinding();
    if (m_cloudProjectSync) m_cloudProjectSync->cancel();
    if (m_cloudAssetHydrator) m_cloudAssetHydrator->cancel();
    if (m_collaboration) m_collaboration->clearProject();
    if (m_cloudProjectClient) {
        const QString userId = m_cloudProjectClient->currentUserId();
        if (!userId.isEmpty()) {
            QSettings settings;
            settings.remove(QStringLiteral("collaboration/v2/") + userId);
            settings.sync();
        }
    }
    m_cloudProjectId.clear();
    m_candidateCloudProjectId.clear();
    m_candidateCloudBackupPath.clear();
    m_cloudLocalBackupPath.clear();
    ++m_projectRevision;
    updateCloudPublicationAction();
    updateCloudSessionActions();
    updateWindowTitle();
}

void MainWindow::onPublishCloudProject() {
    if (!m_cloudPublisher || !m_cloudProjectSync || !m_publicationUi) return;
    if (m_publicationUi->active) {
        const auto answer = QMessageBox::question(
            this, tr("Cancel Cloud Publication"),
            tr("Cancel the current upload? The saved local project will remain "
               "unchanged."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) m_cloudPublisher->cancel();
        return;
    }
    if (!m_cloudProjectId.isEmpty() || !m_candidateCloudProjectId.isEmpty())
        return;

    if (m_controller.isRecording() || m_controller.isCountingIn()) {
        QMessageBox::information(
            this, tr("Finish Recording First"),
            tr("Stop the current recording before publishing the project."));
        return;
    }

    if (m_projectPath.isEmpty() || m_dirty) {
        const auto answer = QMessageBox::question(
            this, tr("Save Before Publishing"),
            tr("Cloud publication starts from a clean saved project. The saved "
               "local .vlt will remain as your backup. Save it now?"),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save);
        if (answer != QMessageBox::Save) return;
        const bool saved = m_projectPath.isEmpty()
                               ? saveProjectAs()
                               : doSave(m_projectPath);
        if (!saved) return;
    }
    if (m_projectPath.isEmpty() || m_dirty ||
        !QFileInfo::exists(m_projectPath)) {
        QMessageBox::warning(
            this, tr("Project Is Not Saved"),
            tr("VLT could not confirm a clean local project package. Save the "
               "project and try again."));
        return;
    }

    statusBar()->showMessage(tr("Capturing built-in plugin state…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::unique_ptr<daw::cloud::CloudPublicationCapture> capture;
    try {
        capture = std::make_unique<daw::cloud::CloudPublicationCapture>(
            m_controller.captureCloudPublicationV1());
    } catch (...) {
        QApplication::restoreOverrideCursor();
        statusBar()->clearMessage();
        QMessageBox::warning(
            this, tr("Project Check Failed"),
            tr("VLT could not stage a safe cloud copy. The local project was "
               "not changed."));
        return;
    }
    QApplication::restoreOverrideCursor();

    if (!capture->readyForHashing()) {
        QStringList lines;
        constexpr qsizetype kMaximumShown = 10;
        for (qsizetype index = 0;
             index < qsizetype(capture->blockers.size()) &&
             index < kMaximumShown;
             ++index) {
            const auto& issue = capture->blockers[std::size_t(index)];
            QString name = QString::fromStdString(issue.displayName).trimmed();
            name.replace(QLatin1Char('\n'), QLatin1Char(' '));
            name.replace(QLatin1Char('\r'), QLatin1Char(' '));
            // A plugin name or source basename is useful; a local directory is
            // never useful in this dialog and must not escape accidentally.
            if (name.contains(QLatin1Char('/')) ||
                name.contains(QLatin1Char('\\')))
                name = QFileInfo(name).fileName();
            if (name.size() > 80) name = name.left(77) + QStringLiteral("…");
            const QString quoted = name.isEmpty()
                                       ? QString()
                                       : tr(" “%1”").arg(name);
            switch (issue.kind) {
                case daw::cloud::PublicationCaptureIssueKind::ThirdPartyPlugin:
                    lines << tr("• Third-party plugin%1 must be removed or "
                                "manually bounced.")
                                 .arg(quoted);
                    break;
                case daw::cloud::PublicationCaptureIssueKind::UnknownInternalPlugin:
                    lines << tr("• Built-in plugin%1 is not compatible with "
                                "cloud projects in this version.")
                                 .arg(quoted);
                    break;
                case daw::cloud::PublicationCaptureIssueKind::MissingLocalSource:
                    lines << tr("• Referenced audio%1 is missing.").arg(quoted);
                    break;
                case daw::cloud::PublicationCaptureIssueKind::UnreadableLocalSource:
                    lines << tr("• Referenced audio%1 cannot be read.").arg(quoted);
                    break;
                case daw::cloud::PublicationCaptureIssueKind::MissingLivePlugin:
                    lines << tr("• Built-in plugin%1 is not available in the "
                                "live audio graph.")
                                 .arg(quoted);
                    break;
                case daw::cloud::PublicationCaptureIssueKind::PluginStateCaptureFailed:
                    lines << tr("• State for built-in plugin%1 could not be "
                                "captured.")
                                 .arg(quoted);
                    break;
                case daw::cloud::PublicationCaptureIssueKind::StagingIo:
                    lines << tr("• Temporary publication storage is unavailable.");
                    break;
            }
        }
        if (capture->blockers.size() > std::size_t(kMaximumShown)) {
            lines << tr("• …and %1 more issue(s).")
                         .arg(capture->blockers.size() -
                              std::size_t(kMaximumShown));
        }
        statusBar()->showMessage(tr("Project is not ready for cloud publication"),
                                 6000);
        QMessageBox::warning(
            this, tr("Project Cannot Be Published"),
            tr("Fix these items, save the project, and try again:\n\n%1")
                .arg(lines.join(QLatin1Char('\n'))));
        return;
    }

    collab::CloudProjectPublicationInput input;
    input.project = capture->document;
    input.metadata.title =
        displayProjectName(m_controller.projectName()).trimmed();
    if (input.metadata.title.size() > 160)
        input.metadata.title.truncate(160);
    const QString version = QCoreApplication::applicationVersion().trimmed();
    input.metadata.engineVersion =
        version.isEmpty() ? QStringLiteral("0.0.0") : version;
    input.metadata.minimumAppVersion = input.metadata.engineVersion;
    input.metadata.formatVersion = daw::collab::kSharedProjectFormatVersion;
    input.assetSources.reserve(qsizetype(capture->sources.size()));
    for (const auto& source : capture->sources) {
        collab::CloudPublicationAssetSource mapped;
        mapped.assetId = QString::fromStdString(source.asset.assetId);
        mapped.sourcePath = QString::fromStdString(source.localPath);
        mapped.contentType = QString::fromStdString(source.asset.mimeType);
        if (mapped.contentType.isEmpty())
            mapped.contentType = QStringLiteral("application/octet-stream");
        input.assetSources.push_back(std::move(mapped));
    }

    m_publicationUi->capture = std::move(capture);
    m_publicationUi->sourcePath = m_projectPath;
    m_publicationUi->sourceRevision = m_projectRevision;
    m_publicationUi->active = true;
    updateCloudPublicationAction();
    updateWindowTitle();
    const quint64 generation = m_cloudPublisher->publish(input);
    if (!generation) {
        m_publicationUi->active = false;
        m_publicationUi->capture.reset();
        m_publicationUi->sourcePath.clear();
        updateCloudPublicationAction();
        updateWindowTitle();
        QMessageBox::warning(
            this, tr("Publish Project Failed"),
            tr("The cloud publication request could not be started. The local "
               "project was not changed."));
        return;
    }
    m_publicationUi->generation = generation;
    statusBar()->showMessage(tr("Checking project for cloud publication…"));
}

void MainWindow::registerNormalizedPresenceSurface(
    QWidget* surface, collab::SurfaceKind kind, const QString& instanceId,
    const QString& ownerId, const QString& objectId) {
    if (!surface || !m_presenceInput || !m_collaboration) return;
    const QString safeOwner = collab::safeSemanticId(ownerId);
    const QString safeObject = collab::safeSemanticId(objectId);
    const collab::SurfaceAddress address =
        presenceAddress(kind, instanceId, ownerId, objectId);
    const QPointer<QWidget> guardedSurface(surface);

    collab::PresenceSurfaceRegistration registration;
    registration.address = address;
    registration.policy = collab::PresencePolicy::Exact;
    registration.encode =
        [guardedSurface, address, safeOwner, safeObject](const QPointF& position)
            -> std::optional<collab::SemanticPoint> {
        if (!guardedSurface) return std::nullopt;
        collab::SemanticPoint point;
        point.surface = address;
        point.normalized =
            collab::normalizedSurfacePoint(position, guardedSurface->size());
        point.trackId = safeOwner;
        point.clipId = safeObject;
        return point;
    };
    m_presenceInput->registerSurface(surface, std::move(registration));

    auto* overlay = new collab::PresenceOverlay(
        surface, address, m_collaboration->presenceStore());
    overlay->setPointMapper(
        [safeOwner, safeObject](const collab::SemanticPoint& point,
                                const QSize& logicalSize)
            -> std::optional<QPointF> {
            if (!presenceContextMatches(point, safeOwner, safeObject))
                return std::nullopt;
            return collab::surfacePointFromNormalized(point, logicalSize);
        });
}

void MainWindow::registerPianoRollPresence() {
    if (!m_pianoRoll || !m_presenceInput || !m_collaboration) return;
    QWidget* surface = m_pianoRoll->collaborationPresenceSurface();
    if (!surface) return;
    if (m_pianoRollPresence) {
        delete m_pianoRollPresence;
        m_pianoRollPresence = nullptr;
    }
    m_presenceInput->unregisterSurface(surface);
    const collab::SurfaceAddress address = presenceAddress(
        collab::SurfaceKind::PianoRoll, QStringLiteral("notes"),
        m_pianoRoll->trackId(), m_pianoRoll->clipId());
    const QPointer<PianoRollWindow> guardedEditor(m_pianoRoll);

    collab::PresenceSurfaceRegistration registration;
    registration.address = address;
    registration.policy = collab::PresencePolicy::Exact;
    registration.encode =
        [guardedEditor](const QPointF& position)
            -> std::optional<collab::SemanticPoint> {
        if (!guardedEditor) return std::nullopt;
        return guardedEditor->collaborationPresenceAt(position);
    };
    m_presenceInput->registerSurface(surface, std::move(registration));
    m_pianoRollPresence = new collab::PresenceOverlay(
        surface, address, m_collaboration->presenceStore());
    m_pianoRollPresence->setPointMapper(
        [guardedEditor](const collab::SemanticPoint& point, const QSize&)
            -> std::optional<QPointF> {
            if (!guardedEditor) return std::nullopt;
            return guardedEditor->collaborationPositionFor(point);
        });
}

void MainWindow::registerAutomationPresence(AutomationEditorWindow* editor) {
    if (!editor || !m_presenceInput || !m_collaboration) return;
    QWidget* surface = editor->collaborationPresenceSurface();
    if (!surface) return;
    const collab::SurfaceAddress address = presenceAddress(
        collab::SurfaceKind::AutomationEditor, QStringLiteral("curve"),
        editor->trackId(), editor->clipId());
    const QPointer<AutomationEditorWindow> guardedEditor(editor);

    collab::PresenceSurfaceRegistration registration;
    registration.address = address;
    registration.policy = collab::PresencePolicy::Exact;
    registration.encode =
        [guardedEditor](const QPointF& position)
            -> std::optional<collab::SemanticPoint> {
        if (!guardedEditor) return std::nullopt;
        return guardedEditor->collaborationPresenceAt(position);
    };
    m_presenceInput->registerSurface(surface, std::move(registration));
    auto* overlay = new collab::PresenceOverlay(
        surface, address, m_collaboration->presenceStore());
    overlay->setPointMapper(
        [guardedEditor](const collab::SemanticPoint& point, const QSize&)
            -> std::optional<QPointF> {
            if (!guardedEditor) return std::nullopt;
            return guardedEditor->collaborationPositionFor(point);
        });
}
#endif

void MainWindow::startRecovery(bool interactive) {
    const QString root = ui::recovery::rootDir();
    // Install this even when ordinary project recovery is disabled. A cloud
    // recording may start the journal on demand later in the run, and a clean
    // quit must preserve its sidecar instead of deleting the whole session.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
#ifdef DAW_ENABLE_COLLABORATION
        if (m_cloudRecording &&
            (m_cloudRecording->phase ==
                 CloudRecordingRuntime::Phase::Capturing ||
             m_cloudRecording->phase ==
                 CloudRecordingRuntime::Phase::FinalizedWriteFailed)) {
            stopCloudRecordingNow(/*interactiveError=*/false);
        }
        if (m_cloudRecording &&
            (m_cloudRecording->phase ==
                 CloudRecordingRuntime::Phase::Acquiring ||
             m_cloudRecording->phase ==
                 CloudRecordingRuntime::Phase::CountingIn)) {
            cancelPendingCloudRecording();
        }
        if (m_cloudRecording || m_preserveCloudRecoverySession ||
            cloudRecordingRecoveryExists()) {
            return;
        }
#endif
        m_journal.stop();
    });
    // A headless run writes into whatever DAW_RECOVERY_ROOT names, and nothing
    // at all when it names nothing — a screenshot must not litter the user's
    // application data or, worse, offer to recover into it.
    if (!interactive && !std::getenv("DAW_RECOVERY_ROOT")) return;
    // Turned off, the leftovers of an earlier session are left where they are
    // rather than deleted: switching recovery off is not a request to throw
    // away work it already saved.
    if (!ui::recoveryprefs::enabled()) return;

    if (interactive) {
        const ui::recovery::Choice choice =
            ui::recovery::offerRecovery(this, m_controller);
        if (choice.restored) {
#ifdef DAW_ENABLE_COLLABORATION
            clearCloudProjectBinding(/*cancelPublication=*/true);
#endif
            m_projectPath = choice.projectPath;
            // Deliberately dirty: the recovered document has not been written
            // to the user's own file, and it must not be until they say so.
            m_dirty = true;
            m_selectedTrackId.clear();
            m_transport->syncTempo();
            syncViews();
            if (!m_controller.project().tracks.empty()) {
                selectTrackFromHeader(QString::fromStdString(
                    m_controller.project().tracks.front().id));
            }
            statusBar()->showMessage(tr("Recovered unsaved work"), 6000);
        }
    }

    m_journal.start(root.toStdString(),
                    QCoreApplication::applicationVersion().toStdString());
    if (!m_journal.running()) return;

    if (!m_projectPath.isEmpty()) {
        m_journal.setProjectPath(m_projectPath.toStdString(),
                                 QFileInfo(m_projectPath).baseName().toStdString());
    }
    // Write once straight away, so a crash in the first seconds still leaves
    // something behind.
    daw::recovery::RecoverySnapshot initial =
        m_controller.captureRecoverySnapshot();
    m_recoveryPluginStateFiles.clear();
    m_recoveryPluginStateFiles.reserve(initial.pluginStates.size());
    for (const auto& state : initial.pluginStates)
        m_recoveryPluginStateFiles.push_back(state.fileName);
    m_journal.requestWrite(std::move(initial));

    // One second: fast enough that little is ever at risk, slow enough that
    // copying the document is invisible next to the 33 ms UI tick.
    m_journalTimer = new QTimer(this);
    connect(m_journalTimer, &QTimer::timeout, this, &MainWindow::sampleForRecovery);
    m_journalTimer->start(1000);

    // The handler cannot save anything either — see crash/CrashHandler.hpp —
    // but it is the only thing that can say WHY, and in a DAW the answer is
    // usually a plugin.
    daw::crash::install(
        QDir(QString::fromStdString(m_journal.sessionDir()))
            .filePath(QString::fromUtf8(daw::recovery::kCrashFile))
            .toStdString());

    // The watchdog: it cannot save anything, but it is the only thing that can
    // notice a freeze, and it keeps the health log a crash report is read from.
    const QString guard =
        ui::recoveryprefs::watchdog() ? ui::recovery::guardPath() : QString();
    if (!guard.isEmpty()) {
        daw::ScanProcess::spawnDetached(
            guard.toStdString(),
            {"--session", m_journal.sessionDir(), "--pid",
             std::to_string(daw::recovery::currentProcessId())},
            &m_guardPipe);
    }

}

void MainWindow::stageAutomationForShot() {
    std::string target;
    for (const auto& track : m_controller.project().tracks) {
        if (track.kind == daw::TrackKind::Audio && !track.clips.empty()) {
            target = track.id;
            break;
        }
    }
    if (target.empty()) return;

    daw::AutomationTarget volume;
    volume.kind = daw::AutomationTargetKind::TrackVolume;
    volume.channelId = target;
    const std::string lane = m_controller.addAutomationLane(target, volume);
    const std::string clip = m_controller.addAutomationClip(lane, volume, 0.0, 8.0);

    // One of each shape, so a grab shows what they look like side by side.
    std::vector<daw::AutomationPoint> shape;
    shape.push_back({0.0, 0.85, daw::AutomationSegment::Linear, 0.0});
    shape.push_back({2.0, 0.25, daw::AutomationSegment::Hold, 0.0});
    shape.push_back({4.0, 0.95, daw::AutomationSegment::Linear, 0.7});
    shape.push_back({6.0, 0.4, daw::AutomationSegment::SCurve, 0.0});
    shape.push_back({8.0, 0.75, daw::AutomationSegment::Linear, 0.0});
    m_controller.setAutomationPoints(lane, clip, shape);
    syncViews();
}

void MainWindow::openAutomationEditorForShot(const QString& mode) {
    stageAutomationForShot();
    for (const auto& track : m_controller.project().tracks) {
        if (!daw::isAutomationLane(track) || track.clips.empty()) continue;
        openAutomationEditor(QString::fromStdString(track.id),
                             QString::fromStdString(track.clips.front().id));
        break;
    }
    if (mode != QStringLiteral("select")) return;
    QApplication::processEvents();
    for (AutomationEditorWindow* editor : m_automationEditors.values()) {
        auto* view = editor->findChild<AutomationCurveView*>();
        if (!view) continue;
        // Shift-drag, the gesture itself, rather than a back door into the
        // selection: a still that shows something the mouse cannot produce is
        // worse than no still.
        const QPoint from(view->width() / 4, view->height() / 2);
        const QPoint to(view->width() * 3 / 4, view->height() / 2);
        const auto send = [&](QEvent::Type type, const QPoint& at,
                              Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent ev(type, QPointF(at), QPointF(view->mapToGlobal(at)),
                           button, held, Qt::ShiftModifier);
            QApplication::sendEvent(view, &ev);
        };
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
    }
}

void MainWindow::setCycleForShot(double fromSeconds, double toSeconds,
                                 bool armed) {
    m_controller.setLoopRangeSeconds(fromSeconds, toSeconds);
    m_controller.setLoopEnabled(armed && toSeconds > fromSeconds);
    m_transport->setCycleEnabled(m_controller.isLoopEnabled());
    syncViews();
}

void MainWindow::selectTracksForShot(const QString& names) {
    QStringList wanted;
    for (const QString& name : names.split(',', Qt::SkipEmptyParts)) {
        const QString needle = name.trimmed().toLower();
        for (const auto& track : m_controller.project().tracks) {
            if (QString::fromStdString(track.name).toLower().startsWith(needle))
                wanted.push_back(QString::fromStdString(track.id));
        }
    }
    if (wanted.isEmpty() || !m_trackList) return;
    m_trackList->setSelectedTracks(wanted, wanted.back());
    selectTrackFromHeader(wanted.back());
}

void MainWindow::nestDemoTracksForShot(int depth) {
    std::string parent;
    for (int level = 0; level < depth; ++level) {
        const std::string folder = m_controller.addFolder(
            level % 2 == 1, "Level " + std::to_string(level + 1));
        if (!parent.empty()) m_controller.moveTrackToFolder(folder, parent);
        parent = folder;
    }
    const std::string leaf =
        m_controller.addTrack(daw::TrackKind::Audio, "Nested");
    if (!parent.empty()) m_controller.moveTrackToFolder(leaf, parent);
    syncViews();
}

bool MainWindow::checkAutomationForTest() {
    if (!m_timeline || !m_trackList) return false;

    const std::string track =
        m_controller.addTrack(daw::TrackKind::Audio, "Automated");
    syncViews();
    m_trackList->setSelectedTrack(QString::fromStdString(track));
    selectTrackFromHeader(QString::fromStdString(track));
    QApplication::processEvents();

    // What the A key does: the first ask makes a volume lane with a curve on
    // it, so there is something to draw into straight away.
    toggleAutomationLanes();
    QApplication::processEvents();
    m_transport->setToolIndex(0);  // Select edits the curve, not the clip.
    m_timeline->setSnapEnabled(false);
    const std::vector<std::string> lanes = m_controller.automationLanesOf(track);
    if (lanes.size() != 1) {
        std::fprintf(stderr, "opening automation made %d lanes, wanted one\n",
                     int(lanes.size()));
        return false;
    }
    const std::string lane = lanes.front();
    const daw::TrackModel* laneTrack = m_controller.project().findTrack(lane);
    if (!laneTrack || laneTrack->clips.size() != 1 ||
        laneTrack->clips.front().kind != daw::ClipKind::Automation) {
        std::fprintf(stderr, "the new lane has no curve on it\n");
        return false;
    }
    const std::string clip = laneTrack->clips.front().id;
    const auto curve = [&]() -> const daw::ClipAutomationModel* {
        const daw::TrackModel* t = m_controller.project().findTrack(lane);
        if (!t) return nullptr;
        for (const auto& c : t->clips) {
            if (c.id == clip) return &c.automation;
        }
        return nullptr;
    };
    if (!curve() || curve()->target.kind != daw::AutomationTargetKind::TrackVolume) {
        std::fprintf(stderr, "the default lane does not automate volume\n");
        return false;
    }

    // Find the lane's row on screen, so the clicks land where the user's would.
    const auto& rows = daw::visibleTracks(m_controller.project());
    int laneRow = -1;
    for (int i = 0; i < int(rows.size()); ++i) {
        if (m_controller.project().tracks[rows[size_t(i)].index].id == lane)
            laneRow = i;
    }
    if (laneRow < 0) {
        std::fprintf(stderr, "the automation lane is not visible\n");
        return false;
    }
    // The new track is at the bottom of a project that already has the demo in
    // it, so the lane has to be brought into view before it can be clicked —
    // exactly as the arrangement does when a lane is opened for real.
    m_timeline->ensureLaneVisible(laneRow);
    QApplication::processEvents();
    const int y = m_timeline->laneCentreForTest(laneRow);
    if (y < ui::kRulerHeight || y > m_timeline->height()) {
        std::fprintf(stderr, "the automation lane is off screen at y=%d\n", y);
        return false;
    }

    const auto send = [&](QEvent::Type type, const QPoint& at,
                          Qt::MouseButton button, Qt::MouseButtons held,
                          Qt::KeyboardModifiers mods) {
        QMouseEvent ev(type, QPointF(at), QPointF(m_timeline->mapToGlobal(at)),
                       button, held, mods);
        QApplication::sendEvent(m_timeline, &ev);
    };
    const auto click = [&](const QPoint& from, const QPoint& to,
                           Qt::KeyboardModifiers mods) {
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
        send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, mods);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
        QApplication::processEvents();
    };

    // ── A click on empty curve adds a breakpoint and places it in one gesture ──
    const std::size_t before = curve()->points.size();
    const QPoint at(m_timeline->width() / 3, y);
    click(at, at, Qt::NoModifier);
    if (!curve() || curve()->points.size() != before + 1) {
        std::fprintf(stderr, "clicking the curve did not add a point\n");
        return false;
    }

    // ── And the same gesture drags it ──
    const double placed = curve()->points[1].value;
    const QPoint lower(at.x(), at.y() + 14);
    click(at, lower, Qt::NoModifier);
    const daw::ClipAutomationModel* after = curve();
    if (!after) return false;
    if (after->points.size() != before + 1 ||
        after->points[1].value >= placed - 0.02) {
        std::fprintf(stderr, "dragging one automation point created another\n");
        return false;
    }
    double moved = placed;
    for (const auto& point : after->points) {
        // The one nearest where the pointer let go.
        if (std::abs(point.value - placed) < std::abs(moved - placed)) continue;
        moved = point.value;
    }
    bool dragged = false;
    for (const auto& point : after->points) {
        if (std::abs(point.value - placed) > 0.02) dragged = true;
    }
    if (!dragged) {
        std::fprintf(stderr, "dragging a breakpoint did not move it\n");
        return false;
    }

    // ── Shift moves a point in time and locks its value ──
    const double lockedValue = after->points[1].value;
    const double lockedFrom = after->points[1].beats;
    const QPoint right(lower.x() + 40, lower.y() + 25);
    click(lower, right, Qt::ShiftModifier);
    after = curve();
    bool horizontalOnly = false;
    if (after) {
        for (const auto& point : after->points) {
            if (point.beats > lockedFrom + 1e-5 &&
                std::abs(point.value - lockedValue) < 1e-6) {
                horizontalOnly = true;
            }
        }
    }
    if (!horizontalOnly) {
        std::fprintf(stderr, "Shift-drag changed a point's value\n");
        return false;
    }

    // ── Double-click returns that point to the target's neutral value ──
    const QPoint movedPoint(right.x(), lower.y());
    send(QEvent::MouseButtonPress, movedPoint, Qt::LeftButton, Qt::LeftButton,
         Qt::NoModifier);
    send(QEvent::MouseButtonRelease, movedPoint, Qt::LeftButton, Qt::NoButton,
         Qt::NoModifier);
    send(QEvent::MouseButtonDblClick, movedPoint, Qt::LeftButton, Qt::LeftButton,
         Qt::NoModifier);
    send(QEvent::MouseButtonRelease, movedPoint, Qt::LeftButton, Qt::NoButton,
         Qt::NoModifier);
    QApplication::processEvents();
    const double reset = m_controller.automationResetValue(curve()->target);
    bool resetFound = false;
    for (const auto& point : curve()->points) {
        if (std::abs(point.value - reset) < 1e-6) resetFound = true;
    }
    if (!resetFound) {
        std::fprintf(stderr, "double-clicking an automation point did not reset it\n");
        return false;
    }

    // ── Alt-dragging a run bends it ──
    // Between two points, away from either, so the press is on the segment and
    // not on a handle.
    {
        // Wide enough that the press below lands *between* the two points at
        // whatever zoom the window happens to be at — a segment is only there
        // to be bent while the pointer is on it.
        std::vector<daw::AutomationPoint> two;
        two.push_back({0.0, 0.2, daw::AutomationSegment::Linear, 0.0});
        two.push_back({64.0, 0.8, daw::AutomationSegment::Linear, 0.0});
        m_controller.setAutomationPoints(lane, clip, two);
        QApplication::processEvents();

        const QPoint mid(m_timeline->width() / 2, y);
        send(QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton,
             Qt::ControlModifier);
        const QPoint up(mid.x(), mid.y() - 30);
        send(QEvent::MouseMove, up, Qt::NoButton, Qt::LeftButton,
             Qt::ControlModifier);
        send(QEvent::MouseButtonRelease, up, Qt::LeftButton, Qt::NoButton,
             Qt::ControlModifier);
        QApplication::processEvents();

        const daw::ClipAutomationModel* bent = curve();
        if (!bent || bent->points.size() != 2) {
            std::fprintf(stderr, "bending a segment changed the points\n");
            return false;
        }
        if (std::abs(bent->points.front().curve) < 0.05) {
            std::fprintf(stderr, "Alt-dragging a segment did not bend it\n");
            return false;
        }
        // A bend is a shape, not a move: the ends stay exactly where they were.
        if (std::abs(bent->points.front().value - 0.2) > 1e-6 ||
            std::abs(bent->points.back().value - 0.8) > 1e-6) {
            std::fprintf(stderr, "the bend moved the points it runs between\n");
            return false;
        }

        // One undo entry for the whole gesture.
        m_controller.undo();
        const daw::ClipAutomationModel* undone = curve();
        if (!undone || std::abs(undone->points.front().curve) > 1e-9) {
            std::fprintf(stderr, "one undo did not take the whole bend back\n");
            return false;
        }
    }

    // ── The grip drags the clip; the curve does not ──
    //
    // Everywhere but the strip along the top, the pointer is editing the curve,
    // which left the clip itself with nothing to take hold of.
    {
        daw::AutomationTarget pan;
        pan.kind = daw::AutomationTargetKind::TrackPan;
        pan.channelId = track;
        const std::string second = m_controller.addAutomationLane(track, pan);
        syncViews();
        QApplication::processEvents();

        const auto rowOf = [&](const std::string& id) {
            const auto& order = daw::visibleTracks(m_controller.project());
            for (int i = 0; i < int(order.size()); ++i) {
                if (m_controller.project().tracks[order[std::size_t(i)].index].id == id)
                    return i;
            }
            return -1;
        };
        const int fromRow = rowOf(lane);
        const int toRow = rowOf(second);
        if (fromRow < 0 || toRow < 0) {
            std::fprintf(stderr, "a lane is not on screen to drag between\n");
            return false;
        }
        m_timeline->ensureLaneVisible(toRow);
        QApplication::processEvents();

        // A recorded clip can start between grid lines. Moving it straight to
        // another lane must not silently quantise that placement.
        constexpr double offGridStart = 0.137;
        m_controller.setClipStartSeconds(lane, clip, offGridStart);
        m_timeline->setSnapEnabled(true);
        QApplication::processEvents();

        // The grip is the top of the lane's row, which is where the clip's name
        // is drawn; the curve owns everything under it.
        const int gripY = m_timeline->laneTopForTest(fromRow) + 6;
        const int dropY = m_timeline->laneCentreForTest(toRow);
        const std::size_t pointsBefore = curve()->points.size();
        const QPoint grip(m_timeline->width() / 3, gripY);
        const QPoint drop(grip.x(), dropY);
        send(QEvent::MouseButtonPress, grip, Qt::LeftButton, Qt::LeftButton,
             Qt::NoModifier);
        send(QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        send(QEvent::MouseButtonRelease, drop, Qt::LeftButton, Qt::NoButton,
             Qt::NoModifier);
        QApplication::processEvents();

        const daw::TrackModel* landed = m_controller.project().findTrack(second);
        if (!landed || landed->clips.size() != 1 || landed->clips.front().id != clip) {
            std::fprintf(stderr, "dragging by the grip did not move the clip\n");
            return false;
        }
        if (std::abs(landed->clips.front().startSeconds - offGridStart) > 1e-9) {
            std::fprintf(stderr,
                         "a vertical clip drag snapped its off-grid start\n");
            return false;
        }
        if (landed->clips.front().automation.points.size() != pointsBefore) {
            std::fprintf(stderr, "the drag changed the curve it carried\n");
            return false;
        }
        // And the grip is not a place points appear.
        const daw::TrackModel* left = m_controller.project().findTrack(lane);
        if (!left || !left->clips.empty()) {
            std::fprintf(stderr, "the clip is on both lanes at once\n");
            return false;
        }

        // Pull it away horizontally, then return within the quiet 6 px guide
        // detent. The clip must land back on its exact off-grid start rather
        // than on the neighbouring grid line.
        const QPoint landedGrip(grip.x(),
                                m_timeline->laneTopForTest(toRow) + 6);
        send(QEvent::MouseButtonPress, landedGrip, Qt::LeftButton,
             Qt::LeftButton, Qt::NoModifier);
        send(QEvent::MouseMove, landedGrip + QPoint(20, 0), Qt::NoButton,
             Qt::LeftButton, Qt::NoModifier);
        send(QEvent::MouseMove, landedGrip + QPoint(4, 0), Qt::NoButton,
             Qt::LeftButton, Qt::NoModifier);
        send(QEvent::MouseButtonRelease, landedGrip + QPoint(4, 0),
             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::processEvents();
        landed = m_controller.project().findTrack(second);
        if (!landed || landed->clips.empty() ||
            std::abs(landed->clips.front().startSeconds - offGridStart) > 1e-9) {
            std::fprintf(stderr,
                         "the original-position guide did not catch the clip\n");
            return false;
        }

        // ── Under another tool the clip is a clip ──
        // The knife splits it instead of dropping a breakpoint into it.
        m_timeline->setTool(TimelineWidget::Tool::Knife);
        const QPoint cut(m_timeline->width() / 2, dropY);
        send(QEvent::MouseButtonPress, cut, Qt::LeftButton, Qt::LeftButton,
             Qt::NoModifier);
        send(QEvent::MouseButtonRelease, cut, Qt::LeftButton, Qt::NoButton,
             Qt::NoModifier);
        QApplication::processEvents();
        m_timeline->setTool(TimelineWidget::Tool::Select);

        const daw::TrackModel* afterCut = m_controller.project().findTrack(second);
        if (!afterCut || afterCut->clips.size() != 2) {
            std::fprintf(stderr,
                         "the knife made %d clips out of a curve, wanted two\n",
                         int(afterCut ? afterCut->clips.size() : 0));
            return false;
        }
        for (const auto& piece : afterCut->clips) {
            if (piece.kind != daw::ClipKind::Automation) {
                std::fprintf(stderr, "the knife changed what the clip is\n");
                return false;
            }
        }
        m_controller.removeAutomationLane(second);
    }

    m_controller.removeAutomationLane(lane);
    m_controller.removeTrack(track);
    syncViews();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkAutomationEditorForTest() {
    if (!m_timeline) return false;

    const std::string track =
        m_controller.addTrack(daw::TrackKind::Audio, "Retargeted");
    daw::AutomationTarget volume;
    volume.kind = daw::AutomationTargetKind::TrackVolume;
    volume.channelId = track;
    const auto [lane, clip] = m_controller.ensureAutomation(volume);
    if (lane.empty() || clip.empty()) {
        std::fprintf(stderr, "ensureAutomation made no curve\n");
        return false;
    }
    // Asking twice must not make a second one — this is what a double-click on
    // a knob does when the user does not notice the lane appear.
    const auto again = m_controller.ensureAutomation(volume);
    if (again.first != lane || again.second != clip ||
        m_controller.automationLanesOf(track).size() != 1) {
        std::fprintf(stderr, "automating the same thing twice made two lanes\n");
        return false;
    }

    std::vector<daw::AutomationPoint> shape;
    shape.push_back({0.0, 0.2, daw::AutomationSegment::Linear, 0.0});
    shape.push_back({4.0, 0.9, daw::AutomationSegment::Hold, 0.0});
    shape.push_back({8.0, 0.4, daw::AutomationSegment::Linear, 0.0});
    m_controller.setAutomationPoints(lane, clip, shape);
    syncViews();
    QApplication::processEvents();

    openAutomationEditor(QString::fromStdString(lane), QString::fromStdString(clip));
    QApplication::processEvents();
    AutomationEditorWindow* editor = m_automationEditors.value(
        QString::fromStdString(lane) + '/' + QString::fromStdString(clip), nullptr);
    if (!editor) {
        std::fprintf(stderr, "the automation editor did not open\n");
        return false;
    }
    // Opening it again raises the one that is open rather than making a second.
    openAutomationEditor(QString::fromStdString(lane), QString::fromStdString(clip));
    QApplication::processEvents();
    if (m_automationEditors.size() != 1) {
        std::fprintf(stderr, "a second editor opened on the same curve\n");
        return false;
    }

    const auto curve = [&]() -> const daw::ClipAutomationModel* {
        const daw::TrackModel* t = m_controller.project().findTrack(lane);
        if (!t) return nullptr;
        for (const auto& c : t->clips) {
            if (c.id == clip) return &c.automation;
        }
        return nullptr;
    };

    // ── The three fields say what the curve drives ──
    const QList<QComboBox*> fields = editor->findChildren<QComboBox*>();
    if (fields.size() < 3) {
        std::fprintf(stderr, "the editor has %d fields, wanted at least three\n",
                     int(fields.size()));
        editor->close();
        return false;
    }
    QComboBox* channelField = fields.at(0);
    QComboBox* whatField = fields.at(1);
    if (channelField->currentData().toString().toStdString() != track) {
        std::fprintf(stderr, "the channel field does not name the track driven\n");
        editor->close();
        return false;
    }
    if (whatField->currentData(Qt::UserRole + 1).toInt() !=
        int(daw::AutomationTargetKind::TrackVolume)) {
        std::fprintf(stderr, "the target field does not say Volume\n");
        editor->close();
        return false;
    }

    // ── Re-pointing keeps every point ──
    // The whole reason the target lives on the clip: a curve that took a while
    // to shape is pointed somewhere else without being drawn again.
    const int panIndex = whatField->findData(
        int(daw::AutomationTargetKind::TrackPan), Qt::UserRole + 1);
    if (panIndex < 0) {
        std::fprintf(stderr, "the target field does not offer Pan\n");
        editor->close();
        return false;
    }
    whatField->setCurrentIndex(panIndex);
    QApplication::processEvents();
    if (!curve() || curve()->target.kind != daw::AutomationTargetKind::TrackPan) {
        std::fprintf(stderr, "choosing Pan did not re-point the curve\n");
        editor->close();
        return false;
    }
    if (!curve() || curve()->points != shape) {
        std::fprintf(stderr, "re-pointing the curve changed its points\n");
        editor->close();
        return false;
    }

    // ── A transform runs on the curve and undoes in one step ──
    auto* view = editor->findChild<AutomationCurveView*>();
    if (!view) {
        std::fprintf(stderr, "the editor has no curve view\n");
        editor->close();
        return false;
    }

    const auto sendView = [&](QEvent::Type type, const QPoint& at,
                              Qt::MouseButton button, Qt::MouseButtons held,
                              Qt::KeyboardModifiers mods) {
        QMouseEvent ev(type, QPointF(at), QPointF(view->mapToGlobal(at)), button,
                       held, mods);
        QApplication::sendEvent(view, &ev);
    };

    // ── The large editor uses the same constrained point gestures as the lane ──
    const QPoint pointAtFour = view->pointPositionForTest(1);
    const QPoint shifted(pointAtFour.x() + 48, pointAtFour.y() + 35);
    sendView(QEvent::MouseButtonPress, pointAtFour, Qt::LeftButton,
             Qt::LeftButton, Qt::ShiftModifier | Qt::AltModifier);
    sendView(QEvent::MouseMove, shifted, Qt::NoButton, Qt::LeftButton,
             Qt::ShiftModifier | Qt::AltModifier);
    sendView(QEvent::MouseButtonRelease, shifted, Qt::LeftButton, Qt::NoButton,
             Qt::ShiftModifier | Qt::AltModifier);
    QApplication::processEvents();
    bool editorHorizontalOnly = false;
    if (curve()) {
        for (const auto& point : curve()->points) {
            if (point.beats > 4.0 + 1e-5 && std::abs(point.value - 0.9) < 1e-6)
                editorHorizontalOnly = true;
        }
    }
    if (!editorHorizontalOnly) {
        std::fprintf(stderr, "the editor's Shift-drag did not lock point value\n");
        editor->close();
        return false;
    }
    m_controller.undo();
    QApplication::processEvents();
    if (!curve() || curve()->points != shape) {
        std::fprintf(stderr, "undo did not restore the editor's Shift-drag\n");
        editor->close();
        return false;
    }

    sendView(QEvent::MouseButtonPress, pointAtFour, Qt::LeftButton,
             Qt::LeftButton, Qt::NoModifier);
    sendView(QEvent::MouseButtonRelease, pointAtFour, Qt::LeftButton,
             Qt::NoButton, Qt::NoModifier);
    sendView(QEvent::MouseButtonDblClick, pointAtFour, Qt::LeftButton,
             Qt::LeftButton, Qt::NoModifier);
    sendView(QEvent::MouseButtonRelease, pointAtFour, Qt::LeftButton,
             Qt::NoButton, Qt::NoModifier);
    QApplication::processEvents();
    if (!curve() || std::abs(curve()->points[1].value - 0.5) > 1e-6) {
        std::fprintf(stderr, "the editor's double-click did not reset pan to centre\n");
        editor->close();
        return false;
    }
    m_controller.undo();
    QApplication::processEvents();
    if (!curve() || curve()->points != shape) {
        std::fprintf(stderr, "undo did not restore the editor's point reset\n");
        editor->close();
        return false;
    }

    // Nothing selected means the whole clip, which is what the toolbar acts on.
    const daw::autotools::Range whole = view->range();
    if (whole.fromBeats > 0.0 || whole.toBeats < 8.0) {
        std::fprintf(stderr, "with no selection the range is not the whole clip\n");
        editor->close();
        return false;
    }
    view->applyPoints(daw::autotools::invert(view->points(), whole),
                      QStringLiteral("Invert"));
    QApplication::processEvents();
    if (!curve() || std::abs(curve()->points.front().value - 0.8) > 1e-6) {
        std::fprintf(stderr, "the transform did not reach the document\n");
        editor->close();
        return false;
    }
    m_controller.undo();
    if (!curve() || curve()->points != shape) {
        std::fprintf(stderr, "one undo did not take the whole transform back\n");
        editor->close();
        return false;
    }

    // ── A preview is audible and free ──
    // It goes through the live path, so it changes what plays without touching
    // the undo stack; cancelling puts the curve back exactly.
    const std::size_t undoDepth = m_controller.undoDepth();
    daw::autotools::LfoSpec spec;
    spec.wave = daw::autotools::LfoWave::Triangle;
    spec.lengthBeats = 8.0;
    spec.rateBeats = 2.0;
    view->showPreview(daw::autotools::lfo(spec));
    QApplication::processEvents();
    if (m_controller.undoDepth() != undoDepth) {
        std::fprintf(stderr, "a preview pushed an undo entry\n");
        editor->close();
        return false;
    }
    if (!curve() || curve()->points == shape) {
        std::fprintf(stderr, "a preview did not reach the engine\n");
        editor->close();
        return false;
    }
    view->cancelPreview();
    QApplication::processEvents();
    if (!curve() || curve()->points != shape ||
        m_controller.undoDepth() != undoDepth) {
        std::fprintf(stderr, "cancelling a preview did not put the curve back\n");
        editor->close();
        return false;
    }

    editor->close();
    QApplication::processEvents();
    if (!m_automationEditors.isEmpty()) {
        std::fprintf(stderr, "the editor stayed in the registry after closing\n");
        return false;
    }

    // ── Double-clicking the grip opens the editor; the curve is for editing ──
    //
    // Two clicks on a curve place two points, so the editor is opened from the
    // strip along the top that is not the curve.
    syncViews();
    QApplication::processEvents();
    const auto& rows = daw::visibleTracks(m_controller.project());
    int laneRow = -1;
    for (int i = 0; i < int(rows.size()); ++i) {
        if (m_controller.project().tracks[rows[std::size_t(i)].index].id == lane)
            laneRow = i;
    }
    if (laneRow < 0) {
        std::fprintf(stderr, "the automation lane is not visible\n");
        return false;
    }
    m_timeline->ensureLaneVisible(laneRow);
    QApplication::processEvents();
    const int x = m_timeline->width() / 3;
    const std::size_t pointsBefore = curve() ? curve()->points.size() : 0;
    const auto sendAt = [&](QEvent::Type type, const QPoint& at,
                            Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent ev(type, QPointF(at), QPointF(m_timeline->mapToGlobal(at)),
                       button, held, Qt::NoModifier);
        QApplication::sendEvent(m_timeline, &ev);
    };
    const auto doubleClickAt = [&](const QPoint& at) {
        sendAt(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        sendAt(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        sendAt(QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
        sendAt(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
    };

    // In the curve, with the pointer tool: this is drawing, not opening.
    doubleClickAt(QPoint(x, m_timeline->laneCentreForTest(laneRow)));
    if (!m_automationEditors.isEmpty()) {
        std::fprintf(stderr, "double-clicking the curve opened the editor\n");
        for (AutomationEditorWindow* open : m_automationEditors.values()) open->close();
        return false;
    }
    // Put back whatever those two clicks drew, so the count below is honest.
    while (curve() && curve()->points.size() > pointsBefore) m_controller.undo();

    doubleClickAt(QPoint(x, m_timeline->laneTopForTest(laneRow) + 6));
    if (m_automationEditors.size() != 1) {
        std::fprintf(stderr, "double-clicking the grip did not open the editor\n");
        return false;
    }
    if (!curve() || curve()->points.size() != pointsBefore) {
        std::fprintf(stderr,
                     "opening the editor left %d points behind, wanted %d\n",
                     int(curve() ? curve()->points.size() : 0), int(pointsBefore));
        return false;
    }

    // ── A curve that goes takes its editor with it ──
    // Deleting the clip under an open window used to leave it editing a ghost.
    m_controller.removeClip(lane, clip);
    syncViews();
    QApplication::processEvents();
    if (!m_automationEditors.isEmpty()) {
        std::fprintf(stderr, "the editor outlived the curve it was editing\n");
        for (AutomationEditorWindow* open : m_automationEditors.values()) open->close();
        return false;
    }

    m_controller.removeAutomationLane(lane);
    m_controller.removeTrack(track);
    syncViews();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkKnobAutomationForTest() {
    // The built-in Sampler is part of this build, so this needs no scanned
    // plugin and runs the same on any machine.
    if (!openDemoSampler(QString())) {
        std::fprintf(stderr, "the demo sampler did not load\n");
        return false;
    }
    QApplication::processEvents();

    std::string track;
    for (const auto& t : m_controller.project().tracks) {
        if (t.kind == daw::TrackKind::Instrument || t.kind == daw::TrackKind::Midi) {
            track = t.id;
            break;
        }
    }
    if (track.empty()) {
        std::fprintf(stderr, "no instrument track to automate on\n");
        return false;
    }
    const std::string instrument =
        m_controller.project().findTrack(track)->instrument.id;
    openPluginEditor(QString::fromStdString(track),
                     QString::fromStdString(instrument));
    QApplication::processEvents();
    PluginEditorWindow* editor = m_pluginEditors.value(
        QString::fromStdString(track) + '/' + QString::fromStdString(instrument),
        nullptr);
    if (!editor) {
        std::fprintf(stderr, "the sampler's editor did not open\n");
        return false;
    }

    ui::Knob* knob = nullptr;
    for (ui::Knob* candidate : editor->findChildren<ui::Knob*>()) {
        if (candidate->isAutomatable()) {
            knob = candidate;
            break;
        }
    }
    if (!knob) {
        std::fprintf(stderr, "no knob in the editor offers automation\n");
        editor->close();
        return false;
    }

    const auto doubleClick = [&](Qt::KeyboardModifiers mods) {
        const QPoint at(knob->width() / 2, knob->height() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(at),
                          QPointF(knob->mapToGlobal(at)), Qt::LeftButton,
                          Qt::LeftButton, mods);
        QApplication::sendEvent(knob, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at),
                            QPointF(knob->mapToGlobal(at)), Qt::LeftButton,
                            Qt::NoButton, mods);
        QApplication::sendEvent(knob, &release);
        QMouseEvent twice(QEvent::MouseButtonDblClick, QPointF(at),
                          QPointF(knob->mapToGlobal(at)), Qt::LeftButton,
                          Qt::LeftButton, mods);
        QApplication::sendEvent(knob, &twice);
        QApplication::processEvents();
    };

    const std::size_t lanesBefore = m_controller.automationLanesOf(track).size();

    // ── Plain double-click resets and never creates automation ──
    const double moved = knob->value() + (knob->value() > 0.5 ? -0.25 : 0.25);
    knob->setValue(moved);
    QApplication::processEvents();
    doubleClick(Qt::NoModifier);
    if (std::abs(knob->value() - moved) < 1e-9) {
        std::fprintf(stderr, "plain double-click no longer resets the knob\n");
        editor->close();
        return false;
    }
    if (m_controller.automationLanesOf(track).size() != lanesBefore) {
        std::fprintf(stderr, "plain double-click made an automation lane\n");
        editor->close();
        return false;
    }

    // ── Alt/Option+double-click creates it ──
    doubleClick(Qt::AltModifier);

    const std::vector<std::string> lanes = m_controller.automationLanesOf(track);
    if (lanes.size() != lanesBefore + 1) {
        std::fprintf(stderr,
                     "Alt/Option-double-clicking a knob made %d lanes, wanted one more than %d\n",
                     int(lanes.size()), int(lanesBefore));
        editor->close();
        return false;
    }
    const daw::TrackModel* lane = m_controller.project().findTrack(lanes.back());
    if (!lane || lane->clips.size() != 1) {
        std::fprintf(stderr, "the new lane has no curve on it\n");
        editor->close();
        return false;
    }
    const daw::AutomationTarget& target = lane->clips.front().automation.target;
    if (target.kind != daw::AutomationTargetKind::PluginParameter ||
        target.channelId != track || target.parameterId.empty()) {
        std::fprintf(stderr, "the curve does not point at a plugin parameter\n");
        editor->close();
        return false;
    }
    // The instrument slot is spelled as an empty slot id, not as its uuid —
    // the convention every automation path here shares.
    if (!target.slotId.empty()) {
        std::fprintf(stderr, "the instrument slot was not spelled as empty\n");
        editor->close();
        return false;
    }

    // Asking again reveals what is there rather than piling up lanes.
    doubleClick(Qt::AltModifier);
    if (m_controller.automationLanesOf(track).size() != lanes.size()) {
        std::fprintf(stderr,
                     "Alt/Option-double-clicking the same knob twice made two lanes\n");
        editor->close();
        return false;
    }

    // ── Holding Alt/Option lights the toolbar mode, releasing clears it ──
    auto* createMode = m_toolPanel
                           ? m_toolPanel->findChild<QAbstractButton*>(
                                 QStringLiteral("AutomationCreateMode"))
                           : nullptr;
    if (!createMode) {
        std::fprintf(stderr, "the automation creation button is missing\n");
        editor->close();
        return false;
    }
    QKeyEvent altPress(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    QApplication::sendEvent(knob, &altPress);
    if (!createMode->isChecked()) {
        std::fprintf(stderr, "holding Alt/Option did not light automation mode\n");
        editor->close();
        return false;
    }
    QKeyEvent altRelease(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    QApplication::sendEvent(knob, &altRelease);
    if (createMode->isChecked()) {
        std::fprintf(stderr, "releasing Alt/Option left automation mode on\n");
        editor->close();
        return false;
    }

    // ── Clicking the toolbar button latches the same gesture ──
    for (const std::string& id : m_controller.automationLanesOf(track))
        m_controller.removeAutomationLane(id);
    syncViews();
    QApplication::processEvents();
    createMode->click();
    if (!createMode->isChecked() || !ui::automationCreationMode()) {
        std::fprintf(stderr, "the automation creation button did not latch\n");
        editor->close();
        return false;
    }
    doubleClick(Qt::NoModifier);
    if (m_controller.automationLanesOf(track).size() != 1) {
        std::fprintf(stderr, "latched creation mode did not automate the knob\n");
        editor->close();
        return false;
    }
    createMode->click();
    if (createMode->isChecked() || ui::automationCreationMode()) {
        std::fprintf(stderr, "the automation creation mode did not switch off\n");
        editor->close();
        return false;
    }

    editor->close();
    QApplication::processEvents();
    for (const std::string& id : m_controller.automationLanesOf(track))
        m_controller.removeAutomationLane(id);
    syncViews();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkParameterDockForTest() {
    if (!openDemoSampler(QString())) {
        std::fprintf(stderr, "the demo sampler did not load\n");
        return false;
    }
    QApplication::processEvents();

    std::string track;
    for (const auto& t : m_controller.project().tracks) {
        if (t.kind == daw::TrackKind::Instrument || t.kind == daw::TrackKind::Midi) {
            track = t.id;
            break;
        }
    }
    if (track.empty()) return false;
    const std::string slot = m_controller.project().findTrack(track)->instrument.id;
    openPluginEditor(QString::fromStdString(track), QString::fromStdString(slot));
    QApplication::processEvents();
    PluginEditorWindow* editor = m_pluginEditors.value(
        QString::fromStdString(track) + '/' + QString::fromStdString(slot), nullptr);
    if (!editor) {
        std::fprintf(stderr, "the plugin editor did not open\n");
        return false;
    }
    // Opening is deferred across two event-loop turns and then a settling
    // delay, so the plugin's own controller can finish before it is asked for a
    // GUI. WaitForMoreEvents, not a plain pump: processEvents returns the
    // instant the queue is empty, and a timer that has not come due yet is not
    // an event — spinning on it would never let the editor finish.
    QElapsedTimer settling;
    settling.start();
    while (!editor->isEditorInitialized() && settling.elapsed() < 4000) {
        QApplication::processEvents(
            QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 20);
    }
    InternalEditorFrame* pluginFrame =
        m_internalEditorFrames.value(editor, nullptr);
    if (!pluginFrame || !m_editorHost || !editor->isEditorInitialized() ||
        editor->isWindow() ||
        pluginFrame->isWindow() || editor->parentWidget() != pluginFrame) {
        std::fprintf(stderr,
                     "the plugin editor did not initialize in internal chrome\n");
        editor->close();
        return false;
    }

    // A native GUI can report a nonsensical desktop-sized natural size. The
    // production clamp must keep it inside the chosen screen, and editor
    // windows must never be permanently pinned after their first layout.
    const QSize bounded = PluginEditorWindow::boundedWindowSizeForTest(
        QSize(8000, 6000), QSize(1200, 700));
    if (bounded != QSize(1200, 700) ||
        editor->minimumSize() == editor->maximumSize()) {
        std::fprintf(stderr,
                     "the plugin editor is not compact/resizable (%dx%d, locked %d)\n",
                     bounded.width(), bounded.height(),
                     int(editor->minimumSize() == editor->maximumSize()));
        editor->close();
        return false;
    }
    // ── The dock widens the internal frame; it never takes width from the plugin ──
    //
    // Taking it from the view crops the plugin, and because the width is
    // measured back from that view, every toggle used to crop it again.
    const int closed = pluginFrame->width();
    editor->setParameterDockVisibleForTest(true);
    QApplication::processEvents();
    const int opened = pluginFrame->width();
    const int hostWidth = m_editorHost->width();
    const bool hostBounded = hostWidth > 0 && closed >= hostWidth - 2;
    if (opened <= closed && !hostBounded) {
        std::fprintf(stderr,
                     "opening the dock did not widen the frame (%d → %d, host %d)\n",
                     closed, opened, hostWidth);
        editor->close();
        return false;
    }
    editor->setParameterDockVisibleForTest(false);
    QApplication::processEvents();
    if (pluginFrame->width() != closed) {
        std::fprintf(stderr, "closing the dock left the frame %d wide, not %d\n",
                     pluginFrame->width(), closed);
        editor->close();
        return false;
    }
    editor->setParameterDockVisibleForTest(true);
    QApplication::processEvents();
    if (pluginFrame->width() != opened) {
        std::fprintf(stderr, "the second open drifted: %d, not %d\n",
                     pluginFrame->width(), opened);
        editor->close();
        return false;
    }

    // ── What the plugin moves rises to the top, marked active ──
    const QStringList order = editor->parameterDockOrderForTest();
    if (order.size() < 3) {
        std::fprintf(stderr, "the dock has %d parameters, wanted a few\n",
                     int(order.size()));
        editor->close();
        return false;
    }
    // Something well down the list, written straight to the slot — which is
    // what a plugin's own GUI moving a knob looks like from here.
    const QString moved = order.at(2);
    const double was = m_controller.insertParameter(track, slot,
                                                    moved.toStdString());
    m_controller.setInsertParameter(track, slot, moved.toStdString(), was + 0.1);
    editor->pollForTest();
    QApplication::processEvents();

    if (editor->parameterDockActiveForTest() != moved) {
        std::fprintf(stderr, "the dock did not mark the moved parameter active\n");
        editor->close();
        return false;
    }
    if (editor->parameterDockOrderForTest().value(0) != moved) {
        std::fprintf(stderr, "the moved parameter did not rise to the top\n");
        editor->close();
        return false;
    }
    // And a knob the user is turning is not mistaken for one the plugin moved:
    // the value we just wrote is now on the knob, so a second poll changes
    // nothing at all.
    const QStringList settled = editor->parameterDockOrderForTest();
    editor->pollForTest();
    QApplication::processEvents();
    if (editor->parameterDockOrderForTest() != settled) {
        std::fprintf(stderr, "the dock kept reordering itself with nothing moving\n");
        editor->close();
        return false;
    }

    editor->close();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkCycleRegionForTest() {
    if (!m_timeline) return false;
    m_controller.setLoopRangeSeconds(0.0, 0.0);
    m_controller.setLoopEnabled(false);
    QApplication::processEvents();

    // A drag across the strip at the very top of the ruler. Ctrl is held so the
    // grid cannot round the two ends onto the same beat at whatever zoom the
    // window happens to be at.
    const int y = ui::kLoopStripHeight / 2;
    const int fromX = m_timeline->width() / 4;
    const int toX = m_timeline->width() / 2;
    const auto send = [&](QEvent::Type type, int x, Qt::MouseButton button,
                          Qt::MouseButtons held) {
        const QPoint at(x, y);
        QMouseEvent ev(type, QPointF(at), QPointF(m_timeline->mapToGlobal(at)),
                       button, held, Qt::ControlModifier);
        QApplication::sendEvent(m_timeline, &ev);
    };
    send(QEvent::MouseButtonPress, fromX, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, toX, Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, toX, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();

    const double from = m_controller.loopStartSeconds();
    const double to = m_controller.loopEndSeconds();
    if (!(to > from)) {
        std::fprintf(stderr, "dragging the cycle strip made no region\n");
        return false;
    }
    if (!m_controller.isLoopEnabled()) {
        std::fprintf(stderr, "dragging a region did not arm the cycle\n");
        return false;
    }
    // Scrubbing must still work on the rest of the ruler: the strip took a
    // band off the top of it, not the whole thing.
    {
        const QPoint at(toX, ui::kLoopStripHeight +
                                 (ui::kRulerHeight - ui::kLoopStripHeight) / 2);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(at),
                          QPointF(m_timeline->mapToGlobal(at)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_timeline, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at),
                            QPointF(m_timeline->mapToGlobal(at)), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(m_timeline, &release);
        QApplication::processEvents();
        if (m_controller.positionSeconds() <= 0.0) {
            std::fprintf(stderr, "the ruler below the strip no longer scrubs\n");
            return false;
        }
    }

    // A double-click removes both the range and its active state.
    {
        const QPoint on((fromX + toX) / 2, ui::kLoopStripHeight / 2);
        const auto strike = [&](QEvent::Type type, Qt::MouseButton button,
                                Qt::MouseButtons held) {
            QMouseEvent ev(type, QPointF(on), QPointF(m_timeline->mapToGlobal(on)),
                           button, held, Qt::NoModifier);
            QApplication::sendEvent(m_timeline, &ev);
        };
        const auto doubleClick = [&] {
            strike(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
            strike(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
            strike(QEvent::MouseButtonDblClick, Qt::LeftButton, Qt::LeftButton);
            strike(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
            QApplication::processEvents();
        };
        doubleClick();
        if (m_controller.isLoopEnabled() ||
            m_controller.loopStartSeconds() != 0.0 ||
            m_controller.loopEndSeconds() != 0.0) {
            std::fprintf(stderr, "double-clicking the region did not remove it\n");
            return false;
        }
    }

    // Ctrl/Cmd+B repeats every track inside the loop, advances the loop, and
    // can be pressed again to continue the chain.
    const double repeatFrom = m_controller.durationSeconds() + 8.0;
    const double repeatLength = 2.0;
    const std::string repeatTrack =
        m_controller.addTrack(daw::TrackKind::Midi, "Loop repeat check");
    const std::string repeatClip = m_controller.addMidiClip(
        repeatTrack, repeatFrom + 0.25, 0.5);
    m_controller.setLoopRangeSeconds(repeatFrom, repeatFrom + repeatLength);
    m_controller.setLoopEnabled(true);
    const bool firstRepeat = m_timeline->repeatSelection();
    const bool secondRepeat = m_timeline->repeatSelection();
    bool repeatedTwice = false;
    if (const auto* track = m_controller.project().findTrack(repeatTrack)) {
        const auto at = [&](double start) {
            return std::any_of(track->clips.begin(), track->clips.end(),
                               [start](const daw::ClipModel& clip) {
                                   return std::abs(clip.startSeconds - start) <
                                          1e-6;
                               });
        };
        repeatedTwice = !repeatClip.empty() && track->clips.size() == 3 &&
                        at(repeatFrom + 0.25) &&
                        at(repeatFrom + repeatLength + 0.25) &&
                        at(repeatFrom + 2.0 * repeatLength + 0.25) &&
                        std::abs(m_controller.loopStartSeconds() -
                                 (repeatFrom + 2.0 * repeatLength)) < 1e-6 &&
                        std::abs(m_controller.loopEndSeconds() -
                                 (repeatFrom + 3.0 * repeatLength)) < 1e-6;
    }
    m_timeline->selectClips({});
    m_controller.removeTrack(repeatTrack);
    if (!firstRepeat || !secondRepeat || !repeatedTwice) {
        std::fprintf(stderr, "the loop did not repeat its contents as a chain\n");
        return false;
    }

    // Restore the first range and prove the playhead is held inside it.
    m_controller.setLoopRangeSeconds(from, to);
    m_controller.setLoopEnabled(true);
    m_transport->setCycleEnabled(true);
    QApplication::processEvents();
    if (!m_controller.isLoopEnabled()) {
        std::fprintf(stderr, "the cycle did not arm\n");
        return false;
    }
    m_controller.seekSeconds(0.0);
    m_controller.play();
    const bool jumpedIn = std::abs(m_controller.positionSeconds() - from) < 1e-6;
    m_controller.stop();
    if (!jumpedIn) {
        std::fprintf(stderr, "playing from outside the cycle did not enter it\n");
        return false;
    }

    m_controller.setLoopEnabled(false);
    m_transport->setCycleEnabled(false);
    m_controller.setLoopRangeSeconds(0.0, 0.0);
    m_controller.seekSeconds(0.0);
    QApplication::processEvents();

    // ── Arrangement edit tools and committed time selections ──
    // Every selected tool carries its own vector cursor, and a region survives
    // switching tools so it can still be grabbed with the knife, eraser, mute
    // or draw tool in hand.
    if (!daw::visibleTracks(m_controller.project()).empty()) {
        m_timeline->ensureLaneVisible(0);
        QApplication::processEvents();
        const int laneY = m_timeline->laneCentreForTest(0);
        const int regionLeft = std::max(12, m_timeline->width() / 2);
        const int regionRight = std::min(m_timeline->width() - 12,
                                         regionLeft + 90);
        const auto pointEvent = [&](QEvent::Type type, const QPoint& at,
                                    Qt::MouseButton button,
                                    Qt::MouseButtons held) {
            QMouseEvent event(type, QPointF(at),
                              QPointF(m_timeline->mapToGlobal(at)), button,
                              held, Qt::NoModifier);
            QApplication::sendEvent(m_timeline, &event);
        };

        m_timeline->setSnapEnabled(false);
        m_transport->setToolIndex(3);   // Region
        pointEvent(QEvent::MouseButtonPress, QPoint(regionLeft, laneY),
                   Qt::LeftButton, Qt::LeftButton);
        pointEvent(QEvent::MouseMove, QPoint(regionRight, laneY),
                   Qt::NoButton, Qt::LeftButton);
        pointEvent(QEvent::MouseButtonRelease, QPoint(regionRight, laneY),
                   Qt::LeftButton, Qt::NoButton);
        if (!m_timeline->hasRegionSelection()) {
            std::fprintf(stderr, "the Region tool made no committed selection\n");
            return false;
        }
        const double offGridRegionStart =
            m_timeline->regionStartSecondsForTest();
        const int originalRegionLane = m_timeline->regionFirstLaneForTest();
        m_timeline->setSnapEnabled(true);

        const int probeLane =
            daw::visibleTracks(m_controller.project()).size() > 1 ? 1 : 0;
        const QPoint cursorProbe(
            std::max(2, regionLeft - 20),
            probeLane == 0 ? std::max(ui::kRulerHeight + 1,
                                      m_timeline->height() - 2)
                           : m_timeline->laneCentreForTest(probeLane));
        for (int tool : {1, 2, 4, 5, 3}) {
            m_transport->setToolIndex(tool);
            pointEvent(QEvent::MouseMove, cursorProbe, Qt::NoButton,
                       Qt::NoButton);
            if (m_timeline->cursor().shape() != Qt::BitmapCursor &&
                m_timeline->cursor().shape() != Qt::CustomCursor) {
                std::fprintf(stderr,
                             "arrangement tool %d has no icon cursor (shape %d)\n",
                             tool, int(m_timeline->cursor().shape()));
                return false;
            }
            if (!m_timeline->hasRegionSelection()) {
                std::fprintf(stderr,
                             "switching to arrangement tool %d lost the region\n",
                             tool);
                return false;
            }
        }

        m_transport->setToolIndex(1);   // Knife, but the region owns its body.
        const QPoint grab((regionLeft + regionRight) / 2, laneY);
        const QPoint drop(grab.x(), m_timeline->laneCentreForTest(probeLane));
        pointEvent(QEvent::MouseButtonPress, grab, Qt::LeftButton,
                   Qt::LeftButton);
        pointEvent(QEvent::MouseMove, grab + QPoint(0, 6), Qt::NoButton,
                   Qt::LeftButton);
        pointEvent(QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton);
        pointEvent(QEvent::MouseButtonRelease, drop, Qt::LeftButton,
                   Qt::NoButton);
        if (!m_timeline->hasRegionSelection()) {
            std::fprintf(stderr,
                         "dragging a committed region with the Knife lost it\n");
            return false;
        }
        if (std::abs(m_timeline->regionStartSecondsForTest() -
                     offGridRegionStart) > 1e-9) {
            std::fprintf(stderr,
                         "a vertical region drag snapped its off-grid start\n");
            return false;
        }
        if (probeLane != originalRegionLane &&
            m_timeline->regionFirstLaneForTest() != probeLane) {
            std::fprintf(stderr, "a vertical region drag stayed on its old lane\n");
            return false;
        }

        // Start and release a zero-width pick on another lane. This reliably
        // dismisses the old region and leaves no time selection behind for the
        // edit-routing checks that follow, regardless of how snapping moved it.
        m_transport->setToolIndex(3);
        pointEvent(QEvent::MouseButtonPress, cursorProbe, Qt::LeftButton,
                   Qt::LeftButton);
        pointEvent(QEvent::MouseButtonRelease, cursorProbe, Qt::LeftButton,
                   Qt::NoButton);
        m_transport->setToolIndex(0);
    }

    // A playback cursor frame restores the cached arrangement underneath its
    // old/new strips. It must not redraw lanes, clips, pattern notes or the
    // ruler; that would put project-size work back on the 16 ms clock.
    m_timeline->zoomToFit();
    m_controller.seekSeconds(0.25);
    m_timeline->update();
    QApplication::processEvents();
    const std::uint64_t staticPaints =
        m_timeline->staticFramePaintCountForTest();
    m_controller.seekSeconds(0.50);
    m_timeline->refreshPlaybackFrame();
    QApplication::processEvents();
    if (m_timeline->staticFramePaintCountForTest() != staticPaints) {
        std::fprintf(stderr,
                     "a cursor-only frame repainted the static timeline\n");
        return false;
    }
    m_controller.seekSeconds(0.0);
    return true;
}

bool MainWindow::checkTrackSelectionForTest() {
    if (!m_trackList) return false;

    // Five plain audio tracks appended to whatever is already there. They are
    // the last rows in the column and all at the top level, so they are
    // contiguous in display order and a shift-range across them cannot reach
    // anything else. The demo project is left standing: the checks that run
    // after this one are built on it.
    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(m_controller.addTrack(daw::TrackKind::Audio,
                                            "Sel " + std::to_string(i + 1)));
    }
    syncViews();
    QApplication::processEvents();

    const auto rowFor = [this](const std::string& trackId) -> QWidget* {
        for (QWidget* candidate : m_trackList->findChildren<QWidget*>()) {
            if (candidate->objectName() == QLatin1String("TrackRow") &&
                candidate->property("trackId").toString() ==
                    QString::fromStdString(trackId)) {
                return candidate;
            }
        }
        return nullptr;
    };
    // The click has to miss the resize strip along a row's bottom edge, which
    // is what a press near it means instead.
    const auto click = [&](const std::string& trackId,
                           Qt::KeyboardModifiers mods) -> bool {
        QWidget* row = rowFor(trackId);
        if (!row) return false;
        const QPoint local(row->width() / 2, row->height() / 2);
        const QPoint global = row->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local),
                          QPointF(global), Qt::LeftButton, Qt::LeftButton, mods);
        QApplication::sendEvent(row, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local),
                            QPointF(global), Qt::LeftButton, Qt::NoButton, mods);
        QApplication::sendEvent(row, &release);
        QApplication::processEvents();
        return true;
    };
    const auto selection = [this] {
        QStringList out = m_trackList->selectedTrackIds();
        out.sort();
        return out;
    };
    const auto expect = [&](std::vector<size_t> wanted, const char* what) {
        QStringList target;
        for (size_t i : wanted) target.push_back(QString::fromStdString(ids[i]));
        target.sort();
        if (selection() == target) return true;
        std::fprintf(stderr, "%s: selected %d rows, expected %d\n", what,
                     int(m_trackList->selectedTrackIds().size()),
                     int(wanted.size()));
        return false;
    };

    // The column itself is a real resizable surface. At its floor the level
    // remains available as a knob; at the opposite stop the timeline keeps a
    // narrow, reachable strip so the divider can always be pulled back.
    {
        auto* handle = qobject_cast<ui::ResizeHandle*>(m_trackHeaderHandle);
        auto* fader = m_trackList->rowFaderForTest(
            QString::fromStdString(ids[0]));
        if (!handle || !handle->onDragStart || !handle->onDrag || !fader) {
            std::fprintf(stderr, "the track-header resize surface is missing\n");
            return false;
        }
        const int original = m_trackHeaderWidth;
        const int originalWindowWidth = width();
        const QSettings settings;
        const bool hadStored = settings.contains(ui::kTrackHeaderWidthSetting);
        const QVariant stored = settings.value(ui::kTrackHeaderWidthSetting);

        handle->onDragStart();
        handle->onDrag(-10000);
        QApplication::processEvents();
        if (m_trackList->width() != ui::kMinTrackHeaderWidth ||
            !fader->isCompactKnob()) {
            std::fprintf(stderr,
                         "the narrow track column did not compact its fader\n");
            return false;
        }

        handle->onDragStart();
        handle->onDrag(10000);
        QApplication::processEvents();
        if (m_trackList->width() <= ui::kMinTrackHeaderWidth ||
            m_timeline->width() < ui::kMinTimelineWidth) {
            std::fprintf(stderr,
                         "the wide track column consumed its resize return path\n");
            return false;
        }

        m_trackHeaderWidth = original;
        applyTrackHeaderWidth();
        applyRightPanelWidths();
        resize(originalWindowWidth, height());
        if (hadStored)
            QSettings().setValue(ui::kTrackHeaderWidthSetting, stored);
        else
            QSettings().remove(ui::kTrackHeaderWidthSetting);
        QApplication::processEvents();
    }

    // A long name shows from its beginning when idle, and Return commits the
    // edit immediately instead of leaving the document with the old value.
    {
        ui::InlineNameEdit* name = nullptr;
        for (ui::InlineNameEdit* candidate :
             m_trackList->findChildren<ui::InlineNameEdit*>()) {
            if (candidate->property("trackId").toString() ==
                QString::fromStdString(ids[0])) {
                name = candidate;
                break;
            }
        }
        if (!name) {
            std::fprintf(stderr, "the track name editor is missing\n");
            return false;
        }
        const int blankX = name->textMargins().left() +
                           std::max(8, name->fontMetrics().horizontalAdvance(
                                           name->text())) +
                           5;
        const QPoint blank(blankX, name->height() / 2);
        QMouseEvent blankName(QEvent::MouseButtonDblClick, QPointF(blank),
                              QPointF(name->mapToGlobal(blank)),
                              Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(name, &blankName);
        if (!name->isReadOnly()) {
            std::fprintf(stderr,
                         "double-clicking past the track text started a rename\n");
            return false;
        }
        const QPoint local(4, name->height() / 2);
        QMouseEvent openName(QEvent::MouseButtonDblClick, QPointF(local),
                             QPointF(name->mapToGlobal(local)),
                             Qt::LeftButton, Qt::LeftButton,
                             Qt::NoModifier);
        QApplication::sendEvent(name, &openName);
        const QString renamed =
            QStringLiteral("Beginning of a deliberately very long track name");
        name->setText(renamed);
        QKeyEvent overrideCommit(QEvent::ShortcutOverride, Qt::Key_Return,
                                 Qt::NoModifier);
        QApplication::sendEvent(name, &overrideCommit);
        QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(name, &commit);
        QApplication::processEvents();
        const auto* track = m_controller.project().findTrack(ids[0]);
        if (!overrideCommit.isAccepted() || !track ||
            QString::fromStdString(track->name) != renamed ||
            !name->isReadOnly() || name->cursorPosition() != 0) {
            std::fprintf(stderr,
                         "Return did not commit the left-anchored track name: "
                         "model='%s' edit='%s' readonly=%d cursor=%d\n",
                         track ? track->name.c_str() : "<missing>",
                         name->text().toUtf8().constData(), name->isReadOnly(),
                         name->cursorPosition());
            return false;
        }
    }

    if (!click(ids[1], Qt::NoModifier)) {
        std::fprintf(stderr, "no row widget for the selection check\n");
        return false;
    }
    if (!expect({1}, "a plain click selects one row")) return false;

    // Shift from the anchor to the last row: everything between them, and the
    // rows the click passed over are included whether or not they were before.
    if (!click(ids[4], Qt::ShiftModifier)) return false;
    if (!expect({1, 2, 3, 4}, "shift-click extends from the anchor")) return false;

    // Ctrl/cmd removes exactly the row clicked and leaves the rest alone.
    if (!click(ids[2], Qt::ControlModifier)) return false;
    if (!expect({1, 3, 4}, "ctrl-click removes one row")) return false;
    if (!click(ids[0], Qt::ControlModifier)) return false;
    if (!expect({0, 1, 3, 4}, "ctrl-click adds one row")) return false;

    // A plain click is a fresh start, not an addition.
    if (!click(ids[3], Qt::NoModifier)) return false;
    if (!expect({3}, "a plain click replaces the selection")) return false;

    // And the whole point of a set: three rows become a summing folder in one
    // gesture, with their audio routed into it.
    if (!click(ids[0], Qt::NoModifier)) return false;
    if (!click(ids[2], Qt::ShiftModifier)) return false;
    if (!expect({0, 1, 2}, "a range is selected for the folder")) return false;
    packSelectionIntoFolder(/*summing=*/true);
    QApplication::processEvents();

    const auto& project = m_controller.project();
    const daw::TrackModel* folder = nullptr;
    for (const auto& track : project.tracks) {
        if (daw::isSummingFolder(track)) folder = &track;
    }
    if (!folder) {
        std::fprintf(stderr, "packing the selection made no summing folder\n");
        return false;
    }
    const std::string folderId = folder->id;
    for (size_t i = 0; i < 3; ++i) {
        const auto* track = project.findTrack(ids[i]);
        if (!track || track->parentId != folderId ||
            track->outputBusId != folderId) {
            std::fprintf(stderr,
                         "track %d is not filed in and routed to the folder\n",
                         int(i));
            return false;
        }
    }
    if (project.findTrack(ids[3])->parentId == folderId) {
        std::fprintf(stderr, "a track outside the selection was packed too\n");
        return false;
    }
    // The header column has to be showing the folder's row, not a row that is
    // now hidden inside it.
    if (m_trackList->selectedTrackIds() !=
        QStringList{QString::fromStdString(folderId)}) {
        std::fprintf(stderr, "the new folder is not what is selected\n");
        return false;
    }

    // Colouring the folder colours everything in it — the one behaviour both
    // kinds of folder share, and the reason a folder has a colour at all.
    m_controller.setTrackColor(folderId, 0x22C55E);
    for (size_t i = 0; i < 3; ++i) {
        if (m_controller.project().findTrack(ids[i])->color != 0x22C55Eu) {
            std::fprintf(stderr, "the folder's colour did not reach its tracks\n");
            return false;
        }
    }

    // ── A control touched on one selected row drives them all ──
    //
    // Selecting several tracks is *for* acting on them together, and every one
    // of these paths is gesture handling that no headless controller test can
    // see.
    if (!click(ids[0], Qt::NoModifier)) return false;
    if (!click(ids[2], Qt::ShiftModifier)) return false;
    {
        auto* mute = m_trackList->rowChipForTest(QString::fromStdString(ids[1]),
                                                 QStringLiteral("M"));
        if (!mute) {
            std::fprintf(stderr, "the middle row has no mute chip\n");
            return false;
        }
        mute->click();
        QApplication::processEvents();
        for (size_t i = 0; i < 3; ++i) {
            if (!m_controller.project().findTrack(ids[i])->muted) {
                std::fprintf(stderr, "muting one selected row left row %d alone\n",
                             int(i));
                return false;
            }
        }
        if (m_controller.project().findTrack(ids[3])->muted) {
            std::fprintf(stderr, "it muted a row outside the selection\n");
            return false;
        }
        mute->click();
        QApplication::processEvents();
        if (m_controller.project().findTrack(ids[0])->muted) {
            std::fprintf(stderr, "un-muting did not reach the rest\n");
            return false;
        }
    }

    // Bare S/M are application-wide track commands, not timeline-only keys.
    // Send them through the application filter rather than clicking a chip.
    const auto globalTrackKey = [&](int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        eventFilter(m_timeline, &press);
        QApplication::processEvents();
    };
    globalTrackKey(Qt::Key_M);
    globalTrackKey(Qt::Key_S);
    for (size_t i = 0; i < 3; ++i) {
        const auto* track = m_controller.project().findTrack(ids[i]);
        if (!track || !track->muted || !track->soloed) {
            std::fprintf(stderr,
                         "global S/M did not reach selected track %d\n", int(i));
            return false;
        }
    }
    globalTrackKey(Qt::Key_M);
    globalTrackKey(Qt::Key_S);

    // The fader, driven by a real drag: the other selected tracks follow by
    // ratio, so the balance they started with survives.
    {
        m_controller.setTrackVolume(ids[0], 1.0f);
        m_controller.setTrackVolume(ids[1], 0.5f);
        m_controller.setTrackVolume(ids[2], 0.25f);
        m_trackList->syncTrackValues();

        auto* fader = m_trackList->rowFaderForTest(QString::fromStdString(ids[0]));
        if (!fader) {
            std::fprintf(stderr, "the first row has no fader\n");
            return false;
        }
        const QPoint from(fader->width() - 6, fader->height() / 2);
        const QPoint to(fader->width() / 2, fader->height() / 2);
        const auto send = [&](QEvent::Type type, const QPoint& at,
                              Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent ev(type, QPointF(at), QPointF(fader->mapToGlobal(at)),
                           button, held, Qt::NoModifier);
            QApplication::sendEvent(fader, &ev);
        };
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();

        const float driver = m_controller.project().findTrack(ids[0])->volume;
        const float second = m_controller.project().findTrack(ids[1])->volume;
        const float third = m_controller.project().findTrack(ids[2])->volume;
        const float untouched = m_controller.project().findTrack(ids[3])->volume;
        if (!(driver < 0.99f)) {
            std::fprintf(stderr, "dragging the fader did not move its own track\n");
            return false;
        }
        // Ratios kept: the second was 6 dB under the first, the third 12.
        if (std::fabs(second - driver * 0.5f) > 0.01f ||
            std::fabs(third - driver * 0.25f) > 0.01f) {
            std::fprintf(stderr,
                         "group fader lost the balance: %.3f / %.3f / %.3f\n",
                         driver, second, third);
            return false;
        }
        if (std::fabs(untouched - 1.0f) > 0.001f) {
            std::fprintf(stderr, "it moved a track outside the selection\n");
            return false;
        }
    }

    // A row squeezed by indentation turns level and pan into a compact round
    // pair, so neither channel control disappears with the full fader throw.
    {
        std::vector<std::string> chain;
        std::string parent = folderId;
        for (int level = 0; level < 5; ++level) {
            const std::string nested = m_controller.addFolder(/*summing=*/false);
            m_controller.moveTrackToFolder(nested, parent);
            chain.push_back(nested);
            parent = nested;
        }
        const std::string leaf =
            m_controller.addTrack(daw::TrackKind::Audio, "Deep");
        m_controller.moveTrackToFolder(leaf, parent);
        chain.push_back(leaf);
        syncViews();
        QApplication::processEvents();

        const QString shallow = QString::fromStdString(ids[0]);   // depth 1
        const QString deep = QString::fromStdString(leaf);        // depth 7
        const auto shallowControls = m_trackList->rowControlsForTest(shallow);
        const auto deepControls = m_trackList->rowControlsForTest(deep);
        if (!shallowControls.first || !shallowControls.second) {
            std::fprintf(stderr, "a shallow row lost controls it had room for\n");
            return false;
        }
        if (!deepControls.second) {
            std::fprintf(stderr, "a deeply nested row lost its compact pan\n");
            return false;
        }
        auto* compact = m_trackList->rowFaderForTest(deep);
        if (!deepControls.first || !compact || !compact->isCompactKnob()) {
            std::fprintf(stderr,
                         "a deeply nested row did not compact its fader\n");
            return false;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            m_controller.removeTrack(*it);
    }

    // Put the project back exactly as it was found — including the folder this
    // check made, which would otherwise be sitting in every screenshot and
    // every later check that walks the tracks.
    for (const std::string& id : ids) m_controller.removeTrack(id);
    m_controller.removeTrack(folderId);
    syncViews();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkTrackRowHeightsForTest() {
    if (!m_trackList || !m_timeline) return false;

    // The mixer is an overlay, not a layout row. Its visibility therefore has
    // to update the timeline's explicit covered-area state. A stale inset is
    // exactly the black, trackless rectangle that used to remain after X.
    if (!m_mixerWindow && m_mixer && m_arrangementHost &&
        m_arrangementHost->height() > 80) {
        const bool mixerWasShown = !m_mixer->isHidden();
        const QSize hostSize = m_arrangementHost->size();
        const auto restoreMixer = [&] {
            if (m_mixerWindow) onDockMixer();
            m_arrangementHost->resize(hostSize);
            setMixerVisible(mixerWasShown);
            layoutMixer();
            QApplication::processEvents();
        };
        const auto insetMatchesGeometry = [&] {
            int overlayTop = m_mixer->y();
            if (m_mixerHandle && !m_mixerHandle->isHidden())
                overlayTop = std::min(
                    overlayTop,
                    m_mixerHandle->mapTo(m_arrangementHost, QPoint()).y());
            const int expected = std::max(
                0, m_arrangementHost->height() - std::max(0, overlayTop));
            return m_mixer->y() >= 0 &&
                   (!m_mixerHandle || m_mixerHandle->y() >= 0) &&
                   m_timeline->bottomInsetForTest() == expected;
        };

        setMixerVisible(true);
        QApplication::processEvents();
        if (m_timeline->bottomInsetForTest() <= 0 ||
            !insetMatchesGeometry()) {
            restoreMixer();
            std::fprintf(stderr,
                         "shown mixer geometry and timeline cover disagree\n");
            return false;
        }

        // A transiently short workspace must clip the body below the host,
        // never place the mixer/handle at a negative y while claiming that a
        // strip of timeline is visible there.
        m_arrangementHost->resize(hostSize.width(), 80);
        layoutMixer();
        if (!insetMatchesGeometry()) {
            restoreMixer();
            std::fprintf(stderr,
                         "compact mixer geometry and timeline cover disagree\n");
            return false;
        }
        m_arrangementHost->resize(hostSize);
        layoutMixer();

        // Reparenting a QWidget hides it. Verify the floating mixer is shown,
        // frees the timeline, and stays in sync with the View action while it
        // is hidden and presented again.
        onDetachMixer();
        QApplication::processEvents();
        if (!m_mixerWindow || m_mixer->isHidden() ||
            m_timeline->bottomInsetForTest() != 0) {
            restoreMixer();
            std::fprintf(stderr, "detached mixer was hidden or left an inset\n");
            return false;
        }
        setMixerVisible(false);
        if (!m_mixerWindow->isHidden() ||
            (m_showMixerAction && m_showMixerAction->isChecked())) {
            restoreMixer();
            std::fprintf(stderr, "detached mixer hide state did not synchronize\n");
            return false;
        }
        setMixerVisible(true);
        QApplication::processEvents();
        if (!m_mixerWindow->isVisible() ||
            (m_showMixerAction && !m_showMixerAction->isChecked())) {
            restoreMixer();
            std::fprintf(stderr, "detached mixer show state did not synchronize\n");
            return false;
        }
        onDockMixer();
        QApplication::processEvents();
        if (m_mixerWindow || m_mixer->isHidden() ||
            m_timeline->bottomInsetForTest() <= 0 ||
            !insetMatchesGeometry()) {
            restoreMixer();
            std::fprintf(stderr, "docked mixer did not restore its cover\n");
            return false;
        }

        setMixerVisible(false);
        QApplication::processEvents();
        if (m_timeline->bottomInsetForTest() != 0) {
            restoreMixer();
            std::fprintf(stderr,
                         "hidden mixer left a %d px timeline paint gap\n",
                         m_timeline->bottomInsetForTest());
            return false;
        }
        restoreMixer();
    }

    // Let the demo's comp editor finish opening. The historical resize bug only
    // appears when the row contains that extra take-stack height.
    QEventLoop settle;
    QTimer::singleShot(ui::kCompAnimMs + 20, &settle, &QEventLoop::quit);
    settle.exec();
    addDemoTracks(3);
    QApplication::processEvents();

    // Resize the expanded row through the real event-filter path. Its base
    // height must move by exactly the pointer delta; using the full container
    // height here would add the comp rows to the base a second time.
    const auto& beforeProject = m_controller.project();
    const auto& beforeRows = daw::visibleTracks(beforeProject);
    int expandedLane = -1;
    std::string expandedTrackId;
    int baseBefore = 0;
    for (int lane = 0; lane < int(beforeRows.size()); ++lane) {
        const auto& track = beforeProject.tracks[beforeRows[size_t(lane)].index];
        if (track.kind == daw::TrackKind::Folder ||
            ui::compExtraHeight(track) <= 0) {
            continue;
        }
        expandedLane = lane;
        expandedTrackId = track.id;
        baseBefore = ui::laneHeightFor(track.height);
        break;
    }
    if (expandedLane < 0) {
        std::fprintf(stderr, "no expanded track for the row resize check\n");
        return false;
    }

    QWidget* expandedRow = nullptr;
    for (QWidget* candidate : m_trackList->findChildren<QWidget*>()) {
        if (candidate->objectName() == QLatin1String("TrackRow") &&
            candidate->property("trackId").toString() ==
                QString::fromStdString(expandedTrackId)) {
            expandedRow = candidate;
            break;
        }
    }
    if (!expandedRow) {
        std::fprintf(stderr, "expanded track has no row widget\n");
        return false;
    }

    // Shrinking was the easiest way to trigger the visual pile-up reported by
    // users; growth goes through the same arithmetic in the opposite direction.
    constexpr int kResizeDelta = -12;
    const QPoint pressLocal(expandedRow->width() / 2,
                            expandedRow->height() - 1);
    const QPoint pressGlobal = expandedRow->mapToGlobal(pressLocal);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pressLocal),
                      QPointF(pressGlobal), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(expandedRow, &press);

    const QPoint moveLocal = pressLocal + QPoint(0, kResizeDelta);
    const QPoint moveGlobal = pressGlobal + QPoint(0, kResizeDelta);
    QMouseEvent move(QEvent::MouseMove, QPointF(moveLocal), QPointF(moveGlobal),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(expandedRow, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(moveLocal),
                        QPointF(moveGlobal), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(expandedRow, &release);

    const auto* resized = m_controller.project().findTrack(expandedTrackId);
    if (!resized || ui::laneHeightFor(resized->height) !=
                        baseBefore + kResizeDelta) {
        std::fprintf(stderr,
                     "expanded row resize changed base from %d to %d, expected %d\n",
                     baseBefore,
                     resized ? ui::laneHeightFor(resized->height) : -1,
                     baseBefore + kResizeDelta);
        return false;
    }

    const auto& project = m_controller.project();
    const auto& rows = daw::visibleTracks(project);
    for (int lane = 0; lane < int(rows.size()); ++lane) {
        const daw::TrackModel& track = project.tracks[rows[size_t(lane)].index];
        const int expected = ui::laneHeightForTrack(track);
        const QRect header = m_trackList->rowRectForTest(int(lane));
        if (header.isNull()) {
            std::fprintf(stderr, "track %d has no header row\n", lane + 1);
            return false;
        }
        if (header.height() != expected) {
            std::fprintf(stderr,
                         "track %d's header is %d px, its lane is %d px\n",
                         lane + 1, header.height(), expected);
            return false;
        }
        // …and the two columns start at the same y, or the headers and their
        // lanes are describing different tracks. Both are measured from the top
        // of their own widget, and the ruler strip is the same height in each.
        const int laneTop = m_timeline->laneTopForTest(lane);
        if (std::abs(header.top() - laneTop) > 1) {
            std::fprintf(stderr, "track %d: header at %d, lane at %d\n",
                         lane + 1, header.top(), laneTop);
            return false;
        }
    }
    return true;
}

bool MainWindow::checkTimelinePanForTest() {
    if (!m_timeline) return false;
    const bool savedFollow = m_timeline->followsPlayhead();
    const double savedPosition = m_controller.positionSeconds();
    m_timeline->setFollowPlayhead(false);
    m_controller.seekSeconds(0.0);
    m_timeline->setVerticalScroll(0);
    const double timeBefore = m_timeline->horizontalScrollForTest();
    const int rowsBefore = m_timeline->verticalScroll();
    const QPoint start(m_timeline->width() / 2,
                       std::min(m_timeline->height() - 20,
                                ui::kRulerHeight + 80));
    const QPoint finish = start - QPoint(80, 40);
    const QPoint globalStart = m_timeline->mapToGlobal(start);
    const QPoint globalFinish = m_timeline->mapToGlobal(finish);

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                      QPointF(globalStart), Qt::MiddleButton, Qt::MiddleButton,
                      Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish), QPointF(globalFinish),
                     Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
                        QPointF(globalFinish), Qt::MiddleButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &release);

    const bool timeMoved = m_timeline->horizontalScrollForTest() > timeBefore;
    const bool rowsMoved = m_timeline->verticalScroll() > rowsBefore;
    const double manualScroll = m_timeline->horizontalScrollForTest();
    m_timeline->ensurePlayheadVisible();
    const bool independent =
        std::abs(m_timeline->horizontalScrollForTest() - manualScroll) < 0.001;
    m_timeline->setFollowPlayhead(true);
    const bool centred = m_timeline->horizontalScrollForTest() < manualScroll;
    const bool scrollbarsPresent = m_timeline->hasNavigationControlsForTest();

    m_timeline->setFollowPlayhead(false);
    m_controller.seekSeconds(savedPosition);
    m_timeline->setFollowPlayhead(savedFollow);

    if (!timeMoved || !rowsMoved || !independent || !centred ||
        !scrollbarsPresent) {
        std::fprintf(stderr,
                     "timeline navigation failed (time %d, tracks %d, "
                     "independent %d, centred %d, scrollbars %d)\n",
                     int(timeMoved), int(rowsMoved), int(independent),
                     int(centred), int(scrollbarsPresent));
    }
    return timeMoved && rowsMoved && independent && centred &&
           scrollbarsPresent;
}

bool MainWindow::checkTimelineClipGesturesForTest() {
    if (!m_timeline) return false;
    m_timeline->setTool(TimelineWidget::Tool::Select);
    m_timeline->setSecondaryTool(TimelineWidget::Tool::Knife);
    QKeyEvent altPress(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    QApplication::sendEvent(m_timeline, &altPress);
    const bool borrowedImmediately =
        m_timeline->activeToolForTest() == TimelineWidget::Tool::Knife;
    QKeyEvent altRelease(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &altRelease);
    const bool restoredImmediately =
        m_timeline->activeToolForTest() == TimelineWidget::Tool::Select;
    if (!borrowedImmediately || !restoredImmediately) {
        std::fprintf(stderr,
                     "Alt secondary tool did not switch immediately (%d/%d)\n",
                     int(borrowedImmediately), int(restoredImmediately));
        return false;
    }

    const double savedGrid = m_transport ? m_transport->gridBeats() : 0.25;
    const bool savedSnap = m_transport ? m_transport->snapEnabled() : true;
    m_timeline->setGridBeats(-1.0);
    m_timeline->setSnapEnabled(true);
    m_timeline->zoomBy(0.000001);
    const double coarseGridSeconds = m_timeline->snapSeconds();
    m_timeline->zoomBy(1000000.0);
    const double fineGridSeconds = m_timeline->snapSeconds();
    m_timeline->setGridBeats(savedGrid);
    m_timeline->setSnapEnabled(savedSnap);
    m_timeline->zoomToFit();
    if (!(fineGridSeconds > 0.0 &&
          fineGridSeconds < coarseGridSeconds)) {
        std::fprintf(stderr,
                     "adaptive grid did not refine with zoom (%.6f/%.6f)\n",
                     coarseGridSeconds, fineGridSeconds);
        return false;
    }

    std::vector<std::string> tracks;
    for (int i = 0; i < 4; ++i) {
        tracks.push_back(m_controller.addTrack(
            daw::TrackKind::Midi, "Clip drag " + std::to_string(i + 1)));
    }
    const std::string first = m_controller.addMidiClip(tracks[0], 1.0, 1.0);
    const std::string second = m_controller.addMidiClip(tracks[1], 1.5, 1.0);
    syncViews();
    QApplication::processEvents();

    const auto rowOf = [&](const std::string& id) {
        const auto& rows = daw::visibleTracks(m_controller.project());
        for (int i = 0; i < int(rows.size()); ++i) {
            if (m_controller.project().tracks[rows[std::size_t(i)].index].id == id)
                return i;
        }
        return -1;
    };
    const auto send = [&](QEvent::Type type, const QPoint& at,
                          Qt::MouseButton button, Qt::MouseButtons held,
                          Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(type, QPointF(at),
                          QPointF(m_timeline->mapToGlobal(at)), button, held,
                          modifiers);
        QApplication::sendEvent(m_timeline, &event);
    };
    const auto clipCentreX = [&](const std::string& track,
                                 const std::string& clip) {
        m_timeline->selectClips({{QString::fromStdString(track),
                                  QString::fromStdString(clip)}});
        int left = 0;
        int right = 0;
        return m_timeline->selectionSpanX(left, right) ? (left + right) / 2 : -1;
    };

    const int rowA = rowOf(tracks[0]);
    const int rowB = rowOf(tracks[1]);
    const int rowC = rowOf(tracks[2]);
    m_timeline->ensureLaneVisible(rowA);
    QApplication::processEvents();
    const int xA = clipCentreX(tracks[0], first);
    const int xB = clipCentreX(tracks[1], second);
    if (rowA < 0 || rowB < 0 || rowC < 0 || xA < 0 || xB < 0) return false;

    // The Inspector edits the exact same per-clip state as the Sample Editor,
    // and one finished field edit must be one undo step.
    std::string audioTrack;
    std::string audioClip;
    for (const auto& track : m_controller.project().tracks) {
        const auto found = std::find_if(
            track.clips.begin(), track.clips.end(),
            [](const daw::ClipModel& clip) {
                return clip.kind == daw::ClipKind::Audio &&
                       !daw::isLayered(clip);
            });
        if (found != track.clips.end()) {
            audioTrack = track.id;
            audioClip = found->id;
            break;
        }
    }
    if (audioTrack.empty() || !m_inspector) {
        std::fprintf(stderr, "no audio clip for Inspector stretch check\n");
        return false;
    }
    m_selection.setClips({{QString::fromStdString(audioTrack),
                           QString::fromStdString(audioClip)}});
    m_selectedTrackId = QString::fromStdString(audioTrack);
    m_inspector->setSelection(m_selectedTrackId,
                              QString::fromStdString(audioClip));
    QApplication::processEvents();
    auto* pitch = m_inspector->findChild<QDoubleSpinBox*>(
        QStringLiteral("ClipParameter_stretch.pitch"));
    const double pitchBefore = m_controller.clipSampleParameter(
        audioTrack, audioClip, "stretch.pitch");
    if (!pitch || !pitch->isVisible()) {
        std::fprintf(stderr, "audio clip controls did not appear in Inspector\n");
        return false;
    }
    pitch->setValue(pitchBefore > 0.0 ? pitchBefore - 1.0
                                     : pitchBefore + 1.0);
    QMetaObject::invokeMethod(pitch, "editingFinished", Qt::DirectConnection);
    if (std::abs(m_controller.clipSampleParameter(
                     audioTrack, audioClip, "stretch.pitch") - pitchBefore) <
        0.5) {
        std::fprintf(stderr, "Inspector pitch did not reach clip state\n");
        return false;
    }
    m_controller.undo();
    if (std::abs(m_controller.clipSampleParameter(
                     audioTrack, audioClip, "stretch.pitch") - pitchBefore) >
        1e-9) {
        std::fprintf(stderr, "Inspector pitch edit did not undo\n");
        return false;
    }

    auto* reverse = m_inspector->findChild<QCheckBox*>(
        QStringLiteral("ClipParameter_pre.reverse"));
    if (!reverse) {
        std::fprintf(stderr, "Inspector Reverse control is missing\n");
        return false;
    }
    const std::uint64_t waveformPaintsBefore =
        m_timeline->staticFramePaintCountForTest();
    reverse->setChecked(!reverse->isChecked());
    QApplication::processEvents();
    const bool reverseRedrewWaveform =
        m_timeline->staticFramePaintCountForTest() > waveformPaintsBefore;
    m_controller.undo();
    m_inspector->syncFromModel();
    m_timeline->update();
    if (!reverseRedrewWaveform) {
        std::fprintf(stderr, "Reverse did not redraw the clip waveform\n");
        return false;
    }

    // Stretching directly on the clip changes duration through stretch.time,
    // then undo restores both the duration and the ratio.
    const int audioRow = rowOf(audioTrack);
    m_timeline->ensureLaneVisible(audioRow);
    QApplication::processEvents();
    const int audioX = clipCentreX(audioTrack, audioClip);
    if (audioRow < 0 || audioX < 0) {
        std::fprintf(stderr, "audio clip is outside the stretch test view\n");
        return false;
    }
    const QPoint stretchFrom(audioX, m_timeline->laneCentreForTest(audioRow));
    const QPoint stretchTo = stretchFrom + QPoint(80, 0);
    const daw::ClipModel* audioBefore = m_controller.audioClip(audioTrack, audioClip);
    const double stretchBefore = audioBefore ? audioBefore->sampleEdit.stretchTime : 0.0;
    const double durationBefore = audioBefore ? audioBefore->durationSeconds : 0.0;
    m_timeline->setTool(TimelineWidget::Tool::Stretch);
    m_timeline->setSnapEnabled(false);
    send(QEvent::MouseButtonPress, stretchFrom, Qt::LeftButton,
         Qt::LeftButton, Qt::NoModifier);
    send(QEvent::MouseMove, stretchTo, Qt::NoButton,
         Qt::LeftButton, Qt::NoModifier);
    send(QEvent::MouseButtonRelease, stretchTo, Qt::LeftButton,
         Qt::NoButton, Qt::NoModifier);
    const daw::ClipModel* audioAfter = m_controller.audioClip(audioTrack, audioClip);
    if (!audioAfter || audioAfter->sampleEdit.stretchTime <= stretchBefore ||
        audioAfter->durationSeconds <= durationBefore) {
        std::fprintf(stderr, "Stretch tool did not lengthen the audio clip\n");
        return false;
    }
    m_controller.undo();
    audioAfter = m_controller.audioClip(audioTrack, audioClip);
    if (!audioAfter ||
        std::abs(audioAfter->sampleEdit.stretchTime - stretchBefore) > 1e-9 ||
        std::abs(audioAfter->durationSeconds - durationBefore) > 1e-9) {
        std::fprintf(stderr, "Stretch tool edit did not undo\n");
        return false;
    }
    m_timeline->setTool(TimelineWidget::Tool::Select);
    m_timeline->setSnapEnabled(savedSnap);

    // The circular fade-shape handle is a horizontal direct manipulation: its
    // position on the half-gain line and the cached shadow must both update in
    // the same frame. One release remains one undo step.
    audioAfter = m_controller.audioClip(audioTrack, audioClip);
    if (!audioAfter) return false;
    const double fadeInBefore = audioAfter->fadeInSeconds;
    const double fadeOutBefore = audioAfter->fadeOutSeconds;
    const double fadeInCurveBefore = audioAfter->fadeInCurve;
    const double fadeOutCurveBefore = audioAfter->fadeOutCurve;
    const double testFade = std::max(
        0.01, std::min(audioAfter->durationSeconds * 0.4, 2.0));
    m_controller.setClipFade(audioTrack, audioClip, testFade, 0.0);
    m_controller.setClipFadeCurve(audioTrack, audioClip, true, 0.0);
    m_timeline->selectClips({{QString::fromStdString(audioTrack),
                              QString::fromStdString(audioClip)}});
    m_timeline->update();
    QApplication::processEvents();
    const QPoint fadeHandle = m_timeline->fadeCurveHandleForTest(
        QString::fromStdString(audioTrack), QString::fromStdString(audioClip),
        true);
    const std::uint64_t paintsBeforeFadeDrag =
        m_timeline->staticFramePaintCountForTest();
    if (fadeHandle.x() < 0) {
        std::fprintf(stderr, "fade curve handle was not drawable\n");
        return false;
    }
    const QPoint bentFadeHandle = fadeHandle - QPoint(18, 0);
    send(QEvent::MouseButtonPress, fadeHandle, Qt::LeftButton,
         Qt::LeftButton, Qt::NoModifier);
    send(QEvent::MouseMove, bentFadeHandle, Qt::NoButton,
         Qt::LeftButton, Qt::NoModifier);
    QApplication::processEvents();
    audioAfter = m_controller.audioClip(audioTrack, audioClip);
    const bool fadeBentHorizontally =
        audioAfter && audioAfter->fadeInCurve > 0.01;
    const bool fadeShadowRepainted =
        m_timeline->staticFramePaintCountForTest() > paintsBeforeFadeDrag;
    send(QEvent::MouseButtonRelease, bentFadeHandle, Qt::LeftButton,
         Qt::NoButton, Qt::NoModifier);
    m_controller.undo();
    audioAfter = m_controller.audioClip(audioTrack, audioClip);
    const bool fadeUndoRestored =
        audioAfter && std::abs(audioAfter->fadeInCurve) < 1e-9;
    if (!fadeBentHorizontally || !fadeShadowRepainted || !fadeUndoRestored) {
        std::fprintf(stderr,
                     "horizontal fade curve gesture failed (%d/%d/%d)\n",
                     int(fadeBentHorizontally), int(fadeShadowRepainted),
                     int(fadeUndoRestored));
        return false;
    }
    m_controller.setClipFade(audioTrack, audioClip, fadeInBefore,
                             fadeOutBefore);
    m_controller.setClipFadeCurve(audioTrack, audioClip, true,
                                  fadeInCurveBefore);
    m_controller.setClipFadeCurve(audioTrack, audioClip, false,
                                  fadeOutCurveBefore);

    // Shift-click adds a clip of another lane/type to the current set without
    // copying until a real drag passes the platform threshold.
    m_timeline->selectClips({{QString::fromStdString(tracks[0]),
                              QString::fromStdString(first)}});
    const QPoint secondPoint(xB, m_timeline->laneCentreForTest(rowB));
    send(QEvent::MouseButtonPress, secondPoint, Qt::LeftButton,
         Qt::LeftButton, Qt::ShiftModifier);
    send(QEvent::MouseButtonRelease, secondPoint, Qt::LeftButton,
         Qt::NoButton, Qt::ShiftModifier);
    QApplication::processEvents();
    if (m_selection.clips().size() != 2) {
        std::fprintf(stderr, "Shift-click selected %d clips, wanted two\n",
                     int(m_selection.clips().size()));
        return false;
    }

    // Move the two clips down together. Their relative lane offset survives.
    const QPoint grab(xA, m_timeline->laneCentreForTest(rowA));
    const QPoint drop(xA, m_timeline->laneCentreForTest(rowB));
    send(QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton,
         Qt::NoModifier);
    send(QEvent::MouseMove, drop, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    send(QEvent::MouseButtonRelease, drop, Qt::LeftButton, Qt::NoButton,
         Qt::NoModifier);
    QApplication::processEvents();
    const auto hasClip = [&](const std::string& trackId,
                             const std::string& clipId) {
        const auto* track = m_controller.project().findTrack(trackId);
        return track && std::any_of(
                            track->clips.begin(), track->clips.end(),
                            [&](const daw::ClipModel& clip) {
                                return clip.id == clipId;
                            });
    };
    if (!hasClip(tracks[1], first) || !hasClip(tracks[2], second)) {
        const auto where = [&](const std::string& clipId) {
            for (const auto& track : m_controller.project().tracks) {
                if (std::any_of(track.clips.begin(), track.clips.end(),
                                [&](const daw::ClipModel& clip) {
                                    return clip.id == clipId;
                                })) {
                    return track.id;
                }
            }
            return std::string("<missing>");
        };
        std::fprintf(stderr,
                     "a selected clip group did not move down as a lane block "
                     "(%s, %s; rows %d/%d/%d, y %d->%d)\n",
                     where(first).c_str(), where(second).c_str(), rowA, rowB,
                     rowC, grab.y(), drop.y());
        return false;
    }

    // Shift-drag moves newly minted copies; the original ids remain in place.
    const QPoint copiedDrop = drop + QPoint(80, 0);
    send(QEvent::MouseButtonPress, drop, Qt::LeftButton, Qt::LeftButton,
         Qt::ShiftModifier);
    send(QEvent::MouseMove, copiedDrop, Qt::NoButton, Qt::LeftButton,
         Qt::ShiftModifier);
    send(QEvent::MouseButtonRelease, copiedDrop, Qt::LeftButton, Qt::NoButton,
         Qt::ShiftModifier);
    QApplication::processEvents();
    if (!hasClip(tracks[1], first) || !hasClip(tracks[2], second) ||
        m_controller.project().findTrack(tracks[1])->clips.size() != 2 ||
        m_controller.project().findTrack(tracks[2])->clips.size() != 2) {
        std::fprintf(stderr,
                     "Shift-drag moved originals or failed to make both copies\n");
        return false;
    }
    m_controller.undo();
    if (m_controller.project().findTrack(tracks[1])->clips.size() != 1 ||
        m_controller.project().findTrack(tracks[2])->clips.size() != 1) {
        std::fprintf(stderr, "one undo did not remove the Shift-drag copies\n");
        return false;
    }

    // A plain empty-space drag is not a marquee. Ctrl turns that same gesture
    // into one, and a marquee pulled above the ruler auto-scrolls upward.
    for (int i = 0; i < 18; ++i) {
        tracks.push_back(m_controller.addTrack(
            daw::TrackKind::Midi, "Scroll drag " + std::to_string(i + 1)));
    }
    syncViews();
    m_timeline->setVerticalScroll(100000);
    QApplication::processEvents();
    const int scrollBefore = m_timeline->verticalScroll();
    const QPoint marqueeStart(m_timeline->width() - 12,
                              m_timeline->height() - 12);
    const QPoint marqueeOutside(marqueeStart.x(), ui::kRulerHeight - 30);
    send(QEvent::MouseButtonPress, marqueeStart, Qt::LeftButton,
         Qt::LeftButton, Qt::NoModifier);
    send(QEvent::MouseMove, marqueeOutside, Qt::NoButton, Qt::LeftButton,
         Qt::NoModifier);
    send(QEvent::MouseButtonRelease, marqueeOutside, Qt::LeftButton,
         Qt::NoButton, Qt::NoModifier);
    QApplication::processEvents();
    const bool plainDragStayedPut =
        m_timeline->verticalScroll() == scrollBefore;
    send(QEvent::MouseButtonPress, marqueeStart, Qt::LeftButton,
         Qt::LeftButton, Qt::ControlModifier);
    send(QEvent::MouseMove, marqueeOutside, Qt::NoButton, Qt::LeftButton,
         Qt::ControlModifier);
    send(QEvent::MouseButtonRelease, marqueeOutside, Qt::LeftButton,
         Qt::NoButton, Qt::ControlModifier);
    QApplication::processEvents();
    if (!plainDragStayedPut || scrollBefore <= 0 ||
        m_timeline->verticalScroll() >= scrollBefore) {
        std::fprintf(stderr,
                     "Ctrl marquee gesture failed (plain=%d, scroll=%d/%d)\n",
                     int(plainDragStayedPut), scrollBefore,
                     m_timeline->verticalScroll());
        return false;
    }

    m_timeline->clearClipSelection();
    if (m_transport) {
        m_timeline->setSecondaryTool(static_cast<TimelineWidget::Tool>(
            m_transport->secondaryToolIndex()));
    }
    for (const std::string& track : tracks) m_controller.removeTrack(track);
    syncViews();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkLiveTempoForTest() {
    if (!m_transport) return false;
    auto* edit = m_transport->findChild<QLineEdit*>(QStringLiteral("TempoField"));
    if (!edit) return false;
    const double before = m_controller.tempo();
    const double wanted = before == 137.0 ? 138.0 : 137.0;
    const QString text = QString::number(wanted, 'f', 1);
    edit->setText(text);
    QMetaObject::invokeMethod(edit, "textEdited", Qt::DirectConnection,
                              Q_ARG(QString, text));
    const bool live = m_controller.tempo() == wanted;
    QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
    if (!live) {
        std::fprintf(stderr, "tempo stayed at %.1f while %.1f was typed\n",
                     m_controller.tempo(), wanted);
    }
    return live;
}

bool MainWindow::checkSettingsViewportForTest() {
    if (!m_settingsWindow) return false;
    QApplication::processEvents();   // runs the post-show native-frame clamp

    auto* tabs = m_settingsWindow->findChild<QTabWidget*>();
    bool scrollable = tabs && tabs->count() == SettingsWindow::kShortcutsTab + 1 &&
                      m_settingsWindow->checkAudioPageForTest();
    if (tabs) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto* scroll = qobject_cast<QScrollArea*>(tabs->widget(i));
            scrollable = scrollable && scroll && scroll->widgetResizable() &&
                         scroll->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded &&
                         scroll->horizontalScrollBarPolicy() ==
                             Qt::ScrollBarAlwaysOff;
        }
    }

    InternalEditorFrame* frame =
        m_internalEditorFrames.value(m_settingsWindow, nullptr);
    const bool bounded = frame && m_editorHost &&
                         !m_settingsWindow->isWindow() && !frame->isWindow() &&
                         m_settingsWindow->parentWidget() == frame &&
                         m_editorHost->rect().contains(frame->geometry().topLeft()) &&
                         m_editorHost->rect().contains(frame->geometry().bottomRight());
    if (!scrollable || !bounded) {
        std::fprintf(stderr,
                     "settings viewport failed (scrollable %d, bounded %d, "
                     "height %d/%d)\n",
                     int(scrollable), int(bounded), m_settingsWindow->height(),
                     frame ? frame->height() : 0);
    }
    return scrollable && bounded;
}

bool MainWindow::checkTempoScrubForTest() {
    if (!m_transport) return false;
    auto* edit = m_transport->findChild<QLineEdit*>(QStringLiteral("TempoField"));
    if (!edit || !edit->isReadOnly()) return false;

    const auto scrub = [edit](int verticalPixels) {
        const QPoint start = edit->rect().center();
        const QPoint finish = start + QPoint(0, verticalPixels);
        const QPoint globalStart = edit->mapToGlobal(start);
        const QPoint globalFinish = edit->mapToGlobal(finish);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                          QPointF(globalStart), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(finish),
                         QPointF(globalFinish), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
                            QPointF(globalFinish), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &release);
    };

    const double original = m_controller.tempo();
    scrub(-24);   // upward movement raises the number
    const double raised = m_controller.tempo();
    scrub(48);    // downward movement lowers it again
    const double lowered = m_controller.tempo();

    const QPoint center = edit->rect().center();
    const QPoint global = edit->mapToGlobal(center);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(center),
                            QPointF(global), Qt::LeftButton, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(edit, &doubleClick);
    const bool enteredTextMode = !edit->isReadOnly();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(edit, &escape);
    const bool leftTextMode = edit->isReadOnly();

    onTempoChanged(original);
    m_transport->syncTempo();
    const bool scrubbed = raised > original && lowered < raised &&
                          raised == std::round(raised) &&
                          lowered == std::round(lowered);
    if (!scrubbed || !enteredTextMode || !leftTextMode) {
        std::fprintf(stderr,
                     "tempo scrub failed (%.1f -> %.1f -> %.1f, text %d/%d)\n",
                     original, raised, lowered, int(enteredTextMode),
                     int(leftTextMode));
    }
    return scrubbed && enteredTextMode && leftTextMode;
}

bool MainWindow::checkPositionScrubForTest() {
    if (!m_transport || !m_timeline) return false;
    auto* position =
        m_transport->findChild<QLineEdit*>(QStringLiteral("BarsPosition"));
    if (!position || !position->isReadOnly()) return false;

    const auto scrub = [](QLineEdit* edit, int verticalPixels) {
        const QPoint start = edit->rect().center();
        const QPoint finish = start + QPoint(0, verticalPixels);
        const QPoint globalStart = edit->mapToGlobal(start);
        const QPoint globalFinish = edit->mapToGlobal(finish);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                          QPointF(globalStart), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(finish),
                         QPointF(globalFinish), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
                            QPointF(globalFinish), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &release);
    };
    const auto enter = [](QLineEdit* edit, const QString& value) {
        const QPoint center = edit->rect().center();
        const QPoint global = edit->mapToGlobal(center);
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(center),
                                QPointF(global), Qt::LeftButton,
                                Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(edit, &doubleClick);
        if (edit->isReadOnly()) return false;
        edit->setText(value);
        QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
        return edit->isReadOnly();
    };

    const double original = m_controller.positionSeconds();
    const bool originalTimelineMode = m_transport->showsBars();
    const bool originalPositionMode = m_transport->positionShowsBars();
    m_transport->setTimeDisplayBars(true);
    m_controller.seekSeconds(10.0);
    m_timeline->refreshPlaybackFrame();
    const int cursorBeforeDrag = m_timeline->displayedPlayheadXForTest();
    scrub(position, -20);
    const double afterBarsDrag = m_controller.positionSeconds();
    const int cursorAfterDrag = m_timeline->displayedPlayheadXForTest();

    const bool barsEntered = enter(position, QStringLiteral("2.1.000"));
    const double barLength =
        m_controller.timeSigNumerator() *
        (4.0 / std::max(1, m_controller.timeSigDenominator()));
    const double expectedBarSeconds =
        barLength * 60.0 / std::max(1.0, m_controller.tempo());
    const bool barsConverted =
        std::abs(m_controller.positionSeconds() - expectedBarSeconds) < 0.001;

    m_transport->setPositionDisplayBars(false);
    const bool localClockMode = !m_transport->positionShowsBars() &&
                                m_transport->showsBars();
    const bool hasFormatMenu =
        position->contextMenuPolicy() == Qt::CustomContextMenu;
    const double beforeClockDrag = m_controller.positionSeconds();
    scrub(position, 10);
    const double afterClockDrag = m_controller.positionSeconds();
    const bool clockEntered = enter(position, QStringLiteral("00.03.25"));
    const bool clockConverted =
        std::abs(m_controller.positionSeconds() - 3.25) < 0.001;
    m_controller.seekSeconds(original);
    m_transport->setTimeDisplayBars(originalTimelineMode);
    m_transport->setPositionDisplayBars(originalPositionMode);
    m_transport->refreshPosition();
    m_timeline->refreshPlaybackFrame();

    const bool dragged = afterBarsDrag > 10.0 &&
                         afterClockDrag < beforeClockDrag;
    const bool cursorMoved = cursorAfterDrag != cursorBeforeDrag;
    if (!dragged || !barsEntered || !clockEntered || !barsConverted ||
        !clockConverted || !cursorMoved || !localClockMode || !hasFormatMenu) {
        std::fprintf(stderr,
                     "position scrub failed (drag %.3f/%.3f, entry %d/%d, "
                     "convert %d/%d, cursor %d, local %d, menu %d)\n",
                     afterBarsDrag, afterClockDrag, int(barsEntered),
                     int(clockEntered), int(barsConverted), int(clockConverted),
                     int(cursorMoved), int(localClockMode), int(hasFormatMenu));
    }
    return dragged && barsEntered && clockEntered && barsConverted &&
           clockConverted && cursorMoved && localClockMode && hasFormatMenu;
}

bool MainWindow::checkAuxiliaryWindowPolicyForTest() {
    QString trackId;
    QString clipId;
    for (const daw::TrackModel& track : m_controller.project().tracks) {
        for (const daw::ClipModel& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Midi) continue;
            trackId = QString::fromStdString(track.id);
            clipId = QString::fromStdString(clip.id);
            break;
        }
        if (!clipId.isEmpty()) break;
    }
    if (trackId.isEmpty() || clipId.isEmpty() || !m_transport || !m_timeline)
        return false;

    openPianoRoll(trackId, clipId);
    QApplication::processEvents();
    const bool internal = m_pianoRoll && m_pianoRollFrame && m_editorHost &&
        m_pianoRoll->isVisible() && m_pianoRollFrame->isVisible() &&
        !m_pianoRoll->isWindow() && !m_pianoRollFrame->isWindow() &&
        m_pianoRollFrame->parentWidget() == m_editorHost &&
        m_pianoRollFrame->property("dawInternalEditor").toBool() &&
        m_pianoRoll->window() == this;
    const bool bounded = internal &&
        m_editorHost->rect().contains(m_pianoRollFrame->geometry().topLeft()) &&
        m_editorHost->rect().contains(m_pianoRollFrame->geometry().bottomRight());
    const bool wasMaximized = internal && m_pianoRollFrame->isMaximized();
    if (wasMaximized) m_pianoRollFrame->setMaximized(false);
    const QRect restoredGeometry = m_pianoRollFrame->geometry();
    m_pianoRollFrame->setMaximized(true);
    QApplication::processEvents();
    const bool maximizedInsideHost = m_pianoRollFrame->isMaximized() &&
        m_pianoRollFrame->geometry() == m_editorHost->rect();
    m_pianoRollFrame->setMaximized(false);
    QApplication::processEvents();
    const bool restoredAfterMaximize =
        m_pianoRollFrame->geometry() == restoredGeometry;
    if (wasMaximized) m_pianoRollFrame->setMaximized(true);

    const QPoint transportAt = m_transport->rect().center();
    QMouseEvent transportPress(
        QEvent::MouseButtonPress, QPointF(transportAt),
        QPointF(m_transport->mapToGlobal(transportAt)), Qt::LeftButton,
        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_transport, &transportPress);
    QMouseEvent transportRelease(
        QEvent::MouseButtonRelease, QPointF(transportAt),
        QPointF(m_transport->mapToGlobal(transportAt)), Qt::LeftButton,
        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_transport, &transportRelease);
    QApplication::processEvents();
    const bool transportReturnedFocus = m_pianoRollFrame->isVisible() &&
        !m_pianoRollFrame->isEditorActive();

    const QPoint timelineAt(std::max(8, m_timeline->width() / 3), 4);
    QMouseEvent timelinePress(
        QEvent::MouseButtonPress, QPointF(timelineAt),
        QPointF(m_timeline->mapToGlobal(timelineAt)), Qt::LeftButton,
        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &timelinePress);
    QMouseEvent timelineRelease(
        QEvent::MouseButtonRelease, QPointF(timelineAt),
        QPointF(m_timeline->mapToGlobal(timelineAt)), Qt::LeftButton,
        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &timelineRelease);
    QApplication::processEvents();
    const bool inactiveButOpen = !m_pianoRollFrame->isEditorActive() &&
                                 m_pianoRollFrame->isVisible();

    openPianoRoll(trackId, clipId);
    QApplication::processEvents();
    const bool reopened = m_pianoRollFrame->isEditorActive() &&
                          m_pianoRollFrame->isVisible();

    QWidget* trackTarget = nullptr;
    if (m_trackList) {
        for (QWidget* candidate : m_trackList->findChildren<QWidget*>()) {
            if (candidate->isVisible() && candidate->property("trackId").isValid() &&
                (!trackTarget || candidate->width() > trackTarget->width())) {
                trackTarget = candidate;
            }
        }
    }
    if (trackTarget) {
        const QPoint at = trackTarget->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(at),
                          QPointF(trackTarget->mapToGlobal(at)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(trackTarget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at),
                            QPointF(trackTarget->mapToGlobal(at)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(trackTarget, &release);
        QApplication::processEvents();
    }
    const bool trackInactiveButOpen = trackTarget &&
        !m_pianoRollFrame->isEditorActive() && m_pianoRollFrame->isVisible();
    openPianoRoll(trackId, clipId);
    QApplication::processEvents();
    const bool reopenedAfterTrack = m_pianoRollFrame->isEditorActive() &&
                                    m_pianoRollFrame->isVisible();
    bool allModelessInternal = m_settingsWindow && m_pluginManagerWindow;
    for (auto it = m_internalEditorFrames.cbegin();
         it != m_internalEditorFrames.cend(); ++it) {
        QWidget* content = it.key();
        InternalEditorFrame* frame = it.value();
        allModelessInternal = allModelessInternal && content && frame &&
            !content->isWindow() && !frame->isWindow() &&
            content->parentWidget() == frame && frame->parentWidget() == m_editorHost &&
            frame->property("dawInternalEditor").toBool();
    }
    const bool ok = internal && bounded && maximizedInsideHost &&
        restoredAfterMaximize && transportReturnedFocus &&
        inactiveButOpen && reopened && trackInactiveButOpen && reopenedAfterTrack &&
        allModelessInternal;
    if (!ok) {
        std::fprintf(stderr,
                     "internal editor policy: internal=%d bounded=%d "
                     "max=%d restore=%d transport=%d timeline=%d reopen=%d "
                     "track=%d trackReopen=%d allInternal=%d\n",
                     int(internal), int(bounded), int(maximizedInsideHost),
                     int(restoredAfterMaximize),
                     int(transportReturnedFocus), int(inactiveButOpen),
                     int(reopened), int(trackInactiveButOpen),
                     int(reopenedAfterTrack), int(allModelessInternal));
    }
    return ok;
}

bool MainWindow::checkPianoRollForTest() {
    if (!m_pianoRoll || !m_contextPanel) return false;
    const bool panelWasEnabled = m_contextPanel->isPanelEnabled();
    if (!panelWasEnabled) m_contextPanel->setPanelEnabled(true);

    const bool gestures = m_pianoRoll->checkInteractionGesturesForTest();
    const bool compact = m_pianoRoll->checkCompactLayoutForTest();
    m_pianoRoll->selectAllNotesForTest();
    syncPianoRollContextPanel();
    QApplication::processEvents();
    const bool mergedContext = m_noteContextPanel && m_toolPanel &&
        m_noteContextPanel->parentWidget() == m_toolPanel &&
        m_noteContextPanel->isVisible() && !m_contextPanel->isVisible() &&
        std::abs(m_noteContextPanel->geometry().center().x() -
                 m_toolPanel->rect().center().x()) <= 1 &&
        m_noteContextPanel->geometry().top() == 0;
    bool contextPressKeptEditorActive = false;
    if (mergedContext) {
        const auto buttons = m_noteContextPanel->findChildren<QAbstractButton*>();
        QAbstractButton* button = nullptr;
        for (QAbstractButton* candidate : buttons) {
            if (candidate && candidate->isVisible() && candidate->isEnabled()) {
                button = candidate;
                break;
            }
        }
        if (button) {
            const QPoint at = button->rect().center();
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(at),
                              QPointF(button->mapToGlobal(at)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(button, &press);
            contextPressKeptEditorActive = m_pianoRollFrame->isEditorActive() &&
                                           m_noteContextPanel->isVisible();
            // Release outside the button: the check must not perform the
            // button's edit, only prove that mouse-down no longer hides it.
            const QPoint outside(-2, -2);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(outside),
                                QPointF(button->mapToGlobal(outside)),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(button, &release);
        }
    }
    if (!mergedContext) {
        std::fprintf(stderr,
                     "the MIDI note context did not take over the shared strip "
                     "(note=%d, arrangement=%d)\n",
                     int(m_noteContextPanel && m_noteContextPanel->isVisible()),
                     int(m_contextPanel->isVisible()));
    }
    const bool cycle = m_pianoRoll->checkCycleGestureForTest();
    const bool ok = gestures && compact && mergedContext &&
                    contextPressKeptEditorActive && cycle;
    // Later shell checks measure and drive the track-header layout. A native
    // editor used to live outside that surface; the internal pilot must leave
    // the same clean harness state rather than covering the rows under test.
    if (m_pianoRoll) m_pianoRoll->close();
    if (m_pianoRollFrame) m_pianoRollFrame->hide();
    syncPianoRollContextPanel();
    if (!panelWasEnabled) {
        m_contextPanel->setPanelEnabled(false);
        syncPianoRollContextPanel();
    }
    return ok;
}

bool MainWindow::checkEditChordRoutingForTest() {
    // Reproduces the macOS routing exactly: Cocoa owns the system menu bar and
    // triggers a menu item's key equivalent itself, whichever window is in
    // front and without asking the focused widget first. So the check triggers
    // the arrangement's own actions with the piano roll active — which is what
    // pressing Cmd+X there really does.
    openFirstMidiClip();
    QApplication::processEvents();
    if (!m_pianoRoll || !m_pianoRoll->isVisible()) {
        std::fprintf(stderr, "the piano roll did not open\n");
        return false;
    }
    struct RouteOverrideReset {
        QPointer<QWidget>& windowSlot;
        QPointer<QWidget> previousWindow;
        QPointer<QWidget>& focusSlot;
        QPointer<QWidget> previousFocus;
        ~RouteOverrideReset() {
            windowSlot = previousWindow;
            focusSlot = previousFocus;
        }
    } reset{m_editChordRouteWindowForTest, m_editChordRouteWindowForTest,
            m_editChordFocusWidgetForTest, m_editChordFocusWidgetForTest};
    m_editChordRouteWindowForTest = m_pianoRoll;
    m_editChordFocusWidgetForTest.clear();

    // The clip the roll is editing, and the lane it sits on.
    std::string trackId;
    std::string clipId;
    for (const auto& t : m_controller.project().tracks) {
        for (const auto& c : t.clips) {
            if (c.kind != daw::ClipKind::Midi || c.notes.empty()) continue;
            trackId = t.id;
            clipId = c.id;
            break;
        }
        if (!trackId.empty()) break;
    }
    if (trackId.empty()) {
        std::fprintf(stderr, "no MIDI clip with notes to edit\n");
        return false;
    }
    const auto clipOf = [&]() -> const daw::ClipModel* {
        const daw::TrackModel* t = m_controller.project().findTrack(trackId);
        if (!t) return nullptr;
        for (const auto& c : t->clips) {
            if (c.id == clipId) return &c;
        }
        return nullptr;
    };
    const auto clipCount = [&]() -> std::size_t {
        const daw::TrackModel* t = m_controller.project().findTrack(trackId);
        return t ? t->clips.size() : 0;
    };

    const auto fire = [this](const char* id) {
        QAction* action = findChild<QAction*>(QString::fromUtf8(id));
        if (action) action->trigger();
        QApplication::processEvents();
        return action != nullptr;
    };

    // The arrangement has to have something to lose, so that a chord going to
    // the wrong place is visible rather than a no-op.
    m_timeline->selectClips({ui::ClipSel{QString::fromStdString(trackId),
                                         QString::fromStdString(clipId)}});
    m_pianoRoll->selectAllNotesForTest();
    QApplication::processEvents();

    const std::size_t clipsBefore = clipCount();
    const std::size_t notesBefore = clipOf() ? clipOf()->notes.size() : 0;
    if (notesBefore == 0) return false;

    // ── Cut ──
    if (!fire("edit.cutClips")) {
        std::fprintf(stderr, "the Cut command is missing\n");
        return false;
    }
    if (clipCount() != clipsBefore) {
        std::fprintf(stderr, "Cut in the piano roll cut the clip, not the notes\n");
        return false;
    }
    if (!clipOf() || !clipOf()->notes.empty()) {
        std::fprintf(stderr, "Cut in the piano roll left %d notes behind\n",
                     int(clipOf() ? clipOf()->notes.size() : -1));
        return false;
    }
    m_controller.undo();
    syncViews();
    QApplication::processEvents();
    if (!clipOf() || clipOf()->notes.size() != notesBefore) {
        std::fprintf(stderr, "undoing the note cut did not put the notes back\n");
        return false;
    }

    // ── Repeat ──
    m_pianoRoll->selectAllNotesForTest();
    QApplication::processEvents();
    if (!fire("edit.repeatClips")) {
        std::fprintf(stderr, "the Repeat command is missing\n");
        return false;
    }
    if (clipCount() != clipsBefore) {
        std::fprintf(stderr, "Repeat in the piano roll repeated the clip\n");
        return false;
    }
    if (!clipOf() || clipOf()->notes.size() <= notesBefore) {
        std::fprintf(stderr, "Repeat in the piano roll did not repeat the notes\n");
        return false;
    }
    m_controller.undo();
    syncViews();
    QApplication::processEvents();

    // ── Delete ──
    // The Track menu's Delete is the destructive one: with the roll in front it
    // must not take the track out from under the notes.
    const std::size_t tracksBefore = m_controller.project().tracks.size();
    m_pianoRoll->selectAllNotesForTest();
    QApplication::processEvents();
    if (!fire("track.remove")) {
        std::fprintf(stderr, "the Delete command is missing\n");
        return false;
    }
    if (m_controller.project().tracks.size() != tracksBefore ||
        clipCount() != clipsBefore) {
        std::fprintf(stderr, "Delete in the piano roll removed the track or clip\n");
        return false;
    }
    if (!clipOf() || !clipOf()->notes.empty()) {
        std::fprintf(stderr, "Delete in the piano roll did not delete the notes\n");
        return false;
    }
    m_controller.undo();
    syncViews();
    QApplication::processEvents();

    // ── And the arrangement still owns the chord when it is in front ──
    m_editChordRouteWindowForTest = this;
    activateWindow();
    m_timeline->selectClips({ui::ClipSel{QString::fromStdString(trackId),
                                         QString::fromStdString(clipId)}});
    QApplication::processEvents();
    if (!fire("edit.cutClips")) return false;
    if (clipCount() != clipsBefore - 1) {
        std::fprintf(stderr,
                     "with the arrangement in front Cut no longer cuts clips\n");
        return false;
    }
    m_controller.undo();
    syncViews();

    // ── A focused text field owns the chord before anything else ──
    // It never sees the key either, for the same reason, so it is served by
    // hand — and the clips behind it are left alone.
    QLineEdit field;
    m_timeline->selectClips({ui::ClipSel{QString::fromStdString(trackId),
                                         QString::fromStdString(clipId)}});
    field.setText(QStringLiteral("automation"));
    field.selectAll();
    m_editChordFocusWidgetForTest = &field;
    if (!fire("edit.cutClips")) return false;
    if (clipCount() != clipsBefore) {
        std::fprintf(stderr, "Cut in a text field cut a clip\n");
        return false;
    }
    if (!field.text().isEmpty()) {
        std::fprintf(stderr, "Cut in a text field left '%s' in it\n",
                     field.text().toUtf8().constData());
        return false;
    }
    m_editChordFocusWidgetForTest.clear();

    // Leave the roll open and in front, with nothing selected in the
    // arrangement — near enough to how this check found the session.
    m_timeline->selectClips({});
    if (m_pianoRollFrame) m_pianoRollFrame->present();
    QApplication::processEvents();
    return true;
}

bool MainWindow::checkContextSyncForTest() {
    if (m_controller.project().tracks.empty()) return false;
    const QString trackId =
        QString::fromStdString(m_controller.project().tracks.front().id);
    setContextPanelVisible(true);
    selectTrackFromHeader(trackId);
    QApplication::processEvents();

    ui::MiniSlider* level = m_contextPanel->findChild<ui::MiniSlider*>(
        QStringLiteral("ContextPanelTrackVolume"));
    if (!level) {
        std::fprintf(stderr, "the context panel has no level control\n");
        return false;
    }

    const double before = m_mixer->faderGainForTest(trackId);
    const std::size_t undoMark = m_controller.undoDepth();
    // Several real mouse moves in one grab: the mixer must follow every sample,
    // but history must see only the press and release endpoints.
    const QPoint start = level->rect().center();
    const QPoint finish = start - QPoint(8, 0);
    const QPoint globalStart = level->mapToGlobal(start);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                      QPointF(globalStart), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(level, &press);
    for (int dx : {2, 5, 8}) {
        const QPoint at = start - QPoint(dx, 0);
        QMouseEvent move(QEvent::MouseMove, QPointF(at),
                         QPointF(level->mapToGlobal(at)), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(level, &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(finish),
                        QPointF(level->mapToGlobal(finish)), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(level, &release);
    QApplication::processEvents();

    const auto* track = m_controller.project().findTrack(trackId.toStdString());
    if (!track) return false;
    const double shown = m_mixer->faderGainForTest(trackId);
    if (std::abs(shown - double(track->volume)) > 0.001 ||
        std::abs(shown - before) < 0.0001) {
        std::fprintf(stderr,
                     "context panel level did not reach the mixer: strip %.4f, "
                     "document %.4f, before %.4f\n",
                     shown, double(track->volume), before);
        return false;
    }
    const double released = track->volume;
    if (m_controller.undoDepth() !=
        std::min(undoMark + 1, m_controller.undoLimit())) {
        std::fprintf(stderr,
                     "one level drag created %zu undo entries instead of one\n",
                     m_controller.undoDepth() - undoMark);
        return false;
    }
    m_controller.undo();
    const auto* undone = m_controller.project().findTrack(trackId.toStdString());
    const bool restored = undone && std::abs(double(undone->volume) - before) < 0.001;
    m_controller.redo();
    const auto* redone = m_controller.project().findTrack(trackId.toStdString());
    const bool replayed = redone &&
                          std::abs(double(redone->volume) - released) < 0.001;
    syncViews();
    if (!restored || !replayed) {
        std::fprintf(stderr, "level gesture undo/redo missed its endpoints\n");
        return false;
    }

    QString audioTrack;
    QString audioClip;
    for (const auto& candidateTrack : m_controller.project().tracks) {
        for (const auto& candidateClip : candidateTrack.clips) {
            if (candidateClip.kind != daw::ClipKind::Audio) continue;
            audioTrack = QString::fromStdString(candidateTrack.id);
            audioClip = QString::fromStdString(candidateClip.id);
            break;
        }
        if (!audioClip.isEmpty()) break;
    }
    if (!audioClip.isEmpty()) {
        m_timeline->selectClips({ui::ClipSel{audioTrack, audioClip}});
        QEventLoop settle;
        QTimer::singleShot(360, &settle, &QEventLoop::quit);
        settle.exec();
        auto* adder = m_contextPanel->findChild<PluginQuickAdder*>(
            QStringLiteral("ContextPanelClipPluginSearch"));
        if (!adder || adder->trackId() != audioTrack ||
            adder->clipId() != audioClip) {
            std::fprintf(stderr,
                         "an audio clip has no correctly routed Clip FX search\n");
            return false;
        }
        m_contextPanel->openPluginSearch();
        QApplication::processEvents();
        if (!adder->isExpanded()) {
            std::fprintf(stderr, "the Clip FX search did not open\n");
            return false;
        }
        adder->closeSearch();
    }

    // MIDI is its own clip context, with a direct Piano Roll action. It used
    // to resolve to `Other`, which silently hid the whole island.
    QString midiTrack;
    QString midiClip;
    for (const auto& candidateTrack : m_controller.project().tracks) {
        for (const auto& candidateClip : candidateTrack.clips) {
            if (candidateClip.kind != daw::ClipKind::Midi) continue;
            midiTrack = QString::fromStdString(candidateTrack.id);
            midiClip = QString::fromStdString(candidateClip.id);
            break;
        }
        if (!midiClip.isEmpty()) break;
    }
    if (!midiClip.isEmpty()) {
        m_timeline->selectClips({ui::ClipSel{midiTrack, midiClip}});
        QApplication::processEvents();
        auto* editor = m_contextPanel->findChild<QAbstractButton*>(
            QStringLiteral("MidiClipEditorButton"));
        if (!m_contextPanel->isVisible() || !editor || !editor->isVisible()) {
            std::fprintf(stderr,
                         "a selected MIDI clip has no context-panel editor action\n");
            return false;
        }
        editor->click();
        QApplication::processEvents();
        if (!m_pianoRoll || m_pianoRoll->trackId() != midiTrack) {
            std::fprintf(stderr,
                         "the MIDI context action did not open its Piano Roll\n");
            return false;
        }
        m_pianoRoll->close();
        if (m_pianoRollFrame) m_pianoRollFrame->hide();
        activateWindow();
        m_timeline->selectClips({});
    }
    return true;
}

bool MainWindow::openDemoPluginMenu(bool instruments, const QString& query) {
    QMenu* menu = ui::buildPluginMenu(
        this, &m_controller, instruments,
        [](const daw::plugins::PluginDescriptor&) {});
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(mapToGlobal(QPoint(220, 180)));
    // A grab cannot type, so the query is put into the field directly — the
    // filtering it drives is the same code a keystroke reaches.
    if (!query.isEmpty()) {
        if (auto* edit = menu->findChild<QLineEdit*>()) edit->setText(query);
    }
    return true;
}

bool MainWindow::openDemoTrackFlyout(const QString& which) {
    if (m_controller.project().tracks.empty()) return false;
    // A whole track selected and no clip is what puts the panel into its
    // channel tools — the context the level and pan controls live in.
    setContextPanelVisible(true);
    selectTrackFromHeader(
        QString::fromStdString(m_controller.project().tracks.front().id));
    if (!m_contextPanel) return false;
    m_contextPanel->relayout();
    // The panel swaps its content with an animation, and the row that is on it
    // right now is the one being animated *out* — opening a flyout off a
    // control that is about to be deleted closes it again a frame later. So the
    // control is looked up once the swap has landed.
    QTimer::singleShot(500, this, [this, which] {
        for (ui::MiniSlider* slider :
             m_contextPanel->findChildren<ui::MiniSlider*>()) {
            if (!slider->toolTip().contains(which, Qt::CaseInsensitive)) continue;
            slider->openFlyoutForTest();
            return;
        }
    });
    return true;
}

void MainWindow::selectDemoClipsForShot(const QString& indices, bool settle) {
    // Every clip in the project, left to right, so an index means the same
    // thing however the demo is arranged.
    QVector<QPair<double, ui::ClipSel>> all;
    for (const auto& track : m_controller.project().tracks) {
        for (const auto& clip : track.clips) {
            all.append({clip.startSeconds,
                        ui::ClipSel{QString::fromStdString(track.id),
                                    QString::fromStdString(clip.id)}});
        }
    }
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    QVector<ui::ClipSel> picked;
    for (const QString& part : indices.split(',', Qt::SkipEmptyParts)) {
        const int index = part.trimmed().toInt();
        if (index >= 0 && index < all.size()) picked.append(all[index].second);
    }
    if (m_timeline) m_timeline->selectClips(picked);
    // The plate normally rides in over a few frames; a still has no frames to
    // spare, so it is put where it belongs at once — unless the caller is
    // photographing the movement itself.
    if (settle && m_contextPanel) m_contextPanel->relayout();
}

void MainWindow::openDemoRecoveryPrompt() {
    daw::recovery::SessionInfo session;
    session.projectName = "Midnight Drive";
    session.projectPath = "/Users/you/Music/Midnight Drive.vlt";
    session.journalUnixMs = daw::recovery::nowUnixMs() - 40 * 1000;
    session.outcome = daw::recovery::Outcome::Crashed;
    session.crashReason = "crashed in Serum (SIGSEGV)";

    QMessageBox* box = ui::recovery::buildRecoveryPrompt(this, session);
    box->setAttribute(Qt::WA_DeleteOnClose);
    // show(), not exec(): the screenshot run has to keep driving its own event
    // loop to reach the grab.
    box->show();
}

int MainWindow::checkRecovery() {
    auto sessions =
        daw::recovery::staleSessions(ui::recovery::rootDir().toStdString());
    if (sessions.empty()) return -1;
    std::sort(sessions.begin(), sessions.end(),
              [](const auto& a, const auto& b) {
                  return a.journalUnixMs > b.journalUnixMs;
              });
    QString error;
    if (!ui::recovery::applySession(sessions.front(), m_controller, &error)) {
        std::fprintf(stderr, "recovery failed: %s\n", qPrintable(error));
        return -1;
    }
    // Consumed, exactly as the dialog does it — a session that has been
    // recovered must not be offered a second time.
    daw::recovery::discardSession(sessions.front().directory);
    syncViews();
    // Printed rather than returned: the caller is a shell check, and seeing the
    // track names is what makes a passing run believable.
    for (const auto& track : m_controller.project().tracks) {
        std::printf("  %s (%zu clips)\n", track.name.c_str(),
                    track.clips.size());
    }
    return int(m_controller.project().tracks.size());
}

void MainWindow::endRecoverySessionForTest() { m_journal.stop(); }

bool MainWindow::flushRecoveryForTest() {
    if (!m_journal.running()) return false;
    m_journalStale = true;
    sampleForRecovery();
    m_journal.flush();
    return true;
}

void MainWindow::sampleForRecovery() {
    if (!m_journal.running()) return;
    // Opaque plugin saveState calls must remain on the plugin/UI thread, and a
    // vendor is free to take an unbounded amount of time in one. Never put that
    // work — or the accompanying deep ProjectModel copy — on the playback or
    // record path. The stale flag remains set and the next idle one-second tick
    // catches up automatically after transport stops.
    const bool transportActive = m_controller.isPlaying() ||
                                 m_controller.isRecording() ||
                                 m_controller.isCountingIn();
    if (!transportActive) {
        // A native plugin can change preset/program data without reporting a
        // host parameter. Sample one state per idle tick, prioritising editors
        // and missing cache entries so a large project is amortised over time.
        std::vector<std::string> preferredPluginStates;
        preferredPluginStates.reserve(std::size_t(m_pluginEditors.size()) * 2);
        for (PluginEditorWindow* editor : m_pluginEditors) {
            if (!editor) continue;
            const std::string stem = editor->insertId().toStdString();
            preferredPluginStates.push_back(stem);
            preferredPluginStates.push_back(stem + "-right");
        }
        const bool pluginStateChanged =
            m_controller.refreshRecoveryPluginStates(1,
                                                       preferredPluginStates);

        if (m_journalStale) {
            daw::recovery::RecoverySnapshot snapshot =
                m_controller.captureRecoverySnapshot(0);
            std::vector<std::string> stateFiles;
            stateFiles.reserve(snapshot.pluginStates.size());
            for (const auto& state : snapshot.pluginStates)
                stateFiles.push_back(state.fileName);
            m_journal.requestWrite(std::move(snapshot));
            m_recoveryPluginStateFiles = std::move(stateFiles);
            m_journalStale = false;
        } else if (pluginStateChanged) {
            // No document copy on unchanged editor polls. Once an opaque chunk
            // really differs, build one snapshot using the cached state.
            daw::recovery::RecoverySnapshot snapshot =
                m_controller.captureRecoverySnapshot(0);
            m_recoveryPluginStateFiles.clear();
            m_recoveryPluginStateFiles.reserve(snapshot.pluginStates.size());
            for (const auto& state : snapshot.pluginStates)
                m_recoveryPluginStateFiles.push_back(state.fileName);
            m_journal.requestWrite(std::move(snapshot));
        }
    }

    daw::recovery::HealthStats stats;
    stats.dspLoad = m_controller.dspLoad();
    m_recoveryDspPeak = std::max(m_recoveryDspPeak, stats.dspLoad);
    stats.dspLoadPeak = m_recoveryDspPeak;
    const QJsonObject process = PlatformDiagnostics::processSample();
    stats.processCpu = process.value(QStringLiteral("process_cpu")).toDouble();
    stats.systemCpu = process.value(QStringLiteral("system_cpu")).toDouble();
    stats.residentBytes = std::uint64_t(
        std::max(0.0, process.value(QStringLiteral("resident_bytes")).toDouble()));
    stats.sampleRate = m_controller.project().sampleRate;
    stats.bufferFrames = int(m_controller.bufferSizeFrames());
    stats.trackCount = int(m_controller.project().tracks.size());
    int clips = 0;
    for (const auto& track : m_controller.project().tracks)
        clips += int(track.clips.size());
    stats.clipCount = clips;
    stats.playing = m_controller.isPlaying();
    stats.recording = m_controller.isRecording();
    stats.lastPlugin = daw::crash::lastPluginName();
    m_journal.setStats(std::move(stats));
}

QJsonObject MainWindow::telemetrySnapshot() {
    QJsonObject sample = PlatformDiagnostics::processSample();
    const auto& project = m_controller.project();
    const double dsp = m_controller.dspLoad();
    m_telemetryDspPeak = std::max(m_telemetryDspPeak, dsp);
    int clips = 0;
    int pluginInstances = 0;
    struct Aggregate { QString name; QString vendor; QString format; int count = 0; };
    QHash<QString, Aggregate> aggregate;
    const auto addPlugin = [&aggregate, &pluginInstances](const daw::InsertModel& insert) {
        if (!insert.isLoaded()) return;
        const QString name = QString::fromStdString(insert.name);
        const QString vendor = QString::fromStdString(insert.vendor);
        const QString format = QString::fromStdString(daw::toString(insert.format));
        const QString key = name + QChar(0x1f) + vendor + QChar(0x1f) + format;
        auto& entry = aggregate[key];
        entry.name = name; entry.vendor = vendor; entry.format = format;
        ++entry.count; ++pluginInstances;
    };
    for (const auto& insert : project.masterInserts) addPlugin(insert);
    for (const auto& track : project.tracks) {
        clips += int(track.clips.size());
        addPlugin(track.instrument);
        for (const auto& insert : track.inserts) addPlugin(insert);
        for (const auto& insert : track.samplerFx.inserts) addPlugin(insert);
    }
    QJsonArray plugins;
    for (const auto& entry : aggregate) {
        plugins.append(QJsonObject{
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("vendor"), entry.vendor},
            {QStringLiteral("version"), QString()},
            {QStringLiteral("format"), entry.format},
            {QStringLiteral("count"), entry.count},
        });
    }
    sample.insert(QStringLiteral("dsp_load"), dsp * 100.0);
    sample.insert(QStringLiteral("dsp_peak"), m_telemetryDspPeak * 100.0);
    sample.insert(QStringLiteral("xruns"), 0);
    sample.insert(QStringLiteral("sample_rate"), m_controller.sampleRate());
    sample.insert(QStringLiteral("buffer_frames"), int(m_controller.bufferSizeFrames()));
    sample.insert(QStringLiteral("track_count"), int(project.tracks.size()));
    sample.insert(QStringLiteral("clip_count"), clips);
    sample.insert(QStringLiteral("plugin_count"), pluginInstances);
    sample.insert(QStringLiteral("playback_state"),
                  m_controller.isPlaying() ? QStringLiteral("playing")
                                           : QStringLiteral("stopped"));
    sample.insert(QStringLiteral("recording"), m_controller.isRecording());
    sample.insert(QStringLiteral("foreground"), isActiveWindow());
    sample.insert(QStringLiteral("plugins"), plugins);
    m_telemetryDspPeak = dsp;
    return sample;
}

void MainWindow::closeEvent(QCloseEvent* event) {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) {
        event->ignore();
        return;
    }
    if (m_publicationUi && m_publicationUi->active) {
        const auto answer = QMessageBox::question(
            this, tr("Publication Is Still Running"),
            tr("Cancel the cloud publication and close VLT? The saved local "
               "project will remain unchanged."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        clearCloudProjectBinding(/*cancelPublication=*/true);
    }
#endif
    if (!maybeSaveChanges()) {
        event->ignore();
        return;
    }
    if (m_persistGeometry) {
        QSettings().setValue(ui::kMainGeometrySetting, saveGeometry());
    }
    // Internal editors are not OS windows, so close their contents explicitly
    // before the shell starts tearing children down.
    if (m_pianoRoll && m_pianoRoll->isVisible()) m_pianoRoll->close();
    if (m_pianoRollFrame) m_pianoRollFrame->hide();
    closeInternalWindows();
    closeAuxiliaryWindows();
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    applyRightPanelWidths();
    applyTrackHeaderWidth();
}

MainWindow::~MainWindow() {
    // Children are destroyed by ~QObject, which runs *after* m_controller — a
    // member of this class — is already gone. Anything whose destructor talks
    // to the engine has to be taken down here, while it is still there: the
    // piano roll ends a keyboard audition, the typing keyboard releases held
    // keys, and a plugin editor hands the view back to its plugin. Deleting a
    // child unparents it, so ~QObject will not see them again.
    //
    // The named ones first, because their order matters to each other; then
    // *every* remaining child, for the same reason and without having to have
    // predicted which one it would be. Naming them one at a time is a list
    // that goes stale on the next panel that talks to the engine on its way
    // out — and the failure mode is a crash on quit, in a destructor, with a
    // stack that points at Qt rather than at the panel that was late.
#ifdef DAW_ENABLE_COLLABORATION
    if (m_cloudPublisher) {
        disconnect(m_cloudPublisher, nullptr, this, nullptr);
        if (m_publicationUi && m_publicationUi->active)
            m_cloudPublisher->cancel();
    }
    if (m_cloudProjectSync)
        disconnect(m_cloudProjectSync, nullptr, this, nullptr);
    if (m_cloudSessionLifecycle)
        disconnect(m_cloudSessionLifecycle, nullptr, this, nullptr);
    if (m_collaborationCommandBridge)
        disconnect(m_collaborationCommandBridge, nullptr, this, nullptr);
    if (m_cloudProjectClient)
        disconnect(m_cloudProjectClient, nullptr, this, nullptr);
    if (m_recordingLeases)
        disconnect(m_recordingLeases, nullptr, this, nullptr);
    m_publicationUi.reset();
#endif
    ui::setAutomationCreationMode(false);
    delete m_noteContextPanel;
    m_noteContextPanel = nullptr;
    delete m_midiInput;
    m_midiInput = nullptr;
    delete m_pianoRoll;
    m_pianoRoll = nullptr;
    delete m_pianoRollFrame;
    m_pianoRollFrame = nullptr;
    delete m_patternWindow;
    m_patternWindow = nullptr;
    delete m_typingKeyboard;
    m_typingKeyboard = nullptr;
    // The assistant's request in flight would answer into a dead controller.
    delete m_webPanel;
    m_webPanel = nullptr;
    delete m_aiPanel;
    m_aiPanel = nullptr;
    const QList<PluginEditorWindow*> editors = m_pluginEditors.values();
    m_pluginEditors.clear();   // deleted below, not through the registry
    for (PluginEditorWindow* editor : editors) delete editor;

    // Whatever is left. Taken as a copy of the list: deleting a child removes
    // it from `children()`, and a child that deletes a sibling of its own —
    // an editor window closing its dependents — would invalidate a live
    // iterator. A guarded pointer covers exactly that case.
    QList<QPointer<QObject>> remaining;
    remaining.reserve(children().size());
    for (QObject* child : children()) remaining.append(QPointer<QObject>(child));
    for (const QPointer<QObject>& child : remaining) {
        if (child) delete child.data();
    }
}

void MainWindow::buildLayout() {
    auto* central = new MainShellHost(this);
    auto* shell = new QHBoxLayout(central);
    // Preferred right-panel widths must not become the top-level window's
    // minimum width. The resize handler deliberately compresses Web and AI;
    // letting QLayout propagate their current fixed widths would prevent the
    // window from ever receiving that smaller resize in the first place.
    shell->setSizeConstraint(QLayout::SetNoConstraint);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    m_shellLayout = shell;

    // The workspace and the AI surface are peers. That makes the assistant a
    // full-height object: when it opens, the transport/header and arrangement
    // both stop at its glass edge instead of the chat beginning below them as
    // one more ordinary sidebar.
    auto* workspace = new MainWorkspaceHost(central);
    m_workspace = workspace;
    auto* column = new QVBoxLayout(workspace);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    // ── Transport ──
    m_transport = new TransportBar(&m_controller, workspace);
    column->addWidget(m_transport);

    // ── Quick playback switches (Restart / play from selected clip) ──
    m_toolPanel = new ToolPanel(workspace);
    m_toolPanel->setRestartMode(
        m_controller.playbackMode() == daw::EngineController::PlaybackMode::Restart);
    column->addWidget(m_toolPanel);

    // The context panel is an island floating in the middle of the tool strip,
    // directly under the transport's position and tempo. It is a child of the
    // strip rather than part of its zone layout, so it can size itself to
    // whatever the selection needs without pushing the zones around.
    m_contextPanel = new ContextPanel(&m_controller, &m_selection, m_toolPanel);
    // Asked, not told: the plate recomputes its geometry from inside a
    // selection change, so a value pushed afterwards would always be one step
    // behind what it had already decided.
    m_contextPanel->setAnchorProvider(
        [this](int& centreX) { return contextPanelAnchor(centreX); });
    m_contextPanel->setBoundsProvider([this](int& left, int& right) {
        return contextPanelBounds(left, right);
    });
    connect(m_toolPanel, &ToolPanel::resized, this,
            &MainWindow::layoutContextPanel);

    // ── Main row: browser | inspector | (arrangement over mixer) ──
    auto* row = new QWidget(workspace);
    m_editorHost = row;
    // Native plugin editors are child NSViews/HWNDs. Establish their nearest
    // stable native ancestor while the main layout is still being built, not
    // when the first plugin is opened. Otherwise that first native child can
    // trigger a one-off backing-view rebuild and remain visually present but
    // outside Qt's hit-test hierarchy; later plugin windows then appear fine.
    const QString platform = QGuiApplication::platformName();
    if (platform != QLatin1String("offscreen") &&
        platform != QLatin1String("minimal")) {
        row->setAttribute(Qt::WA_NativeWindow);
    }
    m_rowLayout = new QHBoxLayout(row);
    m_rowLayout->setContentsMargins(0, 0, 0, 0);
    m_rowLayout->setSpacing(0);

    m_browser = new FileBrowserPanel(&m_controller, row);
    m_browserOnLeft = ui::browserprefs::onLeft();
    m_browserWidth = ui::browserprefs::width();
    m_browser->setFixedWidth(m_browserWidth);
    // The strip's zones are only worth anything if they are the width of the
    // columns they stand over — this is what puts the playback switches at the
    // timeline's left edge instead of adrift over the track headers. Set here
    // rather than beside the strip: the width is not known until now.
    if (m_toolPanel) m_toolPanel->setBrowserZoneWidth(m_browserWidth);
    connect(m_browser, &FileBrowserPanel::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 3000); });
    connect(m_browser, &FileBrowserPanel::settingsRequested, this,
            [this] { openSettings(SettingsWindow::kBrowserTab); });
    connect(m_browser, &FileBrowserPanel::channelStripPresetActivated, this,
            [this](const QString& path) {
                if (m_selectedTrackId.isEmpty()) {
                    statusBar()->showMessage(
                        tr("Select a mixer channel before applying a preset"),
                        4000);
                    return;
                }
                const audio::Result result =
                    m_controller.applyChannelStripPreset(
                        m_selectedTrackId.toStdString(), path.toStdString());
                if (!result) {
                    QMessageBox::warning(
                        this, tr("Channel Strip Preset"),
                        tr("%1 could not be applied.\n\n%2")
                            .arg(QFileInfo(path).fileName(),
                                 QString::fromStdString(result.message())));
                    return;
                }
                syncViews();
                markDirty();
                statusBar()->showMessage(
                    tr("Applied Channel Strip preset “%1”")
                        .arg(ui::channelstrippresets::displayName(path)),
                    4000);
            });
    connect(m_browser, &FileBrowserPanel::projectTemplateActivated, this,
            &MainWindow::createProjectFromTemplatePath);
    connect(m_browser, &FileBrowserPanel::projectTemplateTracksRequested, this,
            &MainWindow::addProjectTemplateTracks);

    auto* browserHandle = new ui::ResizeHandle(Qt::Vertical, m_browser);
    browserHandle->setEdge(m_browserOnLeft ? Qt::RightEdge : Qt::LeftEdge);
    browserHandle->onDragStart = [this] { m_browserDragStartWidth = m_browserWidth; };
    browserHandle->onDrag = [this](int delta) {
        // Dragging away from the window's edge widens the panel, which means
        // the sign flips with the side it is on.
        const int wanted =
            m_browserDragStartWidth + (m_browserOnLeft ? delta : -delta);
        m_browserWidth = std::clamp(wanted, 160, std::max(160, width() / 2));
        m_browser->setFixedWidth(m_browserWidth);
        if (m_toolPanel) m_toolPanel->setBrowserZoneWidth(m_browserWidth);
        ui::browserprefs::setWidth(m_browserWidth);
        applyRightPanelWidths();
    };
    m_browserHandle = browserHandle;

    // The assistant sits outside the workspace row so its surface reaches the
    // same top edge as the transport and becomes their shared right boundary.
    m_aiPanel = new AiChatPanel(&m_controller, central);
    m_aiPanel->setSelectionModel(&m_selection);
    m_aiPanel->setCommandManager(m_shortcuts);
    m_aiWidth = ui::aiprefs::width();
    m_aiPanel->setFixedWidth(m_aiWidth);
    connect(m_aiPanel, &AiChatPanel::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 4000); });
    connect(m_aiPanel, &AiChatPanel::settingsRequested, this,
            [this] { openSettings(SettingsWindow::kAiTab); });
    // Tools change the document as they run, so the views have to follow along
    // rather than wait for the end of the request.
    connect(m_aiPanel, &AiChatPanel::projectChanged, this,
            [this](bool localFileDirty) {
        syncViews();
        if (localFileDirty) markDirty();
    });

    auto* aiHandle = new ui::ResizeHandle(Qt::Vertical, m_aiPanel);
    aiHandle->setEdge(Qt::LeftEdge);
    aiHandle->onDragStart = [this] { m_aiDragStartWidth = m_aiWidth; };
    aiHandle->onDrag = [this](int delta) {
        // Always on the right, so widening is always dragging left.
        m_aiWidth = std::clamp(m_aiDragStartWidth - delta, 240, 720);
        applyRightPanelWidths();
        // A drag stops at the real boundary. Keeping an unreachable preferred
        // width here creates a dead zone and, at the far edge, makes the panel
        // appear to grow out of the window on its right side.
        if (m_aiPanel) m_aiWidth = m_aiPanel->width();
        ui::aiprefs::setWidth(m_aiWidth);
    };
    m_aiHandle = aiHandle;

    // The web surface is a second, independent right-hand plate. Its cheap
    // container and resize handle exist from startup, but Chromium itself is
    // created only when the panel is opened for the first time.
    m_webWidth = ui::webprefs::width();
    m_webContainer = new QWidget(central);
    // QWebEngineView creates a native Chromium child. Give it a stable native
    // parent before the shell is shown, or its first lazy construction rebuilds
    // the backing hierarchy and leaves the workspace visually displaced until
    // another panel change forces a relayout.
    if (platform != QLatin1String("offscreen") &&
        platform != QLatin1String("minimal")) {
        m_webContainer->setAttribute(Qt::WA_NativeWindow);
    }
    m_webContainer->setObjectName(QStringLiteral("WebBrowserContainer"));
    m_webContainer->setFixedWidth(m_webWidth);
    auto* webContainerLayout = new QVBoxLayout(m_webContainer);
    webContainerLayout->setContentsMargins(0, 0, 0, 0);
    webContainerLayout->setSpacing(0);

    auto* webHandle = new ui::ResizeHandle(Qt::Vertical, m_webContainer);
    webHandle->setEdge(Qt::LeftEdge);
    webHandle->onDragStart = [this] { m_webDragStartWidth = m_webWidth; };
    webHandle->onDrag = [this](int delta) {
        m_webWidth = std::clamp(m_webDragStartWidth - delta,
                                ui::webprefs::kMinWidth,
                                ui::webprefs::kMaxWidth);
        applyRightPanelWidths();
        if (m_webContainer) m_webWidth = m_webContainer->width();
        ui::webprefs::setWidth(m_webWidth);
    };
    m_webHandle = webHandle;
    m_webContainer->hide();
    m_webHandle->hide();

    m_inspector = new InspectorWidget(&m_controller, row);

    // Arrangement (track headers + timeline) fills the column; the mixer floats
    // over it at the bottom and can be dragged up across the tracks, all the
    // way to the transport bar.
    auto* host = new ArrangementHost(row);
    host->onResize = [this](const QSize&) {
        applyTrackHeaderWidth();
        layoutMixer();
    };
    m_arrangementHost = host;

    m_trackList = new TrackListWidget(&m_controller, m_arrangementHost);
    m_timeline = new TimelineWidget(&m_controller, m_arrangementHost);
    m_trackHeaderWidth = std::max(
        ui::kMinTrackHeaderWidth,
        QSettings().value(ui::kTrackHeaderWidthSetting,
                          ui::kTrackHeaderWidth).toInt());
    m_trackList->setFixedWidth(m_trackHeaderWidth);
    if (m_toolPanel) m_toolPanel->setTrackZoneWidth(m_trackHeaderWidth);

    auto* trackHeaderHandle =
        new ui::ResizeHandle(Qt::Vertical, m_trackList);
    trackHeaderHandle->setEdge(Qt::RightEdge);
    trackHeaderHandle->setToolTip(
        tr("Drag to resize the track controls"));
    trackHeaderHandle->setAccessibleName(
        tr("Resize track controls"));
    trackHeaderHandle->onDragStart = [this] {
        m_trackHeaderDragStartWidth = m_trackList
                                          ? m_trackList->width()
                                          : m_trackHeaderWidth;
    };
    trackHeaderHandle->onDrag = [this](int delta) {
        if (!m_arrangementHost || !m_trackList || !m_timeline) return;
        const int maximum = std::max(
            ui::kMinTrackHeaderWidth,
            m_arrangementHost->width() - ui::kMinTimelineWidth);
        m_trackHeaderWidth = std::clamp(
            m_trackHeaderDragStartWidth + delta,
            ui::kMinTrackHeaderWidth, maximum);
        applyTrackHeaderWidth();
        QSettings().setValue(ui::kTrackHeaderWidthSetting,
                             m_trackHeaderWidth);
        applyRightPanelWidths();
    };
    m_trackHeaderHandle = trackHeaderHandle;

    auto* arrangementRow = new QHBoxLayout(m_arrangementHost);
    arrangementRow->setContentsMargins(0, 0, 0, 0);
    arrangementRow->setSpacing(0);
    arrangementRow->addWidget(m_trackList);
    arrangementRow->addWidget(m_timeline, 1);

    // The mixer sits in the centre column, so it spans from the track headers
    // to the right edge of the window — never under the inspector. It is kept
    // out of the arrangement layout so it can overlap the tracks instead of
    // pushing them out of the way.
    m_mixer = new MixerWidget(&m_controller, m_arrangementHost);
    // The pane can be squeezed to a sliver: the strips keep their full height
    // and the mixer scrolls, so nothing is ever clipped or folded away.
    m_mixer->setMinimumHeight(140);

    auto* handle = new ui::ResizeHandle(Qt::Horizontal, m_mixer);
    handle->setEdge(Qt::TopEdge);
    handle->onDragStart = [this] { m_mixerDragStartHeight = m_mixerHeight; };
    handle->onDrag = [this](int deltaY) {
        // deltaY is measured from where the drag started, so the new height has
        // to come from the height at that moment. Applying it to the running
        // height made every move event compound and the panel jump.
        const int hostH = m_arrangementHost->height();
        m_mixerHeight = std::clamp(m_mixerDragStartHeight - deltaY,
                                   m_mixer->minimumHeight(),
                                   std::max(m_mixer->minimumHeight(),
                                            hostH));
        layoutMixer();
    };
    m_mixerHandle = handle;

    m_timeline->setSelectionModel(&m_selection);

    relayoutRow();
    layoutMixer();
    column->addWidget(row, 1);

    shell->addWidget(workspace, 1);
    shell->addWidget(m_webContainer);
    shell->addWidget(m_aiPanel);

    setCentralWidget(central);
    // Start with the keyboard on the window itself rather than in whatever
    // field happens to be first in the tab chain, so Space plays straight away.
    central->setFocusPolicy(Qt::StrongFocus);
    central->setFocus();

    // ── Wiring ──
    connect(m_transport, &TransportBar::playPauseRequested, this, &MainWindow::onPlayPause);
    connect(m_transport, &TransportBar::stopRequested, this, &MainWindow::onStop);
    connect(m_transport, &TransportBar::recordRequested, this, &MainWindow::onRecord);
    connect(m_transport, &TransportBar::returnToStartRequested, this, &MainWindow::onReturnToStart);
    connect(m_transport, &TransportBar::nudgeRequested, this, &MainWindow::onNudge);
    connect(m_transport, &TransportBar::loopToggled, this, &MainWindow::onToggleLoop);
    connect(m_transport, &TransportBar::metronomeToggled, this, &MainWindow::onToggleMetronome);
    connect(m_transport, &TransportBar::typingKeyboardToggled, this,
            &MainWindow::setTypingKeyboardEnabled);
    connect(m_typingKeyboard, &TypingKeyboard::octaveChanged, this,
            [this](int octave) {
                m_transport->setTypingKeyboardOctave(octave);
                statusBar()->showMessage(
                    tr("Typing keyboard starts on C%1").arg(octave), 1500);
            });
    connect(m_typingKeyboard, &TypingKeyboard::notePlayed, this, [this](int pitch) {
        statusBar()->showMessage(
            tr("Note %1").arg(QString::fromStdString(
                daw::miditools::pitchName(pitch))),
            800);
    });
    connect(m_transport, &TransportBar::tempoChanged, this, &MainWindow::onTempoChanged);
    connect(m_transport, &TransportBar::timeSignatureChanged, this,
            [this](int numerator, int denominator) {
                const auto result =
                    m_controller.setTimeSignature(numerator, denominator);
                if (m_timeline) m_timeline->update();
                if (m_pianoRoll) m_pianoRoll->refresh();
                m_selection.refresh();
                layoutContextPanel();
                m_transport->refresh();
                if (daw::collab::marksLocalFileDirty(result)) markDirty();
            });
    connect(m_transport, &TransportBar::masterVolumeChanged, this,
            [this](bool committed) {
                if (m_mixer) m_mixer->syncFromModel();
                if (m_inspector) m_inspector->syncFromModel();
                if (committed) markDirty();
            });
    connect(m_transport, &TransportBar::saveRequested, this, &MainWindow::onSaveProject);
    connect(m_transport, &TransportBar::openRequested, this, &MainWindow::onOpenProject);
    connect(m_transport, &TransportBar::importRequested, this, &MainWindow::onImportAudio);
    connect(m_transport, &TransportBar::exportRequested, this, &MainWindow::onExport);
    connect(m_transport, &TransportBar::gridChanged, this, [this] {
        m_timeline->setGridBeats(m_transport->gridBeats());
    });
    connect(m_transport, &TransportBar::snapChanged, this, [this](bool on) {
        m_timeline->setSnapEnabled(on);
    });
    connect(m_transport, &TransportBar::toolChanged, this, [this](int tool) {
        m_timeline->setTool(static_cast<TimelineWidget::Tool>(tool));
    });
    connect(m_transport, &TransportBar::secondaryToolChanged, this,
            [this](int tool) {
                m_timeline->setSecondaryTool(static_cast<TimelineWidget::Tool>(tool));
            });
    // The strip restored both tools from settings before anything was listening,
    // so the stored pair is pushed once here.
    m_timeline->setSecondaryTool(
        static_cast<TimelineWidget::Tool>(m_transport->secondaryToolIndex()));
    // Restart switch mirrors the transport setting (and keeps it in sync).
    connect(m_toolPanel, &ToolPanel::restartModeToggled, this,
            [this](bool on) {
                using Mode = daw::EngineController::PlaybackMode;
                m_controller.setPlaybackMode(on ? Mode::Restart : Mode::Resume);
                QSettings().setValue(ui::kPlaybackModeSetting, int(m_controller.playbackMode()));
            });
    connect(m_toolPanel, &ToolPanel::playFromClipToggled, this,
            [this](bool on) { m_playFromClip = on; });
    connect(m_toolPanel, &ToolPanel::automationVisibilityToggled, this,
            &MainWindow::setAllAutomationLanesVisible);
    connect(m_toolPanel, &ToolPanel::automationCreationModeToggled, this,
            [this](bool enabled) {
                m_automationCreationLatched = enabled;
                updateAutomationCreationMode();
            });
    connect(m_toolPanel, &ToolPanel::addTrackRequested, this,
            &MainWindow::onAddAudioTrack);
    connect(m_toolPanel, &ToolPanel::addTrackMenuRequested, this,
            [this](const QPoint& globalPos) {
                QMenu menu(this);
                const auto kinds = ui::addTrackKindItems(menu);
                QAction* chosen = menu.exec(globalPos);
                const auto spec = kinds.constFind(chosen);
                if (spec == kinds.constEnd()) return;
                spec->create(m_controller);
                syncViews();
                markDirty();
            });
    connect(m_transport, &TransportBar::timeFormatChanged, this, [this] {
        m_timeline->setShowBars(m_transport->showsBars());
    });
    connect(m_transport, &TransportBar::positionChanged, this, [this] {
#ifdef DAW_ENABLE_COLLABORATION
        if (m_collaboration && !m_applyingRemoteTransport) {
            m_collaboration->localSessionState()
                ->noteLocalTransportInteraction();
            publishSessionTransport(true);
        }
#endif
        if (m_timeline) m_timeline->refreshPlaybackFrame();
        if (m_pianoRoll) m_pianoRoll->refreshPlayhead();
    });
    connect(m_transport, &TransportBar::zoomRequested, this, [this](int dir) {
        if (dir == 0) m_timeline->zoomToFit();
        else m_timeline->zoomBy(dir > 0 ? 1.3 : 1.0 / 1.3);
    });
    m_timeline->setGridBeats(m_transport->gridBeats());
    m_timeline->setSnapEnabled(m_transport->snapEnabled());

    // The set first, then the primary: `selectTrackFromHeader` publishes what
    // is selected, and it has to publish all of it, not just the last row
    // clicked. Both signals are emitted together and in this order.
    connect(m_trackList, &TrackListWidget::selectionChanged, this,
            &MainWindow::selectTrackFromHeader);
    connect(m_trackList, &TrackListWidget::packRequested, this,
            [this](bool summing) { packSelectionIntoFolder(summing); });
    connect(m_trackList, &TrackListWidget::automateControlRequested, this,
            [this](const QString& trackId, bool pan) {
                daw::AutomationTarget target;
                target.kind = pan ? daw::AutomationTargetKind::TrackPan
                                  : daw::AutomationTargetKind::TrackVolume;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_trackList, &TrackListWidget::automateMuteRequested, this,
            [this](const QString& trackId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::TrackMute;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_trackList, &TrackListWidget::automateSendRequested, this,
            [this](const QString& trackId, const QString& sendId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::SendLevel;
                target.channelId = trackId.toStdString();
                target.sendId = sendId.toStdString();
                automateTarget(target);
            });
    connect(m_trackList, &TrackListWidget::recordPinToggled, this,
            &MainWindow::setRecordPinned);
    connect(m_trackList, &TrackListWidget::tracksChanged, this,
            &MainWindow::onTracksChanged);
    // The lane stack changes in this frame. Channel strips are much more
    // expensive to construct, so they follow after Qt has had one paint turn.
    connect(m_trackList, &TrackListWidget::orderChanged, this, [this] {
        syncStructureViews();
        markDirty();
    });
    connect(m_trackList, &TrackListWidget::projectTemplateTracksRequested, this,
            &MainWindow::addProjectTemplateTracks);
    connect(m_trackList, &TrackListWidget::pluginEditorRequested, this,
            &MainWindow::openPluginEditor);
    connect(m_timeline, &TimelineWidget::clipSelected, this,
            [this](const QString& trackId, const QString&) {
                // Clips first: selecting a clip also selects its track, and the
                // clip context has to win. Publishing here rather than letting
                // the timeline's own end-of-event push do it keeps the panel
                // from flashing a track context on the way through.
                m_timeline->publishSelection();
                m_trackList->setSelectedTrack(trackId);
                onSelectionChanged(trackId);
            });
    connect(m_timeline, &TimelineWidget::projectTemplateTracksRequested, this,
            &MainWindow::addProjectTemplateTracks);
    connect(m_timeline, &TimelineWidget::loopRangeChanged, this, [this] {
        // Dragging a region out arms the cycle, so the transport's lamp has to
        // follow — and a piano roll open on the same project shows the same
        // region.
        m_transport->setCycleEnabled(m_controller.isLoopEnabled());
        if (m_pianoRoll) m_pianoRoll->refresh();
        m_timeline->update();
        markDirty();
    });
    connect(m_timeline, &TimelineWidget::pluginDropped, this,
            [this](const QString& plugin, const QString& target) {
                statusBar()->showMessage(
                    tr("%1 added to %2").arg(plugin, target), 3000);
                syncViews();
                markDirty();
            });
    connect(m_timeline, &TimelineWidget::pluginEditorRequested, this,
            &MainWindow::openPluginEditor);
    connect(m_timeline, &TimelineWidget::bounceInPlaceRequested, this,
            &MainWindow::onBounceInPlace);
    connect(m_timeline, &TimelineWidget::offlineRenderRequested, this,
            &MainWindow::onOfflineRender);
    connect(m_timeline, &TimelineWidget::projectEdited, this, [this] {
        m_orphanEditorSweepPending = true;
        markDirty();
        // An arrangement edit can retrim or delete the clip being edited, so
        // an open roll follows along.
        if (m_pianoRoll) m_pianoRoll->refresh();
        if (m_inspector) m_inspector->syncFromModel();
        for (SampleEditorWindow* editor : m_sampleEditors) {
            if (editor) editor->refresh();
        }
    });
    connect(m_timeline, &TimelineWidget::playheadMoved, this, [this] {
#ifdef DAW_ENABLE_COLLABORATION
        if (m_collaboration && !m_applyingRemoteTransport) {
            m_collaboration->localSessionState()
                ->noteLocalTransportInteraction();
            publishSessionTransport(true);
        }
#endif
        if (m_pianoRoll) m_pianoRoll->refreshPlayhead();
        if (m_transport) m_transport->refresh();
    });
    connect(m_timeline, &TimelineWidget::tracksChanged, this, [this] {
        syncViews();
        markDirty();
    });
    // The comp editor grows and shrinks a lane on its own animation clock, and
    // the header column is a second column of real widgets that has to follow
    // it frame for frame — otherwise an expanded stack of takes runs down over
    // the tracks below it while their headers stay put.
    connect(m_timeline, &TimelineWidget::laneHeightsChanged, this, [this] {
        if (m_trackList) m_trackList->syncRowHeights();
        m_timeline->clampVerticalScroll();
    });
    // One scroll offset, two columns: the lanes own it and the headers follow.
    connect(m_timeline, &TimelineWidget::verticalScrollChanged, this,
            [this](int y) {
                if (m_trackList) m_trackList->setVerticalScroll(y);
            });
    connect(m_trackList, &TrackListWidget::verticalScrollRequested, this,
            [this](int delta) {
                m_timeline->setVerticalScroll(m_timeline->verticalScroll() + delta);
            });
    connect(m_timeline, &TimelineWidget::midiFileImported, this,
            &MainWindow::onMidiFileImported);
    connect(m_timeline, &TimelineWidget::openPianoRollRequested, this,
            &MainWindow::openPianoRoll);
    connect(m_timeline, &TimelineWidget::openPatternRequested, this,
            &MainWindow::openPattern);
    connect(m_timeline, &TimelineWidget::openSampleEditorRequested, this,
            &MainWindow::openSampleEditor);
    connect(m_timeline, &TimelineWidget::openAutomationEditorRequested, this,
            &MainWindow::openAutomationEditor);
    connect(m_timeline, &TimelineWidget::audioAnalysisRequested, this,
            &MainWindow::analyzeAudioClip);
    connect(m_trackList, &TrackListWidget::trackHeightChanged, this, [this] {
        // The track header already re-laid out its stack in the drag handler.
        // Shrinking can also make the current shared scroll offset illegal, so
        // clamp it now; the resulting signal moves the headers to the exact
        // same origin before either column paints this frame.
        m_timeline->clampVerticalScroll();
        m_timeline->update();
        markDirty();
    });
    connect(m_trackList, &TrackListWidget::openPatternRequested, this,
            &MainWindow::openPattern);

    connect(m_mixer, &MixerWidget::trackSelected, this, [this](const QString& id) {
        m_trackList->setSelectedTrack(id);
        selectTrackFromHeader(id);
    });
    connect(m_mixer, &MixerWidget::edited, this,
            [this](bool localFileDirty) {
        if (localFileDirty) markDirty();
        m_trackList->syncTrackValues();
        // …and the same the other way: the panel is showing the value that
        // just moved in the strip.
        if (m_inspector) m_inspector->syncFromModel();
        if (m_contextPanel) m_contextPanel->refresh();
        m_timeline->update();
    });
    connect(m_mixer, &MixerWidget::structureChanged, this, [this] {
        // The originating mixer rebuilds itself. Only the selected-track peer
        // needs reconstruction; headers and timeline contain no slot widgets.
        if (m_inspector) m_inspector->rebuildForTrack(m_selectedTrackId);
    });
    connect(m_mixer, &MixerWidget::pluginEditorRequested, this,
            &MainWindow::openPluginEditor);
    connect(m_mixer, &MixerWidget::settingsRequested, this,
            [this] { openSettings(SettingsWindow::kAudioTab); });
    connect(m_mixer, &MixerWidget::automateControlRequested, this,
            [this](const QString& trackId, bool pan) {
                daw::AutomationTarget target;
                target.kind = pan ? daw::AutomationTargetKind::TrackPan
                                  : daw::AutomationTargetKind::TrackVolume;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_mixer, &MixerWidget::automateMuteRequested, this,
            [this](const QString& trackId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::TrackMute;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_mixer, &MixerWidget::automateSendRequested, this,
            [this](const QString& trackId, const QString& sendId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::SendLevel;
                target.channelId = trackId.toStdString();
                target.sendId = sendId.toStdString();
                automateTarget(target);
            });
    connect(m_mixer, &MixerWidget::openPatternRequested, this,
            &MainWindow::openPattern);
    connect(m_mixer, &MixerWidget::trackRemoved, this, [this](const QString& id) {
        m_controller.removeTrack(id.toStdString());
        if (m_selectedTrackId == id) m_selectedTrackId.clear();
        syncViews();
        markDirty();
    });

    connect(m_inspector, &InspectorWidget::edited, this,
            [this](bool localFileDirty) {
        if (localFileDirty) markDirty();
        m_trackList->syncTrackValues();
        m_mixer->syncFromModel();
        m_timeline->update();
        for (SampleEditorWindow* editor : m_sampleEditors) {
            if (editor) editor->refresh();
        }
    });
    connect(m_inspector, &InspectorWidget::stretchToolRequested, this,
            [this] { setEditTool(6); });
    connect(m_inspector, &InspectorWidget::structureChanged, this, [this] {
        // The inspector has rebuilt its own strip. Refresh the mixer once so
        // its slot list follows without paying this cost for fader gestures.
        if (m_mixer) {
            m_mixer->rebuild();
            m_mixer->setSelectedTrack(m_selectedTrackId);
        }
    });
    connect(m_inspector, &InspectorWidget::pluginEditorRequested, this,
            &MainWindow::openPluginEditor);
    connect(m_inspector, &InspectorWidget::automateControlRequested, this,
            [this](const QString& trackId, bool pan) {
                daw::AutomationTarget target;
                target.kind = pan ? daw::AutomationTargetKind::TrackPan
                                  : daw::AutomationTargetKind::TrackVolume;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_inspector, &InspectorWidget::automateMuteRequested, this,
            [this](const QString& trackId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::TrackMute;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_inspector, &InspectorWidget::automateSendRequested, this,
            [this](const QString& trackId, const QString& sendId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::SendLevel;
                target.channelId = trackId.toStdString();
                target.sendId = sendId.toStdString();
                automateTarget(target);
            });
    connect(m_inspector, &InspectorWidget::collapsedChanged, this,
            [this](bool collapsed) {
                m_transport->setInspectorVisible(!collapsed);
                m_toolPanel->setInspectorVisible(!collapsed);
                m_toolPanel->setInspectorZoneWidth(collapsed
                                                      ? InspectorWidget::kRailWidth
                                                      : InspectorWidget::kExpandedWidth);
                if (m_showInspectorAction)
                    m_showInspectorAction->setChecked(!collapsed);
                applyRightPanelWidths();
            });

    connect(m_contextPanel, &ContextPanel::projectEdited, this,
            [this](bool localFileDirty) {
        m_orphanEditorSweepPending = true;
        if (localFileDirty) markDirty();
        m_timeline->update();
        m_trackList->update();
        if (m_mixer) m_mixer->refreshMeters();
    });
    // The panel is a second set of controls over the same values as the mixer
    // and the track headers. Without this the level moved in the engine and
    // nowhere else on screen, and the strip only caught up when it was next
    // rebuilt — which read as "I have to re-open the channel for it to apply".
    connect(m_contextPanel, &ContextPanel::liveEdited, this, [this] {
        m_trackList->syncTrackValues();
        if (m_mixer) m_mixer->syncFromModel();
        m_timeline->update();
    });
    connect(m_contextPanel, &ContextPanel::tracksChanged, this,
            [this](bool localFileDirty) {
        syncViews();
        if (localFileDirty) markDirty();
    });
    connect(m_contextPanel, &ContextPanel::recordingToggleRequested, this,
            &MainWindow::onRecordKey);
    // Overwrite / Layer recording is set on the recording panel and nowhere
    // else; this only reports the change and keeps an open settings window
    // from drifting away from it.
    connect(m_contextPanel, &ContextPanel::recordModeChanged, this,
            &MainWindow::onRecordModeChanged);
    connect(m_contextPanel, &ContextPanel::automationToggleRequested, this,
            [this](const QString& trackId) {
                if (m_trackList) m_trackList->setSelectedTrack(trackId);
                selectTrackFromHeader(trackId);
                toggleAutomationLanes();
            });
    connect(m_contextPanel, &ContextPanel::automateControlRequested, this,
            [this](const QString& trackId, bool pan) {
                daw::AutomationTarget target;
                target.kind = pan ? daw::AutomationTargetKind::TrackPan
                                  : daw::AutomationTargetKind::TrackVolume;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_contextPanel, &ContextPanel::automateMuteRequested, this,
            [this](const QString& trackId) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::TrackMute;
                target.channelId = trackId.toStdString();
                automateTarget(target);
            });
    connect(m_contextPanel, &ContextPanel::automationEditorRequested, this,
            &MainWindow::openAutomationEditor);
    connect(m_contextPanel, &ContextPanel::midiEditorRequested, this,
            &MainWindow::openPianoRoll);
    connect(m_contextPanel, &ContextPanel::patternEditorRequested, this,
            &MainWindow::openPattern);
    connect(m_contextPanel, &ContextPanel::pluginEditorRequested, this,
            &MainWindow::openPluginEditor);

    connect(m_transport, &TransportBar::mixerToggled, this,
            &MainWindow::setMixerVisible);
    connect(m_transport, &TransportBar::inspectorToggled, this,
            &MainWindow::setInspectorVisible);
    connect(m_transport, &TransportBar::browserToggled, this,
            &MainWindow::setBrowserVisible);
    connect(m_transport, &TransportBar::webToggled, this,
            [this](bool on) { setWebVisible(on); });
    connect(m_transport, &TransportBar::aiToggled, this,
            [this](bool on) { setAiVisible(on); });
    connect(m_transport, &TransportBar::detachMixerRequested, this,
            &MainWindow::onDetachMixer);
    connect(m_transport, &TransportBar::addTrackRequested, this,
            &MainWindow::onAddAudioTrack);
    connect(m_transport, &TransportBar::settingsRequested, this,
            [this] { openSettings(SettingsWindow::kAudioTab); });
}

// A layer punched into an expanded clip becomes the next row of that clip's
// take stack, and the lane has to grow by that row while the take is running.
// Lane heights are shared layout state (the header column measures itself the
// same way), so this is settled once a frame, here, rather than by either
// widget while it paints.
void MainWindow::syncPendingTakeRows() {
    bool any = false;
    for (const std::string& trackId : m_controller.recordingTracks()) {
        const auto preview = m_controller.recordingPreview(trackId);
        std::string clipId;
        if (preview.active && preview.layered) {
            if (const auto* track = m_controller.project().findTrack(trackId)) {
                for (const auto& clip : track->clips) {
                    if (clip.id == preview.targetClipId && clip.expanded)
                        clipId = clip.id;
                }
            }
        }
        ui::setPendingTakeClip(trackId, clipId);
        any = any || !clipId.empty();
    }
    if (!m_controller.isRecording()) {
        // Nothing is running any more: every lane goes back to its own height.
        for (const auto& track : m_controller.project().tracks)
            ui::setPendingTakeClip(track.id, {});
        any = false;
    }
    if (any != m_hadRecordingRows) {
        m_hadRecordingRows = any;
        // Heights only — rebuilding the header column mid-take would throw away
        // and re-create every row widget while the user is playing into one.
        if (m_trackList) m_trackList->syncRowHeights();
        if (m_timeline) m_timeline->update();
    }
}

void MainWindow::addDemoTracks(int count) {
    for (int i = 0; i < count; ++i) {
        const int n = int(m_controller.project().tracks.size()) + 1;
        m_controller.addTrack(daw::TrackKind::Audio,
                              tr("Audio %1").arg(n).toStdString());
    }
    syncViews();
}

void MainWindow::setMixerHeightForShot(int height) {
    m_mixerHeight = std::max(0, height);
    layoutMixer();
}

void MainWindow::scrollArrangement(int px) {
    if (m_timeline) m_timeline->setVerticalScroll(m_timeline->verticalScroll() + px);
}

void MainWindow::stageRecordingShot(const QString& mode) {
    auto trackNamed = [&](const char* name) {
        for (const auto& t : m_controller.project().tracks) {
            if (t.name == name) return t.id;
        }
        return std::string();
    };
    const bool layered = mode == QLatin1String("layers");
    // Drums carries the demo's layered clip; Bass has room to the right of its
    // own clip for a take that lands as a clip of its own.
    const std::string target = trackNamed(layered ? "Drums" : "Bass");
    if (target.empty()) return;

    auto prefs = m_controller.recordingPrefs();
    prefs.mode = layered ? daw::RecordMode::Layers : daw::RecordMode::Overwrite;
    m_controller.setRecordingPrefs(prefs);

    const double from = layered ? 1.3 : 8.0;
    const double elapsed = layered ? 1.4 : 3.4;
    m_controller.seekSeconds(from);
    setRecordEngaged(true);
    if (!m_controller.startRecording(target)) return;
    m_controller.seekSeconds(from + elapsed);
    // Decaying plucks under a slow swell: a shape, rather than the slab a
    // constant level would draw.
    m_controller.seedRecordingForShot(target, elapsed, [](double t) {
        const double pluck = std::exp(-5.0 * std::fmod(t, 0.45));
        return float(0.12 + 0.75 * pluck * (0.55 + 0.45 * std::sin(t * 2.2)));
    });
    syncViews();
}

void MainWindow::populateDemo() {
    struct Demo { const char* name; daw::TrackKind kind; uint32_t color; };
    const Demo demos[] = {
        {"Drums", daw::TrackKind::Audio, 0xE0645A},
        {"Bass", daw::TrackKind::Audio, 0x4A90D9},
        {"Synth", daw::TrackKind::Instrument, 0x8FBF5A},
        {"Keys", daw::TrackKind::Midi, 0xD69B4A},
        {"Reverb Bus", daw::TrackKind::Bus, 0xB58BD6},
    };
    std::vector<std::string> ids;
    for (const auto& d : demos) {
        const std::string id = m_controller.addTrack(d.kind, d.name);
        m_controller.setTrackColor(id, d.color);
        ids.push_back(id);
    }
    // A send into the bus so the mixer shows a live routing, not just slots,
    // and one folder of each kind so the hierarchy — and the difference
    // between a drawer and a group bus — is visible at a glance.
    if (ids.size() >= 5) {
        m_controller.addSend(ids[0], ids[4]);
        m_controller.packIntoFolder({ids[0], ids[1]}, "Rhythm",
                                    /*summing=*/true);
        m_controller.packIntoFolder({ids[2], ids[3]}, "Ideas",
                                    /*summing=*/false);
    }

    // A MIDI clip with a short chord progression, so the arrangement shows what
    // a note block looks like next to a waveform.
    if (ids.size() >= 4) {
        const std::string midiClip = m_controller.addMidiClip(ids[3], 1.0, 4.0);
        struct Note { int pitch; double start; double length; };
        const Note phrase[] = {
            {60, 0.0, 0.9}, {64, 0.0, 0.9}, {67, 0.0, 0.9},     // C major
            {62, 1.0, 0.9}, {65, 1.0, 0.9}, {69, 1.0, 0.9},     // D minor
            {64, 2.0, 0.9}, {67, 2.0, 0.9}, {71, 2.0, 0.9},     // E minor
            {65, 3.0, 1.9}, {69, 3.0, 1.9}, {72, 3.0, 1.9},     // F major
            {72, 5.0, 0.5}, {74, 5.5, 0.5}, {76, 6.0, 1.5},     // a little melody
        };
        for (const auto& n : phrase) {
            m_controller.addNote(ids[3], midiClip, n.pitch, n.start, n.length);
        }
        // Taller than the audio lanes, so the note block reads as notes rather
        // than as the hairlines a two-octave range makes of a 72 px lane.
        m_controller.setTrackHeight(ids[3], 132.0);
    }

    // Synthesize a short tone WAV and drop a couple of clips so the timeline
    // shows real arrangement content.
    const QString tonePath = QDir::temp().filePath("daw_demo_tone.wav");
    {
        // Four decaying plucks rather than a flat sine, so the clip's waveform
        // overview actually shows a shape.
        audio::AudioBuffer tone(2, 96000); // 2 s @ 48k
        for (audio::BufferSize f = 0; f < 96000; ++f) {
            const float t = float(f) / 48000.0f;
            const float phase = std::fmod(t, 0.5f);
            const float envelope = std::exp(-6.0f * phase);
            const float s = 0.7f * envelope *
                            std::sin(2.0f * 3.14159265f * 220.0f * t);
            tone.getChannel(0)[f] = s;
            tone.getChannel(1)[f] = s;
        }
        audio::AudioRecorder rec;
        rec.initialize(48000, 2);
        rec.writeWAVFile(tonePath.toStdString(), tone, 48000);
    }
    std::string demoClip;
    if (ids.size() >= 2) {
        demoClip = m_controller.importAudio(tonePath.toStdString(), ids[0], 1.0);
        m_controller.importAudio(tonePath.toStdString(), ids[1], 4.0);
    }

    // A layered clip, so a screenshot shows the take badge and the comp editor
    // rather than only plain clips. Three passes over the same stretch, comped
    // so the result is assembled from all three — what loop recording plus a
    // swipe would have produced, built without an input device.
    if (!demoClip.empty()) {
        const std::string track = ids[0];
        for (int i = 0; i < 2; ++i) {
            m_controller.addTakeFromFile(track, demoClip, tonePath.toStdString(),
                                         0.0);
        }
        // addTakeFromFile promoted the imported audio to Take 1, so the clip now
        // holds three; the comp is swiped across all of them.
        const daw::ClipModel* clip = nullptr;
        if (const auto* model = m_controller.project().findTrack(track)) {
            for (const auto& candidate : model->clips) {
                if (candidate.id == demoClip) clip = &candidate;
            }
        }
        if (clip && !clip->takes.empty()) {
            const double length = clip->durationSeconds;
            std::vector<std::string> all;
            for (const auto& take : clip->takes) all.push_back(take.id);
            m_controller.beginCompEdit(track, demoClip);
            for (size_t i = 0; i < all.size(); ++i) {
                m_controller.setCompSegment(track, demoClip, all[i],
                                            length * double(i) / double(all.size()),
                                            length * double(i + 1) / double(all.size()));
            }
            m_controller.endCompEdit();
            m_controller.setClipExpanded(track, demoClip, true);
        }
    }
    syncViews();

    // Select the first clip, so a demo/screenshot run shows the context panel
    // in its normal state rather than an empty arrangement.
    if (!demoClip.empty()) {
        m_selection.setClips({{QString::fromStdString(ids[0]),
                               QString::fromStdString(demoClip)}});
        onSelectionChanged(QString::fromStdString(ids[0]));
        // The clip is flagged expanded above; the lane still has to grow into
        // it, which is what animateClipOpen does.
        m_timeline->animateClipOpen(QString::fromStdString(ids[0]),
                                    QString::fromStdString(demoClip));
    }
}

void MainWindow::syncViews() {
    m_orphanEditorSweepPending = true;
    if (m_trackList) {
        m_trackList->rebuild();
        m_trackList->setSelectedTrack(m_selectedTrackId);
    }
    if (m_mixer) {
        m_mixer->rebuild();
        m_mixer->setSelectedTrack(m_selectedTrackId);
    }
    if (m_timeline) {
        m_timeline->setSelectedTrack(m_selectedTrackId);
        m_timeline->update();
    }
    if (m_inspector) {
        m_inspector->rebuildForTrack(m_selectedTrackId);
        m_inspector->setSelection(m_selectedTrackId,
                                  m_selection.singleClip().clipId);
    }
    if (m_timeline) m_timeline->clampVerticalScroll();
    // Undo, redo, opening a project and deleting a track all come through here,
    // so this one line is what keeps an open piano roll honest — including
    // showing its empty state when the clip it was editing has gone.
    if (m_pianoRoll) m_pianoRoll->refresh();
    for (SampleEditorWindow* editor : m_sampleEditors) {
        if (editor) editor->refresh();
    }
    // And the same for any open curve editor: an undo, a rename or a plugin
    // swapped in the slot it drives all change what its three fields should
    // say. A curve whose clip has gone closes itself rather than editing a
    // ghost.
    for (AutomationEditorWindow* editor : m_automationEditors.values()) {
        editor->refresh();
    }
    if (m_patternWindow && m_patternWindow->isVisible()) {
        QTimer::singleShot(0, m_patternWindow,
                           [window = QPointer<PatternWindow>(m_patternWindow)] {
                               if (window) window->refresh();
                           });
    }
    // The controller emits nothing, so this is the only thing that tells the
    // context panel its values moved — without it a gain readout would still
    // show the pre-undo number.
    m_selection.refresh();
    syncAutomationVisibilityButton();
    layoutContextPanel();
}

namespace {

/// True when the projected change can add, remove, reorder or reparent a track
/// row. Only those need the header stack and the console rebuilt; everything
/// else — a rename, a fader move, a clip edit — is a value refresh on widgets
/// that already exist.
bool collaborationChangeMovesTrackRows(
    const daw::collab::ChangeImpact& impact) {
    if (impact.fullProjection) return true;
    for (const std::string& field : impact.fieldKeys) {
        if (!field.starts_with("track:")) continue;
        if (field.ends_with(":lifecycle") || field.ends_with(":position") ||
            field.ends_with(":parentId")) {
            return true;
        }
    }
    return false;
}

bool collaborationChangeTouchesClipContent(
    const daw::collab::ChangeImpact& impact) {
    return impact.fullProjection || !impact.clipIds.empty() ||
           !impact.noteIds.empty() || !impact.takeIds.empty() ||
           !impact.compSegmentIds.empty() ||
           !impact.automationPointIds.empty() ||
           !impact.controllerLaneIds.empty();
}

} // namespace

void MainWindow::refreshAfterCollaborationProjection(
    const daw::collab::ChangeImpact& impact) {
    // The adapter already committed the model/graph transaction.  This is a
    // visual pull only: calling markDirty() or routing through projectEdited()
    // here would turn a remote operation/ack/rebase into a local file edit and
    // can also echo it back through a future command migration.
    //
    // Every remote operation used to land on syncViews(), which destroys and
    // re-creates every track row and every channel strip. A peer dragging a
    // clip emits a stream of operations, so that tore down O(tracks) widgets
    // dozens of times a second. The reducer already reports what it touched;
    // honour it instead.
    if (collaborationChangeMovesTrackRows(impact)) {
        syncViews();
    } else {
        if (m_trackList) m_trackList->syncTrackValues();
        if (m_mixer) m_mixer->syncFromModel();
        if (m_inspector) m_inspector->syncFromModel();
        if (m_timeline) m_timeline->update();
        if (collaborationChangeTouchesClipContent(impact)) {
            if (m_pianoRoll) m_pianoRoll->refresh();
            for (AutomationEditorWindow* editor : m_automationEditors.values())
                editor->refresh();
        }
        m_selection.refresh();
        layoutContextPanel();
    }
    if (m_transport) m_transport->refresh();
    if (m_contextPanel) m_contextPanel->refresh();
}

void MainWindow::syncStructureViews() {
    m_orphanEditorSweepPending = true;
    // This is the visual acknowledgement of a move/fold/drop. Rebuilding the
    // header stack is required because its widget order is the structure; the
    // timeline reads that same model at paint time and needs no reconstruction.
    if (m_trackList) {
        m_trackList->rebuild();
        m_trackList->setSelectedTrack(m_selectedTrackId);
    }
    if (m_timeline) {
        m_timeline->setSelectedTrack(m_selectedTrackId);
        m_timeline->setSelectedTracks(
            m_trackList ? m_trackList->selectedTrackIds() : QStringList{});
        m_timeline->clampVerticalScroll();
        m_timeline->update();
    }
    m_selection.refresh();
    syncAutomationVisibilityButton();
    layoutContextPanel();
    scheduleDeferredStructureRefresh();
}

void MainWindow::scheduleDeferredStructureRefresh() {
    if (m_structureRefreshPending) return;
    m_structureRefreshPending = true;
    QTimer::singleShot(16, this, [this] {
        if (!m_structureRefreshPending) return;
        m_structureRefreshPending = false;
        if (m_mixer) {
            m_mixer->rebuild();
            m_mixer->setSelectedTrack(m_selectedTrackId);
        }
        if (m_inspector) {
            m_inspector->rebuildForTrack(m_selectedTrackId);
        }
        if (m_pianoRoll) m_pianoRoll->refresh();
        if (m_patternWindow && m_patternWindow->isVisible())
            m_patternWindow->refresh();
        if (m_contextPanel) m_contextPanel->refresh();
    });
}

void MainWindow::markDirty() {
    m_dirty = true;
    ++m_projectRevision;
    // Only a flag: the journal timer decides when to pay for a document copy.
    m_journalStale = true;
    updateWindowTitle();
}

void MainWindow::setMixerVisible(bool visible) {
    if (m_mixerWindow) {           // detached: bring the window forward instead
        if (visible) presentInternalWindow(m_mixerWindow);
        else hideInternalWindow(m_mixerWindow);
    } else {
        m_mixer->setVisible(visible);
        if (m_mixerHandle) m_mixerHandle->setVisible(visible);
        // Hiding an overlay does not resize its host, so no resize callback will
        // clear the timeline's old covered area unless we relayout explicitly.
        layoutMixer();
    }

    if (m_transport) m_transport->setMixerVisible(visible);
    if (m_showMixerAction && m_showMixerAction->isChecked() != visible) {
        // setChecked emits toggled synchronously. Without the blocker this
        // method re-entered and laid out the complete overlay twice.
        const QSignalBlocker block(m_showMixerAction);
        m_showMixerAction->setChecked(visible);
    }
}

void MainWindow::relayoutRow() {
    if (!m_rowLayout) return;
    const bool onLeft = m_browserOnLeft;
    // Taken out and put back rather than reparented: a QHBoxLayout has no
    // "move this widget" and the four columns are the whole row.
    for (QWidget* w : {static_cast<QWidget*>(m_browser),
                       static_cast<QWidget*>(m_inspector), m_arrangementHost}) {
        if (w) m_rowLayout->removeWidget(w);
    }
    if (onLeft) {
        m_rowLayout->addWidget(m_browser);
        m_rowLayout->addWidget(m_inspector);
        m_rowLayout->addWidget(m_arrangementHost, 1);
    } else {
        // The inspector stays next to the arrangement in both arrangements: it
        // is the selected track's channel strip and belongs beside its lanes.
        m_rowLayout->addWidget(m_inspector);
        m_rowLayout->addWidget(m_arrangementHost, 1);
        m_rowLayout->addWidget(m_browser);
    }
    if (auto* handle = qobject_cast<ui::ResizeHandle*>(m_browserHandle))
        handle->setEdge(onLeft ? Qt::RightEdge : Qt::LeftEdge);
    if (m_browser) m_browser->setOnLeft(onLeft);
    if (m_toolPanel) m_toolPanel->setBrowserOnLeft(onLeft);
    // The mixer overlay is positioned by hand inside the arrangement host, and
    // a side switch can change that host's width without resizing it.
    layoutMixer();
    if (m_pianoRollFrame && m_pianoRollFrame->isVisible())
        m_pianoRollFrame->raise();
    for (InternalEditorFrame* frame : m_internalEditorFrames) {
        if (frame && frame->isVisible() && frame->isEditorActive()) frame->raise();
    }
}

void MainWindow::setBrowserOnLeft(bool onLeft, bool persist) {
    if (m_browserOnLeft == onLeft) return;
    m_browserOnLeft = onLeft;
    if (persist) ui::browserprefs::setOnLeft(onLeft);
    relayoutRow();
}

void MainWindow::setBrowserVisible(bool visible) {
    if (!m_browser) return;
    m_browser->setVisible(visible);
    if (m_browserHandle) m_browserHandle->setVisible(visible);
    ui::browserprefs::setVisible(visible);
    if (m_showBrowserAction && m_showBrowserAction->isChecked() != visible) {
        QSignalBlocker block(m_showBrowserAction);
        m_showBrowserAction->setChecked(visible);
    }
    if (m_toolPanel) m_toolPanel->setBrowserVisible(visible);
    if (m_transport) m_transport->setBrowserVisible(visible);
    applyRightPanelWidths();
    layoutMixer();
}

void MainWindow::ensureWebBrowser() {
    if (m_webPanel || !m_webContainer) return;
    m_webPanel = new WebBrowserPanel(m_webContainer);
    m_webContainer->layout()->addWidget(m_webPanel);
    connect(m_webPanel, &WebBrowserPanel::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 4000); });
    connect(m_webPanel, &WebBrowserPanel::settingsRequested, this,
            [this] { openSettings(SettingsWindow::kBrowserTab); });
    connect(m_webPanel, &WebBrowserPanel::audioDownloadReady, this,
            [this](const QString& path,
                   const audio::platform::AudioFileInfo& info) {
                m_downloadedAudio.enqueue(
                    {path, info.durationSeconds(), info.sampleRate,
                     int(info.channels)});
                showNextDownloadedAudioPrompt();
            });
}

void MainWindow::setWebVisible(bool visible, bool persist) {
    if (!m_webContainer) return;
    if (visible) ensureWebBrowser();
    m_webContainer->setVisible(visible);
    if (m_webHandle) m_webHandle->setVisible(visible);
    if (persist) ui::webprefs::setVisible(visible);
    if (m_showWebAction && m_showWebAction->isChecked() != visible) {
        QSignalBlocker block(m_showWebAction);
        m_showWebAction->setChecked(visible);
    }
    if (m_transport) m_transport->setWebVisible(visible);
    applyRightPanelWidths();
    layoutMixer();
}

void MainWindow::applyRightPanelWidths() {
    const bool webVisible = m_webContainer && !m_webContainer->isHidden();
    const bool aiVisible = m_aiPanel && !m_aiPanel->isHidden();

    // Right panels may consume the whole timeline, but never the track header
    // column or the fixed panels that precede it. The old hard-coded 480 px
    // reserve was smaller than Browser + Inspector + track headers, so a wide
    // Web/AI drag eventually forced QHBoxLayout to crop the tracks themselves.
    int workspaceReserve = m_trackList
                               ? std::max(ui::kMinTrackHeaderWidth,
                                          m_trackList->width())
                               : ui::kTrackHeaderWidth;
    if (m_inspector && !m_inspector->isHidden())
        workspaceReserve += m_inspector->width();
    if (m_browser && !m_browser->isHidden())
        workspaceReserve += m_browser->width();
    // The fixed arrangement columns and the persistent header readouts are two
    // independent constraints on the same workspace. Protect the larger one;
    // this is the value QHBoxLayout was previously guessing from stale child
    // size hints, which is why the last track pixels could still be cropped.
    if (m_transport)
        workspaceReserve = std::max(workspaceReserve,
                                    m_transport->minimumResponsiveWidth());
    if (m_workspace && m_workspace->minimumWidth() != workspaceReserve)
        m_workspace->setMinimumWidth(workspaceReserve);

    // Resize hit areas now live on top of panel edges and consume no layout
    // width, so the shell minimum is exactly its usable workspace.
    const int shellMinimum = workspaceReserve;
    if (minimumWidth() != shellMinimum) setMinimumWidth(shellMinimum);
    const int panelBudget = std::max(0, width() - workspaceReserve);

    int webEffective = webVisible ? std::clamp(m_webWidth,
                                               ui::webprefs::kMinWidth,
                                               ui::webprefs::kMaxWidth)
                                  : 0;
    int aiEffective = aiVisible ? std::clamp(m_aiWidth, 240, 720) : 0;
    int overflow = std::max(0, webEffective + aiEffective - panelBudget);

    // The broad browsing surface gives space back first; the assistant then
    // follows down to its established minimum. Preferred widths are untouched,
    // so enlarging the window restores what the user chose.
    if (webVisible && overflow > 0) {
        const int take = std::min(overflow,
                                  webEffective - ui::webprefs::kMinWidth);
        webEffective -= take;
        overflow -= take;
    }
    if (aiVisible && overflow > 0) {
        const int take = std::min(overflow, aiEffective - 240);
        aiEffective -= take;
        overflow -= take;
    }
    // A window can itself become narrower than both panels' normal minima.
    // Keeping those minima at that point would reintroduce the cropped-track
    // bug. Continue yielding Web first and then AI; their preferred widths stay
    // intact and return as soon as the window has room again.
    if (webVisible && overflow > 0) {
        const int take = std::min(overflow, webEffective);
        webEffective -= take;
        overflow -= take;
    }
    if (aiVisible && overflow > 0) {
        const int take = std::min(overflow, aiEffective);
        aiEffective -= take;
    }
    if (webVisible) m_webContainer->setFixedWidth(webEffective);
    if (aiVisible) m_aiPanel->setFixedWidth(aiEffective);

    if (m_timeline) m_timeline->setRightCornerRadius(0);
    if (m_shellLayout) m_shellLayout->setContentsMargins(0, 0, 0, 0);
}

void MainWindow::applyTrackHeaderWidth() {
    if (!m_trackList || !m_timeline || !m_arrangementHost) return;
    const int available = m_arrangementHost->width();
    const int maximum = available > 0
                            ? std::max(ui::kMinTrackHeaderWidth,
                                       available - ui::kMinTimelineWidth)
                            : m_trackHeaderWidth;
    const int effective = std::clamp(
        m_trackHeaderWidth, ui::kMinTrackHeaderWidth, maximum);
    if (m_trackList->width() != effective)
        m_trackList->setFixedWidth(effective);
    if (m_toolPanel) m_toolPanel->setTrackZoneWidth(effective);
}

void MainWindow::setAiVisible(bool visible, bool persist) {
    if (!m_aiPanel) return;
    m_aiPanel->setVisible(visible);
    if (m_aiHandle) m_aiHandle->setVisible(visible);
    if (persist) ui::aiprefs::setVisible(visible);
    if (m_showAiAction && m_showAiAction->isChecked() != visible) {
        QSignalBlocker block(m_showAiAction);
        m_showAiAction->setChecked(visible);
    }
    if (m_toolPanel) m_toolPanel->setAiVisible(visible);
    if (m_transport) m_transport->setAiVisible(visible);
    applyRightPanelWidths();
    layoutMixer();
}

void MainWindow::setInspectorVisible(bool visible) {
    m_inspector->setCollapsed(!visible);
}

void MainWindow::onDetachMixer() {
    if (m_mixerWindow) return;
    auto* window = new MixerWindow([this] { onDockMixer(); }, this);
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(6, 6, 6, 6);
    m_mixer->setParent(window);
    layout->addWidget(m_mixer);
    // QWidget::setParent hides the child even when it was visible while docked.
    m_mixer->show();
    m_mixerWindow = window;
    if (m_mixerHandle) m_mixerHandle->hide();
    // The mixer no longer covers the arrangement. layoutMixer intentionally
    // ignores detached mixers, so remove its stale paint/scroll inset here.
    if (m_timeline) m_timeline->setBottomInset(0);
    m_transport->setMixerDetached(true);
    m_transport->setMixerVisible(true);
    if (m_showMixerAction && !m_showMixerAction->isChecked()) {
        const QSignalBlocker block(m_showMixerAction);
        m_showMixerAction->setChecked(true);
    }
    hostInternalWindow(window, QStringLiteral("internalEditors/mixer"));
    presentInternalWindow(window);
}

void MainWindow::onDockMixer() {
    if (!m_mixerWindow) return;
    QWidget* detached = m_mixerWindow;
    hideInternalWindow(detached);
    m_mixer->setParent(m_arrangementHost);
    detached->deleteLater();
    m_mixerWindow = nullptr;
    if (m_mixerHandle) m_mixerHandle->show();
    m_mixer->show();
    // Reparenting hides the widget. Show it before computing whether it covers
    // the timeline, then restore its geometry and viewport inset together.
    layoutMixer();
    m_transport->setMixerDetached(false);
    m_transport->setMixerVisible(true);
    if (m_showMixerAction && !m_showMixerAction->isChecked()) {
        const QSignalBlocker block(m_showMixerAction);
        m_showMixerAction->setChecked(true);
    }
}

void MainWindow::layoutContextPanel() {
    if (m_contextPanel) m_contextPanel->relayout();
    if (m_noteContextPanel) m_noteContextPanel->relayout();
    if (m_noteContextPanel && m_noteContextPanel->isVisible()) {
        m_noteContextPanel->raise();
    } else if (m_contextPanel) {
        m_contextPanel->raise();
    }
}

void MainWindow::syncPianoRollContextPanel() {
    if (!m_contextPanel) return;
    const bool editorActive = m_pianoRoll && m_pianoRollFrame &&
                              m_pianoRollFrame->isVisible() &&
                              m_pianoRollFrame->isEditorActive();
    const bool notesSelected = editorActive && m_pianoRoll->hasSelectedNotes();
    const bool panelEnabled = m_contextPanel->isPanelEnabled();

    // A selected note owns the same strip a selected clip normally uses. With
    // no note selection the underlying arrangement context remains useful.
    m_contextPanel->setSuppressed(notesSelected);
    if (!m_noteContextPanel) {
        layoutContextPanel();
        return;
    }

    m_noteContextPanel->setPanelEnabled(panelEnabled && editorActive);
    m_noteContextPanel->refresh();
    if (!notesSelected || !panelEnabled) {
        // Do not leave the outgoing note island overlapping the arrangement's
        // incoming one while its normal exit animation finishes.
        m_noteContextPanel->hide();
    } else {
        m_noteContextPanel->invalidateBackdrop();
        m_noteContextPanel->relayout();
        m_noteContextPanel->raise();
    }
    layoutContextPanel();
}

bool MainWindow::contextPanelAnchor(int& centreX) const {
    if (!m_timeline || !m_toolPanel) return false;

    // Only a clip selection has a horizontal extent. A whole track spans the
    // window and the recording options belong to no clip at all, so both leave
    // the plate where it has always been — in the middle.
    int left = 0, right = 0;
    if (!m_timeline->selectionSpanX(left, right)) return false;

    // The timeline and the tool strip are siblings, not ancestors, so the trip
    // goes through the screen. Only x matters: the plate rides along the strip.
    const QPoint global =
        m_timeline->mapToGlobal(QPoint((left + right) / 2, 0));
    centreX = m_toolPanel->mapFromGlobal(global).x();
    return true;
}

bool MainWindow::contextPanelBounds(int& left, int& right) const {
    if (!m_timeline || !m_toolPanel || !m_timeline->isVisible()) return false;
    // The arrangement's own edges, brought into the strip's coordinates the
    // same way the anchor is — the two are siblings, so the trip goes through
    // the screen.
    const QPoint topLeft = m_timeline->mapToGlobal(QPoint(0, 0));
    const QPoint topRight = m_timeline->mapToGlobal(QPoint(m_timeline->width(), 0));
    left = m_toolPanel->mapFromGlobal(topLeft).x();
    right = m_toolPanel->mapFromGlobal(topRight).x();
    return right > left;
}

void MainWindow::setContextPanelVisible(bool visible) {
    if (!m_contextPanel) return;
    m_contextPanel->setPanelEnabled(visible);
    QSettings().setValue("ui/contextPanelVisible", visible);
    if (m_showContextPanelAction) m_showContextPanelAction->setChecked(visible);
    syncPianoRollContextPanel();
}

void MainWindow::layoutMixer() {
    if (m_mixerWindow || !m_arrangementHost || !m_mixer) return;
    const int hostH = m_arrangementHost->height();
    if (hostH <= 0) return;
    const int minH = m_mixer->minimumHeight();
    // Preserve the user's requested body height, but position an oversized
    // mixer below a 60 px arrangement reveal instead of giving it a negative
    // y. The parent clips the surplus body; the actual covered pixels and the
    // timeline inset therefore remain identical even in a very short window.
    constexpr int kMinArrangementReveal = 60;
    const int bodyH = std::max(minH, m_mixerHeight);
    const int visibleBodyH = std::min(
        bodyH, std::max(0, hostH - kMinArrangementReveal));
    const int mixerY = std::max(0, hostH - visibleBodyH);
    const int w = m_arrangementHost->width();
    m_mixer->setGeometry(0, mixerY, w, bodyH);
    m_mixer->raise();
    // The mixer covers the bottom of the arrangement rather than shortening it,
    // so the lanes have to be told how much of themselves is hidden — otherwise
    // the last tracks can only be reached by collapsing the mixer.
    if (m_timeline) {
        // isVisible() also requires all ancestors to be on-screen. Explicitly
        // hidden is the stable state while constructing/restoring a workspace.
        const int covered = !m_mixer->isHidden()
                                ? std::min(hostH, visibleBodyH)
                                : 0;
        m_timeline->setBottomInset(covered);
    }
}

QAction* MainWindow::addCommand(QMenu* menu, const QString& id,
                                const QString& text, const QString& category,
                                const QKeySequence& def) {
    QAction* action = menu ? menu->addAction(text) : new QAction(text, this);
    // The registry label is the plain text without the '&' menu mnemonic.
    m_shortcuts->registerCommand(id, QString(text).remove('&'), category, def,
                                 action);
    return action;
}

void MainWindow::buildSemanticCommands() {
    using Risk = ShortcutManager::Risk;
    const QString kTransport = tr("Transport"), kEdit = tr("Edit"),
                  kView = tr("View"), kTools = tr("Tools"),
                  kBrowser = tr("Browser"), kTrack = tr("Track"),
                  kEditors = tr("Editors");

    // These actions do not need another menu. Adding them to the window gives
    // a user-assigned shortcut the same scope as the fixed toolbar control;
    // triggering them calls the control's existing state transition.
    auto bind = [this](const char* rawId, const QString& label,
                       const QString& category, const QString& description,
                       const QString& helpId, Risk risk, auto&& run) {
        const QString id = QString::fromLatin1(rawId);
        auto* action = new QAction(label, this);
        QWidget::addAction(action);
        ShortcutManager::Metadata metadata;
        metadata.description = description;
        metadata.helpId = helpId;
        metadata.risk = risk;
        metadata.modes = ShortcutManager::AllModes;
        m_shortcuts->registerCommand(id, label, category, {}, action,
                                     std::move(metadata));
        connect(action, &QAction::triggered, this,
                std::forward<decltype(run)>(run));
        return action;
    };

    bind("transport.rewindBar", tr("Rewind One Bar"), kTransport,
         tr("Move the playhead backward by one bar."),
         QStringLiteral("transport.navigation"), Risk::Safe,
         [this] { onNudge(-1); });
    bind("transport.forwardBar", tr("Forward One Bar"), kTransport,
         tr("Move the playhead forward by one bar."),
         QStringLiteral("transport.navigation"), Risk::Safe,
         [this] { onNudge(1); });
    bind("view.timeBars", tr("Show Position as Bars"), kView,
         tr("Display the transport and ruler position in bars and beats."),
         QStringLiteral("transport.time-display"), Risk::Safe,
         [this] { m_transport->setTimeDisplayBars(true); });
    bind("view.timeClock", tr("Show Position as Time"), kView,
         tr("Display the transport and ruler position as clock time."),
         QStringLiteral("transport.time-display"), Risk::Safe,
         [this] { m_transport->setTimeDisplayBars(false); });
    bind("edit.snapOn", tr("Enable Snap"), kEdit,
         tr("Make arrangement edits snap to the current grid."),
         QStringLiteral("arrangement.snap"), Risk::Safe,
         [this] { m_transport->setSnapEnabled(true); });
    bind("edit.snapOff", tr("Disable Snap"), kEdit,
         tr("Allow free arrangement edits between grid lines."),
         QStringLiteral("arrangement.snap"), Risk::Safe,
         [this] { m_transport->setSnapEnabled(false); });

    static constexpr std::array<const char*, 10> kGridCommandIds = {
        "edit.grid.off", "edit.grid.whole", "edit.grid.half",
        "edit.grid.quarter", "edit.grid.eighth", "edit.grid.sixteenth",
        "edit.grid.thirtySecond", "edit.grid.quarterTriplet",
        "edit.grid.eighthTriplet", "edit.grid.adaptive"};
    const auto& divisions = ui::gridDivisions();
    for (int i = 0; i < int(kGridCommandIds.size()) && i < divisions.size(); ++i) {
        const QString divisionName =
            divisions[i].beats < 0.0 ? tr("Adaptive") : divisions[i].name;
        const QString label = i == 0
            ? tr("Turn Grid Off")
            : tr("Set Grid to %1").arg(divisionName);
        bind(kGridCommandIds[std::size_t(i)], label, kEdit,
             i == 0
                 ? tr("Turn off the arrangement grid division.")
                 : tr("Use %1 as the arrangement grid division.")
                       .arg(divisionName),
             QStringLiteral("arrangement.grid"), Risk::Safe,
             [this, i] { m_transport->setGridIndex(i); });
    }

    static constexpr std::array<const char*, 6> kSecondaryToolIds = {
        "tool.secondarySelect", "tool.secondaryKnife",
        "tool.secondaryEraser", "tool.secondaryRegion",
        "tool.secondaryMute", "tool.secondaryDraw"};
    const QStringList secondaryTools = {
        tr("Pointer"), tr("Knife"), tr("Eraser"), tr("Region"),
        tr("Mute"), tr("Draw")};
    for (int i = 0; i < int(kSecondaryToolIds.size()); ++i) {
        bind(kSecondaryToolIds[std::size_t(i)],
             tr("Use %1 as Secondary Tool").arg(secondaryTools[i]), kTools,
             tr("Choose the %1 tool used while the secondary-tool modifier is held.")
                 .arg(secondaryTools[i]),
             QStringLiteral("arrangement.secondary-tool"), Risk::Safe,
             [this, i] { m_transport->setSecondaryToolIndex(i); });
    }

    bind("transport.playbackResume", tr("Resume Playback from Stop Position"),
         kTransport, tr("Make Play continue from the current stopped position."),
         QStringLiteral("transport.playback-start"), Risk::Safe,
         [this] { m_toolPanel->setRestartMode(false); });
    bind("transport.playbackRestart", tr("Restart Playback from Anchor"),
         kTransport, tr("Make Play restart from the anchored position."),
         QStringLiteral("transport.playback-start"), Risk::Safe,
         [this] { m_toolPanel->setRestartMode(true); });
    bind("transport.playFromClipOn", tr("Enable Play from Selected Clip"),
         kTransport, tr("Loop playback over the selected clip."),
         QStringLiteral("transport.play-from-clip"), Risk::Safe,
         [this] { m_toolPanel->setPlayFromClip(true); });
    bind("transport.playFromClipOff", tr("Disable Play from Selected Clip"),
         kTransport, tr("Stop constraining playback to the selected clip."),
         QStringLiteral("transport.play-from-clip"), Risk::Safe,
         [this] { m_toolPanel->setPlayFromClip(false); });

    bind("view.automation.showAll", tr("Show Automation for All Tracks"), kView,
         tr("Reveal automation lanes, creating a volume lane where needed."),
         QStringLiteral("automation.visibility"), Risk::Reversible,
         [this] { setAllAutomationLanesVisible(true); });
    bind("view.automation.hideAll", tr("Hide Automation for All Tracks"), kView,
         tr("Hide every track's automation lanes without deleting curves."),
         QStringLiteral("automation.visibility"), Risk::Reversible,
         [this] { setAllAutomationLanesVisible(false); });
    bind("tool.automationCreationOn", tr("Enable Automation Creation Mode"),
         kTools, tr("Latch the mode that creates automation from a control."),
         QStringLiteral("automation.creation-mode"), Risk::Safe,
         [this] {
             m_automationCreationLatched = true;
             updateAutomationCreationMode();
         });
    bind("tool.automationCreationOff", tr("Disable Automation Creation Mode"),
         kTools, tr("Release the latched automation-creation mode."),
         QStringLiteral("automation.creation-mode"), Risk::Safe,
         [this] {
             m_automationCreationLatched = false;
             updateAutomationCreationMode();
         });

    bind("track.clearMutes", tr("Unmute Every Track"), kTrack,
         tr("Clear the mute state from every track."),
         QStringLiteral("tracks.mute-solo"), Risk::Destructive,
         [this] { onClearMutes(); });

    bind("browser.addFolder", tr("Add Browser Folder…"), kBrowser,
         tr("Open the folder picker and grant the chosen folder to the browser."),
         QStringLiteral("browser.folders"), Risk::ExternalSideEffect,
         [this] { m_browser->requestAddFolder(); });
    bind("browser.refresh", tr("Refresh Browser Folders"), kBrowser,
         tr("Re-read the files in the browser's granted folders."),
         QStringLiteral("browser.folders"), Risk::Safe,
         [this] { m_browser->refreshFolders(); });
    bind("browser.settings", tr("Open Browser Settings"), kBrowser,
         tr("Open settings for browser folders, audition and placement."),
         QStringLiteral("browser.settings"), Risk::Safe,
         [this] { openSettings(SettingsWindow::kBrowserTab); });
    QAction* preview = bind(
        "browser.previewToggle", tr("Play or Stop Browser Selection"), kBrowser,
        tr("Audition the selected audio file, or stop its current audition."),
        QStringLiteral("browser.audition"), Risk::Safe,
        [this] { m_browser->togglePreview(); });
    preview->setEnabled(m_browser->hasPreviewableSelection());
    connect(m_browser, &FileBrowserPanel::previewAvailabilityChanged, preview,
            [preview](bool available) { preview->setEnabled(available); });
    bind("browser.previewLoopOn", tr("Loop Browser Preview"), kBrowser,
         tr("Repeat browser auditions until they are stopped."),
         QStringLiteral("browser.audition"), Risk::Safe,
         [this] { m_browser->setPreviewLoopEnabled(true); });
    bind("browser.previewLoopOff", tr("Play Browser Preview Once"), kBrowser,
         tr("Play browser auditions once instead of looping."),
         QStringLiteral("browser.audition"), Risk::Safe,
         [this] { m_browser->setPreviewLoopEnabled(false); });
    bind("browser.autoPreviewOn", tr("Enable Browser Auto-preview"), kBrowser,
         tr("Audition an audio file automatically when it is selected."),
         QStringLiteral("browser.audition"), Risk::Safe,
         [this] { m_browser->setAutoPreviewEnabled(true); });
    bind("browser.autoPreviewOff", tr("Disable Browser Auto-preview"), kBrowser,
         tr("Select browser files silently until Play is requested."),
         QStringLiteral("browser.audition"), Risk::Safe,
         [this] { m_browser->setAutoPreviewEnabled(false); });

    QAction* openEditor = bind(
        "editor.openSelected", tr("Open Editor for Selection"), kEditors,
        tr("Open the Piano Roll, Sample, Automation or Pattern editor for the current selection."),
        QStringLiteral("editors.open-selection"), Risk::Safe,
        [this] { openSelectedEditor(); });
    const auto updateEditorAvailability = [this, openEditor] {
        openEditor->setEnabled(canOpenSelectedEditor());
    };
    connect(&m_selection, &ui::SelectionModel::changed, openEditor,
            updateEditorAvailability);
    updateEditorAvailability();
}

void MainWindow::buildMenus() {
    const QString kFile = tr("File"), kEdit = tr("Edit"), kTrack = tr("Track"),
                  kTransport = tr("Transport"), kView = tr("View"),
                  kTools = tr("Tools"), kApp = tr("Application");

    auto* file = menuBar()->addMenu(tr("&File"));
    connect(addCommand(file, "file.new", tr("&New Project"), kFile, QKeySequence::New),
            &QAction::triggered, this, &MainWindow::onNewProject);
    connect(addCommand(file, "file.newFromTemplate",
                       tr("New Project from &Template…"), kFile),
            &QAction::triggered, this,
            &MainWindow::onNewProjectFromTemplate);
    connect(addCommand(file, "file.open", tr("&Open Project…"), kFile, QKeySequence::Open),
            &QAction::triggered, this, &MainWindow::onOpenProject);
    file->addSeparator();
    connect(addCommand(file, "file.save", tr("&Save"), kFile, QKeySequence::Save),
            &QAction::triggered, this, &MainWindow::onSaveProject);
    connect(addCommand(file, "file.saveAs", tr("Save &As…"), kFile, QKeySequence::SaveAs),
            &QAction::triggered, this, &MainWindow::onSaveProjectAs);
    connect(addCommand(file, "file.saveTemplate", tr("Save as Template…"), kFile),
            &QAction::triggered, this, &MainWindow::onSaveProjectTemplate);
    file->addSeparator();
    connect(addCommand(file, "file.import", tr("&Import Audio…"), kFile,
                       QKeySequence(tr("Ctrl+Shift+I"))),
            &QAction::triggered, this, &MainWindow::onImportAudio);
    connect(addCommand(file, "file.export", tr("&Render / Export…"), kFile,
                       QKeySequence(tr("Ctrl+E"))),
            &QAction::triggered, this, &MainWindow::onExport);
    file->addSeparator();
    connect(addCommand(file, "app.quit", tr("&Quit"), kApp, QKeySequence::Quit),
            &QAction::triggered, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    connect(addCommand(edit, "edit.undo", tr("&Undo"), kEdit, QKeySequence::Undo),
            &QAction::triggered, this, &MainWindow::onUndo);
    connect(addCommand(edit, "edit.redo", tr("&Redo"), kEdit, QKeySequence::Redo),
            &QAction::triggered, this, &MainWindow::onRedo);
    edit->addSeparator();
    connect(addCommand(edit, "edit.cutClips", tr("Cu&t Clips"), kEdit,
                       QKeySequence::Cut),
            &QAction::triggered, this, [this] {
                if (routeEditChord(EditChord::Cut)) return;
                if (m_timeline->cutSelection()) markDirty();
            });
    connect(addCommand(edit, "edit.copyClips", tr("&Copy Clips"), kEdit,
                       QKeySequence::Copy),
            &QAction::triggered, this, [this] {
                if (routeEditChord(EditChord::Copy)) return;
                m_timeline->copySelection();
            });
    connect(addCommand(edit, "edit.pasteClips", tr("&Paste Clips"), kEdit,
                       QKeySequence::Paste),
            &QAction::triggered, this, [this] {
                if (routeEditChord(EditChord::Paste)) return;
                if (m_timeline->pasteClipboard()) markDirty();
            });
    connect(addCommand(edit, "edit.repeatClips", tr("&Repeat Clips / Region"),
                       kEdit, QKeySequence(tr("Ctrl+B"))),
            &QAction::triggered, this, [this] {
                if (routeEditChord(EditChord::Repeat)) return;
                if (m_timeline->repeatSelection()) markDirty();
            });
    connect(addCommand(edit, "edit.muteClips", tr("Toggle Clip &Mute"), kEdit,
                       QKeySequence(tr("Ctrl+M"))),
            &QAction::triggered, this, [this] {
                if (routeEditChord(EditChord::Mute)) return;
                if (m_timeline->toggleSelectedClipsMuted()) markDirty();
             });
    edit->addSeparator();
    m_bounceInPlaceAction = addCommand(
        edit, "timeline.bounce_in_place", tr("Bounce in Place…"), kEdit,
        QKeySequence(tr("Ctrl+Alt+C")));
    connect(m_bounceInPlaceAction, &QAction::triggered, this,
            &MainWindow::onBounceInPlace);
    m_offlineRenderAction = addCommand(
        edit, "timeline.offline_render", tr("Offline Render…"), kEdit);
    connect(m_offlineRenderAction, &QAction::triggered, this,
            &MainWindow::onOfflineRender);
    edit->addSeparator();
    connect(addCommand(edit, "edit.toggleComp", tr("&Expand Take Layers"), kEdit,
                       QKeySequence(Qt::Key_E)),
            &QAction::triggered, this, [this] {
                if (!m_timeline->toggleSelectedClipExpanded()) {
                    statusBar()->showMessage(
                        tr("Select a clip with more than one take"), 1500);
                }
            });
    connect(addCommand(edit, "edit.duplicateTake", tr("&Duplicate Take"), kEdit),
            &QAction::triggered, this, [this] {
                if (m_timeline->duplicateActiveTake()) markDirty();
            });

    auto* track = menuBar()->addMenu(tr("&Track"));
    connect(addCommand(track, "track.addAudio", tr("Add &Audio Track"), kTrack,
                       QKeySequence(tr("Ctrl+Alt+A"))),
            &QAction::triggered, this, &MainWindow::onAddAudioTrack);
    connect(addCommand(track, "track.addMidi", tr("Add &MIDI Track"), kTrack,
                       QKeySequence(tr("Ctrl+Alt+M"))),
            &QAction::triggered, this, &MainWindow::onAddMidiTrack);
    connect(addCommand(track, "track.addPattern", tr("Add &Pattern Track"),
                       kTrack, QKeySequence(tr("Ctrl+Alt+P"))),
            &QAction::triggered, this, &MainWindow::onAddPatternTrack);
    connect(addCommand(track, "track.addBus", tr("Add &Bus Track"), kTrack),
            &QAction::triggered, this, &MainWindow::onAddBusTrack);
    connect(addCommand(track, "track.addSend", tr("Add &Send Track"), kTrack),
            &QAction::triggered, this, &MainWindow::onAddSendTrack);
    connect(addCommand(track, "track.duplicate", tr("&Duplicate"), kTrack,
                       QKeySequence(tr("Ctrl+D"))),
            &QAction::triggered, this, &MainWindow::onDuplicateSelectedTrack);
    QAction* removeAct = addCommand(track, "track.remove", tr("&Remove Selected"),
                                    kTrack, QKeySequence::Delete);
    // The physical "delete" key on Mac laptops is Backspace — bind both so
    // Delete removes selected clips (or the track) either way.
    removeAct->setShortcuts({QKeySequence(QKeySequence::Delete),
                             QKeySequence(Qt::Key_Backspace)});
    connect(removeAct, &QAction::triggered, this, [this] {
        if (routeEditChord(EditChord::Delete)) return;
        onRemoveSelectedTrack();
    });
    connect(addCommand(track, "track.findPlugin", tr("Find a &Plugin…"), kTrack,
                       QKeySequence::Find),
            &QAction::triggered, this, [this] {
                if (m_contextPanel) m_contextPanel->openPluginSearch();
            });
    track->addSeparator();
    // One item, one shortcut, one question: the dialog is where the two kinds
    // of folder are explained, and it makes a folder of the selection — or an
    // empty one when nothing is selected.
    connect(addCommand(track, "track.newFolder", tr("New &Folder…"), kTrack,
                       QKeySequence(tr("Ctrl+Shift+D"))),
            &QAction::triggered, this, &MainWindow::onNewFolder);
    connect(addCommand(track, "track.packPlain",
                       tr("Move Selected into a &Folder"), kTrack),
            &QAction::triggered, this,
            [this] { packSelectionIntoFolder(false); });
    connect(addCommand(track, "track.packSumming",
                       tr("Move Selected into a &Summing Folder"), kTrack),
            &QAction::triggered, this,
            [this] { packSelectionIntoFolder(true); });
    track->addSeparator();
    connect(addCommand(track, "track.automation", tr("Show &Automation"), kTrack,
                       QKeySequence(Qt::Key_A)),
            &QAction::triggered, this, &MainWindow::toggleAutomationLanes);
    track->addSeparator();
    connect(addCommand(track, "track.clearSolos", tr("Clear All Solos"), kTrack),
            &QAction::triggered, this, &MainWindow::onClearSolos);

    auto* transport = menuBar()->addMenu(tr("Trans&port"));
    connect(addCommand(transport, "transport.playPause", tr("Play / Pause"),
                       kTransport, QKeySequence(Qt::Key_Space)),
            &QAction::triggered, this, &MainWindow::onPlayPause);
    connect(addCommand(transport, "transport.stop", tr("Stop"), kTransport),
            &QAction::triggered, this, &MainWindow::onStop);
    connect(addCommand(transport, "transport.record", tr("Record"), kTransport,
                       QKeySequence(Qt::Key_R)),
            &QAction::triggered, this, &MainWindow::onRecordKey);
    connect(addCommand(transport, "transport.recordEngage",
                       tr("Record Enable"), kTransport),
            &QAction::triggered, this, &MainWindow::onRecord);
    connect(addCommand(transport, "transport.returnToStart", tr("Return to Start"),
                       kTransport, QKeySequence(Qt::Key_Return)),
            &QAction::triggered, this, &MainWindow::onReturnToStart);
    connect(addCommand(transport, "transport.cycle", tr("Cycle / Loop"), kTransport,
                       QKeySequence(Qt::Key_C)),
            &QAction::triggered, m_transport, &TransportBar::toggleCycle);
    connect(addCommand(transport, "transport.metronome", tr("Metronome"), kTransport,
                       QKeySequence(Qt::Key_K)),
            &QAction::triggered, m_transport, &TransportBar::toggleMetronome);
    connect(addCommand(transport, "transport.layerMode", tr("Layer Recording"),
                       kTransport, QKeySequence(tr("Shift+L"))),
            &QAction::triggered, this, &MainWindow::onToggleLayerMode);

    // The transport belongs to the project, not to a window: Space starts
    // playback while the piano roll, a plugin editor or the detached mixer has
    // focus, which is the whole point of having those open next to the
    // arrangement. Qt still keeps these out of the way where it must — a text
    // field claims the key first through ShortcutOverride, and a modal dialog
    // blocks application shortcuts outright — so the tempo box and the file
    // dialogs are unaffected.
    for (QAction* action : transport->actions())
        action->setShortcutContext(Qt::ApplicationShortcut);

#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration) {
        auto* session = menuBar()->addMenu(tr("&Session"));
        m_cloudProjectsAction = session->addAction(tr("Cloud Projects…"));
        connect(m_cloudProjectsAction, &QAction::triggered, this,
                &MainWindow::onOpenCloudProjects);
        m_publishProjectAction =
            session->addAction(tr("Publish Project to Cloud…"));
        m_publishProjectAction->setEnabled(false);
        m_publishProjectAction->setToolTip(
            tr("Save a local project, validate it, and publish an immutable "
               "cloud copy."));
        connect(m_publishProjectAction, &QAction::triggered, this,
                &MainWindow::onPublishCloudProject);
        m_startSessionAction =
            session->addAction(tr("Start Collaboration Session"));
        m_startSessionAction->setEnabled(false);
        connect(m_startSessionAction, &QAction::triggered, this, [this] {
            if (!m_cloudSessionLifecycle ||
                !m_cloudSessionLifecycle->startSession()) {
                statusBar()->showMessage(
                    tr("The cloud project is not ready to start a session."),
                    5000);
            }
        });
        m_invitePeopleAction = session->addAction(tr("Invite People…"));
        m_invitePeopleAction->setEnabled(false);
        connect(m_invitePeopleAction, &QAction::triggered, this,
                &MainWindow::onInvitePeople);
        m_sessionSettingsAction = session->addAction(tr("Session Settings…"));
        m_sessionSettingsAction->setEnabled(false);
        connect(m_sessionSettingsAction, &QAction::triggered, this,
                &MainWindow::onSessionSettings);
        m_endSessionAction = session->addAction(tr("End Session…"));
        m_endSessionAction->setEnabled(false);
        connect(m_endSessionAction, &QAction::triggered, this, [this] {
            if (!m_cloudSessionLifecycle ||
                !m_cloudSessionLifecycle->canEndSession())
                return;
            const auto answer = QMessageBox::question(
                this, tr("End Collaboration Session"),
                tr("End this session for everyone? VLT will keep the room "
                   "connected while the final verified snapshot is saved."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes &&
                !m_cloudSessionLifecycle->endSession()) {
                statusBar()->showMessage(
                    tr("The session can no longer be ended from this device."),
                    5000);
            }
        });
        QAction* makeLocalCopy = session->addAction(tr("Make Local Copy…"));
        connect(makeLocalCopy, &QAction::triggered, this,
                &MainWindow::onMakeLocalCopy);
        m_leaveSessionAction = session->addAction(tr("Leave Session…"));
        m_leaveSessionAction->setEnabled(false);
        connect(m_leaveSessionAction, &QAction::triggered, this, [this] {
            if (!m_cloudSessionLifecycle ||
                !m_cloudSessionLifecycle->canLeaveSession())
                return;
            const auto answer = QMessageBox::question(
                this, tr("Leave Collaboration Session"),
                tr("Leave this session? Other participants can continue "
                   "working in the room."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes &&
                !m_cloudSessionLifecycle->leaveSession()) {
                statusBar()->showMessage(
                    tr("The session can no longer be left from this device."),
                    5000);
            }
        });
        session->addSeparator();
        m_retryAssetMutationAction =
            session->addAction(tr("Retry Asset Upload"));
        m_retryAssetMutationAction->setEnabled(false);
        connect(m_retryAssetMutationAction, &QAction::triggered, this,
                [this] {
                    if (!m_cloudSharedAssetMutationBridge ||
                        !m_cloudSharedAssetMutationBridge->retry()) {
                        statusBar()->showMessage(
                            tr("There is no retryable shared asset upload."),
                            5000);
                    }
                });
        m_cancelAssetMutationAction =
            session->addAction(tr("Cancel Asset Upload"));
        m_cancelAssetMutationAction->setEnabled(false);
        connect(m_cancelAssetMutationAction, &QAction::triggered, this,
                [this] {
                    if (m_cloudSharedAssetMutationBridge)
                        m_cloudSharedAssetMutationBridge->cancel();
                });
        session->addSeparator();
        auto* independent = session->addAction(tr("Independent Transport"));
        auto* followHost = session->addAction(tr("Follow Session Host"));
        independent->setCheckable(true);
        followHost->setCheckable(true);
        auto* transportModeGroup = new QActionGroup(session);
        transportModeGroup->setExclusive(true);
        transportModeGroup->addAction(independent);
        transportModeGroup->addAction(followHost);
        independent->setChecked(true);
        connect(independent, &QAction::triggered, this, [this] {
            m_collaboration->localSessionState()->useIndependentTransport();
        });
        connect(followHost, &QAction::triggered, this, [this] {
            m_collaboration->localSessionState()->followHost();
        });
        connect(m_collaboration->localSessionState(),
                &collab::LocalSessionState::transportModeChanged, this,
                [independent, followHost](collab::TransportMode mode,
                                          const QString&) {
                    independent->setChecked(
                        mode == collab::TransportMode::Independent);
                    followHost->setChecked(
                        mode == collab::TransportMode::FollowHost);
                });

        auto* followParticipant = session->addMenu(tr("Follow Participant"));
        connect(followParticipant, &QMenu::aboutToShow, this,
                [this, followParticipant] {
                    followParticipant->clear();
                    const auto participants =
                        m_collaboration->presenceStore()->participants();
                    const QString localId =
                        m_collaboration->presenceStore()->localParticipantId();
                    for (const collab::ParticipantIdentity& participant :
                         participants) {
                        if (participant.participantId == localId) continue;
                        QAction* action = followParticipant->addAction(
                            collab::safeDisplayName(participant.nickname));
                        connect(action, &QAction::triggered, this,
                                [this, id = participant.participantId] {
                                    m_collaboration->localSessionState()
                                        ->followParticipant(id);
                                });
                    }
                    if (followParticipant->actions().isEmpty()) {
                        QAction* empty = followParticipant->addAction(
                            tr("No other participants"));
                        empty->setEnabled(false);
                    }
                });

        session->addSeparator();
        auto* reconnect = session->addAction(tr("Reconnect Session"));
        auto* disconnectSession = session->addAction(tr("Disconnect Session"));
        connect(reconnect, &QAction::triggered, m_collaboration,
                &collab::CollaborationService::reconnectNow);
        connect(disconnectSession, &QAction::triggered, m_collaboration,
                &collab::CollaborationService::disconnectFromProject);
        auto* status = session->addAction(QString());
        status->setEnabled(false);
        const auto updateSessionMenu =
            [this, followHost, reconnect, disconnectSession, status] {
                const auto state = m_collaboration->state();
                followHost->setEnabled(
                    !m_collaboration->localSessionState()
                         ->hostParticipantId().isEmpty());
                const bool knownRoom =
                    m_cloudSessionLifecycle &&
                    !m_cloudSessionLifecycle->sessionId().isEmpty();
                reconnect->setEnabled(
                    !m_collaboration->projectId().isEmpty() &&
                    (knownRoom ||
                     state != collab::CollaborationState::LocalOnly));
                disconnectSession->setEnabled(
                    state == collab::CollaborationState::Synced ||
                    state == collab::CollaborationState::ReadOnly ||
                    state == collab::CollaborationState::Joining ||
                    state == collab::CollaborationState::Connecting ||
                    state == collab::CollaborationState::Reconnecting);
                status->setText(tr("Status: %1")
                                    .arg(m_collaboration->stateDetail().isEmpty()
                                             ? collab::collaborationStateName(state)
                                             : m_collaboration->stateDetail()));
            };
        connect(session, &QMenu::aboutToShow, this, updateSessionMenu);
        connect(m_collaboration, &collab::CollaborationService::stateChanged,
                this, [updateSessionMenu](collab::CollaborationState,
                                          const QString&) {
                    updateSessionMenu();
                });
        updateSessionMenu();
        updateCloudSessionActions();
    }
#endif

    auto* view = menuBar()->addMenu(tr("&View"));
    m_showMixerAction = addCommand(view, "view.toggleMixer", tr("Show &Mixer"), kView,
                                   QKeySequence(Qt::Key_X));
    m_showMixerAction->setCheckable(true);
    m_showMixerAction->setChecked(true);
    connect(m_showMixerAction, &QAction::toggled, this, &MainWindow::setMixerVisible);

    m_showInspectorAction = addCommand(view, "view.toggleInspector",
                                       tr("Show &Inspector"), kView,
                                       QKeySequence(Qt::Key_I));
    m_showInspectorAction->setCheckable(true);
    m_showInspectorAction->setChecked(true);
    connect(m_showInspectorAction, &QAction::toggled, this,
            &MainWindow::setInspectorVisible);

    m_showContextPanelAction = addCommand(view, "view.toggleContextPanel",
                                          tr("Show &Context Panel"), kView,
                                          QKeySequence(tr("Alt+C")));
    m_showContextPanelAction->setCheckable(true);
    const bool contextPanelOn =
        QSettings().value("ui/contextPanelVisible", true).toBool();
    m_showContextPanelAction->setChecked(contextPanelOn);
    if (m_contextPanel) m_contextPanel->setPanelEnabled(contextPanelOn);
    connect(m_showContextPanelAction, &QAction::toggled, this,
            &MainWindow::setContextPanelVisible);

    m_showBrowserAction = addCommand(view, "view.toggleBrowser",
                                     tr("Show &Browser"), kView,
                                     QKeySequence(Qt::Key_B));
    m_showBrowserAction->setCheckable(true);
    m_showBrowserAction->setChecked(ui::browserprefs::visible());
    connect(m_showBrowserAction, &QAction::toggled, this,
            &MainWindow::setBrowserVisible);

    m_showWebAction = addCommand(view, "view.toggleWeb", tr("Show &Web Browser"),
                                 kView, QKeySequence(tr("Alt+W")));
    m_showWebAction->setCheckable(true);
    m_showWebAction->setChecked(ui::webprefs::visible());
    connect(m_showWebAction, &QAction::toggled, this,
            [this](bool on) { setWebVisible(on); });

    // Alt+A rather than a bare letter: bare letters are parked while the typing
    // keyboard has the keys, and a panel toggle should not come and go with it.
    m_showAiAction = addCommand(view, "view.toggleAi", tr("Show &AI Assistant"),
                                kView, QKeySequence(tr("Alt+A")));
    m_showAiAction->setCheckable(true);
    m_showAiAction->setChecked(ui::aiprefs::visible());
    connect(m_showAiAction, &QAction::toggled, this,
            [this](bool on) { setAiVisible(on); });

    view->addSeparator();
    m_followPlayheadAction = addCommand(
        view, "view.followPlayhead", tr("Follow &Playhead"), kView,
        QKeySequence(Qt::Key_P));
    m_followPlayheadAction->setCheckable(true);
    const bool followPlayhead =
        QSettings().value(QStringLiteral("timeline/followPlayhead"), false)
            .toBool();
    m_followPlayheadAction->setChecked(followPlayhead);
    m_timeline->setFollowPlayhead(followPlayhead);
    m_toolPanel->setFollowPlayhead(followPlayhead);
    connect(m_followPlayheadAction, &QAction::toggled, this,
            [this](bool on) {
                m_timeline->setFollowPlayhead(on);
                m_toolPanel->setFollowPlayhead(on);
                QSettings().setValue(
                    QStringLiteral("timeline/followPlayhead"), on);
            });
    connect(m_toolPanel, &ToolPanel::followPlayheadToggled,
            m_followPlayheadAction, &QAction::setChecked);

    view->addSeparator();
    // Zoom follows the focus. One pair of keys, and whichever panel the user
    // is working in is the one that grows — a second binding for the browser
    // would only be ambiguous with these.
    connect(addCommand(view, "view.zoomIn", tr("Zoom In"), kView,
                       QKeySequence(tr("Ctrl+="))),
            &QAction::triggered, this, [this] { zoomFocusedView(1.0); });
    connect(addCommand(view, "view.zoomOut", tr("Zoom Out"), kView,
                       QKeySequence(tr("Ctrl+-"))),
            &QAction::triggered, this, [this] { zoomFocusedView(-1.0); });
    connect(addCommand(view, "view.zoomFit", tr("Zoom to Fit"), kView,
                       QKeySequence(Qt::Key_Z)),
            &QAction::triggered, this, [this] { m_timeline->zoomToFit(); });
    view->addSeparator();
    connect(addCommand(view, "view.detachMixer", tr("Mixer in Separate &Window"),
                       kView),
            &QAction::triggered, this, &MainWindow::onDetachMixer);

    // Tool selection (also reachable from the transport chip).
    auto* tools = menuBar()->addMenu(tr("Too&ls"));
    connect(addCommand(tools, "tool.select", tr("Pointer Tool"), kTools,
                       QKeySequence(Qt::Key_1)),
            &QAction::triggered, this, [this] { setEditTool(0); });
    connect(addCommand(tools, "tool.knife", tr("Knife Tool"), kTools,
                       QKeySequence(Qt::Key_2)),
            &QAction::triggered, this, [this] { setEditTool(1); });
    connect(addCommand(tools, "tool.eraser", tr("Eraser Tool"), kTools,
                       QKeySequence(Qt::Key_3)),
            &QAction::triggered, this, [this] { setEditTool(2); });
    connect(addCommand(tools, "tool.region", tr("Region Tool"), kTools,
                       QKeySequence(Qt::Key_4)),
            &QAction::triggered, this, [this] { setEditTool(3); });
    connect(addCommand(tools, "tool.mute", tr("Mute Tool"), kTools,
                       QKeySequence(Qt::Key_5)),
            &QAction::triggered, this, [this] { setEditTool(4); });
    connect(addCommand(tools, "tool.draw", tr("Draw Tool"), kTools,
                       QKeySequence(Qt::Key_6)),
            &QAction::triggered, this, [this] { setEditTool(5); });
    connect(addCommand(tools, "tool.stretch", tr("Stretch Tool"), kTools,
                       QKeySequence(Qt::Key_7)),
            &QAction::triggered, this, [this] { setEditTool(6); });
    tools->addSeparator();
    m_typingKeyboardAction = addCommand(tools, "tool.typingKeyboard",
                                        tr("Typing &Keyboard"), kTools,
                                        QKeySequence(tr("Ctrl+T")));
    m_typingKeyboardAction->setCheckable(true);
    connect(m_typingKeyboardAction, &QAction::toggled, this,
            &MainWindow::setTypingKeyboardEnabled);

    auto* settings = menuBar()->addMenu(tr("&Settings"));
    connect(addCommand(settings, "app.settings", tr("&Settings…"), kApp,
                       QKeySequence(QKeySequence::Preferences)),
            &QAction::triggered, this, [this] { openSettings(SettingsWindow::kAudioTab); });
    connect(addCommand(settings, "app.plugins", tr("&Plugin Manager…"), kApp),
            &QAction::triggered, this, &MainWindow::openPluginManager);
}

void MainWindow::openPluginManager(int tab) {
    if (!m_pluginManagerWindow) {
        m_pluginManagerWindow = new PluginManagerWindow(&m_controller, this);
        // The browser lists the scanned plugins too; a rescan has to reach it.
        connect(m_pluginManagerWindow, &PluginManagerWindow::pluginsChanged,
                this, [this] {
                    if (m_browser) m_browser->reloadPlugins();
                });
        hostInternalWindow(m_pluginManagerWindow,
                           QStringLiteral("internalEditors/pluginManager"));
    }
    m_pluginManagerWindow->showTab(tab);
    presentInternalWindow(m_pluginManagerWindow);
}

void MainWindow::applyStartupPluginScanResults() {
    m_controller.pluginManager().takeScanFinished();
    if (m_browser) m_browser->reloadPlugins();
}

void MainWindow::retirePluginEditor(const QString& channelId,
                                    const QString& insertId) {
    // The controller is about to destroy this slot's plugin and is telling us
    // while it is still alive. The window has the plugin's own view inside it,
    // so it must let go here — the periodic orphan sweep below runs up to a
    // tenth of a second later, and by then the plugin would be gone.
    PluginEditorWindow* editor =
        m_pluginEditors.value(channelId + '/' + insertId, nullptr);
    if (!editor) return;
    editor->detachFromPlugin();
    editor->close();
}

void MainWindow::closeOrphanedPluginEditors() {
    if (m_pluginEditors.isEmpty() && m_sampleEditors.isEmpty()) return;
    // A slot can go away under an open editor — removed, or the whole project
    // closed. The plugin drops its view when the instance dies, leaving an
    // empty window behind, so the window has to go with it. Callers schedule
    // this after structural edits/undo/load; ordinary playback frames never
    // rescan every clip and insert merely as a safety net.
    std::vector<PluginEditorWindow*> orphaned;
    for (PluginEditorWindow* editor : m_pluginEditors) {
        const std::string channel = editor->channelId().toStdString();
        const std::string wanted = editor->insertId().toStdString();
        const std::string wantedUid = editor->pluginUid().toStdString();
        bool alive = false;
        if (const std::vector<daw::InsertModel>* inserts =
                m_controller.channelInserts(channel)) {
            for (const daw::InsertModel& model : *inserts) {
                if (model.id == wanted && model.uid == wantedUid) alive = true;
            }
        }
        // The instrument is a slot too, and it does not live in the insert
        // list — an editor open on it would otherwise be closed on the next
        // frame, which looks exactly like the window refusing to open.
        if (const daw::TrackModel* track = m_controller.project().findTrack(channel)) {
            if (track->instrument.isLoaded() && track->instrument.id == wanted &&
                track->instrument.uid == wantedUid) {
                alive = true;
            }
            for (const daw::InsertModel& model : track->samplerFx.inserts) {
                if (model.id == wanted && model.uid == wantedUid) alive = true;
            }
            for (const daw::ClipModel& clip : track->clips) {
                for (const daw::InsertModel& model : clip.inserts) {
                    if (model.id == wanted && model.uid == wantedUid) alive = true;
                }
            }
        }
        if (!alive) orphaned.push_back(editor);
    }
    // Closing mutates the registry through the `closing` signal, so it happens
    // outside the loop over it.
    for (PluginEditorWindow* editor : orphaned) editor->close();

    std::vector<SampleEditorWindow*> orphanedSamples;
    for (SampleEditorWindow* editor : m_sampleEditors) {
        if (!m_controller.audioClip(editor->trackId().toStdString(),
                                    editor->clipId().toStdString()))
            orphanedSamples.push_back(editor);
    }
    for (SampleEditorWindow* editor : orphanedSamples) editor->close();
}

void MainWindow::openPluginEditor(const QString& channelId, const QString& insertId) {
    const QString key = channelId + '/' + insertId;
    if (PluginEditorWindow* open = m_pluginEditors.value(key, nullptr)) {
        open->prepareForPresentation();
        presentInternalWindow(open);
        return;
    }

    auto* editor = new PluginEditorWindow(&m_controller, channelId, insertId, this);
    m_pluginEditors.insert(key, editor);
    hostInternalWindow(editor, QStringLiteral("internalEditors/plugins/") + key);
    // Reparenting is complete now and the frame is still hidden. Make this
    // exact hierarchy native before show(), so Qt never has to replace a
    // handle already handed to CLAP/VST3/AU.
    editor->prepareNativeHostHierarchy();
    // The window deletes itself on close (WA_DeleteOnClose), so the registry
    // has to drop the key or the next open would raise a dangling pointer.
    connect(editor, &PluginEditorWindow::closing, this,
            [this](const QString& channel, const QString& insert) {
                m_pluginEditors.remove(channel + '/' + insert);
                // A plugin may change opaque preset/MIDI-learn state without a
                // parameter callback. Capture once after its GUI closes even if
                // it did not report an ordinary document edit.
                m_journalStale = true;
            });
    connect(editor, &PluginEditorWindow::nestedPluginEditorRequested, this,
            &MainWindow::openPluginEditor);
    connect(editor, &PluginEditorWindow::projectEdited, this, [this] {
        markDirty();
        if (m_timeline) m_timeline->update();
    });
#ifdef DAW_ENABLE_COLLABORATION
    connect(editor, &PluginEditorWindow::builtInPanelReady, this,
            [this, channelId, insertId](QWidget* panel,
                                        const QString& stableTypeId) {
                registerNormalizedPresenceSurface(
                    panel, collab::SurfaceKind::BuiltinPlugin, stableTypeId,
                    channelId, insertId);
            });
#endif
    connect(editor, &PluginEditorWindow::automationRequested, this,
            [this](const QString& channel, const QString& insert,
                   const QString& parameter) {
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::PluginParameter;
                target.channelId = channel.toStdString();
                // The instrument slot is spelled as an *empty* slot id — the
                // convention `ControllerLane` set and every automation path
                // here follows.
                const daw::TrackModel* track =
                    m_controller.project().findTrack(channel.toStdString());
                const bool isInstrument =
                    track && track->instrument.id == insert.toStdString();
                target.slotId = isInstrument ? std::string() : insert.toStdString();
                target.parameterId = parameter.toStdString();
                automateTarget(target);
    });
    // Do not create the first native frame from inside the plugin picker's
    // mousePressEvent. Its overlay and global event filter finish their own
    // signal turn first; then the complete native frame is shown and mapped.
    const QPointer<PluginEditorWindow> guardedEditor(editor);
    QTimer::singleShot(0, this, [this, guardedEditor] {
        if (!guardedEditor) return;
        presentInternalWindow(guardedEditor);
        // A second queued boundary lets Cocoa/Win32 finish mapping the freshly
        // shown native ancestor chain before editor initialization begins.
        QTimer::singleShot(0, guardedEditor,
                           &PluginEditorWindow::initializeEditor);
    });
}

void MainWindow::openAutomationEditor(const QString& trackId,
                                     const QString& clipId) {
    const QString key = trackId + '/' + clipId;
    if (AutomationEditorWindow* open = m_automationEditors.value(key, nullptr)) {
        presentInternalWindow(open);
        return;
    }
    auto* editor = new AutomationEditorWindow(&m_controller, trackId, clipId, this);
    m_automationEditors.insert(key, editor);
    hostInternalWindow(editor,
                       QStringLiteral("internalEditors/automation/") + key);
#ifdef DAW_ENABLE_COLLABORATION
    registerAutomationPresence(editor);
#endif
    connect(editor, &AutomationEditorWindow::closing, this,
            [this](const QString& track, const QString& clip) {
                m_automationEditors.remove(track + '/' + clip);
            });
    connect(editor, &AutomationEditorWindow::projectEdited, this, [this] {
        markDirty();
        syncViews();
    });
    // Mid-drag: the curve on the arrangement is already the new shape, so it is
    // repainted at once. A dirty flag and an undo entry belong to the finished
    // gesture, not to every mouse move inside it.
    connect(editor, &AutomationEditorWindow::liveEdited, this, [this] {
        if (m_timeline) m_timeline->update();
    });
    presentInternalWindow(editor);
}

void MainWindow::automateTarget(const daw::AutomationTarget& target) {
    if (target.channelId.empty()) return;
    const auto [lane, clip] = m_controller.ensureAutomation(target);
    if (lane.empty()) return;
    syncViews();
    markDirty();

    // Bring the lane into view and select the curve, so the answer to "where
    // did that go" is on screen rather than somewhere below the fold.
    const QString laneId = QString::fromStdString(lane);
    if (m_timeline) {
        const auto& rows = daw::visibleTracks(m_controller.project());
        for (int i = 0; i < int(rows.size()); ++i) {
            if (m_controller.project().tracks[rows[std::size_t(i)].index].id == lane) {
                m_timeline->ensureLaneVisible(i);
                break;
            }
        }
        m_timeline->selectClips({ui::ClipSel{laneId, QString::fromStdString(clip)}});
        m_timeline->update();
    }
    statusBar()->showMessage(
        tr("Automating %1")
            .arg(QString::fromStdString(m_controller.automationTargetName(target))),
        3000);
}

void MainWindow::openSampleEditor(const QString& trackId, const QString& clipId) {
    const QString key = trackId + '/' + clipId;
    if (SampleEditorWindow* open = m_sampleEditors.value(key, nullptr)) {
        presentInternalWindow(open);
        return;
    }
    const daw::ClipModel* clip =
        m_controller.audioClip(trackId.toStdString(), clipId.toStdString());
    if (!clip) return;

    // A layered clip has no audio of its own — its comp decides what plays, and
    // its `filePath` is empty — so the editor used to open on nothing at all.
    // The editor works on one piece of audio, so the way in is to make the clip
    // one: commit the comp, which is what the take stack's own menu offers, and
    // open on the result. Asked rather than done, because it dissolves the
    // takes; one undo puts them back.
    if (daw::isLayered(*clip)) {
        const int takes = int(clip->takes.size());
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Edit layered clip"));
        box.setText(tr("“%1” holds %2 takes.")
                        .arg(QString::fromStdString(clip->name))
                        .arg(takes));
        box.setInformativeText(
            tr("The Sample Editor works on a single piece of audio. Committing "
               "the comp bakes what you hear into the clip and removes the "
               "takes from the project — their files stay on disk, and one undo "
               "brings the takes back."));
        QPushButton* go = box.addButton(tr("Commit and Edit"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(go);
        box.exec();
        if (box.clickedButton() != go) return;

        m_controller.commitComp(trackId.toStdString(), clipId.toStdString());
        syncViews();
        markDirty();
        clip = m_controller.audioClip(trackId.toStdString(), clipId.toStdString());
        if (!clip || clip->filePath.empty()) {
            statusBar()->showMessage(
                tr("Nothing to edit — the comp produced no audio"), 4000);
            return;
        }
    }

    auto* editor = new SampleEditorWindow(&m_controller, trackId, clipId, this);
    m_sampleEditors.insert(key, editor);
    hostInternalWindow(editor, QStringLiteral("internalEditors/sample/") + key);
#ifdef DAW_ENABLE_COLLABORATION
    registerNormalizedPresenceSurface(
        editor->collaborationPresenceSurface(),
        collab::SurfaceKind::SampleEditor, QStringLiteral("clip-sampler"),
        trackId, clipId);
#endif
    connect(editor, &SampleEditorWindow::closing, this,
            [this](const QString& track, const QString& clip) {
                m_sampleEditors.remove(track + '/' + clip);
            });
    connect(editor, &SampleEditorWindow::projectEdited, this, [this] {
        markDirty();
        if (m_timeline) m_timeline->update();
        if (m_contextPanel) m_contextPanel->refresh();
    });
    // Mid-drag: the clip on the arrangement is already the new length, so it is
    // repainted at once. Nothing else — a dirty flag and an undo entry belong
    // to the finished gesture, not to every mouse move inside it.
    connect(editor, &SampleEditorWindow::liveEdited, this, [this] {
        if (m_timeline) m_timeline->update();
    });
    connect(editor, &SampleEditorWindow::pluginEditorRequested, this,
            &MainWindow::openPluginEditor);
    // Read through to the arrangement rather than copied: the snap can be
    // switched off, or the division and the tempo changed, while the editor
    // stands open, and the detent has to follow all three.
    editor->setSnapProvider([this] {
        return m_timeline ? m_timeline->snapSeconds() : 0.0;
    });
    presentInternalWindow(editor);
}

void MainWindow::openDemoPluginSearch(bool expand) {
    if (m_controller.project().tracks.empty() || !m_contextPanel) return;
    const QString trackId =
        QString::fromStdString(m_controller.project().tracks.front().id);
    selectTrackFromHeader(trackId);
    if (expand) m_contextPanel->openPluginSearch();
}

bool MainWindow::openDemoPluginEditor(const QString& nameFragment) {
    if (m_controller.project().tracks.empty()) return false;
    for (const daw::plugins::PluginDescriptor& descriptor :
         m_controller.pluginManager().plugins()) {
        const QString name = QString::fromStdString(descriptor.name);
        if (!name.contains(nameFragment, Qt::CaseInsensitive)) continue;

        if (descriptor.isInstrument) {
            for (const auto& track : m_controller.project().tracks) {
                if (!daw::trackAccepts(track.kind, daw::ClipKind::Midi)) continue;
                if (!m_controller.setTrackInstrumentPlugin(track.id, descriptor)) {
                    return false;
                }
                const auto* updated = m_controller.project().findTrack(track.id);
                if (!updated || !updated->instrument.isLoaded()) return false;
                onTracksChanged();
                openPluginEditor(QString::fromStdString(track.id),
                                 QString::fromStdString(updated->instrument.id));
                return true;
            }
            return false;
        }

        const std::string trackId = m_controller.project().tracks.front().id;
        const std::string insertId = m_controller.addInsert(trackId, descriptor);
        if (insertId.empty()) return false;
        // Same refresh the mixer triggers for itself when the user does this,
        // so the screenshot shows the filled slot rather than a stale strip.
        onTracksChanged();
        openPluginEditor(QString::fromStdString(trackId),
                         QString::fromStdString(insertId));
        return true;
    }
    return false;
}

bool MainWindow::probePluginEditorSwap(const QString& firstName,
                                       const QString& secondName) {
    auto findPlugin = [this](const QString& query)
        -> std::optional<daw::plugins::PluginDescriptor> {
        // "vst3:Pro-Q" pins a format; a bare name takes whichever comes first.
        QString fragment = query;
        daw::plugins::Format format = daw::plugins::Format::Unknown;
        const int colon = query.indexOf(':');
        if (colon > 0) {
            format = daw::plugins::formatFromString(query.left(colon).toStdString());
            fragment = query.mid(colon + 1);
        }
        for (const daw::plugins::PluginDescriptor& d :
             m_controller.pluginManager().plugins()) {
            if (format != daw::plugins::Format::Unknown && d.format != format) continue;
            if (QString::fromStdString(d.name).contains(fragment, Qt::CaseInsensitive))
                return d;
        }
        return std::nullopt;
    };

    const auto first = findPlugin(firstName);
    const auto second = findPlugin(secondName);
    if (!first || !second) {
        std::fprintf(stderr, "swap probe: no plugin matches '%s'\n",
                     (first ? secondName : firstName).toUtf8().constData());
        return false;
    }
    if (m_controller.project().tracks.empty()) {
        m_controller.addTrack(daw::TrackKind::Audio, "Probe");
        onTracksChanged();
    }
    const QString track =
        QString::fromStdString(m_controller.project().tracks.front().id);

    auto settle = [] {
        // Long enough for the refresh tick — the orphan sweep lives there — to
        // run several times.
        for (int i = 0; i < 40; ++i)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    auto describe = [this](const QString& channel, const QString& slot,
                           const char* stage) {
        PluginEditorWindow* window =
            m_pluginEditors.value(channel + '/' + slot, nullptr);
        daw::plugins::PluginInstance* live =
            m_controller.insertInstance(channel.toStdString(), slot.toStdString());
        std::fprintf(stderr,
                     "  %-14s window %-3s embedded %-3s instance %-3s (%s)\n",
                     stage, window ? "yes" : "no",
                     window && window->isEmbedded() ? "yes" : "no",
                     live ? "yes" : "NO",
                     live ? live->descriptor().name.c_str() : "-");
        return window;
    };

    std::fprintf(stderr, "swap probe: %s -> %s\n", first->name.c_str(),
                 second->name.c_str());
    const std::string slot = m_controller.addInsert(track.toStdString(), *first);
    if (slot.empty()) {
        std::fprintf(stderr, "  the first plugin did not load\n");
        return false;
    }
    onTracksChanged();
    const QString slotId = QString::fromStdString(slot);
    openPluginEditor(track, slotId);
    settle();
    const bool openedFirst = describe(track, slotId, "after open") != nullptr;

    // The swap, exactly as the strip's Replace button does it: the old plugin
    // is destroyed while its editor window is still on screen.
    m_controller.replaceInsert(track.toStdString(), slot, *second);
    onTracksChanged();
    settle();
    describe(track, slotId, "after replace");

    openPluginEditor(track, slotId);
    settle();
    PluginEditorWindow* reopened = describe(track, slotId, "after reopen");
    daw::plugins::PluginInstance* live =
        m_controller.insertInstance(track.toStdString(), slot);
    const bool ok = openedFirst && reopened && live &&
                    live->descriptor().uid == second->uid &&
                    (!live->hasEditor() || reopened->isEmbedded());
    std::fprintf(stderr, "  %s\n", ok ? "OK" : "BROKEN");
    return ok;
}

bool MainWindow::openDemoSampler(const QString& samplePath) {
    const auto sampler = m_controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.sampler");
    if (!sampler) return false;

    for (const auto& track : m_controller.project().tracks) {
        if (track.kind != daw::TrackKind::Instrument && track.kind != daw::TrackKind::Midi) {
            continue;
        }
        m_controller.setTrackInstrumentPlugin(track.id, *sampler);
        const daw::TrackModel* updated = m_controller.project().findTrack(track.id);
        if (!updated || !updated->instrument.isLoaded()) return false;
        if (!samplePath.isEmpty()) {
            m_controller.loadSamplerSample(track.id, updated->instrument.id,
                                           samplePath.toStdString());
        }
        // The slot screenshot flag also populates the nested rack once the
        // Sampler exists. This is the only reliable headless visual check for
        // the hover-only power / open / replace actions: loadDemoPluginSlots()
        // runs before this helper and its temporary instrument is replaced by
        // the Sampler above.
        if (qEnvironmentVariableIsSet("DAW_SHOT_SLOTS")) {
            const daw::plugins::PluginDescriptor* effect = nullptr;
            for (const auto& descriptor : m_controller.pluginManager().plugins()) {
                if (!descriptor.isInstrument) {
                    effect = &descriptor;
                    break;
                }
            }
            if (effect) {
                const std::string first = m_controller.addSamplerFxInsert(
                    track.id, updated->instrument.id, *effect, 0);
                const std::string second = m_controller.addSamplerFxInsert(
                    track.id, updated->instrument.id, *effect, 1);
                if (!second.empty())
                    m_controller.setInsertBypassed(track.id, second, true);
                (void)first;
            }
        }
        onTracksChanged();
        openPluginEditor(QString::fromStdString(track.id),
                         QString::fromStdString(updated->instrument.id));
        return true;
    }
    return false;
}

bool MainWindow::openDemoGravity() {
    const auto gravity = m_controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.gravity");
    if (!gravity) return false;

    for (const auto& track : m_controller.project().tracks) {
        if (track.kind != daw::TrackKind::Audio) continue;
        const std::string slot = m_controller.addInsert(track.id, *gravity, 0);
        if (slot.empty()) return false;
        if (qEnvironmentVariableIsSet("DAW_SHOT_GRAVITY_DUAL"))
            m_controller.setInsertParameter(track.id, slot, "pitch.spread", 7.0);
        onTracksChanged();
        openPluginEditor(QString::fromStdString(track.id),
                         QString::fromStdString(slot));
        if (qEnvironmentVariableIsSet("DAW_SHOT_GRAVITY")) {
            // The offscreen screenshot runner has no audio device callback.
            // Exercise the real DSP here so every dot in the captured field
            // still comes from the instance's grain-spawn telemetry.
            auto* instance = dynamic_cast<daw::plugins::gravity::GravityInstance*>(
                m_controller.insertInstance(track.id, slot));
            if (instance) {
                constexpr std::uint32_t frames = 256;
                std::array<float, frames> inLeft{};
                std::array<float, frames> inRight{};
                std::array<float, frames> outLeft{};
                std::array<float, frames> outRight{};
                const float* inputs[] = {inLeft.data(), inRight.data()};
                float* outputs[] = {outLeft.data(), outRight.data()};
                const bool preparedHere = !instance->isActive();
                if (preparedHere) instance->activate({48000.0, frames, true});
                instance->startProcessing();
                daw::plugins::PluginProcessContext context;
                context.inputs = inputs;
                context.inputChannels = 2;
                context.outputs = outputs;
                context.outputChannels = 2;
                context.frames = frames;
                context.transport.tempo = 120.0;
                for (int block = 0; block < 4200; ++block) {
                    for (std::uint32_t i = 0; i < frames; ++i) {
                        const float sample = ((block * int(frames) + int(i)) %
                                              2400 == 0) ? 0.42f : 0.0f;
                        inLeft[i] = sample;
                        inRight[i] = sample * 0.78f;
                    }
                    instance->process(context);
                }
                if (preparedHere) {
                    instance->stopProcessing();
                    instance->deactivate();
                }
            }
            m_controller.play();
        }
        return true;
    }
    return false;
}

bool MainWindow::openDemoGraphit() {
    const auto graphit = m_controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.graphit");
    if (!graphit) return false;

    for (const auto& track : m_controller.project().tracks) {
        if (track.kind != daw::TrackKind::Audio) continue;
        const std::string slot = m_controller.addInsert(track.id, *graphit, 0);
        if (slot.empty()) return false;
        if (qEnvironmentVariableIsSet("DAW_SHOT_GRAPHIT")) {
            m_controller.setInsertParameter(track.id, slot, "amount", 0.78);
            m_controller.setInsertParameter(track.id, slot, "mode", 2.0);
        }
        onTracksChanged();
        openPluginEditor(QString::fromStdString(track.id),
                         QString::fromStdString(slot));
        if (qEnvironmentVariableIsSet("DAW_SHOT_GRAPHIT")) {
            auto* instance = dynamic_cast<daw::plugins::graphit::GraphitInstance*>(
                m_controller.insertInstance(track.id, slot));
            if (instance) {
                constexpr std::uint32_t frames = 256;
                std::array<float, frames> inLeft{};
                std::array<float, frames> inRight{};
                std::array<float, frames> outLeft{};
                std::array<float, frames> outRight{};
                const float* inputs[] = {inLeft.data(), inRight.data()};
                float* outputs[] = {outLeft.data(), outRight.data()};
                const bool preparedHere = !instance->isActive();
                if (preparedHere) instance->activate({48000.0, frames, true});
                instance->startProcessing();
                daw::plugins::PluginProcessContext context;
                context.inputs = inputs;
                context.inputChannels = 2;
                context.outputs = outputs;
                context.outputChannels = 2;
                context.frames = frames;
                for (int block = 0; block < 320; ++block) {
                    for (std::uint32_t i = 0; i < frames; ++i) {
                        const double at = double(block * int(frames) + int(i));
                        const float pulse = (int(at) % 2400) < 32 ? 0.32f : 0.0f;
                        inLeft[i] = float(0.24 * std::sin(at * 0.031) +
                                          0.14 * std::sin(at * 0.117)) + pulse;
                        inRight[i] = float(0.22 * std::sin(at * 0.033) +
                                           0.12 * std::sin(at * 0.109)) + pulse;
                    }
                    instance->process(context);
                }
                if (preparedHere) {
                    instance->stopProcessing();
                    instance->deactivate();
                }
            }
        }
        return true;
    }
    return false;
}

bool MainWindow::checkGraphitPanelForTest() {
    if (!openDemoGraphit()) return false;
    QElapsedTimer wait;
    wait.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        for (PluginEditorWindow* editor : std::as_const(m_pluginEditors)) {
            if (GraphitPanel* panel = editor->findChild<GraphitPanel*>())
                return panel->checkForTest();
        }
        QThread::msleep(1);
    } while (wait.elapsed() < 1000);
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_VERBOSE"))
        std::fprintf(stderr, "Graphit UI selftest: editor did not become ready\n");
    return false;
}

void MainWindow::resizeGraphitForShot() {
    for (PluginEditorWindow* editor : std::as_const(m_pluginEditors)) {
        if (!editor || editor->pluginUid() != QStringLiteral("daw.graphit"))
            continue;
        if (InternalEditorFrame* frame =
                m_internalEditorFrames.value(editor, nullptr))
            frame->resizeForContent(QSize(440, 499));
    }
}

bool MainWindow::checkGravityPanelForTest() {
    if (!openDemoGravity()) return false;
    QElapsedTimer wait;
    wait.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        for (PluginEditorWindow* editor : std::as_const(m_pluginEditors)) {
            if (GravityPanel* panel = editor->findChild<GravityPanel*>())
                return panel->checkForTest();
        }
        QThread::msleep(1);
    } while (wait.elapsed() < 1000);
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_VERBOSE")) {
        std::fprintf(stderr,
                     "Gravity UI selftest: editor did not become ready\n");
    }
    return false;
}

void MainWindow::resizeGravityForShot() {
    for (PluginEditorWindow* editor : std::as_const(m_pluginEditors)) {
        if (!editor || editor->pluginUid() != QStringLiteral("daw.gravity"))
            continue;
        if (InternalEditorFrame* frame =
                m_internalEditorFrames.value(editor, nullptr))
            frame->resizeForContent(QSize(820, 638));
    }
}

bool MainWindow::openDemoEqualizer() {
    const auto equalizer = m_controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.equalizer");
    if (!equalizer) return false;
    for (const auto& track : m_controller.project().tracks) {
        if (track.kind != daw::TrackKind::Audio) continue;
        const std::string slot = m_controller.addInsert(track.id, *equalizer, 0);
        if (slot.empty()) return false;
        if (qEnvironmentVariableIsSet("DAW_SHOT_EQUALIZER")) {
            const auto presets = daw::plugins::equalizer::factoryPresets();
            const auto& preset = presets[2]; // Vocal Cleanup: a useful graph for docs.
            for (const daw::plugins::ParameterInfo& info :
                 daw::plugins::equalizer::parameterTable())
                m_controller.setInsertParameter(track.id, slot, info.id,
                                                preset.values[info.index]);
            if (auto* instance = dynamic_cast<
                    daw::plugins::equalizer::EqualizerInstance*>(
                    m_controller.insertInstance(track.id, slot)))
                instance->setPresetReference("factory", "Vocal Cleanup");
        }
        onTracksChanged();
        openPluginEditor(QString::fromStdString(track.id),
                         QString::fromStdString(slot));
        return true;
    }
    return false;
}

bool MainWindow::checkEqualizerPanelForTest() {
    if (!openDemoEqualizer()) return false;
    QElapsedTimer wait;
    wait.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        for (PluginEditorWindow* editor : std::as_const(m_pluginEditors)) {
            if (EqualizerPanel* panel = editor->findChild<EqualizerPanel*>())
                return panel->checkForTest();
        }
        QThread::msleep(1);
    } while (wait.elapsed() < 1000);
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_VERBOSE"))
        std::fprintf(stderr, "Equalizer UI selftest: editor did not become ready\n");
    return false;
}

void MainWindow::resizeEqualizerForShot() {
    for (PluginEditorWindow* editor : std::as_const(m_pluginEditors)) {
        if (!editor || editor->pluginUid() != QStringLiteral("daw.equalizer"))
            continue;
        if (InternalEditorFrame* frame = m_internalEditorFrames.value(editor, nullptr))
            frame->resizeForContent(QSize(820, 559));
    }
}

bool MainWindow::checkMidiInput() {
    if (!m_midiInput || !m_pianoRoll || !m_pianoRollFrame ||
        !m_pianoRollFrame->isEditorActive()) {
        return false;
    }

    const auto message = [](int status, int data1, int data2 = -1) {
        QByteArray bytes;
        bytes.append(char(status));
        bytes.append(char(data1));
        if (data2 >= 0) bytes.append(char(data2));
        return bytes;
    };
    constexpr int pitch = 64;
    constexpr quint64 sourceA = 0xFFFFFF01u;
    constexpr quint64 sourceB = 0xFFFFFF02u;

    const bool first = m_midiInput->injectMessageForTest(
        message(0x93, pitch, 117), sourceA);
    const bool lit = m_pianoRoll->livePitchHeldForTest(pitch);
    const bool second = m_midiInput->injectMessageForTest(
        message(0x93, pitch, 91), sourceB);
    const bool firstOff = m_midiInput->injectMessageForTest(
        message(0x83, pitch, 0), sourceA);
    const bool overlapStayedLit = m_pianoRoll->livePitchHeldForTest(pitch);
    const bool secondOff = m_midiInput->injectMessageForTest(
        message(0x93, pitch, 0), sourceB);
    const bool cleared = !m_pianoRoll->livePitchHeldForTest(pitch);
    const bool filtered =
        !m_midiInput->injectMessageForTest(message(0x90, pitch), sourceA) &&
        !m_midiInput->injectMessageForTest(QByteArray(1, char(0xF8)), sourceA);

    // A held note belongs only to the track that accepted it. Switching the
    // open roll releases that track and must not paint the same pitch on the
    // newly opened one.
    const QString originalTrack = m_pianoRoll->trackId();
    QString originalClip;
    QString otherTrack;
    QString otherClip;
    for (const daw::TrackModel& track : m_controller.project().tracks) {
        const QString id = QString::fromStdString(track.id);
        if (!daw::trackAccepts(track.kind, daw::ClipKind::Midi)) continue;
        QString midiClip;
        for (const daw::ClipModel& clip : track.clips) {
            if (clip.kind == daw::ClipKind::Midi) {
                midiClip = QString::fromStdString(clip.id);
                break;
            }
        }
        if (id == originalTrack) originalClip = midiClip;
        else if (otherTrack.isEmpty()) {
            otherTrack = id;
            otherClip = midiClip;
        }
    }
    if (!otherTrack.isEmpty() && otherClip.isEmpty()) {
        otherClip = QString::fromStdString(
            m_controller.addMidiClip(otherTrack.toStdString(), 0.0, 1.0));
    }
    if (originalClip.isEmpty() || otherTrack.isEmpty() || otherClip.isEmpty())
        return false;

    constexpr quint64 sourceC = 0xFFFFFF03u;
    const bool originalOn = m_midiInput->injectMessageForTest(
        message(0x92, pitch, 100), sourceC);
    const bool originalLit = m_pianoRoll->livePitchHeldForTest(pitch);
    openPianoRoll(otherTrack, otherClip);
    const bool releasedOnSwitch = m_midiInput->heldCount() == 0 &&
        !livePitchesForTrack(originalTrack).test(std::size_t(pitch));
    const bool otherStayedDark = !m_pianoRoll->livePitchHeldForTest(pitch);
    openPianoRoll(originalTrack, originalClip);

    return first && lit && second && firstOff && overlapStayedLit &&
           secondOff && cleared && filtered && originalOn && originalLit &&
           releasedOnSwitch && otherStayedDark;
}

bool MainWindow::checkTypingKeyboard() {
    if (!m_typingKeyboard || !m_shortcuts) return false;

    // What every command is bound to before the keyboard borrows any keys.
    // Compared against, rather than hard-coded, so the check holds for a user
    // who has rebound half the application.
    QHash<QString, QKeySequence> bound;
    for (const ShortcutManager::Command& c : m_shortcuts->commands())
        bound.insert(c.id, m_shortcuts->shortcut(c.id));

    setTypingKeyboardEnabled(true);
    if (!m_typingKeyboard->isEnabled()) return false;

    for (const ShortcutManager::Command& c : m_shortcuts->commands()) {
        const QKeySequence seq = bound.value(c.id);
        // A borrowed key comes off the action — that is the whole point on
        // macOS — but the command still owns it: the shortcut editor must go on
        // showing it, and the conflict check must go on refusing to give it
        // away, or two commands end up on one key when it comes back.
        const bool borrowed = seq.count() == 1 &&
                              seq[0].keyboardModifiers() == Qt::NoModifier &&
                              TypingKeyboard::usesKey(seq[0].key());
        if (m_shortcuts->shortcut(c.id) != seq) return false;
        if (!c.action || seq.isEmpty()) continue;
        if (borrowed) {
            if (c.action->shortcuts().contains(seq)) return false;
            if (m_shortcuts->conflict(QStringLiteral("<probe>"), seq) != c.id)
                return false;
        } else if (!c.action->shortcuts().contains(seq)) {
            return false;   // a key the keyboard never asked for
        }
    }

    // Sent, not posted, and through the application so the filter that does all
    // the work is the thing under test rather than a copy of its logic. The
    // physical Z arrives as Russian Я, proving the live MIDI keyboard follows
    // positions just like command shortcuts do.
    QWidget* target = m_timeline ? static_cast<QWidget*>(m_timeline) : this;
    quint32 nativeScan = 0;
    quint32 nativeVirtual = 0;
#if defined(Q_OS_MACOS)
    nativeVirtual = 0x06;
#elif defined(Q_OS_WIN)
    nativeScan = 0x2c;
#elif defined(Q_OS_LINUX)
    nativeScan = QApplication::platformName() == QStringLiteral("xcb")
                     ? 0x34 : 0x2c;
#endif
    constexpr int kCyrillicYa = 0x0100042f;
    QKeyEvent press(QEvent::KeyPress, kCyrillicYa, Qt::NoModifier,
                    nativeScan, nativeVirtual, 0,
                    QString(QChar(0x042f)), false, 1);
    QApplication::sendEvent(target, &press);
    const bool sounded = m_typingKeyboard->heldCount() == 1;
    const std::string routedTrack = liveInputTarget();
    const int typedPitch = m_typingKeyboard->octave() * 12;
    const bool tracked = !routedTrack.empty() &&
        livePitchesForTrack(QString::fromStdString(routedTrack))
            .test(std::size_t(typedPitch));

    QKeyEvent release(QEvent::KeyRelease, kCyrillicYa, Qt::NoModifier,
                      nativeScan, nativeVirtual, 0,
                      QString(QChar(0x042f)), false, 1);
    QApplication::sendEvent(target, &release);
    const bool stopped = m_typingKeyboard->heldCount() == 0;
    const bool untracked = routedTrack.empty() ||
        !livePitchesForTrack(QString::fromStdString(routedTrack))
             .test(std::size_t(typedPitch));

    setTypingKeyboardEnabled(false);
    for (const ShortcutManager::Command& c : m_shortcuts->commands()) {
        const QKeySequence seq = bound.value(c.id);
        if (seq.isEmpty() || !c.action) continue;
        if (!c.action->shortcuts().contains(seq)) return false;   // handed back
    }

    // Left held on purpose, and never released: quitting with a key down is how
    // a note outlives the window, and the teardown has to end it while the
    // engine is still alive (see ~MainWindow). Last, so nothing above runs with
    // a note stuck on.
    setTypingKeyboardEnabled(true);
    QApplication::sendEvent(target, &press);

    return sounded && stopped && tracked && untracked;
}

bool MainWindow::checkLayoutIndependentShortcuts() {
    // A shortcut recorded while Russian is active is canonicalised too, so a
    // custom binding is just as layout-independent as a factory default.
    const QString commandId = QStringLiteral("edit.repeatClips");
    const QKeySequence original = m_shortcuts->shortcut(commandId);
    constexpr int kCyrillicI = 0x01000418;
    m_shortcuts->setShortcut(
        commandId,
        QKeySequence(QKeyCombination(Qt::ControlModifier,
                                     Qt::Key(kCyrillicI))));
    const bool capturedAsPhysicalB =
        m_shortcuts->shortcut(commandId) ==
        QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_B));
    m_shortcuts->setShortcut(commandId, original);

    // Disable the production Ctrl/Cmd+B action only for this synthetic press,
    // so a non-mutating probe can own the exact same requested combination.
    QAction* production = findChild<QAction*>(commandId);
    const bool productionEnabled = production && production->isEnabled();
    if (production) production->setEnabled(false);
    QAction probe(this);
    probe.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    probe.setShortcutContext(Qt::ApplicationShortcut);
    int triggered = 0;
    connect(&probe, &QAction::triggered, this, [&triggered] { ++triggered; });

    // Physical ANSI B. Native codes are deliberately supplied for each
    // desktop family; the logical Qt key/text say Russian И, exactly as they do
    // with that input source active.
    quint32 nativeScan = 0;
    quint32 nativeVirtual = 0;
#if defined(Q_OS_MACOS)
    nativeVirtual = 0x0b;
#elif defined(Q_OS_WIN)
    nativeScan = 0x30;
#elif defined(Q_OS_LINUX)
    nativeScan = QApplication::platformName() == QStringLiteral("xcb")
                     ? 0x38 : 0x30;
#endif
    QKeyEvent press(QEvent::KeyPress, kCyrillicI,
                    Qt::ControlModifier,
                    nativeScan, nativeVirtual, 0,
                    QString(QChar(0x0418)), false, 1);
    QApplication::sendEvent(m_timeline ? static_cast<QWidget*>(m_timeline) : this,
                            &press);
    if (production) production->setEnabled(productionEnabled);

    // The fixed toolbar controls are not menu items, but they are first-class
    // commands: searchable metadata, stable ids, and the same outcome as the
    // visible control. Keep the complete coverage list here so a renamed or
    // accidentally unregistered button fails the existing runnable selftest.
    const QStringList semanticIds = {
        QStringLiteral("transport.rewindBar"),
        QStringLiteral("transport.forwardBar"),
        QStringLiteral("view.timeBars"),
        QStringLiteral("view.timeClock"),
        QStringLiteral("view.followPlayhead"),
        QStringLiteral("edit.snapOn"),
        QStringLiteral("edit.snapOff"),
        QStringLiteral("edit.grid.off"),
        QStringLiteral("edit.grid.whole"),
        QStringLiteral("edit.grid.half"),
        QStringLiteral("edit.grid.quarter"),
        QStringLiteral("edit.grid.eighth"),
        QStringLiteral("edit.grid.sixteenth"),
        QStringLiteral("edit.grid.thirtySecond"),
        QStringLiteral("edit.grid.quarterTriplet"),
        QStringLiteral("edit.grid.eighthTriplet"),
        QStringLiteral("edit.grid.adaptive"),
        QStringLiteral("tool.secondarySelect"),
        QStringLiteral("tool.secondaryKnife"),
        QStringLiteral("tool.secondaryEraser"),
        QStringLiteral("tool.secondaryRegion"),
        QStringLiteral("tool.secondaryMute"),
        QStringLiteral("tool.secondaryDraw"),
        QStringLiteral("transport.playbackResume"),
        QStringLiteral("transport.playbackRestart"),
        QStringLiteral("transport.playFromClipOn"),
        QStringLiteral("transport.playFromClipOff"),
        QStringLiteral("view.automation.showAll"),
        QStringLiteral("view.automation.hideAll"),
        QStringLiteral("tool.automationCreationOn"),
        QStringLiteral("tool.automationCreationOff"),
        QStringLiteral("track.clearMutes"),
        QStringLiteral("browser.addFolder"),
        QStringLiteral("browser.refresh"),
        QStringLiteral("browser.settings"),
        QStringLiteral("browser.previewToggle"),
        QStringLiteral("browser.previewLoopOn"),
        QStringLiteral("browser.previewLoopOff"),
        QStringLiteral("browser.autoPreviewOn"),
        QStringLiteral("browser.autoPreviewOff"),
        QStringLiteral("editor.openSelected")};
    bool catalogComplete = true;
    for (const QString& id : semanticIds) {
        const ShortcutManager::Command* command = m_shortcuts->command(id);
        catalogComplete = catalogComplete && command && command->action &&
            command->action->objectName() == id &&
            !command->metadata.description.isEmpty() &&
            !command->metadata.helpId.isEmpty() &&
            command->metadata.risk != ShortcutManager::Risk::Unknown;
    }
    std::set<QString> uniqueIds;
    bool idsUnique = true;
    for (const ShortcutManager::Command& command : m_shortcuts->commands())
        idsUnique = uniqueIds.insert(command.id).second && idsUnique;
    const ShortcutManager::Command* followCommand =
        m_shortcuts->command(QStringLiteral("view.followPlayhead"));
    const bool followShortcut =
        followCommand &&
        followCommand->defaultSeq == QKeySequence(Qt::Key_P);

    const bool snapBefore = m_transport->snapEnabled();
    const bool snapCommands =
        m_shortcuts->invoke(QStringLiteral("edit.snapOff")) &&
        !m_transport->snapEnabled() &&
        m_shortcuts->invoke(QStringLiteral("edit.snapOn")) &&
        m_transport->snapEnabled();
    m_transport->setSnapEnabled(snapBefore);

    const bool barsBefore = m_transport->showsBars();
    const bool timeCommands =
        m_shortcuts->invoke(QStringLiteral("view.timeClock")) &&
        !m_transport->showsBars() &&
        m_shortcuts->invoke(QStringLiteral("view.timeBars")) &&
        m_transport->showsBars();
    m_transport->setTimeDisplayBars(barsBefore);

    const int gridBefore = m_transport->gridIndex();
    const bool gridCommand =
        m_shortcuts->invoke(QStringLiteral("edit.grid.eighth")) &&
        m_transport->gridIndex() == 4;
    m_transport->setGridIndex(gridBefore);

    const int secondaryBefore = m_transport->secondaryToolIndex();
    const bool secondaryCommand =
        m_shortcuts->invoke(QStringLiteral("tool.secondaryKnife")) &&
        m_transport->secondaryToolIndex() == 1;
    m_transport->setSecondaryToolIndex(secondaryBefore);

    const bool semanticCommands = catalogComplete && idsUnique && followShortcut &&
                                  snapCommands && timeCommands && gridCommand &&
                                  secondaryCommand;
    if (!semanticCommands) {
        std::fprintf(stderr,
                     "semantic command catalog: complete=%d unique=%d "
                     "follow=%d snap=%d time=%d grid=%d secondary=%d\n",
                     int(catalogComplete), int(idsUnique), int(followShortcut),
                     int(snapCommands), int(timeCommands), int(gridCommand),
                     int(secondaryCommand));
    }
    return capturedAsPhysicalB && triggered == 1 && semanticCommands;
}

bool MainWindow::checkFileDrop(const QStringList& paths, int expectedClips) {
    if (!m_timeline || paths.isEmpty()) return false;

    const auto countClips = [this] {
        size_t total = 0;
        for (const daw::TrackModel& track : m_controller.project().tracks)
            total += track.clips.size();
        return total;
    };
    const size_t before = countClips();

    // The same mime a file manager produces, and the same the browser's drag
    // builds — a plain list of local URLs.
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString& path : paths) urls << QUrl::fromLocalFile(path);
    mime.setUrls(urls);

    // Below the last lane, so the drop lands on empty space and takes the
    // "make a track for it" branch, which is the one a fresh project uses.
    const QPointF where(m_timeline->width() * 0.5, m_timeline->height() - 12.0);
    QDragEnterEvent enter(where.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &enter);
    if (!enter.isAccepted()) return false;

    QDropEvent drop(where, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &drop);

    return int(countClips() - before) == expectedClips;
}

bool MainWindow::openDemoBrowser(const QString& folder, const QString& selectFile) {
    if (!m_browser) return false;
    setBrowserVisible(true);
    return m_browser->showFolderForTest(folder, selectFile, /*persist=*/false);
}

bool MainWindow::openDemoBrowserPlugins() {
    if (!m_browser) return false;
    setBrowserVisible(true);
    return m_browser->showPluginsForTest();
}

bool MainWindow::checkBrowser(const QString& folder, const QString& audioFile) {
    constexpr qint64 kDecodeTimeoutMs = 15'000;
    const auto fail = [](const char* reason) {
        std::fprintf(stderr, "browser self-test: %s\n", reason);
        return false;
    };
    if (!m_browser) return fail("panel was not constructed");
    if (!openDemoBrowser(folder, audioFile))
        return fail("demo file was not found in the tree");

    // Selecting the file decodes it on a worker thread; the audition and the
    // waveform both arrive by queued signal, so the loop below is the honest
    // way to wait for them without a fixed sleep. What is checked is the arming
    // — `previewPlaying()` would need an audio device, and a headless run has
    // none; the node's own behaviour is covered in engine_graph_test.
    const std::string wanted = QFileInfo(audioFile).absoluteFilePath().toStdString();
    QElapsedTimer decodeTimeout;
    decodeTimeout.start();
    while ((m_controller.previewPath() != wanted ||
            !(m_controller.previewDurationSeconds() > 0.0) ||
            !m_browser->hasPreviewWaveformForTest()) &&
           decodeTimeout.elapsed() < kDecodeTimeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (m_controller.previewPath() != wanted)
        return fail("decoded preview path did not arrive");
    if (!(m_controller.previewDurationSeconds() > 0.0))
        return fail("decoded preview has no duration");
    if (!m_browser->hasPreviewWaveformForTest())
        return fail("decoded preview has no waveform");

    m_controller.stopPreview();

    // With auto-preview off, selecting a file must still draw its waveform and
    // still make no sound — the switch is the difference between a browser you
    // can work over a playing project and one you cannot.
    {
        const bool wasAuto = ui::browserprefs::autoPreview();
        ui::browserprefs::setAutoPreview(false);
        if (!m_browser->reloadSelectedPreviewForTest()) {
            ui::browserprefs::setAutoPreview(wasAuto);
            return fail("selected audio file could not be reloaded");
        }
        decodeTimeout.restart();
        while (!m_browser->hasPreviewWaveformForTest() &&
               decodeTimeout.elapsed() < kDecodeTimeoutMs) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        const bool drew = m_browser->hasPreviewWaveformForTest();
        const bool silent = !m_controller.previewPlaying();
        ui::browserprefs::setAutoPreview(wasAuto);
        if (!drew) return fail("waveform did not redraw with auto-preview off");
        if (!silent) return fail("auto-preview off still started playback");
    }

    // What a drag out of the browser would carry, fed straight into the
    // arrangement's real drop handler: this is the whole chain the user's
    // gesture takes, minus the mouse.
    const QStringList dragged = m_browser->dragUrlsForTest();
    if (dragged.size() != 1 ||
        QFileInfo(dragged.first()).absoluteFilePath() !=
            QFileInfo(audioFile).absoluteFilePath()) {
        return fail("selected audio file produced the wrong drag payload");
    }
    if (!checkFileDrop(dragged, 1))
        return fail("dragged audio file was not imported");
    m_controller.undo();

    // ── The plugin folders, and dragging one out of them ──
    //
    // Skipped when nothing has been scanned — a machine with no plugins is a
    // legitimate state, and failing there would make the check about the host
    // rather than the code.
    if (openDemoBrowserPlugins()) {
        const QStringList urls = m_browser->dragUrlsForTest();
        if (!urls.isEmpty()) {
            std::fprintf(stderr,
                         "a plugin row offered file URLs, which every audio "
                         "target would try to import\n");
            return false;
        }
        // The real payload, decoded the way a drop target decodes it, then sent
        // through the actual track-header and Channel Strip event handlers.
        std::unique_ptr<QMimeData> mime(m_browser->pluginDragForTest());
        int format = 0;
        QString uid;
        if (!mime || !ui::decodePluginRef(mime.get(), format, uid)) {
            std::fprintf(stderr, "a plugin row carried no plugin reference\n");
            return false;
        }
        const auto descriptor = m_controller.pluginManager().find(
            daw::plugins::Format(format), uid.toStdString());
        if (!descriptor) {
            std::fprintf(stderr, "the dragged reference resolves to no plugin\n");
            return false;
        }

        // An instrument needs a track that plays notes; an effect takes any
        // channel. Picking the wrong one here would test the guard, not the
        // drop.
        const std::string track = m_controller.addTrack(
            descriptor->isInstrument ? daw::TrackKind::Instrument
                                     : daw::TrackKind::Audio,
            "Plugin Drop");
        syncViews();
        QApplication::processEvents();
        QWidget* targetRow = nullptr;
        for (QWidget* candidate : m_trackList->findChildren<QWidget*>()) {
            if (candidate->objectName() == QLatin1String("TrackRow") &&
                candidate->property("trackId").toString() ==
                    QString::fromStdString(track)) {
                targetRow = candidate;
                break;
            }
        }
        if (!targetRow) {
            std::fprintf(stderr, "the plugin-drop track has no header row\n");
            return false;
        }
        const QPoint rowPoint = targetRow->mapTo(
            m_trackList, targetRow->rect().center());
        QDragEnterEvent trackEnter(rowPoint, Qt::CopyAction, mime.get(),
                                   Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_trackList, &trackEnter);
        QDropEvent trackDrop(QPointF(rowPoint), Qt::CopyAction, mime.get(),
                             Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_trackList, &trackDrop);
        QApplication::processEvents();
        const auto* droppedTrack = m_controller.project().findTrack(track);
        const bool onTrack = droppedTrack &&
            (descriptor->isInstrument
                 ? droppedTrack->instrument.uid == descriptor->uid
                 : droppedTrack->inserts.size() == 1);
        if (!onTrack) {
            std::fprintf(stderr, "dropping a plugin on a track did nothing\n");
            return false;
        }

        // Aim at a drop-enabled child well, not the outer strip. This is the
        // former intermittent hole: a slot used to consume the drag before the
        // channel could see the browser payload.
        ChannelStrip strip(&m_controller, QString::fromStdString(track),
                           /*master=*/false);
        strip.resize(strip.sizeHint());
        QWidget* well = strip.findChild<QWidget*>(QStringLiteral("SlotWell"));
        if (!well) {
            std::fprintf(stderr, "the channel strip has no plugin well\n");
            return false;
        }
        const QPoint wellPoint = well->rect().center();
        QDragEnterEvent stripEnter(wellPoint, Qt::CopyAction, mime.get(),
                                   Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(well, &stripEnter);
        QDropEvent stripDrop(QPointF(wellPoint), Qt::CopyAction, mime.get(),
                             Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(well, &stripDrop);
        QApplication::processEvents();
        droppedTrack = m_controller.project().findTrack(track);
        const bool onStrip = droppedTrack &&
            (descriptor->isInstrument
                 ? droppedTrack->instrument.uid == descriptor->uid
                 : droppedTrack->inserts.size() == 2);
        if (!onStrip) {
            std::fprintf(stderr,
                         "dropping a plugin on a Channel Strip well did nothing\n");
            return false;
        }

        // The other half: onto a clip, where the plugin runs on that clip alone
        // instead of on everything the track plays.
        if (!descriptor->isInstrument) {
            const std::string audio =
                m_controller.addTrack(daw::TrackKind::Audio, "Clip Drop");
            const std::string clip =
                m_controller.importAudio(audioFile.toStdString(), audio, 0.0);
            if (clip.empty()) {
                std::fprintf(stderr, "could not make a clip to drop onto\n");
                return false;
            }
            if (m_controller.addClipFxInsert(audio, clip, *descriptor).empty()) {
                std::fprintf(stderr, "dropping a plugin on a clip did nothing\n");
                return false;
            }
            const auto* chain = m_controller.clipFx(audio, clip);
            if (!chain || chain->size() != 1) {
                std::fprintf(stderr, "the clip's own chain did not take it\n");
                return false;
            }
            m_controller.removeTrack(audio);
        }


        // Presets use plain file URLs, but they are channel data rather than
        // audio. Prove the browser marks `.vlts` rows draggable and that both
        // the track header and a child well in the strip route them correctly.
        const std::string presetSource = m_controller.addTrack(
            daw::TrackKind::Audio, "Preset Drag Source");
        m_controller.setTrackVolume(presetSource, 0.37f);
        const QString presetPath =
            QDir::temp().filePath(QStringLiteral("daw-browser-drop.vlts"));
        QFile::remove(presetPath);
        const audio::Result saved = m_controller.saveChannelStripPreset(
            presetSource, presetPath.toStdString());
        m_controller.removeTrack(presetSource);
        if (!saved ||
            !m_browser->showFolderForTest(QFileInfo(presetPath).absolutePath(),
                                          presetPath, /*persist=*/false)) {
            QFile::remove(presetPath);
            std::fprintf(stderr,
                         "the browser could not select a Channel Strip preset\n");
            return false;
        }
        std::unique_ptr<QMimeData> presetMime(m_browser->dragPayloadForTest());
        if (!presetMime || presetMime->urls().size() != 1) {
            QFile::remove(presetPath);
            std::fprintf(stderr,
                         "a Channel Strip preset was not draggable\n");
            return false;
        }

        syncViews();
        QApplication::processEvents();
        targetRow = nullptr;
        for (QWidget* candidate : m_trackList->findChildren<QWidget*>()) {
            if (candidate->objectName() == QLatin1String("TrackRow") &&
                candidate->property("trackId").toString() ==
                    QString::fromStdString(track)) {
                targetRow = candidate;
                break;
            }
        }
        if (!targetRow) {
            QFile::remove(presetPath);
            std::fprintf(stderr, "the preset-drop track has no header row\n");
            return false;
        }
        const QPoint presetRowPoint = targetRow->mapTo(
            m_trackList, targetRow->rect().center());
        QDragEnterEvent presetTrackEnter(
            presetRowPoint, Qt::CopyAction, presetMime.get(), Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(m_trackList, &presetTrackEnter);
        QDropEvent presetTrackDrop(QPointF(presetRowPoint), Qt::CopyAction,
                                   presetMime.get(), Qt::LeftButton,
                                   Qt::NoModifier);
        QApplication::sendEvent(m_trackList, &presetTrackDrop);
        QApplication::processEvents();
        droppedTrack = m_controller.project().findTrack(track);
        if (!droppedTrack || std::abs(droppedTrack->volume - 0.37f) > 0.001f) {
            QFile::remove(presetPath);
            std::fprintf(stderr,
                         "dropping a preset on a track did nothing\n");
            return false;
        }

        m_controller.setTrackVolume(track, 1.1f);
        QDragEnterEvent presetStripEnter(
            wellPoint, Qt::CopyAction, presetMime.get(), Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(well, &presetStripEnter);
        QDropEvent presetStripDrop(QPointF(wellPoint), Qt::CopyAction,
                                   presetMime.get(), Qt::LeftButton,
                                   Qt::NoModifier);
        QApplication::sendEvent(well, &presetStripDrop);
        QApplication::processEvents();
        droppedTrack = m_controller.project().findTrack(track);
        QFile::remove(presetPath);
        if (!droppedTrack || std::abs(droppedTrack->volume - 0.37f) > 0.001f) {
            std::fprintf(stderr,
                         "dropping a preset on a Channel Strip did nothing\n");
            return false;
        }
        m_controller.removeTrack(track);
    }

    // Project templates are directory packages, but the browser must expose
    // each one as a single draggable row. Exercise both drop surfaces, then
    // activate that same row as a new project (the double-click/Enter signal's
    // real destination).
    {
        const QString templateBase =
            QStringLiteral("daw-browser-project-template-%1")
                .arg(QCoreApplication::applicationPid());
        const QString templatePath =
            QDir(ui::projecttemplates::folder())
                .filePath(templateBase + QStringLiteral(".vltt"));
        QDir(templatePath).removeRecursively();
        const std::size_t templateTracks = m_controller.project().tracks.size();
        const audio::Result saved = m_controller.saveProjectTemplate(
            templatePath.toStdString(), "Browser Template");
        if (!saved || templateTracks == 0) {
            QDir(templatePath).removeRecursively();
            if (!saved) {
                std::fprintf(stderr, "browser self-test: VLTT save failed: %s\n",
                             saved.message().c_str());
            }
            return fail("the Templates library could not store VLTT");
        }

        const QStringList templateResults =
            m_browser->searchForTest(templateBase);
        const QStringList internalResults =
            m_browser->searchForTest(QStringLiteral("Project.vlt"));
        const QString packagePrefix =
            QDir::fromNativeSeparators(templatePath) + QLatin1Char('/');
        const bool exposedInternal =
            std::any_of(internalResults.cbegin(), internalResults.cend(),
                        [&packagePrefix](const QString& result) {
                            return QDir::fromNativeSeparators(result)
                                .startsWith(packagePrefix);
                        });
        if (!templateResults.contains(templatePath) || exposedInternal ||
            !m_browser->showFolderForTest(QFileInfo(templatePath).absolutePath(),
                                          templatePath, /*persist=*/false) ||
            !m_browser->selectedProjectTemplateForTest()) {
            QDir(templatePath).removeRecursively();
            return fail("VLTT search did not expose one atomic template row");
        }

        std::unique_ptr<QMimeData> mime(m_browser->dragPayloadForTest());
        if (!mime || mime->urls().size() != 1 ||
            mime->urls().front().toLocalFile() != templatePath) {
            QDir(templatePath).removeRecursively();
            return fail("the project template carried the wrong drag payload");
        }

        const std::size_t beforeHeaders = m_controller.project().tracks.size();
        const QPoint headerPoint(m_trackList->width() / 2,
                                 std::max(1, m_trackList->height() - 4));
        QDragEnterEvent headerEnter(headerPoint, Qt::CopyAction, mime.get(),
                                    Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_trackList, &headerEnter);
        QDropEvent headerDrop(QPointF(headerPoint), Qt::CopyAction, mime.get(),
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_trackList, &headerDrop);
        QApplication::processEvents();
        if (!headerEnter.isAccepted() ||
            m_controller.project().tracks.size() !=
                beforeHeaders + templateTracks) {
            QDir(templatePath).removeRecursively();
            return fail("dropping VLTT on the track headers did not add tracks");
        }
        m_controller.undo();
        syncViews();

        const std::size_t beforeTimeline = m_controller.project().tracks.size();
        const QPoint timelinePoint(m_timeline->width() / 2,
                                   std::max(1, m_timeline->height() - 4));
        QDragEnterEvent timelineEnter(timelinePoint, Qt::CopyAction, mime.get(),
                                      Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_timeline, &timelineEnter);
        QDropEvent timelineDrop(QPointF(timelinePoint), Qt::CopyAction,
                                mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_timeline, &timelineDrop);
        QApplication::processEvents();
        if (!timelineEnter.isAccepted() ||
            m_controller.project().tracks.size() !=
                beforeTimeline + templateTracks) {
            QDir(templatePath).removeRecursively();
            return fail("dropping VLTT on the timeline did not add tracks");
        }
        m_controller.undo();
        syncViews();

        // Earlier drop checks deliberately made this harness document dirty;
        // the activation signal itself is under test here, not the unrelated
        // save-changes prompt (which is exercised by the normal new/open flow).
        m_dirty = false;
        if (!m_browser->activateSelectedProjectTemplateForTest() ||
            !m_projectPath.isEmpty() ||
            m_controller.projectName() != "Browser Template") {
            QDir(templatePath).removeRecursively();
            return fail("activating VLTT did not create an unsaved project");
        }
        QDir(templatePath).removeRecursively();
    }

    // Both sides of the window, since the row is torn down and rebuilt for it.
    const bool wasLeft = m_browserOnLeft;
    setBrowserOnLeft(!wasLeft, /*persist=*/false);
    setBrowserOnLeft(wasLeft, /*persist=*/false);
    // Zoom is the browser's own, and nothing else in the window may move with
    // it — the whole point of scoping it to the panel.
    {
        const double before = m_browser->zoom();
        const int lanesBefore = m_timeline->width();
        m_browser->zoomBy(1.15);
        if (!(m_browser->zoom() > before)) {
            std::fprintf(stderr, "the browser did not zoom in\n");
            return false;
        }
        if (m_timeline->width() != lanesBefore) {
            std::fprintf(stderr, "zooming the browser resized the arrangement\n");
            return false;
        }
        m_browser->setZoom(before);
    }
    if (!m_browser->isVisible()) return fail("panel was hidden after moving sides");
    return true;
}

bool MainWindow::checkAiAssistant() {
    // The panel installs a scripted stand-in for a provider, so this exercises
    // the real panel → session → dispatch → document → undo path with no key
    // and no network, and costs nothing to run on every build.
    return m_aiPanel && m_aiPanel->checkAgentForTest();
}

bool MainWindow::checkAiMusic() {
    // Same idea one mode over: a stand-in answers with a real file, so the
    // brief -> file -> import -> undo path is checked without a music server.
    return m_aiPanel && m_aiPanel->checkMusicForTest();
}

void MainWindow::openDemoAi(bool withTranscript) {
    setAiVisible(true, /*persist=*/false);
    if (m_aiPanel && withTranscript) m_aiPanel->showDemoTranscriptForTest();
}

void MainWindow::openDemoAiMusic() {
    setAiVisible(true, /*persist=*/false);
    if (m_aiPanel) m_aiPanel->showDemoMusicTranscriptForTest();
}


bool MainWindow::loadDemoPluginSlots() {
    if (m_controller.project().tracks.empty()) return false;
    const auto& scanned = m_controller.pluginManager().plugins();

    const daw::plugins::PluginDescriptor* effect = nullptr;
    const daw::plugins::PluginDescriptor* instrument = nullptr;
    for (const daw::plugins::PluginDescriptor& d : scanned) {
        if (d.isInstrument) {
            if (!instrument) instrument = &d;
        } else if (!effect) {
            effect = &d;
        }
        if (effect && instrument) break;
    }
    if (!effect) return false;

    const std::string trackId = m_controller.project().tracks.front().id;
    // Two, so both a first and a following slot are on screen: the hover
    // actions have to sit inside their own row and not the one below it.
    m_controller.addInsert(trackId, *effect);
    m_controller.addInsert(trackId, *effect);
    // The second one is bypassed, so a grab shows an active and a bypassed
    // slot side by side — the two states are only worth anything if they can
    // be told apart without the pointer on them.
    if (const auto* inserts = m_controller.project().findTrack(trackId)
                                  ? &m_controller.project().findTrack(trackId)->inserts
                                  : nullptr;
        inserts && inserts->size() >= 2) {
        m_controller.setInsertBypassed(trackId, (*inserts)[1].id, true);
    }

    if (instrument) {
        for (const auto& t : m_controller.project().tracks) {
            if (!daw::trackAccepts(t.kind, daw::ClipKind::Midi)) continue;
            m_controller.setTrackInstrumentPlugin(t.id, *instrument);
            break;
        }
    }
    onTracksChanged();
    return true;
}

bool MainWindow::checkPluginAutoOpenForTest() {
    daw::EngineController probe;
    const auto sampler = probe.pluginManager().find(
        daw::plugins::Format::Internal, "daw.sampler");
    if (!sampler) return false;

    const std::string track =
        probe.addTrack(daw::TrackKind::Midi, "Plugin Auto-open Probe");
    ChannelStrip strip(&probe, QString::fromStdString(track), false);

    QString requestedChannel;
    QString requestedSlot;
    connect(&strip, &ChannelStrip::editorRequested, &strip,
            [&requestedChannel, &requestedSlot](const QString& channel,
                                                const QString& slot) {
                requestedChannel = channel;
                requestedSlot = slot;
            });

    std::function<QAction*(QMenu*)> findSampler =
        [&findSampler](QMenu* menu) -> QAction* {
            if (!menu) return nullptr;
            for (QAction* action : menu->actions()) {
                if (QAction* nested = findSampler(action->menu())) return nested;
                if (action->isEnabled() &&
                    action->text().contains(QStringLiteral("Sampler"),
                                            Qt::CaseInsensitive)) {
                    return action;
                }
            }
            return nullptr;
        };

    QMenu* instrumentMenu = nullptr;
    bool effectMenuFound = false;
    for (QToolButton* button : strip.findChildren<QToolButton*>()) {
        QMenu* menu = button->menu();
        if (!menu || !menu->property("pluginPickerLazy").toBool()) continue;
        // Constructing a ChannelStrip must leave every empty slot as a tiny
        // placeholder rather than one complete plugin/action tree per slot.
        if (!menu->actions().isEmpty()) return false;
        if (menu->property("pluginPickerInstruments").toBool())
            instrumentMenu = menu;
        else
            effectMenuFound = true;
    }
    if (!instrumentMenu || !effectMenuFound) return false;

    instrumentMenu->popup(QPoint(0, 0));
    QApplication::processEvents();
    QAction* pick = findSampler(instrumentMenu);
    if (!pick) {
        instrumentMenu->close();
        QApplication::processEvents();
        return false;
    }
    pick->trigger();
    instrumentMenu->close();
    QApplication::processEvents();

    const auto* loaded = probe.project().findTrack(track);
    return loaded && loaded->instrument.isLoaded() &&
           instrumentMenu->actions().isEmpty() &&
           requestedChannel == QString::fromStdString(track) &&
           requestedSlot == QString::fromStdString(loaded->instrument.id);
}

bool MainWindow::checkPluginSearchFocusForTest() {
    daw::EngineController probe;
    QMenu* menu = ui::buildPluginMenu(
        this, &probe, /*instruments=*/true,
        [](const daw::plugins::PluginDescriptor&) {});
    auto* edit = menu->findChild<QLineEdit*>(
        QStringLiteral("PluginPickerSearch"));
    if (!edit) {
        delete menu;
        return false;
    }

    menu->popup(mapToGlobal(QPoint(16, 16)));
    QApplication::processEvents();
    const bool focusedOnOpen = edit->hasFocus();

    QMenu* submenu = nullptr;
    for (QAction* action : menu->actions()) {
        if (action->menu()) {
            submenu = action->menu();
            break;
        }
    }
    if (!submenu) {
        menu->close();
        delete menu;
        return false;
    }

    // This is the state produced by moving through a vendor/category with the
    // mouse: the child QMenu, rather than the line edit, receives the key.
    submenu->setFocus(Qt::MouseFocusReason);
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_B,
                            Qt::NoModifier, QStringLiteral("и"));
    overrideEvent.setAccepted(false);
    QApplication::sendEvent(submenu, &overrideEvent);
    QKeyEvent pressEvent(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier,
                         QStringLiteral("и"));
    QApplication::sendEvent(submenu, &pressEvent);

    const bool routed = overrideEvent.isAccepted() && edit->hasFocus() &&
                        edit->text() == QStringLiteral("и");

    // Ctrl/Cmd+F must stay inside the open picker as a search gesture rather
    // than invoking an application command which tears the popup down.
    const QList<QKeySequence> findBindings =
        QKeySequence::keyBindings(QKeySequence::Find);
    if (findBindings.isEmpty() || findBindings.front().isEmpty()) {
        menu->close();
        delete menu;
        return false;
    }
    const QKeyCombination findKey = findBindings.front()[0];
    submenu->setFocus(Qt::MouseFocusReason);
    QKeyEvent findOverride(QEvent::ShortcutOverride, findKey.key(),
                           findKey.keyboardModifiers(),
                           QStringLiteral("f"));
    findOverride.setAccepted(false);
    QApplication::sendEvent(submenu, &findOverride);
    QKeyEvent findPress(QEvent::KeyPress, findKey.key(),
                        findKey.keyboardModifiers(),
                        QStringLiteral("f"));
    QApplication::sendEvent(submenu, &findPress);
    const bool shortcutStayedInSearch = findOverride.isAccepted() &&
                                        edit->hasFocus() &&
                                        edit->hasSelectedText() &&
                                        edit->text() == QStringLiteral("и");
    menu->close();
    delete menu;
    return focusedOnOpen && routed && shortcutStayedInSearch;
}

void MainWindow::openSettings(int tab) {
    if (!m_settingsWindow) {
        m_settingsWindow = new SettingsWindow(&m_controller, m_shortcuts, this);
        hostInternalWindow(m_settingsWindow,
                           QStringLiteral("internalEditors/settings"));
        connect(m_settingsWindow, &SettingsWindow::contextPanelSettingsChanged,
                this, [this] {
                    if (!m_contextPanel) return;
                    m_contextPanel->invalidateBackdrop();
                    m_contextPanel->reloadFollowSetting();
                    m_contextPanel->rebuild();
                    if (m_noteContextPanel) {
                        m_noteContextPanel->invalidateBackdrop();
                        m_noteContextPanel->reloadFollowSetting();
                        m_noteContextPanel->rebuild();
                    }
                    syncPianoRollContextPanel();
                    layoutContextPanel();
                });
        // The mode can be changed from either place, so each one follows the
        // other rather than the two drifting apart.
        connect(m_settingsWindow, &SettingsWindow::recordModeChanged, m_transport,
                &TransportBar::refresh);
        connect(m_settingsWindow, &SettingsWindow::browserSettingsChanged, this,
                [this] {
                    if (m_browser) m_browser->reloadSettings();
                    if (m_webPanel) m_webPanel->reloadSettings();
                    relayoutRow();
                });
        connect(m_settingsWindow, &SettingsWindow::aiSettingsChanged, this,
                [this] {
                    if (m_aiPanel) m_aiPanel->reloadSettings();
                });
        connect(m_settingsWindow, &SettingsWindow::restartRequested, this,
                [this] {
                    if (!maybeSaveChanges()) return;
                    QStringList arguments;
                    if (!m_projectPath.isEmpty()) arguments.push_back(m_projectPath);
                    m_dirty = false;
                    if (QProcess::startDetached(
                            QCoreApplication::applicationFilePath(), arguments)) {
                        QApplication::quit();
                    } else {
                        QMessageBox::warning(
                            this, tr("Restart failed"),
                            tr("VLT Studio Pro could not start a new process. "
                               "Please restart the application manually."));
                    }
                });
        connect(m_settingsWindow, &SettingsWindow::accountLogoutRequested, this,
                [this] {
                    if (!maybeSaveChanges()) return;
                    // Discard is a deliberate answer too; suppress a second
                    // prompt from closeEvent while the clean auth process is
                    // started.
                    m_dirty = false;
                    if (auto* service = account::Service::instance()) {
                        connect(service, &account::Service::logoutFinished, this, [] {
                            QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList{});
                            QApplication::quit();
                        }, Qt::SingleShotConnection);
                        service->logout();
                    }
                });
    }
    m_settingsWindow->showTab(tab);
    presentInternalWindow(m_settingsWindow);
}

void MainWindow::selectAllNotesForShot() {
    if (m_pianoRoll) m_pianoRoll->selectAllNotesForTest();
}

void MainWindow::openFirstMidiClip() {
    for (const auto& track : m_controller.project().tracks) {
        for (const auto& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Midi) continue;
            openPianoRoll(QString::fromStdString(track.id),
                          QString::fromStdString(clip.id));
            return;
        }
    }
}

void MainWindow::openFirstAudioClip() {
    for (const daw::TrackModel& track : m_controller.project().tracks) {
        for (const daw::ClipModel& clip : track.clips) {
            if (clip.kind != daw::ClipKind::Audio || daw::isLayered(clip)) continue;
            openSampleEditor(QString::fromStdString(track.id),
                             QString::fromStdString(clip.id));
            return;
        }
    }
}

void MainWindow::openDemoPattern(bool showEditor) {
    const std::string patternId = m_controller.addPattern("Night Drive");
    struct Source {
        const char* name;
        const char* instrument;
        uint32_t color;
        int pitch;
        double step;
    };
    static constexpr Source sources[] = {
        {"Kick 909", "Sampler", 0xE0645A, 36, 1.0},
        {"Analog Bass", "Serum", 0x55A7E8, 40, 0.5},
        {"Glass Keys", "Pigments", 0x8FBF5A, 60, 0.75},
        {"Air Texture", "Sampler", 0xD69B4A, 72, 2.0},
    };
    for (const Source& source : sources) {
        const std::string trackId = m_controller.addTrack(
            daw::TrackKind::Instrument, source.name);
        m_controller.moveTrackToFolder(trackId, patternId);
        m_controller.setTrackColor(trackId, source.color);
        m_controller.setTrackInstrument(trackId, source.instrument);
        const std::string clipId = m_controller.addMidiClip(trackId, 0.0);
        std::vector<daw::NoteModel> notes;
        for (double beat = 0.0; beat < 4.0; beat += source.step) {
            daw::NoteModel note;
            note.id = daw::newUuid();
            note.pitch = source.pitch + (int(beat / source.step) % 3 == 2 ? 7 : 0);
            note.startBeats = beat;
            note.lengthBeats = std::min(0.42, source.step * 0.72);
            note.velocity = 92 + int(beat * 5.0) % 26;
            notes.push_back(note);
        }
        m_controller.setClipNotes(trackId, clipId, std::move(notes),
                                  "Seed Pattern Demo");
    }
    const bool expandedTimelineShot =
        qEnvironmentVariable("DAW_SHOT_PATTERN") ==
        QStringLiteral("expanded-timeline");
    m_controller.setFolderExpanded(patternId, expandedTimelineShot);
    syncViews();
    selectTrackFromHeader(QString::fromStdString(patternId));
    if (showEditor) openPattern(QString::fromStdString(patternId));
}

bool MainWindow::canOpenSelectedEditor() const {
    const ui::ClipSel clip = m_selection.singleClip();
    if (!clip.clipId.isEmpty()) {
        const daw::TrackModel* track =
            m_controller.project().findTrack(clip.trackId.toStdString());
        if (!track) return false;
        return std::any_of(track->clips.cbegin(), track->clips.cend(),
                           [&clip](const daw::ClipModel& candidate) {
                               return candidate.id == clip.clipId.toStdString();
                           });
    }
    const QString trackId = m_selection.singleTrack();
    const daw::TrackModel* track =
        m_controller.project().findTrack(trackId.toStdString());
    return track && track->kind == daw::TrackKind::Pattern;
}

void MainWindow::openSelectedEditor() {
    const ui::ClipSel selected = m_selection.singleClip();
    if (!selected.clipId.isEmpty()) {
        const daw::TrackModel* track =
            m_controller.project().findTrack(selected.trackId.toStdString());
        if (track) {
            const auto found = std::find_if(
                track->clips.cbegin(), track->clips.cend(),
                [&selected](const daw::ClipModel& clip) {
                    return clip.id == selected.clipId.toStdString();
                });
            if (found != track->clips.cend()) {
                switch (found->kind) {
                    case daw::ClipKind::Midi:
                        openPianoRoll(selected.trackId, selected.clipId);
                        return;
                    case daw::ClipKind::Audio:
                        openSampleEditor(selected.trackId, selected.clipId);
                        return;
                    case daw::ClipKind::Automation:
                        openAutomationEditor(selected.trackId, selected.clipId);
                        return;
                    case daw::ClipKind::Pattern:
                        openPattern(selected.trackId);
                        return;
                }
            }
        }
    } else {
        const QString trackId = m_selection.singleTrack();
        const daw::TrackModel* track =
            m_controller.project().findTrack(trackId.toStdString());
        if (track && track->kind == daw::TrackKind::Pattern) {
            openPattern(trackId);
            return;
        }
    }
    statusBar()->showMessage(
        tr("Select a MIDI, audio, automation or Pattern clip to open its editor"),
        3000);
}

void MainWindow::openPianoRoll(const QString& trackId, const QString& clipId) {
    if (!m_pianoRoll) {
        if (!m_editorHost) return;
        m_pianoRollFrame = new InternalEditorFrame(
            QStringLiteral("internalEditors/pianoRoll"), m_editorHost);
        m_pianoRoll = new PianoRollWindow(&m_controller, m_pianoRollFrame);
        m_pianoRollFrame->setContent(m_pianoRoll);
        // Keep the note island in the actual shared strip. Parenting it to the
        // whole workspace made it only look like a toolbar control: a press was
        // classified as leaving the internal editor, so the panel disappeared
        // before the button could receive its release/click.
        m_noteContextPanel = m_pianoRoll->createContextPanel(m_toolPanel);
        if (m_noteContextPanel) {
            m_pianoRollFrame->setAccessoryWidget(m_noteContextPanel);
            m_noteContextPanel->setPanelEnabled(
                m_contextPanel && m_contextPanel->isPanelEnabled());
        }
        connect(m_pianoRollFrame, &InternalEditorFrame::activeChanged, this,
                [this](bool) {
                    syncPianoRollContextPanel();
                    if (m_midiInput) m_midiInput->refreshTarget();
                });
        connect(m_pianoRoll, &PianoRollWindow::noteSelectionChanged, this,
                [this](bool) { syncPianoRollContextPanel(); });
        connect(m_pianoRoll, &PianoRollWindow::internalWindowRequested, this,
                [this](QWidget* content, const QString& settingsKey) {
                    hostInternalWindow(content, settingsKey);
                    presentInternalWindow(content);
                });
        connect(m_pianoRollFrame, &InternalEditorFrame::closeRequested, this,
                [this] {
                    if (m_pianoRoll) m_pianoRoll->close();
                    if (m_pianoRollFrame) m_pianoRollFrame->hide();
                    if (centralWidget())
                        centralWidget()->setFocus(Qt::OtherFocusReason);
                });
        connect(m_pianoRoll, &PianoRollWindow::edited, this, [this] {
            markDirty();
            if (m_timeline) m_timeline->update();
        });
        connect(m_pianoRoll, &PianoRollWindow::loopRangeChanged, this, [this] {
            m_transport->setCycleEnabled(m_controller.isLoopEnabled());
            m_timeline->update();
            markDirty();
        });
        connect(m_pianoRoll, &PianoRollWindow::playheadMoved, this, [this] {
#ifdef DAW_ENABLE_COLLABORATION
            if (m_collaboration && !m_applyingRemoteTransport) {
                m_collaboration->localSessionState()
                    ->noteLocalTransportInteraction();
                publishSessionTransport(true);
            }
#endif
            if (m_timeline) m_timeline->update();
            if (m_transport) m_transport->refresh();
        });
        connect(m_pianoRoll, &PianoRollWindow::trackStateChanged, this,
                [this](bool localFileDirty) {
            syncViews();
            if (localFileDirty) markDirty();
        });
        connect(m_pianoRoll, &PianoRollWindow::automateMuteRequested, this,
                [this](const QString& trackId) {
                    daw::AutomationTarget target;
                    target.kind = daw::AutomationTargetKind::TrackMute;
                    target.channelId = trackId.toStdString();
                    automateTarget(target);
        });
    }
    m_pianoRoll->setClip(trackId, clipId);
#ifdef DAW_ENABLE_COLLABORATION
    registerPianoRollPresence();
#endif
    m_pianoRoll->setLivePitches(livePitchesForTrack(trackId));
    m_pianoRollFrame->present();
    if (m_midiInput) m_midiInput->refreshTarget();
    syncPianoRollContextPanel();
}

void MainWindow::openPattern(const QString& patternId) {
    const auto* pattern =
        m_controller.project().findTrack(patternId.toStdString());
    if (!pattern || pattern->kind != daw::TrackKind::Pattern) return;
    if (!m_patternWindow) {
        m_patternWindow = new PatternWindow(&m_controller, this);
        hostInternalWindow(m_patternWindow,
                           QStringLiteral("internalEditors/pattern"));
        connect(m_patternWindow, &PatternWindow::projectEdited, this,
                [this](bool localFileDirty) {
            m_orphanEditorSweepPending = true;
            // The Pattern editor already updates its own rows synchronously.
            // Mirror the lane order now and let channel-strip reconstruction
            // follow after the first paint, matching direct track gestures.
            syncStructureViews();
            if (localFileDirty) markDirty();
        });
        connect(m_patternWindow, &PatternWindow::openPianoRollRequested, this,
                &MainWindow::openPianoRoll);
        connect(m_patternWindow, &PatternWindow::openPluginEditorRequested, this,
                &MainWindow::openPluginEditor);
        connect(m_patternWindow, &PatternWindow::automateControlRequested, this,
                [this](const QString& trackId, bool pan) {
                    daw::AutomationTarget target;
                    target.kind = pan ? daw::AutomationTargetKind::TrackPan
                                      : daw::AutomationTargetKind::TrackVolume;
                    target.channelId = trackId.toStdString();
                    automateTarget(target);
                });
        connect(m_patternWindow, &PatternWindow::automateMuteRequested, this,
                [this](const QString& trackId) {
                    daw::AutomationTarget target;
                    target.kind = daw::AutomationTargetKind::TrackMute;
                    target.channelId = trackId.toStdString();
                    automateTarget(target);
                });
    }
    m_patternWindow->setPattern(patternId);
    presentInternalWindow(m_patternWindow);
}

void MainWindow::setEditTool(int index) {
    // Route through the transport chip so its label follows and the timeline is
    // updated via the existing toolChanged signal.
    if (m_transport) m_transport->setToolIndex(index);
}

void MainWindow::buildStatusBar() {
    m_statusLeft = new QLabel(this);
    m_statusRight = new QLabel(this);
    statusBar()->addWidget(m_statusLeft);
    statusBar()->addPermanentWidget(m_statusRight);
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration) {
        m_sessionStatus = new collab::SessionStatusWidget(m_collaboration, this);
        statusBar()->addPermanentWidget(m_sessionStatus);
    }
#endif
    statusBar()->setSizeGripEnabled(false);
    // The transport now owns the useful live status. Keep the QStatusBar
    // object for existing transient notifications, but remove its permanent
    // bottom strip from the workspace.
    statusBar()->hide();
}

bool MainWindow::checkProcessingCommandsForTest() const {
    bool bounceFound = false;
    bool offlineFound = false;
    for (const ShortcutManager::Command& command : m_shortcuts->commands()) {
        if (command.id == QLatin1String("timeline.bounce_in_place")) {
            bounceFound = command.action == m_bounceInPlaceAction &&
                          m_shortcuts->shortcut(command.id) ==
                              QKeySequence(QStringLiteral("Ctrl+Alt+C"));
        } else if (command.id == QLatin1String("timeline.offline_render")) {
            offlineFound = command.action == m_offlineRenderAction;
        }
    }
    return bounceFound && offlineFound;
}

void MainWindow::publishSessionTransport(bool force) {
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_collaboration || m_applyingRemoteTransport ||
        m_collaboration->localSessionState()->transportMode() !=
            collab::TransportMode::Independent)
        return;
    const bool playing = m_controller.isPlaying();
    if (!force && !playing && m_transportBroadcastInitialized &&
        !m_lastTransportBroadcastPlaying)
        return;
    collab::TransportFrame frame;
    frame.positionSeconds = m_controller.presentationPositionSeconds();
    frame.playing = playing;
    frame.recording = m_controller.isRecording();
    frame.sentAtMs = QDateTime::currentMSecsSinceEpoch();
    m_collaboration->sendTransport(frame);
    m_transportBroadcastInitialized = true;
    m_lastTransportBroadcastPlaying = playing;
#else
    Q_UNUSED(force)
#endif
}

void MainWindow::applySessionTransport(const collab::TransportFrame& frame) {
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_collaboration || m_controller.isRecording()) return;
    QScopedValueRollback<bool> applying(m_applyingRemoteTransport, true);
    double position = std::max(0.0, frame.positionSeconds);
    if (frame.playing && frame.sentAtMs > 0) {
        const qint64 ageMs = std::clamp<qint64>(
            QDateTime::currentMSecsSinceEpoch() - frame.sentAtMs, 0, 1000);
        position += ageMs / 1000.0;
    }
    if (std::abs(m_controller.presentationPositionSeconds() - position) > 0.025)
        m_controller.seekSeconds(position);
    if (frame.playing && !m_controller.isPlaying()) m_controller.play();
    else if (!frame.playing && m_controller.isPlaying()) m_controller.pause();
    m_transportBroadcastInitialized = true;
    m_lastTransportBroadcastPlaying = frame.playing;
    syncPlayheadTimer();
    if (m_transport) m_transport->refresh();
    if (m_timeline) m_timeline->refreshPlaybackFrame();
#else
    Q_UNUSED(frame)
#endif
}

void MainWindow::updateWindowTitle() {
    const QString name = displayProjectName(m_controller.projectName());
    QString cloudState;
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty()) cloudState = tr(" [Cloud]");
    else if (m_publicationUi && m_publicationUi->active)
        cloudState = tr(" [Publishing]");
#endif
    setWindowTitle(QString("%1%2%3 — %4")
                       .arg(name)
                       .arg(m_dirty ? "*" : "")
                       .arg(cloudState)
                       .arg(QApplication::applicationDisplayName()));
    // Every path change runs through here, which makes it the one place the
    // assistant's copy of it can be kept honest without three separate hooks.
    if (m_aiPanel) m_aiPanel->setProjectPath(m_projectPath);
}

// ── Transport ──

void MainWindow::onPlayPause() {
    // A focused web page owns Space (media controls, form buttons, scrolling).
    // Application-wide DAW shortcuts must not fire through the embedded browser.
    if (m_webPanel && m_webPanel->ownsFocus()) return;
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration)
        m_collaboration->localSessionState()->noteLocalTransportInteraction();
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::Acquiring) {
        cancelPendingCloudRecording(tr("Recording request cancelled"));
        publishSessionTransport(true);
        return;
    }
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        stopCloudRecordingNow();
        return;
    }
#endif

    // Space never *starts* a take — R does, and one key starting a recording is
    // enough. It does stop one, because leaving the recorder running under a
    // paused transport is worse than any argument about which key owns record.
    if (m_controller.isCountingIn()) {
        cancelCountIn();
        publishSessionTransport(true);
        return;
    }
    if (m_controller.isRecording()) {
        stopRecordingNow();
        m_controller.pause();
        syncPlayheadTimer();
        m_playRangeStart = -1.0;
        m_playRangeEnd = -1.0;
        publishSessionTransport(true);
        return;
    }
    if (m_controller.isPlaying()) {
        m_controller.pause();
        syncPlayheadTimer();
        m_playRangeStart = -1.0;
        m_playRangeEnd = -1.0;
        publishSessionTransport(true);
        return;
    }
    m_playRangeStart = -1.0;
    m_playRangeEnd = -1.0;
    // "From clip": play the selected clip from its (trimmed) beginning and loop
    // it — when it ends it starts again from the top instead of running on past
    // the clip. Without a selection the switch behaves as if it were off (so
    // Restart / Resume decide as usual).
    if (m_playFromClip) {
        const double start = m_timeline->selectedClipStartSeconds();
        if (start >= 0.0) {
            m_controller.seekSeconds(start);
            const double end = m_timeline->selectedClipEndSeconds();
            if (end > start) {
                m_playRangeStart = start;
                m_playRangeEnd = end;
            }
        }
    }
    m_controller.play();
    syncPlayheadTimer();
    publishSessionTransport(true);
}
void MainWindow::onStop() {
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration)
        m_collaboration->localSessionState()->noteLocalTransportInteraction();
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::Acquiring) {
        cancelPendingCloudRecording(tr("Recording request cancelled"));
    } else if (m_cloudRecording &&
               m_cloudRecording->phase ==
                   CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        stopCloudRecordingNow();
    }
#endif
    // Stop lands the take, but leaves Record engaged — the light is the user's
    // to switch off, and nothing here is a decision to stop recording for good.
    cancelCountIn();
    if (m_controller.isRecording()) stopRecordingNow();
    m_controller.stop();
    m_playRangeStart = -1.0;
    m_playRangeEnd = -1.0;
    m_controller.seekSeconds(0);
    syncPlayheadTimer();
    publishSessionTransport(true);
}

bool MainWindow::routeEditChord(EditChord chord) {
    if (m_webPanel && m_webPanel->ownsFocus()) {
        switch (chord) {
            case EditChord::Cut:
                m_webPanel->handleEditCommand(WebBrowserPanel::EditCommand::Cut);
                break;
            case EditChord::Copy:
                m_webPanel->handleEditCommand(WebBrowserPanel::EditCommand::Copy);
                break;
            case EditChord::Paste:
                m_webPanel->handleEditCommand(WebBrowserPanel::EditCommand::Paste);
                break;
            case EditChord::Repeat:
            case EditChord::Mute:
            case EditChord::Delete:
                break;
        }
        return true;
    }

    // A text field owns these chords before any editor does, and it never sees
    // them for the same reason the piano roll does not: the menu bar took the
    // key first. So the field is served here, by hand.
    QWidget* focus = m_editChordRouteWindowForTest
                         ? m_editChordFocusWidgetForTest.data()
                         : QApplication::focusWidget();
    QLineEdit* line = qobject_cast<QLineEdit*>(focus);
    if (auto* box = qobject_cast<QAbstractSpinBox*>(focus)) {
        line = box->findChild<QLineEdit*>();
    }
    auto* plain = qobject_cast<QPlainTextEdit*>(focus);
    auto* rich = qobject_cast<QTextEdit*>(focus);
    if (line || plain || rich) {
        switch (chord) {
            case EditChord::Cut:
                if (line) line->cut();
                else if (plain) plain->cut();
                else rich->cut();
                break;
            case EditChord::Copy:
                if (line) line->copy();
                else if (plain) plain->copy();
                else rich->copy();
                break;
            case EditChord::Paste:
                if (line) line->paste();
                else if (plain) plain->paste();
                else rich->paste();
                break;
            case EditChord::Repeat:
            case EditChord::Mute:
            case EditChord::Delete:
                // Nothing sensible to do to text, and everything to lose in the
                // project behind it — so it is swallowed rather than guessed at.
                break;
        }
        return true;
    }

    QWidget* active = m_editChordRouteWindowForTest
                          ? m_editChordRouteWindowForTest.data()
                          : QApplication::activeWindow();
    const bool pianoRollOwnsChord = m_pianoRoll &&
        ((m_editChordRouteWindowForTest && active == m_pianoRoll) ||
         (!m_editChordRouteWindowForTest && m_pianoRollFrame &&
          m_pianoRollFrame->isEditorActive()));
    if (pianoRollOwnsChord) {
        switch (chord) {
            case EditChord::Cut: m_pianoRoll->cutNotes(); break;
            case EditChord::Copy: m_pianoRoll->copyNotes(); break;
            case EditChord::Paste: m_pianoRoll->pasteNotes(); break;
            case EditChord::Repeat: m_pianoRoll->repeatNotes(); break;
            case EditChord::Delete: m_pianoRoll->deleteNotes(); break;
            case EditChord::Mute:
                // The roll has no clip-mute of its own; muting notes is a
                // different command with a different key, so this one simply
                // does not apply here.
                break;
        }
        return true;
    }

    // Some other editor of ours — a plugin, the curve editor, the settings.
    // It is now a child of MainWindow, so QApplication::activeWindow() alone
    // can no longer distinguish it from the arrangement.
    if (hasActiveInternalWindow()) return true;

    // The arrangement is in front — nothing to route, act as always.
    if (!active || active == this) return false;

    // A remaining native modal/system window also owns the chord. Whatever it
    // means there, it does not mean "cut the clips behind it".
    return true;
}

InternalEditorFrame* MainWindow::hostInternalWindow(
    QWidget* content, const QString& settingsKey) {
    if (!content || !m_editorHost) return nullptr;
    if (InternalEditorFrame* existing = m_internalEditorFrames.value(content))
        return existing;

    auto* frame = new InternalEditorFrame(settingsKey, m_editorHost);
    frame->setContent(content);
    m_internalEditorFrames.insert(content, frame);
    content->setProperty("dawInternalEditorContent", true);
    content->installEventFilter(this);

    const QPointer<QWidget> guardedContent(content);
    const QPointer<InternalEditorFrame> guardedFrame(frame);
    connect(frame, &InternalEditorFrame::closeRequested, this,
            [guardedContent, guardedFrame] {
                if (guardedContent) guardedContent->close();
                if (guardedFrame) guardedFrame->hide();
            });
    connect(content, &QObject::destroyed, this,
            [this, content, guardedFrame] {
                m_internalEditorFrames.remove(content);
                if (guardedFrame) guardedFrame->deleteLater();
            });
    return frame;
}

void MainWindow::presentInternalWindow(QWidget* content) {
    if (!content) return;
    InternalEditorFrame* frame = m_internalEditorFrames.value(content, nullptr);
    if (!frame) return;
    content->show();
    frame->present();
}

void MainWindow::hideInternalWindow(QWidget* content) {
    if (content) content->hide();
    if (InternalEditorFrame* frame = m_internalEditorFrames.value(content, nullptr))
        frame->hide();
}

void MainWindow::closeInternalWindows() {
    const QList<QWidget*> contents = m_internalEditorFrames.keys();
    for (QWidget* content : contents) {
        const QPointer<QWidget> guarded(content);
        const QPointer<InternalEditorFrame> frame =
            m_internalEditorFrames.value(content, nullptr);
        if (guarded && guarded->isVisible()) guarded->close();
        if (frame) frame->hide();
    }
}

bool MainWindow::hasActiveInternalWindow() const {
    for (auto it = m_internalEditorFrames.cbegin();
         it != m_internalEditorFrames.cend(); ++it) {
        if (it.value() && it.value()->isEditorActive()) return true;
    }
    return false;
}

void MainWindow::registerAuxiliaryWindow(QWidget* window) {
    if (!window) return;
    for (const QPointer<QWidget>& registered : m_auxiliaryWindows) {
        if (registered == window) return;
    }
    window->setProperty("dawAuxiliaryWindow", true);
    // A QWidget window with a QWidget parent is normally made a native
    // transient of that parent. Several desktop window managers then force it
    // above the parent, which makes lower() ineffective. Keep QObject
    // ownership (and therefore deterministic destruction), but detach only
    // the native stacking relationship so a timeline click can genuinely put
    // the editor behind the main window.
    window->winId();
    if (!window->property("dawKeepTransientParent").toBool()) {
        if (QWindow* handle = window->windowHandle())
            handle->setTransientParent(nullptr);
    }
    m_auxiliaryWindows.push_back(QPointer<QWidget>(window));
}

void MainWindow::presentAuxiliaryWindow(QWidget* window) {
    if (!window) return;
    registerAuxiliaryWindow(window);
    // Most recently requested wins among several editors, while every visible
    // one still remains above the main shell as a family.
    for (int i = m_auxiliaryWindows.size() - 1; i >= 0; --i) {
        if (m_auxiliaryWindows[i] == window) m_auxiliaryWindows.removeAt(i);
    }
    m_auxiliaryWindows.push_back(QPointer<QWidget>(window));
    m_auxiliaryLowered = false;
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::raiseAuxiliaryWindows() {
    if (m_auxiliaryLowered) return;
    for (int i = m_auxiliaryWindows.size() - 1; i >= 0; --i) {
        if (m_auxiliaryWindows[i].isNull()) m_auxiliaryWindows.removeAt(i);
    }
    // Raising in registration order preserves the active editor as the last
    // (topmost) one without activating it and stealing focus from Play, Stop,
    // or another control the user just clicked on the main window.
    for (const QPointer<QWidget>& window : m_auxiliaryWindows) {
        if (window && window->isVisible()) window->raise();
    }
}

void MainWindow::lowerAuxiliaryWindowsForWorkspace() {
    m_auxiliaryLowered = true;
    for (const QPointer<QWidget>& window : m_auxiliaryWindows) {
        if (window && window->isVisible()) window->lower();
    }
    // Window managers apply activation after delivering the press. Repeating
    // the order once the event unwinds makes the result deterministic without
    // hiding or destroying any editor state.
    QTimer::singleShot(0, this, [this] {
        if (!m_auxiliaryLowered) return;   // a MIDI double-click reopened one
        raise();
        for (const QPointer<QWidget>& window : m_auxiliaryWindows) {
            if (window && window->isVisible()) window->lower();
        }
    });
}

void MainWindow::closeAuxiliaryWindows() {
    // Closing an editor can remove or delete it, so iterate over a stable copy
    // of guarded pointers rather than the registration list itself.
    const QList<QPointer<QWidget>> windows = m_auxiliaryWindows;
    for (const QPointer<QWidget>& window : windows) {
        if (window && window->isVisible()) window->close();
    }
    m_auxiliaryWindows.clear();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* ev) {
    switch (ev->type()) {
        case QEvent::Hide: {
            auto* content = qobject_cast<QWidget*>(watched);
            if (!content ||
                !content->property("dawInternalEditorContent").toBool()) {
                break;
            }
            if (InternalEditorFrame* frame =
                    m_internalEditorFrames.value(content, nullptr)) {
                frame->hide();
            }
            break;
        }
        case QEvent::WindowActivate: {
            auto* activated = qobject_cast<QWidget*>(watched);
            if (activated == this && !m_auxiliaryLowered &&
                !m_auxiliaryRaiseQueued) {
                m_auxiliaryRaiseQueued = true;
                QTimer::singleShot(0, this, [this] {
                    m_auxiliaryRaiseQueued = false;
                    raiseAuxiliaryWindows();
                });
                break;
            }
            if (!activated || !activated->isWindow() ||
                !activated->property("dawAuxiliaryWindow").toBool()) {
                break;
            }
            for (int i = m_auxiliaryWindows.size() - 1; i >= 0; --i) {
                if (m_auxiliaryWindows[i] == activated)
                    m_auxiliaryWindows.removeAt(i);
            }
            m_auxiliaryWindows.push_back(QPointer<QWidget>(activated));
            m_auxiliaryLowered = false;
            break;
        }
        case QEvent::MouseButtonPress: {
            auto* target = qobject_cast<QWidget*>(watched);
            if (!target || target->window() != this) break;
            const bool onTimeline =
                m_timeline &&
                (target == m_timeline || m_timeline->isAncestorOf(target));
            const bool onTrackList =
                m_trackList &&
                (target == m_trackList || m_trackList->isAncestorOf(target));
            if (onTimeline || onTrackList) {
                lowerAuxiliaryWindowsForWorkspace();
            } else if (!m_auxiliaryLowered && !m_auxiliaryRaiseQueued) {
                m_auxiliaryRaiseQueued = true;
                QTimer::singleShot(0, this, [this] {
                    m_auxiliaryRaiseQueued = false;
                    raiseAuxiliaryWindows();
                });
            }
            break;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto* key = static_cast<QKeyEvent*>(ev);
            if (key->isAutoRepeat()) break;
            const bool down = ev->type() == QEvent::KeyPress;
            // Modifier feedback must work even while a text field or the web
            // panel owns focus. The focused widget still receives Alt/Option
            // normally; the arrangement only mirrors its held state so its
            // secondary cursor changes before the next mouse event.
            if (key->key() == Qt::Key_Alt) {
                setAutomationModifierHeld(down);
                if (m_timeline) m_timeline->setSecondaryToolHeld(down);
            }
            if (m_webPanel && m_webPanel->ownsFocus()) break;
            // Never while typing: a name field would swallow half its letters.
            if (auto* focus = QApplication::focusWidget();
                qobject_cast<QLineEdit*>(focus) ||
                qobject_cast<QAbstractSpinBox*>(focus) ||
                qobject_cast<QPlainTextEdit*>(focus) ||
                qobject_cast<QTextEdit*>(focus)) {
                break;
            }
            const Qt::KeyboardModifiers modifiers =
                key->modifiers() & ~Qt::KeypadModifier;
            if (down && modifiers == Qt::NoModifier &&
                (key->key() == Qt::Key_S || key->key() == Qt::Key_M) &&
                toggleSelectedTrackState(key->key() == Qt::Key_S)) {
                return true;
            }
            // T, not A: A opens a track's automation lanes, which is a thing
            // reached far more often than auditioning one layer of a comp.
            if (key->key() == Qt::Key_T && !(key->modifiers() & ~Qt::KeypadModifier)) {
                setAuditionHeld(down);
            } else if (key->key() == Qt::Key_L &&
                       (key->modifiers() & Qt::ShiftModifier) &&
                       (key->modifiers() & Qt::ControlModifier)) {
                setLayerInvertHeld(down);
            } else if (!down && key->key() == Qt::Key_Control) {
                // Letting go of the modifier ends the invert even if L is still
                // down — Qt stops reporting the combination at that point.
                setLayerInvertHeld(false);
            }
            break;
        }
        case QEvent::WindowDeactivate:
        case QEvent::ApplicationDeactivate:
            // A key released while another window had focus never reaches us,
            // so drop both gestures rather than leave one stuck on.
            setAuditionHeld(false);
            setLayerInvertHeld(false);
            setAutomationModifierHeld(false);
            if (m_timeline) m_timeline->setSecondaryToolHeld(false);
            break;
        default:
            break;
    }
    return QMainWindow::eventFilter(watched, ev);
}

void MainWindow::setAutomationModifierHeld(bool held) {
    if (m_automationModifierHeld == held) return;
    m_automationModifierHeld = held;
    updateAutomationCreationMode();
}

void MainWindow::updateAutomationCreationMode() {
    const bool active = m_automationCreationLatched || m_automationModifierHeld;
    ui::setAutomationCreationMode(active);
    if (m_toolPanel) m_toolPanel->setAutomationCreationActive(active);
}

void MainWindow::setAuditionHeld(bool held) {
    if (m_auditionHeld == held) return;
    m_auditionHeld = held;
    m_timeline->setAuditionHeld(held);
    statusBar()->showMessage(held ? tr("Audition: click a take layer to hear it "
                                      "while T is held")
                                  : QString(),
                             held ? 3000 : 1);
}

void MainWindow::setLayerInvertHeld(bool held) {
    if (m_layerInvertHeld == held) return;
    m_layerInvertHeld = held;
    m_controller.setRecordModeInverted(held);
    if (!held) {
        statusBar()->clearMessage();
        return;
    }
    const bool layered =
        m_controller.recordMode() == daw::RecordMode::Overwrite;
    statusBar()->showMessage(layered ? tr("This take: layer recording")
                                     : tr("This take: overwrite"),
                             3000);
}

std::vector<std::string> MainWindow::recordTargets() const {
    const auto& project = m_controller.project();
    std::vector<std::string> ids;
    const auto add = [&](const std::string& id) {
        const auto* track = project.findTrack(id);
        if (!track) return;
        if (track->kind == daw::TrackKind::Folder ||
            track->kind == daw::TrackKind::Master) {
            return;
        }
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };

    // A pinned track outranks everything. Pressing R says "record here", and it
    // has to keep meaning that while another track is selected to be looked at
    // — otherwise the take follows the pointer around the arrangement.
    if (!m_recordPins.isEmpty()) {
        for (const QString& id : m_recordPins) add(id.toStdString());
        if (!ids.empty()) return ids;
    }

    // With nothing pinned, the selection is the target — the whole of it.
    for (const QString& id : m_selection.tracks()) add(id.toStdString());
    // …and whatever the header column has, which is the same set unless a clip
    // click has since replaced the track selection with a clip one. Selecting a
    // clip re-points the header at that clip's track, so this never widens the
    // target beyond what the user last pointed at.
    if (m_trackList) {
        for (const QString& id : m_trackList->selectedTrackIds())
            add(id.toStdString());
    }
    if (ids.empty() && !m_selectedTrackId.isEmpty()) {
        add(m_selectedTrackId.toStdString());
    }
    if (ids.empty()) {
        for (const auto& track : project.tracks) {
            if (track.kind == daw::TrackKind::Audio ||
                track.kind == daw::TrackKind::Instrument) {
                add(track.id);
                break;
            }
        }
    }
    return ids;
}

void MainWindow::setRecordEngaged(bool engaged) {
    if (m_recordEngaged == engaged) return;
    if (!engaged) cancelCountIn();
    m_recordEngaged = engaged;
    m_transport->setRecordEngaged(engaged);
    // The panel hands itself over to the recording options while engaged; that
    // is where the count-in and the start button live.
    if (m_contextPanel) {
        m_contextPanel->setRecordEngaged(engaged);
        layoutContextPanel();
    }
    // The header rows grow (or lose) their record chip with this: there is no
    // point offering one when there is no recording to be had.
    refreshRecordChips();
}

void MainWindow::startRecordingSelection() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty()) {
        statusBar()->showMessage(
            tr("Cloud recording is disabled in this version. Make a local copy to record."),
            7000);
        return;
    }
#endif
    if (m_controller.isCountingIn() || m_controller.isRecording()) return;
    const std::vector<std::string> targets = recordTargets();
    if (targets.empty()) {
        statusBar()->showMessage(tr("Select a track to record onto"), 2000);
        return;
    }

    const int beats = std::max(0, m_controller.recordingPrefs().countInBeats);
    if (beats > 0) {
        if (!m_controller.armCountIn(targets, beats)) {
            statusBar()->showMessage(
                tr("Could not start recording — check the audio input in "
                   "Preferences"),
                3000);
            return;
        }
        setRecordEngaged(true);
        statusBar()->showMessage(tr("Counting in… %1").arg(beats), 2000);
        // Its own timer, ten times finer than the UI refresh: the count decides
        // where the take begins, so a beat landing a frame late is audible.
        m_countInClock.start();
        m_countInTimer->start();
        if (m_contextPanel) m_contextPanel->refresh();
        m_timeline->update();
        return;
    }

    if (!m_controller.startRecordingTracks(targets)) {
        statusBar()->showMessage(
            tr("Could not start recording — check the audio input in "
               "Preferences"),
            3000);
        return;
    }
    announceRecordingStarted(targets);
    syncPlayheadTimer();
}

void MainWindow::cancelCountIn() {
#ifdef DAW_ENABLE_COLLABORATION
    if (m_cloudRecording &&
        (m_cloudRecording->phase ==
             CloudRecordingRuntime::Phase::Acquiring ||
         m_cloudRecording->phase ==
             CloudRecordingRuntime::Phase::CountingIn)) {
        cancelPendingCloudRecording(tr("Count-in cancelled"));
        return;
    }
#endif
    if (!m_controller.isCountingIn()) return;
    m_countInTimer->stop();
    m_controller.cancelCountIn();
    statusBar()->showMessage(tr("Count-in cancelled"), 2000);
    if (m_contextPanel) m_contextPanel->refresh();
    m_timeline->update();
}

void MainWindow::tickCountIn() {
#ifdef DAW_ENABLE_COLLABORATION
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::CountingIn &&
        (!cloudRecordingContextIsWritable(m_cloudRecording->trackIds) ||
         m_cloudRecording->projectId != m_cloudProjectId ||
         !m_cloudSessionLifecycle ||
         m_cloudRecording->sessionId !=
             m_cloudSessionLifecycle->sessionId())) {
        cancelPendingCloudRecording(
            tr("Recording cancelled because the cloud session changed"));
        return;
    }
#endif
    if (!m_controller.isCountingIn()) {
        m_countInTimer->stop();
#ifdef DAW_ENABLE_COLLABORATION
        if (m_cloudRecording &&
            m_cloudRecording->phase ==
                CloudRecordingRuntime::Phase::CountingIn) {
            cancelPendingCloudRecording(
                tr("Could not start recording — check the audio input in Preferences"));
        }
#endif
        return;
    }
    const int before = m_controller.countInBeatsRemaining();
    const bool started =
        m_controller.tickCountIn(double(m_countInClock.restart()) / 1000.0);
    if (!started) {
        // Only on the beat: repainting the timeline at the tick rate would cost
        // more than the countdown is worth.
        if (m_controller.countInBeatsRemaining() != before) m_timeline->update();
        return;
    }

    m_countInTimer->stop();
    if (!m_controller.isRecording()) {
#ifdef DAW_ENABLE_COLLABORATION
        if (m_cloudRecording &&
            m_cloudRecording->phase ==
                CloudRecordingRuntime::Phase::CountingIn) {
            cancelPendingCloudRecording(
                tr("Could not start recording — check the audio input in Preferences"));
            return;
        }
#endif
        statusBar()->showMessage(
            tr("Could not start recording — check the audio input in "
               "Preferences"),
            3000);
        if (m_contextPanel) m_contextPanel->refresh();
        return;
    }
#ifdef DAW_ENABLE_COLLABORATION
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::CountingIn) {
        m_cloudRecording->phase = CloudRecordingRuntime::Phase::Capturing;
    }
#endif
    announceRecordingStarted(m_controller.recordingTracks());
    syncPlayheadTimer();
}

void MainWindow::announceRecordingStarted(
    const std::vector<std::string>& targets) {
    setRecordEngaged(true);
    if (m_contextPanel) m_contextPanel->refresh();

    QStringList names;
    for (const std::string& id : targets) {
        if (const auto* track = m_controller.project().findTrack(id)) {
            names << QString::fromStdString(track->name);
        }
    }
    const bool layered = m_controller.effectiveRecordMode(targets.front()) ==
                         daw::RecordMode::Layers;
    statusBar()->showMessage(layered
                                 ? tr("Recording layers onto %1").arg(names.join(", "))
                                 : tr("Recording onto %1").arg(names.join(", ")),
                             3000);
    // Arming and smart monitoring both changed track state, so the headers and
    // the mixer have to catch up.
    syncViews();
}

#ifdef DAW_ENABLE_COLLABORATION
void MainWindow::stopCloudRecordingNow(bool interactiveError) {
    if (!m_cloudRecording) return;
    if (m_cloudRecording->phase ==
        CloudRecordingRuntime::Phase::Capturing) {
        m_cloudRecording->finalized =
            m_controller.finalizeRecordingCapture();
        m_cloudRecording->phase =
            CloudRecordingRuntime::Phase::FinalizedWriteFailed;
        m_countInTimer->stop();
        syncPlayheadTimer();
        if (m_contextPanel) m_contextPanel->refresh();
        syncViews();
    }
    if (m_cloudRecording->phase !=
        CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        return;
    }

    auto releaseAndClear = [this] {
        if (m_recordingLeases) {
            if (m_recordingLeases->state() ==
                collab::RecordingLeaseState::Lost)
                m_recordingLeases->resetAfterLoss();
            else
                m_recordingLeases->releaseAll();
        }
        m_cloudRecording.reset();
        if (m_contextPanel) m_contextPanel->refresh();
        syncViews();
    };

    std::vector<daw::recovery::CloudRecordingCapture> captures;
    captures.reserve(m_cloudRecording->finalized.tracks.size());
    bool missingCaptureIdentity = false;
    bool missingCaptureFile = false;
    for (const auto& track : m_cloudRecording->finalized.tracks) {
        const QString trackId = QString::fromStdString(track.trackId);
        const QString leaseId =
            m_cloudRecording->leaseIdByTrack.value(trackId);
        const QString captureId =
            m_cloudRecording->captureIdByTrack.value(trackId);
        const QString uploadId =
            m_cloudRecording->uploadIdByTrack.value(trackId);
        const QString assetId =
            m_cloudRecording->assetIdByTrack.value(trackId);
        if (canonicalCloudUuid(trackId) != trackId ||
            canonicalCloudUuid(leaseId) != leaseId ||
            canonicalCloudUuid(captureId) != captureId ||
            canonicalCloudUuid(uploadId) != uploadId ||
            canonicalCloudUuid(assetId) != assetId) {
            missingCaptureIdentity = true;
            continue;
        }
        const QFileInfo wav(QString::fromStdString(track.closedWavPath));
        if (!wav.isAbsolute() || !wav.exists() || !wav.isFile() ||
            wav.isSymLink()) {
            missingCaptureFile = true;
            continue;
        }

        daw::recovery::CloudRecordingCapture capture;
        capture.captureId = captureId.toStdString();
        capture.trackId = track.trackId;
        capture.leaseId = leaseId.toStdString();
        capture.uploadId = uploadId.toStdString();
        capture.assetId = assetId.toStdString();
        capture.localWavPath = track.closedWavPath;
        capture.startSeconds = track.startSeconds;
        capture.durationSeconds = track.durationSeconds;
        capture.sampleRate = track.sampleRate;
        capture.channels = track.channels;
        capture.frames = track.frames;
        capture.status = daw::recovery::classifyCloudRecordingCaptureStatus(
            track.fileWriteSucceeded, track.capturedFrames,
            track.writtenFrames, track.droppedFrames,
            track.audioReadable, track.frames);
        capture.semantics.mode =
            track.semantics.mode == daw::RecordMode::Layers
                ? daw::recovery::CloudRecordingMode::Layers
                : daw::recovery::CloudRecordingMode::Overwrite;
        capture.semantics.loopEnabled = track.semantics.loopEnabled;
        capture.semantics.loopStartSeconds =
            track.semantics.loopStartSeconds;
        capture.semantics.loopEndSeconds = track.semantics.loopEndSeconds;
        capture.semantics.loopCreatesTakes =
            track.semantics.loopCreatesTakes;
        capture.semantics.trimTakesToRegion =
            track.semantics.trimTakesToRegion;
        capture.semantics.autoExpandAfterRecord =
            track.semantics.autoExpandAfterRecord;
        capture.semantics.compCrossfadeMs =
            track.semantics.compCrossfadeMs;
        // V2 permits a failed writer to retain the geometry of a readable
        // prefix for repair/recovery. Unreadable and clean zero-frame files
        // deliberately carry no passes under the closed schema.
        if (capture.status ==
                daw::recovery::CloudRecordingCaptureStatus::Ready ||
            capture.status ==
                daw::recovery::CloudRecordingCaptureStatus::WriteFailed) {
            capture.passes.reserve(track.passes.size());
            for (const daw::RecordingSpan& pass : track.passes) {
                capture.passes.push_back(
                    {pass.startSeconds, pass.endSeconds,
                     pass.captureOffsetSeconds});
            }
        }
        captures.push_back(std::move(capture));
    }

    if (missingCaptureIdentity || missingCaptureFile) {
        const QString message = tr(
            "The finished take could not be matched to every cloud track reservation or closed WAV. Its audio remains local; press R or Stop to retry recovery.");
        statusBar()->showMessage(message, 9000);
        if (interactiveError)
            QMessageBox::critical(this, tr("Recovery Identity Failed"),
                                  message);
        return;
    }

    // No recorder/file identity exists in this exceptional case. Zero-frame,
    // short and unreadable *existing* WAVs were retained above as explicit v2
    // capture states and must never take this cleanup path.
    if (captures.empty()) {
        releaseAndClear();
        const QString message = tr(
            "Recording stopped, but no closed audio file was produced. Track reservations were released.");
        statusBar()->showMessage(message, 8000);
        if (interactiveError)
            QMessageBox::warning(this, tr("Recording Was Not Saved"), message);
        return;
    }

    if (!ensureCloudRecordingRecoverySession()) {
        const QString message = tr(
            "The finished take could not be written to durable recovery storage. Press R or Stop to retry before leaving the project.");
        statusBar()->showMessage(message, 9000);
        if (interactiveError)
            QMessageBox::critical(this, tr("Recovery Write Failed"), message);
        return;
    }

    daw::recovery::CloudRecordingRecoveryStore store(
        m_journal.sessionDir());
    daw::recovery::CloudRecordingRecoveryRun recoveryRun;
    recoveryRun.runId = m_cloudRecording->runId.toStdString();
    recoveryRun.opId = m_cloudRecording->opId.toStdString();
    recoveryRun.transactionId =
        m_cloudRecording->transactionId.toStdString();
    recoveryRun.createdAtUnixMs = m_cloudRecording->createdAtUnixMs;
    recoveryRun.recoveryOnly = m_cloudRecording->recoveryOnly;
    recoveryRun.lostLease = m_cloudRecording->lostLease;
    recoveryRun.captures = std::move(captures);

    daw::recovery::CloudRecordingRecoveryManifest manifest;
    if (store.exists()) {
        const auto read = store.read(manifest);
        if (!read ||
            manifest.projectId !=
                m_cloudRecording->projectId.toStdString() ||
            manifest.sessionId !=
                m_cloudRecording->sessionId.toStdString()) {
            const QString message = tr(
                "The existing cloud recording recovery data could not be safely merged. Press R or Stop to retry; the finished audio remains local.");
            m_preserveCloudRecoverySession = true;
            statusBar()->showMessage(message, 9000);
            if (interactiveError)
                QMessageBox::critical(this, tr("Recovery Merge Failed"),
                                      message);
            return;
        }
    } else {
        manifest.projectId = m_cloudRecording->projectId.toStdString();
        manifest.sessionId = m_cloudRecording->sessionId.toStdString();
        manifest.createdAtUnixMs = recoveryRun.createdAtUnixMs;
    }

    bool mergeConflict = false;
    const auto sameRun = std::find_if(
        manifest.runs.begin(), manifest.runs.end(),
        [&recoveryRun](const auto& existing) {
            return existing.runId == recoveryRun.runId;
        });
    if (sameRun != manifest.runs.end()) {
        mergeConflict = *sameRun != recoveryRun;
    } else {
        for (const auto& run : manifest.runs) {
            for (const auto& existing : run.captures) {
                const auto collision = std::find_if(
                    recoveryRun.captures.begin(),
                    recoveryRun.captures.end(),
                    [&existing](const auto& capture) {
                        return existing.captureId == capture.captureId ||
                               existing.leaseId == capture.leaseId ||
                               existing.uploadId == capture.uploadId ||
                               existing.assetId == capture.assetId ||
                               existing.localWavPath == capture.localWavPath;
                    });
                if (collision != recoveryRun.captures.end()) {
                    mergeConflict = true;
                    break;
                }
            }
            if (mergeConflict) break;
        }
        if (!mergeConflict)
            manifest.runs.push_back(std::move(recoveryRun));
    }
    if (mergeConflict) {
        const QString message = tr(
            "The finished take conflicts with existing recovery metadata. VLT kept both the audio and track reservations for a safe retry.");
        m_preserveCloudRecoverySession = true;
        statusBar()->showMessage(message, 9000);
        if (interactiveError)
            QMessageBox::critical(this, tr("Recovery Merge Failed"), message);
        return;
    }

    const auto written = store.write(manifest);
    if (!written) {
        const QString message = tr(
            "The finished take could not be saved to durable recovery storage. Press R or Stop to retry before leaving the project.");
        m_preserveCloudRecoverySession = store.exists();
        statusBar()->showMessage(message, 9000);
        if (interactiveError)
            QMessageBox::critical(this, tr("Recovery Write Failed"), message);
        return;
    }

    m_preserveCloudRecoverySession = true;
    releaseAndClear();
    statusBar()->showMessage(
        tr("Take saved for recovery; cloud commit is pending"), 8000);
}
#endif

void MainWindow::stopRecordingNow() {
#ifdef DAW_ENABLE_COLLABORATION
    if (m_cloudRecording &&
        (m_cloudRecording->phase ==
             CloudRecordingRuntime::Phase::Capturing ||
         m_cloudRecording->phase ==
             CloudRecordingRuntime::Phase::FinalizedWriteFailed)) {
        stopCloudRecordingNow();
        return;
    }
#endif
    cancelCountIn();
    // Copied, not referenced: stopRecording() clears the controller's list.
    const std::vector<std::string> tracks = m_controller.recordingTracks();
    m_controller.stopRecording();
    syncPlayheadTimer();
    // Record stays engaged: landing a take is not a decision to stop recording,
    // and the next one usually follows straight after with the same settings.
    // Only the Record button itself puts the light out.
    if (m_contextPanel) m_contextPanel->refresh();
    syncViews();
    markDirty();

    if (!m_controller.recordingPrefs().autoExpandAfterRecord) return;
    // The controller has already flagged the clip that gained a take; the lane
    // still has to grow into it, and doing that here means the editor slides
    // open the same way it does when opened by hand.
    for (const std::string& id : tracks) {
        const auto* track = m_controller.project().findTrack(id);
        if (!track) continue;
        for (const auto& clip : track->clips) {
            if (!clip.expanded || clip.takes.size() < 2) continue;
            m_timeline->animateClipOpen(QString::fromStdString(id),
                                        QString::fromStdString(clip.id));
            return;   // one editor: opening every lane at once buries the screen
        }
    }
}

void MainWindow::suppressTypingKeyConflicts(bool suppress) {
    // The registry does the work: it keeps reporting the real binding while the
    // key is lent to the keyboard, so the shortcut editor still shows E on
    // "Expand Take Layers" and still refuses to hand E to anything else.
    m_shortcuts->setKeySuppressor(
        suppress ? std::function<bool(int)>(&TypingKeyboard::usesKey)
                 : std::function<bool(int)>());
}

std::string MainWindow::liveInputTarget() const {
    // The focused piano roll is playing its own track — that is the part being
    // written. Otherwise input follows the selection and finally the first
    // track that can take MIDI.
    QString preferred = m_selection.singleTrack();
    if (preferred.isEmpty() && !m_selection.clips().isEmpty())
        preferred = m_selection.clips().first().trackId;
    if (m_pianoRoll && m_pianoRoll->isVisible() && m_pianoRollFrame &&
        m_pianoRollFrame->isEditorActive() &&
        !m_pianoRoll->trackId().isEmpty()) {
        preferred = m_pianoRoll->trackId();
    }
    return m_controller.liveNoteTarget(preferred.toStdString());
}

std::bitset<128> MainWindow::livePitchesForTrack(
    const QString& trackId) const {
    std::bitset<128> pitches;
    const auto found = m_livePitchCounts.constFind(trackId);
    if (found == m_livePitchCounts.cend()) return pitches;
    for (std::size_t pitch = 0; pitch < found->size(); ++pitch)
        pitches.set(pitch, (*found)[pitch] != 0);
    return pitches;
}

void MainWindow::onLiveNoteStateChanged(const QString& trackId, int pitch,
                                        bool down) {
    if (trackId.isEmpty() || pitch < 0 || pitch > 127) return;
    auto& counts = m_livePitchCounts[trackId];
    unsigned int& count = counts[std::size_t(pitch)];
    if (down) {
        if (count != std::numeric_limits<unsigned int>::max()) ++count;
    } else if (count > 0) {
        --count;
    }

    if (m_pianoRoll && m_pianoRoll->trackId() == trackId)
        m_pianoRoll->setLivePitches(livePitchesForTrack(trackId));
}

void MainWindow::setTypingKeyboardEnabled(bool enabled) {
    if (!m_typingKeyboard || m_typingKeyboard->isEnabled() == enabled) return;
    m_typingKeyboard->setEnabled(enabled);
    suppressTypingKeyConflicts(enabled);
    // Three things say the same state — the button, the menu item and the
    // keyboard — and any of the three can be the one the user touched.
    if (m_typingKeyboardAction && m_typingKeyboardAction->isChecked() != enabled) {
        QSignalBlocker block(m_typingKeyboardAction);
        m_typingKeyboardAction->setChecked(enabled);
    }
    if (m_transport) m_transport->setTypingKeyboardActive(enabled);

    if (enabled) {
        const std::string target = m_controller.liveNoteTarget(
            m_selection.singleTrack().toStdString());
        if (target.empty()) {
            statusBar()->showMessage(
                tr("Typing keyboard on — add a MIDI track to hear it"),
                4000);
        } else {
            statusBar()->showMessage(
                tr("Typing keyboard on — Z…M and Q…P play notes, [ and ] shift "
                   "the octave"),
                4000);
        }
    } else {
        statusBar()->showMessage(tr("Typing keyboard off"), 1500);
    }
}

void MainWindow::onMidiFileImported(const QString& path, double fileTempoBpm,
                                    bool hasTempoChanges) {
    syncViews();
    markDirty();

    const QString name = QFileInfo(path).fileName();
    const double projectTempo = m_controller.tempo();
    const bool differs =
        fileTempoBpm > 0.0 && std::abs(fileTempoBpm - projectTempo) > 0.01;
    if (!differs) {
        statusBar()->showMessage(tr("Imported %1").arg(name), 3000);
        return;
    }

    using Policy = ui::browserprefs::MidiTempo;
    Policy policy = ui::browserprefs::midiTempoPolicy();
    if (policy == Policy::Ask) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Imported %1").arg(name));
        box.setText(tr("The file was written at %1 BPM; the project is at %2 BPM.")
                        .arg(fileTempoBpm, 0, 'f', 2)
                        .arg(projectTempo, 0, 'f', 2));
        box.setInformativeText(
            tr("The notes are stored in beats, so they play either way — this "
               "only sets what a beat is worth."));
        QPushButton* keep =
            box.addButton(tr("Keep %1 BPM").arg(projectTempo, 0, 'f', 2),
                          QMessageBox::RejectRole);
        QPushButton* adopt =
            box.addButton(tr("Use %1 BPM").arg(fileTempoBpm, 0, 'f', 2),
                          QMessageBox::AcceptRole);
        QCheckBox* remember = new QCheckBox(tr("Always do this"), &box);
        box.setCheckBox(remember);
        box.setDefaultButton(keep);
        box.exec();

        policy = box.clickedButton() == adopt ? Policy::Adopt : Policy::Keep;
        if (remember->isChecked()) ui::browserprefs::setMidiTempoPolicy(policy);
    }

    if (policy == Policy::Adopt) {
        m_controller.setTempo(fileTempoBpm);
        m_transport->syncTempo();
        syncViews();
    }
    QString message = policy == Policy::Adopt
                          ? tr("Imported %1 — project tempo is now %2 BPM")
                                .arg(name)
                                .arg(fileTempoBpm, 0, 'f', 2)
                          : tr("Imported %1 — it was written at %2 BPM")
                                .arg(name)
                                .arg(fileTempoBpm, 0, 'f', 2);
    if (hasTempoChanges) {
        message += tr(" (the file changes tempo later; only the first is used)");
    }
    statusBar()->showMessage(message, 6000);
}

void MainWindow::onToggleLayerMode() {
    const auto next = m_controller.recordMode() == daw::RecordMode::Layers
                          ? daw::RecordMode::Overwrite
                          : daw::RecordMode::Layers;
    m_controller.setRecordMode(next);
    RecordingSettingsPage::persistMode(next);
    if (m_contextPanel) m_contextPanel->refresh();
    onRecordModeChanged();
}

void MainWindow::onRecordModeChanged() {
    if (m_settingsWindow) m_settingsWindow->reloadRecordingPage();
    statusBar()->showMessage(
        m_controller.recordMode() == daw::RecordMode::Layers
            ? tr("Layer recording: every take is kept")
            : tr("Overwrite: a new take replaces what was there"),
        2000);
}

void MainWindow::onRecord() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty()) {
        statusBar()->showMessage(
            tr("Cloud recording is disabled in this version. Make a local copy to record."),
            7000);
        return;
    }
    if (m_collaboration)
        m_collaboration->localSessionState()->noteLocalTransportInteraction();
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::Acquiring) {
        cancelPendingCloudRecording(tr("Recording request cancelled"));
        return;
    }
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        stopCloudRecordingNow();
        return;
    }
#endif
    if (m_controller.isCountingIn()) {
        cancelCountIn();
        return;
    }
    if (m_controller.isRecording()) {
        stopRecordingNow();
        return;
    }
    // The button engages; it never starts. Pressing it lights the transport red
    // and hands the context panel over to the recording options, so the count-in
    // and the target tracks can be settled before anything is captured. Starting
    // is a second, deliberate gesture — the panel's button, or R.
    setRecordEngaged(!m_recordEngaged);
    statusBar()->showMessage(
        m_recordEngaged
            ? tr("Record engaged — set up the take, then press R or Start")
            : QString(),
        m_recordEngaged ? 3000 : 1);
}

void MainWindow::onRecordKey() {
    // R is the whole record gesture: it starts, it cancels a count-in it
    // started, and it lands a running take.
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty()) {
        statusBar()->showMessage(
            tr("Cloud recording is disabled in this version. Make a local copy to record."),
            7000);
        return;
    }
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::Acquiring) {
        cancelPendingCloudRecording(tr("Recording request cancelled"));
        return;
    }
    if (m_cloudRecording &&
        m_cloudRecording->phase ==
            CloudRecordingRuntime::Phase::FinalizedWriteFailed) {
        stopCloudRecordingNow();
        return;
    }
#endif
    if (m_controller.isCountingIn()) {
        cancelCountIn();
        return;
    }
    if (m_controller.isRecording()) {
        stopRecordingNow();
        return;
    }
    if (!m_recordEngaged &&
        !m_controller.recordingPrefs().recordKeyArmsAndStarts) {
        statusBar()->showMessage(
            tr("Engage Record on the transport first, or let R do both in "
               "Preferences ▸ Recording"),
            3000);
        return;
    }
    startRecordingSelection();
}
void MainWindow::onReturnToStart() {
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration)
        m_collaboration->localSessionState()->noteLocalTransportInteraction();
#endif
    m_controller.seekSeconds(0);
    publishSessionTransport(true);
}
void MainWindow::onNudge(int bars) {
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaboration)
        m_collaboration->localSessionState()->noteLocalTransportInteraction();
#endif
    const double secondsPerBar = 60.0 / std::max(1.0, m_controller.tempo()) *
                                 std::max(1, m_controller.timeSigNumerator());
    m_controller.seekSeconds(
        std::max(0.0, m_controller.positionSeconds() + bars * secondsPerBar));
    m_timeline->update();
    publishSessionTransport(true);
}
void MainWindow::onToggleLoop(bool on) {
    // A loop with no range would trap the playhead at zero; default to the
    // arrangement length.
    if (on && m_controller.loopEndSeconds() <= m_controller.loopStartSeconds()) {
        const double end = std::max(m_controller.durationSeconds(),
                                    8 * 60.0 / std::max(1.0, m_controller.tempo()));
        m_controller.setLoopRangeSeconds(0.0, end);
    }
    m_controller.setLoopEnabled(on);
    // The region is drawn lit or dim depending on this, in both rulers — and C
    // is an application shortcut, so it is pressed as often from the roll as
    // from the arrangement.
    m_timeline->update();
    if (m_pianoRoll) m_pianoRoll->refresh();
    statusBar()->showMessage(
        on ? tr("Cycle on — the playhead stays inside the region")
           : tr("Cycle off"),
        2000);
    markDirty();
}
void MainWindow::onToggleMetronome(bool on) {
    m_controller.setMetronomeEnabled(on);
    statusBar()->showMessage(on ? tr("Metronome on") : tr("Metronome off"), 1500);
}
void MainWindow::onTempoChanged(double bpm) {
    // A tempo change moves every clip onto the bar it was written against, so
    // it is not only a repaint — it is a document edit, and every view that
    // reads a clip's time geometry has to be told.
    const double previous = m_controller.project().tempo;
    if (previous == bpm) return;
    m_controller.setTempo(bpm);
    // The "play this clip" range is remembered here, in seconds, so it moves
    // with the clip it was taken from instead of pointing into the middle of it.
    if (previous > 0.0 && bpm > 0.0 && m_playRangeEnd >= 0.0) {
        const double ratio = previous / bpm;
        m_playRangeStart *= ratio;
        m_playRangeEnd *= ratio;
    }
    // Tempo changes move clips and grid lines but do not change the track or
    // mixer structure. Rebuilding those widget trees here held the event loop
    // up before the timeline could paint, making the grid visibly lag the BPM
    // field. Refresh only the views whose time geometry/value actually moved.
    if (m_timeline) m_timeline->update();
    if (m_pianoRoll) m_pianoRoll->refresh();
    m_selection.refresh();
    layoutContextPanel();
    markDirty();
}

// ── Tracks ──

void MainWindow::onAddAudioTrack() {
    const int n = int(m_controller.project().tracks.size()) + 1;
    m_controller.addTrack(daw::TrackKind::Audio, tr("Audio %1").arg(n).toStdString());
    syncViews();
    markDirty();
}
void MainWindow::onAddMidiTrack() {
    const int n = int(m_controller.project().tracks.size()) + 1;
    m_controller.addTrack(daw::TrackKind::Midi, tr("MIDI %1").arg(n).toStdString());
    syncViews();
    markDirty();
}
void MainWindow::onAddPatternTrack() {
    const int n = int(std::count_if(
                      m_controller.project().tracks.begin(),
                      m_controller.project().tracks.end(),
                      [](const daw::TrackModel& track) {
                          return track.kind == daw::TrackKind::Pattern;
                      })) + 1;
    const std::string id = m_controller.addPattern(
        tr("Pattern %1").arg(n).toStdString());
    syncViews();
    selectTrackFromHeader(QString::fromStdString(id));
    markDirty();
    openPattern(QString::fromStdString(id));
}
void MainWindow::onAddBusTrack() {
    m_controller.addTrack(daw::TrackKind::Bus, tr("Bus").toStdString());
    syncViews();
    markDirty();
}
void MainWindow::onAddSendTrack() {
    const int n = int(std::count_if(
                      m_controller.project().tracks.begin(),
                      m_controller.project().tracks.end(),
                      [](const daw::TrackModel& track) {
                          return track.kind == daw::TrackKind::Aux;
                      })) + 1;
    m_controller.addTrack(daw::TrackKind::Aux,
                          tr("Send %1").arg(n).toStdString());
    syncViews();
    markDirty();
}
void MainWindow::onDuplicateSelectedTrack() {
    QWidget* focus = QApplication::focusWidget();
    const bool timelineFocused =
        m_timeline && focus &&
        (focus == m_timeline || m_timeline->isAncestorOf(focus));
    if (timelineFocused && m_timeline->duplicateActiveTake()) {
        markDirty();
        return;
    }
    if (timelineFocused && m_timeline->duplicateSelection()) {
        markDirty();
        return;
    }

    const QStringList selection = selectedTrackIds();
    const QString trackId = !m_selectedTrackId.isEmpty()
                                ? m_selectedTrackId
                                : (selection.isEmpty() ? QString{}
                                                       : selection.front());
    if (trackId.isEmpty()) return;
    const auto* track =
        m_controller.project().findTrack(trackId.toStdString());
    if (!track) return;

    std::string copy;
    if (track->kind == daw::TrackKind::Pattern) {
        copy = m_controller.duplicatePattern(trackId.toStdString());
    } else if (!daw::isFolder(*track)) {
        copy = m_controller.duplicateTrack(trackId.toStdString(),
                                           /*withInserts=*/true);
    }
    if (copy.empty()) return;

    const QString copyId = QString::fromStdString(copy);
    syncViews();
    selectTrackFromHeader(copyId);
    markDirty();
}
void MainWindow::onRemoveSelectedTrack() {
    // Delete reaches here from the Track menu, so it carries the same hazard
    // the clipboard chords do: with the piano roll in front it would take the
    // track out from under the notes being edited.
    if (routeEditChord(EditChord::Delete)) return;
    // Delete is contextual: a committed region first, then selected clips,
    // otherwise the selected track.
    if (m_timeline && m_timeline->hasRegionSelection()) {
        m_timeline->deleteRegionSelection();
        markDirty();
        return;
    }
    if (m_timeline && m_timeline->hasClipSelection()) {
        m_timeline->deleteSelectedClips();
        markDirty();
        return;
    }
    if (m_selectedTrackId.isEmpty()) return;
    m_controller.removeTrack(m_selectedTrackId.toStdString());
    m_selectedTrackId.clear();
    syncViews();
    markDirty();
}
QStringList MainWindow::selectedTrackIds() const {
    QStringList ids = m_trackList ? m_trackList->selectedTrackIds() : QStringList();
    if (ids.isEmpty() && !m_selectedTrackId.isEmpty()) ids = {m_selectedTrackId};
    return ids;
}

bool MainWindow::toggleSelectedTrackState(bool solo) {
    std::vector<std::string> targets;
    const auto& project = m_controller.project();
    for (const QString& selectedId : selectedTrackIds()) {
        const auto* selected = project.findTrack(selectedId.toStdString());
        if (!selected) continue;
        const std::string id = daw::isAutomationLane(*selected)
                                   ? selected->parentId
                                   : selected->id;
        if (id.empty() || std::find(targets.begin(), targets.end(), id) !=
                              targets.end()) {
            continue;
        }
        targets.push_back(id);
    }
    if (targets.empty()) return false;

    const bool allSet = std::all_of(
        targets.begin(), targets.end(), [&](const std::string& id) {
            const auto* track = project.findTrack(id);
            return track && (solo ? track->soloed : track->muted);
        });
    const bool next = !allSet;
    bool localFileDirty = true;
    if (solo) {
        for (const std::string& id : targets) {
            if (!project.findTrack(id)) continue;
            m_controller.setTrackSoloed(id, next);
        }
    } else {
        const auto result = m_controller.setTracksMuted(targets, next);
        localFileDirty = daw::collab::marksLocalFileDirty(result);
    }

    if (m_trackList) m_trackList->syncTrackValues();
    if (m_mixer) m_mixer->syncFromModel();
    if (m_inspector) m_inspector->syncFromModel();
    if (m_contextPanel) m_contextPanel->refresh();
    if (m_timeline) m_timeline->update();
    m_selection.refresh();
    if (localFileDirty) markDirty();
    return true;
}

void MainWindow::onNewFolder() {
    const QStringList selection = selectedTrackIds();
    FolderKindDialog dialog(int(selection.size()), this);
    if (dialog.exec() != QDialog::Accepted) return;
    packSelectionIntoFolder(dialog.summing());
}

void MainWindow::packSelectionIntoFolder(bool summing) {
    const QStringList selection = selectedTrackIds();
    const std::string localizedName =
        (summing ? tr("Group") : tr("Folder")).toStdString();
    std::string folder;
    if (selection.isEmpty()) {
        // Nothing selected is a perfectly good request for an empty folder to
        // drag things into later.
        folder = m_controller.addFolder(summing, localizedName);
    } else {
        std::vector<std::string> ids;
        ids.reserve(size_t(selection.size()));
        for (const QString& id : selection) ids.push_back(id.toStdString());
        folder = m_controller.packIntoFolder(ids, localizedName, summing);
    }
    if (folder.empty()) return;

    const QString id = QString::fromStdString(folder);
    m_selectedTrackId = id;
    if (m_trackList) m_trackList->setSelectedTrack(id);
    if (m_timeline) m_timeline->clearClipSelection();
    m_selection.setTracks({id});
    syncStructureViews();
    refreshRecordChips();
    markDirty();
    selectTrackFromHeader(id);
    statusBar()->showMessage(
        summing ? tr("Summing folder created — everything inside it now runs "
                     "through its channel")
                : tr("Folder created — drag other tracks onto it to add them"),
        4000);
}

void MainWindow::zoomFocusedView(double direction) {
    // The browser owns the keys while the pointer's work is in it — which is
    // what "only the browser" means when the same keys also zoom the timeline.
    QWidget* focus = QApplication::focusWidget();
    if (m_browser && m_browser->isVisible() && focus &&
        (focus == m_browser || m_browser->isAncestorOf(focus))) {
        m_browser->zoomBy(direction > 0 ? 1.15 : 1.0 / 1.15);
        return;
    }
    m_timeline->zoomBy(direction > 0 ? 1.3 : 1.0 / 1.3);
}

void MainWindow::toggleAutomationLanes() {
    const QStringList selection = selectedTrackIds();
    if (selection.isEmpty()) return;

    bool opened = false;
    bool changed = false;
    std::set<std::string> owners;
    for (const QString& id : selection) {
        const auto* selected =
            m_controller.project().findTrack(id.toStdString());
        if (!selected) continue;
        const std::string ownerId = daw::isAutomationLane(*selected)
                                        ? selected->parentId
                                        : selected->id;
        const auto* track = m_controller.project().findTrack(ownerId);
        if (!track || !daw::carriesAudio(*track) || !owners.insert(ownerId).second)
            continue;

        if (m_controller.automationLanesOf(ownerId).empty()) {
            // The first press on a track with no lanes makes the one everybody
            // wants first, with a curve already on it to draw into. An empty
            // lane would just be a second thing to ask for.
            daw::AutomationTarget volume;
            volume.kind = daw::AutomationTargetKind::TrackVolume;
            volume.channelId = ownerId;
            const int mark = m_controller.undoDepth();
            const std::string lane =
                m_controller.addAutomationLane(ownerId, volume);
            m_controller.addAutomationClip(lane, volume, 0.0);
            m_controller.collapseUndo(mark, "Show Automation");
            opened = true;
            changed = true;
        } else {
            const bool willOpen = !track->automationExpanded;
            m_controller.setAutomationExpanded(ownerId, willOpen);
            opened = opened || willOpen;
            changed = true;
        }
    }
    if (!changed) return;
    syncViews();
    markDirty();
    statusBar()->showMessage(
        opened ? tr("Automation shown — drag in the lane to draw, Alt-drag a "
                    "segment to curve it")
               : tr("Automation hidden"),
        3000);
}

void MainWindow::setAllAutomationLanesVisible(bool visible) {
    std::vector<std::string> folders;
    std::vector<std::string> channels;
    folders.reserve(m_controller.project().tracks.size());
    channels.reserve(m_controller.project().tracks.size());

    // Snapshot ids first: adding an automation lane grows the project track
    // vector and would invalidate an iterator held across the operation.
    for (const auto& track : m_controller.project().tracks) {
        if (daw::isAutomationLane(track)) continue;
        if (daw::isFolder(track)) folders.push_back(track.id);
        if (daw::carriesAudio(track)) channels.push_back(track.id);
    }
    if (channels.empty()) {
        syncAutomationVisibilityButton();
        statusBar()->showMessage(tr("No tracks can show automation"), 2500);
        return;
    }

    const std::size_t mark = m_controller.undoDepth();
    bool changed = false;
    if (visible) {
        // Reveal nested channels, but keep this separate from the automation
        // disclosure so the reverse command never collapses musical children.
        for (const std::string& id : folders) {
            const auto* track = m_controller.project().findTrack(id);
            if (track && !track->expanded) {
                m_controller.setFolderExpanded(id, true);
                changed = true;
            }
        }
    }
    for (const std::string& id : channels) {
        auto lanes = m_controller.automationLanesOf(id);
        if (visible && lanes.empty()) {
            daw::AutomationTarget volume;
            volume.kind = daw::AutomationTargetKind::TrackVolume;
            volume.channelId = id;
            const std::string lane = m_controller.addAutomationLane(id, volume);
            m_controller.addAutomationClip(lane, volume, 0.0);
            changed = true;
            continue;
        }
        const auto* track = m_controller.project().findTrack(id);
        if (!track || lanes.empty() || track->automationExpanded == visible)
            continue;
        m_controller.setAutomationExpanded(id, visible);
        changed = true;
    }

    if (changed) {
        m_controller.collapseUndo(mark, visible ? "Show All Automation"
                                                : "Hide All Automation");
        syncViews();
        markDirty();
        statusBar()->showMessage(
            visible ? tr("All automation lanes shown — drag in a lane to draw")
                    : tr("All automation lanes hidden"),
            3000);
    } else {
        syncAutomationVisibilityButton();
        statusBar()->showMessage(
            visible ? tr("All automation lanes are already shown")
                    : tr("All automation lanes are already hidden"),
            2200);
    }
}

void MainWindow::syncAutomationVisibilityButton() {
    if (!m_toolPanel) return;
    bool anyVisible = false;
    for (const auto& track : m_controller.project().tracks) {
        if (!track.automationExpanded) continue;
        if (!m_controller.automationLanesOf(track.id).empty()) {
            anyVisible = true;
            break;
        }
    }
    m_toolPanel->setAutomationVisible(anyVisible);
}

void MainWindow::onClearSolos() {
    for (const auto& t : m_controller.project().tracks) {
        if (t.soloed) m_controller.setTrackSoloed(t.id, false);
    }
    syncViews();
    markDirty();
}

void MainWindow::onClearMutes() {
    const auto result = m_controller.clearAllMutes();
    syncViews();
    if (daw::collab::marksLocalFileDirty(result)) markDirty();
}

void MainWindow::onSelectionChanged(const QString& trackId) {
    m_selectedTrackId = trackId;
    m_timeline->setSelectedTrack(trackId);
    // The lanes show the whole selection, not just the row the panels follow.
    m_timeline->setSelectedTracks(m_trackList ? m_trackList->selectedTrackIds()
                                              : QStringList{trackId});
    if (m_inspector)
        m_inspector->setSelection(trackId, m_selection.singleClip().clipId);
    if (m_mixer) m_mixer->setSelectedTrack(trackId);
}

void MainWindow::selectTrackFromHeader(const QString& trackId) {
    // Picking a track is a newer intent than whatever clip was selected before
    // it, so the clip selection goes — the context panel follows the last thing
    // clicked, not whatever is still technically selected somewhere. The
    // timeline drops its clips silently and setTracks does the one model
    // update, so the panel changes over instead of closing and reopening.
    if (m_timeline) m_timeline->clearClipSelection();
    // The whole set, which is what a recording lands on and what a folder is
    // made of. `trackId` is only the primary row within it.
    QStringList tracks = m_trackList ? m_trackList->selectedTrackIds()
                                     : QStringList();
    if (tracks.isEmpty() && !trackId.isEmpty()) tracks = {trackId};
    m_selection.setTracks(tracks);
    onSelectionChanged(trackId);
    refreshRecordChips();
}

void MainWindow::refreshRecordChips() {
    if (!m_trackList) return;
    // A pin on a track that has since been deleted would keep the selection
    // from ever deciding again, invisibly — there is no chip left to un-pin.
    m_recordPins.removeIf([this](const QString& id) {
        return m_controller.project().findTrack(id.toStdString()) == nullptr;
    });
    QStringList targets;
    for (const std::string& id : recordTargets())
        targets.push_back(QString::fromStdString(id));
    m_trackList->setRecordState(m_recordEngaged || m_controller.isRecording(),
                                targets);
}

void MainWindow::setRecordPinned(const QString& trackId, bool pinned) {
    const bool had = m_recordPins.contains(trackId);
    if (pinned == had) return;
    if (pinned) m_recordPins.push_back(trackId);
    else m_recordPins.removeAll(trackId);

    // Un-pinning the last one hands the decision back to the selection, which
    // is where it started.
    refreshRecordChips();
    if (m_recordPins.isEmpty()) {
        statusBar()->showMessage(
            tr("Recording follows the selected track again"), 3000);
    }
}
void MainWindow::onTracksChanged(bool localFileDirty) {
    // Header controls already changed the document and themselves. Copy their
    // live values into existing strips now; reconstructing every strip before
    // the next paint is what made a mute, rename or folder operation feel as
    // if the application had to think first.
    if (m_mixer) m_mixer->syncFromModel();
    if (m_inspector) m_inspector->syncFromModel();
    if (m_timeline) m_timeline->update();
    // The panel shows the same track's level, pan and flags; a change made in
    // a header row has to reach it too.
    if (m_contextPanel) m_contextPanel->refresh();
    if (localFileDirty) markDirty();
}

// ── Import / export ──

void MainWindow::analyzeAudioClip(const QString& trackId, const QString& clipId,
                                  bool detectTempo, bool detectKey) {
    const daw::TrackModel* track =
        m_controller.project().findTrack(trackId.toStdString());
    const daw::ClipModel* clip = nullptr;
    if (track) {
        for (const auto& candidate : track->clips) {
            if (candidate.id == clipId.toStdString()) {
                clip = &candidate;
                break;
            }
        }
    }
    if (!clip || clip->kind != daw::ClipKind::Audio ||
        (!detectTempo && !detectKey)) return;
    QString path;
    double sourceOffset = 0.0;
    double sourceDuration = 0.0;
    double stretchTime = 1.0;
    double pitchShift = 0.0;
    if (daw::isLayered(*clip)) {
        // Takes in a comp are alternate performances of the same musical
        // section. Analyze the unmuted take that contributes the most audible
        // time: unlike flattening, this is read-only and keeps every edit live.
        const daw::TakeModel* best = nullptr;
        double bestContribution = -1.0;
        for (const auto& take : clip->takes) {
            if (take.muted || take.filePath.empty()) continue;
            double contribution = 0.0;
            for (const auto& segment : clip->comp) {
                if (segment.takeId == take.id)
                    contribution += std::max(0.0,
                        segment.endSeconds - segment.startSeconds);
            }
            if (!best || contribution > bestContribution) {
                best = &take;
                bestContribution = contribution;
            }
        }
        if (best) {
            path = QString::fromStdString(best->filePath);
            sourceOffset = std::max(0.0, best->offsetSeconds);
            sourceDuration = std::max(0.0, best->lengthSeconds);
        }
    } else {
        path = QString::fromStdString(clip->filePath);
        sourceOffset = std::max(0.0, clip->offsetSeconds);
        stretchTime = std::max(0.01, clip->sampleEdit.stretchTime);
        sourceDuration = std::max(0.0, clip->durationSeconds) / stretchTime;
        pitchShift = clip->sampleEdit.stretchPitch;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("Analyze audio clip"),
                             tr("The source audio file could not be found."));
        return;
    }

    daw::analysis::MusicalAnalysisRequest request;
    request.detectTempo = detectTempo;
    request.detectKey = detectKey;
    request.offsetSeconds = sourceOffset;
    request.stretchTime = stretchTime;
    request.durationSeconds = sourceDuration;
    request.pitchShiftSemitones = pitchShift;
    request.fileNameHint = QFileInfo(path).completeBaseName().toStdString();

    daw::analysis::MusicalAnalysisResult analysis;
    QString error;
    bool cancelled = false;
    if (!runMusicalAnalysis(this, path, request, analysis, error, cancelled)) {
        if (!cancelled) {
            QMessageBox::warning(
                this, tr("Audio analysis failed"),
                error.isEmpty() ? tr("The clip could not be analyzed.") : error);
        }
        return;
    }

    daw::ClipMusicalAnalysisModel model =
        daw::analysis::toClipAnalysisModel(analysis, request);
    // A one-field re-analysis must not erase a useful result from the other
    // command. Both still belong to this exact clip instance.
    if (!detectTempo) model.tempo = clip->musicalAnalysis.tempo;
    if (!detectKey) model.key = clip->musicalAnalysis.key;
    const std::size_t undoStart = m_controller.undoDepth();
    m_controller.setClipMusicalAnalysis(trackId.toStdString(), clipId.toStdString(),
                                        model, "Analyze Audio Clip");

    AudioAnalysisResultDialog resultDialog(
        analysis, request, m_controller.tempo(), /*importing=*/false,
        /*autoApplyTempo=*/false, this);
    const bool resultAccepted = resultDialog.exec() == QDialog::Accepted;
    if (resultAccepted && resultDialog.appliesTempo())
        m_controller.setTempo(resultDialog.selectedTempo());
    m_controller.collapseUndo(undoStart, "Analyze Audio Clip");
    syncViews();
    if (m_contextPanel) m_contextPanel->rebuild();
    markDirty();
}

void MainWindow::showNextDownloadedAudioPrompt() {
    if (m_audioImportDialogOpen || m_downloadedAudio.isEmpty()) return;
    m_audioImportDialogOpen = true;
    const PendingAudioDownload pending = m_downloadedAudio.dequeue();

    const daw::TrackModel* selected =
        m_selectedTrackId.isEmpty()
            ? nullptr
            : m_controller.project().findTrack(m_selectedTrackId.toStdString());
    const QString selectedId =
        selected && selected->kind == daw::TrackKind::Audio
            ? QString::fromStdString(selected->id)
            : QString();
    const QString selectedName =
        selectedId.isEmpty() ? QString() : QString::fromStdString(selected->name);

    DownloadedAudioDialog::Choice choice = DownloadedAudioDialog::Choice::None;
    bool detectTempo = false;
    bool detectKey = false;
    const int forcedChoice = std::exchange(m_audioImportChoiceForTest, 0);
    if (forcedChoice == 1) {
        choice = DownloadedAudioDialog::Choice::NewTrack;
    } else if (forcedChoice == 2) {
        choice = DownloadedAudioDialog::Choice::SelectedTrack;
    } else if (forcedChoice == 3) {
        choice = DownloadedAudioDialog::Choice::None;
    } else {
        DownloadedAudioDialog dialog(
            pending.path, pending.durationSeconds, pending.sampleRate,
            pending.channels, selectedName, this);
        if (dialog.exec() == QDialog::Accepted) {
            choice = dialog.choice();
            detectTempo = dialog.detectTempo();
            detectKey = dialog.detectKey();
        }
    }

    daw::ClipMusicalAnalysisModel analysisModel;
    double tempoToApply = 0.0;
    if (choice != DownloadedAudioDialog::Choice::None &&
        (detectTempo || detectKey)) {
        daw::analysis::MusicalAnalysisRequest request;
        request.detectTempo = detectTempo;
        request.detectKey = detectKey;
        request.fileNameHint = QFileInfo(pending.path).completeBaseName().toStdString();
        daw::analysis::MusicalAnalysisResult analysis;
        QString error;
        bool cancelled = false;
        if (!runMusicalAnalysis(this, pending.path, request, analysis, error,
                                cancelled)) {
            choice = DownloadedAudioDialog::Choice::None;
            if (!cancelled) {
                QMessageBox::warning(
                    this, tr("Audio analysis failed"),
                    error.isEmpty() ? tr("The file could not be analyzed.") : error);
            }
        } else {
            analysisModel = daw::analysis::toClipAnalysisModel(analysis, request);
            AudioAnalysisResultDialog resultDialog(
                analysis, request, m_controller.tempo(), /*importing=*/true,
                detectTempo && analysis.tempo.highConfidence(), this);
            if (resultDialog.exec() != QDialog::Accepted) {
                choice = DownloadedAudioDialog::Choice::None;
            } else if (resultDialog.appliesTempo()) {
                tempoToApply = resultDialog.selectedTempo();
            }
        }
    }

    bool imported = false;
    QString landedTrack;
    const std::size_t undoStart = m_controller.undoDepth();
    if (choice != DownloadedAudioDialog::Choice::None && tempoToApply > 0.0)
        m_controller.setTempo(tempoToApply);
    if (choice == DownloadedAudioDialog::Choice::NewTrack) {
        const QString name = QFileInfo(pending.path).completeBaseName();
        const std::string trackId = m_controller.importAudioToNewTrack(
            pending.path.toStdString(), m_controller.positionSeconds(),
            name.toStdString(), analysisModel);
        if (!trackId.empty()) {
            imported = true;
            landedTrack = QString::fromStdString(trackId);
            syncViews();
            selectTrackFromHeader(landedTrack);
        }
    } else if (choice == DownloadedAudioDialog::Choice::SelectedTrack &&
               !selectedId.isEmpty()) {
        const auto* current =
            m_controller.project().findTrack(selectedId.toStdString());
        if (current && current->kind == daw::TrackKind::Audio) {
            imported = !m_controller
                            .importAudio(pending.path.toStdString(),
                                         selectedId.toStdString(),
                                         m_controller.positionSeconds(),
                                         analysisModel)
                            .empty();
            landedTrack = selectedId;
            if (imported) syncViews();
        }
    }

    if (imported) {
        m_controller.collapseUndo(undoStart, "Import Downloaded Audio");
        markDirty();
        const auto* track =
            m_controller.project().findTrack(landedTrack.toStdString());
        statusBar()->showMessage(
            tr("Imported %1 to %2")
                .arg(QFileInfo(pending.path).fileName(),
                     track ? QString::fromStdString(track->name) : landedTrack),
            4000);
    } else if (choice != DownloadedAudioDialog::Choice::None) {
        // If applying the detected tempo succeeded but the import itself did
        // not, do not leave that unrelated project edit behind.
        while (m_controller.undoDepth() > undoStart) m_controller.undo();
        if (forcedChoice != 0)
            statusBar()->showMessage(tr("Downloaded audio import failed"), 3000);
        else
        QMessageBox::warning(
            this, tr("Import failed"),
            tr("The downloaded file was kept in Downloads, but could not be "
               "added to the project.\n\n%1")
                .arg(pending.path));
    }

    m_audioImportDialogOpen = false;
    QTimer::singleShot(0, this, &MainWindow::showNextDownloadedAudioPrompt);
}

void MainWindow::openWebBrowserForShot() {
    setWebVisible(true, /*persist=*/false);
    if (m_webPanel) {
        const QString url = qEnvironmentVariable("DAW_SHOT_WEB_URL").trimmed();
        if (!url.isEmpty()) m_webPanel->openUrlForTest(url);
    }
}

bool MainWindow::checkWebBrowserForTest(const QString& audioFile) {
    if (audioFile.isEmpty() || !QFileInfo::exists(audioFile)) return false;
    setWebVisible(true, /*persist=*/false);
    if (!m_webPanel) return false;

    m_webPanel->openUrlForTest(QLatin1String(ui::webprefs::kStartUrl));
    QElapsedTimer startTimeout;
    startTimeout.start();
    while (!m_webPanel->startPageReadyForTest() &&
           startTimeout.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (!m_webPanel->startPageReadyForTest()) {
        std::fprintf(stderr, "web self-test: VLT start page did not render\n");
        return false;
    }

    // Persistent bookmark data drives both the compact bar and the start page.
    // Use an invalid domain so the test can never contact the network.
    const QString bookmarkUrl =
        QStringLiteral("https://bookmark-selftest.invalid/example");
    ui::webprefs::removeBookmark(bookmarkUrl);
    if (!ui::webprefs::addBookmark(QStringLiteral("Bookmark self-test"),
                                   bookmarkUrl) ||
        !ui::webprefs::isBookmarked(bookmarkUrl)) {
        std::fprintf(stderr, "web self-test: bookmark was not persisted\n");
        return false;
    }
    m_webPanel->reloadSettings();
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    const bool bookmarkVisible =
        !m_webPanel
             ->findChildren<QPushButton*>(QStringLiteral("WebBookmarkChip"))
             .isEmpty();
    ui::webprefs::removeBookmark(bookmarkUrl);
    m_webPanel->reloadSettings();
    if (!bookmarkVisible || ui::webprefs::isBookmarked(bookmarkUrl)) {
        std::fprintf(stderr, "web self-test: bookmark bar did not synchronize\n");
        return false;
    }

    const std::size_t tracksBefore = m_controller.project().tracks.size();
    const std::size_t undoBefore = m_controller.undoDepth();
    constexpr double kPlayhead = 1.375;
    m_controller.seekSeconds(kPlayhead);
    m_audioImportChoiceForTest = 1;
    m_webPanel->downloadAudioForTest(audioFile);

    QElapsedTimer timeout;
    timeout.start();
    while (m_controller.project().tracks.size() == tracksBefore &&
           timeout.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    // The undo stack has a ceiling, and by this point in the run it is at it:
    // every further edit drops the oldest entry, so the depth stops rising.
    // Comparing against `undoBefore + 1` unconditionally made this check fail
    // for a reason that has nothing to do with the import — and it will do it
    // again to the next check written this way.
    const std::size_t expectedUndo =
        std::min(undoBefore + 1, m_controller.undoLimit());
    if (m_controller.project().tracks.size() != tracksBefore + 1 ||
        m_controller.undoDepth() != expectedUndo) {
        std::fprintf(stderr,
                     "web self-test import: tracks=%zu expected=%zu undo=%zu "
                     "expectedUndo=%zu pendingChoice=%d\n",
                     m_controller.project().tracks.size(), tracksBefore + 1,
                     m_controller.undoDepth(), expectedUndo,
                     m_audioImportChoiceForTest);
        return false;
    }
    const std::string importedId = m_controller.project().tracks.back().id;
    const auto* imported = m_controller.project().findTrack(importedId);
    if (!imported || imported->kind != daw::TrackKind::Audio ||
        imported->clips.size() != 1 ||
        std::fabs(imported->clips.front().startSeconds - kPlayhead) > 1e-9) {
        std::fprintf(stderr, "web self-test: the imported track is not one "
                             "audio clip at the playhead\n");
        return false;
    }
    m_controller.undo();
    const bool undone = m_controller.project().findTrack(importedId) == nullptr;
    m_controller.redo();
    const auto* redone = m_controller.project().findTrack(importedId);
    const bool restored = redone && redone->clips.size() == 1;
    m_controller.undo();
    syncViews();
    if (!undone || !restored) {
        std::fprintf(stderr, "web self-test: undo=%d redo=%d of the import\n",
                     int(undone), int(restored));
        return false;
    }

    // The selected-track branch lands on the selected primary Audio Track at
    // the same playhead and contributes one ordinary import undo entry.
    std::string selectedId;
    for (const auto& track : m_controller.project().tracks) {
        if (track.kind == daw::TrackKind::Audio) {
            selectedId = track.id;
            break;
        }
    }
    if (selectedId.empty()) {
        std::fprintf(stderr, "web self-test: no audio track to select\n");
        return false;
    }
    selectTrackFromHeader(QString::fromStdString(selectedId));
    const auto* selectedBefore = m_controller.project().findTrack(selectedId);
    const std::size_t selectedClips = selectedBefore->clips.size();
    const std::size_t selectedUndo = m_controller.undoDepth();
    m_audioImportChoiceForTest = 2;
    m_webPanel->handleDownloadedFileForTest(audioFile, QStringLiteral("audio/wav"));
    timeout.restart();
    while (m_audioImportChoiceForTest != 0 && timeout.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    const auto* selectedAfter = m_controller.project().findTrack(selectedId);
    if (!selectedAfter || selectedAfter->clips.size() != selectedClips + 1 ||
        std::fabs(selectedAfter->clips.back().startSeconds - kPlayhead) > 1e-9 ||
        m_controller.undoDepth() !=
            std::min(selectedUndo + 1, m_controller.undoLimit())) {
        std::fprintf(stderr,
                     "web self-test import onto the selected track: clips=%zu "
                     "expected=%zu undo=%zu expected=%zu\n",
                     selectedAfter ? selectedAfter->clips.size() : 0,
                     selectedClips + 1, m_controller.undoDepth(),
                     std::min(selectedUndo + 1, m_controller.undoLimit()));
        return false;
    }
    m_controller.undo();
    syncViews();

    // Cancel is a project no-op and never removes the downloaded source.
    const std::size_t cancelTracks = m_controller.project().tracks.size();
    const std::size_t cancelUndo = m_controller.undoDepth();
    m_audioImportChoiceForTest = 3;
    m_webPanel->handleDownloadedFileForTest(audioFile, QStringLiteral("audio/wav"));
    timeout.restart();
    while (m_audioImportChoiceForTest != 0 && timeout.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (!QFileInfo::exists(audioFile) ||
        m_controller.project().tracks.size() != cancelTracks ||
        m_controller.undoDepth() != cancelUndo) {
        std::fprintf(stderr,
                     "web self-test cancel: file=%d tracks=%zu/%zu undo=%zu/%zu\n",
                     int(QFileInfo::exists(audioFile)),
                     m_controller.project().tracks.size(), cancelTracks,
                     m_controller.undoDepth(), cancelUndo);
        return false;
    }

    // A non-audio extension is ignored immediately; a corrupt supported file
    // reaches the worker probe but must never enqueue an import dialog.
    const QString invalidRoot = QDir::temp().filePath("daw-web-invalid");
    QDir().mkpath(invalidRoot);
    const QString textPath = QDir(invalidRoot).filePath("readme.txt");
    const QString corruptPath = QDir(invalidRoot).filePath("broken.wav");
    for (const QString& path : {textPath, corruptPath}) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write("not an audio file") <= 0) {
            return false;
        }
    }
    m_webPanel->handleDownloadedFileForTest(textPath, QStringLiteral("text/plain"));
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    if (m_controller.project().tracks.size() != cancelTracks ||
        m_controller.undoDepth() != cancelUndo) {
        return false;
    }
    bool corruptRejected = false;
    const QString corruptPrefix = QCoreApplication::translate(
        "WebBrowserPanel", "Downloaded audio could not be read: %1")
                                      .section(QStringLiteral("%1"), 0, 0);
    const QMetaObject::Connection rejectedConnection = connect(
        m_webPanel, &WebBrowserPanel::statusMessage, this,
        [&corruptRejected, corruptPrefix](const QString& message) {
            if (message.startsWith(corruptPrefix, Qt::CaseInsensitive)) {
                corruptRejected = true;
            }
        });
    m_webPanel->handleDownloadedFileForTest(corruptPath,
                                            QStringLiteral("audio/wav"));
    timeout.restart();
    while (!corruptRejected && timeout.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    disconnect(rejectedConnection);
    if (!corruptRejected || m_controller.project().tracks.size() != cancelTracks ||
        m_controller.undoDepth() != cancelUndo) {
        return false;
    }

    // ── Tabs ──
    //
    // Opening, switching and closing, plus the rule that the panel always has
    // a tab: closing the last one leaves a fresh start page rather than a dead
    // toolbar over an empty rectangle.
    const int tabsBefore = m_webPanel->tabCountForTest();
    m_webPanel->openTabForTest(QStringLiteral("vlt:start"));
    m_webPanel->openTabForTest(QStringLiteral("vlt:start"));
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    if (m_webPanel->tabCountForTest() != tabsBefore + 2) {
        std::fprintf(stderr, "web self-test tabs: %d open, expected %d\n",
                     m_webPanel->tabCountForTest(), tabsBefore + 2);
        return false;
    }
    m_webPanel->closeCurrentTabForTest();
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    if (m_webPanel->tabCountForTest() != tabsBefore + 1) {
        std::fprintf(stderr, "web self-test tabs: closing one left %d\n",
                     m_webPanel->tabCountForTest());
        return false;
    }
    while (m_webPanel->tabCountForTest() > 1) {
        m_webPanel->closeCurrentTabForTest();
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    m_webPanel->closeCurrentTabForTest();
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    if (m_webPanel->tabCountForTest() != 1) {
        std::fprintf(stderr,
                     "web self-test tabs: closing the last one left %d, not a "
                     "fresh one\n", m_webPanel->tabCountForTest());
        return false;
    }
    // And it reopens: the address of a closed tab is remembered.
    m_webPanel->openTabForTest(QStringLiteral("https://example.com"));
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    m_webPanel->closeCurrentTabForTest();
    m_webPanel->reopenClosedTabForTest();
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    if (m_webPanel->tabCountForTest() != 2) {
        std::fprintf(stderr, "web self-test tabs: reopen left %d tabs\n",
                     m_webPanel->tabCountForTest());
        return false;
    }
    while (m_webPanel->tabCountForTest() > 1) {
        m_webPanel->closeCurrentTabForTest();
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    // Both right-hand panels remain independent. At a constrained width Web
    // yields its preferred width first, then AI; at a wide width both preferred
    // values return without having been overwritten by the temporary squeeze.
    const int originalWidth = width();
    const int originalHeight = height();
    const int originalWebWidth = m_webWidth;
    const int originalAiWidth = m_aiWidth;
    m_webWidth = 600;
    m_aiWidth = 350;
    setWebVisible(true, /*persist=*/false);
    setAiVisible(true, /*persist=*/false);
    resize(2000, originalHeight);
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!m_webContainer->isVisible() || !m_aiPanel->isVisible() ||
        m_webContainer->width() != 600 || m_aiPanel->width() != 350) {
        std::fprintf(stderr, "web self-test wide: window=%d web=%d ai=%d\n",
                     width(), m_webContainer->width(), m_aiPanel->width());
        return false;
    }
    resize(1500, originalHeight);
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto insideShell = [this](QWidget* widget) {
        return widget && centralWidget() &&
               widget->mapTo(centralWidget(), QPoint(widget->width(), 0)).x() <=
                   centralWidget()->width();
    };
    if (!(m_webContainer->width() < 600 || m_aiPanel->width() < 350) ||
        m_trackList->width() < ui::kMinTrackHeaderWidth ||
        !insideShell(m_webContainer) || !insideShell(m_aiPanel)) {
        std::fprintf(stderr,
                     "web self-test first shrink: window=%d min=%d hint=%d "
                     "centralMin=%d centralHint=%d web=%d ai=%d arrangement=%d\n",
                     width(), minimumWidth(), minimumSizeHint().width(),
                     centralWidget()->minimumWidth(),
                     centralWidget()->minimumSizeHint().width(),
                     m_webContainer->width(), m_aiPanel->width(),
                     m_arrangementHost->width());
        return false;
    }
    resize(1180, originalHeight);
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    auto* transportPill =
        m_transport->findChild<QWidget*>(QStringLiteral("TransportPill"));
    auto* lcdScreen =
        m_transport->findChild<QWidget*>(QStringLiteral("LcdScreen"));
    auto* positionSection =
        m_transport->findChild<QWidget*>(QStringLiteral("PositionSection"));
    auto* tempoSection =
        m_transport->findChild<QWidget*>(QStringLiteral("TempoSection"));
    auto* lcdInset =
        m_transport->findChild<QWidget*>(QStringLiteral("LcdInsetWell"));
    auto* spectrum =
        m_transport->findChild<QWidget*>(QStringLiteral("TransportSpectrum"));
    auto* transportButtons =
        m_transport->findChild<QWidget*>(QStringLiteral("TransportGroup"));
    auto* headerTools =
        m_transport->findChild<QWidget*>(QStringLiteral("HeaderToolGroup"));
    auto* barsPosition =
        m_transport->findChild<QWidget*>(QStringLiteral("BarsPosition"));
    auto* timeSignature =
        m_transport->findChild<QToolButton*>(QStringLiteral("TimeSignatureButton"));
    auto* stopButton =
        m_transport->findChild<QAbstractButton*>(QStringLiteral("TransportStop"));
    auto* playButton =
        m_transport->findChild<QAbstractButton*>(QStringLiteral("TransportPlay"));
    auto* recordButton =
        m_transport->findChild<QAbstractButton*>(QStringLiteral("TransportRecord"));
    auto* leftDock =
        m_transport->findChild<QWidget*>(QStringLiteral("HeaderLeftDock"));
    auto* rightDock =
        m_transport->findChild<QWidget*>(QStringLiteral("HeaderRightDock"));
    auto* dockReveal =
        m_transport->findChild<QAbstractButton*>(QStringLiteral("HeaderDockReveal"));
    auto* dockBrowser =
        m_transport->findChild<QAbstractButton*>(QStringLiteral("HeaderBrowserButton"));
    const auto gridChips =
        m_transport->findChildren<QToolButton*>(QStringLiteral("GridChip"));
    const bool gridVisible = !gridChips.isEmpty() &&
        std::all_of(gridChips.begin(), gridChips.end(),
                    [](QToolButton* chip) { return chip->isVisible(); });
    const bool gridContained = !gridChips.isEmpty() &&
        std::all_of(gridChips.begin(), gridChips.end(),
                    [this](QToolButton* chip) {
                        const int left = chip->mapTo(m_transport, QPoint()).x();
                        const int right = left + chip->width();
                        return chip->width() > 0 && left >= 0 &&
                               right <= m_transport->width();
                    });
    const bool gridCompact = gridChips.size() == 2 &&
        std::all_of(gridChips.begin(), gridChips.end(),
                    [headerTools](QToolButton* chip) {
                        return chip->width() <= 64 && chip->menu() &&
                               !chip->menu()->actions().isEmpty() && headerTools &&
                               headerTools->isAncestorOf(chip) &&
                               chip->toolButtonStyle() ==
                                    Qt::ToolButtonTextBesideIcon &&
                               !chip->text().isEmpty() &&
                               !chip->accessibleName().isEmpty();
                    });
    const QList<QWidget*> requiredControls{
        transportButtons, lcdScreen, positionSection, headerTools,
        barsPosition, tempoSection, timeSignature, spectrum,
        stopButton, playButton, recordButton};
    const bool requiredVisible = transportPill &&
        std::all_of(requiredControls.begin(), requiredControls.end(),
                    [transportPill](QWidget* control) {
                        if (!control || !control->isVisible()) return false;
                        const int left = control->mapTo(transportPill, QPoint()).x();
                        return left >= 0 && left + control->width() <=
                                                transportPill->width();
                    });
    const bool sectionOrder = transportButtons && lcdScreen && headerTools &&
        transportButtons->x() < lcdScreen->x() &&
        lcdScreen->x() < headerTools->x();
    const bool zonesAligned = transportPill && transportButtons && lcdScreen &&
        headerTools && transportPill->isAncestorOf(transportButtons) &&
        transportPill->isAncestorOf(lcdScreen) &&
        transportPill->isAncestorOf(headerTools) &&
        std::abs(transportButtons->height() - headerTools->height()) <= 4 &&
        lcdScreen->height() > transportButtons->height() &&
        lcdScreen->height() > headerTools->height() &&
        std::abs(transportButtons->geometry().center().y() -
                 lcdScreen->geometry().center().y()) <= 1 &&
        std::abs(headerTools->geometry().center().y() -
                 lcdScreen->geometry().center().y()) <= 1;
    const bool threeBlockLayout = transportPill && lcdScreen &&
        !qobject_cast<ui::GlassPanel*>(transportPill) &&
        qobject_cast<ui::GlassPanel*>(lcdScreen) &&
        transportPill->findChildren<ui::GlassPanel*>().size() == 1;
    const auto displayDividers = lcdScreen
        ? lcdScreen->findChildren<QWidget*>(QStringLiteral("LcdDivider"))
        : QList<QWidget*>();
    const bool lcdContent = lcdScreen && positionSection && timeSignature &&
        tempoSection && lcdInset && spectrum &&
        lcdScreen->isAncestorOf(lcdInset) &&
        lcdInset->isAncestorOf(positionSection) &&
        lcdInset->isAncestorOf(tempoSection) &&
        lcdInset->isAncestorOf(timeSignature) &&
        lcdInset->isAncestorOf(spectrum) &&
        lcdScreen->isAncestorOf(timeSignature) &&
        positionSection->x() < tempoSection->x() &&
        tempoSection->x() < timeSignature->x() &&
        timeSignature->x() < spectrum->x() && displayDividers.size() == 3;
    const bool signatureMenuComplete = timeSignature && timeSignature->menu() &&
        timeSignature->menu()->actions().size() == 10;
    const bool clusterCentered = transportPill &&
        std::abs(transportPill->mapTo(m_transport, QPoint()).x() * 2 +
                     transportPill->width() - m_transport->width()) <= 2;
    const auto rightDockButtons = rightDock
        ? rightDock->findChildren<QAbstractButton*>(
              QString(), Qt::FindDirectChildrenOnly)
        : QList<QAbstractButton*>();
    const bool insetDocksPlaced = leftDock && rightDock && dockReveal &&
        leftDock->isVisible() && rightDock->isVisible() &&
        transportPill && leftDock->height() < transportPill->height() &&
        rightDock->height() < transportPill->height() &&
        std::abs(leftDock->geometry().center().y() -
                 transportPill->geometry().center().y()) <= 1 &&
        std::abs(rightDock->geometry().center().y() -
                 transportPill->geometry().center().y()) <= 1 &&
        std::abs(leftDock->mapTo(m_transport, QPoint()).x() - 14) <= 1 &&
        std::abs(rightDock->mapTo(m_transport, QPoint()).x() +
                     rightDock->width() - m_transport->width() + 14) <= 1 &&
        rightDockButtons.size() == 2;
    const int collapsedDockWidth = leftDock ? leftDock->width() : 0;
    const bool drawerInitiallyCompact = dockBrowser && !dockBrowser->isVisible();
    if (dockReveal) dockReveal->click();
    QElapsedTimer drawerTimer;
    drawerTimer.start();
    while (drawerTimer.elapsed() < 220)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    const bool drawerExpanded = leftDock && dockBrowser &&
        leftDock->width() > collapsedDockWidth && dockBrowser->isVisible();
    const bool expandedClusterCentered = transportPill &&
        std::abs(transportPill->mapTo(m_transport, QPoint()).x() * 2 +
                     transportPill->width() - m_transport->width()) <= 2;
    if (dockReveal) dockReveal->click();
    drawerTimer.restart();
    while (drawerTimer.elapsed() < 220)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    const bool drawerCollapsedAgain = leftDock && dockBrowser &&
        leftDock->width() == collapsedDockWidth && !dockBrowser->isVisible();
    const auto headerButtons = m_transport->findChildren<QAbstractButton*>();
    const bool hasUndoRedo =
        std::any_of(headerButtons.begin(), headerButtons.end(),
                    [this](QAbstractButton* button) {
                        return button->toolTip() == tr("Undo") ||
                               button->toolTip() == tr("Redo");
                    });
    if (!(m_webContainer->width() < ui::webprefs::kMinWidth ||
          m_aiPanel->width() < 240) ||
        m_trackList->width() < ui::kMinTrackHeaderWidth ||
        !insideShell(m_webContainer) || !insideShell(m_aiPanel) ||
        !requiredVisible || !gridVisible || !gridContained ||
        !gridCompact ||
        !sectionOrder || !zonesAligned || !threeBlockLayout ||
        !lcdContent ||
        !signatureMenuComplete ||
        !clusterCentered || hasUndoRedo || !statusBar()->isHidden() ||
        !insetDocksPlaced || !drawerInitiallyCompact || !drawerExpanded ||
        !expandedClusterCentered || !drawerCollapsedAgain) {
        std::fprintf(stderr,
                     "web self-test second shrink: window=%d web=%d ai=%d "
                     "arrangement=%d required=%d grid=%d "
                     "contained=%d compact=%d "
                     "order=%d aligned=%d threeBlocks=%d lcd=%d signature=%d centered=%d undoRedo=%d "
                     "docks=%d drawer=%d/%d/%d expandedCentered=%d min=%d\n",
                     width(), m_webContainer->width(), m_aiPanel->width(),
                     m_arrangementHost->width(), requiredVisible, gridVisible,
                     gridContained, gridCompact, sectionOrder, zonesAligned,
                     threeBlockLayout, lcdContent,
                     signatureMenuComplete, clusterCentered, hasUndoRedo,
                     insetDocksPlaced, drawerInitiallyCompact, drawerExpanded,
                     drawerCollapsedAgain, expandedClusterCentered,
                     m_transport->minimumResponsiveWidth());
        for (QToolButton* chip : gridChips) {
            const int left = chip->mapTo(m_transport, QPoint()).x();
            std::fprintf(stderr, "grid chip: left=%d width=%d right=%d\n",
                         left, chip->width(), left + chip->width());
        }
        return false;
    }

    // Reproduce the original failure, not just a window resize: pull each
    // right-hand handle far past its maximum while the workspace is already
    // constrained. The surface must stop at the frame and its live preferred
    // width must be clamped back to the width it could actually take.
    auto* webResize = qobject_cast<ui::ResizeHandle*>(m_webHandle);
    auto* aiResize = qobject_cast<ui::ResizeHandle*>(m_aiHandle);
    if (webResize && webResize->onDragStart && webResize->onDrag) {
        webResize->onDragStart();
        webResize->onDrag(-10000);
    }
    if (aiResize && aiResize->onDragStart && aiResize->onDrag) {
        aiResize->onDragStart();
        aiResize->onDrag(-10000);
    }
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!insideShell(m_webContainer) || !insideShell(m_aiPanel) ||
        m_webWidth != m_webContainer->width() ||
        m_aiWidth != m_aiPanel->width()) {
        std::fprintf(stderr,
                     "web self-test overdrag: web=%d/%d ai=%d/%d inside=%d/%d\n",
                     m_webWidth, m_webContainer->width(), m_aiWidth,
                     m_aiPanel->width(), insideShell(m_webContainer),
                     insideShell(m_aiPanel));
        return false;
    }
    m_webWidth = 600;
    m_aiWidth = 350;
    resize(2000, originalHeight);
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    if (m_webContainer->width() != 600 || m_aiPanel->width() != 350) {
        std::fprintf(stderr, "web self-test restore: window=%d web=%d ai=%d\n",
                     width(), m_webContainer->width(), m_aiPanel->width());
        return false;
    }
    setWebVisible(false, /*persist=*/false);
    setWebVisible(true, /*persist=*/false);
    setAiVisible(false, /*persist=*/false);
    setAiVisible(true, /*persist=*/false);
    if (m_webContainer->width() != 600 || m_aiPanel->width() != 350) {
        std::fprintf(stderr, "web self-test toggles: window=%d web=%d ai=%d\n",
                     width(), m_webContainer->width(), m_aiPanel->width());
        return false;
    }

    // Address/page focus owns editing and transport keys. In particular Undo
    // must not touch the project and Space must not start playback behind it.
    auto* address =
        m_webPanel->findChild<QLineEdit*>(QStringLiteral("WebAddress"));
    if (!address) {
        std::fprintf(stderr, "web self-test: address field missing\n");
        return false;
    }
    activateWindow();
    address->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    const std::size_t focusUndo = m_controller.undoDepth();
    onUndo();
    if (m_controller.undoDepth() != focusUndo) {
        std::fprintf(stderr, "web self-test: focused Undo changed the project\n");
        return false;
    }
    if (!routeEditChord(EditChord::Copy)) {
        QWidget* focus = QApplication::focusWidget();
        std::fprintf(stderr,
                     "web self-test: focused Copy escaped (focus=%s object=%s)\n",
                     focus ? focus->metaObject()->className() : "none",
                     focus ? focus->objectName().toUtf8().constData() : "none");
        return false;
    }
    m_controller.pause();
    onPlayPause();
    if (m_controller.isPlaying()) {
        std::fprintf(stderr, "web self-test: focused Space started transport\n");
        return false;
    }

    // Bare Return belongs to the address field, not the application-wide
    // return-to-start shortcut. Exercise the real ShortcutOverride + key path,
    // and verify that a plain phrase becomes a search URL.
    address->setText(QStringLiteral("vlt browser search"));
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_Return,
                            Qt::NoModifier);
    QApplication::sendEvent(address, &overrideEvent);
    QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                         QStringLiteral("\r"));
    QApplication::sendEvent(address, &enterEvent);
    if (!overrideEvent.isAccepted() ||
        !address->text().startsWith(QStringLiteral("https://duckduckgo.com/")) ||
        !address->text().contains(QStringLiteral("vlt browser search"))) {
        std::fprintf(stderr,
                     "web self-test: Return did not submit the address/search field\n");
        return false;
    }
    m_webPanel->openUrlForTest(QLatin1String(ui::webprefs::kStartUrl));

    m_webWidth = originalWebWidth;
    m_aiWidth = originalAiWidth;
    resize(originalWidth, originalHeight);
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    centralWidget()->setFocus(Qt::OtherFocusReason);

    DownloadedAudioDialog disabledDialog(audioFile, 1.0, 48000.0, 2,
                                         QString(), this);
    auto* selectedButton = disabledDialog.findChild<QPushButton*>(
        QStringLiteral("DownloadedAudioSelected"));
    return selectedButton && !selectedButton->isEnabled();
}

void MainWindow::onImportAudio() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Audio"), QString(), ui::audioNameFilter());
    if (path.isEmpty()) return;

    QString targetId = m_selectedTrackId;
    if (targetId.isEmpty()) {
        for (const auto& t : m_controller.project().tracks) {
            if (t.kind == daw::TrackKind::Audio) {
                targetId = QString::fromStdString(t.id);
                break;
            }
        }
    }
    if (targetId.isEmpty()) {
        targetId = QString::fromStdString(
            m_controller.addTrack(daw::TrackKind::Audio, "Audio 1"));
    }

    const std::string clip = m_controller.importAudio(
        path.toStdString(), targetId.toStdString(), m_controller.positionSeconds());
    if (clip.empty()) {
        QMessageBox::warning(this, tr("Import failed"),
                             tr("Could not read %1").arg(path));
        return;
    }
    syncViews();
    markDirty();
}

bool MainWindow::checkExportDialogForTest() {
    ExportDialog dialog(m_controller, &m_selection, this);
    return dialog.checkForTest();
}

void MainWindow::openExportDialogForShot(bool stems) {
    auto* dialog = new ExportDialog(m_controller, &m_selection, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    if (stems) dialog->stageStemsForShot();
    dialog->show();
}

void MainWindow::onExport() {
    // Modal and short-lived: a render takes over the engine while it runs, so
    // there is nothing useful to do with a second one open beside it.
    ExportDialog dialog(m_controller, &m_selection, this);
    if (dialog.exec() != QDialog::Accepted) return;
    statusBar()->showMessage(tr("Render finished"), 4000);
}

void MainWindow::updateLocalProcessingActions() {
    bool localOnlyAvailable = true;
#ifdef DAW_ENABLE_COLLABORATION
    localOnlyAvailable = m_cloudProjectId.isEmpty();
#endif
    const bool hasRegion = m_timeline && m_timeline->hasRegionSelection();
    const bool hasLoop = m_controller.isLoopEnabled() &&
                         m_controller.loopEndSeconds() >
                             m_controller.loopStartSeconds();
    const bool hasClips = !m_selection.clips().isEmpty();
    if (m_bounceInPlaceAction) {
        m_bounceInPlaceAction->setEnabled(
            localOnlyAvailable && (hasRegion || hasLoop || hasClips));
        m_bounceInPlaceAction->setToolTip(
            localOnlyAvailable ? tr("Render and insert selected material")
                               : tr("Local-only in v1"));
    }

    bool audioOnly = hasClips;
    for (const ui::ClipSel& selected : m_selection.clips()) {
        const daw::TrackModel* track =
            m_controller.project().findTrack(selected.trackId.toStdString());
        if (!track) {
            audioOnly = false;
            break;
        }
        const auto clip = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const daw::ClipModel& item) {
                return item.id == selected.clipId.toStdString();
            });
        if (clip == track->clips.end() || clip->kind != daw::ClipKind::Audio) {
            audioOnly = false;
            break;
        }
    }
    if (m_offlineRenderAction) {
        m_offlineRenderAction->setEnabled(localOnlyAvailable && audioOnly);
        m_offlineRenderAction->setToolTip(
            !localOnlyAvailable ? tr("Local-only in v1")
                                : tr("Available for audio clips only"));
    }
    if (m_timeline) m_timeline->setLocalProcessingEnabled(localOnlyAvailable);
}

void MainWindow::onBounceInPlace() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty()) {
        statusBar()->showMessage(tr("Bounce in Place — Local-only in v1"),
                                 4000);
        return;
    }
#endif
    daw::EngineController::BounceRequest request;
    QString sourceDescription;

    if (m_timeline && m_timeline->hasRegionSelection()) {
        request.startSeconds = m_timeline->regionStartSeconds();
        request.endSeconds = m_timeline->regionEndSeconds();
        const QStringList regionTracks = m_timeline->regionTrackIds();
        for (const QString& trackId : regionTracks) {
            const daw::TrackModel* track =
                m_controller.project().findTrack(trackId.toStdString());
            if (!track) continue;
            for (const daw::ClipModel& clip : track->clips) {
                if (clip.kind == daw::ClipKind::Automation ||
                    clip.startSeconds >= request.endSeconds ||
                    clip.startSeconds + m_controller.clipPlaybackDuration(clip) <=
                        request.startSeconds) {
                    continue;
                }
                request.clips.push_back({track->id, clip.id});
            }
        }
        sourceDescription = tr("Region: %1–%2 s across %3 track(s)")
                                .arg(request.startSeconds, 0, 'f', 3)
                                .arg(request.endSeconds, 0, 'f', 3)
                                .arg(regionTracks.size());
    } else if (m_controller.isLoopEnabled() &&
               m_controller.loopEndSeconds() >
                   m_controller.loopStartSeconds()) {
        request.startSeconds = m_controller.loopStartSeconds();
        request.endSeconds = m_controller.loopEndSeconds();
        QStringList sources;
        for (const ui::ClipSel& selected : m_selection.clips()) {
            if (!sources.contains(selected.trackId))
                sources.push_back(selected.trackId);
        }
        if (sources.isEmpty()) sources = selectedTrackIds();
        for (const QString& id : sources)
            request.tracks.push_back(id.toStdString());
        request.fullMix = request.tracks.empty();
        sourceDescription = request.fullMix
                                ? tr("Loop: full audible mix, %1–%2 s")
                                      .arg(request.startSeconds, 0, 'f', 3)
                                      .arg(request.endSeconds, 0, 'f', 3)
                                : tr("Loop: %1 track(s), %2–%3 s")
                                      .arg(request.tracks.size())
                                      .arg(request.startSeconds, 0, 'f', 3)
                                      .arg(request.endSeconds, 0, 'f', 3);
    } else {
        request.startSeconds = std::numeric_limits<double>::max();
        request.endSeconds = 0.0;
        for (const ui::ClipSel& selected : m_selection.clips()) {
            const daw::TrackModel* track =
                m_controller.project().findTrack(selected.trackId.toStdString());
            if (!track) continue;
            const auto clip = std::find_if(
                track->clips.begin(), track->clips.end(),
                [&](const daw::ClipModel& item) {
                    return item.id == selected.clipId.toStdString();
                });
            if (clip == track->clips.end() ||
                clip->kind == daw::ClipKind::Automation) {
                continue;
            }
            request.clips.push_back({track->id, clip->id});
            request.startSeconds = std::min(request.startSeconds,
                                            clip->startSeconds);
            request.endSeconds = std::max(
                request.endSeconds,
                clip->startSeconds + m_controller.clipPlaybackDuration(*clip));
        }
        sourceDescription = tr("%1 selected clip(s), %2–%3 s")
                                .arg(request.clips.size())
                                .arg(request.startSeconds, 0, 'f', 3)
                                .arg(request.endSeconds, 0, 'f', 3);
    }

    if (!(request.endSeconds > request.startSeconds) ||
        (!request.fullMix && request.clips.empty() && request.tracks.empty())) {
        statusBar()->showMessage(tr("Select clips, a region, or enable Loop"),
                                 4000);
        return;
    }

    BounceInPlaceDialog dialog(m_controller, std::move(request),
                               sourceDescription, this);
    dialog.exec();
    if (!dialog.rendered()) return;
    syncViews();
    m_selection.clear();
    markDirty();
    statusBar()->showMessage(tr("Bounce in Place complete"), 4000);
}

void MainWindow::onOfflineRender() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty()) {
        statusBar()->showMessage(tr("Offline Render — Local-only in v1"), 4000);
        return;
    }
#endif
    std::vector<daw::EngineController::ClipAddress> clips;
    for (const ui::ClipSel& selected : m_selection.clips()) {
        const daw::TrackModel* track =
            m_controller.project().findTrack(selected.trackId.toStdString());
        if (!track) continue;
        const auto clip = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const daw::ClipModel& item) {
                return item.id == selected.clipId.toStdString();
            });
        if (clip == track->clips.end() || clip->kind != daw::ClipKind::Audio) {
            QMessageBox::information(
                this, tr("Offline Render"),
                tr("Offline Render accepts audio clips only. Use Bounce in "
                   "Place for MIDI or Pattern clips first."));
            return;
        }
        clips.push_back({track->id, clip->id});
    }
    if (clips.empty()) {
        statusBar()->showMessage(tr("Select one or more audio clips"), 3000);
        return;
    }

    const auto primary = m_controller.offlineProcessChain(clips.front());
    const auto sameChain = [](const daw::EngineController::ChannelSnapshot& a,
                              const daw::EngineController::ChannelSnapshot& b) {
        const auto sameParameters = [](const auto& left, const auto& right) {
            if (left.size() != right.size()) return false;
            for (std::size_t i = 0; i < left.size(); ++i) {
                if (left[i].id != right[i].id ||
                    left[i].value != right[i].value)
                    return false;
            }
            return true;
        };
        if (a.inserts.size() != b.inserts.size()) return false;
        for (std::size_t i = 0; i < a.inserts.size(); ++i) {
            const auto& left = a.inserts[i];
            const auto& right = b.inserts[i];
            if (left.model.format != right.model.format ||
                left.model.uid != right.model.uid ||
                left.model.bypassed != right.model.bypassed ||
                left.model.mix != right.model.mix ||
                !sameParameters(left.model.parameters,
                                right.model.parameters) ||
                !sameParameters(left.model.rightParameters,
                                right.model.rightParameters) ||
                left.state != right.state ||
                left.rightState != right.rightState) {
                return false;
            }
        }
        return true;
    };
    bool chainsDiffer = false;
    for (std::size_t i = 1; i < clips.size(); ++i) {
        if (!sameChain(primary, m_controller.offlineProcessChain(clips[i]))) {
            chainsDiffer = true;
            break;
        }
    }

    OfflineRenderDialog dialog(m_controller, clips, chainsDiffer, this);
    dialog.exec();
    if (!dialog.rendered()) return;
    syncViews();
    m_selection.refresh();
    markDirty();
    statusBar()->showMessage(tr("Offline Render complete"), 4000);
}

// ── Project I/O ──

void MainWindow::onNewProject() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return;
#endif
    if (!maybeSaveChanges()) return;
#ifdef DAW_ENABLE_COLLABORATION
    clearCloudProjectBinding(/*cancelPublication=*/true);
#endif
    m_controller.newProject();
    m_projectPath.clear();
    m_selectedTrackId.clear();
    m_dirty = false;
    m_journal.setProjectPath({}, {});
    m_journalStale = true;
    m_transport->syncTempo();
    syncViews();
    updateWindowTitle();
}

QString MainWindow::chooseProjectTemplate() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Project from Template"));
    dialog.setModal(true);
    dialog.resize(460, 360);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* instructions = new QLabel(
        tr("Choose a saved template. The new project will be independent of "
           "the template file."),
        &dialog);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    auto* list = new QListWidget(&dialog);
    list->setAccessibleName(tr("Project templates"));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const QString& path : ui::projecttemplates::files()) {
        auto* item = new QListWidgetItem(
            icons::icon(icons::Glyph::Layers, th().textSecondary, 16),
            ui::projecttemplates::displayName(path), list);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    layout->addWidget(list, 1);

    auto* empty = new QLabel(
        tr("No templates have been saved yet. Use File → Save as Template… "
           "to create one."),
        &dialog);
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    empty->setVisible(list->count() == 0);
    list->setVisible(list->count() != 0);
    layout->addWidget(empty, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                              QDialogButtonBox::Cancel,
                                          &dialog);
    QPushButton* create = buttons->button(QDialogButtonBox::Ok);
    create->setText(tr("Create"));
    create->setEnabled(false);
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [create](QListWidgetItem* current, QListWidgetItem*) {
                create->setEnabled(current != nullptr);
            });
    connect(list, &QListWidget::itemDoubleClicked, &dialog,
            [&dialog](QListWidgetItem*) { dialog.accept(); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (list->count() > 0) list->setCurrentRow(0);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return {};
    return list->currentItem()->data(Qt::UserRole).toString();
}

void MainWindow::onNewProjectFromTemplate() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return;
#endif
    const QString path = chooseProjectTemplate();
    if (!path.isEmpty()) createProjectFromTemplatePath(path);
}

bool MainWindow::createProjectFromTemplatePath(const QString& packageDir) {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return false;
#endif
    if (packageDir.isEmpty() || !maybeSaveChanges()) return false;

    statusBar()->showMessage(tr("Creating project from template…"));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const audio::Result result =
        m_controller.openProjectTemplate(packageDir.toStdString());
    QApplication::restoreOverrideCursor();
    if (!result) {
        statusBar()->clearMessage();
        QMessageBox::warning(
            this, tr("Open Template Failed"),
            tr("The template could not be opened.\n\n%1\n\n"
               "Check that the package still contains Project.vlt, State, "
               "and any required Content files.")
                .arg(QString::fromStdString(result.message())));
        return false;
    }

#ifdef DAW_ENABLE_COLLABORATION
    clearCloudProjectBinding(/*cancelPublication=*/true);
#endif
    m_projectPath.clear();
    m_selectedTrackId.clear();
    m_dirty = false;
    m_journal.setProjectPath({}, {});
    m_journalStale = true;
    m_transport->syncTempo();
    syncViews();
    if (!m_controller.project().tracks.empty()) {
        selectTrackFromHeader(
            QString::fromStdString(m_controller.project().tracks.front().id));
    }
    updateWindowTitle();
    statusBar()->showMessage(
        tr("Created a new project from “%1”")
            .arg(ui::projecttemplates::displayName(packageDir)),
        4000);
    raise();
    activateWindow();
    return true;
}

void MainWindow::addProjectTemplateTracks(const QString& packageDir) {
    std::vector<std::string> added;
    statusBar()->showMessage(tr("Adding tracks from template…"));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const audio::Result result = m_controller.importProjectTemplateTracks(
        packageDir.toStdString(), added);
    QApplication::restoreOverrideCursor();
    if (!result) {
        statusBar()->clearMessage();
        QMessageBox::warning(
            this, tr("Add Template Tracks Failed"),
            tr("No tracks were added.\n\n%1\n\n"
               "The current project has not been changed.")
                .arg(QString::fromStdString(result.message())));
        return;
    }

    syncViews();
    if (!added.empty())
        selectTrackFromHeader(QString::fromStdString(added.front()));
    markDirty();
    statusBar()->showMessage(
        tr("Tracks added from “%1”: %2")
            .arg(ui::projecttemplates::displayName(packageDir))
            .arg(added.size()),
        4000);
}

void MainWindow::onOpenProject() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open VLT Project"), QString(),
        tr("VLT Project (*.vlt);;Project Template (*.vltt);;"
           "Legacy Project (project.json);;All Files (*)"));
    if (!path.isEmpty()) openProjectPath(path);
}

bool MainWindow::openProjectPath(const QString& path) {
    const QString dir = packagePathFromSelection(path);
    if (dir.isEmpty()) return false;
    if (ui::projecttemplates::isTemplatePackage(dir))
        return createProjectFromTemplatePath(dir);

    // Finder may deliver the same launch document once through argv and once
    // through QFileOpenEvent. Treat the second delivery as activation, not a
    // destructive reopen (and, importantly, do not ask about unsaved work).
    if (!m_projectPath.isEmpty() &&
        absoluteCleanPath(m_projectPath) == absoluteCleanPath(dir)) {
        raise();
        activateWindow();
        return true;
    }
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return false;
#endif
    if (!maybeSaveChanges()) return false;

    auto r = m_controller.openProject(dir.toStdString());
    if (!r) {
        QMessageBox::warning(this, tr("Open failed"),
                             tr("Could not open the VLT project.\n\n%1\n\n"
                                "Make sure the package still contains "
                                "Project.vlt and its Content folder.")
                                 .arg(QString::fromStdString(r.message())));
        return false;
    }
#ifdef DAW_ENABLE_COLLABORATION
    clearCloudProjectBinding(/*cancelPublication=*/true);
#endif
    m_projectPath = dir;
    m_selectedTrackId.clear();
    m_dirty = false;
    m_journal.setProjectPath(dir.toStdString(),
                             QFileInfo(dir).baseName().toStdString());
    m_journalStale = true;
    m_transport->syncTempo();
    syncViews();
    // Same as a fresh project: land on the first track so the context panel has
    // something to show straight away.
    if (!m_controller.project().tracks.empty()) {
        selectTrackFromHeader(
            QString::fromStdString(m_controller.project().tracks.front().id));
    }
    updateWindowTitle();
    statusBar()->showMessage(
        tr("Opened %1").arg(QFileInfo(dir).fileName()), 4000);
    raise();
    activateWindow();
    return true;
}

bool MainWindow::doSave(const QString& packageDir) {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return false;
#endif
    // A take in flight is not in the document yet. Land it before serializing
    // so Cmd+S can never report success while omitting the audio currently
    // being recorded.
    if (m_controller.isCountingIn()) cancelCountIn();
    if (m_controller.isRecording()) stopRecordingNow();

    QString dir = packagePathFromSelection(packageDir);
    const QString extension =
        QStringLiteral(".") + QString::fromLatin1(daw::ProjectSerializer::kExtension);
    if (!dir.endsWith(extension, Qt::CaseInsensitive)) dir += extension;
    dir = absoluteCleanPath(dir);

#ifdef DAW_ENABLE_COLLABORATION
    const bool isActivePublicationSource =
        m_publicationUi && m_publicationUi->active &&
        !m_publicationUi->sourcePath.isEmpty() &&
        absoluteCleanPath(m_publicationUi->sourcePath) == dir;
    const bool isCloudBackup =
        !m_cloudProjectId.isEmpty() && !m_cloudLocalBackupPath.isEmpty() &&
        absoluteCleanPath(m_cloudLocalBackupPath) == dir;
    if (isActivePublicationSource || isCloudBackup) {
        QMessageBox::warning(
            this, tr("Local Backup Is Protected"),
            tr("This file is the protected local source for cloud publication. "
               "Use Save As and choose a different name or folder for the "
               "new local copy."));
        return false;
    }
#endif

    // Save As gives the document its file name, as desktop applications do.
    // Revert it if persistence fails so a failed disk operation changes no
    // live project data.
    const std::string previousName = m_controller.projectName();
    const bool savingToNewPath = m_projectPath.isEmpty() ||
                                 absoluteCleanPath(m_projectPath) != dir;
    if (savingToNewPath) {
        m_controller.setProjectName(
            QFileInfo(dir).completeBaseName().toStdString());
    }
    auto r = m_controller.saveProject(dir.toStdString());
    if (!r) {
        m_controller.setProjectName(previousName);
        QMessageBox::warning(this, tr("Save failed"),
                             tr("The project was not saved.\n\n%1\n\n"
                                "Check free disk space, folder permissions, "
                                "and missing referenced files, then try again.")
                                 .arg(QString::fromStdString(r.message())));
        updateWindowTitle();
        return false;
    }
    m_projectPath = dir;
    m_dirty = false;
#ifdef DAW_ENABLE_COLLABORATION
    if (!m_cloudProjectId.isEmpty())
        clearCloudProjectBinding(/*cancelPublication=*/false);
#endif
    m_journal.setProjectPath(dir.toStdString(),
                             QFileInfo(dir).baseName().toStdString());
    updateWindowTitle();
    statusBar()->showMessage(
        tr("Saved %1").arg(QFileInfo(dir).fileName()), 4000);
    return true;
}

void MainWindow::onSaveProject() {
    if (m_projectPath.isEmpty()) onSaveProjectAs();
    else doSave(m_projectPath);
}

void MainWindow::onSaveProjectAs() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return;
#endif
    saveProjectAs();
}

void MainWindow::onSaveProjectTemplate() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return;
#endif
    bool accepted = false;
    const QString initial = displayProjectName(m_controller.projectName()).trimmed();
    const QString name = QInputDialog::getText(
                             this, tr("Save as Template"),
                             tr("Template name:"), QLineEdit::Normal, initial,
                             &accepted)
                             .trimmed();
    if (!accepted) return;

    const QString path = ui::projecttemplates::filePathForName(name);
    if (path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Invalid Template Name"),
            tr("Choose a non-empty name without /, \\, :, *, ?, \", <, >, "
               "or |."));
        return;
    }

    if (QFileInfo::exists(path)) {
        const auto replace = QMessageBox::question(
            this, tr("Replace Project Template"),
            tr("A template named “%1” already exists. Replace it?")
                .arg(ui::projecttemplates::displayName(path)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (replace != QMessageBox::Yes) return;
    }

    // Match ordinary Save: a take in flight must not race a snapshot of the
    // plugin graph. The landed clip is deliberately stripped from the template.
    if (m_controller.isCountingIn()) cancelCountIn();
    if (m_controller.isRecording()) stopRecordingNow();

    statusBar()->showMessage(tr("Saving template…"));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const audio::Result result = m_controller.saveProjectTemplate(
        path.toStdString(), name.toStdString());
    QApplication::restoreOverrideCursor();
    if (!result) {
        statusBar()->clearMessage();
        QMessageBox::warning(
            this, tr("Save Template Failed"),
            tr("The template was not saved.\n\n%1\n\n"
               "Check free disk space and folder permissions, then try again.")
                .arg(QString::fromStdString(result.message())));
        return;
    }

    if (m_browser) m_browser->reloadSettings();
    statusBar()->showMessage(tr("Saved template “%1”").arg(name), 4000);
}

bool MainWindow::saveProjectAs() {
#ifdef DAW_ENABLE_COLLABORATION
    if (!prepareCloudRecordingForProjectTransition()) return false;
#endif
    const QString dir = QFileDialog::getSaveFileName(
        this, tr("Save Project As"),
        displayProjectName(m_controller.projectName()) + ".vlt",
        tr("VLT Project (*.vlt)"));
    if (dir.isEmpty()) return false;
    return doSave(dir);
}

bool MainWindow::maybeSaveChanges() {
    // Automated/headless runs intentionally build and mutate throwaway
    // projects; never leave an invisible modal dialog waiting for them.
    if (!m_dirty || !m_persistGeometry) return true;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Unsaved Project"));
    box.setText(tr("Save changes to “%1”?")
                    .arg(displayProjectName(m_controller.projectName())));
    box.setInformativeText(
        tr("Your changes will be lost if you don’t save them."));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard |
                           QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);

    const auto choice = static_cast<QMessageBox::StandardButton>(box.exec());
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Discard) return true;
    if (choice == QMessageBox::Save) {
        return m_projectPath.isEmpty() ? saveProjectAs() : doSave(m_projectPath);
    }
    return false;
}

// ── Undo / redo ──

void MainWindow::onUndo() {
    if (m_webPanel && m_webPanel->handleUndoRedo(false)) return;
#ifdef DAW_ENABLE_COLLABORATION
    // This routes the main-window project history only. PianoRollWindow still
    // owns legacy editor-local undo paths and remains a mutation-coverage gate;
    // do not silently mix those closures into the cloud actor history.
    if (m_collaborationCommandBridge &&
        m_collaborationCommandBridge->handlesCloudBinding()) {
        m_collaborationCommandBridge->requestUndo();
        return;
    }
#endif
    if (!m_controller.canUndo()) return;
    m_controller.undo();
    m_transport->syncTempo();
    syncViews();
    markDirty();
}
void MainWindow::onRedo() {
    if (m_webPanel && m_webPanel->handleUndoRedo(true)) return;
#ifdef DAW_ENABLE_COLLABORATION
    if (m_collaborationCommandBridge &&
        m_collaborationCommandBridge->handlesCloudBinding()) {
        m_collaborationCommandBridge->requestRedo();
        return;
    }
#endif
    if (!m_controller.canRedo()) return;
    m_controller.redo();
    m_transport->syncTempo();
    syncViews();
    markDirty();
}

// ── Periodic refresh ──

void MainWindow::refreshPlayheadFrame() {
    const bool moving = m_controller.isPlaying() || m_controller.isRecording();
    const bool arrangementVisible = isVisible() && !isMinimized();
    const bool pianoVisible = m_pianoRoll && m_pianoRoll->isVisible() &&
                              !m_pianoRoll->isMinimized();
    if (!moving || (!arrangementVisible && !pianoVisible)) {
        // Also catches a transport transition made outside the normal buttons,
        // and lets playback continue without 62 wakeups/s while all consumers
        // are hidden or minimized.
        const bool wasActive = m_playheadTimer && m_playheadTimer->isActive();
        if (m_playheadTimer) m_playheadTimer->stop();
        if (wasActive && !moving) {
            if (arrangementVisible) {
                if (m_transport) m_transport->refreshPosition();
                if (m_timeline) m_timeline->refreshPlaybackFrame();
            }
            if (pianoVisible) m_pianoRoll->refreshPlayhead();
        }
        return;
    }

    if (arrangementVisible) {
        if (m_transport) m_transport->refreshPosition();
        if (m_timeline) {
            m_timeline->refreshPlaybackFrame();
            if (m_controller.isRecording())
                m_timeline->refreshRecordingFrame();
        }
    }
    if (pianoVisible) m_pianoRoll->refreshPlayhead();
}

void MainWindow::syncPlayheadTimer() {
    if (!m_playheadTimer) return;
    const bool moving = m_controller.isPlaying() || m_controller.isRecording();
    const bool arrangementVisible = isVisible() && !isMinimized();
    const bool pianoVisible = m_pianoRoll && m_pianoRoll->isVisible() &&
                              !m_pianoRoll->isMinimized();
    if (moving && (arrangementVisible || pianoVisible)) {
        if (!m_playheadTimer->isActive()) {
            refreshPlayheadFrame();
            m_playheadTimer->start();
        }
        return;
    }

    const bool wasActive = m_playheadTimer->isActive();
    m_playheadTimer->stop();
    if (wasActive) {
        // Paint the exact parked position once, including the old cursor strip.
        if (arrangementVisible) {
            if (m_transport) m_transport->refreshPosition();
            if (m_timeline) m_timeline->refreshPlaybackFrame();
        }
        if (pianoVisible) m_pianoRoll->refreshPlayhead();
    }
}

void MainWindow::refreshUi() {
    publishSessionTransport(false);
    updateLocalProcessingActions();
    const bool realtimeUi = m_controller.isPlaying() || m_controller.isRecording() ||
                            m_controller.isCountingIn();
    const int desiredInterval = realtimeUi ? 33 : 100;
    if (m_refreshTimer && m_refreshTimer->interval() != desiredInterval) {
        m_refreshTimer->setInterval(desiredInterval);
    }
    syncPlayheadTimer();
    if (m_orphanEditorSweepPending) {
        m_orphanEditorSweepPending = false;
        closeOrphanedPluginEditors();
    }
    // Driven from the tick rather than from a signal: the plate has to follow
    // scrolling, zooming and a clip being dragged, and those happen in half a
    // dozen places inside the timeline. Asking costs a few integer operations
    // and the panel ignores an answer that has not moved.
    if (m_contextPanel) m_contextPanel->followSelection();
    m_transport->refresh();
    if (m_trackList && m_trackList->isVisible()) m_trackList->refreshMeters();
    if (m_mixer && m_mixer->isVisible()) m_mixer->refreshMeters();
    if (m_inspector && m_inspector->isVisible()) m_inspector->refreshMeters();

    const bool automationPlaying = m_controller.isPlaying();
    if (automationPlaying) {
        if (m_trackList && m_trackList->isVisible())
            m_trackList->refreshAutomationValues();
        if (m_mixer && m_mixer->isVisible()) m_mixer->refreshAutomationValues();
        if (m_inspector && m_inspector->isVisible())
            m_inspector->refreshAutomationValues();
    } else if (m_wasAutomationPlaying) {
        // Return controls from their last automated positions to the document
        // once. Repeating this scan ten times a second while stopped was pure
        // work in large sessions.
        if (m_trackList) m_trackList->syncTrackValues();
        if (m_mixer) m_mixer->syncFromModel();
        if (m_inspector) m_inspector->syncFromModel();
    }
    m_wasAutomationPlaying = automationPlaying;

    // The take being recorded has no file yet, so its shape is sampled here —
    // once per frame, from the same input meters the mixer reads.
    if (m_controller.isRecording()) {
        m_controller.pumpRecordingEnvelopes();
        syncPendingTakeRows();
    } else if (m_hadRecordingRows) {
        syncPendingTakeRows();
    }

    // Plugins report back on their own schedule: a parameter moved in a plugin's
    // own editor, or a latency change after a preset load (which rebuilds the
    // graph so delay compensation follows). Polling here rather than signalling
    // keeps the controller free of any observer machinery, the same way the
    // recorder is handled.
    if (m_controller.pumpPluginEvents()) markDirty();

    const QString left = tr("%1 kHz   Buffer %2   %3 tracks   %4")
                             .arg(m_controller.sampleRate() / 1000.0, 0, 'f', 1)
                             .arg(m_controller.bufferSizeFrames())
                             .arg(m_controller.project().tracks.size())
                             .arg(displayProjectName(m_controller.projectName()));
    if (m_statusLeft->text() != left) m_statusLeft->setText(left);
    const QString right = tr("DSP %1%   %2")
                              .arg(int(m_controller.dspLoad() * 100))
                              .arg(m_controller.isPlaying() ? tr("Playing")
                                                            : tr("Stopped"));
    if (m_statusRight->text() != right) m_statusRight->setText(right);

    // "From clip": loop the selected clip — as soon as the playhead reaches the
    // clip's end it snaps back to its start and keeps playing, so playback
    // never runs past the clip and never stops. Checked on the refresh timer
    // (33 ms), which is plenty for a loop point.
    if (m_playRangeEnd >= 0.0 && m_controller.isPlaying() &&
        m_controller.positionSeconds() >= m_playRangeEnd) {
        m_controller.seekSeconds(m_playRangeStart);
    }

    if (m_controller.isRecording()) {
        // The envelope is sampled here; its growing edge and the playhead are
        // repainted by the independent 16 ms display clock.
    } else if (m_controller.isPlaying()) {
        // Cursor repainting is handled by the lightweight playhead clock.
    } else if (m_controller.isCountingIn()) {
        // The count pulses over the arrangement; the number itself is repainted
        // by the count-in's own tick, this only keeps the swell moving.
        m_timeline->update();
    }
}
