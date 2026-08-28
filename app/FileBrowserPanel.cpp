#include "FileBrowserPanel.hpp"
#include "ChannelStripPresets.hpp"

#include "BrowserPrefs.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "FileBrowserTree.hpp"
#include "ProjectTemplates.hpp"
#include "FileSearchWorker.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "PreviewLoader.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include "WaveformStrip.hpp"

#include <QFileDialog>
#include <QEventLoop>
#include <QFileInfo>
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
        tr("Search — name, .wav, .vlts or .vltt"));
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
            [this](const QStringList& paths, bool truncated) {
                m_tree->showResults(paths, truncated);
                emit statusMessage(truncated
                                       ? tr("%1 matches (more were found)")
                                             .arg(paths.size())
                                       : tr("%1 matches").arg(paths.size()));
            });

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        QStringList roots = ui::browserprefs::folders();
        roots.prepend(ui::channelstrippresets::rootFolder());
        roots.removeDuplicates();
        m_search->search(roots, m_searchField->text());
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
    return bar;
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
QTreeWidget::item { padding: %ROWPADPX%px 2px; }
QTreeWidget::item:selected { background: %SELECT%; color: %TEXT1%; }
)")
                      .replace("%TITLEPX%", px(10))
                      .replace("%SMALLPX%", px(10))
                      .replace("%BODYPX%", px(11))
                      .replace("%ROWPADPX%", px(2))
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
                      .replace("%TEXT1%", t.textPrimary.name())
                      .replace("%TEXT2%", t.textSecondary.name()));
    if (m_tree) {
        // The icons and the indent are widget properties, not stylesheet ones,
        // and a 14 px glyph beside a 22 px row is what a zoom that forgot them
        // looks like.
        const int glyph = std::max(10, int(std::lround(14.0 * m_zoom)));
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

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    const QMetaObject::Connection finished = connect(
        m_search, &FileSearchWorker::results, &loop,
        [&result, &loop](const QStringList& paths, bool) {
            result = paths;
            loop.quit();
        });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    QStringList roots = ui::browserprefs::folders();
    roots.prepend(ui::channelstrippresets::rootFolder());
    roots.removeDuplicates();
    m_search->search(roots, query);
    timeout.start(5000);
    loop.exec();
    disconnect(finished);
    return result;
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
    if (m_tree) m_tree->refresh();
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
    emit statusMessage(tr("Added %1").arg(folder));
}

void FileBrowserPanel::searchChanged(const QString& query) {
    if (query.trimmed().isEmpty()) {
        m_searchTimer->stop();
        m_search->cancel();
        m_tree->showTree();
        return;
    }
    m_searchTimer->start();
}

void FileBrowserPanel::selectFile(const QString& path) {
    m_selectedPath = path;
    const QFileInfo info(path);

    if (!ui::isAudioFile(path)) {
        // MIDI and everything else: no waveform, no audition, but the row is
        // still selectable and (for MIDI) draggable.
        m_loader->cancel();
        stopPreview();
        m_strip->clear(ui::isMidiFile(path) ? tr("MIDI file — drag it onto a track")
                                            : tr("Not an audio file"));
        setFileLabel(info.fileName(), path);
        refreshPreviewState();
        return;
    }

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
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() != root) continue;
        item->setExpanded(true);
        for (int c = 0; c < item->childCount(); ++c) {
            QTreeWidgetItem* child = item->child(c);
            if (child->data(0, Qt::UserRole).toString() != wanted) continue;
            m_tree->setCurrentItem(child);
            return true;
        }
    }
    return false;
}

bool FileBrowserPanel::hasPreviewWaveformForTest() const {
    return m_strip && m_strip->hasWaveform();
}

QStringList FileBrowserPanel::dragUrlsForTest() const {
    QStringList paths;
    std::unique_ptr<QMimeData> mime(m_tree ? m_tree->dragPayload() : nullptr);
    if (!mime) return paths;
    for (const QUrl& url : mime->urls()) paths << url.toLocalFile();
    return paths;
}
