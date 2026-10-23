#include "ClipTrackPicker.hpp"

#include "Theme.hpp"
#include "TrackIcons.hpp"
#include "model/Document.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>
#include <algorithm>

ClipTrackPicker::ClipTrackPicker(const daw::ProjectModel& project,
                               const QStringList& tracks, QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("ClipTrackPicker"));
    setWindowTitle(tr("Move to track"));
    setAccessibleName(windowTitle());
    setAttribute(Qt::WA_DeleteOnClose);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(3, 3, 3, 3);
    column->setSpacing(0);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("ClipTrackDestinations"));
    m_list->setAccessibleName(tr("Compatible destination tracks"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(QSize(16, 16));
    m_list->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->installEventFilter(this);
    for (const auto& id : tracks) {
        const auto* track = project.findTrack(id.toStdString());
        if (!track) continue;
        auto* item = new QListWidgetItem(QString::fromStdString(track->name), m_list);
        const QColor ink = mixColors(colorFromRgb(track->color), th().textPrimary, 0.30);
        QIcon icon = ui::trackicons::icon(QString::fromStdString(track->iconId), ink);
        if (icon.isNull()) {
            const auto glyph = track->kind == daw::TrackKind::Audio ? icons::Glyph::Waveform
                : track->kind == daw::TrackKind::Pattern ? icons::Glyph::Pattern
                : track->kind == daw::TrackKind::Automation ? icons::Glyph::Automation
                : icons::Glyph::MidiKeys;
            icon = icons::icon(glyph, ink, 16);
        }
        item->setIcon(icon);
        item->setData(Qt::UserRole, id);
        item->setToolTip(item->text());
        item->setSizeHint(QSize(0, 24));
    }
    column->addWidget(m_list, 1);
    connect(m_list, &QListWidget::itemClicked, this, &ClipTrackPicker::choose);
    connect(m_list, &QListWidget::itemActivated, this, &ClipTrackPicker::choose);
    const auto applyTheme = [this] {
        const auto& t = th();
        setStyleSheet(QStringLiteral(
            "#ClipTrackPicker { background: %1; border: 1px solid %2; border-radius: 5px; }"
            "#ClipTrackDestinations { background: transparent; color: %4; border: none; outline: none; font-size: 11px; }"
            "#ClipTrackDestinations::item { padding: 0 3px; border: 1px solid transparent; border-radius: 3px; }"
            "#ClipTrackDestinations::item:hover { background: %3; }"
            "#ClipTrackDestinations::item:selected { background: %3; color: %4; border-color: %5; }"
            "#ClipTrackDestinations QScrollBar:vertical { background: transparent; width: 5px; margin: 0; }"
            "#ClipTrackDestinations QScrollBar::handle:vertical { background: %2; min-height: 18px; border-radius: 2px; }"
            "#ClipTrackDestinations QScrollBar::handle:vertical:hover { background: %5; }"
            "#ClipTrackDestinations QScrollBar::add-line:vertical, #ClipTrackDestinations QScrollBar::sub-line:vertical { height: 0; }"
            "#ClipTrackDestinations QScrollBar::add-page:vertical, #ClipTrackDestinations QScrollBar::sub-page:vertical { background: transparent; }")
            .arg(t.surface.name(), t.separator().name(), mixColors(t.surface, t.accent, .16).name(),
                 t.textPrimary.name(), t.accent.name()));
    };
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, applyTheme);
    applyTheme();
    if (m_list->count()) m_list->setCurrentRow(0);
    setFixedWidth(80);
    resize(width(), 6 + std::min(6, m_list->count()) * 24);
}

void ClipTrackPicker::choose(QListWidgetItem* item) {
    if (!item || item->isHidden() || !isVisible()) return;
    const QString track = item->data(Qt::UserRole).toString();
    accept();
    emit trackSelected(track);
}

void ClipTrackPicker::popup(const QPoint& globalPosition) {
    if (!m_list->count()) { close(); return; }
    auto* screen = QGuiApplication::screenAt(globalPosition);
    if (!screen) screen = QGuiApplication::primaryScreen();
    QPoint origin = globalPosition + QPoint(12, 12);
    if (screen) {
        const QRect bounds = screen->availableGeometry().adjusted(8, 8, -8, -8);
        resize(size().boundedTo(bounds.size()));
        if (origin.x() + width() > bounds.right() + 1) origin.setX(globalPosition.x() - width() - 12);
        if (origin.y() + height() > bounds.bottom() + 1) origin.setY(globalPosition.y() - height() - 12);
        origin.setX(std::clamp(origin.x(), bounds.left(), bounds.right() - width() + 1));
        origin.setY(std::clamp(origin.y(), bounds.top(), bounds.bottom() - height() + 1));
    }
    move(origin);
    show();
    m_list->setFocus();
}

bool ClipTrackPicker::eventFilter(QObject* watched, QEvent* event) {
    // Claim typing and navigation before arrangement shortcuts see the key.
    if (watched == m_list && event->type() == QEvent::ShortcutOverride &&
        !(static_cast<QKeyEvent*>(event)->modifiers() &
          (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        event->accept();
        return true;
    }
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            choose(m_list->currentItem());
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
