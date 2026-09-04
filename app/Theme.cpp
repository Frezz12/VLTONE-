#include "Theme.hpp"

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
    return mixColors(background, QColor(0, 0, 0), dark ? 0.35 : 0.10);
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
        m_systemFont = app->font();

    m_presets = {
        make("logic", "Logic Graphite", true,
             QColor(24, 26, 27), QColor(36, 37, 38), QColor(48, 50, 51),
             QColor(230, 232, 230), QColor(146, 150, 153),
             QColor(51, 158, 184), QColor(77, 186, 209),
             QColor(77, 184, 179), QColor(184, 190, 198),
             QColor(46, 47, 48), QColor(71, 73, 74),
             QColor(51, 158, 184, 72),
             QColor(17, 18, 19), QColor(31, 32, 33)),
        // Logic's *light* face: pale, faintly cool greys with near-black text,
        // the chrome a shade darker than the workspace rather than a shade
        // lighter. The trap with a light DAW palette is going white — the
        // arrangement has to stay a grey the clips can sit on top of, or every
        // clip reads as a hole punched in the page.
        make("logic-light", "Logic Light", false,
             QColor(203, 204, 206), QColor(226, 227, 229), QColor(238, 239, 241),
             QColor(29, 30, 32), QColor(107, 110, 115),
             QColor(31, 122, 150), QColor(45, 150, 181),
             QColor(46, 110, 128), QColor(46, 52, 59),
             QColor(185, 186, 189), QColor(151, 154, 158),
             QColor(31, 122, 150, 56),
             QColor(213, 214, 217), QColor(222, 223, 226)),
        // Neither a dark room nor a sheet of paper: the mid-grey a hardware
        // console is moulded in. Light palettes in this application had only
        // near-white surfaces, where every well, groove and shadow had almost
        // no room left to recede into. Starting the surfaces two shades down
        // gives the whole depth vocabulary somewhere to go in both directions.
        make("graphite", "Graphite Grey", false,
             QColor(158, 160, 163), QColor(182, 184, 188),
             QColor(200, 202, 206),
             QColor(24, 26, 29), QColor(78, 82, 88),
             QColor(38, 110, 134), QColor(54, 138, 164),
             QColor(44, 86, 102), QColor(30, 36, 44),
             QColor(140, 142, 146), QColor(112, 114, 118),
             QColor(38, 110, 134, 64),
             QColor(146, 148, 152), QColor(152, 154, 158)),
        make("dark", "Dark", true,
             grey(20), grey(31), grey(38),
             grey(242), grey(153),
             QColor(74, 143, 217), QColor(102, 166, 230),
             QColor(128, 191, 255), QColor(184, 190, 198),
             grey(51), grey(77), QColor(74, 143, 217, 77),
             grey(13), grey(26)),
        make("light", "Light", false,
             grey(242), grey(230), grey(217),
             grey(20), grey(102),
             QColor(74, 143, 217), QColor(102, 166, 230),
             QColor(51, 115, 204), QColor(110, 118, 128),
             grey(204), grey(179), QColor(74, 143, 217, 51),
             grey(235), grey(237)),
        make("midnight", "Midnight", true,
             QColor(8, 10, 15), QColor(15, 20, 31), QColor(23, 28, 41),
             QColor(217, 230, 242), QColor(115, 128, 153),
             QColor(64, 140, 255), QColor(102, 166, 255),
             QColor(77, 153, 255), QColor(184, 190, 198),
             QColor(31, 36, 51), QColor(51, 56, 77),
             QColor(64, 140, 255, 77),
             QColor(5, 8, 13), QColor(13, 15, 26)),
        make("carbon", "Carbon", true,
             grey(15), grey(26), grey(36),
             grey(235), grey(140),
             QColor(230, 128, 51), QColor(255, 153, 77),
             QColor(255, 166, 77), QColor(184, 190, 198),
             grey(41), grey(61), QColor(230, 128, 51, 77),
             grey(10), grey(20)),
        make("dracula", "Dracula", true,
             QColor(36, 36, 46), QColor(41, 41, 56), QColor(51, 51, 71),
             QColor(242, 242, 242), QColor(153, 153, 179),
             QColor(217, 102, 204), QColor(242, 128, 230),
             QColor(204, 128, 255), QColor(184, 190, 198),
             QColor(56, 56, 71), QColor(77, 77, 97),
             QColor(217, 102, 204, 77),
             QColor(26, 26, 36), QColor(36, 36, 46)),
        make("solarized-dark", "Solarized Dark", true,
             QColor(0, 43, 54), QColor(10, 54, 66), QColor(18, 69, 82),
             QColor(237, 232, 214), QColor(140, 161, 168),
             QColor(38, 140, 209), QColor(64, 166, 235),
             QColor(89, 191, 217), QColor(184, 190, 198),
             QColor(18, 61, 74), QColor(31, 77, 89),
             QColor(38, 140, 209, 77),
             QColor(0, 36, 46), QColor(8, 48, 59)),
        make("solarized-light", "Solarized Light", false,
             QColor(253, 245, 227), QColor(245, 237, 219), QColor(237, 227, 207),
             QColor(102, 122, 130), QColor(140, 161, 168),
             QColor(38, 140, 209), QColor(64, 166, 235),
             QColor(38, 140, 209), QColor(110, 118, 128),
             QColor(224, 214, 194), QColor(209, 196, 176),
             QColor(38, 140, 209, 51),
             QColor(247, 240, 222), QColor(250, 242, 224)),
        make("nord", "Nord", true,
             QColor(33, 41, 51), QColor(41, 48, 61), QColor(51, 59, 74),
             QColor(217, 222, 232), QColor(133, 143, 158),
             QColor(102, 153, 204), QColor(128, 179, 230),
             QColor(128, 179, 230), QColor(184, 190, 198),
             QColor(56, 64, 77), QColor(71, 79, 94),
             QColor(102, 153, 204, 77),
             QColor(28, 36, 46), QColor(38, 46, 56)),
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
    const QString saved = settings.value("ui/themeId", "logic").toString();
    m_theme = m_presets.first();
    for (const auto& p : m_presets) {
        if (p.id == saved) m_theme = p;
    }
    // A user-authored palette is stored inline and restored on launch.
    if (saved == "custom") {
        const QString json = settings.value("ui/customTheme").toString();
        const auto doc = QJsonDocument::fromJson(json.toUtf8());
        if (doc.isObject()) m_theme = fromJson(doc.object(), m_presets.first());
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

    QStringList families = m_systemFont.families();
    if (families.isEmpty() && !m_systemFont.family().isEmpty())
        families.append(m_systemFont.family());
    if (!m_fontFamily.isEmpty()) {
        families.removeAll(m_fontFamily);
        families.prepend(m_fontFamily);
    }

    QFont applicationFont = m_systemFont;
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
    if (hasCustomFont() || existingWidget.font().family() != m_systemFont.family()) {
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
QMenu::item:checked { color: %ACCENT_HL%; font-weight: 700; }
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
    padding: 3px 9px; min-height: 20px; color: %TEXT%; font-weight: 600;
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
    padding: 4px 12px;
}
QPushButton:hover { background: %ELEV_HOVER%; }
QPushButton:pressed { background: %ACCENT_SOFT%; }
QPushButton:checked { background: %ACCENT%; color: white; border-color: %ACCENT%; }
QPushButton:disabled { color: %TEXT2%; }

QToolButton { background: transparent; border: none; border-radius: 6px; padding: 3px; }
QToolButton:hover { background: %HOVER%; }
QToolButton:checked { background: %ACCENT_SOFT%; }

QSplitter::handle { background: %SECTION%; }
QSplitter::handle:hover { background: %ACCENT_SOFT%; }

QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: %TOOLBAR%; padding: 5px 8px; border-bottom: 1px solid %SEP%; }

QGroupBox { border: 1px solid %SEP%; border-radius: 8px; margin-top: 14px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; color: %TEXT2%; }

QLabel[role="section"] { color: %TEXT2%; font-size: 10px; font-weight: 700; }

/* One thick track with a round glass handle riding inside it — the same look
   `ui::paintSlider` paints by hand for the faders, so a plain QSlider in the
   settings or a generic plugin editor is not a different control. The handle is
   10px in a 14px groove: 2px of track shows all the way round it. */
QSlider { outline: none; }
QSlider::groove:horizontal {
    height: 14px; background: %WELL%; border: none; border-radius: 7px;
}
QSlider::sub-page:horizontal {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                stop:0 %ACCENT_DARK%, stop:1 %ACCENT%);
    border-radius: 7px;
}
QSlider::add-page:horizontal { background: %WELL%; border-radius: 7px; }
QSlider::handle:horizontal {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_HI%, stop:1 %GLASS_LO%);
    width: 10px; margin: 2px; border-radius: 5px; border: none;
}
QSlider::handle:horizontal:hover, QSlider::handle:horizontal:pressed {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_LIT%, stop:1 %GLASS_HI%);
}
QSlider::groove:horizontal:focus { border: 1px solid %ACCENT%; }
QSlider::groove:vertical {
    width: 14px; background: %WELL%; border: none; border-radius: 7px;
}
QSlider::add-page:vertical {
    background: qlineargradient(x1:0, y1:1, x2:0, y2:0,
                                stop:0 %ACCENT_DARK%, stop:1 %ACCENT%);
    border-radius: 7px;
}
QSlider::sub-page:vertical { background: %WELL%; border-radius: 7px; }
QSlider::handle:vertical {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %GLASS_HI%, stop:1 %GLASS_LO%);
    height: 10px; margin: 2px; border-radius: 5px; border: none;
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
