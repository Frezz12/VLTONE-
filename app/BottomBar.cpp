#include "BottomBar.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QCoreApplication>
#include <QSignalBlocker>
#include <QPainter>

/// AI and Web are peer panel toggles, so they use the same compact icon-only
/// treatment as the rest of the bottom bar. Tooltips and accessible names keep
/// the icons unambiguous without spending permanent horizontal space on text.
class AiChatButton final : public ui::IconButton {
    Q_DECLARE_TR_FUNCTIONS(AiChatButton)
public:
    explicit AiChatButton(QWidget* parent)
        : ui::IconButton(icons::Glyph::Assistant, tr("Open the AI assistant"),
                         parent) {
        setCheckable(true);
        setButtonSize(28, 28);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(tr("AI assistant"));
    }
};

class WebBrowserButton final : public ui::IconButton {
    Q_DECLARE_TR_FUNCTIONS(WebBrowserButton)
public:
    explicit WebBrowserButton(QWidget* parent)
        : ui::IconButton(icons::Glyph::Globe,
                         tr("Open the integrated web browser (Alt+W)"), parent) {
        setCheckable(true);
        setButtonSize(28, 28);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(tr("Web browser"));
    }
};

BottomBar::BottomBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(ui::kBottomBarHeight);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(3);

    m_mixerButton = new ui::IconButton(icons::Glyph::Mixer,
                                       tr("Show or hide the mixer (X)"), this);
    m_mixerButton->setCheckable(true);
    m_mixerButton->setChecked(true);
    connect(m_mixerButton, &QAbstractButton::toggled, this,
            &BottomBar::mixerToggled);

    m_inspectorButton = new ui::IconButton(icons::Glyph::Sidebar,
                                           tr("Show or hide the inspector"), this);
    m_inspectorButton->setCheckable(true);
    m_inspectorButton->setChecked(true);
    connect(m_inspectorButton, &QAbstractButton::toggled, this,
            &BottomBar::inspectorToggled);

    m_detachButton = new ui::IconButton(icons::Glyph::Detach,
                                        tr("Open the mixer in its own window"),
                                        this);
    connect(m_detachButton, &QAbstractButton::clicked, this,
            &BottomBar::detachMixerRequested);

    auto* addTrack = new ui::IconButton(icons::Glyph::Plus,
                                        tr("Add audio track"), this);
    connect(addTrack, &QAbstractButton::clicked, this,
            &BottomBar::addTrackRequested);

    auto* settings = new ui::IconButton(icons::Glyph::Gear,
                                        tr("Audio settings"), this);
    connect(settings, &QAbstractButton::clicked, this,
            &BottomBar::settingsRequested);

    // The browser's toggle lives here now, with the other panels. It used to be
    // in the strip over the arrangement, where the one thing it could not do
    // was bring the browser back — the strip's own button went with the column.
    m_browserButton = new ui::IconButton(icons::Glyph::Folder,
                                         tr("Show or hide the browser"), this);
    m_browserButton->setCheckable(true);
    m_browserButton->setChecked(true);
    connect(m_browserButton, &QAbstractButton::toggled, this,
            &BottomBar::browserToggled);

    m_aiButton = new AiChatButton(this);
    connect(m_aiButton, &QAbstractButton::toggled, this, &BottomBar::aiToggled);

    m_webButton = new WebBrowserButton(this);
    connect(m_webButton, &QAbstractButton::toggled, this,
            &BottomBar::webToggled);

    m_hint = new QLabel(this);

    row->addWidget(m_browserButton);
    row->addWidget(m_inspectorButton);
    row->addWidget(ui::separatorLine(Qt::Vertical, 14, this));
    row->addWidget(m_mixerButton);
    row->addWidget(m_detachButton);
    row->addWidget(ui::separatorLine(Qt::Vertical, 14, this));
    row->addWidget(addTrack);
    row->addWidget(settings);
    row->addStretch(1);
    row->addWidget(m_hint);
    row->addSpacing(6);
    row->addWidget(m_webButton);
    row->addSpacing(2);
    row->addWidget(m_aiButton);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &BottomBar::applyTheme);
    applyTheme();
}

void BottomBar::applyTheme() {
    m_hint->setStyleSheet(
        QString("color: %1; font-size: 10px;").arg(th().textSecondary.name()));
    update();
}

void BottomBar::setMixerVisible(bool visible) {
    if (m_mixerButton->isChecked() != visible)
        m_mixerButton->setChecked(visible);
}

void BottomBar::setInspectorVisible(bool visible) {
    if (m_inspectorButton->isChecked() != visible)
        m_inspectorButton->setChecked(visible);
}

void BottomBar::setBrowserVisible(bool visible) {
    if (m_browserButton && m_browserButton->isChecked() != visible) {
        QSignalBlocker block(m_browserButton);
        m_browserButton->setChecked(visible);
    }
}

void BottomBar::setAiVisible(bool visible) {
    // Blocked: this is the shell reporting what happened. Letting the button
    // echo it back would run the shell's handler again — which was enough,
    // once, to persist a visibility a headless run had asked it not to.
    if (m_aiButton && m_aiButton->isChecked() != visible) {
        QSignalBlocker block(m_aiButton);
        m_aiButton->setChecked(visible);
    }
}

void BottomBar::setWebVisible(bool visible) {
    if (m_webButton && m_webButton->isChecked() != visible) {
        QSignalBlocker block(m_webButton);
        m_webButton->setChecked(visible);
    }
}

void BottomBar::setMixerDetached(bool detached) {
    m_detachButton->setEnabled(!detached);
    m_hint->setText(detached ? tr("Mixer window") : QString());
}

void BottomBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const Theme& t = th();
    p.fillRect(rect(), t.surface);
    p.setPen(QPen(t.separator(), 1));
    p.drawLine(0, 0, width(), 0);
}
