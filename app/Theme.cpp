#include "Theme.hpp"
#include "Typography.hpp"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QWidget>

#include <cmath>

namespace {

/// Build a theme from 0-255 component tuples — keeps the preset table compact.
Theme make(const char* id, const char* name, bool dark,
           QColor background, QColor surface, QColor surfaceElevated,
           QColor textPrimary, QColor textSecondary,
           QColor accent, QColor accentHighlight,
           QColor waveform, QColor cursor,
           QColor gridLine, QColor gridLineStrong, QColor selection,
           QColor transportBackground, QColor toolbarBackground) {
    Theme t;
    t.id = QString::fromLatin1(id);
    t.name = QString::fromLatin1(name);
    t.dark = dark;
    t.background = background;
    t.surface = surface;
    t.surfaceElevated = surfaceElevated;
    t.textPrimary = textPrimary;
    t.textSecondary = textSecondary;
    t.accent = accent;
    t.accentHighlight = accentHighlight;
    t.waveform = waveform;
    t.cursor = cursor;
    t.gridLine = gridLine;
    t.gridLineStrong = gridLineStrong;
    t.selection = selection;
    t.transportBackground = transportBackground;
    t.toolbarBackground = toolbarBackground;
    return t;
}

QColor grey(int v) { return QColor(v, v, v); }

} // namespace

QColor Theme::well() const {
    // A well is a hole in the surface, so it always recedes: darker still on a
    // dark palette, and *down* towards grey on a light one rather than up
    // towards white. Brightening it on a light theme turned every meter, groove
    // and knob arc into white-on-white.
    return mixColors(background, QColor(0, 0, 0), dark ? 0.35 : 0.07);
}

QColor Theme::separator() const {
    return mixColors(surface, dark ? QColor(255, 255, 255) : QColor(0, 0, 0),
                     0.10);
}

QColor Theme::sectionDivider() const {
    QColor line = mixColors(surface,
                            dark ? QColor(255, 255, 255) : QColor(0, 0, 0),
                            dark ? 0.18 : 0.22);
    // A trace of the product accent keeps the large structural lines from
    // looking like generic grey dividers, while staying neutral at a glance.
    return mixColors(line, accent, dark ? 0.08 : 0.05);
}

QColor Theme::ink(int alpha) const {
    return dark ? QColor(255, 255, 255, alpha) : QColor(22, 25, 29, alpha);
}

QColor colorFromRgb(quint32 rgb) {
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF),
                  int(rgb & 0xFF));
}

QColor mixColors(const QColor& a, const QColor& b, double t) {
    const double u = 1.0 - t;
    return QColor(int(a.red() * u + b.red() * t),
                  int(a.green() * u + b.green() * t),
                  int(a.blue() * u + b.blue() * t),
                  int(a.alpha() * u + b.alpha() * t));
}

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager() {
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
        m_defaultFont = app->font();

    m_presets = {
        make("dark", "Dark", true,
             grey(20), grey(31), grey(38),
             grey(242), grey(153),
             QColor(74, 143, 217), QColor(102, 166, 230),
             QColor(128, 191, 255), QColor(184, 190, 198),
             grey(51), grey(77), QColor(74, 143, 217, 77),
             grey(13), grey(26)),
        // Clean cool neutrals give panels a visible hierarchy without turning
        // the workspace into a flat grey sheet. Saturated blue carries active
        // state; the warm playhead remains easy to find in a dense project.
        make("light", "Light", false,
             QColor(244, 247, 250), QColor(250, 252, 253), QColor(255, 255, 255),
             QColor(24, 34, 45), QColor(84, 101, 118),
             QColor(22, 127, 211), QColor(56, 152, 232),
             QColor(35, 109, 181), QColor(214, 72, 72),
             QColor(213, 222, 231), QColor(174, 190, 205),
             QColor(22, 127, 211, 54),
             QColor(230, 237, 243), QColor(242, 246, 249)),
        // Solarized warmth stays recognisable, with stronger ink and distinct
        // teal waveform/orange playhead accents for faster visual parsing.
        make("solarized-light", "Solarized Light", false,
             QColor(253, 246, 227), QColor(249, 241, 221), QColor(255, 250, 237),
             QColor(48, 75, 84), QColor(82, 103, 111),
             QColor(22, 139, 210), QColor(59, 164, 230),
             QColor(28, 154, 145), QColor(214, 93, 46),
             QColor(222, 211, 184), QColor(190, 171, 131),
             QColor(22, 139, 210, 56),
             QColor(242, 232, 208), QColor(248, 239, 220)),
        make("gruvbox", "Gruvbox", true,
             QColor(41, 38, 33), QColor(51, 46, 41), QColor(64, 59, 51),
             QColor(240, 232, 209), QColor(168, 153, 122),
             QColor(217, 153, 51), QColor(242, 179, 77),
             QColor(217, 179, 77), QColor(184, 190, 198),
             QColor(66, 61, 54), QColor(84, 77, 69),
             QColor(217, 153, 51, 77),
             QColor(33, 31, 26), QColor(46, 41, 36)),
    };

    // The header colour is a first-class, separately editable field. Presets
    // don't list it (to keep the make() table compact), so default it to the
    // transport background — the two matched before this field existed.
    for (auto& p : m_presets)
        if (!p.headerBackground.isValid()) p.headerBackground = p.transportBackground;

    QSettings settings;
    const QString saved = settings.value("ui/themeId", "dark").toString();
    m_theme = m_presets.first();
    bool matchedPreset = false;
    for (const auto& p : m_presets) {
        if (p.id == saved) {
            m_theme = p;
            matchedPreset = true;
        }
    }
    // A user-authored palette is stored inline and restored on launch.
    if (saved == "custom") {
        const QString json = settings.value("ui/customTheme").toString();
        const auto doc = QJsonDocument::fromJson(json.toUtf8());
        if (doc.isObject()) m_theme = fromJson(doc.object(), m_presets.first());
    } else if (!matchedPreset) {
        // Retired built-in IDs migrate once to the default instead of leaving
        // a stale value that can never be selected in Settings.
        settings.setValue("ui/themeId", QStringLiteral("dark"));
    }
    loadStoredFont();
}

namespace {
QString colorToStr(const QColor& c) { return c.name(QColor::HexArgb); }
QColor strToColor(const QString& s, const QColor& fallback) {
    const QColor c(s);
    return c.isValid() ? c : fallback;
}
} // namespace

QJsonObject ThemeManager::toJson(const Theme& t) {
    QJsonObject o;
    o["id"] = t.id;
    o["name"] = t.name;
    o["dark"] = t.dark;
    o["background"] = colorToStr(t.background);
    o["surface"] = colorToStr(t.surface);
    o["surfaceElevated"] = colorToStr(t.surfaceElevated);
    o["textPrimary"] = colorToStr(t.textPrimary);
    o["textSecondary"] = colorToStr(t.textSecondary);
    o["accent"] = colorToStr(t.accent);
    o["accentHighlight"] = colorToStr(t.accentHighlight);
    o["waveform"] = colorToStr(t.waveform);
    o["cursor"] = colorToStr(t.cursor);
    o["gridLine"] = colorToStr(t.gridLine);
    o["gridLineStrong"] = colorToStr(t.gridLineStrong);
    o["selection"] = colorToStr(t.selection);
    o["transportBackground"] = colorToStr(t.transportBackground);
    o["headerBackground"] = colorToStr(t.headerBackground);
    o["toolbarBackground"] = colorToStr(t.toolbarBackground);
    o["pluginMenuBackground"] = colorToStr(t.pluginMenuBackground);
    return o;
}

Theme ThemeManager::fromJson(const QJsonObject& o, const Theme& base) {
    Theme t = base;
    t.id = o.value("id").toString(base.id);
    t.name = o.value("name").toString(base.name);
    t.dark = o.value("dark").toBool(base.dark);
    auto col = [&](const char* key, const QColor& fb) {
        return o.contains(key) ? strToColor(o.value(key).toString(), fb) : fb;
    };
    t.background = col("background", base.background);
    t.surface = col("surface", base.surface);
    t.surfaceElevated = col("surfaceElevated", base.surfaceElevated);
    t.textPrimary = col("textPrimary", base.textPrimary);
    t.textSecondary = col("textSecondary", base.textSecondary);
    t.accent = col("accent", base.accent);
    t.accentHighlight = col("accentHighlight", base.accentHighlight);
    t.waveform = col("waveform", base.waveform);
    t.cursor = col("cursor", base.cursor);
    t.gridLine = col("gridLine", base.gridLine);
    t.gridLineStrong = col("gridLineStrong", base.gridLineStrong);
    t.selection = col("selection", base.selection);
    t.transportBackground = col("transportBackground", base.transportBackground);
    t.headerBackground = col("headerBackground", base.transportBackground);
    t.toolbarBackground = col("toolbarBackground", base.toolbarBackground);
    t.pluginMenuBackground = col("pluginMenuBackground", base.pluginMenuBackground);
    return t;
}

void ThemeManager::applyCustomTheme(Theme theme, bool persist) {
    theme.id = "custom";
    if (theme.name.isEmpty()) theme.name = "Custom";
    if (!theme.headerBackground.isValid())
        theme.headerBackground = theme.transportBackground;
    m_theme = theme;
    if (persist) {
        QSettings s;
        s.setValue("ui/themeId", "custom");
        s.setValue("ui/customTheme",
                   QString::fromUtf8(QJsonDocument(toJson(theme)).toJson(
                       QJsonDocument::Compact)));
    }
    apply();
    emit changed();
}

void ThemeManager::setThemeId(const QString& id, bool persist) {
    for (const auto& p : m_presets) {
        if (p.id != id) continue;
        m_theme = p;
        if (persist) QSettings().setValue("ui/themeId", id);
        apply();
        emit changed();
        return;
    }
}

QString ThemeManager::fontDataPath() const {
    QString root;
    if (QCoreApplication* app = QCoreApplication::instance())
        root = app->property("dawHeadlessDataRoot").toString();
    if (root.isEmpty())
        root = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
    return QDir(root).filePath(QStringLiteral("Fonts/custom-font.data"));
}

QString ThemeManager::customFontPath() const {
    return hasCustomFont() && QFileInfo::exists(fontDataPath())
               ? fontDataPath()
               : QString();
}

void ThemeManager::loadStoredFont() {
    QSettings settings;
    const QString fileName =
        settings.value(QStringLiteral("ui/customFontFileName")).toString();
    if (fileName.isEmpty()) return;

    QFile file(fontDataPath());
    constexpr qint64 kMaxFontBytes = 64 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
        file.size() > kMaxFontBytes) {
        settings.remove(QStringLiteral("ui/customFontFileName"));
        return;
    }
    const int id = QFontDatabase::addApplicationFontFromData(file.readAll());
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    if (id < 0 || families.isEmpty()) {
        if (id >= 0) QFontDatabase::removeApplicationFont(id);
        settings.remove(QStringLiteral("ui/customFontFileName"));
        return;
    }
    m_fontId = id;
    m_fontFamily = families.first();
    m_fontFileName = fileName;
}

bool ThemeManager::importFont(const QString& path, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (suffix != QLatin1String("ttf") && suffix != QLatin1String("otf") &&
        suffix != QLatin1String("ttc") && suffix != QLatin1String("otc")) {
        return fail(tr("The selected file is not a supported font. Choose a "
                       "TTF, OTF, TTC, or OTC file."));
    }

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly))
        return fail(tr("Could not open %1.").arg(path));
    constexpr qint64 kMaxFontBytes = 64 * 1024 * 1024;
    if (source.size() <= 0)
        return fail(tr("The selected font file is empty."));
    if (source.size() > kMaxFontBytes)
        return fail(tr("The selected font is larger than 64 MiB."));
    const QByteArray data = source.readAll();
    if (data.size() != source.size())
        return fail(tr("Could not read %1.").arg(path));

    const int newId = QFontDatabase::addApplicationFontFromData(data);
    const QStringList families = QFontDatabase::applicationFontFamilies(newId);
    if (newId < 0 || families.isEmpty()) {
        if (newId >= 0) QFontDatabase::removeApplicationFont(newId);
        return fail(tr("The selected file does not contain a usable font. "
                       "Choose another TTF, OTF, TTC, or OTC file."));
    }

    const QString destination = fontDataPath();
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        QFontDatabase::removeApplicationFont(newId);
        return fail(tr("Could not create the application font folder."));
    }
    QSaveFile saved(destination);
    if (!saved.open(QIODevice::WriteOnly) || saved.write(data) != data.size() ||
        !saved.commit()) {
        QFontDatabase::removeApplicationFont(newId);
        return fail(tr("Could not save the font in the application data "
                       "folder. Check free disk space and folder permissions."));
    }

    if (m_fontId >= 0) QFontDatabase::removeApplicationFont(m_fontId);
    m_fontId = newId;
    m_fontFamily = families.first();
    m_fontFileName = info.fileName();
    QSettings().setValue(QStringLiteral("ui/customFontFileName"),
                         m_fontFileName);
    applyFont();
    emit fontChanged();
    return true;
}

void ThemeManager::resetFont() {
    if (m_fontId >= 0) QFontDatabase::removeApplicationFont(m_fontId);
    m_fontId = -1;
    m_fontFamily.clear();
    m_fontFileName.clear();
    QSettings().remove(QStringLiteral("ui/customFontFileName"));
    QFile::remove(fontDataPath());
    applyFont();
    emit fontChanged();
}

void ThemeManager::applyFont() {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) return;

    QStringList families = m_defaultFont.families();
    if (families.isEmpty() && !m_defaultFont.family().isEmpty())
        families.append(m_defaultFont.family());
    if (!m_fontFamily.isEmpty()) {
        families.removeAll(m_fontFamily);
        families.prepend(m_fontFamily);
    }

    QFont applicationFont = m_defaultFont;
    applicationFont.setFamilies(families);
    app->setFont(applicationFont);
    // Some controls derive a bold/small font once at construction time. Keep
    // those traits, but replace their family too so the change is truly live.
    for (QWidget* widget : QApplication::allWidgets()) {
        QFont font = widget->font();
        font.setFamilies(families);
        widget->setFont(font);
    }
}

bool ThemeManager::checkFontForTest(QString* error) {
    if (!ui::checkBundledFontsForTest(error)) return false;
    if (hasCustomFont()) {
        if (error) *error = QStringLiteral("test started with a custom font");
        return false;
    }

    QTemporaryDir invalidRoot;
    QFile invalid(invalidRoot.filePath(QStringLiteral("invalid.ttf")));
    if (!invalid.open(QIODevice::WriteOnly) || invalid.write("not a font") < 0) {
        if (error) *error = QStringLiteral("could not create invalid font fixture");
        return false;
    }
    invalid.close();
    QString rejected;
    if (importFont(invalid.fileName(), &rejected) || hasCustomFont()) {
        if (error) *error = QStringLiteral("invalid font was accepted");
        return false;
    }

    QWidget existingWidget;
    QFont explicitFont = existingWidget.font();
    explicitFont.setBold(true);
    existingWidget.setFont(explicitFont);

    QStringList roots =
        QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
#if defined(Q_OS_WIN)
    const QString windows = qEnvironmentVariable("WINDIR");
    if (!windows.isEmpty()) roots.append(QDir(windows).filePath("Fonts"));
#elif defined(Q_OS_MACOS)
    roots << QStringLiteral("/System/Library/Fonts")
          << QStringLiteral("/Library/Fonts");
#else
    roots << QStringLiteral("/usr/share/fonts")
          << QStringLiteral("/usr/local/share/fonts");
#endif
    roots.removeDuplicates();

    const QStringList filters = {QStringLiteral("*.ttf"),
                                 QStringLiteral("*.otf"),
                                 QStringLiteral("*.ttc"),
                                 QStringLiteral("*.otc")};
    QString imported;
    for (const QString& root : roots) {
        QDirIterator files(root, filters, QDir::Files,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString candidate = files.next();
            QString importError;
            if (importFont(candidate, &importError)) {
                imported = candidate;
                break;
            }
        }
        if (!imported.isEmpty()) break;
    }
    if (imported.isEmpty()) {
        if (error) *error = QStringLiteral("no readable system font fixture found");
        return false;
    }
    if (!hasCustomFont() || customFontFamily().isEmpty()) {
        if (error) *error = QStringLiteral("valid font was not applied");
        resetFont();
        return false;
    }
    if (existingWidget.font().families().isEmpty() ||
        existingWidget.font().families().first() != customFontFamily()) {
        if (error) *error = QStringLiteral("existing widget kept its old font");
        resetFont();
        return false;
    }
    resetFont();
    if (hasCustomFont() || existingWidget.font().family() != QStringLiteral("Inter") ||
        !existingWidget.font().bold() || QApplication::font().family() != QStringLiteral("Inter")) {
        if (error) *error = QStringLiteral("font reset did not restore defaults");
        return false;
    }
    return true;
}

void ThemeManager::apply() {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) return;

    app->setStyle(QStyleFactory::create("Fusion"));

    const Theme& t = m_theme;
    QPalette p;
    p.setColor(QPalette::Window, t.background);
    p.setColor(QPalette::WindowText, t.textPrimary);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.surfaceElevated);
    p.setColor(QPalette::Text, t.textPrimary);
    p.setColor(QPalette::PlaceholderText, t.textSecondary);
    p.setColor(QPalette::Button, t.surfaceElevated);
    p.setColor(QPalette::ButtonText, t.textPrimary);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.dark ? Qt::white : Qt::white);
    p.setColor(QPalette::ToolTipBase, t.surfaceElevated);
    p.setColor(QPalette::ToolTipText, t.textPrimary);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textSecondary);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textSecondary);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.textSecondary);
    app->setPalette(p);
    app->setStyleSheet(styleSheet());
    applyFont();
}

QString ThemeManager::styleSheet() const {
    const Theme& t = m_theme;
    auto c = [](const QColor& col) {
        return QString("rgba(%1,%2,%3,%4)")
            .arg(col.red()).arg(col.green()).arg(col.blue())
            .arg(QString::number(col.alphaF(), 'f', 3));
    };
    const auto relativeLuminance = [](const QColor& colour) {
        const auto channel = [](double value) {
            return value <= 0.04045
                       ? value / 12.92
                       : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(colour.redF()) +
               0.7152 * channel(colour.greenF()) +
               0.0722 * channel(colour.blueF());
    };
    const QColor accentText = relativeLuminance(t.accent) > 0.179
                                  ? QColor(18, 18, 20)
                                  : QColor(250, 250, 252);

    // Deliberately flat: thin 1px borders, 6px radii, no bevels or gradients —
    // the "3D" Fusion look is what we are getting away from.
    return QString(R"(
QWidget { color: %TEXT%; font-size: 12px; }
QMainWindow, QDialog { background: %BG%; }
QMenuBar { background: %TOOLBAR%; border: none; }
QMenuBar::item { padding: 4px 10px; background: transparent; border-radius: 5px; }
QMenuBar::item:selected { background: %ACCENT_SOFT%; }
/* Menus are a list, not a set of buttons: tight rows, one line of text each,
   and only as much padding as it takes to keep the highlight off the border.
   The 24px rows this used to have turned a plugin list into a scroll.

   The plate is a *well*, not a raised surface: a popup that recedes reads as a
   hole punched through the window rather than a card floating over it, and it
   is the one look every drop-down in the application shares — the grid chip in
   the transport bar had it on its own, and now nothing has to. */
QMenu { background: %WELL%; border: 1px solid %SEP%; border-radius: 10px;
        padding: 5px; }
QMenu::item { min-height: 17px; padding: 3px 20px 3px 9px; border-radius: 6px;
              font-size: 12px; }
QMenu::item:selected { background: %ACCENT_SOFT%; color: %TEXT%; }
QMenu::item:checked { color: %ACCENT_HL%; font-weight: 600; }
QMenu::item:disabled { color: %TEXT2%; }
QMenu::icon { padding-left: 5px; }
QMenu::indicator { width: 0; height: 0; }
QMenu::separator { height: 1px; background: %SEP%; margin: 3px 6px; }
/* A menu long enough to scroll gets arrows at its ends; unstyled they are a
   grey Fusion strip that does not belong to any of this. */
QMenu::scroller { height: 14px; background: %WELL%; }

QStatusBar { background: %TOOLBAR%; border-top: 1px solid %SEP%; }
QStatusBar QLabel { color: %TEXT2%; font-size: 11px; }
QStatusBar::item { border: none; }

QToolTip { background: %ELEV%; color: %TEXT%; border: 1px solid %SEP%;
           border-radius: 6px; padding: 4px 6px; }

QScrollArea, QAbstractScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: %SCROLL%; border-radius: 4px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %SCROLL_HOVER%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
    background: %WELL%; border: 1px solid %SEP%; border-radius: 6px;
    padding: 3px 7px; selection-background-color: %ACCENT%;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus {
    border: 1px solid %ACCENT%;
}
/* A closed combo is a chip: the same pill the transport bar's grid selector
   is, so the two are one control with different contents rather than two
   different-looking ways to pick from a list. */
QComboBox {
    background: %WELL%; border: 1px solid %SEP%; border-radius: 9px;
    padding: 3px 9px; min-height: 20px; color: %TEXT%; font-weight: 500;
    selection-background-color: %ACCENT%;
}
QComboBox:hover { background: %HOVER%; border-color: %HOVER%; }
QComboBox:focus, QComboBox:on { border: 1px solid %ACCENT%; }
QComboBox:disabled { color: %TEXT2%; }
/* No Fusion arrow: the chip is the affordance, and a native triangle in the
   corner is the one part of it that never matched anything else here. */
QComboBox::drop-down { border: none; width: 14px; }
QComboBox::down-arrow { image: none; width: 0; height: 0; }
/* Exactly the menu plate above — a combo's list and a menu are the same
   object with different contents. */
QComboBox QAbstractItemView {
    background: %WELL%; border: 1px solid %SEP%; border-radius: 10px;
    selection-background-color: %ACCENT_SOFT%; outline: none; padding: 5px;
}
QComboBox QAbstractItemView::item { min-height: 18px; padding: 3px 7px;
                                    border-radius: 6px; }

QPushButton {
    background: %ELEV%; border: 1px solid %SEP%; border-radius: 6px;
    padding: 4px 12px; font-weight: 500;
}
QPushButton:hover { background: %ELEV_HOVER%; }
QPushButton:pressed { background: %ACCENT_SOFT%; }
QPushButton:checked { background: %ACCENT%; color: white; border-color: %ACCENT%; }
QPushButton:disabled { color: %TEXT2%; }
QPushButton[accentAction="true"] {
    background: %ACCENT%; color: %ACCENT_TEXT%; border-color: %ACCENT%;
    font-weight: 600;
}
QPushButton[accentAction="true"]:hover { background: %ACCENT_HL%; }
QPushButton[accentAction="true"]:pressed { background: %ACCENT_DARK%; }
QPushButton[accentAction="true"]:disabled {
    background: %ELEV%; color: %TEXT2%; border-color: %SEP%;
}

QToolButton { background: transparent; border: none; border-radius: 6px; padding: 3px; font-weight: 500; }
QToolButton:hover { background: %HOVER%; }
QToolButton:checked { background: %ACCENT_SOFT%; }

QSplitter::handle { background: %SECTION%; }
QSplitter::handle:hover { background: %ACCENT_SOFT%; }

QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: %TOOLBAR%; padding: 5px 8px; border-bottom: 1px solid %SEP%; }

QGroupBox { border: 1px solid %SEP%; border-radius: 8px; margin-top: 14px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; color: %TEXT2%; font-weight: 600; }

QLabel[role="section"] { color: %TEXT2%; font-size: 10px; font-weight: 600; }
QLabel[role="pageTitle"] { font-size: 18px; font-weight: 600; }
QLabel[role="secondary"] { color: %TEXT2%; }

/* Fallback for third-party/plain QSliders. Application-owned sliders use
   ui::GlassSlider and the same proportions: a quiet 4px rail under an 18px
   translucent handle. Negative margins let the pane float over the rail. */
QSlider { outline: none; }
QSlider::groove:horizontal {
    height: 4px; background: %WELL%; border: none; border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                stop:0 %ACCENT_DARK%, stop:1 %ACCENT%);
    border-radius: 2px;
}
QSlider::add-page:horizontal { background: %WELL%; border-radius: 2px; }
QSlider::handle:horizontal {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_HI%, stop:1 %GLASS_LO%);
    width: 18px; margin: -7px; border-radius: 9px;
    border: 1px solid %GLASS_LIT%;
}
QSlider::handle:horizontal:hover, QSlider::handle:horizontal:pressed {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_LIT%, stop:1 %GLASS_HI%);
}
QSlider::groove:horizontal:focus { border: 1px solid %ACCENT%; }
QSlider::groove:vertical {
    width: 4px; background: %WELL%; border: none; border-radius: 2px;
}
QSlider::add-page:vertical {
    background: qlineargradient(x1:0, y1:1, x2:0, y2:0,
                                stop:0 %ACCENT_DARK%, stop:1 %ACCENT%);
    border-radius: 2px;
}
QSlider::sub-page:vertical { background: %WELL%; border-radius: 2px; }
QSlider::handle:vertical {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_HI%, stop:1 %GLASS_LO%);
    height: 18px; margin: -7px; border-radius: 9px;
    border: 1px solid %GLASS_LIT%;
}
QSlider::handle:vertical:hover, QSlider::handle:vertical:pressed {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_LIT%, stop:1 %GLASS_HI%);
}
QSlider::groove:vertical:focus { border: 1px solid %ACCENT%; }
)")
        .replace("%TEXT%", c(t.textPrimary))
        .replace("%TEXT2%", c(t.textSecondary))
        .replace("%BG%", c(t.background))
        .replace("%SURFACE%", c(t.surface))
        .replace("%ELEV_HOVER%", c(mixColors(t.surfaceElevated, t.textPrimary, 0.10)))
        .replace("%ELEV%", c(t.surfaceElevated))
        .replace("%ACCENT_TEXT%", c(accentText))
        .replace("%TOOLBAR%", c(t.toolbarBackground))
        .replace("%SEP%", c(t.separator()))
        .replace("%SECTION%", c(t.sectionDivider()))
        .replace("%WELL%", c(t.well()))
        // The glass handle, flattened to two opaque stops: QSS has no backdrop
        // and no specular, so the cap is mixed against the surface it sits on
        // rather than composited over it.
        .replace("%GLASS_LIT%", c(mixColors(t.surfaceElevated, QColor(255, 255, 255),
                                            t.dark ? 0.88 : 0.98)))
        .replace("%GLASS_HI%", c(mixColors(t.surfaceElevated, QColor(255, 255, 255),
                                           t.dark ? 0.74 : 0.92)))
        .replace("%GLASS_LO%", c(mixColors(t.surfaceElevated, QColor(255, 255, 255),
                                           t.dark ? 0.50 : 0.74)))
        .replace("%ACCENT_SOFT%", c(QColor(t.accent.red(), t.accent.green(),
                                           t.accent.blue(), 60)))
        .replace("%ACCENT_HL%", c(t.accentHighlight))
        .replace("%ACCENT_DARK%", c(mixColors(t.accent, t.background, 0.23)))
        .replace("%ACCENT%", c(t.accent))
        .replace("%HOVER%", c(QColor(t.textPrimary.red(), t.textPrimary.green(),
                                     t.textPrimary.blue(), 26)))
        .replace("%SCROLL_HOVER%", c(mixColors(t.gridLineStrong, t.textPrimary, 0.35)))
        .replace("%SCROLL%", c(t.gridLineStrong));
}
