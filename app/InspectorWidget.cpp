#include "InspectorWidget.hpp"
#include "ChannelStrip.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

InspectorWidget::InspectorWidget(daw::EngineController* controller,
                                 QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("InspectorPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi();
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &InspectorWidget::applyTheme);
    applyTheme();
    setFixedWidth(kExpandedWidth);
}

void InspectorWidget::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Header with the collapse toggle ──
    m_header = new QWidget(this);
    m_header->setObjectName("InspectorHeader");
    // This is one continuous ruler line with the track header and timeline.
    // Keep the geometry identical; a 26 px inspector header beside their
    // 30 px rulers leaves a visible four-pixel step.
    m_header->setFixedHeight(ui::kRulerHeight);
    auto* head = new QHBoxLayout(m_header);
    head->setContentsMargins(8, 0, 4, 0);
    head->setSpacing(6);
    auto* title = new QLabel(tr("INSPECTOR"), m_header);
    title->setObjectName("InspectorTitle");
    m_collapseButton = new ui::IconButton(icons::Glyph::Sidebar,
                                          tr("Collapse inspector"), m_header);
    m_collapseButton->setButtonSize(22, 20);
    connect(m_collapseButton, &QAbstractButton::clicked, this,
            &InspectorWidget::toggleCollapsed);
    head->addWidget(title, 1);
    head->addWidget(m_collapseButton);
    outer->addWidget(m_header);

    // ── Expanded content ──
    m_content = new QWidget(this);
    auto* col = new QVBoxLayout(m_content);
    col->setContentsMargins(8, 8, 8, 8);
    col->setSpacing(6);

    m_nameEdit = new QLineEdit(m_content);
    m_nameEdit->setPlaceholderText(tr("Track name"));
    // Same reasoning as the tempo field: nothing should own the keyboard until
    // it is clicked, or the transport shortcuts stop working.
    m_nameEdit->setFocusPolicy(Qt::ClickFocus);
    connect(m_nameEdit, &QLineEdit::editingFinished, this, [this] {
        if (m_trackId.isEmpty()) return;
        const auto result = m_controller->renameTrack(
            m_trackId.toStdString(), m_nameEdit->text().toStdString());
        m_nameEdit->clearFocus();
        loadProperties();
        emit edited(daw::collab::marksLocalFileDirty(result));
    });

    auto* colorRow = new QHBoxLayout;
    colorRow->setContentsMargins(0, 0, 0, 0);
    colorRow->setSpacing(6);
    m_colorSwatch = new QWidget(m_content);
    m_colorSwatch->setFixedHeight(18);
    m_colorSwatch->setCursor(Qt::PointingHandCursor);
    m_colorSwatch->setToolTip(tr("Track colour"));
    m_colorSwatch->installEventFilter(this);
    auto* colorButton = new ui::IconButton(icons::Glyph::Gear,
                                           tr("Choose colour"), m_content);
    colorButton->setButtonSize(22, 20);
    connect(colorButton, &QAbstractButton::clicked, this,
            &InspectorWidget::pickColor);
    colorRow->addWidget(m_colorSwatch, 1);
    colorRow->addWidget(colorButton);

    m_kindLabel = new QLabel(m_content);
    m_clipsLabel = new QLabel(m_content);

    col->addWidget(ui::sectionLabel(tr("Track"), m_content));
    col->addWidget(m_nameEdit);
    col->addLayout(colorRow);
    m_clipSection = buildClipSection();
    col->addWidget(m_clipSection);
    col->addWidget(m_kindLabel);
    col->addWidget(m_clipsLabel);

    // Track/clip properties stay compact at the top; the console surface is
    // anchored to the bottom whenever the inspector has spare vertical room.
    col->addStretch(1);
    col->addWidget(ui::separatorLine(Qt::Horizontal, 0, m_content));
    col->addWidget(ui::sectionLabel(tr("Channel"), m_content));

    m_stripSlot = new QVBoxLayout;
    m_stripSlot->setContentsMargins(0, 0, 0, 0);
    m_stripSlot->setSpacing(0);
    col->addLayout(m_stripSlot);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(m_content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll, 1);

    // ── Collapsed rail ──
    m_rail = new QWidget(this);
    auto* railCol = new QVBoxLayout(m_rail);
    railCol->setContentsMargins(3, 6, 3, 6);
    railCol->setSpacing(0);
    auto* expand = new ui::IconButton(icons::Glyph::ChevronRight,
                                      tr("Show inspector"), m_rail);
    expand->setButtonSize(24, 24);
    connect(expand, &QAbstractButton::clicked, this,
            &InspectorWidget::toggleCollapsed);
    railCol->addWidget(expand, 0, Qt::AlignHCenter);
    railCol->addStretch(1);
    m_rail->hide();
    outer->addWidget(m_rail, 1);
}

QDoubleSpinBox* InspectorWidget::addClipSpin(
    QFormLayout* form, const QString& label, const QString& parameterId,
    double minimum, double maximum, double step, int decimals,
    const QString& suffix, double scale) {
    auto* spin = new QDoubleSpinBox(m_content);
    spin->setObjectName(QStringLiteral("ClipParameter_%1").arg(parameterId));
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    spin->setFocusPolicy(Qt::StrongFocus);
    spin->setProperty("parameterScale", scale);
    spin->setAccessibleName(label);
    spin->setToolTip(label);
    form->addRow(label, spin);
    m_clipSpins.insert(parameterId, spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            [this, spin, parameterId](double shown) {
                if (m_loadingClipControls || m_clipId.isEmpty()) return;
                if (!m_clipGestureStarts.contains(parameterId)) {
                    m_clipGestureStarts.insert(
                        parameterId,
                        m_controller->clipSampleParameter(
                            m_trackId.toStdString(), m_clipId.toStdString(),
                            parameterId.toStdString()));
                }
                applyClipParameter(
                    parameterId,
                    shown * spin->property("parameterScale").toDouble());
            });
    connect(spin, &QDoubleSpinBox::editingFinished, this,
            [this, parameterId] {
                if (!m_clipGestureStarts.contains(parameterId)) return;
                commitClipParameter(parameterId,
                                    m_clipGestureStarts.take(parameterId));
            });
    return spin;
}

QWidget* InspectorWidget::buildClipSection() {
    auto* section = new QWidget(m_content);
    section->setObjectName(QStringLiteral("InspectorClipSection"));
    auto* col = new QVBoxLayout(section);
    col->setContentsMargins(6, 6, 6, 6);
    col->setSpacing(5);

    col->addWidget(ui::sectionLabel(tr("Audio clip"), section));
    m_clipNameLabel = new QLabel(section);
    m_clipNameLabel->setObjectName(QStringLiteral("InspectorClipName"));
    m_clipNameLabel->setWordWrap(true);
    col->addWidget(m_clipNameLabel);

    auto* stretchTool = new QPushButton(tr("Stretch tool"), section);
    stretchTool->setObjectName(QStringLiteral("InspectorStretchTool"));
    stretchTool->setToolTip(
        tr("Select the Stretch tool, then drag an audio clip horizontally."));
    stretchTool->setAccessibleName(tr("Select Stretch tool"));
    stretchTool->setFocusPolicy(Qt::StrongFocus);
    connect(stretchTool, &QPushButton::clicked, this,
            &InspectorWidget::stretchToolRequested);
    col->addWidget(stretchTool);

    auto* playback = new QFormLayout;
    playback->setContentsMargins(0, 0, 0, 0);
    playback->setHorizontalSpacing(5);
    playback->setVerticalSpacing(4);
    playback->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    playback->setRowWrapPolicy(QFormLayout::WrapLongRows);

    auto* stretchMode = new QComboBox(section);
    stretchMode->setObjectName(QStringLiteral("InspectorStretchMode"));
    stretchMode->addItems({tr("Resample"), tr("Drums"), tr("Loop"),
                           tr("Vocal"), tr("Complex")});
    stretchMode->setAccessibleName(tr("Stretch mode"));
    stretchMode->setToolTip(tr("Playback strategy used while stretching the clip."));
    playback->addRow(tr("Mode"), stretchMode);
    m_clipCombos.insert(QStringLiteral("stretch.mode"), stretchMode);

    addClipSpin(playback, tr("Time"), QStringLiteral("stretch.time"),
                25.0, 400.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(playback, tr("Pitch"), QStringLiteral("stretch.pitch"),
                -24.0, 24.0, 0.1, 1, tr(" st"));
    addClipSpin(playback, tr("Formant"), QStringLiteral("formant"),
                -12.0, 12.0, 0.1, 1, tr(" st"));

    auto* loopMode = new QComboBox(section);
    loopMode->setObjectName(QStringLiteral("InspectorLoopMode"));
    loopMode->addItems({tr("Off"), tr("Forward"), tr("Ping-Pong")});
    loopMode->setAccessibleName(tr("Loop mode"));
    playback->addRow(tr("Loop"), loopMode);
    m_clipCombos.insert(QStringLiteral("loop.mode"), loopMode);

    auto* reverse = new QCheckBox(tr("Reverse"), section);
    reverse->setObjectName(QStringLiteral("ClipParameter_pre.reverse"));
    reverse->setAccessibleName(tr("Reverse clip audio"));
    playback->addRow(reverse);
    m_clipToggles.insert(QStringLiteral("pre.reverse"), reverse);
    col->addLayout(playback);

    auto* more = new QToolButton(section);
    more->setObjectName(QStringLiteral("InspectorMoreClipSettings"));
    more->setText(tr("More clip settings"));
    more->setCheckable(true);
    more->setChecked(false);
    more->setArrowType(Qt::RightArrow);
    more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more->setFocusPolicy(Qt::StrongFocus);
    more->setAccessibleName(tr("Show more clip settings"));
    col->addWidget(more);

    m_clipAdvanced = new QWidget(section);
    auto* advanced = new QVBoxLayout(m_clipAdvanced);
    advanced->setContentsMargins(0, 0, 0, 0);
    advanced->setSpacing(6);

    auto* source = new QFormLayout;
    source->setContentsMargins(0, 0, 0, 0);
    source->setHorizontalSpacing(5);
    source->setVerticalSpacing(4);
    source->setRowWrapPolicy(QFormLayout::WrapLongRows);
    source->addRow(ui::sectionLabel(tr("Source"), m_clipAdvanced));
    addClipSpin(source, tr("Start"), QStringLiteral("startoffset"),
                0.0, 100.0, 0.5, 1, QStringLiteral("%"), 0.01);
    addClipSpin(source, tr("End"), QStringLiteral("endoffset"),
                0.0, 100.0, 0.5, 1, QStringLiteral("%"), 0.01);
    addClipSpin(source, tr("Fade in"), QStringLiteral("fadein"),
                0.0, 100.0, 0.5, 1, QStringLiteral("%"), 0.01);
    addClipSpin(source, tr("Fade out"), QStringLiteral("fadeout"),
                0.0, 100.0, 0.5, 1, QStringLiteral("%"), 0.01);
    addClipSpin(source, tr("Loop start"), QStringLiteral("loop.start"),
                0.0, 100.0, 0.5, 1, QStringLiteral("%"), 0.01);
    addClipSpin(source, tr("Loop end"), QStringLiteral("loop.end"),
                0.0, 100.0, 0.5, 1, QStringLiteral("%"), 0.01);
    advanced->addLayout(source);

    auto* tone = new QFormLayout;
    tone->setContentsMargins(0, 0, 0, 0);
    tone->setHorizontalSpacing(5);
    tone->setVerticalSpacing(4);
    tone->setRowWrapPolicy(QFormLayout::WrapLongRows);
    tone->addRow(ui::sectionLabel(tr("Tone"), m_clipAdvanced));
    addClipSpin(tone, tr("Boost"), QStringLiteral("pre.boost"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("EQ low"), QStringLiteral("pre.eq.low"),
                -100.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("EQ mid"), QStringLiteral("pre.eq.mid"),
                -100.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("EQ high"), QStringLiteral("pre.eq.high"),
                -100.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("Ring mix"), QStringLiteral("pre.rm.mix"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("Ring freq"), QStringLiteral("pre.rm.freq"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("Cut"), QStringLiteral("pre.cut"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("Resonance"), QStringLiteral("pre.res"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);

    auto* reverbType = new QComboBox(m_clipAdvanced);
    reverbType->addItems({tr("Room"), tr("Hall")});
    reverbType->setAccessibleName(tr("Reverb type"));
    tone->addRow(tr("Reverb"), reverbType);
    m_clipCombos.insert(QStringLiteral("pre.rev.type"), reverbType);
    addClipSpin(tone, tr("Reverb amount"), QStringLiteral("pre.rev"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("Stereo delay"), QStringLiteral("pre.delay"),
                0.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    addClipSpin(tone, tr("Pogo"), QStringLiteral("pre.pogo"),
                -100.0, 100.0, 1.0, 0, QStringLiteral("%"), 0.01);
    advanced->addLayout(tone);

    auto addToggle = [this, advanced](const QString& text,
                                      const QString& parameterId) {
        auto* toggle = new QCheckBox(text, m_clipAdvanced);
        toggle->setAccessibleName(text);
        m_clipToggles.insert(parameterId, toggle);
        advanced->addWidget(toggle);
    };
    addToggle(tr("Remove DC"), QStringLiteral("pre.dc"));
    addToggle(tr("Invert polarity"), QStringLiteral("pre.polarity"));
    addToggle(tr("Normalize"), QStringLiteral("pre.normalize"));
    addToggle(tr("Fade stereo"), QStringLiteral("pre.fadestereo"));
    addToggle(tr("Swap stereo"), QStringLiteral("pre.swap"));

    auto connectCombo = [this](QComboBox* combo, const QString& parameterId) {
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, parameterId](int index) {
                    if (m_loadingClipControls || m_clipId.isEmpty()) return;
                    const double before = m_controller->clipSampleParameter(
                        m_trackId.toStdString(), m_clipId.toStdString(),
                        parameterId.toStdString());
                    applyClipParameter(parameterId, index);
                    commitClipParameter(parameterId, before);
                });
    };
    connectCombo(stretchMode, QStringLiteral("stretch.mode"));
    connectCombo(loopMode, QStringLiteral("loop.mode"));
    connectCombo(reverbType, QStringLiteral("pre.rev.type"));

    for (auto it = m_clipToggles.cbegin(); it != m_clipToggles.cend(); ++it) {
        const QString parameterId = it.key();
        connect(it.value(), &QAbstractButton::toggled, this,
                [this, parameterId](bool checked) {
                    if (m_loadingClipControls || m_clipId.isEmpty()) return;
                    const double before = m_controller->clipSampleParameter(
                        m_trackId.toStdString(), m_clipId.toStdString(),
                        parameterId.toStdString());
                    applyClipParameter(parameterId, checked ? 1.0 : 0.0);
                    commitClipParameter(parameterId, before);
                });
    }

    m_clipAdvanced->hide();
    connect(more, &QToolButton::toggled, this, [this, more](bool shown) {
        m_clipAdvanced->setVisible(shown);
        more->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
        more->setAccessibleName(shown ? tr("Hide more clip settings")
                                      : tr("Show more clip settings"));
    });
    col->addWidget(m_clipAdvanced);
    section->hide();
    return section;
}

bool InspectorWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_colorSwatch && event->type() == QEvent::MouseButtonRelease) {
        pickColor();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void InspectorWidget::setCollapsed(bool collapsed) {
    if (m_collapsed == collapsed) return;
    m_collapsed = collapsed;

    m_header->setVisible(!collapsed);
    if (auto* scroll = findChild<QScrollArea*>()) scroll->setVisible(!collapsed);
    m_rail->setVisible(collapsed);
    setFixedWidth(collapsed ? kRailWidth : kExpandedWidth);
    emit collapsedChanged(collapsed);
}

void InspectorWidget::setTrack(const QString& trackId) {
    if (m_trackId == trackId && m_clipId.isEmpty() && m_strip) return;
    const auto pending = m_clipGestureStarts;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        commitClipParameter(it.key(), it.value());
    m_clipGestureStarts.clear();
    m_trackId = trackId;
    m_clipId.clear();
    rebuild();
}

void InspectorWidget::setSelection(const QString& trackId,
                                   const QString& clipId) {
    const bool trackChanged = m_trackId != trackId;
    const bool clipChanged = m_clipId != clipId;
    if (!trackChanged && !clipChanged) return;
    const auto pending = m_clipGestureStarts;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        commitClipParameter(it.key(), it.value());
    m_clipGestureStarts.clear();
    m_trackId = trackId;
    m_clipId = clipId;
    if (trackChanged || !m_strip) rebuild();
    else loadProperties();
}

void InspectorWidget::rebuildForTrack(const QString& trackId) {
    if (m_trackId != trackId) m_clipId.clear();
    m_trackId = trackId;
    rebuild();
}

void InspectorWidget::rebuild() {
    if (m_strip) {
        m_stripSlot->removeWidget(m_strip);
        m_strip->deleteLater();
        m_strip = nullptr;
    }
    const daw::TrackModel* selected =
        m_trackId.isEmpty()
            ? nullptr
            : m_controller->project().findTrack(m_trackId.toStdString());
    // A folder that does not sum has no channel — no fader, no inserts, no
    // routing. Showing an empty console for it would offer controls that
    // govern nothing.
    const bool valid = selected != nullptr && daw::carriesAudio(*selected);
    if (valid) {
        m_strip = new ChannelStrip(m_controller, m_trackId, /*master=*/false,
                                   m_content);
        // The inspector shows the whole console for the selected track and
        // scrolls if it does not fit, rather than folding sections away.
        m_strip->setStretchable(false);
        m_strip->setInspectorCompact(true);
        connect(m_strip, &ChannelStrip::edited, this, &InspectorWidget::edited);
        connect(m_strip, &ChannelStrip::editorRequested, this,
                &InspectorWidget::pluginEditorRequested);
        connect(m_strip, &ChannelStrip::automateControlRequested, this,
                &InspectorWidget::automateControlRequested);
        connect(m_strip, &ChannelStrip::automateMuteRequested, this,
                &InspectorWidget::automateMuteRequested);
        connect(m_strip, &ChannelStrip::automateSendRequested, this,
                &InspectorWidget::automateSendRequested);
        connect(m_strip, &ChannelStrip::structureChanged, this, [this] {
            emit structureChanged();
            rebuild();
        }, Qt::QueuedConnection);
        m_stripSlot->addWidget(m_strip, 0, Qt::AlignHCenter);
    }
    loadProperties();
}

void InspectorWidget::syncFromModel() {
    if (m_strip) m_strip->syncFromModel();
    loadProperties();
}

void InspectorWidget::refreshAutomationValues() {
    if (m_strip && m_strip->isVisible()) m_strip->refreshAutomationValues();
}

void InspectorWidget::loadProperties() {
    const auto* track =
        m_trackId.isEmpty()
            ? nullptr
            : m_controller->project().findTrack(m_trackId.toStdString());
    const bool valid = track != nullptr;

    m_nameEdit->setEnabled(valid);
    m_colorSwatch->setEnabled(valid);
    if (!valid) {
        m_clipId.clear();
        m_clipSection->hide();
        m_nameEdit->clear();
        m_kindLabel->setText(tr("No track selected"));
        m_clipsLabel->clear();
        m_colorSwatch->setStyleSheet(
            QString("background: %1; border-radius: 4px;").arg(th().well().name()));
        return;
    }

    if (!m_nameEdit->hasFocus())
        m_nameEdit->setText(QString::fromStdString(track->name));
    m_kindLabel->setText(
        tr("Type  %1").arg(QString::fromStdString(daw::toString(track->kind))));
    m_clipsLabel->setText(tr("Clips  %1").arg(track->clips.size()));
    m_colorSwatch->setStyleSheet(QString("background: %1; border-radius: 4px;")
                                     .arg(colorFromRgb(track->color).name()));

    const daw::ClipModel* clip = nullptr;
    if (!m_clipId.isEmpty()) {
        for (const daw::ClipModel& candidate : track->clips) {
            if (QString::fromStdString(candidate.id) == m_clipId &&
                candidate.kind == daw::ClipKind::Audio) {
                clip = &candidate;
                break;
            }
        }
    }
    m_clipSection->setVisible(clip != nullptr);
    if (!clip) return;

    m_clipNameLabel->setText(QString::fromStdString(clip->name));
    m_clipNameLabel->setToolTip(QString::fromStdString(clip->name));
    m_loadingClipControls = true;
    for (auto it = m_clipSpins.cbegin(); it != m_clipSpins.cend(); ++it) {
        const QSignalBlocker block(it.value());
        const double scale = it.value()->property("parameterScale").toDouble();
        const double value = m_controller->clipSampleParameter(
            m_trackId.toStdString(), m_clipId.toStdString(),
            it.key().toStdString());
        it.value()->setValue(value / (scale == 0.0 ? 1.0 : scale));
    }
    for (auto it = m_clipCombos.cbegin(); it != m_clipCombos.cend(); ++it) {
        const QSignalBlocker block(it.value());
        const int value = int(std::lround(m_controller->clipSampleParameter(
            m_trackId.toStdString(), m_clipId.toStdString(),
            it.key().toStdString())));
        it.value()->setCurrentIndex(value);
    }
    for (auto it = m_clipToggles.cbegin(); it != m_clipToggles.cend(); ++it) {
        const QSignalBlocker block(it.value());
        const bool value = m_controller->clipSampleParameter(
                               m_trackId.toStdString(), m_clipId.toStdString(),
                               it.key().toStdString()) >= 0.5;
        it.value()->setChecked(value);
    }
    m_loadingClipControls = false;
}

void InspectorWidget::applyClipParameter(const QString& parameterId,
                                         double value) {
    if (m_trackId.isEmpty() || m_clipId.isEmpty()) return;
    m_controller->setClipSampleParameter(
        m_trackId.toStdString(), m_clipId.toStdString(),
        parameterId.toStdString(), value);
    // Keep the waveform and every peer control live during a drag/spin without
    // declaring the project dirty until the gesture is committed.
    emit edited(false);
}

void InspectorWidget::commitClipParameter(const QString& parameterId,
                                          double before) {
    if (m_trackId.isEmpty() || m_clipId.isEmpty()) return;
    const double after = m_controller->clipSampleParameter(
        m_trackId.toStdString(), m_clipId.toStdString(),
        parameterId.toStdString());
    if (std::abs(after - before) < 1e-9) return;
    m_controller->commitClipSampleParameterEdit(
        m_trackId.toStdString(), m_clipId.toStdString(),
        parameterId.toStdString(), before, "Change Clip Sample Parameter");
    emit edited(true);
}

void InspectorWidget::pickColor() {
    const auto* track =
        m_trackId.isEmpty()
            ? nullptr
            : m_controller->project().findTrack(m_trackId.toStdString());
    if (!track) return;
    const QColor chosen = QColorDialog::getColor(colorFromRgb(track->color), this,
                                                 tr("Track Colour"));
    if (!chosen.isValid()) return;
    const uint32_t rgb = (uint32_t(chosen.red()) << 16) |
                         (uint32_t(chosen.green()) << 8) | uint32_t(chosen.blue());
    m_controller->setTrackColor(m_trackId.toStdString(), rgb);
    emit edited();
    rebuild();
}

void InspectorWidget::refreshMeters() {
    if (m_strip && m_strip->isVisible()) m_strip->refreshMeter();
}

void InspectorWidget::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#InspectorPanel { background: %SURFACE%; border-right: 2px solid %SECTION%; }
#InspectorHeader { background: %HEADER%; border-bottom: 1px solid %SECTION%; }
#InspectorTitle { color: %TEXT2%; font-size: 10px; font-weight: 700;
                  letter-spacing: 0.6px; }
#InspectorPanel QLabel { color: %TEXT2%; font-size: 10px; }
#InspectorClipSection { background: %WELL%; border: 1px solid %SEP%;
                        border-radius: 6px; }
#InspectorClipName { color: %TEXT%; font-weight: 600; }
#InspectorStretchTool { min-height: 22px; padding: 2px 5px; }
#InspectorMoreClipSettings { color: %TEXT2%; background: transparent;
                             border: none; text-align: left; padding: 2px 0; }
#InspectorMoreClipSettings:hover, #InspectorMoreClipSettings:focus {
    color: %ACCENT%;
}
#InspectorPanel QComboBox, #InspectorPanel QDoubleSpinBox {
    min-height: 20px; padding: 1px 3px;
}
)")
        .replace("%SURFACE%", t.surface.name())
        .replace("%TOOLBAR%", t.toolbarBackground.name())
        .replace("%HEADER%", mixColors(t.toolbarBackground,
                                        t.surfaceElevated, 0.22).name())
        .replace("%SEP%", t.separator().name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%WELL%", t.well().name()));
    loadProperties();
}
