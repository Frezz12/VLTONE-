#include "VltContentInstaller.hpp"
#include "VltPackageArchive.hpp"
#include "MiniModuleLibrary.hpp"
#include "CreatorProject.hpp"
#include "ChannelStripPresets.hpp"
#include "ChannelStripPreset.hpp"
#include "ProjectSerializer.hpp"
#include "ProjectTemplates.hpp"
#include "SlicerTools.hpp"
#include "Internal/MorphProgram.hpp"
#include "ThemePackage.hpp"
#include "LocalizationManager.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVersionNumber>
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>

namespace ui {
namespace {
QString documents(const QString& folder) {return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/VLTONE/" + folder;}
QString managedRoot() {const auto testRoot=QCoreApplication::instance()->property("dawHeadlessDataRoot").toString();if(!testRoot.isEmpty())return testRoot+"/ForumContent";return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/ForumContent";}
QString fileHash(const QString& path, const VltContentInstaller::KeepGoing& keepGoing = {}) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (keepGoing && !keepGoing()) return {};
        const auto chunk = file.read(1024 * 1024); if (chunk.isEmpty() && file.error() != QFile::NoError) return {};
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}
bool copyFile(const QString& source, const QString& target, QString& error, const VltContentInstaller::KeepGoing& keepGoing) {
    QFile input(source); QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {error="Could not open installation files";return false;}
    while (!input.atEnd()) {
        if (keepGoing && !keepGoing()) {error="Installation cancelled";return false;}
        const auto chunk=input.read(1024*1024);
        if ((chunk.isEmpty() && input.error()!=QFile::NoError)||output.write(chunk)!=chunk.size()) {error="Could not write file; check free disk space";return false;}
    }
    if (!output.commit()) {error=output.errorString();return false;}
    return true;
}
bool validateProject(const QString& path, bool package, QString& error) {
    const QString manifest=package?QString::fromStdString(daw::ProjectSerializer::manifestPath(path.toStdString())):path;
    if (QFileInfo(manifest).size()>64*1024*1024) {error="Project manifest exceeds 64 MiB";return false;}
    daw::ProjectModel project;
    const auto result=package?daw::ProjectSerializer::load(project,path.toStdString()):daw::ProjectSerializer::loadDocument(project,path.toStdString(),QFileInfo(path).absolutePath().toStdString());
    if (!result) error=QString::fromStdString(result.message());
    return bool(result);
}
}
QString VltContentInstaller::resourceType(const QString& name) {
    const QString lower=name.toLower();
    const std::pair<const char*,const char*> formats[]={{".vltlang.json","language"},{".vlt.zip","project-package"},{".vltt.zip","template"},{".vltmini","mini"},{".vltnode","node"},{".vltcreator","creator"},{".vlts","strip"},{".vltmorph","morph"},{".vltslicer","slicer"},{".vlttheme","theme"},{".vlt","project"},{".vltt","template-document"}};
    for (const auto& [extension,type]:formats) if (lower.endsWith(QLatin1String(extension))) return QLatin1String(type);
    return {};
}
QString VltContentInstaller::libraryDirectory(const QString& type) {
    const auto testRoot=QCoreApplication::instance()->property("dawHeadlessDataRoot").toString();
    if(!testRoot.isEmpty())return testRoot+"/ForumLibraries/"+type;
    if(type=="mini")return MiniModuleLibrary::directory();
    if(type=="node")return documents("CreatorNodes");
    if(type=="creator")return documents("CreatorProjects");
    if(type=="strip")return channelstrippresets::stripFolder();
    if(type=="offline-strip")return channelstrippresets::offlineFolder();
    if(type=="morph")return managedRoot()+"/Morph";
    if(type=="slicer")return channelstrippresets::rootFolder()+"/Slicer";
    if(type=="theme")return ThemePackage::libraryDirectory();
    if(type=="language")return LocalizationManager::instance().languageDirectory();
    if(type=="project"||type=="project-package")return documents("DownloadedProjects");
    if(type=="template"||type=="template-document")return projecttemplates::folder();
    return {};
}
QString VltContentInstaller::attachmentId(const QUrl& url,const QUrl& origin) {
    if (url.scheme()!=origin.scheme()||url.host()!=origin.host()||url.port()!=origin.port()||!url.userInfo().isEmpty()||url.hasQuery()||url.hasFragment())return {};
    if(origin.scheme()!="https" && !(origin.scheme()=="http" && (origin.host()=="localhost"||origin.host()=="127.0.0.1")))return {};
    static const QRegularExpression pattern("^/api/v1/forum/attachments/([0-9a-fA-F-]{36})/download$");
    const auto match=pattern.match(url.path());if(!match.hasMatch()||QUuid(match.captured(1)).isNull())return {};
    return match.captured(1);
}
VltInstallResult VltContentInstaller::install(const QString& source,const QJsonObject& meta,KeepGoing keepGoing,bool replaceLanguage) {
    VltInstallResult result;
    const auto fail=[&](const QString& error){result.error=error;result.cancelled=keepGoing&&!keepGoing();return result;};
    const QString name=meta["name"].toString(), hash=meta["sha256"].toString();
    QString type=resourceType(name);
    if(type=="strip"&&meta["resource_type"]=="offline-strip")type="offline-strip";
    result.resourceType=type;
    if(type.isEmpty()||meta["kind"]!="vlt"||meta["status"]!="ready"||meta["resource_type"].toString()!=type)return fail(QCoreApplication::translate("VltContentInstaller", "Unsupported VLT content metadata"));
    if(!QRegularExpression("^[a-f0-9]{64}$").match(hash).hasMatch()||QFileInfo(source).isSymLink()||!QFileInfo(source).isFile()||QFileInfo(source).size()!=meta["bytes"].toInteger()||QFileInfo(source).size()>1'000'000'000LL)return fail(QCoreApplication::translate("VltContentInstaller", "Download size or metadata is invalid"));
    const QString minimum=meta["minimum_version"].toString();
    if(!minimum.isEmpty()){
        const auto required=QVersionNumber::fromString(minimum),current=QVersionNumber::fromString(QCoreApplication::applicationVersion());
        if(required.isNull()||current.isNull()||current<required)return fail(QCoreApplication::translate("VltContentInstaller", "This file requires a newer VLTONE version"));
    }
    if(type=="language" && QFileInfo(source).size()>5*1024*1024)return fail(QCoreApplication::translate("VltContentInstaller", "Language pack exceeds 5 MiB"));
    if(fileHash(source,keepGoing)!=hash)return fail(QCoreApplication::translate("VltContentInstaller", "Download is damaged or was cancelled (SHA-256 mismatch)"));
    QDir().mkpath(managedRoot());
    QLockFile lock(managedRoot()+"/install.lock");lock.setStaleLockTime(0);
    if(!lock.tryLock(0))return fail(QCoreApplication::translate("VltContentInstaller", "Another installation is running. Retry this download when it finishes"));
    const QString indexPath=managedRoot()+"/index.json";
    QFile indexFile(indexPath);QJsonObject index;if(indexFile.open(QIODevice::ReadOnly))index=QJsonDocument::fromJson(indexFile.readAll()).object();indexFile.close();
    const QString indexKey=type+":"+hash;
    const QString previous=index[indexKey].toString();
    if(!previous.isEmpty()&&QFileInfo::exists(previous)&&(QFileInfo(previous).isDir()||fileHash(previous,keepGoing)==hash)) {result.ok=true;result.duplicate=true;result.path=previous;return result;}
    if(keepGoing&&!keepGoing())return fail(QCoreApplication::translate("VltContentInstaller", "Installation cancelled"));
    QString folder;
    std::function<bool()> registerMorph;
    if(type=="language") {
        // Language import is bounded to 5 MiB and dispatched by the bridge on Qt's thread.
        if(QThread::currentThread()!=QCoreApplication::instance()->thread())return fail(QCoreApplication::translate("VltContentInstaller", "Language installation requires the application thread"));
        auto installLanguage=[&]{auto& manager=LocalizationManager::instance();auto r=manager.importLanguagePack(source,replaceLanguage);result.ok=r.ok;result.languageConflict=r.alreadyExists;result.error=r.error;result.path=manager.languageDirectory()+"/"+r.locale+".vltlang.json";};
        installLanguage();
        if(!result.ok)return result;
    }else{
        folder=libraryDirectory(type);if(folder.isEmpty()||!QDir().mkpath(folder))return fail(QCoreApplication::translate("VltContentInstaller", "Could not create the content library"));
        if(QStorageInfo(folder).bytesAvailable()<QFileInfo(source).size()+1'000'000)return fail(QCoreApplication::translate("VltContentInstaller", "Not enough free disk space"));
        QTemporaryDir staging(folder+"/.install-XXXXXX");if(!staging.isValid())return fail(QCoreApplication::translate("VltContentInstaller", "Could not create installation staging directory"));
        QString error,staged=source;
        bool package=type=="project-package"||type=="template";
        if(package){
            if(!vltarchive::extract(source,staging.path(),error,keepGoing))return fail(error);
            staged=staging.path();
            // Also accept ZIPs made by Finder with one outer package directory.
            auto entries=QDir(staged).entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden);
            if(entries.size()==1&&entries[0].isDir())staged=entries[0].absoluteFilePath();
            if(!validateProject(staged,true,error))return fail(error);
        }else if(type=="project"||type=="template-document"){
            if(!validateProject(source,false,error))return fail(error);
        }else if(type=="mini"){
            const auto parsed=MiniModuleLibrary::read(source);if(!parsed.error.isEmpty())return fail(parsed.error);
        }else if(type=="creator"){
            CreatorProject parsed;if(!CreatorProject::open(source,parsed,error))return fail(error);
        }else if(type=="node"){
            QFile file(source);if(!file.open(QIODevice::ReadOnly)||file.size()>32*1024*1024)return fail(QCoreApplication::translate("VltContentInstaller", "Cannot read Creator node"));
            const auto bytes=file.readAll();auto document=nlohmann::json::parse(bytes.constData(),bytes.constData()+bytes.size(),nullptr,false);
            try{
                if(document.at("format")!="vltnode"||document.at("version")!=1)return fail(QCoreApplication::translate("VltContentInstaller", "Unsupported Creator node version"));
                const auto root=document.at("root").get<std::string>();document["format"]="vltcreator";document["version"]=3;
                CreatorProject parsed;if(!CreatorProject::fromJson(document,parsed,error))return fail(error);
                if(std::none_of(parsed.definition.subgraphs.begin(),parsed.definition.subgraphs.end(),[&](const auto& g){return g.id==root;}))return fail(QCoreApplication::translate("VltContentInstaller", "Missing Creator root graph"));
            }catch(...){return fail(QCoreApplication::translate("VltContentInstaller", "Damaged Creator node"));}
        }else if(type=="strip"||type=="offline-strip"){
            if(QFileInfo(source).size()>64*1024*1024)return fail(QCoreApplication::translate("VltContentInstaller", "Channel Strip preset exceeds 64 MiB"));
            daw::EngineController::ChannelSnapshot snapshot;const auto parsed=daw::ChannelStripPreset::load(snapshot,source.toStdString());if(!parsed)return fail(QString::fromStdString(parsed.message()));
        }else if(type=="slicer"){
            daw::plugins::slicer::ControlState state;std::string why;
            if(!daw::slicer::loadPreset(source.toStdString(),staging.path().toStdString(),state,why,keepGoing))return fail(QString::fromStdString(why));
        }else if(type=="theme"){
            const auto installed=ThemePackage::install(source,[&](qint64,qint64){return !keepGoing||keepGoing();});
            if(!installed.ok)return fail(installed.error);result.path=installed.filePath;
        }else if(type=="morph"){
            QFile file(source);if(!file.open(QIODevice::ReadOnly)||file.size()>2*1024*1024)return fail(QCoreApplication::translate("VltContentInstaller", "Invalid Morph preset"));
            auto bytes=file.readAll();auto state=daw::plugins::morph::decodeState({reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size())});if(!state)return fail(QCoreApplication::translate("VltContentInstaller", "Invalid or unsupported Morph preset"));
            registerMorph=[state=std::move(*state),name,hash]() mutable {
                QSettings settings;auto entries=QJsonDocument::fromJson(settings.value("Morph/users.v1").toByteArray()).array();
                for(const auto& entry:entries)if(entry.toObject()["forum_sha256"]==hash)return true;
                QString label=QString::fromStdString(state.presetName);if(label.isEmpty())label=QFileInfo(name).completeBaseName();
                const QString original=label;auto exists=[&]{for(const auto& entry:entries)if(entry.toObject()["name"]==label)return true;return false;};
                for(int n=2;exists();++n)label=original+QString(" (%1)").arg(n);
                state.presetKind="user";state.presetName=label.toStdString();std::vector<uint8_t> encoded;
                if(!daw::plugins::morph::encodeState(state,encoded))return false;
                entries.append(QJsonObject{{"forum_sha256",hash},{"name",label},{"category",QString::fromStdString(state.category)},{"state",QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(encoded.data()),int(encoded.size())).toBase64())}});
                settings.setValue("Morph/users.v1",QJsonDocument(entries).toJson(QJsonDocument::Compact));settings.sync();
                return settings.status()==QSettings::NoError;
            };
        }
        if(result.path.isEmpty()&&keepGoing&&!keepGoing())return fail(QCoreApplication::translate("VltContentInstaller", "Installation cancelled"));
        if(result.path.isEmpty()){
            QString safe=name;safe.replace(QRegularExpression("[^\\p{L}\\p{N}_. -]"),"_");if(safe.startsWith('.'))safe.prepend("material");safe=safe.left(180);
            if(package)safe.chop(4);
            const QString extension=type=="template-document"?"vltt":QFileInfo(safe).suffix();
            const QString base=QFileInfo(safe).completeBaseName();
            QString target=QDir(folder).filePath(safe);
            if(QFileInfo::exists(target))target=QDir(folder).filePath(base+"-"+hash+"."+extension);
            if(QFileInfo::exists(target))return fail(QCoreApplication::translate("VltContentInstaller", "An installation with this name already exists"));
            if(package){
                if(!QDir().rename(staged,target))return fail(QCoreApplication::translate("VltContentInstaller", "Could not atomically install the project package"));
            }else if(type=="template-document"){
                const QString document=staging.path()+"/"+daw::ProjectSerializer::kProjectFile;
                if(!copyFile(source,document,error,keepGoing)||!QDir().rename(staging.path(),target))return fail(error.isEmpty()?"Could not install template":error);
            }else if(!copyFile(source,target,error,keepGoing))return fail(error);
            result.path=target;
        }
        if(registerMorph&&!registerMorph()){QFile::remove(result.path);return fail(QCoreApplication::translate("VltContentInstaller", "Could not save Morph user library"));}
        result.ok=true;
    }
    index.insert(indexKey,result.path);QSaveFile output(indexPath);
    if(output.open(QIODevice::WriteOnly)){const auto bytes=QJsonDocument(index).toJson();if(output.write(bytes)==bytes.size())output.commit();}
    return result;
}
bool VltContentInstaller::exportPackage(const QString& savedDirectory,const QString& destination,QString& error,KeepGoing keepGoing) {
    if(!validateProject(savedDirectory,true,error))return false;
    return vltarchive::pack(savedDirectory,destination,error,keepGoing);
}
}
