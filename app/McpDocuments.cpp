#include "McpService.hpp"
#include "MainWindow.hpp"
#include "CreatorWindow.hpp"
#include "CreatorAiPanel.hpp"
#include "CreatorAiWorkspace.hpp"
#include "LuaCreatorWindow.hpp"
#include "ScriptService.hpp"
#include "SettingsWindow.hpp"
#include "ProjectSerializer.hpp"
#include "recovery/RecoverySnapshot.hpp"
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QUuid>

namespace ui {
namespace {
QString identity(QObject *object) {
    auto id=object->property("vlt.documentId").toString();
    if(id.isEmpty()) { id=QUuid::createUuid().toString(QUuid::WithoutBraces); object->setProperty("vlt.documentId",id); }
    return id;
}
QString absolute(const nlohmann::json &args,const char *key="path") {
    auto path=QString::fromStdString(args.at(key).get<std::string>());
    if(path.isEmpty()||!QFileInfo(path).isAbsolute()) throw std::runtime_error("An absolute path is required");
    return QDir::cleanPath(path);
}
QString projectPath(const QString &path) {
    if(path.isEmpty()) return {};
    QFileInfo info(path);
    if(info.isFile()&&info.suffix().compare("vlt",Qt::CaseInsensitive)==0) info.setFile(info.absolutePath());
    auto value=info.canonicalFilePath();
    if(value.isEmpty()) value=info.absoluteFilePath();
#ifdef Q_OS_WIN
    value=value.toCaseFolded();
#endif
    return value;
}
}
McpService::Json McpService::projects(Client &client,const Json &args) {
    const auto action=args.at("action").get<std::string>();
    m_window.captureProjectView();
    if(action=="list") {
        Json items=Json::array();
        for(const auto &project:m_window.m_projects.documents) {
            auto item=stamp(*project);
            item.update({{"name",project->controller.projectName()},{"path",project->path.toStdString()},
                {"dirty",project->dirty},{"selected",project->id==m_window.m_projects.selected},
                {"audio",m_window.m_projects.audio.active()==&project->controller},
                {"readOnly",project->controller.hasCloudProjectBinding()}});
            items.push_back(item);
        }
        return {{"projects",items}};
    }
    if(action=="create"||action=="open") {
        if(m_window.m_projectFileJob||m_window.m_closingProjects||m_window.m_switchingProject)
            throw std::runtime_error("Busy: workspace is changing");
        if(!m_window.m_projectTabsEnabled) m_window.setProjectTabsEnabled(true);
        if(action=="open") {
            auto path=absolute(args);
            const QFileInfo requested(path);
            if(requested.isFile()) {
                if(requested.suffix().compare("vlt",Qt::CaseInsensitive)!=0)
                    throw std::runtime_error("Open expects a project directory or its .vlt manifest");
                const auto directory=requested.absolutePath();
                const QFileInfo manifest(QString::fromStdString(daw::ProjectSerializer::manifestPath(directory.toStdString())));
                if(manifest!=requested)
                    throw std::runtime_error("The selected file is not this directory's active project manifest");
                path=directory;
            }
            for(const auto &open:m_window.m_projects.documents) if(projectPath(open->path)==projectPath(path)) {
                auto result=stamp(*open); result["alreadyOpen"]=true; return result;
            }
            auto prepared=std::make_shared<daw::EngineController::PreparedProject>();
            auto job=newJob(client,nullptr,"open_project");
            const auto rate=m_window.projectController().sampleRate();
            job->future=std::async(std::launch::async,[job,prepared,path,rate] {
                const auto result=daw::EngineController::prepareProjectOpen(path.toStdString(),rate,*prepared,[job]{return !job->cancel.load();});
                if(!result) throw std::runtime_error(result.message());
                return Json{{"path",path.toStdString()}};
            });
            job->publish=[this,prepared,path](Json result) {
                if(m_window.m_projectFileJob||m_window.m_closingProjects||m_window.m_switchingProject) throw std::runtime_error("Busy: workspace is changing; retry open");
                for(const auto &open:m_window.m_projects.documents) if(projectPath(open->path)==projectPath(path)) {
                    result.update(stamp(*open)); result["alreadyOpen"]=true; return result;
                }
                auto next=std::make_unique<ProjectSession>();
                next->controller.useWorkspacePluginManager(m_window.m_projects.plugins);
                auto initialized=next->controller.initializeDocumentOnly(m_window.projectController().sampleRate(),m_window.projectController().bufferSizeFrames());
                if(!initialized) throw std::runtime_error(initialized.message());
                const auto opened=next->controller.openPreparedProject(std::move(*prepared));
                if(!opened) throw std::runtime_error(opened.message());
                next->path=path;
                m_window.configureProjectSession(*next);
                result.update(stamp(*next));
                const auto id=next->id;
                m_window.m_projects.documents.push_back(std::move(next));
                m_window.refreshProjectTabs(); m_window.selectProjectTab(id);
                return result;
            };
            return jobStatus(*job);
        }
        auto &next=m_window.m_projects.append();
        auto initialized=next.controller.initializeDocumentOnly(m_window.projectController().sampleRate(),m_window.projectController().bufferSizeFrames());
        if(initialized) initialized=next.controller.newProject(true);
        if(!initialized) { const auto error=initialized.message(); m_window.m_projects.documents.pop_back(); throw std::runtime_error(error); }
        m_window.configureProjectSession(next);
        m_window.refreshProjectTabs(); m_window.selectProjectTab(next.id);
        return stamp(next);
    }
    auto &project=resolve(args,true);
    if(action=="select") {
        if(!m_window.selectProjectTab(project.id)) throw std::runtime_error("Busy: cannot select project");
        return stamp(project);
    }
    if(action=="audio") {
        activateAudio(project,args.value("enabled",true));
        return stamp(project);
    }
    if(action=="save") {
        auto request=args;
        if(!request.contains("path") || request["path"]=="") request["path"]=project.path.toStdString();
        const auto destination=absolute(request);
        for(const auto &open:m_window.m_projects.documents)
            if(open->id!=project.id&&projectPath(open->path)==projectPath(destination)) throw std::runtime_error("Conflict: destination belongs to another open project");
        if(QFileInfo(destination).isSymLink()) throw std::runtime_error("Cannot save over a linked directory");
        if(QFileInfo::exists(destination) && !QFileInfo(destination).isDir()) throw std::runtime_error("Save path must be a project directory");
        if(QFileInfo::exists(destination) && !QDir(destination).entryList({"*.vlt"},QDir::Files).size())
            throw std::runtime_error("Destination is not an existing VLTONE project; choose a new directory");
        for(const auto &other:m_jobs) if(!other->complete&&other->kind=="save_project") throw std::runtime_error("Busy: another project save is running");
        const auto parent=QFileInfo(destination).absolutePath();
        if(!QDir().mkpath(parent)) throw std::runtime_error("Cannot create destination directory");
        auto staging=std::make_shared<QTemporaryDir>(parent+"/.vlt-mcp-XXXXXX");
        if(!staging->isValid()) throw std::runtime_error("Cannot create save staging directory");
        const auto staged=staging->filePath(QFileInfo(destination).fileName());
        auto snapshot=std::make_shared<daw::recovery::RecoverySnapshot>(project.controller.prepareProjectSave());
        const auto revision=project.controller.projectRevision(), generation=project.controller.projectGeneration();
        const auto id=project.id;
        const auto originalPath=project.path;
        auto job=newJob(client,&project,"save_project");
        job->future=std::async(std::launch::async,[snapshot,staged,staging,job,destination] {
            if(job->cancel) throw std::runtime_error("Cancelled");
            // Preserve package-owned files the current document does not use
            // (alternate media, notes, user backups), like an ordinary save.
            if(QFileInfo::exists(destination)) {
                QDirIterator files(destination,QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden,QDirIterator::Subdirectories);
                while(files.hasNext()) {
                    files.next(); if(job->cancel) throw std::runtime_error("Cancelled");
                    if(files.fileInfo().isSymLink()) throw std::runtime_error("Project package contains a link; choose a new save directory");
                    const auto target=QDir(staged).filePath(QDir(destination).relativeFilePath(files.filePath()));
                    if(files.fileInfo().isDir()) QDir().mkpath(target);
                    else { QDir().mkpath(QFileInfo(target).absolutePath()); if(!QFile::copy(files.filePath(),target)) throw std::runtime_error("Cannot stage existing package contents"); }
                }
            }
            const auto result=daw::EngineController::writePreparedProject(*snapshot,staged.toStdString());
            if(!result) throw std::runtime_error(result.message());
            return Json::object();
        });
        job->publish=[this,staging,staged,destination,snapshot,id,revision,generation,originalPath](Json) {
            auto *live=m_window.m_projects.find(id);
            if(m_window.m_projectFileJob||m_window.m_switchingProject||m_window.m_closingProjects)
                throw std::runtime_error("Busy: a user file operation is active; nothing published");
            if(!live || live->controller.projectGeneration()!=generation || live->controller.projectRevision()!=revision || live->path!=originalPath)
                throw std::runtime_error("Conflict: project changed during save; nothing published");
            for(const auto &open:m_window.m_projects.documents)
                if(open->id!=id&&projectPath(open->path)==projectPath(destination)) throw std::runtime_error("Conflict: destination was opened while saving; nothing published");
            // Keep the previous package recoverable until the staged package is published.
            const auto backup=staging->filePath("previous");
            const bool exists=QFileInfo::exists(destination);
            if(exists&&!QDir().rename(destination,backup)) throw std::runtime_error("Cannot move previous project into save staging");
            if(!QDir().rename(staged,destination)) {
                if(exists&&!QDir().rename(backup,destination)) { staging->setAutoRemove(false); throw std::runtime_error("Cannot publish or restore project; previous package retained at "+backup.toStdString()); }
                throw std::runtime_error("Cannot publish project");
            }
            live->controller.acceptPreparedProjectSave(snapshot->project,destination.toStdString());
            live->path=destination; live->dirty=false;
            if(id==m_window.m_projects.selected) { m_window.m_projectPath=destination; m_window.m_dirty=false; m_window.updateWindowTitle(); }
            m_window.refreshProjectTabs();
            auto result=stamp(*live); result["path"]=destination.toStdString(); return result;
        };
        return jobStatus(*job);
    }
    if(action=="close") {
        if(project.controller.isRecording()) throw std::runtime_error("Stop recording before closing a project");
        if(project.dirty&&!args.value("discard",false)) throw std::runtime_error("Unsaved project: save first or specify discard=true");
        const auto id=project.id;
        const bool dirty=project.dirty; project.dirty=false;
        if(id==m_window.m_projects.selected) m_window.m_dirty=false;
        if(!m_window.closeProjectTabs({id},{},false)) {
            if(auto *live=m_window.m_projects.find(id)) live->dirty=dirty;
            if(id==m_window.m_projects.selected) m_window.m_dirty=dirty;
            throw std::runtime_error("Busy: project could not close");
        }
        return {{"closed",id.toStdString()}};
    }
    throw std::runtime_error("Unknown workspace_projects action");
}
CreatorWindow *McpService::creatorWindow(ProjectSession &project,bool create) {
    for(auto *window:m_window.findChildren<CreatorWindow*>())
        if(window->m_controller==&project.controller) {
            window->setProperty("vlt.projectId",project.id); m_creators[identity(window)]=window;
            if(!create) return window;
        }
    if(!create) return nullptr;
    auto *window=new CreatorWindow(&project.controller,&m_window);
    window->m_started=true;
    window->setProperty("vlt.projectId",project.id);
    m_creators[identity(window)]=window;
    connect(window,&CreatorWindow::modulesCompiled,this,[this,id=project.id] { if(auto *p=m_window.m_projects.find(id)) edited(*p); });
    connect(window,&CreatorWindow::aiSettingsRequested,&m_window,[this] { m_window.openSettings(SettingsWindow::kAiTab); });
    return window;
}
LuaCreatorWindow *McpService::luaWindow(ProjectSession &project,bool create) {
    for(auto *window:m_window.findChildren<LuaCreatorWindow*>())
        if(window->property("vlt.projectId").toString()==project.id) {
            m_lua[identity(window)]=window;
            if(!create) return window;
        }
    if(!create) return nullptr;
    auto *window=new LuaCreatorWindow(m_window.m_scripts,&m_window);
    window->setProperty("vlt.projectId",project.id); m_lua[identity(window)]=window;
    return window;
}
McpService::Json McpService::creator(Client &client,ProjectSession &project,const std::string &name,const Json &args) {
    // Refresh the registry so documents opened through the GUI share the same IDs.
    for(auto *w:m_window.findChildren<CreatorWindow*>()) if(w->m_controller==&project.controller) {
        w->setProperty("vlt.projectId",project.id); m_creators[identity(w)]=w;
    }
    const auto state=[&](CreatorWindow *w) { auto result=stamp(project); result.update({{"documentId",identity(w).toStdString()},
        {"revision",w->m_revision},{"path",w->m_path.toStdString()},{"name",w->m_project.name.toStdString()},
        {"dirty",!w->m_undo->isClean()},{"busy",w->m_build->busy()||w->property("vlt.mcp.busy").toBool()||w->m_ai->running()}}); return result; };
    const auto action=args.value("action","");
    if(name=="creator_document" && action=="list") {
        Json documents=Json::array();
        for(auto *w:m_window.findChildren<CreatorWindow*>()) if(w->m_controller==&project.controller) documents.push_back(state(w));
        return {{"documents",documents}};
    }
    if(name=="creator_document"&&(action=="create"||action=="open")) {
        CreatorProject document; QString error,path;
        if(action=="open") { path=absolute(args); if(!CreatorProject::open(path,document,error)) throw std::runtime_error(error.toStdString()); }
        else { auto title=QString::fromStdString(args.value("name","MCP module")); document=CreatorProject::create(title,title); }
        auto *w=creatorWindow(project,true); w->m_path=path; w->apply(document); w->m_undo->clear();
        if(path.isEmpty()) w->m_undo->resetClean(); else w->m_undo->setClean();
        w->show(); w->raise(); return state(w);
    }
    const auto id=QString::fromStdString(args.at("documentId")); auto *w=m_creators.value(id).data();
    if(!w||identity(w)!=id||w->m_controller!=&project.controller) throw std::runtime_error("Conflict: Creator document closed or replaced");
    const bool read=name=="creator_context"||name=="creator_catalog"||name=="creator_sdk"||name=="creator_source";
    if(!read && (!args.contains("revision")||args["revision"]!=w->m_revision)) throw std::runtime_error("Conflict: Creator revision changed; read creator_context");
    if(!read && (w->property("vlt.mcp.busy").toBool()||w->m_ai->running()||w->m_build->busy()||w->m_codeFuture.valid()))
        throw std::runtime_error("Busy: Creator has an active operation");
    if(name=="creator_document") {
        if(action=="save") { QString error; const auto path=absolute(args); if(!w->m_project.save(path,error)) throw std::runtime_error(error.toStdString()); w->m_path=path; w->m_undo->setClean(); w->rememberRecent(); }
        else if(action=="show") { w->show(); w->raise(); }
        else if(action=="close") {
            if(!w->m_undo->isClean()&&!args.value("discard",false)) throw std::runtime_error("Unsaved Creator document: save first or specify discard=true");
            delete w; return {{"closed",id.toStdString()}};
        } else throw std::runtime_error("Unknown creator_document action");
        return state(w);
    }
    const bool asynchronous=name=="creator_cpp"||name=="creator_build";
    auto job=asynchronous?newJob(client,&project,QString::fromStdString(name)):std::shared_ptr<Job>{};
    auto ownedWorkspace=std::make_unique<CreatorAiWorkspace>(*w);
    auto *workspace=ownedWorkspace.get();
    auto body=args; for(const auto *key:{"projectId","generation","projectRevision","documentId"}) body.erase(key);
    const daw::ai::ToolCall invocation{"mcp",name,body};
    workspace->begin(1,"draft only",1);
    if(asynchronous) {
        job->document=id;
        w->setProperty("vlt.mcp.busy",true);
        QPointer<CreatorWindow> window(w); QPointer<CreatorAiWorkspace> work(workspace);
        workspace->completed=[job,window,this](auto,auto,daw::ai::ToolResult value) {
            job->result=value.toJson();
            if(window) job->result["revision"]=window->m_revision;
            if(auto *live=m_window.m_projects.find(job->project)) job->result.update(stamp(*live));
        };
        job->stop=[work] { if(work) work->cancel(); };
        job->poll=[job,work] { return !work||!work->busy(); };
        job->cleanup=[window,work] { if(window) window->setProperty("vlt.mcp.busy",false); if(work) { work->finish(true,false); work->deleteLater(); } };
        ownedWorkspace.release();
        if(auto value=workspace->execute(invocation,context(client,project),1)) job->result=value->toJson();
        return jobStatus(*job);
    }
    const auto value=workspace->execute(invocation,context(client,project),1);
    workspace->finish(true,false);
    auto result=value?value->toJson():Json{{"ok",false},{"error","Unexpected pending operation"}};
    result.update(state(w)); return result;
}
McpService::Json McpService::lua(Client &client,ProjectSession &project,const std::string &name,const Json &args) {
    for(auto *w:m_window.findChildren<LuaCreatorWindow*>()) if(w->property("vlt.projectId").toString()==project.id) m_lua[identity(w)]=w;
    const auto state=[&](LuaCreatorWindow *w) { auto result=stamp(project); result.update({{"documentId",identity(w).toStdString()},
        {"revision",w->m_revision},{"path",w->m_path.toStdString()},{"dirty",!w->m_undo->isClean()},
        {"busy",w->property("vlt.mcp.busy").toBool()||w->m_session->running()}}); return result; };
    const auto action=args.value("action","");
    if(name=="lua_document"&&action=="list") {
        Json documents=Json::array();
        for(auto *w:m_window.findChildren<LuaCreatorWindow*>()) if(w->property("vlt.projectId").toString()==project.id) documents.push_back(state(w));
        return {{"documents",documents}};
    }
    if(name=="lua_document"&&(action=="create"||action=="open")) {
        Json document; QString path,error;
        if(action=="open") { path=absolute(args); if(!ScriptLibrary::readCreatorDocument(path,document,error)) throw std::runtime_error(error.toStdString()); }
        auto *w=luaWindow(project,true);
        if(action=="open") { w->apply(document); w->m_path=path; w->m_undo->clear(); w->m_undo->setClean(); w->setWindowFilePath(path); }
        else if(args.contains("name")) {
            ScriptPackage p{{},ScriptLibrary::manifestForLua(QString::fromStdString(args["name"])),{{"main.lua","print(\"Hello from VLTONE\")\n"}}};
            w->openPackage(p); m_lua[identity(w)]=w;
        }
        w->show(); w->raise(); return state(w);
    }
    const auto id=QString::fromStdString(args.at("documentId")); auto *w=m_lua.value(id).data();
    if(!w||identity(w)!=id||w->property("vlt.projectId").toString()!=project.id) throw std::runtime_error("Conflict: Lua document closed or replaced");
    const bool read=name=="lua_context"||name=="lua_read"||name=="lua_sdk"||name=="lua_example"||name=="lua_status"||name=="lua_check";
    if(!read&&name!="lua_stop"&&(!args.contains("revision")||args["revision"]!=w->m_revision)) throw std::runtime_error("Conflict: Lua revision changed; read lua_context");
    if(!read&&name!="lua_stop"&&(w->property("vlt.mcp.busy").toBool()||w->m_session->running())) throw std::runtime_error("Busy: Lua editor has an active operation");
    if(name=="lua_document"&&action!="run") {
        if(action=="save") {
            const auto path=absolute(args); QSaveFile file(path); auto bytes=QByteArray::fromStdString(w->m_document.dump(2));
            if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()) throw std::runtime_error(file.errorString().toStdString());
            w->m_path=path; w->setWindowFilePath(path); w->m_undo->setClean();
        } else if(action=="show") { w->show(); w->raise(); }
        else if(action=="close") {
            if(!w->m_undo->isClean()&&!args.value("discard",false)) throw std::runtime_error("Unsaved Lua document: save first or specify discard=true");
            delete w; return {{"closed",id.toStdString()}};
        } else throw std::runtime_error("Unknown lua_document action");
        return state(w);
    }
    if(name=="lua_check"||name=="lua_install") {
        const auto package=w->package(); const auto revision=w->m_revision; QPointer<LuaCreatorWindow> window(w);
        const bool install=name=="lua_install";
        auto job=newJob(client,&project,QString::fromStdString(name)); job->document=id;
        if(install) w->setProperty("vlt.mcp.busy",true);
        job->future=std::async(std::launch::async,[package] { QString error; const bool ok=ScriptLibrary::validate(package,error); return Json{{"ok",ok},{"error",error.toStdString()}}; });
        job->publish=[this,window,package,revision,id,install](Json result) {
            if(!window||window->m_revision!=revision||identity(window)!=id) throw std::runtime_error("Conflict: Lua source changed during validation");
            if(install&&result.value("ok",false)) {
                QString error; if(!ScriptLibrary::install(package,error,false)) throw std::runtime_error(error.toStdString());
                m_window.m_scripts->reload(); result["installed"]=package.id().toStdString();
            }
            result["revision"]=revision; return result;
        };
        job->cleanup=[window,install] { if(window&&install) window->setProperty("vlt.mcp.busy",false); };
        return jobStatus(*job);
    }
    if(name=="lua_preview"||(name=="lua_document"&&action=="run")) {
        Json pinned={{"project",project.id.toStdString()},{"generation",project.controller.projectGeneration()},
            {"fileAccess",args.value("fileAccess",Json::array())}};
        const auto command=QString::fromStdString(args.value(name=="lua_preview"?"action":"actionId",w->m_commands->currentData().toString().toStdString()));
        auto job=newJob(client,&project,name=="lua_preview"?"lua_preview":"lua_run"); job->document=id;
        try {
            job->scriptRun=m_window.m_scripts->runPackage(w->package(),command,pinned,name=="lua_preview","mcp");
            if(job->scriptRun.isEmpty()) throw std::runtime_error("Cannot start Lua command; inspect manifest and actionId");
        } catch(...) { cancelJob(job); throw; }
        w->m_run=job->scriptRun; w->refreshRunLog(); auto result=jobStatus(*job); result["runId"]=job->scriptRun.toStdString(); return result;
    }
    if(name=="lua_stop") {
        for(const auto &job:m_jobs) if(job->owner==client.id&&job->document==id) cancelJob(job);
        return state(w);
    }
    auto body=args; for(const auto *key:{"projectId","generation","projectRevision","documentId"}) body.erase(key);
    LuaAiWorkspace workspace(*w);
    const auto value=workspace.execute({"mcp",name,body},context(client,project),1);
    auto result=value?value->toJson():Json::object(); result.update(state(w)); return result;
}
}
