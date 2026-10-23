#include "ScriptLibrary.hpp"
#include "VltPackageArchive.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

namespace ui {
using Json = vlt::script::Json;
QString ScriptPackage::id() const {
    return QString::fromStdString(manifest.value("id", ""));
}
QString ScriptPackage::name() const {
    return QString::fromStdString(manifest.value("name", ""));
}
QString ScriptLibrary::directory() {
    if (qEnvironmentVariableIsSet("VLT_SCRIPT_LIBRARY"))
        return qEnvironmentVariable("VLT_SCRIPT_LIBRARY");
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/VLTONE/Scripts";
}
QString ScriptLibrary::workerPath() {
    return QCoreApplication::applicationDirPath() + "/vlt_script_worker"
#ifdef Q_OS_WIN
                                                    ".exe"
#endif
        ;
}
Json ScriptLibrary::manifestForLua(const QString &name, const QString &id) {
    return {
        {"format", "vltscript"},
        {"sdk", 1},
        {"id", (id.isEmpty() ? "local." + QUuid::createUuid().toString(QUuid::WithoutBraces) : id)
                   .toStdString()},
        {"version", "1.0.0"},
        {"name", name.toStdString()},
        {"permissions", {"project.read", "project.write", "files.read", "files.write"}},
        {"commands", Json::array({{{"id", "run"},
                                   {"label", name.toStdString()},
                                   {"entry", "main.lua"},
                                   {"contexts",
                                    {"global", "clip", "track", "arrangement", "notes",
                                     "automation", "mixer", "device", "parameter", "file"}}}})}};
}
bool ScriptLibrary::checkSource(const QString &source, const QString &name, QString &error) {
    QTemporaryDir temp;
    if (!temp.isValid()) {
        error = "Cannot create validation directory";
        return false;
    }
    QFile file(temp.filePath("source.lua"));
    if (!file.open(QIODevice::WriteOnly) || file.write(source.toUtf8()) < 0) {
        error = file.errorString();
        return false;
    }
    file.close();
    QProcess worker;
    worker.start(workerPath(), {"--check", file.fileName()});
    if (!worker.waitForStarted(1000)) {
        error = "Cannot start Lua validator: " + worker.errorString();
        return false;
    }
    if (!worker.waitForFinished(5000)) {
        worker.kill();
        worker.waitForFinished(250);
        error = "Lua validation timed out";
        return false;
    }
    if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
        error = QString::fromUtf8(worker.readAllStandardError()).replace(file.fileName(), name);
        if (error.isEmpty())
            error = worker.errorString();
        return false;
    }
    return true;
}
bool ScriptLibrary::validate(const ScriptPackage &package, QString &error, bool syntax) {
    error = QString::fromStdString(vlt::script::validateManifest(package.manifest));
    if (!error.isEmpty())
        return false;
    if (!package.sources.is_object() || package.sources.size() > 128) {
        error = "Invalid Lua source collection";
        return false;
    }
    QSet<QString> paths;
    const auto uniquePath = [&](const std::string &path) {
        const auto folded =
            QString::fromStdString(path).normalized(QString::NormalizationForm_C).toCaseFolded();
        if (paths.contains(folded)) {
            error = "Source and resource paths must be unique on Windows and macOS";
            return false;
        }
        paths.insert(folded);
        return true;
    };
    size_t total = 0;
    for (const auto &[path, source] : package.sources.items()) {
        if (!vlt::script::safeRelativePath(path) || path.starts_with("resources/") ||
            !source.is_string() || !QString::fromStdString(path).endsWith(".lua")) {
            error = "Invalid source path";
            return false;
        }
        total += source.get_ref<const std::string &>().size();
        if (!uniquePath(path))
            return false;
        if (total > vlt::script::maxJsonBytes / 2) {
            error = "Lua sources exceed 1 MiB";
            return false;
        }
        if (syntax && !checkSource(QString::fromStdString(source.get<std::string>()),
                                   QString::fromStdString(path), error))
            return false;
    }
    for (const auto &command : package.manifest["commands"])
        if (!package.sources.contains(command["entry"].get<std::string>())) {
            error = "Command entry is missing";
            return false;
        }
    if (!package.resources.is_object() || package.resources.size() > 2048) {
        error = "Invalid resources";
        return false;
    }
    size_t resourceBytes = 0;
    size_t decodedBytes = 0;
    for (const auto &[path, data] : package.resources.items()) {
        if (!vlt::script::safeRelativePath(path) || !path.starts_with("resources/") ||
            !data.is_string()) {
            error = "Invalid resource";
            return false;
        }
        resourceBytes += data.get_ref<const std::string &>().size();
        if (!uniquePath(path))
            return false;
        if (resourceBytes > 90 * 1024 * 1024) {
            error = "Embedded resources exceed 64 MiB";
            return false;
        }
        const auto decoded =
            QByteArray::fromBase64Encoding(QByteArray::fromStdString(data.get<std::string>()),
                                           QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            error = "Resource contains invalid base64 data";
            return false;
        }
        decodedBytes += decoded.decoded.size();
        if (decodedBytes > 64 * 1024 * 1024) {
            error = "Embedded resources exceed 64 MiB";
            return false;
        }
    }
    for (const auto &path : paths)
        for (auto slash = path.indexOf('/'); slash >= 0; slash = path.indexOf('/', slash + 1))
            if (paths.contains(path.left(slash))) {
                error = "A package file cannot also be a directory";
                return false;
            }
    return true;
}
bool ScriptLibrary::readDirectory(const QString &path, ScriptPackage &package, QString &error) {
    try {
        const QDir directory(QDir(path).absolutePath());
        QFile manifest(directory.filePath("manifest.json"));
        if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 128 * 1024) {
            error = "Cannot read manifest.json";
            return false;
        }
        package = {directory.absolutePath(), Json::parse(manifest.readAll().toStdString()), Json::object()};
        QDirIterator files(directory.absolutePath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories);
        qint64 total = 0;
        int count = 0;
        while (files.hasNext()) {
            files.next();
            const auto info = files.fileInfo();
            const auto relative = directory.relativeFilePath(info.absoluteFilePath());
            if (info.isSymLink() || !vlt::script::safeRelativePath(relative.toStdString()) ||
                ++count > 2048 || (total += info.size()) > 256 * 1024 * 1024) {
                error = "Unsafe or oversized package";
                return false;
            }
            if (info.suffix() == "lua" && !relative.startsWith("resources/")) {
                if (info.size() > 1024 * 1024) {
                    error = "Source too large";
                    return false;
                }
                QFile source(info.filePath());
                if (!source.open(QIODevice::ReadOnly)) {
                    error = source.errorString();
                    return false;
                }
                package.sources[relative.toStdString()] = source.readAll().toStdString();
            } else if (relative.startsWith("resources/")) {
                if (info.size() > 64 * 1024 * 1024) {
                    error = "Resource too large";
                    return false;
                }
                QFile resource(info.filePath());
                if (!resource.open(QIODevice::ReadOnly)) {
                    error = resource.errorString();
                    return false;
                }
                package.resources[relative.toStdString()] =
                    resource.readAll().toBase64().toStdString();
            }
        }
        return validate(package, error, false);
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
bool ScriptLibrary::readCreatorDocument(const QString &path, Json &project, QString &error) {
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) || source.size() > 96 * 1024 * 1024) {
        error = "Cannot read Creator project";
        return false;
    }
    try {
        project = Json::parse(source.readAll().toStdString());
        if (project.at("format") != "vltcreator" || project.at("version") != 4 ||
            project.at("kind") != "lua")
            throw std::runtime_error("Expected Lua Creator project");
        if (!project.at("sources").is_object() || project["sources"].size() > 128)
            throw std::runtime_error("Invalid Lua source collection");
        size_t bytes = 0;
        for (const auto &[path, text] : project["sources"].items()) {
            if (!vlt::script::safeRelativePath(path) || !path.ends_with(".lua") ||
                !text.is_string())
                throw std::runtime_error("Invalid source");
            bytes += text.get_ref<const std::string &>().size();
        }
        if (bytes > 1024 * 1024)
            throw std::runtime_error("Lua sources exceed 1 MiB");
        if (!project.contains("manifestSource"))
            project["manifestSource"] = project.at("manifest").dump(2);
        if (!project["manifestSource"].is_string() ||
            project["manifestSource"].get_ref<const std::string &>().size() > 128 * 1024)
            throw std::runtime_error("Invalid manifest source");
        if (!project.contains("manifest"))
            project["manifest"] = Json::object();
        if (!project["manifest"].is_object())
            throw std::runtime_error("Invalid manifest metadata");
        if (!project.contains("resources"))
            project["resources"] = Json::object();
        if (!project["resources"].is_object() || project["resources"].size() > 2048)
            throw std::runtime_error("Invalid resources");
        for (const auto &[path, data] : project["resources"].items())
            if (!vlt::script::safeRelativePath(path) || !path.starts_with("resources/") ||
                !data.is_string())
                throw std::runtime_error("Invalid resource");
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
bool ScriptLibrary::read(const QString &path, ScriptPackage &package, QString &error) {
    const QFileInfo info(path);
    if (info.isDir())
        return readDirectory(path, package, error);
    if (info.suffix() == "lua") {
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly) || source.size() > 1024 * 1024) {
            error = "Cannot read Lua file";
            return false;
        }
        const auto identity =
            "local.file." +
            QString::fromLatin1(QCryptographicHash::hash(info.canonicalFilePath().toUtf8(),
                                                         QCryptographicHash::Sha256)
                                    .toHex()
                                    .left(24));
        package = {{},
                   manifestForLua(info.completeBaseName(), identity),
                   {{"main.lua", source.readAll().toStdString()}}};
        return true;
    }
    if (info.suffix() == "vltcreator") {
        Json project;
        if (!readCreatorDocument(path, project, error))
            return false;
        try {
            package = {{},
                       Json::parse(project.at("manifestSource").get<std::string>()),
                       project.at("sources"),
                       project["resources"]};
            return validate(package, error, false);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
            return false;
        }
    }
    error = "Use install() to import an archive";
    return false;
}
namespace {
bool writePackage(const ScriptPackage &package, const QString &directory, QString &error) {
    if (!QDir().mkpath(directory)) {
        error = "Cannot create package directory";
        return false;
    }
    if (!package.directory.isEmpty()) {
        QDirIterator files(package.directory, QDir::Files | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const auto relative = QDir(package.directory).relativeFilePath(files.filePath());
            if (relative == "manifest.json" || relative.endsWith(".lua") ||
                package.resources.contains(relative.toStdString()))
                continue;
            if (files.fileInfo().isSymLink() ||
                !vlt::script::safeRelativePath(relative.toStdString())) {
                error = "Unsafe package resource";
                return false;
            }
            const auto destination = QDir(directory).filePath(relative);
            QDir().mkpath(QFileInfo(destination).path());
            if (!QFile::copy(files.filePath(), destination)) {
                error = "Cannot copy package resource";
                return false;
            }
        }
    }
    for (const auto &[name, value] : package.resources.items()) {
        const auto destination = QDir(directory).filePath(QString::fromStdString(name));
        QDir().mkpath(QFileInfo(destination).path());
        QSaveFile file(destination);
        const auto encoded = QByteArray::fromStdString(value.get<std::string>());
        const auto decoded =
            QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || !file.open(QIODevice::WriteOnly) ||
            file.write(decoded.decoded) != decoded.decoded.size() || !file.commit()) {
            error = "Cannot write resource";
            return false;
        }
    }
    auto values = package.sources;
    values["manifest.json"] = package.manifest.dump(2);
    for (const auto &[name, text] : values.items()) {
        const auto path = QDir(directory).filePath(QString::fromStdString(name));
        QDir().mkpath(QFileInfo(path).path());
        QSaveFile file(path);
        const auto bytes = QByteArray::fromStdString(text.get<std::string>());
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            error = file.errorString();
            return false;
        }
    }
    return true;
}
} // namespace
bool ScriptLibrary::install(const QString &path, ScriptPackage &package, QString &error,
                            const QString &expectedId) {
    QTemporaryDir staging;
    if (QFileInfo(path).suffix() == "vltscript") {
        if (!staging.isValid() || !vltarchive::extract(path, staging.path(), error) ||
            !readDirectory(staging.path(), package, error))
            return false;
    } else if (!read(path, package, error))
        return false;
    if (!expectedId.isEmpty()) {
        if (QFileInfo(path).suffix() == "lua")
            package.manifest["id"] = expectedId.toStdString();
        else if (package.id() != expectedId) {
            error = "Update has a different extension id";
            return false;
        }
    }
    if (!install(package, error))
        return false;
    package.directory = QDir(directory()).filePath(package.id());
    return true;
}
bool ScriptLibrary::install(const ScriptPackage &package, QString &error, bool checkSyntax) {
    if (!validate(package, error, checkSyntax))
        return false;
    if (!QDir().mkpath(directory())) {
        error = "Cannot create Scripts library";
        return false;
    }
    QTemporaryDir staging(directory() + "/.install-XXXXXX");
    if (!staging.isValid() || !writePackage(package, staging.path(), error))
        return false;
    const auto target = QDir(directory()).filePath(package.id());
    const auto backup = target + ".previous-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool exists = QFileInfo::exists(target);
    if (exists && !QDir().rename(target, backup)) {
        error = "Cannot stage existing package";
        return false;
    }
    if (!QDir().rename(staging.path(), target)) {
        if (exists)
            QDir().rename(backup, target);
        error = "Cannot publish package";
        return false;
    }
    staging.setAutoRemove(false);
    if (exists)
        QDir(backup).removeRecursively();
    return true;
}
bool ScriptLibrary::exportPackage(const ScriptPackage &package, const QString &path,
                                  QString &error) {
    if (!validate(package, error))
        return false;
    QTemporaryDir staging;
    if (!staging.isValid() || !writePackage(package, staging.path(), error))
        return false;
    return vltarchive::pack(staging.path(), path, error);
}
QList<ScriptPackage> ScriptLibrary::installed() {
    QList<ScriptPackage> result;
    for (const auto &entry : QDir(directory()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.isSymLink() || entry.fileName().startsWith('.'))
            continue;
        ScriptPackage package;
        QString error;
        if (readDirectory(entry.filePath(), package, error) && package.id() == entry.fileName())
            result.append(package);
    }
    return result;
}
bool ScriptLibrary::remove(const QString &id, QString &error) {
    const auto target = QDir(directory()).filePath(id);
    ScriptPackage package;
    if (!readDirectory(target, package, error) || package.id() != id ||
        QFileInfo(target).isSymLink())
        return false;
    if (!QDir(target).removeRecursively()) {
        error = "Cannot remove extension";
        return false;
    }
    return true;
}
} // namespace ui
