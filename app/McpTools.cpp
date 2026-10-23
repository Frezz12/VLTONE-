#include "McpService.hpp"
#include "MainWindow.hpp"
#include "AiChatPanel.hpp"
#include "CreatorAiWorkspace.hpp"
#include "LuaCreatorWindow.hpp"
#include "ScriptService.hpp"
#include "ShortcutManager.hpp"
#include "MidiInputManager.hpp"
#include "TimelineWidget.hpp"
#include "MixerWidget.hpp"
#include "TrackListWidget.hpp"
#include "PianoRollWindow.hpp"
#include "WarpEditorWidget.hpp"
#include "SettingsWindow.hpp"
#include "PerformanceWindow.hpp"
#include "scripting/ScriptProject.hpp"
#include "ai/ContentCatalog.hpp"
#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QScopedValueRollback>
#include <QSettings>
#include <algorithm>

namespace ui {
namespace {
using Json=nlohmann::json;
using Effect=daw::ai::ToolSpec::Effect;
Json field(const char *type) { return {{"type",type}}; }
bool modifies(Effect e) { return e!=Effect::ReadOnly; }
// Only commands known to finish synchronously without a modal decision/picker.
bool directCommand(const QString &id) {
    static const QSet<QString> allowed{"view.toggleMixer", "view.toggleBrowser", "view.toggleInspector", "view.toggleMaster",
        "view.toggleContextPanel", "view.toggleTrackOverview", "view.toggleAi", "view.toggleMixerRack", "view.zoomFit",
        "view.timeBars", "view.timeClock", "view.timeBoth", "view.positionBars", "view.positionClock",
        "view.followPlayhead", "view.automation.showAll", "view.automation.hideAll"};
    return allowed.contains(id);
}
}
std::vector<daw::ai::ToolSpec> McpService::specs() const {
    auto result=daw::ai::toolSpecs();
    auto creator=CreatorAiWorkspace::toolSpecs();
    auto lua=LuaAiWorkspace::toolSpecs(daw::ai::InteractionMode::Do);
    result.insert(result.end(),creator.begin(),creator.end());
    result.insert(result.end(),lua.begin(),lua.end());
    const auto add=[&](const char *name,const char *description,Json fields,Json required,Effect effect) {
        result.push_back({name,description,{{"type","object"},{"properties",fields},{"required",required},{"additionalProperties",false}},effect});
    };
    const auto string=field("string"), boolean=field("boolean"), object=field("object"), integer=field("integer");
    add("workspace_projects", "Manage open project identities. action=list|create|open|select|save|close|audio. "
        "create/open return a job or new identity; open/save use an absolute path, never a dialog. close requires discard=true for unsaved work. "
        "audio enabled=true powers the specified project. Inspect list before commands. Writes need generation and projectRevision.",
        {{"action",string},{"path",string},{"discard",boolean},{"enabled",boolean}}, {"action"},Effect::ExternalSideEffect);
    for (const auto *name : {"creator_document","lua_document"})
        add(name,"Manage shared editor documents: action=list|create|open|save|close|show. create/open returns documentId, revision, generation. "
            "open/save accept path. Each open creates a separate document. close requires discard=true if dirty. "
            "Lua also supports action=run with optional command actionId and explicit fileAccess grants. No file dialogs.",
            {{"action",string},{"path",string},{"name",string},{"discard",boolean},{"actionId",string},
             {"revision",integer},{"fileAccess",{{"type","array"},{"maxItems",64},{"items",{{"type","object"},
                 {"properties",{{"path",string},{"write",boolean},{"folder",boolean}}},{"required",{"path"}},{"additionalProperties",false}}}}}},
            {"action"},Effect::ExternalSideEffect);
    add("plugin_library","Manage installed plugin catalog: action=list|paths|set_paths|scan|scan_status|install_module. "
        "Also add_module(path,channelId), preset_save/preset_load(path,channelId). format=vst3|clap|vst|au. paths is an array of absolute directories. scan is a cancellable job. "
        "install_module installs an existing local .vltmini; no downloads/system installers.",
        {{"action",string},{"format",string},{"paths",{{"type","array"},{"items",string},{"maxItems",128}}},
         {"rescan",boolean},{"path",string},{"channelId",string}}, {"action"},Effect::ExternalSideEffect);
    add("get_job","Read state, progress, artifacts, diagnostics and result for one job belonging to this connection.",
        {{"jobId",string}}, {"jobId"},Effect::ReadOnly);
    add("cancel_job","Cancel one job belonging to this connection. Completed edits remain. Staged files are not published.",
        {{"jobId",string}}, {"jobId"},Effect::LiveControl);
    add("render_preview","Render an immutable project snapshot in the background, publish a WAV atomically, and measure its peak/RMS. "
        "Optional startSeconds/endSeconds select a range. path is optional for a private preview artifact. Poll get_job.",
        {{"path",string},{"startSeconds",{{"type","number"},{"minimum",0}}},{"endSeconds",{{"type","number"},{"minimum",0}}},
         {"sampleRate",{{"type","integer"},{"minimum",8000},{"maximum",192000}}},{"normalize",boolean}},Json::array(),Effect::ExternalSideEffect);
    add("capture_view","Capture a real VLTONE window as PNG. view=main|creator|lua|script; documentId required for editors; runId and windowId for your script window. Optional output path.",
        {{"view",string},{"path",string},{"documentId",string},{"runId",string},{"windowId",string}},Json::array(),Effect::ReadOnly);
    add("script_ui","Inspect your running Lua extension and exercise its UI. action=list|get|click|tab|pointer|key. runId is from get_job. Other than list, actions require widgetId. tab uses one-based index. pointer clicks a canvas at normalized x/y (0..1); key sends Left/Right/Up/Down/Space/Return/R to a canvas. Buttons/toggles use click. Reads return actual widget state. Inputs queue real callbacks; inspect the run afterwards. Only your live run in this project is accessible.",
        {{"action",string},{"runId",string},{"widgetId",string},{"index",integer},{"key",string},
         {"x",{{"type","number"},{"minimum",0},{"maximum",1}}},{"y",{{"type","number"},{"minimum",0},{"maximum",1}}}}, {"action","runId"},Effect::LiveControl);
    add("project_edit","Apply one atomic, revision-checked Script SDK project.apply batch. Read vltone://lua/api for operation contracts. Beats, not bars. Returns one Undo step.",
        {{"operations",{{"type","array"},{"maxItems",4096},{"items",object}}},{"label",string}}, {"operations"},Effect::ReversibleEdit);
    add("plugin_parameters","Script SDK device parameters, including stable host-exposed IDs. action=list|get|set, track, device; parameter/identity/value for get/set, offset/limit for list.",
        {{"action",string},{"track",string},{"device",string},{"parameter",string},{"identity",string},{"value",field("number")},{"offset",integer},{"limit",integer}},
        {"action","track","device"},Effect::ReversibleEdit);
    add("workspace_ui","Read/open/select using the Script SDK workspace API. request contains method=workspace.get|workspace.open|workspace.select and SDK arguments. "
        "Opening/selecting requires this project to be the selected tab; use workspace_projects/select explicitly.",
        {{"request",object}}, {"request"},Effect::LiveControl);
    add("dsp_status","Read host DSP runtime status for a device without consuming the user's diagnostic notifications.",
        {{"channelId",string},{"insertId",string}}, {"channelId","insertId"},Effect::ReadOnly);
    add("recording","Start/stop/status recording on explicitly named tracks. action=start requires tracks and an absolute directory. A recording belongs to its client and stops on disconnect.",
        {{"action",string},{"tracks",{{"type","array"},{"maxItems",256},{"items",string}}},{"directory",string}}, {"action"},Effect::LiveControl);
    for(auto &spec:result) {
        // The local connection already carries DAW authorization. Keep the
        // built-in chat's approval wording/modes in its original registry.
        if(spec.name=="save_project") spec.description="Save this project to an explicit directory, or its current path. Returns a background job; no dialog.";
        else if(spec.name=="export_audio") spec.description="Render this project/channel to a new WAV path. Returns a background job with measured audio; no dialog.";
        else if(spec.name=="remove_track") spec.description="Delete a track and its contents as one undoable edit, preserving object locks.";
        else if(spec.name=="remove_clip") spec.description="Delete a clip as one undoable edit, preserving object locks.";
        else if(spec.name=="transport") spec.description="Play, stop, seek or control the metronome for this explicit project. Playback activates its audio engine.";
        else if(spec.name=="run_command") spec.description="Execute an available synchronous application command. Use parameterized tools for file operations and modal actions.";
        auto &p=spec.inputSchema["properties"];
        p["projectId"]=string; p["generation"]=integer; p["projectRevision"]=integer;
        if(spec.name=="export_audio") p["normalize"]["description"]="Normalize the rendered peak to -1 dBFS.";
        auto &required=spec.inputSchema["required"];
        if (!required.is_array()) required=Json::array();
        const bool scope=spec.name!="workspace_projects" && spec.name!="get_job" && spec.name!="cancel_job";
        if(scope) required.push_back("projectId");
        const bool document=spec.name.starts_with("creator_")||spec.name.starts_with("lua_");
        if(document) {
            p["documentId"]=string;
            if(spec.name!="creator_document" && spec.name!="lua_document") required.push_back("documentId");
        }
        if(modifies(spec.effect)) spec.description += " Mutations require current generation and projectRevision; editor mutations also require revision.";
        if(spec.name=="import_audio" || spec.name=="load_sampler" || spec.name=="analyze_sample") {
            p["path"]=string;
            required.erase(std::remove(required.begin(),required.end(),Json("contentId")),required.end());
            spec.description += " MCP also accepts an explicit absolute path instead of contentId.";
        }
        if(spec.name=="lua_preview"||spec.name=="lua_install") p["revision"]=integer;
        if(spec.name=="lua_preview"||spec.name=="run_script")
            p["fileAccess"]={{"type","array"},{"maxItems",64},{"items",{{"type","object"},
                {"properties",{{"path",string},{"write",boolean},{"folder",boolean}}},{"required",{"path"}},{"additionalProperties",false}}}};
    }
    return result;
}
McpService::Json McpService::dispatch(Client &client,const std::string &method,const Json &params) {
    if(method=="tools/list") {
        Json tools=Json::array();
        for(const auto &spec:specs()) tools.push_back({{"name",spec.name},{"description",spec.description},{"inputSchema",spec.inputSchema},
            {"annotations",{{"readOnlyHint",spec.effect==Effect::ReadOnly},{"destructiveHint",spec.effect==Effect::DestructiveEdit},{"openWorldHint",false}}}});
        return {{"tools",tools}};
    }
    if(method=="resources/list") return {{"resources",resources()}};
    if(method=="resources/templates/list") return {{"resourceTemplates",Json::array()}};
    if(method=="resources/read") return readResource(params.at("uri").get<std::string>());
    if(method!="tools/call") throw std::invalid_argument("Unknown method");
    const auto name=params.at("name").get<std::string>();
    auto arguments=params.value("arguments",Json::object());
    const auto catalog=specs();
    const auto spec=std::find_if(catalog.begin(),catalog.end(),[&](const auto &item){return item.name==name;});
    if(spec==catalog.end()) throw std::invalid_argument("Unknown tool: "+name);
    vlt::mcp::validate(spec->inputSchema,arguments);
    try {
        auto value=call(client,name,arguments);
        const bool ok=!value.is_object()||value.value("ok",true);
        const auto message=value.is_object()?value.value("error",std::string()):std::string();
        auto result=vlt::mcp::toolResult(ok,std::move(value),message);
        if(name=="capture_view" && result["structuredContent"].contains("image")) {
            result["content"].push_back(result["structuredContent"]["image"]);
            result["structuredContent"].erase("image");
            result["content"][0]["text"]=result["structuredContent"].dump();
        }
        if(name!="get_job") appendLog(QString::fromStdString(name)+(ok?" · OK":" · "+tr("Failed")));
        return result;
    } catch(const std::exception &error) {
        // A setup failure must not leave an unreturned job/run alive.
        for(const auto &id:client.requestJobs.value(QString::fromStdString(client.requestId.dump())))
            if(auto job=m_jobs.value(id)) cancelJob(job);
        const std::string message=error.what();
        Json state={{"code",message.starts_with("Conflict")?"conflict":message.starts_with("Busy")?"busy":"operation_failed"}};
        if(arguments.contains("projectId"))
            if(auto *project=m_window.m_projects.find(QString::fromStdString(arguments["projectId"]))) state.update(stamp(*project));
        appendLog(QString::fromStdString(name)+" · "+tr("Failed"));
        return vlt::mcp::toolResult(false,state,message);
    }
}
ProjectSession &McpService::resolve(const Json &args,bool edit) {
    auto *project=m_window.m_projects.find(QString::fromStdString(args.at("projectId")));
    if(!project) throw std::runtime_error("Conflict: project closed or unknown projectId");
    if(args.contains("generation") && args["generation"]!=project->controller.projectGeneration())
        throw std::runtime_error("Conflict: project generation changed");
    if(edit) {
        if(!args.contains("generation") || !args.contains("projectRevision"))
            throw std::runtime_error("Conflict: mutations require generation and projectRevision from current state");
        if(args["projectRevision"]!=project->controller.projectRevision()) throw std::runtime_error("Conflict: project revision changed; read state again");
        if(project->controller.hasCloudProjectBinding()) throw std::runtime_error("Collaborative projects are read-only for external agents");
        if(m_window.m_projectFileJob || m_window.m_switchingProject || m_window.m_closingProjects || m_window.m_projects.audio.busy())
            throw std::runtime_error("Busy: project workspace is changing");
    }
    return *project;
}
McpService::Json McpService::stamp(ProjectSession &project) const {
    return {{"projectId",project.id.toStdString()},{"generation",project.controller.projectGeneration()},
        {"projectRevision",project.controller.projectRevision()}};
}
void McpService::edited(ProjectSession &project) {
    m_window.m_scripts->m_bindings.edited(project.id);
}
void McpService::activateAudio(ProjectSession &project, bool enabled) {
    auto *target=enabled?&project.controller:nullptr;
    auto *previous=m_window.m_projects.audio.active();
    if(previous==target || (!enabled && previous!=&project.controller)) return;
    if(previous && previous->isRecording()) throw std::runtime_error("Busy: stop recording before switching the audio engine");
    const auto revision=project.controller.projectRevision();
    const QScopedValueRollback<bool> switching(m_window.m_switchingProject,true);
    if(m_window.m_midiInput) QCoreApplication::sendPostedEvents(m_window.m_midiInput,QEvent::MetaCall);
    if(auto *focus=QApplication::focusWidget()) focus->clearFocus();
    m_window.m_timeline->finishProjectEdits();
    m_window.m_mixer->finishProjectEdits();
    m_window.m_trackList->finishProjectEdits();
    if(m_window.m_pianoRoll) m_window.m_pianoRoll->finishPendingNoteEdit();
    if(m_window.m_warpEditor) m_window.m_warpEditor->finishPendingEdit();
    if(project.controller.projectRevision()!=revision) throw std::runtime_error("Conflict: pending user edits changed the project; read state again");
    delete m_window.m_settingsWindow; m_window.m_settingsWindow=nullptr;
    delete m_window.m_performanceWindow; m_window.m_performanceWindow=nullptr;
    m_window.m_midiInput->setProjectController(nullptr);
    const auto result=m_window.m_projects.audio.activate(target);
    m_window.m_midiInput->setProjectController(m_window.m_projects.audio.active());
    m_window.m_journalStale=true;
    m_window.refreshProjectTabs(); m_window.syncViews();
    if(!result) throw std::runtime_error(result.message());
}
daw::ai::ToolContext McpService::context(Client &client,ProjectSession &project) {
    daw::ai::ToolContext result;
    result.projectPath=(project.id==m_window.m_projects.selected?m_window.m_projectPath:project.path).toStdString();
    result.contentCatalog=client.catalog; result.compositionCandidates=client.candidates;
    if(m_window.m_aiPanel) {
        for(const auto &path:m_window.m_aiPanel->contentPaths()) result.sampleFolders.push_back(path.toStdString());
        if(client.catalog->status().state==daw::ai::CatalogIndexState::Idle) {
            client.catalog->setBrowserRoots(result.sampleFolders); client.catalog->startRefresh();
        }
    }
    const auto selection=m_window.m_scripts->m_bindings.context(project.id);
    for(const auto &id:selection["selection"]["tracks"]) result.focus.trackIds.push_back(id);
    for(const auto &clip:selection["selection"]["clips"]) result.focus.clipIds.push_back(clip["clip"]);
    if(!result.focus.trackIds.empty()) result.focus.trackId=result.focus.trackIds.front();
    if(!result.focus.clipIds.empty()) result.focus.clipId=result.focus.clipIds.front();
    const auto id=project.id;
    result.uiContext=[this,id,selection] {
        if(id==m_window.m_projects.selected && m_window.m_aiPanel && m_window.m_aiPanel->m_uiContext) return m_window.m_aiPanel->m_uiContext();
        return selection;
    };
    result.focusInApp=[this,id](const Json &target)->Json {
        if(id!=m_window.m_projects.selected) throw std::runtime_error("Select the pinned project before opening its editors");
        if(!m_window.m_aiPanel || !m_window.m_aiPanel->m_uiFocus) throw std::runtime_error("Editor navigation is unavailable");
        return m_window.m_aiPanel->m_uiFocus(target);
    };
    result.searchCommands=[this](const std::string &query,auto) {
        Json commands=Json::array();
        for(const auto &cmd:m_window.m_shortcuts->search(QString::fromStdString(query))) commands.push_back({{"id",cmd.id.toStdString()},
            {"label",cmd.label.toStdString()},{"description",cmd.metadata.description.toStdString()},
            {"available",directCommand(cmd.id) && cmd.action && cmd.action->isEnabled()}});
        return commands;
    };
    result.invokeCommand=[this,id](const std::string &command,auto,std::string &error) {
        if(id!=m_window.m_projects.selected || !directCommand(QString::fromStdString(command))) {
            error="Use a parameterized MCP/Script SDK operation for this command; modal actions are unavailable"; return false;
        }
        return m_window.m_shortcuts->invoke(QString::fromStdString(command));
    };
    return result;
}
McpService::Json McpService::call(Client &client,const std::string &name,const Json &args) {
    if(name=="get_job"||name=="cancel_job") {
        auto job=m_jobs.value(QString::fromStdString(args.at("jobId")));
        if(!job || job->owner!=client.id) throw std::runtime_error("Unknown job for this connection");
        if(name=="cancel_job") cancelJob(job);
        return jobStatus(*job);
    }
    if(name=="workspace_projects") return projects(client,args);
    auto catalog=specs();
    auto spec=std::find_if(catalog.begin(),catalog.end(),[&](const auto &s){return s.name==name;});
    bool edit=modifies(spec->effect);
    const auto action=args.value("action","");
    if((name=="creator_document"||name=="lua_document") && action=="list") edit=false;
    if(name=="plugin_library" && (action=="list"||action=="paths"||action=="scan_status")) edit=false;
    if(name=="plugin_parameters" && action!="set") edit=false;
    if(name=="recording" && action=="status") edit=false;
    if(name=="script_ui" && (action=="list"||action=="get")) edit=false;
    auto &project=resolve(args,edit);
    if(name.starts_with("creator_")) return creator(client,project,name,args);
    if(name.starts_with("lua_")) return lua(client,project,name,args);
    if(name=="plugin_library") return plugins(client,args);
    if(name=="render_preview"||name=="export_audio") return render(client,project,args);
    if(name=="analyze_track"||name=="analyze_mix"||name=="analyze_sample") return analyze(client,project,name,args);
    if(name=="capture_view") return capture(client,project,args);
    if(name=="script_ui") return scriptUi(client,project,args);
    if(name=="dsp_status") {
        const auto status=project.controller.insertRuntimeStatus(args.at("channelId"),args.at("insertId"));
        return {{"state",int(status.state)},{"detail",status.detail},{"canRetry",status.canRetry},{"incident",status.incident}};
    }
    if(name=="recording") {
        if(action=="start") {
            if(project.controller.isRecording()) throw std::runtime_error("Busy: project is recording");
            const auto path=QString::fromStdString(args.at("directory"));
            if(!QFileInfo(path).isAbsolute()||!QDir().mkpath(path)) throw std::runtime_error("Recording requires an absolute writable directory");
            const auto tracks=args.at("tracks").get<std::vector<std::string>>();
            for(const auto &track:tracks) if(project.controller.isTrackEditLocked(track)) throw std::runtime_error("Track is locked");
            auto job=newJob(client,&project,"recording");
            try {
                activateAudio(project);
                project.controller.setRecordDirectory(path.toStdString());
                if(!project.controller.startRecordingTracksExactly(tracks)) throw std::runtime_error("Cannot start recording on the requested tracks");
            } catch(...) { cancelJob(job); throw; }
            job->stop=[this,id=project.id] { if(auto *p=m_window.m_projects.find(id)) { p->controller.stopRecording(); edited(*p); } };
            job->poll=[this,job] { auto *p=m_window.m_projects.find(job->project); return !p||!p->controller.isRecording(); };
            return jobStatus(*job);
        }
        if(action=="stop") {
            bool owned=false;
            for(const auto &job:m_jobs) if(job->owner==client.id&&job->project==project.id&&job->kind=="recording"&&!job->complete) { if(job->stop) job->stop(); owned=true; }
            if(!owned&&project.controller.isRecording()) throw std::runtime_error("Recording belongs to another client or the user");
        } else if(action!="status") throw std::runtime_error("Unknown recording action");
        auto result=stamp(project); result["recording"]=project.controller.isRecording(); result["warning"]=project.controller.recordingWarning(); return result;
    }
    if(name=="save_project") {
        auto request=args; request["action"]="save"; return projects(client,request);
    }
    auto body=args;
    body.erase("projectId"); body.erase("generation"); body.erase("projectRevision");
    const auto revision=project.controller.projectRevision();
    if(edit) {
        for(const auto *key:{"trackId","channelId","toTrackId","destinationTrackId","track"})
            if(args.contains(key)&&args[key].is_string()&&project.controller.isTrackEditLocked(args[key])) throw std::runtime_error("Track is locked");
        if(args.contains("trackId")&&args.contains("clipId")&&project.controller.isClipContentEditLocked(args["trackId"],args["clipId"]))
            throw std::runtime_error("Clip content is locked");
    }
    Json value;
    if(name=="project_edit") {
        body["revision"]=revision;
        if(!body.contains("label")) body["label"]="MCP edit";
        value=daw::scripting::applyEdits(project.controller,body);
    } else if(name=="plugin_parameters") {
        if(action!="list"&&action!="get"&&action!="set") throw std::invalid_argument("Unknown plugin_parameters action");
        body.erase("action");
        value=action=="list"?daw::scripting::parameters(project.controller,body):daw::scripting::parameter(project.controller,body,action=="set");
        if(action=="list") value={{"parameters",value},{"identity",daw::scripting::parameterIdentity(project.controller,body.at("track"),body.at("device"))}};
    } else if(name=="workspace_ui") value=m_window.m_scripts->m_bindings.workspace(project.id,body.at("request"));
    else if(name=="search_scripts") value=m_window.m_scripts->describe(QString::fromStdString(body.value("query","")));
    else if(name=="run_script") {
        Json pinned={{"project",project.id.toStdString()},{"generation",project.controller.projectGeneration()},
            {"fileAccess",body.value("fileAccess",Json::array())}};
        auto job=newJob(client,&project,"lua_run");
        try {
            job->scriptRun=m_window.m_scripts->run(QString::fromStdString(body.at("id")),pinned,body.value("arguments",Json::object()),"mcp");
            if(job->scriptRun.isEmpty()) throw std::runtime_error("Cannot start script");
        } catch(...) { cancelJob(job); throw; }
        value=jobStatus(*job);
    } else if(name=="get_script_run"||name=="stop_script") {
        const auto run=QString::fromStdString(body.at("id"));
        bool owned=false;
        for(const auto &job:m_jobs) if(job->owner==client.id&&job->project==project.id&&job->scriptRun==run) owned=true;
        if(!owned) throw std::runtime_error("Unknown script run for this connection/project");
        if(name=="stop_script") m_window.m_scripts->stop(run);
        value=m_window.m_scripts->status(run);
    } else {
        auto ctx=context(client,project);
        if(body.contains("path") && (name=="import_audio"||name=="load_sampler"||name=="analyze_sample")) {
            daw::ai::Attachment attachment;
            attachment.path=body["path"]; attachment.name=QFileInfo(QString::fromStdString(attachment.path)).fileName().toStdString();
            ctx.attachments.push_back(attachment);
            body["filePath"]=attachment.path;
        }
        if(name=="transport" && body.value("action","")=="play" && m_window.m_projects.audio.active()!=&project.controller) {
            activateAudio(project);
        }
        const auto group=edit&&spec->effect!=Effect::History?project.controller.beginUndoGroup():daw::EngineController::UndoGroup{};
        const auto result=daw::ai::callTool(project.controller,name,body,ctx);
        if(group) { project.controller.collapseUndo(group,"MCP: "+name); project.controller.releaseUndoGroup(group); }
        value=result.toJson();
    }
    if(project.controller.projectRevision()!=revision) edited(project);
    if(!value.is_object()) value={{"value",value}};
    value.update(stamp(project));
    return value;
}
}
