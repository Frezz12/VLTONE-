#pragma once

#include <QObject>
#include <QList>
#include <QString>

#include <memory>

class QTranslator;

namespace ui {

struct LanguageInfo {
    QString locale;
    QString languageName;
    QString author;
    QString filePath;
    bool builtIn = false;
};

struct LanguagePackResult {
    bool ok = false;
    bool alreadyExists = false;
    QString error;
    QString locale;
    QString languageName;
    int translated = 0;
    int missing = 0;
    int unknown = 0;
};

/// Application-wide translations and the editable JSON language-pack boundary.
///
/// English is the source language, Russian is the bundled Qt Linguist catalog,
/// and imported packs are ordinary JSON files loaded through a small
/// QTranslator implementation. The manager is initialized once, before the
/// first widget is constructed.
class LocalizationManager final : public QObject {
    Q_OBJECT
public:
    static LocalizationManager& instance();

    /// Load the persisted language. Call after QApplication metadata and the
    /// headless QSettings sandbox have been configured, before creating UI.
    void initialize();

    QString activeLocale() const { return m_activeLocale; }
    QString preferredLocale() const;
    QString websiteLocale() const;
    QList<LanguageInfo> languages() const;

    /// Install a language immediately. Used by the startup window, where no
    /// application workspace exists yet and LanguageChange can be handled in
    /// one place. Optionally persists the choice for the next launch.
    bool activateLanguage(const QString& locale, bool persist,
                          QString* error = nullptr);

    /// Store a choice for the next launch without producing a half-translated
    /// live workspace.
    bool setPreferredLocale(const QString& locale, QString* error = nullptr);

    LanguagePackResult importLanguagePack(const QString& sourcePath,
                                          bool replaceExisting = false);
    bool exportTemplate(const QString& destinationPath,
                        QString* error = nullptr) const;
    bool removeLanguagePack(const QString& locale, QString* error = nullptr);

    bool isBuiltIn(const QString& locale) const;
    QString languageDirectory() const;
    QString takeStartupWarning();

    /// Small, filesystem-isolated check used by --selftest.
    bool checkJsonPackForTest(QString* error = nullptr) const;

signals:
    void languageChanged(const QString& locale);
    void languagesChanged();

private:
    LocalizationManager();
    ~LocalizationManager() override;
    Q_DISABLE_COPY_MOVE(LocalizationManager)

    class JsonTranslator;
    struct ParsedPack;

    ParsedPack parsePack(const QString& path) const;
    LanguageInfo languageInfoForLocale(const QString& locale) const;
    bool install(const LanguageInfo& language, QString* error);
    static QString canonicalLocale(const QString& locale);
    static QString packFileName(const QString& locale);

    std::unique_ptr<QTranslator> m_qtTranslator;
    std::unique_ptr<QTranslator> m_appTranslator;
    std::unique_ptr<JsonTranslator> m_jsonTranslator;
    QString m_activeLocale;
    QString m_startupWarning;
};

} // namespace ui
