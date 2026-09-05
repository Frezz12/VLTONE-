#include "CollaborationDialogStyle.hpp"

#include "Theme.hpp"

#include <QFont>
#include <QLabel>
#include <QRegularExpression>
#include <QUuid>

namespace collab::dialog {

QLabel* titleLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 3);
    label->setFont(font);
    label->setWordWrap(true);
    return label;
}

QString boundedSafeMessage(const QString& value, const QString& fallback) {
    const QString bounded =
        value.trimmed().left(kMaximumSafeMessageCharacters);
    return bounded.isEmpty() ? fallback : bounded;
}

void wipe(QString& value) {
    if (!value.isEmpty()) value.fill(QChar(0));
    value.clear();
    value.squeeze();
}

QString canonicalUuid(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
        "[0-9a-f]{4}-[0-9a-f]{12}$"));
    const QString text = value.trimmed();
    const QUuid uuid(text);
    if (!pattern.match(text).hasMatch() || uuid.isNull()) return {};
    return uuid.toString(QUuid::WithoutBraces).toLower();
}

QString styleSheet() {
    const Theme& t = th();
    return QString(R"(
QGroupBox { border: 1px solid %SEP%; border-radius: 8px; margin-top: 9px;
            padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px;
                   color: %TEXT2%; font-size: 11px; font-weight: 700; }
QListWidget, QTreeWidget { background: %WELL%; border: 1px solid %SEP%;
                           border-radius: 7px;
                           alternate-background-color: %ALT%; outline: none; }
QListWidget::item, QTreeWidget::item { padding: 3px 6px; border: none; }
QListWidget::item:selected, QTreeWidget::item:selected { background: %ACCENT%;
                                                         color: white; }
QScrollArea { background: transparent; border: none; }
QProgressBar { background: %WELL%; border: none; border-radius: 3px;
               text-align: center; color: %TEXT2%; font-size: 10px; }
QProgressBar::chunk { background: %ACCENT%; border-radius: 3px; }
#CollabSecondary { color: %TEXT2%; font-size: 11px; }
#CollabWarning { color: %WARN%; font-size: 11px; }
#CollabError { color: %ERROR%; font-size: 11px; }
#CollabCode { color: %TEXT1%; font-size: 21px; font-weight: 600;
              letter-spacing: 3px; background: %WELL%;
              border: 1px solid %SEP%; border-radius: 8px; padding: 10px 14px; }
)")
        .replace("%WELL%", t.well().name())
        .replace("%ALT%", mixColors(t.well(), t.surface, 0.45).name())
        .replace("%SEP%", t.separator().name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%WARN%", Theme::cycle().name())
        .replace("%ERROR%", Theme::record().name())
        .replace("%TEXT1%", t.textPrimary.name())
        .replace("%TEXT2%", t.textSecondary.name());
}

} // namespace collab::dialog
