#include "VltContentInstaller.hpp"
#include "VltPackageArchive.hpp"
#include "CreatorProject.hpp"
#include "MiniModuleLibrary.hpp"
#include "ThemePackage.hpp"
#include "LocalizationManager.hpp"
#include "ProjectSerializer.hpp"
#include "ChannelStripPreset.hpp"
#include "SlicerTools.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Internal/MorphProgram.hpp"
#include "Internal/SlicerInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDataStream>
#include <archive.h>
#include <archive_entry.h>
#include <nlohmann/json.hpp>
#include <cstdio>

int main(int argc,char** argv) {
 QTemporaryDir root;
 QSettings::setDefaultFormat(QSettings::IniFormat);
 QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,root.path());
 QApplication app(argc,argv);app.setOrganizationName("VLTONE Tests");app.setApplicationName("Forum Installer");app.setApplicationVersion("0.5.1");app.setProperty("dawHeadlessDataRoot",root.path());
 ui::LocalizationManager::instance();
 int failures=0;auto check=[&](bool ok,const QString& what){if(!ok){++failures;fprintf(stderr,"FAIL: %s\n",qPrintable(what));}};
 auto write=[&](const QString& name,const QByteArray& data){const auto path=root.path()+"/"+name;QFile f(path);check(f.open(QIODevice::WriteOnly)&&f.write(data)==data.size(),name+" fixture");return path;};
 auto metadata=[](const QString& path,const QString& name=QString()) {QFile f(path);f.open(QIODevice::ReadOnly);auto bytes=f.readAll();const auto filename=name.isEmpty()?QFileInfo(path).fileName():name;return QJsonObject{{"id","12345678-1234-1234-1234-123456789abc"},{"name",filename},{"kind","vlt"},{"status","ready"},{"bytes",qint64(bytes.size())},{"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},{"resource_type",ui::VltContentInstaller::resourceType(filename)}};};
 auto install=[&](const QString& path,QJsonObject meta=QJsonObject{}) {if(meta.isEmpty())meta=metadata(path);const auto result=ui::VltContentInstaller::install(path,meta);check(result.ok,QFileInfo(path).fileName()+": "+result.error);check(QFileInfo::exists(result.path),"installed path exists");const auto repeat=ui::VltContentInstaller::install(path,meta);check(repeat.ok&&repeat.duplicate&&repeat.path==result.path,"identical download is idempotent");return result;};
 auto creator=ui::CreatorProject::create("Shared","Module");QString error;
 auto mini=write("first.vltmini",QByteArray::fromStdString(nlohmann::json{{"format","vltmini"},{"version",5},{"definition",daw::plugins::mini::toJson(creator.definition)}}.dump()));
 auto miniResult=install(mini);check(QFileInfo(miniResult.path).absolutePath()==ui::VltContentInstaller::libraryDirectory("mini"),"Mini library");
 creator.definition.name="Another";auto other=write("second.vltmini",QByteArray::fromStdString(nlohmann::json{{"format","vltmini"},{"version",5},{"definition",daw::plugins::mini::toJson(creator.definition)}}.dump()));
 auto otherResult=install(other,metadata(other,"first.vltmini"));check(otherResult.path!=miniResult.path,"same name different bytes preserved");
 auto creatorPath=root.path()+"/shared.vltcreator";check(creator.save(creatorPath,error),error);install(creatorPath);
 creator.definition.nodes.push_back(daw::plugins::mini::makeNode("gain","gain"));
 creator.definition.connections={{"input","gain","out","in"},{"gain","output","out","in"}};
 const auto packed=creator.pack({"gain"},"Shared node",error);check(!packed.isEmpty(),"Creator node fixture "+error);
 auto node=creator.toJson();node["format"]="vltnode";node["version"]=1;node["root"]=creator.definition.subgraphs.front().id;install(write("node.vltnode",QByteArray::fromStdString(node.dump())));
 daw::EngineController::ChannelSnapshot strip;strip.hasSettings=true;
 const auto stripPath=root.path()+"/strip.vlts";check(bool(daw::ChannelStripPreset::save(strip,stripPath.toStdString())),"strip fixture");install(stripPath);
 auto offline=metadata(stripPath);offline["resource_type"]="offline-strip";const auto offlineResult=install(stripPath,offline);check(QFileInfo(offlineResult.path).absolutePath()==ui::VltContentInstaller::libraryDirectory("offline-strip"),"offline strip library");
 std::vector<uint8_t> morph;check(daw::plugins::morph::encodeState(daw::plugins::morph::factoryState(0),morph),"Morph fixture");install(write("preset.vltmorph",QByteArray(reinterpret_cast<const char*>(morph.data()),int(morph.size()))));
 check(QJsonDocument::fromJson(QSettings().value("Morph/users.v1").toByteArray()).array().size()==1,"Morph registration appears once");
 // Portable Slicer: a tiny PCM WAV embedded by the production serializer.
 QByteArray wav;QDataStream stream(&wav,QIODevice::WriteOnly);stream.setByteOrder(QDataStream::LittleEndian);
 stream.writeRawData("RIFF",4);stream<<quint32(36+960);stream.writeRawData("WAVEfmt ",8);stream<<quint32(16)<<quint16(1)<<quint16(1)<<quint32(48000)<<quint32(96000)<<quint16(2)<<quint16(16);stream.writeRawData("data",4);stream<<quint32(960);for(int i=0;i<480;++i)stream<<qint16(i%30*100);
 auto wavPath=write("sample.wav",wav);daw::plugins::slicer::SlicerInstance slicer;audio::platform::DecodedAudio decoded;check(bool(audio::platform::decodeAudioFile(wavPath.toStdString(),decoded)),"Slicer WAV fixture");check(slicer.adoptSample(wavPath.toStdString(),daw::engine::SampleBuffer::fromInterleaved(decoded.interleaved,decoded.channels,decoded.frames,decoded.sampleRate)),"Slicer sample fixture");std::string reason;
 const auto slicerPath=root.path()+"/preset.vltslicer";check(daw::slicer::savePreset(slicerPath.toStdString(),slicer.captureState(),reason),QString::fromStdString(reason));install(slicerPath);
 ui::ThemePackageSnapshot theme;check(ui::ThemePackage::captureCurrent("Forum theme",theme,&error),error);const auto themePath=root.path()+"/theme.vlttheme";check(ui::ThemePackage::write(theme,themePath).ok,"theme fixture");const auto beforeTheme=QSettings().value("theme/id");install(themePath);check(QSettings().value("theme/id")==beforeTheme,"theme is not applied");
 auto language=QJsonObject{{"format","vlt-language-pack"},{"schemaVersion",1},{"locale","de"},{"languageName","Deutsch"},{"translations",QJsonObject{}}};
 auto languagePath=write("de.vltlang.json",QJsonDocument(language).toJson());const auto beforeLocale=ui::LocalizationManager::instance().activeLocale();install(languagePath);check(ui::LocalizationManager::instance().activeLocale()==beforeLocale,"language is not applied");
 language["author"]="Updated";auto newer=write("de-new.vltlang.json",QJsonDocument(language).toJson());auto conflict=ui::VltContentInstaller::install(newer,metadata(newer));check(!conflict.ok&&conflict.languageConflict,"language replacement requires explicit action");check(ui::VltContentInstaller::install(newer,metadata(newer),{},true).ok,"explicit language replacement");
 daw::ProjectModel project;const auto projectPath=root.path()+"/project.vlt";check(bool(daw::ProjectSerializer::saveDocument(project,projectPath.toStdString(),daw::MediaPaths::Basenames)),"project fixture");install(projectPath);auto templateResult=install(projectPath,metadata(projectPath,"template.vltt"));check(QFileInfo(templateResult.path).isDir(),"template becomes package");
 const auto package=root.path()+"/saved";check(bool(daw::ProjectSerializer::save(project,package.toStdString())),"saved package fixture");QDir().mkpath(package+"/State");QFile state(package+"/State/opaque.bin");state.open(QIODevice::WriteOnly);state.write("plugin state");state.close();
 const auto zip=root.path()+"/shared.vlt.zip";check(ui::VltContentInstaller::exportPackage(package,zip,error),error);auto zipped=install(zip);check(QFileInfo::exists(zipped.path+"/State/opaque.bin"),"package keeps State");install(zip,metadata(zip,"shared.vltt.zip"));
 for(const auto& path:{mini,creatorPath,stripPath,slicerPath,themePath,languagePath,projectPath,zip}) {
  auto meta=metadata(path);meta["sha256"]=QString(64,'0');check(!ui::VltContentInstaller::install(path,meta).ok,"damaged file rejected");
  check(!ui::VltContentInstaller::install(path,metadata(path),[]{return false;}).ok,"cancelled installation rejected");
 }
 for(const auto& suffix:{"vltmini","vltnode","vltcreator","vlts","vltmorph","vltslicer","vlttheme","vltlang.json","vlt","vlt.zip","vltt.zip"}) {
  const auto broken=write(QString("broken.")+suffix,"not a valid native document");check(!ui::VltContentInstaller::install(broken,metadata(broken)).ok,QString("native parser rejects corrupt ")+suffix);
 }
 auto future=metadata(mini);future["minimum_version"]="99.0.0";check(!ui::VltContentInstaller::install(mini,future).ok,"future version rejected");check(ui::VltContentInstaller::resourceType("x.vltfuture").isEmpty(),"future extension falls back to regular download");
 for(const auto& bad:{"../escape","/root","C:/escape","folder/../escape","a\\b","CON.txt","a/"})check(!ui::vltarchive::safeEntryPath(bad),QString("unsafe path ")+bad);
 for(bool link:{false,true}) {
  auto bad=root.path()+QString(link?"/link.zip":"/traversal.zip");archive* a=archive_write_new();archive_write_set_format_zip(a);archive_write_open_filename(a,QFile::encodeName(bad));archive_entry* e=archive_entry_new();archive_entry_set_pathname(e,link?"link":"../escaped");archive_entry_set_filetype(e,link?AE_IFLNK:AE_IFREG);archive_entry_set_perm(e,0644);archive_entry_set_size(e,0);if(link)archive_entry_set_symlink(e,"/tmp");archive_write_header(a,e);archive_entry_free(e);archive_write_close(a);archive_write_free(a);QTemporaryDir staging;check(!ui::vltarchive::extract(bad,staging.path(),error),"unsafe ZIP rejected");
 }
 const QUrl origin("https://forum.vltstudio.ru"), good("https://forum.vltstudio.ru/api/v1/forum/attachments/12345678-1234-1234-1234-123456789abc/download");check(!ui::VltContentInstaller::attachmentId(good,origin).isEmpty(),"trusted route");check(ui::VltContentInstaller::attachmentId(QUrl("https://evil.test"+good.path()),origin).isEmpty(),"untrusted route ignored");
 fprintf(stderr,"Forum installer: %d failure(s)\n",failures);return failures?1:0;
}
