#include "McpService.hpp"
#include "MainWindow.hpp"
#include "CreatorWindow.hpp"
#include "LuaCreatorWindow.hpp"
#include "MiniModuleLibrary.hpp"
#include "RenderWorker.hpp"
#include "AudioAnalysis.hpp"
#include "ai/ContentCatalog.hpp"
#include "Creator/CreatorCompilerClient.hpp"
#include "Api.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "ScriptRun.hpp"
#include <QAbstractButton>
#include <QTabWidget>
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QBuffer>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <sndfile.h>
#include <cmath>
#include <algorithm>

namespace ui {
namespace {
using Json=nlohmann::json;
Json metrics(const QString &path,const std::atomic<bool> &cancel) {
    SF_INFO info{};
#ifdef Q_OS_WIN
    auto *file=sf_wchar_open(reinterpret_cast<const wchar_t*>(path.utf16()),SFM_READ,&info);
#else
    auto *file=sf_open(path.toUtf8().constData(),SFM_READ,&info);
#endif
    if(!file) throw std::runtime_error("Cannot read rendered audio");
    std::unique_ptr<SNDFILE,decltype(&sf_close)> close(file,&sf_close);
    if(info.channels<1||info.channels>64||info.samplerate<1) throw std::runtime_error("Invalid audio format");
    daw::analysis::MetricsAccumulator accumulator(info.samplerate,info.channels);
    std::vector<float> interleaved(4096*size_t(info.channels)),planar(interleaved.size());
    std::vector<const float*> channels(size_t(info.channels));
    for(int c=0;c<info.channels;++c) channels[size_t(c)]=planar.data()+size_t(c)*4096;
    sf_count_t count;
    while((count=sf_readf_float(file,interleaved.data(),4096))>0) {
        if(cancel) throw std::runtime_error("Cancelled");
        for(int c=0;c<info.channels;++c) for(sf_count_t f=0;f<count;++f) {
            const float sample=interleaved[size_t(f)*info.channels+c];
            if(!std::isfinite(sample)) throw std::runtime_error("DSP produced non-finite audio");
            planar[size_t(c)*4096+size_t(f)]=sample;
        }
        accumulator.add(channels.data(),size_t(count));
    }
    if(sf_error(file)!=SF_ERR_NO_ERROR) throw std::runtime_error("Error reading rendered audio");
    auto m=accumulator.result();
    return {{"seconds",m.seconds},{"sampleRate",info.samplerate},{"channels",info.channels},{"frames",info.frames},
        {"silent",m.silent},{"peakDb",m.peakDb},{"rmsDb",m.rmsDb},{"headroomDb",-m.peakDb},
        {"clippedSamples",m.clipped},{"lowShare",m.lowFraction},{"midShare",m.midFraction},{"highShare",m.highFraction}};
}
QString artifactPath(const char *extension) {
    const auto directory=QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/mcp-artifacts";
    if(!QDir().mkpath(directory)) throw std::runtime_error("Cannot create artifact directory");
    return directory+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces)+extension;
}
void normalizeFile(const QString &source,const QString &destination,double peakDb,const std::atomic<bool> &cancel) {
    SF_INFO info{};
#ifdef Q_OS_WIN
    auto *input=sf_wchar_open(reinterpret_cast<const wchar_t*>(source.utf16()),SFM_READ,&info);
#else
    auto *input=sf_open(source.toUtf8().constData(),SFM_READ,&info);
#endif
    if(!input) throw std::runtime_error("Cannot open normalization input");
    std::unique_ptr<SNDFILE,decltype(&sf_close)> closeInput(input,&sf_close);
    info.format=SF_FORMAT_WAV|SF_FORMAT_FLOAT;
#ifdef Q_OS_WIN
    auto *output=sf_wchar_open(reinterpret_cast<const wchar_t*>(destination.utf16()),SFM_WRITE,&info);
#else
    auto *output=sf_open(destination.toUtf8().constData(),SFM_WRITE,&info);
#endif
    if(!output) throw std::runtime_error("Cannot open normalization output");
    std::unique_ptr<SNDFILE,decltype(&sf_close)> closeOutput(output,&sf_close);
    const double gain=peakDb<=-119?1:std::pow(10.0,(-1.0-peakDb)/20.0);
    std::vector<float> block(4096*size_t(info.channels));sf_count_t frames;
    while((frames=sf_readf_float(input,block.data(),4096))>0) {
        if(cancel) throw std::runtime_error("Cancelled");
        for(size_t i=0;i<size_t(frames)*info.channels;++i) block[i]=float(block[i]*gain);
        if(sf_writef_float(output,block.data(),frames)!=frames) throw std::runtime_error("Cannot write normalized output");
    }
    if(sf_error(input)!=SF_ERR_NO_ERROR) throw std::runtime_error("Cannot read normalization input");
    if(sf_close(closeOutput.release())!=0) throw std::runtime_error("Cannot finish normalized output");
}
Json scanStatus(const daw::PluginManager &manager) {
    auto s=manager.scanSnapshot(); Json results=Json::array();
    for(const auto &r:s.results) results.push_back({{"path",r.path},{"uid",r.uid},{"name",r.name},{"reason",r.reason},{"state",int(r.state)}});
    return {{"running",manager.isScanning()},{"phase",int(s.phase)},{"progress",manager.scanProgress()},
        {"passed",s.passed},{"failed",s.failed},{"filesTotal",s.filesTotal},{"filesCompleted",s.filesCompleted},
        {"results",results},{"error",s.error}};
}
}
McpService::Json McpService::render(Client &client,ProjectSession &project,const Json &args) {
    const auto path=args.contains("path")?QString::fromStdString(args["path"]):artifactPath(".wav");
    if(!QFileInfo(path).isAbsolute()||!path.endsWith(".wav",Qt::CaseInsensitive)) throw std::runtime_error("An absolute .wav output path is required");
    if(QFileInfo::exists(path)) throw std::runtime_error("Output already exists; choose a new output path");
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Cannot create output directory");
    auto staging=std::make_shared<QTemporaryDir>(QFileInfo(path).absolutePath()+"/.vlt-render-XXXXXX");
    if(!staging->isValid()) throw std::runtime_error("Cannot create render staging directory");
    daw::rendering::Spec spec; spec.outputDir=staging->path().toStdString(); spec.baseName="preview";
    spec.sampleRate=args.value("sampleRate",0.0);
    if(args.contains("startSeconds")!=args.contains("endSeconds")) throw std::runtime_error("Both range endpoints are required");
    if(args.contains("startSeconds")) {
        spec.range=daw::rendering::Range::Custom;
        spec.customStartSeconds=args["startSeconds"]; spec.customEndSeconds=args["endSeconds"];
        if(spec.customEndSeconds<=spec.customStartSeconds) throw std::runtime_error("Render end must follow start");
    }
    if(args.contains("channelId")) {
        spec.soloChannelId=args["channelId"];
        if(spec.soloChannelId!="master"&&!project.controller.project().findTrack(spec.soloChannelId)) throw std::runtime_error("Unknown channel");
        if(args.value("toBar",0.0)>0) {
            spec.range=daw::rendering::Range::Custom;
            spec.customStartSeconds=daw::ai::barsToSeconds(project.controller,args.value("fromBar",1.0)-1);
            spec.customEndSeconds=daw::ai::barsToSeconds(project.controller,args.at("toBar").get<double>()-1);
        }
    }
    daw::RenderSessionSpec session;
    const auto captured=project.controller.captureRenderSession(spec,session);
    if(!captured) throw std::runtime_error(captured.message());
    auto job=newJob(client,&project,"render");
    const auto revision=project.controller.projectRevision();
    const bool normalize=args.value("normalize",false);
    job->future=std::async(std::launch::async,[job,session,staging,revision,normalize] {
        daw::rendering::Report report;
        auto rendered=daw::RenderWorker::render(session,[job](const auto &progress){job->progress=progress.fraction*.9;return !job->cancel.load();},report);
        if(!rendered) throw std::runtime_error(rendered.message());
        if(job->cancel||report.cancelled) throw std::runtime_error("Cancelled");
        if(report.files.empty()) throw std::runtime_error("Renderer produced no audio");
        const auto file=QString::fromStdString(report.files.front());
        auto analysis=metrics(file,job->cancel);
        if(normalize) {
            const auto normalized=staging->filePath("normalized.wav");
            normalizeFile(file,normalized,analysis.at("peakDb"),job->cancel);
            if(job->cancel) throw std::runtime_error("Cancelled");
            analysis=metrics(normalized,job->cancel); analysis["_staged"]=normalized.toStdString();
        } else analysis["_staged"]=file.toStdString();
        analysis["snapshotRevision"]=revision; return analysis;
    });
    job->publish=[staging,path](Json result) {
        const auto source=QString::fromStdString(result.at("_staged"));
        if(QFileInfo::exists(path)||!QFile::rename(source,path)) throw std::runtime_error("Cannot publish output (destination may now exist)");
        result.erase("_staged"); result["path"]=path.toStdString(); return result;
    };
    return jobStatus(*job);
}
McpService::Json McpService::analyze(Client &client,ProjectSession &project,const std::string &name,const Json &args) {
    auto job=newJob(client,&project,QString::fromStdString(name));
    if(name=="analyze_sample") {
        std::string path=args.value("path","");
        if(path.empty()&&args.contains("contentId")) path=client.catalog->resolvePath(args["contentId"]).value_or("");
        if(path.empty()||!QFileInfo(QString::fromStdString(path)).isAbsolute()) { cancelJob(job); throw std::runtime_error("Supply an absolute path or valid contentId"); }
        job->future=std::async(std::launch::async,[job,path] {
            audio::platform::DecodedAudio audio;
            audio::platform::DecodeOptions options;options.keepGoing=[job]{return !job->cancel.load();};
            const auto decoded=audio::platform::decodeAudioFile(path,audio,options);
            if(!decoded) throw std::runtime_error(decoded.message());
            if(job->cancel) throw std::runtime_error("Cancelled");
            std::vector<std::vector<float>> planar(audio.channels,std::vector<float>(size_t(audio.frames)));
            std::vector<const float*> channels;
            for(size_t c=0;c<audio.channels;++c) {
                for(size_t f=0;f<audio.frames;++f) planar[c][f]=audio.interleaved[f*audio.channels+c];
                channels.push_back(planar[c].data());
            }
            const auto traits=daw::analysis::describe(channels.data(),int(audio.channels),size_t(audio.frames),audio.sampleRate);
            return Json{{"seconds",traits.level.seconds},{"peakDb",traits.level.peakDb},{"rmsDb",traits.level.rmsDb},
                {"lowShare",traits.level.lowFraction},{"midShare",traits.level.midFraction},{"highShare",traits.level.highFraction},
                {"character",traits.character},{"attackSeconds",traits.attackSeconds},{"tonal",traits.tonal},{"pitch",traits.pitch},{"pitchHz",traits.pitchHz}};
        });
    } else {
        auto staging=std::make_shared<QTemporaryDir>();
        std::vector<std::pair<Json,daw::RenderSessionSpec>> sessions;
        const auto captureChannel=[&](const std::string &id,const std::string &label) {
            daw::rendering::Spec spec;spec.outputDir=staging->path().toStdString();spec.baseName=std::to_string(sessions.size());
            spec.soloChannelId=id;
            if(args.value("fromBar",1.0)>1||args.value("toBar",0.0)>0) {
                spec.range=daw::rendering::Range::Custom;
                spec.customStartSeconds=daw::ai::barsToSeconds(project.controller,args.value("fromBar",1.0)-1);
                spec.customEndSeconds=args.value("toBar",0.0)>0?daw::ai::barsToSeconds(project.controller,args.at("toBar").get<double>()-1):project.controller.durationSeconds();
            }
            daw::RenderSessionSpec session;auto result=project.controller.captureRenderSession(spec,session);
            if(!result) throw std::runtime_error(result.message());
            sessions.push_back({{{"channelId",id},{"name",label}},session});
        };
        try {
            if(name=="analyze_track") {
                const auto id=args.at("channelId").get<std::string>();
                const auto *track=project.controller.project().findTrack(id);
                if(!track&&id!="master") throw std::runtime_error("Unknown channel");
                captureChannel(id,track?track->name:"Master");
            } else {
                captureChannel("master","Master");
                for(const auto &track:project.controller.project().tracks)
                    if(track.kind!=daw::TrackKind::Automation&&(track.kind!=daw::TrackKind::Folder||track.summing)) captureChannel(track.id,track.name);
            }
        } catch(...) { cancelJob(job); throw; }
        job->future=std::async(std::launch::async,[job,sessions=std::move(sessions),staging,name] {
            Json values=Json::array();
            for(size_t i=0;i<sessions.size();++i) {
                daw::rendering::Report report;
                const auto result=daw::RenderWorker::render(sessions[i].second,[job,i,count=sessions.size()](const auto &p){job->progress=(double(i)+p.fraction)/count;return !job->cancel.load();},report);
                if(job->cancel||report.cancelled) throw std::runtime_error("Cancelled");
                auto value=sessions[i].first;
                if(!result) value["error"]=result.message();
                else if(report.files.empty()) value["error"]="Renderer produced no files";
                else value.update(metrics(QString::fromStdString(report.files.front()),job->cancel));
                values.push_back(value);
            }
            if(name=="analyze_track") return values.at(0);
            auto master=values.at(0);values.erase(values.begin());return Json{{"master",master},{"channels",values}};
        });
    }
    return jobStatus(*job);
}
McpService::Json McpService::plugins(Client &client,const Json &args) {
    auto &project=resolve(args,false); auto &manager=m_window.m_projects.plugins;
    const auto action=args.at("action").get<std::string>();
    if(action=="list") {
        auto result=daw::ai::callTool(project.controller,"list_plugins",{{"kind","all"}},context(client,project)).toJson();
        result["modules"]=Json::array();
        for(const auto &module:MiniModuleLibrary::entries()) result["modules"].push_back({{"id",module.definition.id},{"name",module.definition.name},{"path",module.path.toStdString()},{"error",module.error.toStdString()}});
        return result;
    }
    if(action=="paths"||action=="set_paths") {
        const auto name=args.value("format","vst3");
        daw::plugins::Format format=daw::plugins::Format::Unknown;
        if(name=="vst3") format=daw::plugins::Format::Vst3;
        else if(name=="clap") format=daw::plugins::Format::Clap;
        else if(name=="vst") format=daw::plugins::Format::Vst;
        else if(name=="au") format=daw::plugins::Format::AudioUnit;
        else throw std::runtime_error("Unknown scan format");
        if(action=="set_paths") {
            if(manager.isScanning()) throw std::runtime_error("Busy: scan in progress");
            const auto paths=args.at("paths").get<std::vector<std::string>>();
            for(const auto &path:paths) if(!QFileInfo(QString::fromStdString(path)).isAbsolute()||!QFileInfo(QString::fromStdString(path)).isDir()) throw std::runtime_error("Search paths must be existing absolute directories");
            manager.setSearchPaths(format,paths); if(!manager.save()) throw std::runtime_error("Cannot save plugin search paths");
        }
        return {{"format",name},{"paths",manager.searchPaths(format)}};
    }
    if(action=="scan_status") return scanStatus(manager);
    if(action=="add_module"||action=="preset_save"||action=="preset_load") {
        const auto channel=args.at("channelId").get<std::string>();
        if(channel!="master"&&!project.controller.project().findTrack(channel)) throw std::runtime_error("Unknown channel");
        if(project.controller.isTrackEditLocked(channel)) throw std::runtime_error("Channel is locked");
        const auto path=QString::fromStdString(args.at("path"));
        if(!QFileInfo(path).isAbsolute()) throw std::runtime_error("An absolute path is required");
        Json result;
        if(action=="add_module") {
            const auto module=MiniModuleLibrary::read(path);
            if(!module.error.isEmpty()) throw std::runtime_error(module.error.toStdString());
            const auto id=project.controller.addMiniModule(channel,module.definition);
            if(id.empty()) throw std::runtime_error("Module could not be added");
            result={{"insertId",id}};
        } else {
            const auto outcome=action=="preset_save"?project.controller.saveChannelStripPreset(channel,path.toStdString()):project.controller.applyChannelStripPreset(channel,path.toStdString());
            if(!outcome) throw std::runtime_error(outcome.message());
            result={{"path",path.toStdString()}};
        }
        if(action!="preset_save") edited(project);
        result.update(stamp(project)); return result;
    }
    if(action=="scan") {
        if(manager.isScanning()) throw std::runtime_error("Busy: plugin scan already running");
        auto job=newJob(client,&project,"plugin_scan");
        manager.startScan(args.value("rescan",false));
        job->stop=[this] { m_window.m_projects.plugins.cancelScan(); };
        job->poll=[this,job] { auto &manager=m_window.m_projects.plugins; job->progress=manager.scanProgress(); job->result=scanStatus(manager); return !manager.isScanning(); };
        return jobStatus(*job);
    }
    if(action=="install_module") {
        const auto path=QString::fromStdString(args.at("path"));
        if(!QFileInfo(path).isAbsolute()||!path.endsWith(".vltmini",Qt::CaseInsensitive)) throw std::runtime_error("An absolute .vltmini path is required");
        const auto parsed=MiniModuleLibrary::read(path);
        if(!parsed.error.isEmpty()) throw std::runtime_error(parsed.error.toStdString());
        const auto id=QString::fromStdString(parsed.definition.id);
        if(!QRegularExpression("^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,127}$").match(id).hasMatch()) throw std::runtime_error("Invalid module identity");
        const auto destination=MiniModuleLibrary::directory()+"/"+id+".vltmini";
        QString error; if(!MiniModuleLibrary::write(destination,parsed.definition,error)) throw std::runtime_error(error.toStdString());
        return {{"installed",true},{"module",parsed.definition.id},{"path",destination.toStdString()}};
    }
    throw std::runtime_error("Unknown plugin_library action");
}
ScriptRun *McpService::ownedScript(Client &client,ProjectSession &project,const QString &id) {
    bool owned=false;
    for(const auto &job:m_jobs)
        if(job->owner==client.id&&job->project==project.id&&job->scriptRun==id) { owned=true; break; }
    if(owned) for(const auto &run:m_window.m_scripts->m_runs)
        if(run->id==id&&run->projectId==project.id&&!run->complete&&run->generation==project.controller.projectGeneration())
            return run.get();
    throw std::runtime_error("Unknown live script run for this connection/project");
}
McpService::Json McpService::scriptUi(Client &client,ProjectSession &project,const Json &args) {
    auto *run=ownedScript(client,project,QString::fromStdString(args.at("runId")));
    const auto action=args.at("action").get<std::string>();
    if(action=="list") {
        Json widgets=Json::array(),windows=Json::array();
        auto ids=run->widgets.keys(); ids.sort();
        for(const auto &id:ids) if(auto *widget=run->widgets.value(id).data())
            widgets.push_back({{"id",id.toStdString()},{"type",widget->property("vlt.type").toString().toStdString()},
                {"label",widget->accessibleName().toStdString()},{"visible",widget->isVisible()},{"enabled",widget->isEnabled()}});
        ids=run->windows.keys(); ids.sort();
        for(const auto &id:ids) if(auto *window=run->windows.value(id).data())
            windows.push_back({{"id",id.toStdString()},{"title",window->windowTitle().toStdString()},{"visible",window->isVisible()}});
        return {{"windows",windows},{"widgets",widgets},{"idle",run->idle}};
    }
    const auto id=QString::fromStdString(args.at("widgetId"));
    auto *widget=run->widgets.value(id,run->windows.value(id)).data();
    if(!widget) throw std::runtime_error("Unknown widget in this script run");
    if(action=="get") return m_window.m_scripts->uiCall(run,"ui.get",{{"id",id.toStdString()}});
    if(project.id!=m_window.m_projects.selected) throw std::runtime_error("Select the pinned project before operating its script UI");
    if(!run->idle) throw std::runtime_error("Busy: script callback is still running");
    if(!widget->isVisible()||!widget->isEnabled()) throw std::runtime_error("Script widget is hidden or disabled");
    if(action=="click") {
        auto *button=qobject_cast<QAbstractButton*>(widget);
        if(!button) throw std::runtime_error("click requires a native script button or toggle");
        button->click();
    } else if(action=="tab") {
        auto *tabs=qobject_cast<QTabWidget*>(widget);
        const auto index=args.at("index").get<int>();
        if(!tabs||index<1||index>tabs->count()||!tabs->isTabEnabled(index-1)) throw std::runtime_error("Invalid script tab index");
        tabs->setCurrentIndex(index-1);
    } else if(action=="pointer"||action=="key") {
        if(widget->property("vlt.type").toString()!="canvas") throw std::runtime_error("pointer/key require a script canvas");
        if(action=="pointer") {
            const double x=args.at("x"),y=args.at("y");
            if(!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1) throw std::invalid_argument("Canvas coordinates must be between zero and one");
            const QPointF local(x*std::max(0,widget->width()-1),y*std::max(0,widget->height()-1));
            const QPointF global=widget->mapToGlobal(local.toPoint());
            QMouseEvent down(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QMouseEvent up(QEvent::MouseButtonRelease,local,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(widget,&down);QApplication::sendEvent(widget,&up);
        } else {
            static const QHash<QString,int> keys{{"Left",Qt::Key_Left},{"Right",Qt::Key_Right},{"Up",Qt::Key_Up},
                {"Down",Qt::Key_Down},{"Space",Qt::Key_Space},{"Return",Qt::Key_Return},{"R",Qt::Key_R}};
            const auto name=QString::fromStdString(args.at("key"));
            if(!keys.contains(name)) throw std::invalid_argument("Unsupported canvas key");
            QKeyEvent event(QEvent::KeyPress,keys.value(name),Qt::NoModifier);
            QApplication::sendEvent(widget,&event);
        }
    } else throw std::runtime_error("Unknown script_ui action");
    return {{"accepted",true},{"widgetId",id.toStdString()}};
}
McpService::Json McpService::capture(Client &client,ProjectSession &project,const Json &args) {
    QWidget *window=&m_window; const auto view=args.value("view","main");
    if(view=="creator"||view=="lua") {
        const auto id=QString::fromStdString(args.at("documentId"));
        window=view=="creator"?static_cast<QWidget*>(m_creators.value(id).data()):static_cast<QWidget*>(m_lua.value(id).data());
        if(!window||window->property("vlt.documentId").toString()!=id||window->property("vlt.projectId").toString()!=project.id) throw std::runtime_error("Unknown document for capture");
    } else if(view=="script") {
        auto *run=ownedScript(client,project,QString::fromStdString(args.at("runId")));
        window=run->windows.value(QString::fromStdString(args.at("windowId"))).data();
        if(!window) throw std::runtime_error("Unknown window in this script run");
    } else if(view!="main") throw std::runtime_error("Unknown view");
    else if(project.id!=m_window.m_projects.selected) throw std::runtime_error("Select the project before capturing the main window");
    if(!window->isVisible()) throw std::runtime_error("Show the editor before capture");
    auto pixmap=window->grab().toImage();
    bool gpu=false;
    // Native Quick surfaces are absent from QWidget's backing-store readback.
    // Reuse the explicit project-preview capture, including pending UI changes.
    // Never include a different top-level editor found through QObject ownership.
    for(auto *surface:window->findChildren<graphics::WorkspaceSurface*>()) {
        auto *source=qobject_cast<QWidget*>(surface->parent());
        if(!source||!source->isVisible()||source->window()!=window->window()) continue;
        const auto scene=surface->grabImage();
        if(scene.isNull()) throw std::runtime_error("GPU window is not exposed; show the window before capturing it");
        QPainter painter(&pixmap);
        painter.drawImage(QRect(source->mapTo(window,QPoint()),source->size()),scene);
        gpu=true;
    }
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    if(pixmap.isNull()||!pixmap.save(&buffer,"PNG")) throw std::runtime_error("Cannot capture window");
    if(bytes.size()>3*1024*1024) throw std::runtime_error("Capture exceeds 3 MiB; reduce the window size");
    const auto path=args.contains("path")?QString::fromStdString(args["path"]):artifactPath(".png");
    if(!QFileInfo(path).isAbsolute()) throw std::runtime_error("An absolute output path is required");
    QSaveFile output(path);
    if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit()) throw std::runtime_error("Cannot save screenshot");
    return {{"path",path.toStdString()},{"width",pixmap.width()},{"height",pixmap.height()},{"rendering",gpu?"gpu":"widgets"},
        {"image",{{"type","image"},{"mimeType","image/png"},{"data",bytes.toBase64().toStdString()}}}};
}
McpService::Json McpService::resources() const {
    Json items=Json::array();
    const auto add=[&](const std::string &uri,const std::string &name,const char *mime) { items.push_back({{"uri",uri},{"name",name},{"mimeType",mime}}); };
    add("vltone://guide","External agent guide","text/markdown");
    add("vltone://lua/api","Lua SDK API","application/json");
    for(const auto &book:daw::ai::builtinPrompts().playbooks) add("vltone://playbooks/"+book.id,book.title,"text/markdown");
    const std::pair<QString,QString> roots[]={
        {"creator",QString::fromStdWString(daw::plugins::mini::creatorToolsDirectory().wstring())+"/sdk"},
        {"lua",QDir(QCoreApplication::applicationDirPath()).filePath("ScriptSDK")}};
    for(const auto &[kind,root]:roots) {
        QDirIterator files(root,{"*.md","*.lua","*.cpp","*.hpp","*.json"},QDir::Files,QDirIterator::Subdirectories);
        while(files.hasNext() && items.size()<512) {
            files.next(); const auto relative=QDir(root).relativeFilePath(files.filePath());
            if(files.fileInfo().isSymLink()||files.fileInfo().size()>2*1024*1024) continue;
            add(("vltone://"+kind+"/files/"+relative).toStdString(),relative.toStdString(),"text/plain");
        }
    }
    return items;
}
McpService::Json McpService::readResource(const std::string &uri) const {
    const auto entries=resources();
    const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto &item){return item["uri"]==uri;});
    if(found==entries.end()) throw std::invalid_argument("Unknown resource URI");
    std::string body;
    if(uri=="vltone://guide") body="Start with workspace_projects/list. Every project call uses projectId; writes require generation and projectRevision from the latest result. Creator/Lua use independent documentId and revision. Create/open never replaces another editor. Tool edits are one Undo step each. Music AI tools use bars; Script SDK project_edit uses beats. Long operations return jobId; poll get_job and inspect state, error and result. Re-read state after a conflict. Lua uses only declared DAW/file permissions with explicit fileAccess, and has no network/process access. Use direct paths; dialogs are unavailable. render_preview produces measured WAV artifacts. capture_view captures real VLTONE windows. Disconnect cancels unfinished work; finished edits remain. Read the relevant SDK and playbooks before composing or writing code.";
    else if(uri=="vltone://lua/api") body=vlt::script::apiCatalog().dump(2);
    else if(uri.starts_with("vltone://playbooks/")) body=daw::ai::findPlaybook(daw::ai::builtinPrompts(),uri.substr(std::string("vltone://playbooks/").size()))->body;
    else {
        const bool creator=uri.starts_with("vltone://creator/files/");
        const auto root=creator?QString::fromStdWString(daw::plugins::mini::creatorToolsDirectory().wstring())+"/sdk":QDir(QCoreApplication::applicationDirPath()).filePath("ScriptSDK");
        const auto prefix=creator?std::string("vltone://creator/files/"):std::string("vltone://lua/files/");
        const auto path=QFileInfo(QDir(root).filePath(QString::fromStdString(uri.substr(prefix.size())))).canonicalFilePath();
        if(path.isEmpty()||!path.startsWith(QFileInfo(root).canonicalFilePath()+"/")) throw std::runtime_error("Resource escaped SDK root");
        QFile file(path); if(!file.open(QIODevice::ReadOnly)||file.size()>2*1024*1024) throw std::runtime_error("Cannot read SDK resource");
        body=file.readAll().toStdString();
    }
    return {{"contents",Json::array({{{"uri",uri},{"mimeType",(*found)["mimeType"]},{"text",body}}})}};
}
}
