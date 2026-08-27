#include "InspectorWidget.hpp"
#include "ChannelStrip.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QColorDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QVBoxLayout>

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
        m_controller->renameTrack(m_trackId.toStdString(),
                                  m_nameEdit->text().toStdString());
        m_nameEdit->clearFocus();
        emit edited();
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
    col->addWidget(m_kindLabel);
    col->addWidget(m_clipsLabel);
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
    if (m_trackId == trackId && m_strip) return;
    m_trackId = trackId;
    rebuild();
}

void InspectorWidget::rebuildForTrack(const QString& trackId) {
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
#InspectorPanel { background: %SURFACE%; border-right: 1px solid %SECTION%; }
#InspectorHeader { background: %HEADER%; border-bottom: 1px solid %SECTION%; }
#InspectorTitle { color: %TEXT2%; font-size: 10px; font-weight: 700;
                  letter-spacing: 0.6px; }
#InspectorPanel QLabel { color: %TEXT2%; font-size: 10px; }
)")
        .replace("%SURFACE%", t.surface.name())
        .replace("%TOOLBAR%", t.toolbarBackground.name())
        .replace("%HEADER%", mixColors(t.toolbarBackground,
                                        t.surfaceElevated, 0.22).name())
        .replace("%SEP%", t.separator().name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%TEXT2%", t.textSecondary.name()));
    loadProperties();
}
