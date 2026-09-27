#include "TrackIconPicker.hpp"
#include "TrackIcons.hpp"
#include "Theme.hpp"

#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

TrackIconPicker::TrackIconPicker(const QString& currentId, const QIcon& defaultIcon,
                               QWidget* parent)
    : QDialog(parent, Qt::Popup), m_currentId(currentId), m_defaultIcon(defaultIcon) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("TrackIconPicker"));
    setWindowTitle(tr("Track icon"));
    setAccessibleName(tr("Choose a track icon"));
    resize(556, 432);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(14, 12, 14, 12);
    column->setSpacing(10);
    auto* heading = new QLabel(tr("Track icon"), this);
    QFont title = heading->font(); title.setBold(true); heading->setFont(title);
    column->addWidget(heading);
    auto* filters = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("TrackIconSearch"));
    m_search->setPlaceholderText(tr("Search instruments and sounds…"));
    m_search->setAccessibleName(tr("Search icons"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    m_category = new QComboBox(this);
    m_category->setObjectName(QStringLiteral("TrackIconCategory"));
    m_category->setAccessibleName(tr("Icon category"));
    m_category->setMaximumWidth(190);
    filters->addWidget(m_search, 1);
    filters->addWidget(m_category);
    column->addLayout(filters);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("TrackIconGrid"));
    m_list->setAccessibleName(tr("Available track icons"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(QSize(32, 32));
    m_list->setGridSize(QSize(100, 80));
    m_list->setSpacing(2);
    m_list->setWordWrap(true);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    column->addWidget(m_list, 1);
    m_empty = new QLabel(tr("No matching icons"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    column->addWidget(m_empty);
    auto* footer = new QHBoxLayout;
    auto* reset = new QPushButton(tr("Use default"), this);
    reset->setAutoDefault(false);
    reset->setObjectName(QStringLiteral("TrackIconDefault"));
    auto* add = new QPushButton(tr("Add icon…"), this);
    add->setAutoDefault(false);
    add->setObjectName(QStringLiteral("TrackIconAdd"));
    add->setToolTip(tr("Import an image in theme settings"));
    footer->addWidget(reset);
    footer->addStretch(1);
    footer->addWidget(add);
    column->addLayout(footer);
    const auto choose = [this](QListWidgetItem* item) {
        if (!item || !isVisible()) return;
        const QString id = item->data(Qt::UserRole).toString();
        accept();
        emit iconSelected(id);
    };
    connect(m_list, &QListWidget::itemClicked, this, choose);
    connect(m_list, &QListWidget::itemActivated, this, choose);
    connect(reset, &QPushButton::clicked, this, [this] { accept(); emit iconSelected({}); });
    connect(add, &QPushButton::clicked, this, [this] { accept(); emit addIconRequested(); });
    connect(m_search, &QLineEdit::textChanged, this, &TrackIconPicker::filter);
    connect(m_category, &QComboBox::currentIndexChanged, this, &TrackIconPicker::filter);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &TrackIconPicker::reload);
    connect(&ThemeManager::instance(), &ThemeManager::trackIconsChanged, this, &TrackIconPicker::reload);
    reload();
}

void TrackIconPicker::reload() {
    const QSignalBlocker block(m_category);
    const QString previous = m_category->currentData().toString();
    m_category->clear();
    m_category->addItem(tr("All categories"), QString());
    m_list->clear();
    auto entries = ui::trackicons::builtins();
    entries += ui::trackicons::customIcons();
    entries.prepend({{}, tr("Default"), {}});
    for (const auto& entry : entries) {
        if (!entry.category.isEmpty() && m_category->findData(entry.category) < 0)
            m_category->addItem(entry.category, entry.category);
        QIcon image = entry.id.isEmpty() ? m_defaultIcon : ui::trackicons::icon(entry.id, th().textPrimary);
        if (image.isNull()) image = m_defaultIcon;
        auto* item = new QListWidgetItem(image, entry.name, m_list);
        item->setData(Qt::UserRole, entry.id);
        item->setSizeHint(m_list->gridSize());
        item->setTextAlignment(Qt::AlignHCenter);
        item->setData(Qt::UserRole + 1, entry.category);
        item->setToolTip(entry.name);
        if (entry.id == m_currentId) m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem()) m_list->setCurrentRow(0);
    m_category->setCurrentIndex(std::max(0, m_category->findData(previous)));
    const auto& t = th();
    setStyleSheet(QStringLiteral(
        "#TrackIconPicker { background: %1; border: 1px solid %2; border-radius: 8px; }"
        "#TrackIconGrid { background: transparent; border: none; outline: none; }"
        "#TrackIconGrid::item { border: 1px solid transparent; border-radius: 6px; padding: 4px; }"
        "#TrackIconGrid::item:hover { background: %3; }"
        "#TrackIconGrid::item:selected { background: %3; border: 1px solid %4; color: %5; }")
        .arg(t.surface.name(), t.separator().name(), mixColors(t.surface, t.accent, .16).name(),
             t.accent.name(), t.textPrimary.name()));
    filter();
}
void TrackIconPicker::filter() {
    const QString query = m_search->text().trimmed();
    const QString category = m_category->currentData().toString();
    int visible = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        auto* item = m_list->item(i);
        const QString itemCategory = item->data(Qt::UserRole + 1).toString();
        const QString words = item->text() + QLatin1Char(' ') + itemCategory + QLatin1Char(' ') + item->data(Qt::UserRole).toString();
        const bool show = (category.isEmpty() || category == itemCategory) && words.contains(query, Qt::CaseInsensitive);
        item->setHidden(!show);
        visible += show;
    }
    if (m_list->currentItem() && m_list->currentItem()->isHidden()) m_list->setCurrentRow(-1);
    m_empty->setVisible(visible == 0);
}
void TrackIconPicker::popup(const QPoint& globalPosition) {
    auto* screen = QGuiApplication::screenAt(globalPosition);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry().adjusted(6, 6, -6, -6);
        resize(size().boundedTo(available.size()));
        move(std::clamp(globalPosition.x(), available.left(), available.right() - width() + 1),
             std::clamp(globalPosition.y(), available.top(), available.bottom() - height() + 1));
    } else move(globalPosition);
    show();
    m_search->setFocus();
    if (m_list->currentItem()) m_list->scrollToItem(m_list->currentItem());
}
bool TrackIconPicker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Return || key == Qt::Key_Enter) {
            auto* choice = m_list->currentItem();
            if (key == Qt::Key_Down || !choice || choice->isHidden()) {
                choice = nullptr;
                for (int i = 0; i < m_list->count(); ++i) if (!m_list->item(i)->isHidden()) {
                    choice = m_list->item(i); break;
                }
            }
            if (choice) {
                if (key == Qt::Key_Down) {
                    m_list->setCurrentItem(choice); m_list->setFocus();
                } else {
                    const QString id = choice->data(Qt::UserRole).toString();
                    accept(); emit iconSelected(id);
                }
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
