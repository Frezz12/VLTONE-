#include "ExportDialog.hpp"

#include "EngineController.hpp"
#include "SelectionModel.hpp"
#include "ExportPrefs.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ap = audio::platform;
namespace rd = daw::rendering;

namespace {

/// Containers offered, in the order a musician would look for them. The list is
/// filtered against `isWriteSpecSupported` before it reaches the menu, so a
/// build without LAME simply has no MP3 entry rather than an entry that fails.
struct ContainerEntry {
    ap::Container container;
    const char* label;
};
const ContainerEntry kContainers[] = {
    {ap::Container::Wav, QT_TRANSLATE_NOOP("ExportDialog", "WAV")},
    {ap::Container::Aiff, QT_TRANSLATE_NOOP("ExportDialog", "AIFF")},
    {ap::Container::Flac, QT_TRANSLATE_NOOP("ExportDialog", "FLAC")},
    {ap::Container::Mp3, QT_TRANSLATE_NOOP("ExportDialog", "MP3")},
    {ap::Container::OggVorbis, QT_TRANSLATE_NOOP("ExportDialog", "Ogg Vorbis")},
    {ap::Container::Opus, QT_TRANSLATE_NOOP("ExportDialog", "Opus")},
    {ap::Container::Caf, QT_TRANSLATE_NOOP("ExportDialog", "CAF")},
    {ap::Container::W64, QT_TRANSLATE_NOOP("ExportDialog", "Wave64")},
};

const double kSampleRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
const int kMp3Bitrates[] = {128, 192, 256, 320};

bool isLossy(ap::Container container) {
    return container == ap::Container::Mp3 ||
           container == ap::Container::OggVorbis ||
           container == ap::Container::Opus;
}

/// Roughly how large the result will be. Exact for PCM; for a codec it is the
/// nominal rate, which is what a user wants to know before committing.
double bytesPerSecond(const rd::Spec& spec, double rate, int channels) {
    switch (spec.file.encoding) {
        case ap::Encoding::Int16: return rate * channels * 2;
        case ap::Encoding::Int24: return rate * channels * 3;
        case ap::Encoding::Int32:
        case ap::Encoding::Float32: return rate * channels * 4;
        case ap::Encoding::Float64: return rate * channels * 8;
        case ap::Encoding::Mp3:
            return (spec.file.bitrateKbps > 0 ? spec.file.bitrateKbps : 192) * 125.0;
        case ap::Encoding::Vorbis:
        case ap::Encoding::Opus:
            // FLAC is not here: it is lossless and its size depends entirely on
            // the material, so guessing a ratio would be worse than a range.
            return (64.0 + 384.0 * spec.file.vbrQuality) * 125.0;
    }
    return rate * channels * 3;
}

QString formatSize(double bytes) {
    if (bytes >= 1024.0 * 1024.0 * 1024.0) {
        return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
}

QString formatTime(double seconds) {
    const int total = int(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60)
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

ExportDialog::ExportDialog(daw::EngineController& controller,
                           const ui::SelectionModel* selection, QWidget* parent)
    : QDialog(parent), m_controller(controller), m_selection(selection) {
    setWindowTitle(tr("Render — %1").arg(QApplication::applicationName()));
    setModal(true);
    setSizeGripEnabled(true);
    resize(640, 860);

    buildUi();

    m_populating = true;
    populateChannels();
    populateContainers();

    const rd::Spec remembered = ui::exportprefs::load();
    m_folder->setText(QDir::toNativeSeparators(ui::exportprefs::lastFolder()));
    QString projectName =
        QString::fromStdString(m_controller.projectName()).trimmed();
    if (projectName == QLatin1String("Untitled"))
        projectName = tr("Untitled");
    m_baseName->setText(projectName);
    if (m_baseName->text().isEmpty()) m_baseName->setText(tr("Mixdown"));

    for (int i = 0; i < m_container->count(); ++i) {
        if (m_container->itemData(i).toInt() == int(remembered.file.container)) {
            m_container->setCurrentIndex(i);
            break;
        }
    }
    repopulateEncodings();
    for (int i = 0; i < m_encoding->count(); ++i) {
        if (m_encoding->itemData(i).toInt() == int(remembered.file.encoding)) {
            m_encoding->setCurrentIndex(i);
            break;
        }
    }
    repopulateSampleRates();
    for (int i = 0; i < m_sampleRate->count(); ++i) {
        if (std::fabs(m_sampleRate->itemData(i).toDouble() -
                      remembered.sampleRate) < 0.01) {
            m_sampleRate->setCurrentIndex(i);
            break;
        }
    }
    m_fileChannels->setCurrentIndex(
        remembered.channels == rd::Channels::Mono ? 1 : 0);

    m_writeMixdown->setChecked(remembered.writeMixdown);
    m_bypassInserts->setChecked(remembered.bypassChannelInserts);
    m_bypassMaster->setChecked(remembered.bypassMasterChain);
    m_ignoreMuteSolo->setChecked(remembered.ignoreMuteSolo);
    m_preFaderStems->setChecked(remembered.stemsPreFader);
    m_dither->setChecked(remembered.file.dither);
    m_artist->setText(QString::fromStdString(remembered.tags.artist));
    m_preRoll->setValue(remembered.preRollSeconds);

    m_tail->setCurrentIndex(int(remembered.tail));
    m_tailSeconds->setValue(remembered.tailSeconds);
    m_tailSilenceDb->setValue(remembered.tailSilenceDb);
    m_tailMaxSeconds->setValue(remembered.tailMaxSeconds);

    switch (remembered.range) {
        case rd::Range::CycleRegion: m_rangeCycle->setChecked(true); break;
        case rd::Range::Custom: m_rangeCustom->setChecked(true); break;
        case rd::Range::WholeProject: m_rangeWhole->setChecked(true); break;
    }
    m_populating = false;

    syncRangeFields();
    syncEnabledState();
    updateSummary();

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ExportDialog::applyTheme);
    applyTheme();
}

// ── Construction ───────────────────────────────────────────────────────────

void ExportDialog::buildUi() {
    auto* shell = new QVBoxLayout(this);
    shell->setContentsMargins(12, 12, 12, 12);
    shell->setSpacing(10);

    // The form is taller than a laptop screen once every section is open, so it
    // scrolls inside the dialog instead of pushing the Render button off the
    // bottom edge where nobody can reach it.
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(scroll);
    scroll->setWidget(page);
    shell->addWidget(scroll, 1);

    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto rebuildSummary = [this] {
        if (!m_populating) updateSummary();
    };
    auto rebuildAll = [this] {
        if (m_populating) return;
        syncEnabledState();
        updateSummary();
    };

    // ── Destination ──
    {
        auto* box = new QGroupBox(tr("Destination"), this);
        auto* form = new QFormLayout(box);
        auto* row = new QHBoxLayout;
        m_folder = new QLineEdit(box);
        m_browse = new QPushButton(tr("Browse…"), box);
        row->addWidget(m_folder, 1);
        row->addWidget(m_browse);
        form->addRow(tr("Folder"), row);
        m_baseName = new QLineEdit(box);
        form->addRow(tr("Name"), m_baseName);
        m_title = new QLineEdit(box);
        m_title->setPlaceholderText(tr("defaults to each file's name"));
        m_artist = new QLineEdit(box);
        m_comment = new QLineEdit(box);
        form->addRow(tr("Title"), m_title);
        form->addRow(tr("Artist"), m_artist);
        form->addRow(tr("Comment"), m_comment);
        root->addWidget(box);

        connect(m_browse, &QPushButton::clicked, this, [this] {
            const QString chosen = QFileDialog::getExistingDirectory(
                this, tr("Render into folder"), m_folder->text());
            if (!chosen.isEmpty()) {
                m_folder->setText(QDir::toNativeSeparators(chosen));
            }
        });
        connect(m_baseName, &QLineEdit::textChanged, this, rebuildSummary);
    }

    // ── What to write ──
    {
        m_stemsBox = new QGroupBox(tr("What to render"), this);
        auto* column = new QVBoxLayout(m_stemsBox);
        m_writeMixdown = new QCheckBox(tr("Master mix"), m_stemsBox);
        m_writeStems = new QCheckBox(tr("Separate stems"), m_stemsBox);
        column->addWidget(m_writeMixdown);
        column->addWidget(m_writeStems);

        m_channels = new QListWidget(m_stemsBox);
        m_channels->setMinimumHeight(120);
        column->addWidget(m_channels, 1);

        auto* buttons = new QHBoxLayout;
        m_allChannels = new QPushButton(tr("All"), m_stemsBox);
        m_selectedChannels = new QPushButton(tr("Selected"), m_stemsBox);
        m_noChannels = new QPushButton(tr("None"), m_stemsBox);
        buttons->addWidget(m_allChannels);
        buttons->addWidget(m_selectedChannels);
        buttons->addWidget(m_noChannels);
        buttons->addStretch(1);
        column->addLayout(buttons);
        m_stemWarning = new QLabel(m_stemsBox);
        m_stemWarning->setObjectName(QStringLiteral("ExportWarning"));
        m_stemWarning->setWordWrap(true);
        m_stemWarning->hide();
        column->addWidget(m_stemWarning);
        root->addWidget(m_stemsBox, 1);

        auto setAll = [this](Qt::CheckState state) {
            for (int i = 0; i < m_channels->count(); ++i) {
                m_channels->item(i)->setCheckState(state);
            }
            updateSummary();
        };
        connect(m_allChannels, &QPushButton::clicked, this,
                [setAll] { setAll(Qt::Checked); });
        connect(m_noChannels, &QPushButton::clicked, this,
                [setAll] { setAll(Qt::Unchecked); });
        connect(m_selectedChannels, &QPushButton::clicked, this, [this] {
            QStringList wanted;
            if (m_selection) {
                wanted = m_selection->tracks();
                for (const ui::ClipSel& clip : m_selection->clips()) {
                    if (!wanted.contains(clip.trackId)) wanted << clip.trackId;
                }
            }
            for (int i = 0; i < m_channels->count(); ++i) {
                QListWidgetItem* item = m_channels->item(i);
                item->setCheckState(wanted.contains(item->data(Qt::UserRole).toString())
                                        ? Qt::Checked
                                        : Qt::Unchecked);
            }
            updateSummary();
        });
        connect(m_channels, &QListWidget::itemChanged, this, rebuildSummary);
        connect(m_writeMixdown, &QCheckBox::toggled, this, rebuildAll);
        connect(m_writeStems, &QCheckBox::toggled, this, rebuildAll);
    }

    // ── Range and tail ──
    {
        auto* box = new QGroupBox(tr("Range"), this);
        auto* grid = new QGridLayout(box);
        m_rangeWhole = new QRadioButton(tr("Whole project"), box);
        m_rangeCycle = new QRadioButton(tr("Cycle region"), box);
        m_rangeSelection = new QRadioButton(tr("Selection"), box);
        m_rangeCustom = new QRadioButton(tr("Custom"), box);
        grid->addWidget(m_rangeWhole, 0, 0);
        grid->addWidget(m_rangeCycle, 0, 1);
        grid->addWidget(m_rangeSelection, 1, 0);
        grid->addWidget(m_rangeCustom, 1, 1);

        auto* times = new QHBoxLayout;
        m_rangeStart = new QDoubleSpinBox(box);
        m_rangeEnd = new QDoubleSpinBox(box);
        for (QDoubleSpinBox* spin : {m_rangeStart, m_rangeEnd}) {
            spin->setRange(0.0, 24.0 * 3600.0);
            spin->setDecimals(3);
            spin->setSuffix(tr(" s"));
        }
        times->addWidget(new QLabel(tr("From"), box));
        times->addWidget(m_rangeStart);
        times->addWidget(new QLabel(tr("to"), box));
        times->addWidget(m_rangeEnd);
        times->addStretch(1);
        grid->addLayout(times, 2, 0, 1, 2);

        auto* tailRow = new QHBoxLayout;
        m_tail = new QComboBox(box);
        m_tail->addItem(tr("No tail"));
        m_tail->addItem(tr("Fixed length"));
        m_tail->addItem(tr("Until silence"));
        m_tailSeconds = new QDoubleSpinBox(box);
        m_tailSeconds->setRange(0.0, 600.0);
        m_tailSeconds->setSuffix(tr(" s"));
        m_tailSilenceDb = new QDoubleSpinBox(box);
        m_tailSilenceDb->setRange(-160.0, -20.0);
        m_tailSilenceDb->setSuffix(tr(" dB"));
        m_tailMaxSeconds = new QDoubleSpinBox(box);
        m_tailMaxSeconds->setRange(1.0, 600.0);
        m_tailMaxSeconds->setPrefix(tr("max "));
        m_tailMaxSeconds->setSuffix(tr(" s"));
        tailRow->addWidget(new QLabel(tr("Tail"), box));
        tailRow->addWidget(m_tail);
        tailRow->addWidget(m_tailSeconds);
        tailRow->addWidget(m_tailSilenceDb);
        tailRow->addWidget(m_tailMaxSeconds);
        tailRow->addStretch(1);
        grid->addLayout(tailRow, 3, 0, 1, 2);

        auto* preRollRow = new QHBoxLayout;
        m_preRoll = new QDoubleSpinBox(box);
        m_preRoll->setRange(0.0, 60.0);
        m_preRoll->setSuffix(tr(" s"));
        m_preRoll->setToolTip(
            tr("Play this much of the arrangement before the range and throw it "
               "away, so a range starting mid-project opens with the reverb "
               "that was already ringing instead of from silence."));
        preRollRow->addWidget(new QLabel(tr("Pre-roll"), box));
        preRollRow->addWidget(m_preRoll);
        preRollRow->addStretch(1);
        grid->addLayout(preRollRow, 4, 0, 1, 2);
        connect(m_preRoll, &QDoubleSpinBox::valueChanged, this, rebuildSummary);
        root->addWidget(box);

        for (QRadioButton* button :
             {m_rangeWhole, m_rangeCycle, m_rangeSelection, m_rangeCustom}) {
            connect(button, &QRadioButton::toggled, this, [this](bool on) {
                if (!on || m_populating) return;
                syncRangeFields();
                syncEnabledState();
                updateSummary();
            });
        }
        connect(m_tail, &QComboBox::currentIndexChanged, this, rebuildAll);
        for (QDoubleSpinBox* spin : {m_rangeStart, m_rangeEnd, m_tailSeconds,
                                     m_tailMaxSeconds}) {
            connect(spin, &QDoubleSpinBox::valueChanged, this, rebuildSummary);
        }
    }

    // ── Processing ──
    {
        auto* box = new QGroupBox(tr("Processing"), this);
        auto* column = new QVBoxLayout(box);
        m_bypassInserts = new QCheckBox(tr("Bypass channel effects"), box);
        m_bypassMaster = new QCheckBox(tr("Bypass master chain"), box);
        m_ignoreMuteSolo = new QCheckBox(tr("Ignore mute and solo"), box);
        m_preFaderStems =
            new QCheckBox(tr("Stems ignore faders and pan (pre-fader)"), box);
        m_preFaderStems->setToolTip(
            tr("Take each stem from ahead of its fader, so it arrives at unity "
               "with pan centred. The master mix is unaffected."));
        m_dither = new QCheckBox(tr("Dither to 16/24-bit"), box);
        m_dither->setToolTip(
            tr("Adds a whisper of noise on the way down to a fixed-point word, "
               "so a quiet fade ends in noise rather than in distortion. Only "
               "affects integer formats; ignored by float and by MP3."));
        for (QCheckBox* box2 : {m_bypassInserts, m_bypassMaster, m_ignoreMuteSolo,
                                m_preFaderStems, m_dither}) {
            column->addWidget(box2);
            connect(box2, &QCheckBox::toggled, this, rebuildSummary);
        }
        root->addWidget(box);
    }

    // ── Format ──
    {
        auto* box = new QGroupBox(tr("Format"), this);
        auto* form = new QFormLayout(box);
        m_formatForm = form;
        m_container = new QComboBox(box);
        m_encoding = new QComboBox(box);
        m_quality = new QComboBox(box);
        m_sampleRate = new QComboBox(box);
        m_fileChannels = new QComboBox(box);
        m_fileChannels->addItem(tr("Stereo"));
        m_fileChannels->addItem(tr("Mono"));
        form->addRow(tr("File type"), m_container);
        form->addRow(tr("Depth"), m_encoding);
        form->addRow(tr("Quality"), m_quality);
        form->addRow(tr("Sample rate"), m_sampleRate);
        form->addRow(tr("Channels"), m_fileChannels);
        root->addWidget(box);

        connect(m_container, &QComboBox::currentIndexChanged, this, [this] {
            if (m_populating) return;
            repopulateEncodings();
            repopulateSampleRates();
            syncEnabledState();
            updateSummary();
        });
        connect(m_encoding, &QComboBox::currentIndexChanged, this, rebuildAll);
        connect(m_quality, &QComboBox::currentIndexChanged, this, rebuildSummary);
        connect(m_sampleRate, &QComboBox::currentIndexChanged, this, [this] {
            if (m_populating) return;
            repopulateEncodings();
            updateSummary();
        });
        connect(m_fileChannels, &QComboBox::currentIndexChanged, this,
                [this] {
                    if (m_populating) return;
                    repopulateEncodings();
                    updateSummary();
                });
    }

    root->addStretch(1);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("ExportSummary"));
    m_summary->setWordWrap(true);
    shell->addWidget(m_summary);

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();
    shell->addWidget(m_progress);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("ExportStatus"));
    m_status->hide();
    shell->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel,
                                     this);
    m_renderButton = m_buttons->button(QDialogButtonBox::Ok);
    m_renderButton->setText(tr("Render"));
    m_renderButton->setDefault(true);
    shell->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this,
            &ExportDialog::startRender);
    connect(m_buttons, &QDialogButtonBox::rejected, this, [this] {
        // While a render is running, Cancel stops it rather than closing the
        // dialog out from under a pass that is still writing files.
        if (m_rendering) {
            m_cancelRequested = true;
            m_status->setText(tr("Cancelling…"));
        } else {
            reject();
        }
    });
}

// ── Population ─────────────────────────────────────────────────────────────

void ExportDialog::populateChannels() {
    m_channels->clear();
    const daw::ProjectModel& project = m_controller.project();
    for (const daw::TrackModel& track : project.tracks) {
        // Exactly the tracks the engine builds a strip for, so the list can
        // never offer a channel that has no tap point.
        if (!daw::carriesAudio(track)) continue;
        const int depth = daw::trackDepth(project, track.id);
        auto* item = new QListWidgetItem(
            QString(depth * 4, QLatin1Char(' ')) +
                QString::fromStdString(track.name),
            m_channels);
        item->setData(Qt::UserRole, QString::fromStdString(track.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    auto* master = new QListWidgetItem(tr("Master"), m_channels);
    master->setData(Qt::UserRole,
                    QString::fromLatin1(daw::EngineController::kMasterChannelId));
    master->setFlags(master->flags() | Qt::ItemIsUserCheckable);
    master->setCheckState(Qt::Unchecked);
}

void ExportDialog::populateContainers() {
    m_container->clear();
    for (const ContainerEntry& entry : kContainers) {
        // A container earns its place only if at least one of its encodings can
        // actually be written here — that is what keeps MP3 off a build with no
        // LAME instead of offering it and failing at sf_open.
        bool usable = false;
        for (ap::Encoding encoding : ap::encodingsFor(entry.container)) {
            ap::WriteSpec probe;
            probe.container = entry.container;
            probe.encoding = encoding;
            for (double rate : kSampleRates) {
                if (ap::isWriteSpecSupported(probe, 2, rate)) {
                    usable = true;
                    break;
                }
            }
            if (usable) break;
        }
        if (!usable) continue;
        m_container->addItem(tr(entry.label), int(entry.container));
    }
}

void ExportDialog::repopulateEncodings() {
    const QSignalBlocker blocker(m_encoding);
    const auto container =
        ap::Container(m_container->currentData().toInt());
    const int previous = m_encoding->currentData().isValid()
                             ? m_encoding->currentData().toInt()
                             : -1;
    const double rate = m_sampleRate->currentData().isValid() &&
                                m_sampleRate->currentData().toDouble() > 0.0
                            ? m_sampleRate->currentData().toDouble()
                            : m_controller.sampleRate();
    const audio::ChannelCount channels =
        m_fileChannels->currentIndex() == 1 ? 1 : 2;

    m_encoding->clear();
    for (ap::Encoding encoding : ap::encodingsFor(container)) {
        ap::WriteSpec probe;
        probe.container = container;
        probe.encoding = encoding;
        if (!ap::isWriteSpecSupported(probe, channels, rate)) continue;
        m_encoding->addItem(
            QString::fromUtf8(ap::describeEncoding(encoding).data(),
                              int(ap::describeEncoding(encoding).size())),
            int(encoding));
    }
    for (int i = 0; i < m_encoding->count(); ++i) {
        if (m_encoding->itemData(i).toInt() == previous) {
            m_encoding->setCurrentIndex(i);
            break;
        }
    }

    const QSignalBlocker qualityBlocker(m_quality);
    m_quality->clear();
    if (container == ap::Container::Mp3) {
        for (int kbps : kMp3Bitrates) {
            m_quality->addItem(tr("%1 kbps CBR").arg(kbps), kbps);
        }
        m_quality->addItem(tr("VBR (best)"), 0);
        m_quality->setCurrentIndex(m_quality->count() - 2);  // 320 kbps
    } else if (isLossy(container)) {
        const int steps[] = {40, 60, 75, 90, 100};
        for (int step : steps) {
            m_quality->addItem(tr("Quality %1%").arg(step), step);
        }
        m_quality->setCurrentIndex(3);  // 90%
    }
}

void ExportDialog::repopulateSampleRates() {
    const QSignalBlocker blocker(m_sampleRate);
    const double previous = m_sampleRate->currentData().isValid()
                                ? m_sampleRate->currentData().toDouble()
                                : 0.0;
    const auto container = ap::Container(m_container->currentData().toInt());
    const audio::ChannelCount channels =
        m_fileChannels->currentIndex() == 1 ? 1 : 2;

    // Every entry is filtered, the project's own rate included. Adding that one
    // unconditionally is what let a 44.1 kHz project offer Opus at its own rate
    // and then refuse the render, since Opus encodes at only a few rates.
    auto writable = [&](double rate) {
        for (ap::Encoding encoding : ap::encodingsFor(container)) {
            ap::WriteSpec probe;
            probe.container = container;
            probe.encoding = encoding;
            if (ap::isWriteSpecSupported(probe, channels, rate)) return true;
        }
        return false;
    };

    m_sampleRate->clear();
    if (writable(m_controller.sampleRate())) {
        m_sampleRate->addItem(
            tr("Project (%1 Hz)").arg(int(m_controller.sampleRate())), 0.0);
    }
    for (double rate : kSampleRates) {
        // Opus, for one, encodes at only a handful of rates. Asking the writer
        // rather than hard-coding a table keeps this honest per container.
        if (!writable(rate)) continue;
        m_sampleRate->addItem(tr("%1 Hz").arg(int(rate)), rate);
    }
    for (int i = 0; i < m_sampleRate->count(); ++i) {
        if (std::fabs(m_sampleRate->itemData(i).toDouble() - previous) < 0.01) {
            m_sampleRate->setCurrentIndex(i);
            break;
        }
    }
}

// ── State ──────────────────────────────────────────────────────────────────

bool ExportDialog::selectionRange(double& start, double& end) const {
    if (!m_selection) return false;
    const daw::ProjectModel& project = m_controller.project();
    bool any = false;
    for (const ui::ClipSel& selected : m_selection->clips()) {
        const daw::TrackModel* track =
            project.findTrack(selected.trackId.toStdString());
        if (!track) continue;
        for (const daw::ClipModel& clip : track->clips) {
            if (clip.id != selected.clipId.toStdString()) continue;
            const double from = clip.startSeconds;
            const double to = clip.startSeconds + clip.durationSeconds;
            start = any ? std::min(start, from) : from;
            end = any ? std::max(end, to) : to;
            any = true;
        }
    }
    return any && end > start;
}

void ExportDialog::syncRangeFields() {
    const QSignalBlocker startBlocker(m_rangeStart);
    const QSignalBlocker endBlocker(m_rangeEnd);
    if (m_rangeWhole->isChecked()) {
        m_rangeStart->setValue(0.0);
        m_rangeEnd->setValue(m_controller.durationSeconds());
    } else if (m_rangeCycle->isChecked()) {
        m_rangeStart->setValue(m_controller.loopStartSeconds());
        m_rangeEnd->setValue(m_controller.loopEndSeconds());
    } else if (m_rangeSelection->isChecked()) {
        double start = 0.0;
        double end = 0.0;
        if (selectionRange(start, end)) {
            m_rangeStart->setValue(start);
            m_rangeEnd->setValue(end);
        }
    }
}

void ExportDialog::syncEnabledState() {
    const bool stems = m_writeStems->isChecked();
    m_channels->setEnabled(stems);
    m_allChannels->setEnabled(stems);
    m_selectedChannels->setEnabled(stems);
    m_noChannels->setEnabled(stems);
    // Pre-fader capture is a property of a stem; with only a mixdown to write
    // there is nothing for it to apply to.
    m_preFaderStems->setEnabled(stems);

    const bool custom = m_rangeCustom->isChecked();
    m_rangeStart->setEnabled(custom);
    m_rangeEnd->setEnabled(custom);

    double start = 0.0;
    double end = 0.0;
    m_rangeSelection->setEnabled(selectionRange(start, end));

    const int tail = m_tail->currentIndex();
    m_tailSeconds->setVisible(tail == int(rd::Tail::Fixed));
    m_tailSilenceDb->setVisible(tail == int(rd::Tail::UntilSilence));
    m_tailMaxSeconds->setVisible(tail == int(rd::Tail::UntilSilence));

    // Rows are hidden whole: hiding just the field would leave its caption
    // sitting on an empty line. A lossy container has exactly one encoding and
    // a quality to pick; a PCM one is the other way round.
    const auto container = ap::Container(m_container->currentData().toInt());
    m_formatForm->setRowVisible(m_quality, isLossy(container));
    m_formatForm->setRowVisible(m_encoding, !isLossy(container));

    if (!m_rendering) {
        m_renderButton->setEnabled(m_writeMixdown->isChecked() ||
                                   (stems && m_channels->count() > 0));
    }
}

QString ExportDialog::stemSelectionWarning() const {
    if (!m_writeStems->isChecked()) return {};

    QSet<QString> chosen;
    for (int i = 0; i < m_channels->count(); ++i) {
        const QListWidgetItem* item = m_channels->item(i);
        if (item->checkState() == Qt::Checked) {
            chosen.insert(item->data(Qt::UserRole).toString());
        }
    }
    if (chosen.isEmpty()) return {};

    const daw::ProjectModel& project = m_controller.project();
    const QString master =
        QString::fromLatin1(daw::EngineController::kMasterChannelId);

    // Two ways the "stems add up to the mix" guarantee stops holding, and the
    // dialog is the only place that can see either coming.
    bool overlapping = chosen.contains(master) && chosen.size() > 1;
    bool missingReturn = false;
    for (const daw::TrackModel& track : project.tracks) {
        const QString id = QString::fromStdString(track.id);
        if (!chosen.contains(id)) continue;

        // Counted twice: a track and something it feeds are both being written.
        const std::string bus = track.outputBusId.empty()
                                    ? daw::summingParent(project, track.id)
                                    : track.outputBusId;
        if (!bus.empty() && chosen.contains(QString::fromStdString(bus))) {
            overlapping = true;
        }
        // Missing: this track sends somewhere that is not being written, so its
        // share of that return exists in the mix and in no stem.
        for (const daw::SendModel& send : track.sends) {
            if (!send.enabled) continue;
            if (!chosen.contains(QString::fromStdString(send.destinationTrackId))) {
                missingReturn = true;
            }
        }
    }

    if (overlapping && missingReturn) {
        return tr("Stems overlap and a send return is missing — they will not "
                  "add up to the mix.");
    }
    if (overlapping) {
        return tr("A track and a bus it feeds are both selected, so that audio "
                  "is written twice.");
    }
    if (missingReturn) {
        return tr("A selected track sends to a bus that is not selected, so its "
                  "return is missing from the stems.");
    }
    return {};
}

void ExportDialog::updateSummary() {
    const rd::Spec spec = collectSpec();
    const double rate =
        spec.sampleRate > 0.0 ? spec.sampleRate : m_controller.sampleRate();
    const int channels = spec.channels == rd::Channels::Mono ? 1 : 2;

    double seconds = std::max(0.0, spec.customEndSeconds - spec.customStartSeconds);
    if (spec.tail == rd::Tail::Fixed) seconds += spec.tailSeconds;

    const int files = (spec.writeMixdown ? 1 : 0) + int(spec.stemChannelIds.size());
    const double bytes = seconds * bytesPerSecond(spec, rate, channels) * files;

    QString text = (files == 1 ? tr("1 file") : tr("%1 files").arg(files)) +
                   QStringLiteral(" · ") + formatTime(seconds) +
                   QStringLiteral(" · ~") + formatSize(bytes);
    if (spec.file.container == ap::Container::Flac) {
        text += tr(" (FLAC will be smaller)");
    }
    if (files == 0) text = tr("Nothing selected to render.");
    m_summary->setText(text);

    const QString warning = stemSelectionWarning();
    m_stemWarning->setText(warning);
    m_stemWarning->setVisible(!warning.isEmpty());
}

daw::rendering::Spec ExportDialog::collectSpec() const {
    rd::Spec spec;
    spec.outputDir = QDir::fromNativeSeparators(m_folder->text()).toStdString();
    spec.baseName = m_baseName->text().trimmed().toStdString();
    if (spec.baseName.empty()) spec.baseName = "Mixdown";

    spec.file.container = ap::Container(m_container->currentData().toInt());
    spec.file.encoding = m_encoding->currentData().isValid()
                             ? ap::Encoding(m_encoding->currentData().toInt())
                             : ap::encodingsFor(spec.file.container).front();
    if (spec.file.container == ap::Container::Mp3) {
        spec.file.bitrateKbps = m_quality->currentData().toInt();
        spec.file.vbrQuality = 1.0;
    } else if (isLossy(spec.file.container)) {
        spec.file.vbrQuality = m_quality->currentData().toInt() / 100.0;
    }

    spec.sampleRate = m_sampleRate->currentData().toDouble();
    spec.channels = m_fileChannels->currentIndex() == 1 ? rd::Channels::Mono
                                                        : rd::Channels::Stereo;

    // The dialog resolves every range to explicit seconds, including the
    // arrangement selection, which the controller knows nothing about.
    spec.range = rd::Range::Custom;
    spec.customStartSeconds = m_rangeStart->value();
    spec.customEndSeconds = m_rangeEnd->value();

    spec.preRollSeconds = m_preRoll->value();
    spec.tail = rd::Tail(m_tail->currentIndex());
    spec.tailSeconds = m_tailSeconds->value();
    spec.tailSilenceDb = m_tailSilenceDb->value();
    spec.tailMaxSeconds = m_tailMaxSeconds->value();

    spec.writeMixdown = m_writeMixdown->isChecked();
    if (m_writeStems->isChecked()) {
        for (int i = 0; i < m_channels->count(); ++i) {
            const QListWidgetItem* item = m_channels->item(i);
            if (item->checkState() != Qt::Checked) continue;
            spec.stemChannelIds.push_back(
                item->data(Qt::UserRole).toString().toStdString());
        }
    }

    spec.bypassChannelInserts = m_bypassInserts->isChecked();
    spec.bypassMasterChain = m_bypassMaster->isChecked();
    spec.ignoreMuteSolo = m_ignoreMuteSolo->isChecked();
    spec.stemsPreFader = m_preFaderStems->isChecked();
    spec.file.dither = m_dither->isChecked();

    spec.tags.title = m_title->text().trimmed().toStdString();
    spec.tags.artist = m_artist->text().trimmed().toStdString();
    spec.tags.comment = m_comment->text().trimmed().toStdString();
    return spec;
}

// ── Rendering ──────────────────────────────────────────────────────────────

void ExportDialog::startRender() {
    if (m_rendering) return;
    const rd::Spec spec = collectSpec();
    if (!spec.writeMixdown && spec.stemChannelIds.empty()) {
        QMessageBox::warning(this, tr("Nothing to render"),
                             tr("Choose the master mix, some stems, or both."));
        return;
    }

    ui::exportprefs::save(spec);
    ui::exportprefs::setLastFolder(m_folder->text());

    m_rendering = true;
    m_cancelRequested = false;
    m_renderButton->setEnabled(false);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->show();
    m_status->setText(tr("Rendering…"));
    m_status->show();

    // Modality blocks edits in the parent. Disabling the parent also disables
    // its child dialog, including Cancel.

    daw::rendering::Report report;
    const auto result = m_controller.renderProject(
        spec,
        [this](const rd::Progress& progress) {
            m_progress->setValue(int(progress.fraction * 1000));
            if (progress.stage == daw::rendering::Progress::Stage::Preparing)
                m_status->setText(tr("Preparing audio and plugins…"));
            else if (progress.stage == daw::rendering::Progress::Stage::PreRoll)
                m_status->setText(tr("Warming up effects: %1 of %2 seconds")
                    .arg(progress.renderedSeconds, 0, 'f', 1)
                    .arg(progress.totalSeconds, 0, 'f', 1));
            else {
            m_status->setText(tr("Rendering %1 of %2")
                                  .arg(formatTime(progress.renderedSeconds))
                                  .arg(formatTime(progress.totalSeconds)));
            }
            QCoreApplication::processEvents();
            return !m_cancelRequested;
        },
        report);

    m_rendering = false;
    m_progress->hide();
    m_renderButton->setEnabled(true);

    if (!result) {
        m_status->setText(tr("Render failed."));
        QMessageBox::warning(this, tr("Render failed"),
                             QString::fromStdString(result.message()));
        return;
    }
    if (report.cancelled) {
        m_status->setText(tr("Cancelled — nothing was written."));
        return;
    }
    accept();
}

// ── Theme ──────────────────────────────────────────────────────────────────

void ExportDialog::applyTheme() {
    const Theme& t = th();
    // Lists, group boxes and the progress bar are not covered by the global
    // stylesheet, so without this the dialog falls back to raw Fusion and reads
    // as a different application.
    setStyleSheet(QString(R"(
QGroupBox { border: 1px solid %SEP%; border-radius: 8px; margin-top: 9px;
            padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px;
                   color: %TEXT2%; font-size: 11px; font-weight: 700; }
QListWidget { background: %WELL%; border: 1px solid %SEP%; border-radius: 7px;
              alternate-background-color: %ALT%; outline: none; }
QListWidget::item { padding: 3px 6px; border: none; }
QListWidget::item:selected { background: %ACCENT%; color: white; }
QProgressBar { background: %WELL%; border: none; border-radius: 3px; }
QProgressBar::chunk { background: %ACCENT%; border-radius: 3px; }
#ExportSummary, #ExportStatus { color: %TEXT2%; font-size: 11px; }
#ExportWarning { color: %WARN%; font-size: 11px; }
)")
                       .replace("%WELL%", t.well().name())
                       .replace("%ALT%", mixColors(t.well(), t.surface, 0.45).name())
                       .replace("%SEP%", t.separator().name())
                       .replace("%ACCENT%", t.accent.name())
                       .replace("%WARN%", Theme::record().name())
                       .replace("%TEXT2%", t.textSecondary.name()));
}

void ExportDialog::stageStemsForShot() {
    m_writeStems->setChecked(true);
    for (int i = 0; i < m_channels->count(); ++i) {
        m_channels->item(i)->setCheckState(Qt::Checked);
    }
    for (int i = 0; i < m_container->count(); ++i) {
        if (m_container->itemData(i).toInt() != int(ap::Container::Mp3)) continue;
        m_container->setCurrentIndex(i);
        break;
    }
    m_tail->setCurrentIndex(int(rd::Tail::UntilSilence));
    m_preFaderStems->setChecked(true);
    syncEnabledState();
    updateSummary();
}

bool ExportDialog::checkForTest() {
    bool ok = m_container->count() > 0 && m_encoding->count() > 0 &&
              m_sampleRate->count() > 0;
    // Every channel the engine builds a strip for, plus the master.
    int expected = 1;
    for (const daw::TrackModel& track : m_controller.project().tracks) {
        if (daw::carriesAudio(track)) ++expected;
    }
    ok = ok && m_channels->count() == expected;
    // Every combination the menus can produce, for every container, has to be
    // one this build can actually write — the project's own sample rate
    // included. Offering it unconditionally is what once let a 44.1 kHz project
    // pick Opus, which encodes at only a handful of rates, and then refused the
    // render with no way for the user to see why.
    const int wasContainer = m_container->currentIndex();
    for (int c = 0; c < m_container->count(); ++c) {
        m_container->setCurrentIndex(c);
        repopulateEncodings();
        repopulateSampleRates();
        if (m_encoding->count() == 0 || m_sampleRate->count() == 0) {
            ok = false;
            break;
        }
        for (int r = 0; r < m_sampleRate->count(); ++r) {
            const double rate = m_sampleRate->itemData(r).toDouble() > 0.0
                                    ? m_sampleRate->itemData(r).toDouble()
                                    : m_controller.sampleRate();
            bool anyEncoding = false;
            for (int e = 0; e < m_encoding->count(); ++e) {
                ap::WriteSpec probe;
                probe.container = ap::Container(m_container->currentData().toInt());
                probe.encoding = ap::Encoding(m_encoding->itemData(e).toInt());
                anyEncoding =
                    anyEncoding || ap::isWriteSpecSupported(probe, 2, rate);
            }
            ok = ok && anyEncoding;
        }
    }
    // The exact case that bit, staged rather than left to whatever rate this
    // project happens to run at: Opus encodes at 8, 12, 16, 24 and 48 kHz and
    // nothing else, so at 44.1 kHz it must offer no rate at all — not even the
    // project's own, which is the entry that used to slip past the filter.
    const double projectRate = m_controller.sampleRate();
    m_controller.setSampleRateHz(44100.0);
    for (int c = 0; c < m_container->count() && ok; ++c) {
        if (m_container->itemData(c).toInt() != int(ap::Container::Opus)) continue;
        m_container->setCurrentIndex(c);
        repopulateEncodings();
        repopulateSampleRates();
        for (int r = 0; r < m_sampleRate->count(); ++r) {
            const double rate = m_sampleRate->itemData(r).toDouble() > 0.0
                                    ? m_sampleRate->itemData(r).toDouble()
                                    : m_controller.sampleRate();
            if (std::fabs(rate - 44100.0) < 0.5) ok = false;
        }
    }
    m_controller.setSampleRateHz(projectRate);

    m_container->setCurrentIndex(wasContainer);
    repopulateEncodings();
    repopulateSampleRates();
    if (!ok) return false;

    // And then actually render, through the same `collectSpec` the Render
    // button uses. The controller's own tests cover the render; what only this
    // can prove is that the controls the user sees add up to a spec that works.
    QTemporaryDir scratch;
    if (!scratch.isValid()) return false;
    m_folder->setText(scratch.path());
    m_baseName->setText(QStringLiteral("selftest"));
    m_writeStems->setChecked(true);
    for (int i = 0; i < m_channels->count(); ++i) {
        m_channels->item(i)->setCheckState(i == 0 ? Qt::Checked : Qt::Unchecked);
    }
    m_rangeCustom->setChecked(true);
    m_rangeStart->setValue(0.0);
    m_rangeEnd->setValue(0.25);

    daw::rendering::Report report;
    if (!m_controller.renderProject(collectSpec(), {}, report)) return false;
    if (report.files.size() != 2) return false;   // one mixdown, one stem
    for (const std::string& file : report.files) {
        if (!QFileInfo::exists(QString::fromStdString(file))) return false;
    }
    return true;
}
