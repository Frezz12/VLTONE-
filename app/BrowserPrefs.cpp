#include "BrowserPrefs.hpp"

#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace ui::browserprefs {

namespace {

QString key(const char* name) {
    return QStringLiteral("browser/") + QLatin1String(name);
}

QString cleanPath(const QString& path) {
    if (path.trimmed().isEmpty()) return {};
    return QFileInfo(path).absoluteFilePath();
}

bool samePath(const QString& a, const QString& b) {
#if defined(Q_OS_WIN)
    return cleanPath(a).compare(cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return cleanPath(a) == cleanPath(b);
#endif
}

QVector<Collection> readCollections() {
    QVector<Collection> out;
    const auto document = QJsonDocument::fromJson(
        QSettings().value(key("collections")).toByteArray());
    if (document.isArray()) {
        for (const QJsonValue& value : document.array()) {
            const QJsonObject object = value.toObject();
            Collection collection;
            collection.id = object.value(QStringLiteral("id")).toString();
            collection.name = object.value(QStringLiteral("name")).toString().trimmed();
            collection.color = object.value(QStringLiteral("color")).toString();
            for (const QJsonValue& path : object.value(QStringLiteral("paths")).toArray()) {
                const QString clean = cleanPath(path.toString());
                if (!clean.isEmpty() && std::none_of(collection.paths.cbegin(),
                                                     collection.paths.cend(),
                    [&clean](const QString& existing) { return samePath(existing, clean); }))
                    collection.paths.append(clean);
            }
            if (!collection.id.isEmpty() && !collection.name.isEmpty() &&
                std::none_of(out.cbegin(), out.cend(), [&collection](const Collection& existing) {
                    return existing.id == collection.id;
                }))
                out.append(collection);
        }
    }

    const QString favorite = QStringLiteral("favorites");
    auto it = std::find_if(out.begin(), out.end(), [&favorite](const Collection& collection) {
        return collection.id == favorite;
    });
    if (it == out.end()) {
        out.prepend({favorite,
                     QCoreApplication::translate("FileBrowserTree", "Favorites"),
                     {}, {}});
    } else {
        it->name = QCoreApplication::translate("FileBrowserTree", "Favorites");
        if (it != out.begin()) std::rotate(out.begin(), it, it + 1);
    }
    return out;
}

void writeCollections(const QVector<Collection>& collections) {
    QJsonArray array;
    for (const Collection& collection : collections) {
        QJsonArray paths;
        for (const QString& path : collection.paths) paths.append(path);
        QJsonObject object;
        object.insert(QStringLiteral("id"), collection.id);
        object.insert(QStringLiteral("name"), collection.name);
        object.insert(QStringLiteral("color"), collection.color);
        object.insert(QStringLiteral("paths"), paths);
        array.append(object);
    }
    QSettings().setValue(key("collections"),
                         QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QJsonObject readFolderColors() {
    const auto document = QJsonDocument::fromJson(
        QSettings().value(key("folderColors")).toByteArray());
    return document.isObject() ? document.object() : QJsonObject{};
}

/// What a first run shows, so the panel is not an empty box: the user's Music
/// folder, and Downloads, which is where a sample pack lands. Only the ones
/// that exist — offering a folder that is not there reads as a bug.
QStringList defaultFolders() {
    QStringList out;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::MusicLocation, QStandardPaths::DownloadLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && QDir(path).exists()) out << path;
    }
    return out;
}

} // namespace

QString favoritesId() { return QStringLiteral("favorites"); }

QVector<Collection> collections() { return readCollections(); }

QString createCollection(const QString& name) {
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty()) return {};
    auto current = readCollections();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    current.append({id, cleanName, {}, {}});
    writeCollections(current);
    return id;
}

bool renameCollection(const QString& id, const QString& name) {
    if (id.isEmpty() || id == favoritesId() || name.trimmed().isEmpty()) return false;
    auto current = readCollections();
    for (Collection& collection : current) {
        if (collection.id != id) continue;
        collection.name = name.trimmed();
        writeCollections(current);
        return true;
    }
    return false;
}

bool removeCollection(const QString& id) {
    if (id.isEmpty() || id == favoritesId()) return false;
    auto current = readCollections();
    const auto end = std::remove_if(current.begin(), current.end(),
                                    [&id](const Collection& collection) {
                                        return collection.id == id;
                                    });
    if (end == current.end()) return false;
    current.erase(end, current.end());
    writeCollections(current);
    removeTab(id);
    if (activeCollection() == id) setActiveCollection({});
    return true;
}

bool addToCollection(const QString& id, const QString& path) {
    const QString clean = cleanPath(path);
    if (id.isEmpty() || clean.isEmpty() || !QFileInfo::exists(clean)) return false;
    auto current = readCollections();
    for (Collection& collection : current) {
        if (collection.id != id) continue;
        if (std::any_of(collection.paths.cbegin(), collection.paths.cend(),
                        [&clean](const QString& existing) {
                            return samePath(existing, clean);
                        })) return false;
        collection.paths.append(clean);
        writeCollections(current);
        return true;
    }
    return false;
}

bool removeFromCollection(const QString& id, const QString& path) {
    auto current = readCollections();
    for (Collection& collection : current) {
        if (collection.id != id) continue;
        const auto end = std::remove_if(collection.paths.begin(), collection.paths.end(),
                                        [&path](const QString& existing) {
                                            return samePath(existing, path);
                                        });
        if (end == collection.paths.end()) return false;
        collection.paths.erase(end, collection.paths.end());
        writeCollections(current);
        return true;
    }
    return false;
}

void setCollectionColor(const QString& id, const QString& color) {
    auto current = readCollections();
    for (Collection& collection : current) {
        if (collection.id != id) continue;
        collection.color = color;
        writeCollections(current);
        return;
    }
}

QVector<Tab> tabs() {
    QVector<Tab> out;
    const auto available = readCollections();
    const auto exists = [&available](const QString& id) {
        return std::any_of(available.cbegin(), available.cend(),
                           [&id](const Collection& collection) {
                               return collection.id == id;
                           });
    };
    const auto document = QJsonDocument::fromJson(
        QSettings().value(key("tabs")).toByteArray());
    if (!document.isArray()) return out;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("collectionId")).toString();
        if (id.isEmpty() || !exists(id) ||
            std::any_of(out.cbegin(), out.cend(), [&id](const Tab& tab) {
                return tab.collectionId == id;
            })) continue;
        out.append({id, object.value(QStringLiteral("icon")).toString(
                            QStringLiteral("folder"))});
        if (out.size() >= kMaxTabs - 1) break;
    }
    return out;
}

void writeTabs(const QVector<Tab>& tabs) {
    QJsonArray array;
    for (const Tab& tab : tabs) {
        QJsonObject object;
        object.insert(QStringLiteral("collectionId"), tab.collectionId);
        object.insert(QStringLiteral("icon"), tab.icon);
        array.append(object);
    }
    QSettings().setValue(key("tabs"),
                         QJsonDocument(array).toJson(QJsonDocument::Compact));
}

bool addTab(const QString& collectionId, const QString& icon) {
    auto current = tabs();
    if (current.size() >= kMaxTabs - 1 ||
        std::any_of(current.cbegin(), current.cend(), [&collectionId](const Tab& tab) {
            return tab.collectionId == collectionId;
        })) return false;
    const auto available = readCollections();
    if (std::none_of(available.cbegin(), available.cend(),
                     [&collectionId](const Collection& collection) {
                         return collection.id == collectionId;
                     })) return false;
    current.append({collectionId, icon.isEmpty() ? QStringLiteral("folder") : icon});
    writeTabs(current);
    return true;
}

void removeTab(const QString& collectionId) {
    auto current = tabs();
    current.erase(std::remove_if(current.begin(), current.end(),
                                 [&collectionId](const Tab& tab) {
                                     return tab.collectionId == collectionId;
                                 }), current.end());
    writeTabs(current);
    if (activeCollection() == collectionId) setActiveCollection({});
}

void setTabIcon(const QString& collectionId, const QString& icon) {
    auto current = tabs();
    for (Tab& tab : current) {
        if (tab.collectionId != collectionId) continue;
        tab.icon = icon.isEmpty() ? QStringLiteral("folder") : icon;
        writeTabs(current);
        return;
    }
}

QString activeCollection() {
    const QString stored = QSettings().value(key("activeCollection")).toString();
    if (stored.isEmpty()) return {};
    if (stored == QStringLiteral("project-clips")) return stored;
    const auto current = tabs();
    return std::any_of(current.cbegin(), current.cend(), [&stored](const Tab& tab) {
        return tab.collectionId == stored;
    }) ? stored : QString{};
}

void setActiveCollection(const QString& collectionId) {
    QSettings().setValue(key("activeCollection"), collectionId);
}

QString directFolderColor(const QString& folder) {
    const QString clean = cleanPath(folder);
    const QJsonObject colors = readFolderColors();
    for (auto it = colors.constBegin(); it != colors.constEnd(); ++it)
        if (samePath(it.key(), clean)) return it.value().toString();
    return {};
}

QString folderColor(const QString& folder) {
    QString current = cleanPath(folder);
    while (!current.isEmpty()) {
        const QString color = directFolderColor(current);
        if (!color.isEmpty()) return color;
        const QString parent = QFileInfo(current).dir().absolutePath();
        if (samePath(parent, current)) break;
        current = parent;
    }
    return {};
}

void setFolderColor(const QString& folder, const QString& color) {
    const QString clean = cleanPath(folder);
    if (clean.isEmpty()) return;
    QJsonObject colors = readFolderColors();
    QString storedKey;
    for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
        if (samePath(it.key(), clean)) {
            storedKey = it.key();
            break;
        }
    }
    if (!storedKey.isEmpty()) colors.remove(storedKey);
    if (!color.isEmpty()) colors.insert(clean, color);
    QSettings().setValue(key("folderColors"),
                         QJsonDocument(colors).toJson(QJsonDocument::Compact));
}

QStringList folders() {
    QSettings settings;
    if (!settings.contains(key("folders"))) return defaultFolders();
    QStringList out = settings.value(key("folders")).toStringList();
    out.removeAll(QString());
    return out;
}

void setFolders(const QStringList& folders) {
    QSettings().setValue(key("folders"), folders);
}

bool addFolder(const QString& folder) {
    const QString clean = QDir::cleanPath(folder);
    if (clean.isEmpty()) return false;
    QStringList current = folders();
    if (current.contains(clean)) return false;
    current << clean;
    setFolders(current);
    return true;
}

void removeFolder(const QString& folder) {
    QStringList current = folders();
    current.removeAll(QDir::cleanPath(folder));
    // Written even when nothing was removed: the first removal has to stop the
    // defaults from coming back, and only a stored (possibly empty) list does.
    setFolders(current);
}

QStringList ignoredExtensions() {
    return QSettings().value(key("ignoredExtensions")).toStringList();
}

void setIgnoredExtensions(const QString& extensions) {
    QStringList normalized;
    const auto parts = extensions.toLower().split(
        QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    for (QString part : parts) {
        if (part.startsWith(QLatin1String("*."))) part.remove(0, 2);
        else if (part.startsWith(QLatin1Char('.'))) part.remove(0, 1);
        if (part.isEmpty() || part.contains(QRegularExpression(QStringLiteral("[*/\\\\?:]"))))
            continue;
        if (!normalized.contains(part)) normalized.append(part);
    }
    QSettings().setValue(key("ignoredExtensions"), normalized);
}

bool isIgnoredFile(const QString& fileName, const QStringList& extensions) {
    for (const QString& extension : extensions)
        if (fileName.endsWith(QLatin1Char('.') + extension, Qt::CaseInsensitive)) return true;
    return false;
}

bool onLeft() { return QSettings().value(key("onLeft"), true).toBool(); }
void setOnLeft(bool onLeft) { QSettings().setValue(key("onLeft"), onLeft); }

bool visible() { return QSettings().value(key("visible"), true).toBool(); }
void setVisible(bool visible) { QSettings().setValue(key("visible"), visible); }

int width() {
    return std::clamp(QSettings().value(key("width"), 240).toInt(), 160, 720);
}
void setWidth(int width) {
    QSettings().setValue(key("width"), std::clamp(width, 160, 720));
}

bool autoPreview() { return QSettings().value(key("autoPreview"), true).toBool(); }
void setAutoPreview(bool on) { QSettings().setValue(key("autoPreview"), on); }

bool previewLoop() { return QSettings().value(key("previewLoop"), false).toBool(); }
void setPreviewLoop(bool loop) { QSettings().setValue(key("previewLoop"), loop); }

float previewGain() {
    return std::clamp(QSettings().value(key("previewGain"), 1.0).toFloat(), 0.0f,
                      2.0f);
}
void setPreviewGain(float gain) {
    QSettings().setValue(key("previewGain"), std::clamp(gain, 0.0f, 2.0f));
}

MidiTempo midiTempoPolicy() {
    const QString stored =
        QSettings().value(key("midiTempo"), QStringLiteral("ask")).toString();
    if (stored == QLatin1String("keep")) return MidiTempo::Keep;
    if (stored == QLatin1String("adopt")) return MidiTempo::Adopt;
    return MidiTempo::Ask;
}

void setMidiTempoPolicy(MidiTempo policy) {
    const char* stored = policy == MidiTempo::Keep    ? "keep"
                         : policy == MidiTempo::Adopt ? "adopt"
                                                      : "ask";
    QSettings().setValue(key("midiTempo"), QLatin1String(stored));
}

double zoom() {
    const double stored = QSettings().value(key("zoom"), 1.0).toDouble();
    return std::clamp(stored, kMinZoom, kMaxZoom);
}

void setZoom(double factor) {
    QSettings().setValue(key("zoom"), std::clamp(factor, kMinZoom, kMaxZoom));
}

} // namespace ui::browserprefs
