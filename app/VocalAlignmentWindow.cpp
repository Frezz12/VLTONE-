#include "VocalAlignmentWindow.hpp"
#include "VocalAlignmentPreview.hpp"
#include "ClipLibraryDrag.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "Job/BackgroundExecutor.hpp"
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QStyle>
#include <QToolButton>
#include <QSplitter>
#include <QShortcut>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>

namespace {
QString reasonText(const std::string& text) {
    const auto translated = QCoreApplication::translate("VocalAlignmentWindow", text.c_str());
    if (translated != QString::fromStdString(text)) return translated;
    const auto separator = text.rfind(": ");
    if (separator != std::string::npos)
        return QString::fromStdString(text.substr(0, separator + 2)) + QCoreApplication::translate("VocalAlignmentWindow", text.substr(separator + 2).c_str());
    return translated;
}
// Analysis and controller modules are Qt-independent. Keep their diagnostic
// messages in the application translation catalogue.
[[maybe_unused]] const char* const diagnostics[] = {
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Zero density: unchanged."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "No reliable matching phrase nearby."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Already within tolerance."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Locked markers prevent a whole-clip shift."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Uncertain or excessive changes were left untouched."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "The alignment could not produce a valid Warp map."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Invalid alignment settings."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Alignment cancelled."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "The clip has no supported readable audio."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Editable vocal alignment is available in local projects only."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "This session is read-only."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Stop recording before aligning vocals."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Choose a guide and at least one target audio clip."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "A clip cannot align to itself or appear twice."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Select audio clips only."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Source audio is unavailable."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Render pitch-changing Warp before aligning vocals."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "The clip has no readable audio."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Invalid source Warp map."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Invalid vocal alignment result."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Invalid vocal alignment Warp map."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Enable or reset the existing Warp map before aligning vocals."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Unfreeze the track before using Warp."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Select an audio clip."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Bounce the comp to an audio clip before using Warp."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Disable the sample loop before using Warp."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Warp supports mono and stereo audio."),
    QT_TRANSLATE_NOOP("VocalAlignmentWindow", "Change tape fades to volume fades before using Warp."),
};
void dropSurface(QWidget* widget, const char* role, QObject* filter) {
    widget->setAcceptDrops(true);
    widget->setProperty("vocalAlignmentDropTarget", true);
    widget->setProperty("alignmentRole", role);
    widget->installEventFilter(filter);
}
QToolButton* iconButton(QWidget* parent, const char* name, icons::Glyph glyph, const QString& label) {
    auto* button = new QToolButton(parent);
    button->setObjectName(name); button->setToolTip(label); button->setAccessibleName(label);
    button->setFocusPolicy(Qt::StrongFocus); button->setFixedSize(32,32); button->setIconSize(QSize(18,18));
    const auto tint = [button, glyph] { button->setIcon(icons::icon(glyph, ThemeManager::instance().theme().textPrimary)); };
    tint(); QObject::connect(&ThemeManager::instance(), &ThemeManager::changed, button, tint);
    return button;
}
}
struct VocalAlignmentWindow::Job {
    std::atomic<bool> cancelled{false}, done{false};
    std::atomic<int> progress{0};
    std::vector<daw::alignment::AlignmentProposal> results;
    std::string error;
    std::uint64_t revision = 0;
};

VocalAlignmentWindow::VocalAlignmentWindow(daw::EngineController* controller, ui::SelectionModel* selection, QWidget* parent)
    : QDialog(parent, Qt::Tool), m_controller(controller), m_selection(selection), m_generation(controller->projectGeneration()) {
    setObjectName("VocalAlignmentWindow");
    setWindowTitle(tr("Vocal Alignment"));
    setModal(false); setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(800, 580); resize(1120, 720);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(16,16,16,16); layout->setSpacing(12);
    auto* split = new QSplitter(Qt::Horizontal, this); split->setChildrenCollapsible(false);
    m_setup = new QWidget(split); auto* setup = new QVBoxLayout(m_setup); setup->setContentsMargins(0,0,12,0); setup->setSpacing(10);
    m_setup->setMinimumWidth(240); m_setup->setMaximumWidth(380);
    auto* heading = new QHBoxLayout;
    auto* guideTitle = new QLabel(tr("Guide clip"), this); guideTitle->setProperty("alignmentHeading", true);
    heading->addWidget(guideTitle); heading->addStretch();
    auto* selectionButton = iconButton(this, "AlignmentTakeSelection", icons::Glyph::Crosshair, tr("Take selection"));
    connect(selectionButton, &QToolButton::clicked, this, &VocalAlignmentWindow::takeSelection);
    heading->addWidget(selectionButton); setup->addLayout(heading);
    m_guideLabel = new QLabel(this); m_guideLabel->setObjectName("AlignmentGuide");
    m_guideLabel->setMargin(14); m_guideLabel->setMinimumHeight(64); m_guideLabel->setTextFormat(Qt::PlainText);
    m_guideLabel->setWordWrap(true); m_guideLabel->setAccessibleName(tr("Guide clip"));
    m_guideLabel->setToolTip(tr("Drop a clip here to replace the guide. The guide stays unchanged."));
    dropSurface(m_guideLabel, "guide", this); setup->addWidget(m_guideLabel);
    auto* targetHeading = new QHBoxLayout;
    m_targetsTitle = new QLabel(this); m_targetsTitle->setProperty("alignmentHeading", true); targetHeading->addWidget(m_targetsTitle, 1);
    auto* add = iconButton(this, "AlignmentAddTargets", icons::Glyph::Plus, tr("Add selected clips / drop here"));
    dropSurface(add, "add", this); targetHeading->addWidget(add); setup->addLayout(targetHeading);
    connect(add, &QToolButton::clicked, this, [this] { addTargets(m_selection->clipsInSelectionOrder()); });
    m_list = new QListWidget(this); m_list->setObjectName("AlignmentTargets"); m_list->setMinimumHeight(100);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection); m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setIconSize(QSize(18,18)); m_list->setSpacing(3); m_targetsTitle->setBuddy(m_list);
    m_list->setAccessibleName(tr("Align these clips"));
    dropSurface(m_list->viewport(), "targets", this); setup->addWidget(m_list, 1);
    auto* dropHint = new QLabel(tr("Drag clips here to add targets"), this); dropHint->setObjectName("AlignmentDropHint");
    dropHint->setAlignment(Qt::AlignCenter); dropHint->setWordWrap(true); dropHint->setMargin(10); dropSurface(dropHint, "add", this); setup->addWidget(dropHint);
    auto* actions = new QHBoxLayout; actions->setSpacing(6);
    auto* promote = iconButton(this, "AlignmentPromote", icons::Glyph::ArrowUp, tr("Set as guide")); actions->addWidget(promote);
    connect(promote, &QToolButton::clicked, this, [this] {
        const int row = m_list->currentRow(); if (row < 0) return;
        const auto next = m_targets[row], old = m_guide;
        setGuide(next); if (isAudio(old)) addTargets({old});
    });
    auto* remove = iconButton(this, "AlignmentRemove", icons::Glyph::Minus, tr("Remove from alignment (Delete)")); actions->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this] {
        const int row = m_list->currentRow(); if (row < 0) return;
        m_targets.removeAt(row); resetSources(); rebuildList(); m_list->setCurrentRow(std::min(row, int(m_targets.size()) - 1));
    });
    auto* removeKey = new QShortcut(QKeySequence(Qt::Key_Delete), m_list);
    removeKey->setContext(Qt::WidgetShortcut); connect(removeKey, &QShortcut::activated, remove, &QToolButton::click);
    actions->addStretch();
    auto* warp = iconButton(this, "AlignmentWarp", icons::Glyph::Edit, tr("Open Warp editor")); actions->addWidget(warp);
    connect(warp, &QToolButton::clicked, this, [this] {
        const int row = m_list->currentRow(); if (row >= 0) emit warpEditorRequested(m_targets[row].trackId, m_targets[row].clipId);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, warp, &QToolButton::click);
    const auto selectionChanged = [=, this] {
        const bool selected = m_list->currentRow() >= 0;
        promote->setEnabled(selected); remove->setEnabled(selected); warp->setEnabled(selected);
        refreshPreview();
    };
    connect(m_list, &QListWidget::currentRowChanged, this, selectionChanged);
    setup->addLayout(actions);

    auto* comparison = new QWidget(split); auto* content = new QVBoxLayout(comparison); content->setContentsMargins(12,0,0,0); content->setSpacing(10);
    auto* previewHeading = new QHBoxLayout; previewHeading->setSpacing(4);
    m_previewTitle = new QLabel(tr("Waveform comparison"), this); m_previewTitle->setProperty("alignmentHeading", true);
    m_previewTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); m_previewTitle->setTextFormat(Qt::PlainText);
    previewHeading->addWidget(m_previewTitle, 1);
    auto* changes = iconButton(this, "AlignmentShowChanges", icons::Glyph::WarpQuantize, tr("Show timing changes"));
    changes->setCheckable(true); changes->setChecked(true); previewHeading->addWidget(changes);
    auto* zoomOut = iconButton(this, "AlignmentZoomOut", icons::Glyph::ZoomOut, tr("Zoom out (−)")); previewHeading->addWidget(zoomOut);
    auto* zoomIn = iconButton(this, "AlignmentZoomIn", icons::Glyph::ZoomIn, tr("Zoom in (+)")); previewHeading->addWidget(zoomIn);
    auto* fit = iconButton(this, "AlignmentZoomFit", icons::Glyph::ZoomFit, tr("Fit all clips (Home)")); previewHeading->addWidget(fit);
    content->addLayout(previewHeading);
    m_preview = new VocalAlignmentPreview(this); content->addWidget(m_preview, 1);
    connect(changes, &QToolButton::toggled, m_preview, &VocalAlignmentPreview::setShowChanges);
    connect(zoomIn, &QToolButton::clicked, this, [this] { m_preview->zoom(1.5); });
    connect(zoomOut, &QToolButton::clicked, this, [this] { m_preview->zoom(1 / 1.5); });
    connect(fit, &QToolButton::clicked, m_preview, &VocalAlignmentPreview::fit);
    m_detail = new QLabel(this); m_detail->setObjectName("AlignmentResultDetail"); m_detail->setWordWrap(true);
    m_detail->setTextFormat(Qt::PlainText); m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setMinimumHeight(fontMetrics().height() * 2); content->addWidget(m_detail);

    m_options = new QWidget(this); auto* options = new QVBoxLayout(m_options); options->setContentsMargins(0,0,0,0); options->setSpacing(10);
    auto* methodRow = new QHBoxLayout;
    auto* methodLabel = new QLabel(tr("Method"), this); methodRow->addWidget(methodLabel);
    m_method = new QComboBox(this); m_method->setObjectName("AlignmentMethod");
    m_method->addItems({tr("Hybrid vocal"), tr("Attacks"), tr("Shift only")}); methodLabel->setBuddy(m_method); methodRow->addWidget(m_method, 1);
    auto* advanced = iconButton(this, "AlignmentAdvanced", icons::Glyph::Gear, tr("Advanced settings")); advanced->setCheckable(true); methodRow->addWidget(advanced); options->addLayout(methodRow);
    auto* densityRow = new QHBoxLayout; auto* densityLabel = new QLabel(tr("Density"), this); densityRow->addWidget(densityLabel);
    m_strength = new QSlider(Qt::Horizontal, this); m_strength->setObjectName("AlignmentDensity"); m_strength->setRange(0,100); m_strength->setValue(100); m_strength->setAccessibleName(tr("Density")); densityLabel->setBuddy(m_strength);
    auto* percent = new QSpinBox(this); percent->setRange(0,100); percent->setValue(100); percent->setSuffix("%"); percent->setAccessibleName(tr("Density")); percent->setObjectName("AlignmentDensityValue");
    densityRow->addWidget(m_strength, 1); densityRow->addWidget(percent);
    connect(m_strength, &QSlider::valueChanged, percent, &QSpinBox::setValue); connect(percent, &QSpinBox::valueChanged, m_strength, &QSlider::setValue);
    options->addLayout(densityRow);
    auto* advancedBody = new QWidget(this); advancedBody->setObjectName("AlignmentAdvancedBody"); auto* form = new QFormLayout(advancedBody); form->setContentsMargins(0,4,0,0);
    m_tolerance = new QSpinBox(this); m_tolerance->setRange(0,200); m_tolerance->setValue(5); m_tolerance->setSuffix(tr(" ms")); m_tolerance->setObjectName("AlignmentTolerance");
    m_tolerance->setToolTip(tr("Differences within this tolerance stay untouched."));
    m_shift = new QSpinBox(this); m_shift->setRange(1,2000); m_shift->setValue(500); m_shift->setSuffix(tr(" ms")); m_shift->setObjectName("AlignmentMaxShift");
    m_stretch = new QDoubleSpinBox(this); m_stretch->setDecimals(2); m_stretch->setRange(1,1.25); m_stretch->setSingleStep(.01); m_stretch->setValue(1.25);
    m_stretch->setToolTip(tr("Voiced segments stay within this stretch factor and its reciprocal."));
    form->addRow(tr("Tolerance"), m_tolerance); form->addRow(tr("Maximum shift ±"), m_shift); form->addRow(tr("Maximum stretch factor"), m_stretch);
    options->addWidget(advancedBody); advancedBody->hide();
    auto* optionsScroll = new QScrollArea(this); optionsScroll->setFrameShape(QFrame::NoFrame);
    optionsScroll->setWidgetResizable(true); optionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    optionsScroll->setWidget(m_options); optionsScroll->setMinimumHeight(74); optionsScroll->setMaximumHeight(76);
    optionsScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    connect(advanced, &QToolButton::toggled, this, [=, this] {
        advancedBody->setVisible(advanced->isChecked());
        options->activate(); optionsScroll->setMaximumHeight(advanced->isChecked() ? std::max(76, m_options->sizeHint().height() + 4) : 76);
        content->setStretchFactor(optionsScroll, advanced->isChecked() ? 1 : 0);
        if (advanced->isChecked()) QTimer::singleShot(0, optionsScroll, [=, this] { optionsScroll->ensureWidgetVisible(m_tolerance, 0, 4); });
    });
    content->addWidget(optionsScroll); split->setStretchFactor(0,0); split->setStretchFactor(1,1); split->setSizes({280,800}); layout->addWidget(split, 1);

    m_message = new QLabel(this); m_message->setWordWrap(true); m_message->setObjectName("AlignmentMessage"); m_message->setTextFormat(Qt::PlainText);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse); m_message->setMinimumHeight(fontMetrics().height() * 2); layout->addWidget(m_message);
    auto* footer = new QHBoxLayout; footer->setSpacing(10);
    m_progress = new QProgressBar(this); m_progress->setRange(0,100); m_progress->setAccessibleName(tr("Alignment progress")); m_progress->setMaximumWidth(260); m_progress->hide(); footer->addWidget(m_progress);
    m_cancel = iconButton(this, "AlignmentCancel", icons::Glyph::Stop, tr("Cancel analysis")); m_cancel->hide(); footer->addWidget(m_cancel); connect(m_cancel, &QToolButton::clicked, this, &VocalAlignmentWindow::cancel);
    footer->addStretch();
    m_run = new QPushButton(tr("Align all"), this); m_run->setObjectName("AlignmentRun"); m_run->setMinimumSize(144,38); m_run->setDefault(true); footer->addWidget(m_run); connect(m_run, &QPushButton::clicked, this, &VocalAlignmentWindow::run);
    layout->addLayout(footer);
    const auto settingsChanged = [this] {
        if (!m_stale && !m_results.empty()) m_message->setText(tr("Settings changed. Press Align to update the comparison."));
    };
    connect(m_method, &QComboBox::currentIndexChanged, this, settingsChanged); connect(m_strength, &QSlider::valueChanged, this, settingsChanged);
    connect(m_tolerance, &QSpinBox::valueChanged, this, settingsChanged); connect(m_shift, &QSpinBox::valueChanged, this, settingsChanged); connect(m_stretch, &QDoubleSpinBox::valueChanged, this, settingsChanged);
    const auto applyTheme = [this] {
        const auto& theme = ThemeManager::instance().theme();
        setStyleSheet(QStringLiteral(
            "QLabel[alignmentHeading=true] { font-weight: 600; }"
            "QLabel#AlignmentGuide { background: %2; border: 1px solid %3; border-radius: 8px; }"
            "QLabel#AlignmentDropHint { color: %5; border: 1px dashed %3; border-radius: 6px; }"
            "QLabel#AlignmentGuide[alignmentHover=true], QLabel#AlignmentDropHint[alignmentHover=true], QToolButton[alignmentHover=true] { border: 2px solid %1; }"
            "QListWidget { background: %2; border: 1px solid %3; border-radius: 8px; }"
            "QListWidget::item { padding: 10px 6px; border-radius: 5px; }"
            "QListWidget::item:selected { background: %1; color: %4; }"
            "QLabel#AlignmentResultDetail, QLabel#AlignmentMessage { color: %5; }"
            "QToolButton { padding: 4px; border-radius: 6px; }"
            "QToolButton:checked { background: %2; color: %1; }"
            "QPushButton#AlignmentRun { background: %1; color: %4; border: 1px solid %1; border-radius: 7px; padding: 6px 18px; }"
            "QPushButton#AlignmentRun:hover { background: %6; }"
            "QPushButton#AlignmentRun:pressed { background: %2; color: %5; }"
            "QPushButton#AlignmentRun:disabled { background: %2; color: %5; border-color: %3; }")
            .arg(theme.accent.name(), theme.surface.name(), theme.separator().name(), theme.accentText().name(), theme.textSecondary.name(), theme.accentHighlight.name()));
        m_run->setIcon(icons::icon(icons::Glyph::WarpAlign, theme.accentText()));
        showResults();
    };
    applyTheme(); connect(&ThemeManager::instance(), &ThemeManager::changed, this, applyTheme);
    selectionChanged();
    m_timer = new QTimer(this); m_timer->setInterval(100); connect(m_timer, &QTimer::timeout, this, &VocalAlignmentWindow::poll); m_timer->start();
    takeSelection();
}
VocalAlignmentWindow::~VocalAlignmentWindow() { if (m_job) m_job->cancelled = true; }
void VocalAlignmentWindow::closeEvent(QCloseEvent* event) { cancel(); QDialog::closeEvent(event); }
void VocalAlignmentWindow::reject() { cancel(); QDialog::reject(); }
QString VocalAlignmentWindow::clipName(const ui::ClipSel& ref) const {
    const auto* clip = m_controller->audioClip(ref.trackId.toStdString(), ref.clipId.toStdString());
    const auto* track = m_controller->project().findTrack(ref.trackId.toStdString());
    return clip && track ? QString::fromStdString(track->name + " · " + clip->name) : tr("Clip unavailable");
}
bool VocalAlignmentWindow::isAudio(const ui::ClipSel& ref) const { return m_controller->audioClip(ref.trackId.toStdString(), ref.clipId.toStdString()); }
void VocalAlignmentWindow::resetSources() { cancel(); m_sources.clear(); m_results.clear(); m_stale = false; m_message->clear(); }
void VocalAlignmentWindow::setGuide(const ui::ClipSel& clip) {
    if (!isAudio(clip)) return;
    m_guide = clip; m_targets.removeAll(clip); resetSources(); rebuildList();
}
void VocalAlignmentWindow::addTargets(const QVector<ui::ClipSel>& clips) {
    bool changed = false;
    for (const auto& clip : clips) if (isAudio(clip) && clip != m_guide && !m_targets.contains(clip)) { m_targets.push_back(clip); changed = true; }
    if (changed) { resetSources(); rebuildList(); }
}
void VocalAlignmentWindow::takeSelection() {
    resetSources(); m_guide = {}; m_targets.clear();
    for (const auto& clip : m_selection->clipsInSelectionOrder()) if (isAudio(clip)) {
        if (m_guide.clipId.isEmpty()) m_guide = clip; else if (!m_targets.contains(clip)) m_targets.push_back(clip);
    }
    rebuildList();
}
void VocalAlignmentWindow::rebuildList() {
    m_guideLabel->setText(m_guide.clipId.isEmpty() ? tr("Drop the guide clip here") : clipName(m_guide));
    m_targetsTitle->setText(tr("Targets · %1").arg(m_targets.size()));
    const int selected = std::max(0, m_list->currentRow());
    m_list->clear(); for (const auto& clip : m_targets) m_list->addItem(clipName(clip));
    showResults(); m_list->setCurrentRow(std::min(selected, int(m_targets.size()) - 1));
    refreshPreview();
    poll();
}
void VocalAlignmentWindow::showResults() {
    const auto& theme = ThemeManager::instance().theme();
    for (int i = 0; i < m_list->count(); ++i) {
        auto* row = m_list->item(i); QString text = clipName(m_targets[i]);
        auto glyph = icons::Glyph::Waveform;
        QString detail;
        if (i < int(m_results.size())) {
            const auto& result = m_results[i];
            const QString status = result.status == daw::alignment::AlignmentStatus::Aligned ? tr("Aligned") : result.status == daw::alignment::AlignmentStatus::Partial ? tr("Partially aligned") : tr("Skipped");
            text += "\n" + status; detail = reasonText(result.reason);
            glyph = result.status == daw::alignment::AlignmentStatus::Aligned ? icons::Glyph::Check : result.status == daw::alignment::AlignmentStatus::Partial ? icons::Glyph::Warning : icons::Glyph::Minus;
        } else text += "\n" + tr("Ready");
        row->setText(text); row->setIcon(icons::icon(glyph, theme.textPrimary));
        row->setToolTip(text + (detail.isEmpty() ? QString() : "\n" + detail));
        row->setData(Qt::AccessibleTextRole, row->toolTip());
    }
}
void VocalAlignmentWindow::refreshPreview() {
    if (!m_stale && !m_sources.empty() && m_previewRevision != m_controller->projectRevision() &&
        !m_controller->vocalAlignmentSourcesCurrent(m_sources)) {
        m_stale = true; m_results.clear(); showResults();
        m_message->setText(tr("The audio changed. Take the selection again."));
    }
    const int row = m_list->currentRow();
    const auto target = row >= 0 && row < m_targets.size() ? m_targets[row] : ui::ClipSel{};
    m_previewTitle->setText(target.clipId.isEmpty() ? tr("Waveform comparison") : clipName(target));
    m_previewTitle->setToolTip(m_previewTitle->text());
    if (!m_stale && row >= 0 && row + 1 < int(m_sources.size())) {
        std::optional<daw::alignment::AlignmentProposal> result;
        if (row < int(m_results.size())) result = m_results[row];
        m_preview->setSnapshot(m_sources.front().source, m_sources[row + 1].source, result);
    } else m_preview->setLive(m_controller, m_guide, target);
    if (!m_stale && row >= 0 && row < int(m_results.size())) {
        const auto& result = m_results[row];
        const QString status = result.status == daw::alignment::AlignmentStatus::Aligned ? tr("Aligned") : result.status == daw::alignment::AlignmentStatus::Partial ? tr("Partially aligned") : tr("Skipped");
        m_detail->setText(status + " · " + tr("Max shift %1 ms · Matches: %2 · Skipped: %3")
            .arg(result.maximumShiftSeconds * 1000, 0, 'f', 1).arg(qulonglong(result.matched)).arg(qulonglong(result.skipped)) +
            (result.reason.empty() ? QString() : "\n" + reasonText(result.reason)));
    } else m_detail->setText(m_stale ? tr("The audio changed. Take the selection again.") :
        tr("Compare each target with the guide on the same time scale. Lines between Before and After show timing changes."));
    m_previewRevision = m_controller->projectRevision(); m_waveformRevision = m_controller->clipWaveformRevision();
}
bool VocalAlignmentWindow::eventFilter(QObject* object, QEvent* event) {
    const auto highlight = [&](bool on) {
        if (object->property("alignmentHover").toBool() == on) return;
        object->setProperty("alignmentHover", on);
        if (auto* widget = qobject_cast<QWidget*>(object)) { widget->style()->unpolish(widget); widget->style()->polish(widget); widget->update(); }
    };
    if (event->type() == QEvent::DragLeave) { highlight(false); return true; }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove && event->type() != QEvent::Drop) return QDialog::eventFilter(object, event);
    auto* drop = static_cast<QDropEvent*>(event);
    auto clips = ui::cliplibrary::timelineGroup(drop->mimeData(), m_controller, m_generation);
    clips.erase(std::remove_if(clips.begin(), clips.end(), [this](const auto& clip) { return !isAudio(clip); }), clips.end());
    if (m_job || m_generation != m_controller->projectGeneration() || clips.isEmpty()) { drop->ignore(); return true; }
    drop->setDropAction(Qt::CopyAction); drop->accept();
    highlight(event->type() != QEvent::Drop);
    if (object->property("alignmentRole").toString() == "targets")
        if (auto* item = m_list->itemAt(drop->position().toPoint())) m_list->setCurrentItem(item);
    if (event->type() != QEvent::Drop) return true;
    const auto role = object->property("alignmentRole").toString();
    if (role == "guide") setGuide(clips.front());
    else if (role == "targets" && m_list->itemAt(drop->position().toPoint())) {
        const int row = m_list->row(m_list->itemAt(drop->position().toPoint()));
        if (clips.size() == 1 && clips.front() != m_guide && !m_targets.contains(clips.front())) {
            m_targets[row] = clips.front(); resetSources(); rebuildList();
        } else drop->ignore();
    } else addTargets(clips);
    return true;
}
void VocalAlignmentWindow::run() {
    if (m_job || m_targets.isEmpty() || !isAudio(m_guide)) return;
    std::vector<daw::EngineController::ClipAddress> clips{{m_guide.trackId.toStdString(), m_guide.clipId.toStdString()}};
    for (const auto& clip : m_targets) clips.push_back({clip.trackId.toStdString(), clip.clipId.toStdString()});
    // Reuse original audio for settings changes only while the entire batch
    // still matches the guide and every target currently in the list.
    const bool sameBatch = clips.size() == m_sources.size() &&
        std::equal(clips.begin(), clips.end(), m_sources.begin(), [](const auto& clip, const auto& source) {
            return clip.trackId == source.address.trackId && clip.clipId == source.address.clipId;
        });
    if (!sameBatch) resetSources();
    if (m_sources.empty()) {
        auto result = m_controller->prepareVocalAlignment(clips, m_sources);
        if (!result) { m_message->setText(reasonText(result.message())); return; }
    }
    if (!m_controller->vocalAlignmentSourcesCurrent(m_sources)) { m_message->setText(tr("The audio changed. Take the selection again.")); return; }
    refreshPreview();
    daw::alignment::AlignmentSettings settings;
    settings.method = daw::alignment::AlignmentMethod(m_method->currentIndex()); settings.strength = m_strength->value() / 100.;
    settings.toleranceSeconds = m_tolerance->value() / 1000.; settings.maximumShiftSeconds = m_shift->value() / 1000.; settings.maximumStretch = m_stretch->value();
    std::vector<daw::alignment::AlignmentSource> targets;
    for (std::size_t i = 1; i < m_sources.size(); ++i) targets.push_back(m_sources[i].source);
    auto job = std::make_shared<Job>(); job->revision = m_controller->projectRevision(); m_job = job;
    m_message->setText(tr("Analyzing vocals…")); m_progress->setValue(0); m_progress->show(); m_cancel->show(); m_setup->setEnabled(false); m_options->setEnabled(false); m_run->setEnabled(false);
    daw::engine::BackgroundExecutor::instance().submit([job, guide = m_sources.front().source, targets = std::move(targets), settings] {
        try { job->results = daw::alignment::align(guide, targets, settings, [job](double progress) { job->progress = int(progress * 100); return !job->cancelled.load(); }); }
        catch (const std::exception& e) { job->error = e.what(); }
        job->done.store(true, std::memory_order_release);
    });
}
void VocalAlignmentWindow::cancel() {
    if (m_job) { m_job->cancelled = true; m_job.reset(); m_message->setText(tr("Analysis cancelled. No changes applied.")); }
    m_setup->setEnabled(true); m_options->setEnabled(true); m_cancel->hide(); m_progress->hide();
}
void VocalAlignmentWindow::poll() {
    if (m_generation != m_controller->projectGeneration()) { cancel(); close(); return; }
    if (m_job) {
        const auto unavailable = m_controller->vocalAlignmentUnavailableReason();
        if (!unavailable.empty()) { cancel(); m_message->setText(reasonText(unavailable)); return; }
        // Do not stat every media file on every UI tick in large batches.
        // Document edits trigger early validation; commit always validates all
        // source files again, including edits made outside the application.
        if (m_job->revision != m_controller->projectRevision()) {
            if (!m_controller->vocalAlignmentSourcesCurrent(m_sources)) { cancel(); m_message->setText(tr("The audio changed during analysis. Take the selection again.")); return; }
            m_job->revision = m_controller->projectRevision();
        }
        m_progress->setValue(m_job->progress.load());
        if (!m_job->done.load(std::memory_order_acquire)) return;
        auto job = m_job; m_job.reset(); m_setup->setEnabled(true); m_options->setEnabled(true); m_cancel->hide(); m_progress->hide();
        if (!job->error.empty()) m_message->setText(reasonText(job->error));
        else {
            const auto undoDepth = m_controller->undoDepth();
            const auto applied = m_controller->applyVocalAlignment(m_sources, job->results);
            if (!applied) m_message->setText(reasonText(applied.message()));
            else {
                m_results = std::move(job->results); showResults(); refreshPreview();
                m_message->setText(m_controller->undoDepth() == undoDepth ? tr("No changes applied. See each clip's result above.") :
                    tr("Done. One Undo restores this run. Change settings and press Align to recalculate from the original clips."));
                emit alignmentApplied();
            }
        }
    }
    if (m_previewRevision != m_controller->projectRevision()) {
        refreshPreview(); showResults();
    } else if (m_waveformRevision != m_controller->clipWaveformRevision()) {
        m_waveformRevision = m_controller->clipWaveformRevision(); m_preview->refreshLive();
    }
    const auto unavailable = m_controller->vocalAlignmentUnavailableReason();
    m_run->setEnabled(!m_stale && unavailable.empty() && isAudio(m_guide) && !m_targets.isEmpty());
    m_run->setToolTip(m_stale ? tr("The audio changed. Take the selection again.") :
        unavailable.empty() ? tr("Align all added targets to the guide. Selecting a row only changes the comparison.") : reasonText(unavailable));
    if (!unavailable.empty()) m_message->setText(reasonText(unavailable));
}
