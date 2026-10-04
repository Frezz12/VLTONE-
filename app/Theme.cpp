#include "Theme.hpp"
#include "Typography.hpp"
#include "PopupStyle.hpp"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QEvent>
#include <QFrame>
#include <QMenu>
#include <QRegion>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

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

double relativeLuminance(const QColor& colour) {
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92
                               : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.redF()) +
           0.7152 * channel(colour.greenF()) +
           0.0722 * channel(colour.blueF());
}

Theme studioGray() {
    // Cool graphite plates with a blue accent, distinct from coloured tracks.
    Theme t = make("studio-gray", "Studio Gray", true,
                   colorFromRgb(0x44484E), colorFromRgb(0x565C64), colorFromRgb(0x606771),
                   colorFromRgb(0xF5F7FA), colorFromRgb(0xDFE4EB),
                   colorFromRgb(0x82B5EA), colorFromRgb(0xA1CCF5),
                   grey(232), grey(255),
                   colorFromRgb(0x555B64), colorFromRgb(0x707985), QColor(130, 181, 234, 48),
                   colorFromRgb(0x4B5058), colorFromRgb(0x50565E));
    t.headerBackground = t.transportBackground;
    t.pluginMenuBackground = colorFromRgb(0x494F57);
    return t;
}

} // namespace

QColor Theme::well() const {
    // A well is a hole in the surface, so it always recedes: darker still on a
    // dark palette, and *down* towards grey on a light one rather than up
    // towards white. Brightening it on a light theme turned every meter, groove
    // and knob arc into white-on-white.
    return mixColors(background, QColor(0, 0, 0), dark ? 0.35 : 0.07);
}

QColor Theme::panelBottom() const {
    return mixColors(surface, headerBackground, 0.35);
}

QColor Theme::wellTop() const {
    return mixColors(well(), Qt::black, dark ? 0.16 : 0.005);
}

QColor Theme::wellBottom() const {
    return mixColors(well(), surface, 0.12);
}

QColor Theme::edgeLight(const QColor& base) const {
    return mixColors(base, Qt::white, dark ? 0.14 : 0.65);
}

QColor Theme::edgeDark(const QColor& base) const {
    return mixColors(base, Qt::black, dark ? 0.26 : 0.18);
}

QColor Theme::controlTop() const {
    return mixColors(surfaceElevated, Qt::white, dark ? 0.035 : 0.24);
}

QColor Theme::controlBottom() const {
    return mixColors(surfaceElevated, surface, 0.60);
}

QColor Theme::panelTop() const {
    return mixColors(panelBottom(), surfaceElevated, 0.60);
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

QColor Theme::accentText() const {
    const QColor darkInk = accent.saturation() == 0 ? grey(18) : QColor(18, 18, 20);
    const QColor lightInk = accent.saturation() == 0 ? grey(250) : QColor(250, 250, 252);
    const double luminance = relativeLuminance(accent);
    const double darkContrast = (luminance + 0.05) /
                                (relativeLuminance(darkInk) + 0.05);
    const double lightContrast = (relativeLuminance(lightInk) + 0.05) /
                                 (luminance + 0.05);
    if (std::max(darkContrast, lightContrast) >= 4.5)
        return darkContrast >= lightContrast ? darkInk : lightInk;
    // Mid-luminance accents need the full ink range to meet normal-text contrast.
    return (luminance + 0.05) / 0.05 >= 1.05 / (luminance + 0.05)
               ? QColor(Qt::black) : QColor(Qt::white);
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
        // Neutral black/grey chrome, including highlights and selection.
        make("dark", "Dark", true,
             grey(18), grey(28), grey(40),
             grey(237), grey(173),
             grey(180), grey(212),
             grey(192), grey(224),
             grey(40), grey(64), QColor(180, 180, 180, 60),
             grey(22), grey(24)),
        make("dark-blue", "Dark Blue", true,
             colorFromRgb(0x17191D), colorFromRgb(0x25292F), colorFromRgb(0x333943),
             colorFromRgb(0xEDF0F4), colorFromRgb(0xA8B1BD),
             colorFromRgb(0x639EE4), colorFromRgb(0x86B8F0),
             QColor(128, 191, 255), QColor(184, 190, 198),
             colorFromRgb(0x30353D), colorFromRgb(0x48515F), QColor(99, 158, 228, 77),
             colorFromRgb(0x20242A), colorFromRgb(0x22262C)),
        studioGray(),
        // Clean cool neutrals give panels a visible hierarchy without turning
        // the workspace into a flat grey sheet. Saturated blue carries active
        // state; the warm playhead remains easy to find in a dense project.
        make("light", "Light", false,
             colorFromRgb(0xD8DDE3), colorFromRgb(0xEEF0F3), colorFromRgb(0xFAFBFC),
             colorFromRgb(0x222831), colorFromRgb(0x4C5662),
             colorFromRgb(0x327BBF), colorFromRgb(0x2468A5),
             QColor(35, 109, 181), QColor(214, 72, 72),
             colorFromRgb(0xBEC6D0), colorFromRgb(0xA0ACBA),
             QColor(50, 123, 191, 54),
             colorFromRgb(0xCFD5DC), colorFromRgb(0xE4E8ED)),
        // Solarized warmth stays recognisable, with stronger ink and distinct
        // teal waveform/orange playhead accents for faster visual parsing.
        make("solarized-light", "Solarized Light", false,
             colorFromRgb(0xE9DFC9), colorFromRgb(0xF6EDDA), colorFromRgb(0xFFF7E8),
             colorFromRgb(0x304B54), colorFromRgb(0x455961),
             QColor(22, 139, 210), colorFromRgb(0x126DA6),
             QColor(28, 154, 145), QColor(214, 93, 46),
             colorFromRgb(0xD0C2A5), colorFromRgb(0xB3A17C),
             QColor(22, 139, 210, 56),
             colorFromRgb(0xDBCFB7), colorFromRgb(0xECE1CB)),
        make("gruvbox", "Gruvbox", true,
             colorFromRgb(0x25221E), colorFromRgb(0x37312A), colorFromRgb(0x494037),
             colorFromRgb(0xF0E8D1), colorFromRgb(0xCAB99A),
             QColor(217, 153, 51), QColor(242, 179, 77),
             QColor(217, 179, 77), QColor(184, 190, 198),
             QColor(66, 61, 54), QColor(84, 77, 69),
             QColor(217, 153, 51, 77),
             colorFromRgb(0x2E2923), colorFromRgb(0x322C26)),
    };

    // Header colour remains separately editable in custom palettes.
    for (auto& p : m_presets)
        if (!p.headerBackground.isValid())
            p.headerBackground = p.transportBackground;

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

    app->installEventFilter(this);
    // Open menus and combo lists directly, without Qt's legacy roll effect.
    app->setEffectEnabled(Qt::UI_AnimateMenu, false);
    app->setEffectEnabled(Qt::UI_FadeMenu, false);
    app->setEffectEnabled(Qt::UI_AnimateCombo, false);

    app->setStyle(new PopupStyle(QStyleFactory::create("Fusion")));

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
    p.setColor(QPalette::Light, t.edgeLight(t.surface));
    p.setColor(QPalette::Midlight, t.controlTop());
    p.setColor(QPalette::Mid, t.separator());
    p.setColor(QPalette::Dark, t.edgeDark(t.surface));
    p.setColor(QPalette::Shadow, t.edgeDark(t.well()));
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.accentText());
    p.setColor(QPalette::Accent, t.accent);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::LinkVisited, t.accentHighlight);
    p.setColor(QPalette::ToolTipBase, t.surfaceElevated);
    p.setColor(QPalette::ToolTipText, t.textPrimary);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textSecondary);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textSecondary);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.textSecondary);
    app->setPalette(p);
    app->setStyleSheet(styleSheet());
    applyFont();
}

bool ThemeManager::eventFilter(QObject* object, QEvent* event) {
    const auto type = event->type();
    if (type != QEvent::Polish && type != QEvent::Show && type != QEvent::Resize)
        return false;
    auto* widget = qobject_cast<QWidget*>(object);
    if (!widget) return false;
#ifdef Q_OS_WIN
    if (type == QEvent::Show && widget->isWindow()) {
        // Windows 11 rounds native top-level frames independently of Qt's QSS.
        static const auto setCornerPreference = [] {
            const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
            return dwm ? reinterpret_cast<decltype(&DwmSetWindowAttribute)>(
                             GetProcAddress(dwm, "DwmSetWindowAttribute"))
                       : nullptr;
        }();
        if (setCornerPreference) {
            const auto preference = DWMWCP_ROUND;
            setCornerPreference(reinterpret_cast<HWND>(widget->winId()),
                                DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                                sizeof(preference));
        }
    }
#endif
    const bool comboPopup = widget->inherits("QComboBoxPrivateContainer");
    const bool tooltip = widget->inherits("QTipLabel");
    if (!comboPopup && !tooltip && !qobject_cast<QMenu*>(widget)) return false;

    if (type == QEvent::Polish) {
        // The popup uses the same frameless surface in every theme.
        widget->setWindowFlag(Qt::FramelessWindowHint);
        widget->setAttribute(Qt::WA_TranslucentBackground);
        if (comboPopup) {
            if (auto* frame = qobject_cast<QFrame*>(widget))
                frame->setFrameStyle(QFrame::NoFrame);
        }
    }
    // Let QSS antialias the rounded popup against its transparent backing.
    // A rounded QRegion would quantize the curve to whole logical pixels.
    widget->clearMask();
    return false;
}

QString ThemeManager::styleSheet() const {
    const Theme& t = m_theme;
    auto c = [](const QColor& col) {
        return QString("rgba(%1,%2,%3,%4)")
            .arg(col.red()).arg(col.green()).arg(col.blue())
            .arg(QString::number(col.alphaF(), 'f', 3));
    };
    const QColor popup = mixColors(t.surface, t.background, t.dark ? 0.55 : 0.35);
    const QColor buttonTop = t.controlTop();
    const QColor buttonBottom = t.controlBottom();
    const QColor buttonEdge = t.edgeDark(buttonBottom);
    // Lift saturated keys away from their ink so hover stays readable in custom themes.
    const QColor accentLift = t.accentText().lightnessF() < 0.5 ? QColor(Qt::white) : QColor(Qt::black);

    // Lighting stays inside the existing one-pixel borders and control bounds.
    return QString(R"(
QWidget { color: %TEXT%; font-size: 12px; }
QMainWindow, QDialog { background: %BG%; }
QMenuBar { background: %TOOLBAR%; border: none; }
QMenuBar::item { padding: 4px 10px; background: transparent; border-radius: %RADIUS%px; }
QMenuBar::item:selected { background: %ACCENT_SOFT%; }
/* Compact, opaque popups with a small, antialiased corner and inset selection. */
QMenu { background: %POPUP%; border: 1px solid %POPUP_BORDER%; border-radius: %RADIUS%px;
        padding: 4px; }
QMenu::item { min-height: 18px; padding: 1px 22px 1px 8px; border-radius: %RADIUS%px;
              font-size: 12px; }
QMenu::item:selected { background: %POPUP_HOVER%; color: %TEXT%; }
QMenu::item:checked { color: %ACCENT_HL%; font-weight: 600; }
QMenu::item:disabled { color: %TEXT2%; }
QMenu::icon { padding-left: 5px; }
QMenu::indicator { width: 12px; height: 12px; }
QMenu::right-arrow, QMenu::left-arrow { width: 12px; height: 12px; }
QMenu::separator { height: 1px; background: %POPUP_BORDER%; margin: 3px 6px; }
/* A menu long enough to scroll gets arrows at its ends; unstyled they are a
   grey Fusion strip that does not belong to any of this. */
QMenu::scroller { height: 14px; background: %POPUP%; }

QStatusBar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 %PANEL_TOP%, stop:1 %PANEL_BOTTOM%);
             border-top: 1px solid %SEP%; }
QStatusBar QLabel { color: %TEXT2%; font-size: 11px; }
QStatusBar QLabel#ProjectStatusText { padding-left: 8px; }
QStatusBar::item { border: none; }

QToolTip { background: %ELEV%; color: %TEXT%; border: 1px solid %SEP%;
           border-radius: %RADIUS%px; padding: 4px 6px; }

QScrollArea, QAbstractScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: %SCROLL%; border-radius: 4px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %SCROLL_HOVER%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %WELL_TOP%,stop:1 %WELL_BOTTOM%);
    border: 1px solid %SEP%; border-top-color: %WELL_EDGE%; border-bottom-color: %PANEL_LIGHT%; border-radius: %RADIUS%px;
    padding: 3px 7px; selection-background-color: %ACCENT%;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus {
    border: 1px solid %ACCENT%;
}
QCheckBox { spacing: 7px; min-height: 20px; }
QCheckBox:disabled { color: %TEXT2%; }
/* Compact closed controls; their lists use the shared popup surface below. */
QComboBox {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %WELL_TOP%,stop:1 %WELL_BOTTOM%);
    border: 1px solid %SEP%; border-top-color: %WELL_EDGE%; border-bottom-color: %PANEL_LIGHT%; border-radius: %RADIUS%px;
    padding: 3px 9px; min-height: 20px; color: %TEXT%; font-weight: 500;
    selection-background-color: %ACCENT%;
}
QComboBox:hover { border-color: %BUTTON_EDGE%; }
QComboBox:focus, QComboBox:on { border: 1px solid %ACCENT%; }
QComboBox:disabled { color: %TEXT2%; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(:/icons/popup-chevron-%APPEARANCE%.svg); width: 12px; height: 12px; }
/* Exactly the menu plate above — a combo's list and a menu are the same
   object with different contents. */
QComboBoxPrivateContainer { background: transparent; border: none; }
QComboBox QAbstractItemView {
    background: %POPUP%; border: 1px solid %POPUP_BORDER%; border-radius: %RADIUS%px;
    selection-background-color: %POPUP_HOVER%; selection-color: %TEXT%;
    outline: none; padding: 4px;
}
QComboBox QAbstractItemView::item { min-height: 18px; padding: 1px 7px;
                                    border-radius: %RADIUS%px; }
QComboBox QAbstractItemView::item:selected { background: %POPUP_HOVER%; color: %TEXT%; }

/* Console keys: a raised face, a lit upper edge and a recessed press.
   State changes keep the same border and padding, including keyboard focus. */
QPushButton {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %BUTTON_TOP%, stop:1 %BUTTON_BOTTOM%);
    color: %TEXT%; border: 1px solid %BUTTON_EDGE%; border-top-color: %BUTTON_LIGHT%;
    border-bottom-color: %BUTTON_SHADOW%; border-radius: 6px;
    padding: 5px 12px; font-weight: 500;
}
QPushButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %BUTTON_HOVER%, stop:1 %BUTTON_TOP%);
    border-color: %BUTTON_LIGHT%; border-bottom-color: %BUTTON_EDGE%;
}
QPushButton:pressed {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %WELL%, stop:1 %BUTTON_BOTTOM%);
    border-color: %BUTTON_SHADOW%; border-bottom-color: %BUTTON_EDGE%;
}
QPushButton:checked {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %BUTTON_ON_TOP%, stop:1 %BUTTON_ON_BOTTOM%);
    color: %TEXT%; border-color: %BUTTON_ON_EDGE%; border-bottom-color: %ACCENT%;
}
QPushButton:checked:hover { background: %BUTTON_ON_TOP%; border-color: %ACCENT%; }
QPushButton:checked:pressed { background: %BUTTON_ON_BOTTOM%; border-color: %BUTTON_ON_EDGE%; }
QPushButton:focus { border-color: %ACCENT%; }
QPushButton:default, QPushButton[accentAction="true"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %BUTTON_ACCENT_TOP%, stop:1 %ACCENT%);
    color: %ACCENT_TEXT%; border-color: %ACCENT%; border-top-color: %BUTTON_ACCENT_EDGE%;
    border-bottom-color: %ACCENT_DARK%; font-weight: 600;
}
QPushButton:default:hover, QPushButton[accentAction="true"]:hover {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 %BUTTON_ACCENT_EDGE%, stop:1 %BUTTON_ACCENT_TOP%);
}
QPushButton:default:pressed, QPushButton[accentAction="true"]:pressed {
    background: %ACCENT%; border-top-color: %ACCENT_DARK%; border-bottom-color: %BUTTON_ACCENT_EDGE%;
}
QPushButton:default:focus, QPushButton[accentAction="true"]:focus { border-color: %ACCENT_TEXT%; }
QPushButton:disabled, QPushButton:checked:disabled, QPushButton:default:disabled,
QPushButton[accentAction="true"]:disabled {
    background: %SURFACE%; color: %TEXT2%; border-color: %SEP%;
}

QToolButton { background: transparent; border: none; border-radius: %RADIUS%px; padding: 3px; font-weight: 500; }
QToolButton:hover { background: %HOVER%; }
QToolButton:checked { background: %ACCENT_SOFT%; }

QSplitter::handle { background: %SECTION%; }
QSplitter::handle:hover { background: %ACCENT_SOFT%; }

QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: %TOOLBAR%; padding: 5px 8px; border-bottom: 1px solid %SEP%; }

QGroupBox { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %PANEL_TOP%,stop:1 %SURFACE%);
            border: 1px solid %SEP%; border-top-color: %PANEL_LIGHT%; border-bottom-color: %PANEL_SHADOW%;
            border-radius: %RADIUS%px; margin-top: 14px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; color: %TEXT2%; font-weight: 600; }

QLabel[role="section"] { color: %TEXT2%; font-size: 10px; font-weight: 500; }
QLabel[role="pageTitle"] { font-size: 18px; font-weight: 600; }
QLabel[role="secondary"] { color: %TEXT2%; }

/* Settings navigation keeps text labels and native keyboard navigation;
   the edge marker also identifies the active page without relying on colour. */
QTreeWidget#SettingsNavigation {
    background: %WELL%; border: none; border-right: 1px solid %SEP%;
    padding: 4px; outline: none;
}
QTreeWidget#SettingsNavigation::item {
    padding: 0 6px; border: 1px solid transparent;
    border-left: 2px solid transparent; border-radius: %RADIUS%px;
}
QTreeWidget#SettingsNavigation::item:hover { background: %HOVER%; }
QTreeWidget#SettingsNavigation::item:selected {
    background: %ACCENT_SOFT%; color: %TEXT%; border-left-color: %ACCENT%;
}
QTreeWidget#SettingsNavigation::item:focus { border-color: %ACCENT%; }
QWidget#SettingsPageHeader {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %PANEL_TOP%,stop:1 %PANEL_BOTTOM%);
    border-bottom: 1px solid %SEP%; }
QLabel#SettingsPageIcon { background: %ACCENT_SOFT%; border-radius: %RADIUS%px; }
SettingsWindow QGroupBox {
    border-radius: %RADIUS%px; margin-top: 18px; padding-top: 8px;
}
SettingsWindow QGroupBox::title { color: %TEXT%; padding: 0 4px; }

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
)").replace("%RADIUS%", QString::number(Theme::cornerRadius))
        .replace("%TEXT%", c(t.textPrimary))
        .replace("%APPEARANCE%", t.dark ? QStringLiteral("dark") : QStringLiteral("light"))
        .replace("%TEXT2%", c(t.textSecondary))
        .replace("%BG%", c(t.background))
        .replace("%SURFACE%", c(t.surface))
        .replace("%BUTTON_TOP%", c(buttonTop))
        .replace("%BUTTON_BOTTOM%", c(buttonBottom))
        .replace("%BUTTON_EDGE%", c(buttonEdge))
        .replace("%BUTTON_LIGHT%", c(t.edgeLight(buttonTop)))
        .replace("%BUTTON_SHADOW%", c(t.edgeDark(buttonBottom)))
        .replace("%BUTTON_HOVER%", c(mixColors(buttonTop, t.dark ? Qt::white : Qt::black, 0.03)))
        .replace("%BUTTON_ON_TOP%", c(mixColors(buttonTop, t.accent, 0.08)))
        .replace("%BUTTON_ON_BOTTOM%", c(mixColors(t.well(), t.accent, 0.18)))
        .replace("%BUTTON_ON_EDGE%", c(mixColors(buttonEdge, t.accent, 0.60)))
        .replace("%BUTTON_ACCENT_TOP%", c(mixColors(t.accent, accentLift, 0.08)))
        .replace("%BUTTON_ACCENT_EDGE%", c(mixColors(t.accent, accentLift, 0.16)))
        .replace("%ELEV%", c(t.surfaceElevated))
        .replace("%ACCENT_TEXT%", c(t.accentText()))
        .replace("%TOOLBAR%", c(t.toolbarBackground))
        .replace("%PANEL_TOP%", c(t.panelTop()))
        .replace("%PANEL_BOTTOM%", c(t.panelBottom()))
        .replace("%PANEL_LIGHT%", c(t.edgeLight(t.panelTop())))
        .replace("%PANEL_SHADOW%", c(t.edgeDark(t.panelBottom())))
        .replace("%SEP%", c(t.separator()))
        .replace("%SECTION%", c(t.sectionDivider()))
        .replace("%WELL%", c(t.well()))
        .replace("%WELL_TOP%", c(t.wellTop()))
        .replace("%WELL_BOTTOM%", c(t.wellBottom()))
        .replace("%WELL_EDGE%", c(t.edgeDark(t.well())))
        .replace("%POPUP%", c(popup))
        .replace("%POPUP_BORDER%", c(mixColors(popup, t.textPrimary, 0.22)))
        .replace("%POPUP_HOVER%", c(mixColors(popup, t.textPrimary, t.dark ? 0.12 : 0.09)))
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
