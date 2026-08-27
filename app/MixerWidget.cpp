#include "MixerWidget.hpp"
#include "ChannelStrip.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

MixerWidget::MixerWidget(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("MixerPanel");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Header ──
    m_header = new QWidget(this);
    m_header->setObjectName("MixerHeader");
    m_header->setFixedHeight(28);
    auto* head = new QHBoxLayout(m_header);
    head->setContentsMargins(12, 0, 12, 0);
    head->setSpacing(8);

    auto* glyph = new QLabel(m_header);
    glyph->setPixmap(icons::icon(icons::Glyph::Mixer, th().accent, 14).pixmap(14, 14));
    auto* title = new QLabel(tr("MIXER"), m_header);
    title->setObjectName("MixerTitle");
    m_headerCount = new QLabel(m_header);
    m_statusDot = new QLabel(m_header);
    m_statusDot->setFixedSize(6, 6);
    m_statusText = new QLabel(m_header);

    head->addWidget(glyph);
    head->addWidget(title);
    head->addWidget(m_headerCount);
    head->addStretch(1);
    head->addWidget(m_statusDot);
    head->addWidget(m_statusText);
    outer->addWidget(m_header);

    // ── Strips: scrolling channels on the left, master pinned right ──
    auto* body = new QWidget(this);
    auto* bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(0);

    m_stripsHost = new QWidget(body);
    m_stripsLayout = new QHBoxLayout(m_stripsHost);
    m_stripsLayout->setContentsMargins(8, 8, 8, 8);
    m_stripsLayout->setSpacing(5);
    m_stripsLayout->addStretch(1);

    m_scroll = new QScrollArea(body);
    m_scroll->setWidget(m_stripsHost);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_masterHost = new QWidget(body);
    auto* masterRow = new QHBoxLayout(m_masterHost);
    masterRow->setContentsMargins(6, 8, 8, 8);
    masterRow->setSpacing(0);

    // The master rides in its own scroll view with hidden bars, slaved to the
    // channel area, so both stay vertically in step when the pane is short.
    auto* masterScroll = new QScrollArea(body);
    masterScroll->setWidget(m_masterHost);
    masterScroll->setWidgetResizable(true);
    masterScroll->setFrameShape(QFrame::NoFrame);
    masterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    masterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    masterScroll->setFixedWidth(140);
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged,
            masterScroll->verticalScrollBar(), &QScrollBar::setValue);

    bodyRow->addWidget(m_scroll, 1);
    bodyRow->addWidget(ui::separatorLine(Qt::Vertical, 0, body));
    bodyRow->addWidget(masterScroll, 0);
    outer->addWidget(body, 1);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &MixerWidget::applyTheme);
    applyTheme();
    rebuild();
}

void MixerWidget::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#MixerPanel { background: %BG%; border: 1px solid %SECTION%; border-radius: 10px; }
#MixerHeader { background: %HEADER%; border-top-left-radius: 10px;
               border-top-right-radius: 10px; border-bottom: 1px solid %SECTION%; }
#MixerTitle { color: %TEXT%; font-size: 10px; font-weight: 700;
              letter-spacing: 0.6px; }
#MixerHeader QLabel { color: %TEXT2%; font-size: 10px; }
)")
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%HEADER%", mixColors(t.surface, t.surfaceElevated, 0.28).name())
        .replace("%SEP%", t.separator().name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name()));
    update();
}

double MixerWidget::faderGainForTest(const QString& trackId) const {
    for (ChannelStrip* strip : m_strips) {
        if (strip->trackId() == trackId) return strip->faderGainForTest();
    }
    return -1.0;
}

void MixerWidget::syncFromModel() {
    for (ChannelStrip* strip : m_strips) strip->syncFromModel();
}

void MixerWidget::refreshAutomationValues() {
    if (!isVisible()) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshAutomationValues();
    }
}

bool MixerWidget::stripIsVisible(const ChannelStrip* strip) const {
    if (!strip || !strip->isVisible()) return false;
    if (strip->isMaster()) return true;
    const QWidget* viewport = m_scroll ? m_scroll->viewport() : nullptr;
    if (!viewport) return false;
    const QRect stripRect(strip->mapTo(const_cast<QWidget*>(viewport), QPoint{}),
                          strip->size());
    return viewport->rect().intersects(stripRect);
}

void MixerWidget::rebuild() {
    // The strips are owned by the two layouts below; clearing those deletes
    // them, so only the bookkeeping vector is emptied here.
    m_strips.clear();

    while (QLayoutItem* item = m_stripsLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    if (auto* masterLayout = m_masterHost->layout()) {
        while (QLayoutItem* item = masterLayout->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
    }

    auto wire = [this](ChannelStrip* strip) {
        connect(strip, &ChannelStrip::selectRequested, this,
                &MixerWidget::trackSelected);
        connect(strip, &ChannelStrip::edited, this, &MixerWidget::edited);
        connect(strip, &ChannelStrip::editorRequested, this,
                &MixerWidget::pluginEditorRequested);
        connect(strip, &ChannelStrip::patternRequested, this,
                &MixerWidget::openPatternRequested);
        connect(strip, &ChannelStrip::removeRequested, this,
                &MixerWidget::trackRemoved);
        connect(strip, &ChannelStrip::automateControlRequested, this,
                &MixerWidget::automateControlRequested);
        connect(strip, &ChannelStrip::automateMuteRequested, this,
                &MixerWidget::automateMuteRequested);
        connect(strip, &ChannelStrip::automateSendRequested, this,
                &MixerWidget::automateSendRequested);
        connect(strip, &ChannelStrip::structureChanged, this, [this] {
            emit structureChanged();
            rebuild();
        }, Qt::QueuedConnection);
        m_strips.push_back(strip);
    };

    int channels = 0;
    for (const auto& t : m_controller->project().tracks) {
        // A summing folder is a bus, and belongs in the mixer like one. A
        // plain folder carries no signal and has no strip to show.
        if (!daw::carriesAudio(t)) continue;
        auto* strip = new ChannelStrip(m_controller,
                                       QString::fromStdString(t.id),
                                       /*master=*/false, m_stripsHost);
        strip->setSelected(QString::fromStdString(t.id) == m_selectedTrackId);
        wire(strip);
        // No alignment flag: an aligned widget keeps its natural height, and
        // the strips are meant to fill the pane so the faders grow with it.
        m_stripsLayout->addWidget(strip);
        ++channels;
    }
    m_stripsLayout->addStretch(1);

    auto* master = new ChannelStrip(m_controller, QString(), /*master=*/true,
                                    m_masterHost);
    wire(master);
    m_masterHost->layout()->addWidget(master);

    m_headerCount->setText(tr("%1 channels").arg(channels + 1));
}

void MixerWidget::refreshMeters() {
    if (!isVisible()) return;
    for (ChannelStrip* strip : m_strips) {
        if (stripIsVisible(strip)) strip->refreshMeter();
    }

    const bool online = m_controller->isDeviceOpen();
    if (m_statusInitialized && m_statusOnline == online) return;
    m_statusInitialized = true;
    m_statusOnline = online;
    m_statusDot->setStyleSheet(
        QString("background: %1; border-radius: 3px;")
            .arg(online ? QColor(0x4C, 0xC4, 0x8A).name()
                        : QColor(0xE0, 0x9B, 0x3B).name()));
    m_statusText->setText(online ? tr("AUDIO ONLINE") : tr("AUDIO OFFLINE"));
}

void MixerWidget::setSelectedTrack(const QString& trackId) {
    m_selectedTrackId = trackId;
    for (ChannelStrip* strip : m_strips) {
        if (!strip->isMaster()) strip->setSelected(strip->trackId() == trackId);
    }
}
