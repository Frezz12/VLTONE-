#include "LocalizationManager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QTranslator>
#include <QXmlStreamReader>

#include <algorithm>
#include <utility>

namespace ui {

namespace {

constexpr qint64 kMaximumPackBytes = 5 * 1024 * 1024;
constexpr auto kPackFormat = "vlt-language-pack";
constexpr int kSchemaVersion = 1;

using TranslationMap = QHash<QString, QHash<QString, QString>>;

struct Catalog {
    QHash<QString, QStringList> contexts;
    int size = 0;
};

Catalog readCatalog() {
    Catalog catalog;
    QFile file(QStringLiteral(":/i18n/vlt_ru.ts"));
    if (!file.open(QIODevice::ReadOnly)) return catalog;

    QXmlStreamReader xml(&file);
    QString context;
    bool obsolete = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QLatin1String("context")) {
            context.clear();
        } else if (xml.name() == QLatin1String("name")) {
            context = xml.readElementText();
        } else if (xml.name() == QLatin1String("message")) {
            obsolete = xml.attributes().value(QStringLiteral("type")) ==
                       QLatin1String("obsolete");
        } else if (xml.name() == QLatin1String("source") && !obsolete &&
                   !context.isEmpty()) {
            const QString source = xml.readElementText(
                QXmlStreamReader::IncludeChildElements);
            if (!source.isEmpty() &&
                !catalog.contexts[context].contains(source)) {
                catalog.contexts[context].push_back(source);
                ++catalog.size;
            }
        }
    }
    for (auto it = catalog.contexts.begin(); it != catalog.contexts.end(); ++it)
        std::sort(it.value().begin(), it.value().end());
    return catalog;
}

const Catalog& applicationCatalog() {
    static const Catalog catalog = readCatalog();
    return catalog;
}

QStringList placeholders(const QString& text) {
    static const QRegularExpression expression(QStringLiteral("%[1-9]"));
    QStringList found;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) found.push_back(matches.next().captured());
    std::sort(found.begin(), found.end());
    return found;
}

bool validLanguageName(const QString& value) {
    const int length = value.trimmed().size();
    return length > 0 && length <= 80;
}

QString customBaseDirectory() {
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        base = QDir(QStandardPaths::writableLocation(
                        QStandardPaths::DocumentsLocation))
                   .filePath(QStringLiteral("VLT Studio Pro"));
    }
    return base;
}

} // namespace

class LocalizationManager::JsonTranslator final : public QTranslator {
public:
    explicit JsonTranslator(TranslationMap translations, QObject* parent = nullptr)
        : QTranslator(parent), m_translations(std::move(translations)) {}

    bool isEmpty() const override { return m_translations.isEmpty(); }

    QString lookupForTest(const char* context, const char* sourceText) const {
        return translate(context, sourceText, nullptr, -1);
    }

    QString translate(const char* context, const char* sourceText,
                      const char*, int) const override {
        if (!context || !sourceText) return {};
        const auto contextIt = m_translations.constFind(QString::fromUtf8(context));
        if (contextIt == m_translations.cend()) return {};
        const auto textIt = contextIt->constFind(QString::fromUtf8(sourceText));
        return textIt == contextIt->cend() || textIt->isEmpty() ? QString()
                                                                : *textIt;
    }

private:
    TranslationMap m_translations;
};

struct LocalizationManager::ParsedPack {
    LanguagePackResult result;
    TranslationMap translations;
    QString author;
};

LocalizationManager& LocalizationManager::instance() {
    static LocalizationManager manager;
    return manager;
}

LocalizationManager::LocalizationManager() = default;

LocalizationManager::~LocalizationManager() {
    if (m_jsonTranslator) QCoreApplication::removeTranslator(m_jsonTranslator.get());
    if (m_appTranslator) QCoreApplication::removeTranslator(m_appTranslator.get());
    if (m_qtTranslator) QCoreApplication::removeTranslator(m_qtTranslator.get());
}

QString LocalizationManager::canonicalLocale(const QString& locale) {
    const QString cleaned = QString(locale).trimmed().replace(QLatin1Char('_'),
                                                               QLatin1Char('-'));
    if (cleaned.isEmpty()) return {};
    static const QRegularExpression languageTag(QStringLiteral(
        "^[A-Za-z]{2,3}(?:-[A-Za-z]{4})?(?:-(?:[A-Za-z]{2}|[0-9]{3}))?"
        "(?:-[A-Za-z0-9]{5,8})*$"));
    if (!languageTag.match(cleaned).hasMatch()) return {};
    const QLocale parsed(cleaned);
    if (parsed.language() == QLocale::C) return {};
    return parsed.bcp47Name();
}

QString LocalizationManager::packFileName(const QString& locale) {
    return canonicalLocale(locale) + QStringLiteral(".vltlang.json");
}

bool LocalizationManager::isBuiltIn(const QString& locale) const {
    const QString canonical = canonicalLocale(locale);
    if (canonical.isEmpty()) return false;
    const QLocale parsed(canonical);
    return parsed.language() == QLocale::English ||
           parsed.language() == QLocale::Russian;
}

QString LocalizationManager::languageDirectory() const {
    return QDir(customBaseDirectory()).filePath(QStringLiteral("Languages"));
}

QString LocalizationManager::preferredLocale() const {
    const QString stored = QSettings().value(QStringLiteral("ui/language"),
                                              QStringLiteral("ru")).toString();
    const QString canonical = canonicalLocale(stored);
    return canonical.isEmpty() ? QStringLiteral("ru") : canonical;
}

QString LocalizationManager::websiteLocale() const {
    return QLocale(m_activeLocale).language() == QLocale::Russian
               ? QStringLiteral("ru")
               : QStringLiteral("en");
}

LocalizationManager::ParsedPack LocalizationManager::parsePack(
    const QString& path) const {
    ParsedPack parsed;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        parsed.result.error = tr("Could not open %1.").arg(path);
        return parsed;
    }
    if (file.size() > kMaximumPackBytes) {
        parsed.result.error = tr("The language pack is larger than 5 MiB.");
        return parsed;
    }

    const QByteArray bytes = file.readAll();
    QStringDecoder utf8(QStringDecoder::Utf8);
    utf8.decode(bytes);
    if (utf8.hasError()) {
        parsed.result.error = tr("The file is not valid UTF-8 JSON.");
        return parsed;
    }

    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
        parsed.result.error = tr("The file is not valid UTF-8 JSON: %1")
                                  .arg(jsonError.errorString());
        return parsed;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() !=
        QLatin1String(kPackFormat)) {
        parsed.result.error = tr("This is not a VLT Studio Pro language pack.");
        return parsed;
    }
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != kSchemaVersion) {
        parsed.result.error = tr("Unsupported language-pack schema version.");
        return parsed;
    }

    parsed.result.locale = canonicalLocale(
        root.value(QStringLiteral("locale")).toString());
    if (parsed.result.locale.isEmpty()) {
        parsed.result.error = tr("The pack has an invalid BCP-47 locale.");
        return parsed;
    }
    parsed.result.languageName =
        root.value(QStringLiteral("languageName")).toString().trimmed();
    if (!validLanguageName(parsed.result.languageName)) {
        parsed.result.error = tr("Language name must contain 1 to 80 characters.");
        return parsed;
    }
    parsed.author = root.value(QStringLiteral("author")).toString().trimmed();
    if (parsed.author.size() > 120) {
        parsed.result.error = tr("Author name must not exceed 120 characters.");
        return parsed;
    }

    const QJsonValue translationsValue = root.value(QStringLiteral("translations"));
    if (!translationsValue.isObject()) {
        parsed.result.error = tr("The pack has no translations object.");
        return parsed;
    }

    const Catalog& catalog = applicationCatalog();
    const QJsonObject contexts = translationsValue.toObject();
    for (auto contextIt = contexts.begin(); contextIt != contexts.end();
         ++contextIt) {
        if (!contextIt.value().isObject()) {
            parsed.result.error =
                tr("Translation context \"%1\" must be a JSON object.")
                    .arg(contextIt.key());
            return parsed;
        }
        const QJsonObject messages = contextIt.value().toObject();
        for (auto messageIt = messages.begin(); messageIt != messages.end();
             ++messageIt) {
            if (!messageIt.value().isString()) {
                parsed.result.error =
                    tr("Translation for \"%1\" must be a string.")
                        .arg(messageIt.key());
                return parsed;
            }
            const QString translated = messageIt.value().toString();
            const bool known = catalog.contexts.value(contextIt.key())
                                   .contains(messageIt.key());
            if (!known) {
                ++parsed.result.unknown;
                continue;
            }
            if (!translated.isEmpty() &&
                placeholders(translated) != placeholders(messageIt.key())) {
                parsed.result.error = tr(
                    "Translation for \"%1\" must preserve every numbered "
                    "placeholder from the English source.").arg(messageIt.key());
                return parsed;
            }
            if (!translated.isEmpty()) {
                parsed.translations[contextIt.key()].insert(messageIt.key(),
                                                             translated);
                ++parsed.result.translated;
            }
        }
    }
    parsed.result.missing = std::max(0, catalog.size - parsed.result.translated);
    parsed.result.ok = true;
    return parsed;
}

LanguageInfo LocalizationManager::languageInfoForLocale(
    const QString& locale) const {
    const QString canonical = canonicalLocale(locale);
    if (QLocale(canonical).language() == QLocale::English) {
        return {QStringLiteral("en"), QStringLiteral("English"), {}, {}, true};
    }
    if (QLocale(canonical).language() == QLocale::Russian) {
        return {QStringLiteral("ru"), QString::fromUtf8("Русский"), {}, {}, true};
    }
    for (const LanguageInfo& language : languages()) {
        if (language.locale == canonical) return language;
    }
    return {};
}

QList<LanguageInfo> LocalizationManager::languages() const {
    QList<LanguageInfo> result = {
        {QStringLiteral("ru"), QString::fromUtf8("Русский"), {}, {}, true},
        {QStringLiteral("en"), QStringLiteral("English"), {}, {}, true},
    };

    const QDir directory(languageDirectory());
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.vltlang.json")}, QDir::Files | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& file : files) {
        const ParsedPack parsed = parsePack(file.absoluteFilePath());
        if (!parsed.result.ok || isBuiltIn(parsed.result.locale)) continue;
        result.push_back({parsed.result.locale, parsed.result.languageName,
                          parsed.author, file.absoluteFilePath(), false});
    }
    std::sort(result.begin() + 2, result.end(),
              [](const LanguageInfo& a, const LanguageInfo& b) {
                  return a.languageName.localeAwareCompare(b.languageName) < 0;
              });
    return result;
}

bool LocalizationManager::install(const LanguageInfo& language,
                                  QString* error) {
    if (language.locale.isEmpty()) {
        if (error) *error = tr("The selected language is not installed.");
        return false;
    }

    const QLocale locale(language.locale);
    auto nextQt = std::make_unique<QTranslator>();
    if (!nextQt->load(locale, QStringLiteral("qt"), QStringLiteral("_"),
                      QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        nextQt.reset();

    std::unique_ptr<QTranslator> nextApp;
    std::unique_ptr<JsonTranslator> nextJson;

    if (language.locale == QLatin1String("ru")) {
        nextApp = std::make_unique<QTranslator>();
        if (!nextApp->load(QStringLiteral(":/i18n/vlt_ru.qm"))) {
            if (error) *error = tr("The bundled Russian translation could not be loaded.");
            return false;
        }
    } else if (!language.builtIn) {
        const ParsedPack parsed = parsePack(language.filePath);
        if (!parsed.result.ok) {
            if (error) *error = parsed.result.error;
            return false;
        }
        nextJson = std::make_unique<JsonTranslator>(parsed.translations);
    }

    if (m_jsonTranslator)
        QCoreApplication::removeTranslator(m_jsonTranslator.get());
    if (m_appTranslator)
        QCoreApplication::removeTranslator(m_appTranslator.get());
    if (m_qtTranslator)
        QCoreApplication::removeTranslator(m_qtTranslator.get());

    m_qtTranslator = std::move(nextQt);
    m_appTranslator = std::move(nextApp);
    m_jsonTranslator = std::move(nextJson);
    QLocale::setDefault(locale);
    if (m_qtTranslator) QCoreApplication::installTranslator(m_qtTranslator.get());
    if (m_appTranslator) QCoreApplication::installTranslator(m_appTranslator.get());
    if (m_jsonTranslator)
        QCoreApplication::installTranslator(m_jsonTranslator.get());

    m_activeLocale = language.locale;
    emit languageChanged(m_activeLocale);
    return true;
}

void LocalizationManager::initialize() {
    if (!m_activeLocale.isEmpty()) return;
    const QString requested = preferredLocale();
    QString error;
    if (install(languageInfoForLocale(requested), &error)) return;

    m_startupWarning = error.isEmpty()
                           ? tr("The selected language is unavailable. English is being used.")
                           : error + QLatin1Char('\n') +
                                 tr("English is being used instead.");
    QSettings().setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
    install(languageInfoForLocale(QStringLiteral("en")), nullptr);
}

bool LocalizationManager::activateLanguage(const QString& locale, bool persist,
                                            QString* error) {
    const LanguageInfo language = languageInfoForLocale(locale);
    if (!install(language, error)) return false;
    if (persist)
        QSettings().setValue(QStringLiteral("ui/language"), language.locale);
    return true;
}

bool LocalizationManager::setPreferredLocale(const QString& locale,
                                             QString* error) {
    const LanguageInfo language = languageInfoForLocale(locale);
    if (language.locale.isEmpty()) {
        if (error) *error = tr("The selected language is not installed.");
        return false;
    }
    QSettings().setValue(QStringLiteral("ui/language"), language.locale);
    return true;
}

LanguagePackResult LocalizationManager::importLanguagePack(
    const QString& sourcePath, bool replaceExisting) {
    ParsedPack parsed = parsePack(sourcePath);
    if (!parsed.result.ok) return parsed.result;
    if (isBuiltIn(parsed.result.locale)) {
        parsed.result.ok = false;
        parsed.result.error = tr("English and Russian are built in and cannot be replaced.");
        return parsed.result;
    }

    QDir directory(languageDirectory());
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        parsed.result.ok = false;
        parsed.result.error = tr("Could not create the Languages folder.");
        return parsed.result;
    }
    const QString destination =
        directory.filePath(packFileName(parsed.result.locale));
    if (QFileInfo::exists(destination) && !replaceExisting) {
        parsed.result.ok = false;
        parsed.result.alreadyExists = true;
        parsed.result.error = tr("A language pack for %1 is already installed.")
                                  .arg(parsed.result.locale);
        return parsed.result;
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        parsed.result.ok = false;
        parsed.result.error = tr("Could not read %1.").arg(sourcePath);
        return parsed.result;
    }
    QSaveFile target(destination);
    if (!target.open(QIODevice::WriteOnly) ||
        target.write(source.readAll()) < 0 || !target.commit()) {
        parsed.result.ok = false;
        parsed.result.error = tr("Could not install the language pack.");
        return parsed.result;
    }
    parsed.result.ok = true;
    parsed.result.alreadyExists = false;
    emit languagesChanged();
    return parsed.result;
}

bool LocalizationManager::exportTemplate(const QString& destinationPath,
                                         QString* error) const {
    const Catalog& catalog = applicationCatalog();
    if (catalog.size == 0) {
        if (error) *error = tr("The application translation catalog is unavailable.");
        return false;
    }

    QJsonObject translations;
    QStringList contextNames = catalog.contexts.keys();
    std::sort(contextNames.begin(), contextNames.end());
    for (const QString& context : contextNames) {
        QJsonObject messages;
        for (const QString& source : catalog.contexts.value(context))
            messages.insert(source, QString());
        translations.insert(context, messages);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(kPackFormat));
    root.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    root.insert(QStringLiteral("locale"), QStringLiteral("de-DE"));
    root.insert(QStringLiteral("languageName"), QStringLiteral("Deutsch"));
    root.insert(QStringLiteral("author"), QString());
    root.insert(QStringLiteral("appVersion"),
                QCoreApplication::applicationVersion());
    root.insert(QStringLiteral("translations"), translations);

    QSaveFile file(destinationPath);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (error) *error = tr("Could not write %1.").arg(destinationPath);
        return false;
    }
    return true;
}

bool LocalizationManager::removeLanguagePack(const QString& locale,
                                             QString* error) {
    const QString canonical = canonicalLocale(locale);
    if (canonical.isEmpty() || isBuiltIn(canonical)) {
        if (error) *error = tr("Built-in languages cannot be removed.");
        return false;
    }
    const LanguageInfo language = languageInfoForLocale(canonical);
    if (language.filePath.isEmpty() || !QFile::remove(language.filePath)) {
        if (error) *error = tr("Could not remove the language pack.");
        return false;
    }
    if (preferredLocale() == canonical)
        QSettings().setValue(QStringLiteral("ui/language"), QStringLiteral("en"));
    emit languagesChanged();
    return true;
}

QString LocalizationManager::takeStartupWarning() {
    return std::exchange(m_startupWarning, QString());
}

bool LocalizationManager::checkJsonPackForTest(QString* error) const {
    QTemporaryDir directory;
    if (!directory.isValid()) {
        if (error) *error = QStringLiteral("temporary directory unavailable");
        return false;
    }
    const QString path = directory.filePath(QStringLiteral("test.vltlang.json"));
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    const auto writeBytes = [&path](const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(bytes) == bytes.size();
    };
    const auto writeObject = [&writeBytes](const QJsonObject& object) {
        return writeBytes(QJsonDocument(object).toJson());
    };

    QJsonObject translations{
        {QStringLiteral("ui::LocalizationManager"),
         QJsonObject{{QStringLiteral("Could not open %1."),
                      QStringLiteral("TEST OPEN %1")},
                     {QStringLiteral("legacy key"),
                      QStringLiteral("ignored")}}},
        {QStringLiteral("StartupWindow"),
         QJsonObject{{QStringLiteral("Ready"), QStringLiteral("TEST READY")}}},
    };
    QJsonObject root{
        {QStringLiteral("format"), QLatin1String(kPackFormat)},
        {QStringLiteral("schemaVersion"), kSchemaVersion},
        {QStringLiteral("locale"), QStringLiteral("de-DE")},
        {QStringLiteral("languageName"), QStringLiteral("Deutsch")},
        {QStringLiteral("author"), QStringLiteral("Test")},
        {QStringLiteral("translations"), translations},
    };
    if (!writeObject(root)) return fail(QStringLiteral("could not write test pack"));

    const ParsedPack valid = parsePack(path);
    if (!valid.result.ok ||
        QLocale(valid.result.locale).language() != QLocale::German ||
        valid.result.translated != 2 || valid.result.unknown != 1 ||
        valid.result.missing != applicationCatalog().size - 2)
        return fail(valid.result.error.isEmpty()
                        ? QStringLiteral("valid partial pack was rejected")
                        : valid.result.error);

    JsonTranslator translator(valid.translations);
    if (translator.lookupForTest("ui::LocalizationManager", "Could not open %1.") !=
            QLatin1String("TEST OPEN %1") ||
        translator.lookupForTest("StartupWindow", "Ready") !=
            QLatin1String("TEST READY") ||
        !translator.lookupForTest("WrongContext", "Ready").isEmpty() ||
        !translator.lookupForTest("StartupWindow", "Missing").isEmpty())
        return fail(QStringLiteral("JSON translator context or fallback failed"));

    QJsonObject completeTranslations;
    const Catalog& catalog = applicationCatalog();
    for (auto contextIt = catalog.contexts.cbegin();
         contextIt != catalog.contexts.cend(); ++contextIt) {
        QJsonObject messages;
        for (const QString& source : contextIt.value())
            messages.insert(source, source);
        completeTranslations.insert(contextIt.key(), messages);
    }
    QJsonObject complete = root;
    complete[QStringLiteral("translations")] = completeTranslations;
    if (!writeObject(complete))
        return fail(QStringLiteral("could not write complete test pack"));
    const ParsedPack full = parsePack(path);
    if (!full.result.ok || full.result.translated != catalog.size ||
        full.result.missing != 0)
        return fail(QStringLiteral("complete pack was not recognized"));

    QJsonObject broken = root;
    broken[QStringLiteral("translations")] =
        QJsonObject{{QStringLiteral("ui::LocalizationManager"),
                     QJsonObject{{QStringLiteral("Could not open %1."),
                                  QStringLiteral("TEST OPEN")}}}};
    if (!writeObject(broken) || parsePack(path).result.ok)
        return fail(QStringLiteral("placeholder mismatch was accepted"));

    broken = root;
    broken[QStringLiteral("locale")] = QStringLiteral("not_a_locale");
    if (!writeObject(broken) || parsePack(path).result.ok)
        return fail(QStringLiteral("invalid locale was accepted"));

    broken = root;
    broken[QStringLiteral("schemaVersion")] = kSchemaVersion + 1;
    if (!writeObject(broken) || parsePack(path).result.ok)
        return fail(QStringLiteral("invalid schema version was accepted"));

    broken = root;
    broken[QStringLiteral("translations")] =
        QJsonObject{{QStringLiteral("StartupWindow"), 42}};
    if (!writeObject(broken) || parsePack(path).result.ok)
        return fail(QStringLiteral("invalid translation structure was accepted"));

    if (!writeBytes(QByteArrayLiteral("{")) || parsePack(path).result.ok)
        return fail(QStringLiteral("damaged JSON was accepted"));
    QByteArray invalidUtf8 = QByteArrayLiteral(
        "{\"format\":\"vlt-language-pack\",\"schemaVersion\":1,"
        "\"locale\":\"de-DE\",\"languageName\":\"");
    invalidUtf8.append(char(0xff));
    invalidUtf8.append(QByteArrayLiteral("\",\"translations\":{}}"));
    if (!writeBytes(invalidUtf8) || parsePack(path).result.ok)
        return fail(QStringLiteral("invalid UTF-8 was accepted"));

    const QString replacementPath =
        directory.filePath(QStringLiteral("replacement.json"));
    for (const QByteArray& contents :
         {QByteArrayLiteral("first"), QByteArrayLiteral("second")}) {
        QSaveFile replacement(replacementPath);
        if (!replacement.open(QIODevice::WriteOnly) ||
            replacement.write(contents) != contents.size() ||
            !replacement.commit())
            return fail(QStringLiteral("atomic replacement failed"));
    }
    QFile replaced(replacementPath);
    if (!replaced.open(QIODevice::ReadOnly) ||
        replaced.readAll() != QByteArrayLiteral("second"))
        return fail(QStringLiteral("replacement did not preserve the new pack"));
    return true;
}

} // namespace ui
