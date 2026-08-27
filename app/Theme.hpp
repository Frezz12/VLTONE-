#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>
#include <QVector>

class QJsonObject;

/// The interface colour system. A `Theme` is a flat palette (surfaces, text,
/// accent, grid, transport chrome); `ThemeManager` owns the active one, hands
/// out a matching QPalette + global stylesheet, remembers the choice in
/// QSettings and notifies every widget when it changes.
///
/// Ported from the retired Swift design system so the Qt front-end keeps the
/// same look and the same preset names.
struct Theme {
    QString id;
    QString name;
    bool dark = true;

    QColor background;        // window / arrangement backdrop
    QColor surface;           // panels, track headers, strips
    QColor surfaceElevated;   // raised controls inside panels
    QColor textPrimary;
    QColor textSecondary;
    QColor accent;
    QColor accentHighlight;
    QColor waveform;
    QColor cursor;            // playhead
    QColor gridLine;
    QColor gridLineStrong;
    QColor selection;
    QColor transportBackground;
    QColor headerBackground;   // the transport/header bar's own colour
    QColor toolbarBackground;

    // Fixed, theme-independent signal colours (mute / solo / record), so the
    // meaning of a lit button never depends on the palette.
    static QColor mute()   { return QColor(0xE0, 0x4B, 0x4B); }
    static QColor solo()   { return QColor(0xF2, 0xC4, 0x3D); }
    static QColor record() { return QColor(0xE0, 0x3B, 0x3B); }

    // Context accents: the tint the context panel takes on for each kind of
    // object, so the colour itself says what is being edited. Fixed like the
    // signal colours above — audio is always blue whatever the palette.
    /// The cycle region on a ruler. Yellow because that is what every DAW
    /// paints it, and because nothing else in the chrome is: a lit cycle has
    /// to be recognisable in the corner of the eye while looking at a clip.
    static QColor cycle()  { return QColor(0xF2, 0xC4, 0x3D); }
    static QColor audioAccent()      { return QColor(0x3B, 0x82, 0xF6); }
    static QColor midiAccent()       { return QColor(0xA8, 0x55, 0xF7); }
    static QColor automationAccent() { return QColor(0xF9, 0x73, 0x16); }

    /// Surface tinted towards the background — used for wells / recessed areas.
    QColor well() const;
    /// A hairline that reads as a separator on this theme.
    QColor separator() const;
    /// A stronger boundary reserved for the application's major regions. It
    /// must read at a glance without making every control border louder.
    QColor sectionDivider() const;
    /// Ink for marks painted straight onto the workspace — the swipe band, a
    /// region overlay, the count-in digits. White on a dark palette, near-black
    /// on a light one. Marks painted on top of a *clip* keep using white: a
    /// clip body is a saturated colour whatever the palette is.
    QColor ink(int alpha = 255) const;
};

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    const Theme& theme() const { return m_theme; }
    const QVector<Theme>& presets() const { return m_presets; }

    /// Switch to a preset by id; no-op if unknown. Persists the choice unless
    /// `persist` is false (used by the `--theme` command-line override, which
    /// should not change what the user picked in the menu).
    void setThemeId(const QString& id, bool persist = true);
    QString themeId() const { return m_theme.id; }

    /// Make an arbitrary (user-authored) palette the active theme. Its id is
    /// forced to "custom"; the palette is persisted so it survives a restart.
    void applyCustomTheme(Theme theme, bool persist = true);

    /// JSON (de)serialisation for exporting / importing a theme file. `fromJson`
    /// fills any missing colour from `base` so partial files still load.
    static QJsonObject toJson(const Theme& theme);
    static Theme fromJson(const QJsonObject& obj, const Theme& base);

    /// Re-apply palette + stylesheet to the whole application.
    void apply();

    /// Copy a user-selected font into the application data folder and apply it
    /// immediately. Qt validates the actual font data; the source file can be
    /// moved or deleted afterwards.
    bool importFont(const QString& path, QString* error = nullptr);
    void resetFont();
    bool hasCustomFont() const { return m_fontId >= 0; }
    QString customFontFamily() const { return m_fontFamily; }
    QString customFontFileName() const { return m_fontFileName; }
    QString systemFontFamily() const { return m_systemFont.family(); }

    /// Filesystem-isolated import/reset check used by --selftest.
    bool checkFontForTest(QString* error = nullptr);

signals:
    void changed();
    void fontChanged();

private:
    ThemeManager();
    QString styleSheet() const;
    QString fontDataPath() const;
    void loadStoredFont();
    void applyFont();

    QVector<Theme> m_presets;
    Theme m_theme;
    QFont m_systemFont;
    int m_fontId = -1;
    QString m_fontFamily;
    QString m_fontFileName;
};

/// Shorthand for the active theme.
inline const Theme& th() { return ThemeManager::instance().theme(); }

/// 0xRRGGBB → QColor.
QColor colorFromRgb(quint32 rgb);

/// QColor blended towards `other` by `t` (0…1).
QColor mixColors(const QColor& a, const QColor& b, double t);
