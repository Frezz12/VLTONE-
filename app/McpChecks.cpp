#include "McpService.hpp"
#include "MainWindow.hpp"
#include "CreatorWindow.hpp"
#include "CreatorAiWorkspace.hpp"
#include "LuaCreatorWindow.hpp"
#include "McpSettingsPage.hpp"
#include "LocalizationManager.hpp"
#include "ScriptService.hpp"
#include "Theme.hpp"
#include "ShortcutManager.hpp"
#include "Internal/MiniModuleDefinition.hpp"
#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>
#include <QUuid>
#include <cmath>
#include <cstdio>
#include <sndfile.h>

namespace ui {
namespace {
using Json=nlohmann::json;
void require(bool value,const std::string &message) {
    if(!value) throw std::runtime_error(message);
    std::fprintf(stderr,"PASS MCP %s\n",message.c_str());
}
bool until(const std::function<bool()> &done,int timeout=15000) {
    QElapsedTimer timer; timer.start();
    while(!done()&&timer.elapsed()<timeout) { QApplication::processEvents(); QThread::msleep(2); }
    return done();
}
class Bridge {
public:
    QProcess process;
    int sequence=0;
    QByteArray pending;
    Bridge(const QString &endpoint,const QString &token) {
        auto env=QProcessEnvironment::systemEnvironment(); env.insert("VLTONE_MCP_TOKEN",token);
        process.setProcessEnvironment(env);
        auto binary=QCoreApplication::applicationDirPath()+"/vlt_mcp";
#ifdef Q_OS_WIN
        binary+=".exe";
#endif
        process.start(binary,{"--endpoint",endpoint});
        if(!process.waitForStarted(3000)) throw std::runtime_error("Cannot start vlt_mcp");
    }
    ~Bridge() { process.closeWriteChannel(); until([&]{return process.state()==QProcess::NotRunning;},5000); if(process.state()!=QProcess::NotRunning) {process.kill(); process.waitForFinished(1000);} }
    Json next() {
        if(!until([&] {pending+=process.readAllStandardOutput();return pending.contains('\n')||process.state()==QProcess::NotRunning;}))
            throw std::runtime_error("MCP response timed out");
        auto at=pending.indexOf('\n');
        if(at<0) throw std::runtime_error("Bridge exited: "+process.readAllStandardError().toStdString());
        auto bytes=pending.left(at); pending.remove(0,at+1); return Json::parse(bytes.toStdString());
    }
    Json request(const std::string &method,Json params=Json::object()) {
        auto value=Json{{"jsonrpc","2.0"},{"id",++sequence},{"method",method},{"params",params}}.dump()+"\n";
        process.write(QByteArray::fromStdString(value)); auto response=next();
        if(response.value("id",Json())!=sequence) throw std::runtime_error("MCP response id mismatch");
        return response;
    }
    void initialize() {
        auto response=request("initialize",{{"protocolVersion",vlt::mcp::legacyVersion},{"capabilities",Json::object()},{"clientInfo",{{"name","integration"},{"version","1"}}}});
        if(response.contains("error")) throw std::runtime_error(response.dump());
        process.write("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    }
    Json tool(const std::string &name,Json args=Json::object(),bool ok=true) {
        auto response=request("tools/call",{{"name",name},{"arguments",args}});
        if(response.contains("error")) throw std::runtime_error(name+": "+response.dump());
        auto value=response.at("result").at("structuredContent");
        if(ok&&!value.value("ok",false)) throw std::runtime_error(name+": "+value.dump());
        return value;
    }
    Json scope(const Json &project) {
        auto state=tool("get_project",{{"projectId",project.at("projectId")}});
        return {{"projectId",state.at("projectId")},{"generation",state.at("generation")},{"projectRevision",state.at("projectRevision")}};
    }
    Json edit(const Json &project,const std::string &name,Json args) { args.update(scope(project)); return tool(name,args); }
    Json done(const Json &job,bool ok=true) {
        Json value; QElapsedTimer timer; timer.start();
        do {
            value=tool("get_job",{{"jobId",job.at("jobId")}});
            if(value["state"]!="running"&&value["state"]!="cancelling") break;
            QApplication::processEvents(); QThread::msleep(10);
        } while(timer.elapsed()<90000);
        if(ok&&value["state"]!="completed") throw std::runtime_error("Job failed: "+value.dump());
        return value;
    }
};
void fixtureTone(const QString &path,bool drum) {
    SF_INFO info{}; info.channels=1;info.samplerate=48000;info.format=SF_FORMAT_WAV|SF_FORMAT_FLOAT;
#ifdef Q_OS_WIN
    auto *file=sf_wchar_open(reinterpret_cast<const wchar_t*>(path.utf16()),SFM_WRITE,&info);
#else
    auto *file=sf_open(path.toUtf8().constData(),SFM_WRITE,&info);
#endif
    if(!file) throw std::runtime_error("Cannot write fixture audio");
    std::vector<float> samples(24000);
    for(size_t n=0;n<samples.size();++n) { const double t=double(n)/48000; samples[n]=float(.2*std::sin(2*3.141592653589793*(drum?70:261.6256)*t)*std::exp(-t*(drum?18:3))); }
    sf_writef_float(file,samples.data(),sf_count_t(samples.size()));sf_close(file);
}
}
bool McpService::runCheck(MainWindow &window,const QString &directory) try {
    auto &service=*window.m_mcp;
    service.setEnabled(false);
    service.m_endpoint="vltone-mcp-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    service.setEnabled(true); require(service.listening(),"same-user local endpoint starts");
    QTemporaryDir temporary;
    require(temporary.isValid(),"isolated fixture directory");
    qputenv("VLT_SCRIPT_LIBRARY",temporary.filePath("scripts").toUtf8());
    window.resize(1280,800); window.show(); QApplication::processEvents();
    Bridge client(service.m_endpoint,service.m_token); client.initialize();
    auto tools=client.request("tools/list").at("result").at("tools");
    require(tools.size()>70,"legacy client discovers full tools through packaged bridge");
    auto resources=client.request("resources/list").at("result").at("resources");
    require(resources.size()>5,"SDK and playbook resources are available");
    require(client.request("resources/read",{{"uri","vltone://guide"}}).at("result").contains("contents"),"resource contents");
    client.process.write("{broken\n"); require(client.next().at("error").at("code")==-32700,"malformed JSON returns parse error without killing connection");
    {
        Bridge modern(service.m_endpoint,service.m_token);
        Json meta={{"io.modelcontextprotocol/protocolVersion",vlt::mcp::modernVersion},{"io.modelcontextprotocol/clientCapabilities",Json::object()}};
        require(modern.request("server/discover",{{"_meta",meta}}).at("result").at("resultType")=="complete","2026 stateless discovery through bridge");
        require(modern.request("tools/list",{{"_meta",meta}}).at("result").at("tools").size()==tools.size(),"2026 tools without initialization");
        require(modern.request("tools/call",{{"_meta",meta},{"name","workspace_projects"},{"arguments",{{"action","list"}}}})
            .at("result").at("structuredContent").at("projects").is_array(),"2026 stateless tool invocation through bridge");
    }
    {
        Bridge denied(service.m_endpoint,"wrong-token");
        require(until([&]{return denied.process.state()==QProcess::NotRunning;})&&denied.process.exitCode()==4,"invalid token rejected");
        require(denied.process.readAllStandardOutput().isEmpty(),"auth errors never pollute MCP stdout");
    }
    auto project=client.tool("workspace_projects",{{"action","create"}});
    auto stale=client.scope(project);
    client.edit(project,"set_tempo",{{"bpm",110}});
    auto staleArgs=stale; staleArgs["bpm"]=90;
    require(client.tool("set_tempo",staleArgs,false).at("code")=="conflict","stale revision refused");
    auto second=client.tool("workspace_projects",{{"action","create"}});
    client.edit(project,"set_tempo",{{"bpm",112}});
    require(window.m_projects.find(QString::fromStdString(project["projectId"]))->controller.tempo()==112&&window.projectController().tempo()!=112,"switching tab never redirects command");
    client.edit(project,"undo",Json::object());
    require(window.m_projects.find(QString::fromStdString(project["projectId"]))->controller.tempo()==110,"Undo reverses one MCP edit, not the connection");
    client.edit(project,"redo",Json::object());
    client.edit(project,"workspace_projects",{{"action","select"}});
    client.edit(project,"workspace_projects",{{"action","audio"},{"enabled",true}});
    auto commands=client.tool("search_commands",{{"projectId",project["projectId"]},{"query","view.toggleMixer"}});
    require(commands.dump().find("view.toggleMixer")!=std::string::npos,"command search uses real application identities");
    const bool mixer=window.m_shortcuts->command("view.toggleMixer")->action->isChecked();
    client.edit(project,"run_command",{{"commandId","view.toggleMixer"}});
    require(window.m_shortcuts->command("view.toggleMixer")->action->isChecked()!=mixer,"external command executes directly without a dialog");
    const auto screenshot=client.request("tools/call",{{"name","capture_view"},{"arguments",{{"projectId",project["projectId"]},{"path",temporary.filePath("window.png").toStdString()}}}}).at("result");
    require(screenshot.at("content").size()==2&&screenshot.at("content")[1].at("type")=="image"&&QFileInfo::exists(temporary.filePath("window.png")),"capture_view returns a real PNG and MCP image content");
    require(screenshot.at("structuredContent").contains("rendering"),"capture_view reports the actual rendering path");
    const auto drum=temporary.filePath("kick.wav"),tone=temporary.filePath("tone.wav"); fixtureTone(drum,true);fixtureTone(tone,false);
    std::vector<std::string> tracks;
    for(int part=0;part<3;++part) {
        const auto track=client.edit(project,"add_track",{{"kind","instrument"},{"name",part==0?"Drums":part==1?"Bass":"Chords"}}).at("trackId").get<std::string>();tracks.push_back(track);
        client.edit(project,"load_sampler",{{"trackId",track},{"path",(part==0?drum:tone).toStdString()}});
        const auto clip=client.edit(project,"add_midi_clip",{{"trackId",track},{"startBar",1},{"lengthBars",2}}).at("clipId");
        Json notes=Json::array();
        for(int beat=0;beat<8;++beat) for(int n=0;n<(part==2?3:1);++n) notes.push_back({{"pitch",part==0?60:part==1?36+(beat/4)*5:60+n*4},
            {"start",double(beat)},{"length",part==0?.2:.8},{"velocity",part==2?55:85}});
        client.edit(project,"set_clip_notes",{{"trackId",track},{"clipId",clip},{"notes",notes}});
    }
    auto plugins=client.tool("list_plugins",{{"projectId",project["projectId"]},{"kind","all"}});
    const auto insert=client.edit(project,"add_insert",{{"channelId",tracks[2]},{"format","internal"},{"uid","daw.delay"}}).at("insertId");
    auto parameters=client.tool("plugin_parameters",{{"projectId",project["projectId"]},{"action","list"},{"track",tracks[2]},{"device",insert}});
    require(!parameters.at("parameters").empty()&&parameters.contains("identity"),"host plugin parameters expose stable identity");
    auto *live=window.m_projects.find(QString::fromStdString(project["projectId"]));
    live->controller.setTracksEditLocked({tracks[2]},true);
    auto locked=client.scope(project);locked.update({{"trackId",tracks[2]},{"volumeDb",-6}});
    require(!client.tool("set_track_mix",locked,false)["ok"].get<bool>(),"locked tracks reject external edits");
    live->controller.setTracksEditLocked({tracks[2]},false);
    const auto trackCount=live->controller.project().tracks.size();
    auto batch=client.scope(project);batch["operations"]=Json::array({{{"op","track.add"},{"kind","midi"},{"name","Rolled back"}},{{"op","unknown.operation"}}});
    require(!client.tool("project_edit",batch,false)["ok"].get<bool>()&&live->controller.project().tracks.size()==trackCount,"failed atomic edit restores the complete project");
    client.edit(project,"automation",{{"action","set_points"},{"channelId",tracks[2]},{"target","volume"},{"points",Json::array({{{"bar",1},{"value",.5}},{{"bar",3},{"value",.7}}})}});
    const auto song=temporary.filePath("Song");
    client.done(client.edit(project,"save_project",{{"path",song.toStdString()}}));
    {
        QFile extra(song+"/user-notes.txt");require(extra.open(QIODevice::WriteOnly),"fixture extra package file");extra.write("keep this file");extra.close();
        client.done(client.edit(project,"save_project",{{"path",song.toStdString()}}));
        require(extra.open(QIODevice::ReadOnly)&&extra.readAll()=="keep this file","saving preserves additional project package files");
    }
    auto rendered=client.done(client.edit(project,"render_preview",{{"path",temporary.filePath("song.wav").toStdString()}}));
    require(rendered.at("result").at("seconds").get<double>()>1&&!rendered.at("result").at("silent").get<bool>(),"drums/bass/chords/effect/automation saved, rendered and measured");
    const auto analysis=client.done(client.tool("analyze_sample",{{"projectId",project["projectId"]},{"path",rendered["result"]["path"]}}));
    require(analysis["result"]["seconds"].get<double>()>1,"background sample analysis reads the actual render");
    require(client.tool("workspace_projects",{{"action","open"},{"path",song.toStdString()}})["alreadyOpen"]==true,"opening the same path reuses its project identity");
    require(client.tool("workspace_projects",{{"action","open"},{"path",(song+"/Song.vlt").toStdString()}})["alreadyOpen"]==true,"a named .vlt manifest reuses the folder's project identity");
    client.done(client.edit(project,"save_project",{{"path",temporary.filePath("Song Copy").toStdString()}}));
    auto opened=client.done(client.tool("workspace_projects",{{"action","open"},{"path",(song+"/Song.vlt").toStdString()}}));
    require(opened["result"].contains("projectId")&&opened["result"]["path"]==song.toStdString(),"saved .vlt file opens its package without a dialog");
    auto foreignSave=client.scope(project);foreignSave.update({{"path",song.toStdString()}});
    require(!client.tool("save_project",foreignSave,false)["ok"].get<bool>(),"saving cannot overwrite another open project");
    client.edit(project,"workspace_projects",{{"action","select"}});
    auto document=client.edit(project,"creator_document",{{"action","create"},{"name","MCP Gain"}});
    auto *creator=service.m_creators.value(QString::fromStdString(document["documentId"])).data();
    creator->m_installDirectory=temporary.filePath("modules"); creator->m_recoveryDirectory=temporary.path();
    const auto docCall=[&](const std::string &name,Json args=Json::object(),bool ok=true) {
        auto current=client.tool("creator_context",{{"projectId",project["projectId"]},{"documentId",document["documentId"]}});
        args["documentId"]=document["documentId"];args["revision"]=current["revision"];args.update(client.scope(project));
        return client.tool(name,args,ok);
    };
    daw::plugins::mini::FunctionDefinition function;
    const std::string source="#include <vlt/creator.hpp>\nVLT_NODE vlt::AudioFrame process(VLT_PORT(\"input\") vlt::AudioFrame input) { return input * 0.5f; }\n";
    function.source=source+"intentional invalid C++;";
    docCall("creator_edit",{{"operations",Json::array({{{"op","add"},{"id","cpp"},{"type","cpp_function"},{"function",daw::plugins::mini::functionToJson(function)}}})}});
    auto failed=client.done(docCall("creator_cpp",{{"node","cpp"},{"action","analyze"}}),false);
    require(failed["state"]=="failed"&&failed["result"].contains("diagnostics"),"Creator compiler returns actual line diagnostics");
    docCall("creator_edit",{{"operations",Json::array({{{"op","source"},{"id","cpp"},{"source",source}}})}});
    client.done(docCall("creator_cpp",{{"node","cpp"},{"action","analyze"}}));
    auto definition=client.tool("creator_source",{{"projectId",project["projectId"]},{"documentId",document["documentId"]},{"node","cpp"}});
    const auto output=definition.at("outputs").at(0).at("id");
    docCall("creator_edit",{{"operations",Json::array({
        {{"op","disconnect"},{"from","input"},{"from_port","out"},{"to","output"},{"to_port","in"}},
        {{"op","connect"},{"from","input"},{"from_port","out"},{"to","cpp"},{"to_port","input"}},
        {{"op","connect"},{"from","cpp"},{"from_port",output},{"to","output"},{"to_port","in"}}})}});
    client.done(docCall("creator_build"));
    const auto module=temporary.filePath("modules/")+QString::fromStdString(creator->project().definition.id)+".vltmini";
    require(QFileInfo::exists(module),"Creator installs a real compiled module");
    client.edit(project,"plugin_library",{{"action","add_module"},{"path",module.toStdString()},{"channelId",tracks[1]}});
    auto building=docCall("creator_build");
    creator->edit("Manual edit",[](auto &p){p.definition.name="Changed during build";});
    require(client.done(building,false)["state"]=="failed","manual edit prevents stale build publication");
    const auto sourcePath=temporary.filePath("gain.vltcreator");
    docCall("creator_document",{{"action","save"},{"path",sourcePath.toStdString()}});
    auto externalChange=docCall("creator_build");
    QFile changedSource(sourcePath);
    require(changedSource.open(QIODevice::Append),"fixture saved Creator source");changedSource.write("\n");changedSource.close();
    require(client.done(externalChange,false)["state"]=="failed","external source changes prevent build from overwriting the saved document");
    require(creator->m_undo->count()>1,"separate MCP edits have separate Undo steps");
    auto luaDoc=client.edit(project,"lua_document",{{"action","create"},{"name","MCP script"}});
    auto luaArgs=[&](Json args) {
        auto context=client.tool("lua_context",{{"projectId",project["projectId"]},{"documentId",luaDoc["documentId"]}});
        args["documentId"]=luaDoc["documentId"];args["revision"]=context["revision"];args.update(client.scope(project));return args;
    };
    const std::string script="local p = vlt.project.get()\nvlt.project.apply({revision=p.revision,label='MCP Lua',operations={{op='track.add',kind='midi',name='Lua created'}}})\nprint('mcp-lua-ok')\nreturn {worked=true}\n";
    client.tool("lua_edit",luaArgs({{"path","main.lua"},{"source",script}}));
    client.done(client.tool("lua_check",luaArgs(Json::object())));
    const auto before=window.m_projects.find(QString::fromStdString(project["projectId"]))->controller.project().tracks.size();
    auto preview=client.done(client.tool("lua_preview",luaArgs(Json::object())));
    require(preview["result"]["error"]==""&&window.m_projects.find(QString::fromStdString(project["projectId"]))->controller.project().tracks.size()==before,"Lua preview edits only the copy and returns log/result");
    client.done(client.tool("lua_install",luaArgs(Json::object())));
    client.done(client.tool("lua_document",luaArgs({{"action","run"}})));
    require(window.m_projects.find(QString::fromStdString(project["projectId"]))->controller.project().tracks.size()==before+1,"Lua installs and runs in the pinned live project");
    client.tool("lua_edit",luaArgs({{"path","main.lua"},{"source",
        "vlt.ui.window{id='probe',title='MCP UI probe',children={{type='tabs',id='pages',children={{type='column',title='First',children={{type='button',id='press',text='Press',onClick=function() vlt.ui.set{id='message',text='clicked'} end},{type='label',id='message',text='ready'},{type='canvas',id='pad',onPointer=function(e) if e.value.phase=='down' then vlt.ui.set{id='message',text='pointer'} end end,onKey=function(e) vlt.ui.set{id='message',text=e.value.key} end}}},{type='column',title='Second',children={{type='label',text='Other page'}}}}}}}\n"}}));
    auto uiJob=client.tool("lua_document",luaArgs({{"action","run"}}));
    const auto uiRun=service.m_jobs.value(QString::fromStdString(uiJob["jobId"]))->scriptRun.toStdString();
    auto uiArgs=Json{{"projectId",project["projectId"]},{"runId",uiRun},{"action","list"}};
    require(until([&]{auto s=client.tool("script_ui",uiArgs); return s.value("idle",false)&&!s["windows"].empty();}),"script window inspection waits for a live UI");
    client.edit(project,"script_ui",{{"action","click"},{"runId",uiRun},{"widgetId","press"}});
    require(until([&]{return client.tool("script_ui",{{"projectId",project["projectId"]},{"runId",uiRun},{"action","get"},{"widgetId","message"}}).value("text","")=="clicked";}),"script_ui clicks the real Lua callback and reads its result");
    client.edit(project,"script_ui",{{"action","pointer"},{"runId",uiRun},{"widgetId","pad"},{"x",.5},{"y",.5}});
    require(until([&]{return client.tool("script_ui",{{"projectId",project["projectId"]},{"runId",uiRun},{"action","get"},{"widgetId","message"}}).value("text","")=="pointer";}),"script_ui delivers a real canvas pointer event");
    require(until([&]{return client.tool("script_ui",uiArgs).value("idle",false);}),"canvas callback becomes idle");
    client.edit(project,"script_ui",{{"action","key"},{"runId",uiRun},{"widgetId","pad"},{"key","Right"}});
    require(until([&]{return client.tool("script_ui",{{"projectId",project["projectId"]},{"runId",uiRun},{"action","get"},{"widgetId","message"}}).value("text","")=="Right";}),"script_ui delivers a real canvas keyboard event");
    auto uiCapture=client.tool("capture_view",{{"projectId",project["projectId"]},{"view","script"},{"runId",uiRun},{"windowId","probe"}});
    require(QFileInfo::exists(QString::fromStdString(uiCapture["path"])),"script window capture returns a real PNG");
    client.edit(project,"script_ui",{{"action","tab"},{"runId",uiRun},{"widgetId","pages"},{"index",2}});
    require(!client.tool("script_ui",[&]{auto a=client.scope(project); a.update({{"action","click"},{"runId",uiRun},{"widgetId","press"}}); return a;}(),false)["ok"].get<bool>(),"hidden script buttons cannot be clicked");
    {
        Bridge other(service.m_endpoint,service.m_token);other.initialize();
        require(!other.tool("script_ui",uiArgs,false)["ok"].get<bool>(),"another client cannot inspect script widgets");
        require(!other.tool("capture_view",{{"projectId",project["projectId"]},{"view","script"},{"runId",uiRun},{"windowId","probe"}},false)["ok"].get<bool>(),"another client cannot capture script windows");
    }
    client.tool("cancel_job",{{"jobId",uiJob["jobId"]}});
    require(!client.tool("script_ui",uiArgs,false)["ok"].get<bool>(),"finished script widgets are unavailable");
    client.tool("lua_edit",luaArgs({{"path","main.lua"},{"source","while true do end\n"}}));
    {
        Bridge secondClient(service.m_endpoint,service.m_token);secondClient.initialize();
        auto pending=client.edit(project,"render_preview",{{"startSeconds",0},{"endSeconds",3600}});
        require(!secondClient.tool("cancel_job",{{"jobId",pending["jobId"]}},false)["ok"].get<bool>(),"clients cannot cancel each other's jobs");
        client.tool("cancel_job",{{"jobId",pending["jobId"]}});
        require(client.done(pending,false)["state"]=="cancelled","render cancellation completes without publishing");
        const auto path=temporary.filePath("disconnected.wav");
        auto disconnected=secondClient.edit(project,"render_preview",{{"path",path.toStdString()},{"startSeconds",0},{"endSeconds",3600}});
        auto running=service.m_jobs.value(QString::fromStdString(disconnected["jobId"]));
        auto scriptJob=secondClient.tool("lua_document",luaArgs({{"action","run"}}));
        auto scriptRun=service.m_jobs.value(QString::fromStdString(scriptJob["jobId"]));
        secondClient.process.closeWriteChannel();
        require(until([&]{return running->complete;},20000)&&running->cancel&&!QFileInfo::exists(path),"client disconnect cancels render without publishing a partial file");
        require(until([&]{return scriptRun->complete;},10000)&&scriptRun->cancel&&window.m_scripts->status(scriptRun->scriptRun).value("complete",false),"client disconnect stops its running Lua worker");
        require(live->controller.project().tracks.size()==before+1,"disconnect preserves completed edits");
    }
    {
        auto closing=client.tool("workspace_projects",{{"action","create"}});
        auto closingDoc=client.edit(closing,"lua_document",{{"action","create"}});
        auto *editor=service.m_lua.value(QString::fromStdString(closingDoc["documentId"])).data();
        client.edit(closing,"workspace_projects",{{"action","close"},{"discard",true}});
        require(!service.m_lua.value(QString::fromStdString(closingDoc["documentId"]))&&editor!=nullptr,"closing a project destroys its shared Lua editor safely");
    }
    client.edit(project,"workspace_projects",{{"action","select"}});
    if(!directory.isEmpty()) {
        QDir().mkpath(directory);
        for(const auto *locale:{"en","ru"}) for(const auto *theme:{"dark","light"}) {
            LocalizationManager::instance().activateLanguage(locale,false);
            ThemeManager::instance().setThemeId(theme,false);
            McpSettingsPage page;page.resize(600,760);page.show();QApplication::processEvents();
            require(page.grab().save(directory+QString("/mcp-%1-%2.png").arg(locale,theme)),"settings screenshot");
        }
    }
    client.edit(project,"creator_document",{{"action","close"},{"documentId",document["documentId"]},{"revision",creator->m_revision},{"discard",true}});
    require(!client.tool("creator_context",{{"projectId",project["projectId"]},{"documentId",document["documentId"]}},false)["ok"].get<bool>(),"closed document identity cannot be reused");
    if(qEnvironmentVariableIsSet("VLT_MCP_CODEX_CLIENT")) {
        QProcess codex;
        auto environment=QProcessEnvironment::systemEnvironment();environment.insert("VLTONE_MCP_TOKEN",service.m_token);
        codex.setProcessEnvironment(environment);codex.setWorkingDirectory(temporary.path());
        auto bridge=QCoreApplication::applicationDirPath()+"/vlt_mcp";
#ifdef Q_OS_WIN
        bridge+=".exe";
#endif
        QStringList arguments{"exec","--ignore-user-config","--ephemeral","--sandbox","read-only","--skip-git-repo-check","--json",
            "-c","mcp_servers.vltone.command="+QString::fromStdString(Json(bridge.toStdString()).dump()),
            "-c","mcp_servers.vltone.args="+QString::fromStdString(Json::array({"--endpoint",service.m_endpoint.toStdString()}).dump()),
            "-c","mcp_servers.vltone.env_vars=[\"VLTONE_MCP_TOKEN\"]",
            "-c","mcp_servers.vltone.required=true",
            "This is an MCP client interoperability check on isolated test fixtures. Use only the VLTONE MCP tools: call workspace_projects with action=list, then get_project for one returned projectId. Do not modify anything, use shell tools or read files. Report the projectId and tempo actually returned by the tools."};
        codex.start(qEnvironmentVariable("VLT_MCP_CODEX_CLIENT"),arguments);
        require(codex.waitForStarted(5000),"real Codex client starts");
        codex.closeWriteChannel();
        QByteArray output,errors;
        const bool finished=until([&]{output+=codex.readAllStandardOutput();errors+=codex.readAllStandardError();return codex.state()==QProcess::NotRunning;},150000);
        if(!finished) {codex.kill();codex.waitForFinished(2000);}
        output+=codex.readAllStandardOutput();errors+=codex.readAllStandardError();
        if(!directory.isEmpty()) {
            QFile file(directory+"/codex-client.jsonl");if(file.open(QIODevice::WriteOnly)) file.write(output);
            QFile diagnostics(directory+"/codex-client.stderr");if(diagnostics.open(QIODevice::WriteOnly)) diagnostics.write(errors);
        }
        QSet<QString> invoked;
        for(const auto &line:output.split('\n')) {
            const auto event=Json::parse(line.toStdString(),nullptr,false);
            if(!event.is_object()||event.value("type","")!="item.completed"||!event.contains("item")) continue;
            const auto &item=event["item"];
            if(item.value("type","")=="mcp_tool_call"&&item.value("server","")=="vltone"&&item.value("status","")=="completed")
                invoked.insert(QString::fromStdString(item.value("tool","")));
        }
        require(finished&&codex.exitCode()==0&&invoked.contains("workspace_projects")&&invoked.contains("get_project"),"real Codex discovers and invokes VLTONE tools");
    }
    service.revoke();
    require(until([&]{return client.process.state()==QProcess::NotRunning;}),"revoke disconnects existing client");
    {
        Bridge offline(service.m_endpoint,service.m_token);
        require(until([&]{return offline.process.state()==QProcess::NotRunning;})&&offline.process.exitCode()==3,"disabled/closed app produces clear connection failure");
    }
    window.endRecoverySessionForTest();
    std::fprintf(stderr,"PASS MCP integration workflow\n");return true;
} catch(const std::exception &error) {
    std::fprintf(stderr,"FAIL MCP %s\n",error.what());window.m_mcp->setEnabled(false);window.endRecoverySessionForTest();return false;
}
}
