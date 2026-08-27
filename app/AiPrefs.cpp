#include "AiPrefs.hpp"
#include "SecureStorage.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>


#include <algorithm>
#include <utility>

namespace ui::aiprefs {

namespace {

QString key(const char* name) {
    return QStringLiteral("ai/") + QLatin1String(name);
}

QString secretSlot(const QString& id) {
    return QStringLiteral("ai-model-") + id;
}

QJsonObject modelJson(const ModelConnection& model) {
    return {
        {QStringLiteral("id"), model.id},
        {QStringLiteral("display_name"), model.displayName},
        {QStringLiteral("provider"), providerId(model.provider)},
        {QStringLiteral("model"), model.model},
        {QStringLiteral("endpoint"), model.endpoint},
        {QStringLiteral("has_api_key"), model.hasApiKey},
    };
}

ModelConnection modelFromJson(const QJsonObject& value, ModelSource source) {
    ModelConnection model;
    model.id = value.value(QStringLiteral("id")).toString();
    model.displayName = value.value(QStringLiteral("display_name")).toString();
    model.provider = providerFromId(
        value.value(QStringLiteral("provider")).toString());
    model.model = value.value(QStringLiteral("model")).toString();
    model.endpoint = value.value(QStringLiteral("endpoint")).toString();
    model.source = source;
    model.hasApiKey = value.value(QStringLiteral("has_api_key")).toBool();
    return model;
}

QList<ModelConnection> readModels(const char* setting, ModelSource source) {
    const QByteArray raw = QSettings().value(key(setting)).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    QList<ModelConnection> models;
    if (!document.isArray()) return models;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) continue;
        ModelConnection model = modelFromJson(value.toObject(), source);
        if (model.id.isEmpty() || model.displayName.trimmed().isEmpty() ||
            model.model.trimmed().isEmpty())
            continue;
        if (source == ModelSource::Custom && model.endpoint.trimmed().isEmpty())
            continue;
        models.append(std::move(model));
    }
    return models;
}

void writeModels(const char* setting, const QList<ModelConnection>& models) {
    QJsonArray array;
    for (const ModelConnection& model : models) array.append(modelJson(model));
    QSettings().setValue(key(setting),
                         QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace

QString providerId(Provider provider) {
    return provider == Provider::OpenAi ? QStringLiteral("openai")
                                        : QStringLiteral("anthropic");
}

Provider providerFromId(const QString& id) {
    return id == QLatin1String("openai") ? Provider::OpenAi : Provider::Anthropic;
}

QList<ModelConnection> managedModels() {
    return readModels("managedModels", ModelSource::Managed);
}

void setManagedModels(const QList<ModelConnection>& value) {
    QList<ModelConnection> cleaned;
    for (ModelConnection model : value) {
        model.id = model.id.trimmed();
        model.displayName = model.displayName.trimmed();
        model.model = model.model.trimmed();
        model.endpoint.clear();
        model.source = ModelSource::Managed;
        model.hasApiKey = true;
        if (model.id.isEmpty() || model.displayName.isEmpty() ||
            model.model.isEmpty())
            continue;
        cleaned.append(std::move(model));
    }
    writeModels("managedModels", cleaned);
    const QString active = activeModelId();
    ModelConnection found;
    if (!active.isEmpty() && !modelById(active, &found)) {
        const QList<ModelConnection> available = availableModels();
        setActiveModelId(available.isEmpty() ? QString() : available.front().id);
    }
}

QList<ModelConnection> customModels() {
    return readModels("customModels", ModelSource::Custom);
}

QList<ModelConnection> availableModels() {
    QList<ModelConnection> result = managedModels();
    result.append(customModels());
    return result;
}

bool modelById(const QString& id, ModelConnection* output) {
    for (const ModelConnection& model : availableModels()) {
        if (model.id != id) continue;
        if (output) *output = model;
        return true;
    }
    return false;
}

bool saveCustomModel(ModelConnection model, const QString& apiKey,
                     QString* savedId) {
    QList<ModelConnection> stored = customModels();
    int index = -1;
    for (int i = 0; i < stored.size(); ++i)
        if (stored.at(i).id == model.id) index = i;
    if (model.id.isEmpty())
        model.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    model.displayName = model.displayName.trimmed();
    model.model = model.model.trimmed();
    model.endpoint = model.endpoint.trimmed();
    model.source = ModelSource::Custom;
    model.hasApiKey = index >= 0 && stored.at(index).hasApiKey;
    const QString trimmedKey = apiKey.trimmed();
    if (!trimmedKey.isEmpty()) {
        if (!account::securestorage::writeNamed(secretSlot(model.id),
                                                trimmedKey.toUtf8()))
            return false;
        model.hasApiKey = true;
    }
    if (index >= 0) stored[index] = model;
    else stored.append(model);
    writeModels("customModels", stored);
    if (savedId) *savedId = model.id;
    if (activeModelId().isEmpty()) setActiveModelId(model.id);
    return true;
}

bool removeCustomModel(const QString& id) {
    QList<ModelConnection> stored = customModels();
    bool removed = false;
    for (int i = stored.size() - 1; i >= 0; --i) {
        if (stored.at(i).id != id) continue;
        stored.removeAt(i);
        removed = true;
    }
    if (!removed) return false;
    if (!account::securestorage::clearNamed(secretSlot(id))) return false;
    writeModels("customModels", stored);
    if (activeModelId() == id) {
        const QList<ModelConnection> available = availableModels();
        setActiveModelId(available.isEmpty() ? QString() : available.front().id);
    }
    return true;
}

QString customApiKey(const QString& id) {
    return QString::fromUtf8(account::securestorage::readNamed(secretSlot(id)));
}

QString activeModelId() {
    return QSettings().value(key("activeModelId")).toString();
}

void setActiveModelId(const QString& id) {
    QSettings settings;
    if (id.isEmpty()) settings.remove(key("activeModelId"));
    else settings.setValue(key("activeModelId"), id);
}

Mode mode() {
    // Music generation still has scripted coverage, but no shipping provider
    // proxy in v1. Never restore the former direct-provider mode.
    QSettings().remove(key("mode"));
    return Mode::Assistant;
}

void setMode(Mode mode) {
    Q_UNUSED(mode);
    QSettings().remove(key("mode"));
}

QString musicUrl() {
    return QSettings()
        .value(key("music.url"),
               QStringLiteral("https://api.minimax.io/v1/music_generation"))
        .toString();
}

void setMusicUrl(const QString& url) {
    QSettings().setValue(key("music.url"), url);
}

QString musicModel() {
    return QSettings().value(key("music.model"), QStringLiteral("music-3.0"))
        .toString();
}

void setMusicModel(const QString& model) {
    QSettings().setValue(key("music.model"), model);
}

bool hasMusicApiKey() {
    return !QSettings().value(key("music.apiKey")).toString().isEmpty();
}

QString musicApiKey() {
    return QSettings().value(key("music.apiKey")).toString();
}

void setMusicApiKey(const QString& value) {
    QSettings settings;
    if (value.isEmpty()) settings.remove(key("music.apiKey"));
    else settings.setValue(key("music.apiKey"), value);
}

QString musicFormat() {
    const QString stored =
        QSettings().value(key("music.format"), QStringLiteral("mp3")).toString();
    return stored == QLatin1String("wav") ? stored : QStringLiteral("mp3");
}

void setMusicFormat(const QString& format) {
    QSettings().setValue(key("music.format"), format);
}

int musicSampleRate() {
    return std::clamp(QSettings().value(key("music.sampleRate"), 44100).toInt(),
                      8000, 192000);
}

void setMusicSampleRate(int rate) {
    QSettings().setValue(key("music.sampleRate"), std::clamp(rate, 8000, 192000));
}

int musicBitrate() {
    return std::clamp(QSettings().value(key("music.bitrate"), 256000).toInt(),
                      32000, 320000);
}

void setMusicBitrate(int bitrate) {
    QSettings().setValue(key("music.bitrate"), std::clamp(bitrate, 32000, 320000));
}

QString musicFolder() {
    const QString stored = QSettings().value(key("music.folder")).toString();
    if (!stored.isEmpty()) return stored;
    // Inside Music rather than a hidden application folder: the file browser
    // starts there too, so a generated take is one click away from being used
    // again somewhere else.
    const QString music =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    const QString base = music.isEmpty()
                             ? QStandardPaths::writableLocation(
                                   QStandardPaths::AppLocalDataLocation)
                             : music;
    return base + QStringLiteral("/VLT Studio Pro Generated");
}

void setMusicFolder(const QString& folder) {
    QSettings().setValue(key("music.folder"), folder);
}

int musicTimeoutSeconds() {
    return std::clamp(QSettings().value(key("music.timeout"), 300).toInt(), 10,
                      3600);
}

void setMusicTimeoutSeconds(int seconds) {
    QSettings().setValue(key("music.timeout"), std::clamp(seconds, 10, 3600));
}

bool musicInstrumental() {
    return QSettings().value(key("music.instrumental"), false).toBool();
}

void setMusicInstrumental(bool instrumental) {
    QSettings().setValue(key("music.instrumental"), instrumental);
}

int maxIterations() {
    return std::clamp(QSettings().value(key("maxIterations"), 24).toInt(), 1, 200);
}

void setMaxIterations(int iterations) {
    QSettings().setValue(key("maxIterations"), std::clamp(iterations, 1, 200));
}

QStringList savedPrompts() {
    return QSettings().value(key("savedPrompts")).toStringList();
}

void setSavedPrompts(const QStringList& prompts) {
    QSettings().setValue(key("savedPrompts"), prompts);
}

int historyLimit() {
    return std::clamp(QSettings().value(key("historyLimit"), 12).toInt(), 0, 200);
}

void setHistoryLimit(int turns) {
    QSettings().setValue(key("historyLimit"), std::clamp(turns, 0, 200));
}

bool streaming() { return QSettings().value(key("streaming"), true).toBool(); }

void setStreaming(bool on) { QSettings().setValue(key("streaming"), on); }

bool visible() { return QSettings().value(key("visible"), false).toBool(); }

void setVisible(bool visible) { QSettings().setValue(key("visible"), visible); }

int width() {
    return std::clamp(QSettings().value(key("width"), 320).toInt(), 240, 720);
}

void setWidth(int width) {
    QSettings().setValue(key("width"), std::clamp(width, 240, 720));
}

} // namespace ui::aiprefs
