#include "FileBrowserPanel.hpp"
#include "ChannelStripPresets.hpp"

#include "BrowserPrefs.hpp"
#include "BrowserSettingsPage.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "FileBrowserTree.hpp"
#include "ProjectTemplates.hpp"
#include "FileSearchWorker.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "MidiPreviewLoader.hpp"
#include "PreviewLoader.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include "WaveformStrip.hpp"

#include <QFileDialog>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QApplication>
#include <QThread>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QKeyEvent>
#include <QWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "platform/AudioFileDecoder.hpp"

namespace {
/// The folder a format is filed under in the browser. Spelled the way the
/// vendors do, not the way the cache does: "vst3" is a file extension, "VST3"
/// is what somebody is looking for.
QString pluginFormatFolder(daw::plugins::Format format) {
    switch (format) {
        case daw::plugins::Format::Clap:      return QObject::tr("CLAP");
        case daw::plugins::Format::Vst3:      return QObject::tr("VST3");
        case daw::plugins::Format::Vst:       return QObject::tr("VST");
        case daw::plugins::Format::AudioUnit: return QObject::tr("Audio Units");
        case daw::plugins::Format::Internal:  return QObject::tr("Built-in");
        case daw::plugins::Format::Unknown:   break;
    }
    return QObject::tr("Other");
}
} // namespace

namespace {

/// Typing pauses this long before a search starts, so a five-letter word is one
/// walk of the disk rather than five.
constexpr int kSearchDebounceMs = 200;
/// The audition head repaints only its old/new hairline strips, so it can move
/// at display cadence and still costs nothing when preview is stopped.
constexpr int kPlayheadPollMs = 16;
/// Past this, selecting a file does not audition it by itself. Decoding an hour
/// of audio because an arrow key moved is not what "click to hear it" means.
constexpr double kAutoPreviewMaxSeconds = 600.0;

QString describe(const audio::platform::AudioFileInfo& info) {
    const double seconds = info.durationSeconds();
    const int minutes = int(seconds) / 60;
    const int rest = int(seconds) % 60;
    return QStringLiteral("%1:%2 · %3 kHz · %4")
        .arg(minutes)
        .arg(rest, 2, 10, QLatin1Char('0'))
        .arg(info.sampleRate / 1000.0, 0, 'g', 4)
        .arg(info.channels == 1 ? QObject::tr("mono") : QObject::tr("stereo"));
}

} // namespace

FileBrowserPanel::FileBrowserPanel(daw::EngineController* controller,
                                   QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("BrowserPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(160);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(buildHeader());

    m_searchField = new QLineEdit(this);
    m_searchField->setObjectName("BrowserSearch");
    m_searchField->setPlaceholderText(
        tr("Search files and plugins…"));
    m_searchField->setAccessibleName(tr("Search files and plugins"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->addAction(icons::icon(icons::Glyph::Search, th().textSecondary, 13),
                             QLineEdit::LeadingPosition);
    connect(m_searchField, &QLineEdit::textChanged, this,
            &FileBrowserPanel::searchChanged);
    auto* searchRow = new QWidget(this);
    auto* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(6, 5, 6, 5);
    searchLayout->addWidget(m_searchField);
    column->addWidget(searchRow);

    m_tree = new FileBrowserTree(this);
    connect(m_tree, &FileBrowserTree::fileSelected, this,
            &FileBrowserPanel::selectFile);
    connect(m_tree, &FileBrowserTree::fileActivated, this,
            [this](const QString& path) {
                // Double-click restarts the audition and nothing else: the ways
                // into the project are the drags.
                startPreview(path);
            });
    connect(m_tree, &FileBrowserTree::channelStripPresetActivated, this,
            &FileBrowserPanel::channelStripPresetActivated);
    connect(m_tree, &FileBrowserTree::projectTemplateActivated, this,
            &FileBrowserPanel::projectTemplateActivated);
    connect(m_tree, &FileBrowserTree::projectTemplateTracksRequested, this,
            &FileBrowserPanel::projectTemplateTracksRequested);
    connect(m_tree, &FileBrowserTree::statusMessage, this,
            &FileBrowserPanel::statusMessage);
    column->addWidget(m_tree, 1);

    column->addWidget(buildPreviewBar());

    m_search = new FileSearchWorker(this);
    connect(m_search, &FileSearchWorker::results, this,
            [this](const QStringList& paths, bool truncated, bool finished) {
                const int count = m_tree->showResults(
                    paths, truncated, m_searchField->text().trimmed(), !finished);
                emit statusMessage(!finished ? tr("Searching… %1 matches").arg(count) : truncated
                                       ? tr("%1 matches (more were found)")
                                             .arg(count)
                                       : tr("%1 matches").arg(count));
            });

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        QStringList roots = ui::browserprefs::folders();
        roots.prepend(ui::channelstrippresets::rootFolder());
        roots.removeDuplicates();
        m_search->search(roots, m_searchField->text(), ui::browserprefs::ignoredExtensions());
    });

    m_loader = new PreviewLoader(this);
    connect(m_loader, &PreviewLoader::loaded, this,
            [this](const QString& path,
                   std::shared_ptr<const daw::engine::SampleBuffer> audio,
                   daw::WaveformPeaks peaks) {
                if (path != m_selectedPath) return;
                m_strip->setPeaks(peaks);
                if (!m_controller) return;
                // The decode is wanted for the waveform whether or not the
                // sound was asked for; only a decode that a *play* started
                // gets to make a noise.
                if (!m_playOnLoad) {
                    refreshPreviewState();
                    return;
                }
                m_playOnLoad = false;
                m_controller->setPreviewGain(ui::browserprefs::previewGain());
                m_controller->previewBuffer(std::move(audio), path.toStdString(),
                                            ui::browserprefs::previewLoop());
                m_playheadTimer->start();
                refreshPreviewState();
            });
    connect(m_loader, &PreviewLoader::failed, this,
            [this](const QString& path, const QString& reason) {
                if (path != m_selectedPath) return;
                m_playOnLoad = false;
                m_strip->clear(tr("Cannot read this file"));
                emit statusMessage(reason.isEmpty()
                                       ? tr("Cannot read %1").arg(QFileInfo(path).fileName())
                                       : reason);
            });

    m_midiLoader = new MidiPreviewLoader(this);
    connect(m_midiLoader, &MidiPreviewLoader::loaded, this,
            [this](const QString& path,
                   std::shared_ptr<const daw::midifile::File> file) {
                if (path != m_selectedPath || !ui::isMidiFile(path)) return;
                if (!file || file->notes.empty()) {
                    m_strip->clear(tr("This MIDI file contains no notes"));
                    return;
                }
                m_strip->setMidi(file);
                const QString details =
                    tr("MIDI · %1 notes · %2 tracks · %3 beats")
                        .arg(qulonglong(file->notes.size()))
                        .arg(file->tracksWithNotes())
                        .arg(file->lengthBeats, 0, 'f',
                             file->lengthBeats < 16.0 ? 1 : 0);
                setFileLabel(QStringLiteral("%1  ·  %2")
                                 .arg(QFileInfo(path).fileName(), details),
                             path);
            });
    connect(m_midiLoader, &MidiPreviewLoader::failed, this,
            [this](const QString& path, const QString& reason) {
                if (path != m_selectedPath || !ui::isMidiFile(path)) return;
                m_strip->clear(tr("Cannot read this MIDI file"));
                emit statusMessage(reason.isEmpty()
                                       ? tr("Cannot read %1")
                                             .arg(QFileInfo(path).fileName())
                                       : reason);
            });

    m_playheadTimer = new QTimer(this);
    m_playheadTimer->setTimerType(Qt::PreciseTimer);
    m_playheadTimer->setInterval(kPlayheadPollMs);
    connect(m_playheadTimer, &QTimer::timeout, this, [this] {
        if (!m_controller) return;
        if (!m_controller->previewPlaying()) {
            m_strip->setPlayheadSeconds(-1.0);
            m_playheadTimer->stop();
            refreshPreviewState();
            return;
        }
        m_strip->setPlayheadSeconds(m_controller->previewPositionSeconds());
    });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &FileBrowserPanel::applyTheme);
    applyTheme();
    reloadSettings();
}

FileBrowserPanel::~FileBrowserPanel() {
    // The panel outliving a playing audition would leave the engine looping a
    // file nobody can see any more.
    if (m_controller) m_controller->stopPreview();
}

QWidget* FileBrowserPanel::buildHeader() {
    auto* header = new QWidget(this);
    header->setObjectName("BrowserHeader");
    header->setFixedHeight(ui::kRulerHeight);

    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(8, 0, 4, 0);
    row->setSpacing(2);

    auto* title = new QLabel(tr("BROWSER"), header);
    title->setObjectName("BrowserTitle");
    row->addWidget(title, 1);

    auto* add = new ui::IconButton(icons::Glyph::Plus, tr("Add a folder…"), header);
    add->setButtonSize(22, 20);
    connect(add, &QAbstractButton::clicked, this,
            &FileBrowserPanel::requestAddFolder);
    row->addWidget(add);

    auto* refresh =
        new ui::IconButton(icons::Glyph::Restart, tr("Re-read the folders"), header);
    refresh->setButtonSize(22, 20);
    connect(refresh, &QAbstractButton::clicked, this,
            &FileBrowserPanel::refreshFolders);
    row->addWidget(refresh);

    auto* settings = new ui::IconButton(icons::Glyph::Gear,
                                        tr("Browser settings"), header);
    settings->setButtonSize(22, 20);
    connect(settings, &QAbstractButton::clicked, this,
            &FileBrowserPanel::settingsRequested);
    row->addWidget(settings);
    return header;
}

QWidget* FileBrowserPanel::buildPreviewBar() {
    auto* bar = new QWidget(this);
    m_previewBar = bar;
    bar->setObjectName("BrowserPreview");

    auto* column = new QVBoxLayout(bar);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    m_strip = new WaveformStrip(bar);
    connect(m_strip, &WaveformStrip::seekRequested, this, [this](double seconds) {
        if (!m_controller) return;
        // Clicking the wave while nothing plays starts it there, which is what
        // pointing at a spot in a sound means.
        if (!m_controller->previewPlaying() && !m_selectedPath.isEmpty()) {
            startPreview(m_selectedPath);
        }
        m_controller->seekPreviewSeconds(seconds);
        m_strip->setPlayheadSeconds(seconds);
    });
    column->addWidget(m_strip);

    auto* row = new QWidget(bar);
    auto* controls = new QHBoxLayout(row);
    controls->setContentsMargins(6, 4, 6, 5);
    controls->setSpacing(3);

    m_playButton = new ui::IconButton(icons::Glyph::Play, tr("Play the selection"), row);
    m_playButton->setButtonSize(22, 20);
    m_playButton->setAccentTint(true);
    connect(m_playButton, &QAbstractButton::clicked, this,
            &FileBrowserPanel::togglePreview);
    controls->addWidget(m_playButton);

    m_loopButton = new ui::IconButton(icons::Glyph::Loop,
                                      tr("Repeat, instead of playing once"), row);
    m_loopButton->setButtonSize(22, 20);
    m_loopButton->setCheckable(true);
    m_loopButton->setChecked(ui::browserprefs::previewLoop());
    connect(m_loopButton, &QAbstractButton::toggled, this, [this](bool on) {
        ui::browserprefs::setPreviewLoop(on);
        if (m_controller) m_controller->setPreviewLoop(on);
    });
    controls->addWidget(m_loopButton);

    m_autoButton = new ui::IconButton(icons::Glyph::Headphones,
                                      tr("Play a file as soon as it is selected"), row);
    m_autoButton->setButtonSize(22, 20);
    m_autoButton->setCheckable(true);
    m_autoButton->setChecked(ui::browserprefs::autoPreview());
    connect(m_autoButton, &QAbstractButton::toggled, this, [this](bool on) {
        ui::browserprefs::setAutoPreview(on);
    });
    controls->addWidget(m_autoButton);

    m_fileLabel = new QLabel(tr("No file selected"), row);
    m_fileLabel->setObjectName("BrowserFileLabel");
    m_fileLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    // The panel is narrow and the name is the part worth reading, so the label
    // elides in the middle rather than being cut off wherever the panel ends.
    m_fileLabel->setMinimumWidth(40);
    m_fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controls->addWidget(m_fileLabel, 1);

    column->addWidget(row);
    bar->hide();
    return bar;
}

void FileBrowserPanel::setPreviewVisible(bool visible, bool audioControls) {
    if (m_previewBar) m_previewBar->setVisible(visible);
    if (m_playButton) m_playButton->setVisible(visible && audioControls);
    if (m_loopButton) m_loopButton->setVisible(visible && audioControls);
    if (m_autoButton) m_autoButton->setVisible(visible && audioControls);
}

void FileBrowserPanel::setFileLabel(const QString& text, const QString& tip) {
    m_fileLabelText = text;
    if (!m_fileLabel) return;
    m_fileLabel->setToolTip(tip.isEmpty() ? text : tip);
    // Elided from the right, not the middle: the name is what identifies the
    // file, and the metadata after it is the part that can be cut.
    m_fileLabel->setText(m_fileLabel->fontMetrics().elidedText(
        text, Qt::ElideRight, std::max(40, m_fileLabel->width())));
}

void FileBrowserPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Re-elide against the new width; a dragged panel edge changes how much of
    // the name fits.
    setFileLabel(m_fileLabelText, m_fileLabel ? m_fileLabel->toolTip() : QString());
}

void FileBrowserPanel::applyTheme() {
    const Theme& t = th();
    const QString edge = m_onLeft ? QStringLiteral("border-right")
                                  : QStringLiteral("border-left");
    // Every size in the sheet below is the design size times the browser's own
    // zoom. Scaling here rather than with a QFont on the panel is what keeps
    // the *relative* sizes — a 10 px caption over an 11 px row — instead of
    // flattening them all to one scaled base.
    const auto px = [this](double base) {
        return QString::number(std::max(1, int(std::lround(base * m_zoom))));
    };
    setStyleSheet(QString(R"(
#BrowserPanel { background: %SURFACE%; %EDGE%: 1px solid %SECTION%; }
#BrowserHeader { background: %HEADER%; border-bottom: 1px solid %SECTION%; }
#BrowserTitle { color: %TEXT2%; font-size: %TITLEPX%px; font-weight: 700;
                letter-spacing: 0.6px; }
#BrowserFileLabel { color: %TEXT2%; font-size: %SMALLPX%px; }
#BrowserSearch { background: %WELL%; border: 1px solid %SEP%; border-radius: 7px;
                 padding: %PADPX%px 6px; color: %TEXT1%; font-size: %BODYPX%px; }
#BrowserSearch:focus { border-color: %ACCENT%; }
#BrowserPreview { background: %TOOLBAR%; border-top: 1px solid %SECTION%; }
QTreeWidget { background: %SURFACE%; border: none; color: %TEXT1%;
              font-size: %BODYPX%px; }
QTreeWidget::item { padding: %ROWPADPX%px 3px; margin: 1px 4px 1px 2px;
                    border-radius: %ROUNDPX%px; }
QTreeWidget::item:hover:!selected { background: %HOVER%; }
QTreeWidget::item:selected { background: %SELECT%; color: %TEXT1%; }
)")
                      .replace("%TITLEPX%", px(10))
                      .replace("%SMALLPX%", px(10))
                      .replace("%BODYPX%", px(11))
                      .replace("%ROWPADPX%", px(2))
                      .replace("%ROUNDPX%", px(6))
                      .replace("%PADPX%", px(3))
                      .replace("%EDGE%", edge)
                      .replace("%SURFACE%", t.surface.name())
                      .replace("%TOOLBAR%", t.toolbarBackground.name())
                      .replace("%HEADER%", mixColors(t.toolbarBackground,
                                                       t.surfaceElevated, 0.22).name())
                      .replace("%WELL%", t.well().name())
                      .replace("%SEP%", t.separator().name())
                      .replace("%SECTION%", t.sectionDivider().name())
                      .replace("%ACCENT%", t.accent.name())
                      .replace("%SELECT%", t.selection.name())
                      .replace("%HOVER%", mixColors(t.surface, t.textPrimary,
                                                     t.dark ? 0.06 : 0.045).name())
                      .replace("%TEXT1%", t.textPrimary.name())
                      .replace("%TEXT2%", t.textSecondary.name()));
    if (m_tree) {
        // The icons and the indent are widget properties, not stylesheet ones,
        // and a 14 px glyph beside a 22 px row is what a zoom that forgot them
        // looks like.
        const int glyph = std::max(14, int(std::lround(18.0 * m_zoom)));
        m_tree->setIconSize(QSize(glyph, glyph));
        m_tree->setIndentation(std::max(8, int(std::lround(12.0 * m_zoom))));
    }
    // The tree builds its rows with themed icons and colours, so it is rebuilt
    // rather than merely repainted when the palette changes.
    if (m_tree && !m_tree->showingResults()) m_tree->refresh();
}

void FileBrowserPanel::setZoom(double factor) {
    const double next = std::clamp(factor, ui::browserprefs::kMinZoom,
                                   ui::browserprefs::kMaxZoom);
    if (std::abs(next - m_zoom) < 0.001) return;
    m_zoom = next;
    ui::browserprefs::setZoom(m_zoom);
    applyTheme();
    emit statusMessage(tr("Browser at %1%").arg(int(std::lround(m_zoom * 100.0))));
}

void FileBrowserPanel::zoomBy(double step) { setZoom(m_zoom * step); }

void FileBrowserPanel::wheelEvent(QWheelEvent* event) {
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QWidget::wheelEvent(event);
        return;
    }
    const int ticks = event->angleDelta().y();
    if (ticks != 0) zoomBy(ticks > 0 ? 1.1 : 1.0 / 1.1);
    event->accept();
}

QMimeData* FileBrowserPanel::dragPayloadForTest() const {
    return m_tree ? m_tree->dragPayload() : nullptr;
}

QMimeData* FileBrowserPanel::pluginDragForTest() const {
    return dragPayloadForTest();
}

QStringList FileBrowserPanel::searchForTest(const QString& query) {
    QStringList result;
    if (!m_search || query.trimmed().isEmpty()) return result;
    m_searchTimer->stop();

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    const QMetaObject::Connection finished = connect(
        m_search, &FileSearchWorker::results, &loop,
        [&result, &loop](const QStringList& paths, bool, bool finished) {
            if (!finished) return;
            result = paths;
            loop.quit();
        });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    // This regression searches its temporary preset fixtures. User folders
    // can be network volumes and must not determine a fixed test timeout.
    const QStringList roots{ui::channelstrippresets::rootFolder()};
    m_search->search(roots, query, ui::browserprefs::ignoredExtensions());
    timeout.start(5000);
    loop.exec();
    disconnect(finished);
    return result;
}

bool FileBrowserPanel::checkSearchForTest(QObject* keyboardTarget) {
    const auto fail = [](const char* reason) {
        std::fprintf(stderr, "browser search: %s\n", reason);
        return false;
    };
    QTemporaryDir fixture;
    if (!fixture.isValid()) return fail("cannot create fixture");
    QDir root(fixture.path());
    root.mkpath(QStringLiteral("nested/Template.vltt"));
    root.mkpath(QStringLiteral("folder.asd"));
    const QStringList names{QStringLiteral("Kick.WAV"), QStringLiteral("Kick.wav.ASD"),
        QStringLiteral("notes.TMP"), QStringLiteral("readme.txt"),
        QStringLiteral("nested/Bass.flac"), QStringLiteral("nested/Template.vltt/Project.vlt")};
    for (const QString& name : names) {
        QFile file(root.filePath(name));
        if (!file.open(QIODevice::WriteOnly)) return fail("cannot write fixture");
    }
    const auto savedFolders = ui::browserprefs::folders();
    const auto savedIgnored = ui::browserprefs::ignoredExtensions();
    const auto restore = qScopeGuard([&] {
        m_searchField->clear();
        m_search->cancel();
        ui::browserprefs::setFolders(savedFolders);
        ui::browserprefs::setIgnoredExtensions(savedIgnored.join(QLatin1Char(',')));
        reloadSettings();
    });
    ui::browserprefs::setFolders({fixture.path(), root.filePath(QStringLiteral("nested"))});
    ui::browserprefs::setIgnoredExtensions({});
    reloadSettings();
    const auto waitFor = [](auto predicate) {
        QElapsedTimer timer; timer.start();
        while (!predicate() && timer.elapsed() < 5000) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return predicate();
    };
    bool finished = false;
    QStringList paths;
    const auto connection = connect(m_search, &FileSearchWorker::results, this,
        [&](const QStringList& found, bool, bool done) {
            if (done) { paths = found; finished = true; }
        });
    const auto disconnectSearch = qScopeGuard([&] { disconnect(connection); });

    // Exercise real typing, including Quick-to-QWidget keyboard forwarding.
    // Consume Windows' first-show hidden hint in a headless native check.
    window()->hide(); window()->show(); window()->activateWindow();
    if (auto* inputWindow = qobject_cast<QWindow*>(keyboardTarget)) inputWindow->requestActivate();
    QApplication::processEvents();
    QApplication::setActiveWindow(window()); // Offscreen platforms do not activate native windows.
    m_searchField->setFocus(Qt::OtherFocusReason);
    if (!waitFor([&] { return QApplication::focusWidget() == m_searchField; }))
    {
        std::fprintf(stderr, "browser search focus: visible=%d size=%dx%d active=%d focus=%s\n",
            m_searchField->isVisible(), width(), height(), window()->isActiveWindow(),
            QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none");
        return fail("search field did not receive focus");
    }
    QObject* target = keyboardTarget ? keyboardTarget : m_searchField;
    for (QChar letter : QStringLiteral("kick")) {
        QKeyEvent overrideKey(QEvent::ShortcutOverride, letter.toUpper().unicode(), Qt::NoModifier, QString(letter));
        QKeyEvent press(QEvent::KeyPress, letter.toUpper().unicode(), Qt::NoModifier, QString(letter));
        QKeyEvent release(QEvent::KeyRelease, letter.toUpper().unicode(), Qt::NoModifier, QString(letter));
        QApplication::sendEvent(target, &overrideKey);
        QApplication::sendEvent(target, &press);
        QApplication::sendEvent(target, &release);
    }
    if (m_searchField->text() != QLatin1String("kick") || !waitFor([&] { return finished; }) ||
        !paths.contains(root.filePath(QStringLiteral("Kick.WAV"))) || paths.size() != 2)
        return fail("typing did not find case-insensitive file names");

    // Commit the real preference field while results are open; it must restart
    // the same query and remove sidecars without closing the browser.
    BrowserSettingsPage settings;
    connect(&settings, &BrowserSettingsPage::changed, this, &FileBrowserPanel::reloadSettings);
    auto* ignored = settings.findChild<QLineEdit*>(QStringLiteral("BrowserIgnoredExtensions"));
    if (!ignored) return fail("ignored extensions field is missing");
    finished = false;
    ignored->setText(QStringLiteral(".ASD; *.tmp, asd"));
    QMetaObject::invokeMethod(ignored, "editingFinished", Qt::DirectConnection);
    if (ui::browserprefs::ignoredExtensions() != QStringList{QStringLiteral("asd"), QStringLiteral("tmp")} ||
        !waitFor([&] { return finished; }) || paths != QStringList{root.filePath(QStringLiteral("Kick.WAV"))})
        return fail("extension preference did not update active search");

    const auto search = [&](const QString& query) {
        finished = false;
        m_searchField->setText(query);
        return waitFor([&] { return finished; });
    };
    if (!search(QStringLiteral("*.FLAC")) || paths != QStringList{root.filePath(QStringLiteral("nested/Bass.flac"))})
        return fail("nested extension search failed or overlapping roots duplicated files");
    if (!search(QStringLiteral(".asd")) || !paths.isEmpty())
        return fail("ignored files leaked into extension search");
    if (!search(QStringLiteral("Project.vlt")) || !paths.isEmpty())
        return fail("search exposed template implementation files");
    if (!search(QStringLiteral(".vltt")) || paths != QStringList{root.filePath(QStringLiteral("nested/Template.vltt"))})
        return fail("template was not returned as one package");

    FileBrowserTree::PluginEntry plugin;
    plugin.name = QStringLiteral("Needle Synth"); plugin.vendor = QStringLiteral("Fixture Audio");
    plugin.uid = QStringLiteral("test.search.synth"); plugin.formatName = QStringLiteral("Test");
    m_tree->setPlugins({plugin});
    if (!search(QStringLiteral("needle")) || m_tree->pluginRowCountForTest() != 1 ||
        !m_tree->selectFirstPluginForTest()) return fail("plugin name search failed");
    const std::unique_ptr<QMimeData> mime(m_tree->dragPayload());
    int format = 0; QString uid;
    if (!ui::decodePluginRef(mime.get(), format, uid) || uid != plugin.uid)
        return fail("plugin result lost its drag payload");
    if (!search(QStringLiteral("fixture audio")) || m_tree->pluginRowCountForTest() != 1)
        return fail("plugin vendor search failed");

    // An already queued result must not land during the next debounce period.
    m_search->search({fixture.path()}, QStringLiteral("kick"));
    if (!search(QStringLiteral("bass")) || paths != QStringList{root.filePath(QStringLiteral("nested/Bass.flac"))})
        return fail("an obsolete query replaced current results");
    m_searchField->clear();
    if (m_tree->showingResults()) return fail("clearing query did not restore folders");
    QTreeWidgetItem* folder = nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (m_tree->topLevelItem(i)->data(0, Qt::UserRole).toString() == fixture.path())
            folder = m_tree->topLevelItem(i);
    if (!folder) return fail("fixture root disappeared");
    folder->setExpanded(true);
    const auto childNames = [&] {
        QStringList result;
        for (int i = 0; i < folder->childCount(); ++i) result << folder->child(i)->text(0);
        return result;
    };
    if (!waitFor([&] { return childNames().contains(QStringLiteral("Kick.WAV")); }) ||
        childNames().contains(QStringLiteral("Kick.wav.ASD")) ||
        childNames().contains(QStringLiteral("notes.TMP")) ||
        !childNames().contains(QStringLiteral("folder.asd")))
        return fail("folder enumeration did not respect ignored suffixes");

    const QString text = root.filePath(QStringLiteral("readme.txt"));
    m_tree->showResults({text}, false, QStringLiteral("text"), true);
    auto* selected = m_tree->topLevelItem(0);
    m_tree->setCurrentItem(selected);
    m_tree->showResults({text, root.filePath(QStringLiteral("notes.TMP"))}, false, QStringLiteral("text"));
    if (m_tree->currentItem() != selected) return fail("incremental results discarded selection");
    m_tree->showTree();
    finished = false;
    ignored->clear();
    QMetaObject::invokeMethod(ignored, "editingFinished", Qt::DirectConnection);
    if (!search(QStringLiteral(".asd")) || paths != QStringList{root.filePath(QStringLiteral("Kick.wav.ASD"))})
        return fail("clearing ignored extensions did not restore files");
    std::fprintf(stderr, "browser search: files, plugins, typing, cancellation, suffixes and live settings PASS\n");
    return true;
}

bool FileBrowserPanel::selectedProjectTemplateForTest() const {
    return m_tree && m_tree->selectedProjectTemplateForTest();
}

bool FileBrowserPanel::activateSelectedProjectTemplateForTest() {
    return m_tree && m_tree->activateSelectedProjectTemplateForTest();
}

bool FileBrowserPanel::showPluginsForTest() {
    reloadPlugins();
    return m_tree && m_tree->selectFirstPluginForTest();
}

bool FileBrowserPanel::containersAreNavigationOnlyForTest() const {
    return m_tree && m_tree->containersAreNavigationOnlyForTest();
}

void FileBrowserPanel::reloadPlugins() {
    if (!m_tree || !m_controller) return;
    QVector<FileBrowserTree::PluginEntry> entries;
    for (const auto& descriptor : m_controller->pluginManager().plugins()) {
        FileBrowserTree::PluginEntry entry;
        entry.name = QString::fromStdString(descriptor.name);
        entry.vendor = QString::fromStdString(descriptor.vendor);
        entry.uid = QString::fromStdString(descriptor.uid);
        entry.path = QString::fromStdString(descriptor.path);
        entry.format = int(descriptor.format);
        entry.formatName = pluginFormatFolder(descriptor.format);
        entry.instrument = descriptor.isInstrument;
        entries.push_back(entry);
    }
    m_tree->setPlugins(entries);
    if (!m_searchField->text().trimmed().isEmpty()) searchChanged(m_searchField->text());
}

void FileBrowserPanel::setOnLeft(bool onLeft) {
    if (m_onLeft == onLeft) return;
    m_onLeft = onLeft;
    applyTheme();
}

void FileBrowserPanel::reloadSettings() {
    m_zoom = ui::browserprefs::zoom();
    applyTheme();
    (void)ui::projecttemplates::folder();
    m_tree->setPresetRoot(ui::channelstrippresets::rootFolder());
    m_tree->setRoots(ui::browserprefs::folders());
    reloadPlugins();
    if (m_loopButton) m_loopButton->setChecked(ui::browserprefs::previewLoop());
    if (m_autoButton) m_autoButton->setChecked(ui::browserprefs::autoPreview());
    if (m_controller) {
        m_controller->setPreviewGain(ui::browserprefs::previewGain());
        m_controller->setPreviewLoop(ui::browserprefs::previewLoop());
    }
}

void FileBrowserPanel::requestAddFolder() { addFolder(); }

void FileBrowserPanel::refreshFolders() {
    if (!m_searchField->text().trimmed().isEmpty()) searchChanged(m_searchField->text());
    else if (m_tree) m_tree->refresh();
}

bool FileBrowserPanel::hasPreviewableSelection() const {
    return !m_selectedPath.isEmpty() && ui::isAudioFile(m_selectedPath);
}

void FileBrowserPanel::togglePreview() {
    if (m_controller && m_controller->previewPlaying()) {
        stopPreview();
    } else if (hasPreviewableSelection()) {
        startPreview(m_selectedPath);
    }
}

void FileBrowserPanel::setPreviewLoopEnabled(bool enabled) {
    if (m_loopButton) {
        if (m_loopButton->isChecked() != enabled)
            m_loopButton->setChecked(enabled);
        return;
    }
    ui::browserprefs::setPreviewLoop(enabled);
    if (m_controller) m_controller->setPreviewLoop(enabled);
}

void FileBrowserPanel::setAutoPreviewEnabled(bool enabled) {
    if (m_autoButton) {
        if (m_autoButton->isChecked() != enabled)
            m_autoButton->setChecked(enabled);
        return;
    }
    ui::browserprefs::setAutoPreview(enabled);
}

void FileBrowserPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_tree->topLevelItemCount() == 0) m_tree->setRoots(ui::browserprefs::folders());
}

void FileBrowserPanel::addFolder() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Add a folder to the browser"), QString());
    if (folder.isEmpty()) return;
    if (!ui::browserprefs::addFolder(folder)) {
        emit statusMessage(tr("%1 is already in the browser").arg(folder));
        return;
    }
    m_tree->setRoots(ui::browserprefs::folders());
    if (!m_searchField->text().trimmed().isEmpty()) searchChanged(m_searchField->text());
    emit statusMessage(tr("Added %1").arg(folder));
}

void FileBrowserPanel::searchChanged(const QString& query) {
    // Invalidate old results immediately, including the debounce interval.
    m_search->cancel();
    if (query.trimmed().isEmpty()) {
        m_searchTimer->stop();
        m_tree->showTree();
        return;
    }
    m_tree->showResults({}, false, query.trimmed(), true);
    m_searchTimer->start();
}

void FileBrowserPanel::selectFile(const QString& path) {
    m_selectedPath = path;
    const QFileInfo info(path);
    const bool audioFile = ui::isAudioFile(path);
    const bool midiFile = ui::isMidiFile(path);

    if (!audioFile && !midiFile) {
        m_loader->cancel();
        m_midiLoader->cancel();
        stopPreview();
        m_strip->clear();
        setFileLabel({});
        setPreviewVisible(false, false);
        refreshPreviewState();
        return;
    }

    setPreviewVisible(true, audioFile);

    if (midiFile) {
        m_loader->cancel();
        stopPreview();
        m_strip->clear(tr("Reading MIDI…"));
        setFileLabel(info.fileName(), path);
        m_midiLoader->request(path);
        refreshPreviewState();
        return;
    }

    m_midiLoader->cancel();

    // The probe is a header read, so the size and shape of a file are known
    // before deciding whether auditioning it is reasonable.
    audio::platform::AudioFileInfo probed;
    const bool known =
        audio::platform::probeAudioFile(path.toStdString(), probed).isOk();
    setFileLabel(known ? QStringLiteral("%1  ·  %2")
                             .arg(info.fileName(), describe(probed))
                       : info.fileName(),
                 path);

    if (known && probed.durationSeconds() > kAutoPreviewMaxSeconds) {
        m_loader->cancel();
        stopPreview();
        m_strip->clear(tr("Long file — press play to hear it"));
        refreshPreviewState();
        return;
    }

    if (ui::browserprefs::autoPreview()) {
        startPreview(path);
    } else {
        // Still decode: the waveform is wanted even when the sound is not.
        m_playOnLoad = false;
        m_strip->clear(tr("Reading…"));
        m_loader->request(path);
    }
    refreshPreviewState();
}

void FileBrowserPanel::startPreview(const QString& path) {
    if (path.isEmpty() || !ui::isAudioFile(path)) return;
    m_selectedPath = path;
    m_playOnLoad = true;
    // The decode happens on a worker; the audition starts when it lands. That
    // is the whole reason the browser does not call the controller's own
    // `previewFile`, which decodes where it is called.
    m_loader->request(path);
    m_strip->clear(tr("Reading…"));
}

void FileBrowserPanel::stopPreview() {
    // A decode still in flight was started to be heard; stopping means it is
    // not, or it would burst into sound when it lands.
    m_playOnLoad = false;
    if (m_controller) m_controller->stopPreview();
    if (m_playheadTimer) m_playheadTimer->stop();
    if (m_strip) m_strip->setPlayheadSeconds(-1.0);
    refreshPreviewState();
}

void FileBrowserPanel::refreshPreviewState() {
    if (!m_playButton) return;
    const bool playing = m_controller && m_controller->previewPlaying();
    const bool available = hasPreviewableSelection();
    m_playButton->setGlyph(playing ? icons::Glyph::Stop : icons::Glyph::Play);
    m_playButton->setToolTip(playing ? tr("Stop") : tr("Play the selection"));
    m_playButton->setEnabled(available);
    emit previewAvailabilityChanged(available);
}

bool FileBrowserPanel::showFolderForTest(const QString& folder,
                                         const QString& selectFile, bool persist) {
    if (folder.isEmpty()) return false;
    // The tree stores absolute paths; a caller passing QDir::tempPath() (which
    // can carry a trailing slash) must still match.
    const QString root = QFileInfo(folder).absoluteFilePath();
    const QString wanted =
        selectFile.isEmpty() ? QString() : QFileInfo(selectFile).absoluteFilePath();

    QStringList roots = ui::browserprefs::folders();
    if (!roots.contains(root)) roots.prepend(root);
    if (persist) ui::browserprefs::setFolders(roots);
    m_tree->setRoots(roots);

    if (wanted.isEmpty()) return m_tree->topLevelItemCount() > 0;
    // Enumeration and chunk insertion are asynchronous. Re-resolve items on
    // every turn, since a watcher refresh may replace them while we wait.
    QElapsedTimer timeout;
    timeout.start();
    do {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            auto* item = m_tree->topLevelItem(i);
            if (item->data(0, Qt::UserRole).toString() != root) continue;
            item->setExpanded(true);
            for (int c = 0; c < item->childCount(); ++c) {
                auto* child = item->child(c);
                if (child->data(0, Qt::UserRole).toString() != wanted) continue;
                m_tree->setCurrentItem(child);
                return true;
            }
        }
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    } while (timeout.elapsed() < 5000);
    return false;
}

bool FileBrowserPanel::reloadSelectedPreviewForTest() {
    if (!hasPreviewableSelection()) return false;
    const QString path = m_selectedPath;
    selectFile(path);
    return true;
}

bool FileBrowserPanel::hasPreviewWaveformForTest() const {
    return m_strip && m_strip->hasWaveform();
}

bool FileBrowserPanel::hasMidiPreviewForTest() const {
    return m_strip && m_strip->hasMidiPreview();
}

bool FileBrowserPanel::previewVisibleForTest() const {
    return m_previewBar && !m_previewBar->isHidden();
}

bool FileBrowserPanel::clearFileSelectionForTest() {
    if (!m_tree) return false;
    m_tree->setCurrentIndex({});
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    return !previewVisibleForTest();
}

QStringList FileBrowserPanel::dragUrlsForTest() const {
    QStringList paths;
    std::unique_ptr<QMimeData> mime(m_tree ? m_tree->dragPayload() : nullptr);
    if (!mime) return paths;
    for (const QUrl& url : mime->urls()) paths << url.toLocalFile();
    return paths;
}
