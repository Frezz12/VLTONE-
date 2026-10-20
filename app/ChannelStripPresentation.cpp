#include "ChannelStrip.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "MiniModuleRack.hpp"
#include "Theme.hpp"
#include "TrackIcons.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QDrag>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>

namespace {
class VerticalChannelName final : public QLabel {
public:
    explicit VerticalChannelName(QWidget* parent) : QLabel(parent) {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        setMinimumHeight(66);
    }
    QSize sizeHint() const override { return {18, 76}; }
    QSize minimumSizeHint() const override { return {0, 66}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(th().textPrimary);
        painter.translate(0, height());
        painter.rotate(-90);
        painter.drawText(QRect(3, 0, height() - 6, width()), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(text(), Qt::ElideRight, height() - 6));
    }
};
}

QWidget* ChannelStrip::buildCompactHeader() {
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("ChannelHeader"));
    header->setFixedHeight(112);
    auto* column = new QVBoxLayout(header);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);
    m_channelNumber = new QLabel(header);
    m_channelNumber->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_channelNumber->setAlignment(Qt::AlignCenter);
    m_channelNumber->setFixedHeight(14);
    m_headerSwatch = new QWidget(header);
    m_headerSwatch->setFixedHeight(3);
    m_headerName = new VerticalChannelName(header);
    m_headerName->setObjectName(QStringLiteral("StripName"));
    column->addWidget(m_channelNumber);
    column->addWidget(m_headerSwatch);
    column->addWidget(m_headerName, 1);
    return header;
}

void ChannelStrip::setChannelNumber(int number) {
    if (m_channelNumber) m_channelNumber->setText(QString::number(number));
}

void ChannelStrip::buildMonoButton(QWidget* parent) {
    m_monoButton = new ui::IconButton(icons::Glyph::StereoRings, tr("Stereo"), parent);
    m_monoButton->setCheckable(true);
    m_monoButton->setButtonSize(24, 24);
    connect(m_monoButton, &QAbstractButton::clicked, this, [this] {
        const bool mono = m_monoButton->isChecked();
        m_controller->setTrackMono(m_trackId.toStdString(), mono);
        m_monoButton->setGlyph(mono ? icons::Glyph::MonoRing : icons::Glyph::StereoRings);
        m_monoButton->setToolTip(mono ? tr("Mono") : tr("Stereo"));
        emit edited();
    });
}

void ChannelStrip::buildPresentation() {
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    m_mainLayout = new QVBoxLayout(this);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &ChannelStrip::applyTheme);
    connect(&ThemeManager::instance(), &ThemeManager::trackIconsChanged, this, &ChannelStrip::updatePresentationHeader);
    if (m_presentation == Presentation::Details) {
        setObjectName(QStringLiteral("MixerChannelDetails"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_mainLayout->setContentsMargins(6, 6, 6, 6);
        m_mainLayout->setSpacing(6);
        m_stripWidth = ui::MixerPreferences::instance().detailsWidth();
        buildDetails();
        return;
    }
    setObjectName(QStringLiteral("CompactChannelStrip"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_mainLayout->setContentsMargins(2, 5, 2, 5);
    m_mainLayout->setSpacing(4);
    m_mainLayout->addWidget(buildHeader());
    auto* faderRow = buildFaderRow();
    // Reparent the existing meter into its own tier above M/S. The fader row
    // keeps exactly the same gesture, automation and history handlers.
    m_mainLayout->addWidget(m_meter, 1, Qt::AlignHCenter);
    m_mainLayout->addWidget(buildButtons());
    m_pan = new ui::PanKnob(this);
    m_pan->setAutomatable(true);
    connect(m_pan, &ui::PanKnob::automateRequested, this,
            [this] { emit automateControlRequested(channelId(), true); });
    bindPan();
    m_mainLayout->addWidget(m_pan, 0, Qt::AlignHCenter);
    m_mainLayout->addWidget(faderRow, 2);
    connect(m_meter, &ui::LevelMeter::peakResetRequested, this, &ChannelStrip::resetPeakHold);
    m_meter->setAccessibleName(tr("Stereo level · Click to reset peak"));
    m_mute->setFocusPolicy(Qt::TabFocus);
    m_solo->setFocusPolicy(Qt::TabFocus);
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
    setStripWidth(ui::MixerPreferences::instance().compactWidth());
    applyTheme();
    syncFromModel();
}

void ChannelStrip::buildDetails() {
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    if (!track || !daw::carriesAudio(*track)) {
        setAcceptDrops(false);
        auto* empty = new QLabel(tr("Select an audio channel to see its routing, plugins and sends."), this);
        empty->setObjectName(QStringLiteral("MixerDetailsEmpty"));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignCenter);
        empty->setContentsMargins(16, 24, 16, 24);
        m_mainLayout->addWidget(empty);
        m_mainLayout->addStretch();
        applyTheme();
        return;
    }
    setAcceptDrops(true);
    m_mainLayout->addWidget(buildHeader());
    m_mainLayout->addWidget(buildButtons());
    buildRack();
    m_mainLayout->addWidget(m_rack);
    m_mainLayout->addStretch();
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
    applyTheme();
    syncFromModel();
    measureRack();
}

void ChannelStrip::setDetailsChannel(const QString& trackId) {
    if (m_presentation != Presentation::Details) return;
    finishEdits();
    // Slot and send callbacks capture their ids, so rebuild the contents while
    // retaining the one details surface. No old control can target a new id.
    while (auto* item = m_mainLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_headerName = m_headerIcon = m_channelNumber = nullptr;
    m_headerSwatch = nullptr;
    m_headerSwatchStyleKey.clear();
    m_rack = m_colorWell = nullptr;
    m_rackSections.fill(nullptr);
    m_inputButton = m_outputButton = nullptr;
    m_monitor = nullptr;
    m_monoButton = m_colorPower = m_fxPower = m_sendsPower = nullptr;
    m_buttonsLayout = nullptr;
    m_sendKnobs.clear();
    m_finishColorEdits.clear();
    m_rackNaturalHeights.fill(0);
    m_rackNaturalHeight = m_rackExtraHeight = m_naturalHeight = m_layoutWidth = 0;
    setMinimumHeight(0);
    m_trackId = trackId;
    buildDetails();
    updateGeometry();
}

void ChannelStrip::finishEdits() {
    if (m_fader) m_fader->finishEditing();
    if (m_pan) m_pan->finishEditing();
    if (auto* rack = qobject_cast<ui::MiniModuleRack*>(m_colorWell)) rack->finishEdits();
    for (auto* knob : findChildren<ui::Knob*>()) knob->finishEditing();
    finishInsertBypassPaint();
    if (m_browserDropActive || m_dropHighlight || !findChildren<QDrag*>().isEmpty()) QDrag::cancel();
    m_browserDropActive = m_dropHighlight = false;
    if (auto* popup = QApplication::activePopupWidget();
        popup && (popup == this || isAncestorOf(popup))) popup->close();
    if (auto* grabber = QWidget::mouseGrabber();
        grabber && (grabber == this || isAncestorOf(grabber))) grabber->releaseMouse();
}

void ChannelStrip::updatePresentationHeader() {
    if (!m_headerIcon) return;
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    if (!track) return;
    const auto ink = mixColors(colorFromRgb(track->color), th().textPrimary, .30);
    auto icon = ui::trackicons::icon(QString::fromStdString(track->iconId), ink);
    if (icon.isNull()) {
        const auto glyph = track->kind == daw::TrackKind::Audio ? icons::Glyph::Waveform
            : track->kind == daw::TrackKind::Pattern ? icons::Glyph::Grid
            : track->kind == daw::TrackKind::Bus || track->kind == daw::TrackKind::Group ||
              track->kind == daw::TrackKind::Folder ? icons::Glyph::Layers : icons::Glyph::MidiKeys;
        icon = icons::icon(glyph, ink, 20);
    }
    m_headerIcon->setPixmap(icon.pixmap(QSize(20, 20), devicePixelRatioF()));
}
