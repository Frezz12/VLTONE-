#include "ScriptService.hpp"
#include "LuaCreatorWindow.hpp"
#include "RackParameterBinding.hpp"
#include "ScriptContextMenu.hpp"
#include "ScriptManagerWindow.hpp"
#include "ScriptRun.hpp"
#include "ShortcutManager.hpp"
#include "scripting/ScriptProject.hpp"
#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QToolBar>
#include <QUuid>
#include <cmath>
#include <limits>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ui {
namespace {
QPointer<ScriptService> service;
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
} // namespace
ScriptService *ScriptService::instance() { return service; }
ScriptService::ScriptService(QMainWindow *parent, ShortcutManager *shortcuts, Bindings bindings)
    : QObject(parent), m_window(parent), m_shortcuts(shortcuts), m_bindings(std::move(bindings)) {
    service = this;
    scripts::contextAppender = [this](QMenu *menu, daw::EngineController *c, Json context) {
        appendMenu(menu, c, std::move(context));
    };
    m_tick.setInterval(50);
    connect(&m_tick, &QTimer::timeout, this, &ScriptService::tick);
    m_tick.start();
    m_spare = prepareWorker();
}
ScriptService::~ScriptService() {
    m_shuttingDown = true;
    if (m_toolbar) {
        QSettings prefs;
        prefs.setValue("scripts/toolbar/layout", m_window->saveState(1));
        prefs.setValue("scripts/toolbar/floating", m_toolbar->isFloating());
        prefs.setValue("scripts/toolbar/geometry", m_toolbar->saveGeometry());
        prefs.setValue("scripts/toolbar/visible", !m_toolbar->isHidden());
    }
    // All shared Lua documents must close before their runtime, including
    // documents opened by external clients rather than the menu singleton.
    for (auto *window : m_window->findChildren<LuaCreatorWindow*>())
        if (window->service() == this) delete window;
    delete m_manager;
    delete m_toolbarEditor;
    delete m_commandPicker;
    stopAll();
    for (auto *process : m_buffers.keys()) {
        process->disconnect(this);
        process->kill();
        process->waitForFinished(250);
    }
    for (auto &run : m_runs) {
        for (auto &job : run->jobs) {
            job->cancel = true;
            if (job->future.valid())
                job->future.wait();
        }
        pollIo(run.get());
    }
    if (service == this)
        service = nullptr;
    scripts::contextAppender = {};
}
ScriptService::Json ScriptService::settings(const QString &id) const {
    try {
        const auto value = Json::parse(
            QSettings().value("scripts/extensions/" + id, "{}").toString().toStdString());
        return value.is_object() ? value : Json::object();
    } catch (...) {
        return Json::object();
    }
}
void ScriptService::saveSettings(const QString &id, const Json &value) {
    QSettings().setValue("scripts/extensions/" + id, QString::fromStdString(value.dump()));
    for (const auto &run : m_runs)
        if (!run->complete && run->package.id() == id && !run->preview) {
            if (!value.value("enabled", true))
                finish(run.get(), tr("Extension disabled"));
            else if (value.value("permissionEpoch", 0) != run->grants.value("permissionEpoch", 0))
                finish(run.get(), tr("Permissions revoked"));
            else if (run->assistantMode != "mcp") {
                run->grants["permissions"] = value.value("permissions", Json::array());
                run->grants["executables"] = value.value("executables", Json::array());
                if (!run->grants.contains("paths"))
                    run->grants["paths"] = Json::array();
                for (const auto &path : value.value("paths", Json::array()))
                    if (std::find(run->grants["paths"].begin(), run->grants["paths"].end(), path) ==
                        run->grants["paths"].end())
                        run->grants["paths"].push_back(path);
            }
        }
    emit changed();
}
QProcess *ScriptService::prepareWorker() {
    auto *process = new QProcess(this);
    m_buffers.insert(process, {});
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process] { receive(process); });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const auto text = QString::fromUtf8(process->readAllStandardError()).left(8192);
        for (const auto &run : m_runs)
            if (run->process == process)
                addLog(run.get(), text);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        for (const auto &run : m_runs)
            if (run->process == process)
                finish(run.get(), process->errorString());
        m_workerRetryAt = QDateTime::currentMSecsSinceEpoch() + 5000;
        if (m_spare == process)
            m_spare = nullptr;
        m_buffers.remove(process);
        process->deleteLater();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int, QProcess::ExitStatus) {
                if (!process->property("script.ready").isValid())
                    m_workerRetryAt = QDateTime::currentMSecsSinceEpoch() + 5000;
                for (const auto &run : m_runs)
                    if (run->process == process && !run->complete)
                        finish(run.get(), tr("Lua worker stopped unexpectedly"));
                if (m_spare == process)
                    m_spare = nullptr;
                m_buffers.remove(process);
                process->deleteLater();
            });
#ifdef Q_OS_WIN
    connect(process, &QProcess::started, this, [process] {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = 512u * 1024u * 1024u;
        HANDLE child =
            OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process->processId()));
        const bool ok = job && child &&
                        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                sizeof(limits)) &&
                        AssignProcessToJobObject(job, child);
        if (child)
            CloseHandle(child);
        if (!ok) {
            if (job)
                CloseHandle(job);
            process->kill();
            return;
        }
        QObject::connect(process, &QObject::destroyed, [job] { CloseHandle(job); });
    });
#endif
    process->start(ScriptLibrary::workerPath(), {});
    return process;
}
void ScriptService::send(QProcess *process, const vlt::script::Message &message) {
    if (!process || process->state() == QProcess::NotRunning)
        throw std::runtime_error("Lua worker is unavailable");
    const auto bytes = vlt::script::encode(message);
    if (process->bytesToWrite() > 4 * 1024 * 1024)
        throw std::runtime_error("Lua worker is not accepting messages");
    process->write(bytes.data(), qint64(bytes.size()));
}
void ScriptService::receive(QProcess *process) {
    if (!m_buffers.contains(process) || process->property("script.receiving").toBool())
        return;
    QPointer<QProcess> aliveProcess = process;
    process->setProperty("script.receiving", true);
    // File/permission dialogs can spin a nested event loop. Never retain a QHash
    // reference across dispatch: worker exit or preparing the spare changes it.
    QByteArray buffer;
    buffer.swap(m_buffers[process]);
    buffer += process->readAllStandardOutput();
    const auto restoreBuffer = qScopeGuard([&] {
        if (!aliveProcess || !m_buffers.contains(process))
            return;
        m_buffers[process] = std::move(buffer);
        process->setProperty("script.receiving", false);
        if (process->bytesAvailable())
            QTimer::singleShot(0, this, [this, aliveProcess] {
                if (aliveProcess)
                    receive(aliveProcess);
            });
    });
    try {
        if (buffer.size() > 2 * (vlt::script::maxJsonBytes + vlt::script::maxBinaryBytes + 8))
            throw std::runtime_error("Worker exceeded IPC receive limit");
        int handled = 0;
        while (buffer.size() >= 8) {
            if (++handled > 128) {
                QTimer::singleShot(0, this, [this, process] {
                    if (m_buffers.contains(process))
                        receive(process);
                });
                break;
            }
            const auto js = vlt::script::readSize(buffer.data()),
                       bs = vlt::script::readSize(buffer.data() + 4);
            if (!js || js > vlt::script::maxJsonBytes || bs > vlt::script::maxBinaryBytes)
                throw std::runtime_error("Invalid worker frame");
            if (buffer.size() < 8 + js + bs)
                break;
            vlt::script::Message message{
                Json::parse(std::string(buffer.constData() + 8, js)),
                {buffer.constData() + 8 + js, buffer.constData() + 8 + js + bs}};
            buffer.remove(0, 8 + js + bs);
            std::shared_ptr<ScriptRun> active;
            for (const auto &candidate : m_runs)
                if (candidate->process == process && !candidate->complete) {
                    active = candidate;
                    break;
                }
            auto *run = active.get();
            const auto type = message.json.value("type", "");
            if (type == "ready") {
                if (message.json.value("protocol", 0) != 1)
                    throw std::runtime_error("Worker protocol mismatch");
                process->setProperty("script.ready", true);
                if (run)
                    launch(run);
            } else if (run) {
                if (type == "call")
                    dispatch(run, message);
                else if (type == "log")
                    addLog(run, QString::fromStdString(message.json.value("text", "")));
                else if (type == "error")
                    finish(run, QString::fromStdString(message.json.value("error", "Lua error")));
                else if (type == "idle") {
                    run->idle = true;
                    if (message.json.contains("value"))
                        run->result = message.json["value"];
                    bool jobs = false;
                    for (const auto &j : run->jobs)
                        jobs |= !j->complete;
                    if (run->windows.isEmpty() && run->subscriptions.isEmpty() && !jobs)
                        finish(run, {}, true);
                } else
                    throw std::runtime_error("Unknown worker message");
            }
            if (!aliveProcess || !m_buffers.contains(process))
                return;
        }
    } catch (const std::exception &e) {
        for (const auto &run : m_runs)
            if (run->process == process)
                finish(run.get(), QString::fromUtf8(e.what()));
        if (aliveProcess)
            process->kill();
    }
}
daw::EngineController *ScriptService::controller(ScriptRun *run) const {
    auto *c =
        run->preview ? run->previewController.get() : m_bindings.resolve(run->projectId).controller;
    return c && c->projectGeneration() == run->generation ? c : nullptr;
}
void ScriptService::authorize(ScriptRun *run, const Json &method) {
    if (run->complete || !controller(run))
        throw std::runtime_error("Project closed or replaced");
    const auto permission = method.value("permission", "");
    const bool edit = method.value("edit", false);
    if (edit && controller(run)->hasCloudProjectBinding())
        throw std::runtime_error("Collaborative projects are read-only for extensions");
    if (!run->assistantMode.isEmpty() && run->assistantMode != "do" &&
        run->assistantMode != "compose" && run->assistantMode != "mcp" &&
        (edit || permission == "files.write" || permission == "network" || permission == "process"))
        throw std::runtime_error("Assistant mode does not allow this operation");
    if (run->assistantMode == "mcp" && (permission == "network" || permission == "process"))
        throw std::runtime_error("MCP grants DAW access only; network and system processes are unavailable");
    if (!permission.empty()) {
        const auto declared = run->package.manifest.value("permissions", Json::array());
        const auto allowed = run->grants.value("permissions", Json::array());
        if (std::find(declared.begin(), declared.end(), permission) == declared.end() ||
            std::find(allowed.begin(), allowed.end(), permission) == allowed.end())
            throw std::runtime_error("Permission not granted: " + permission);
    }
}
ScriptService::Json ScriptService::activeCallbacks(const ScriptRun *run) const {
    QSet<int> references;
    for (const auto &subscription : run->subscriptions)
        references.insert(subscription.callback);
    for (const auto &callbacks : run->windowCallbacks)
        for (int callback : callbacks)
            references.insert(callback);
    for (const auto &event : run->pending)
        references.insert(event.callback);
    Json result = Json::array();
    for (int reference : references)
        result.push_back(reference);
    return result;
}
void ScriptService::reply(ScriptRun *run, std::uint64_t id, const Json &result,
                          const std::vector<char> &binary) {
    if (run->hostWaitStarted >= 0) {
        run->hostWaitMilliseconds += run->elapsed.elapsed() - run->hostWaitStarted;
        run->hostWaitStarted = -1;
    }
    if (!run->complete)
        try {
            send(run->process, {{{"type", "reply"},
                                 {"id", id},
                                 {"ok", true},
                                 {"value", result},
                                 {"callbacks", activeCallbacks(run)}},
                                binary});
        } catch (const std::exception &e) {
            failReply(run, id,
                      QString::fromUtf8(e.what()) +
                          tr(" — request a smaller result using SDK filters"));
        }
}
void ScriptService::failReply(ScriptRun *run, std::uint64_t id, const QString &error) {
    if (run->hostWaitStarted >= 0) {
        run->hostWaitMilliseconds += run->elapsed.elapsed() - run->hostWaitStarted;
        run->hostWaitStarted = -1;
    }
    if (!run->complete)
        try {
            send(run->process, {{{"type", "reply"},
                                 {"id", id},
                                 {"ok", false},
                                 {"error", error.toStdString()},
                                 {"callbacks", activeCallbacks(run)}},
                                {}});
        } catch (const std::exception &e) {
            finish(run, QString::fromUtf8(e.what()));
        }
}
ScriptService::Json ScriptService::parameterCall(ScriptRun *run, const std::string &method,
                                                 const Json &a) {
    auto &c = *controller(run);
    if (!run->preview)
        return method == "parameters.list"
                   ? daw::scripting::parameters(c, a)
                   : daw::scripting::parameter(c, a, method == "parameters.set");
    const auto track = a.at("track").get<std::string>(), device = a.at("device").get<std::string>(),
               key = track + "/" + device;
    const auto *slot = c.insertModel(track, device);
    if (!slot || !run->previewParameters.contains(key) ||
        slot->uid != run->previewParameters[key]["uid"].get_ref<const std::string &>())
        throw std::runtime_error("Preview device has no captured parameter metadata");
    const auto &entry = run->previewParameters[key];
    auto parameters = entry["parameters"];
    for (auto &p : parameters) {
        p["identity"] = entry["identity"];
        p["value"] = c.insertParameter(track, device, p["id"]);
    }
    if (method == "parameters.list") {
        const auto offset = a.value("offset", std::int64_t(0));
        const auto limit = a.value("limit", std::int64_t(256));
        if (offset < 0 || limit < 1 || limit > 4096)
            throw std::runtime_error("Invalid parameter page");
        Json page = Json::array();
        size_t matched = 0;
        for (const auto &parameter : parameters) {
            if (a.contains("parameter") && a["parameter"] != parameter["id"])
                continue;
            if (matched++ >= size_t(offset) && page.size() < size_t(limit))
                page.push_back(parameter);
        }
        return page;
    }
    if (a.at("identity") != entry["identity"])
        throw std::runtime_error("Preview device identity changed");
    for (const auto &p : parameters)
        if (p["id"] == a.at("parameter")) {
            if (method == "parameters.set") {
                const double value = a.at("value").get<double>();
                if (p["readOnly"] || !std::isfinite(value) || value < p["min"].get<double>() ||
                    value > p["max"].get<double>())
                    throw std::runtime_error("Invalid or read-only preview parameter");
                const double before = p["value"];
                c.setInsertParameter(track, device, p["id"], value);
                c.commitInsertParameterEdit(track, device, p["id"], before, "Preview parameter");
            }
            return c.insertParameter(track, device, p["id"]);
        }
    throw std::runtime_error("Unknown preview parameter");
}
void ScriptService::dispatch(ScriptRun *run, const vlt::script::Message &message) {
    run->hostWaitStarted = run->elapsed.elapsed();
    const auto id = message.json.at("id").get<std::uint64_t>();
    try {
        const auto method = message.json.at("method").get<std::string>();
        const auto *spec = vlt::script::apiMethod(method);
        if (!spec)
            throw std::runtime_error("Unknown SDK method");
        const auto &a = message.json.at("arguments");
        const auto validation = vlt::script::validateArguments(*spec, a);
        if (!validation.empty())
            throw std::runtime_error(validation);
        authorize(run, *spec);
        if (run->assistantMode == "mcp" && (method == "ui.chooseFile" || method == "commands.run"))
            throw std::runtime_error("Use explicit fileAccess and parameterized workspace/project operations from MCP; modal commands are unavailable");
        auto &c = *controller(run);
        Json value;
        if (method == "context.get")
            value = run->context;
        else if (method == "project.files") {
            value = ioCall(run, method, a, {}).json;
        } else if (method == "project.get") {
            value = daw::scripting::projectSnapshot(c, a);
            if (run->preview) {
                const auto identities = [&](Json &channel) {
                    for (auto &d : channel["devices"]) {
                        const auto key =
                            channel["id"].get<std::string>() + "/" + d["id"].get<std::string>();
                        if (run->previewParameters.contains(key))
                            d["identity"] = run->previewParameters[key]["identity"];
                    }
                };
                for (auto &track : value["tracks"])
                    identities(track);
                identities(value["master"]);
            }
        } else if (method == "devices.list")
            value = daw::scripting::deviceCatalog(c, a);
        else if (method == "time.convert") {
            const auto from = a.at("from").get<std::string>(), to = a.at("to").get<std::string>();
            const double input = a.at("value"), rate = a.value("sampleRate", 0.0);
            if (!std::isfinite(input) || input < 0)
                throw std::runtime_error("Invalid time");
            if ((from == "frames" || to == "frames") &&
                (!std::isfinite(rate) || rate < 8000 || rate > 768000))
                throw std::runtime_error("Frames require an explicit sampleRate");
            const auto seconds = [&] {
                if (from == "beats")
                    return input * 60 / c.tempo();
                if (from == "seconds")
                    return input;
                if (from == "frames")
                    return input / rate;
                throw std::runtime_error("Unknown time unit");
            }();
            if (to != "beats" && to != "seconds" && to != "frames")
                throw std::runtime_error("Unknown time unit");
            const double converted = to == "beats"    ? seconds * c.tempo() / 60
                                     : to == "frames" ? seconds * rate
                                                      : seconds;
            if (!std::isfinite(converted) ||
                (to == "frames" && converted >= double(std::numeric_limits<std::int64_t>::max())))
                throw std::runtime_error("Time exceeds the supported range");
            value = to == "frames" ? Json(std::llround(converted)) : Json(converted);
        } else if (method == "project.apply") {
            auto edits = a;
            QStringList createdMedia;
            auto discardMedia = qScopeGuard([&] {
                for (const auto &file : createdMedia)
                    QFile::remove(file);
            });
            for (auto &op : edits.at("operations"))
                if (op.value("op", "") == "clip.importAudio") {
                    authorize(run, *vlt::script::apiMethod("files.read"));
                    const auto path = QString::fromStdString(op.at("path"));
                    if (!permittedPath(run, path, false))
                        throw std::runtime_error("Audio import requires a granted resource");
                    op["path"] = persistImport(run, path, createdMedia).toStdString();
                }
            value = daw::scripting::applyEdits(c, edits);
            discardMedia.dismiss();
            if (!run->preview)
                m_bindings.edited(run->projectId);
            else
                for (const auto &op : a["operations"])
                    if (op.at("op").get<std::string>().starts_with("device."))
                        run->previewParameters = Json::object();
        } else if (method == "parameters.list")
            value = parameterCall(run, method, a);
        else if (method == "parameters.get" || method == "parameters.set") {
            value = parameterCall(run, method, a);
            if (method == "parameters.set" && !run->preview)
                m_bindings.edited(run->projectId);
        } else if (method == "commands.list") {
            value = Json::array();
            for (const auto &command :
                 m_shortcuts->search(QString::fromStdString(a.value("query", ""))))
                value.push_back(
                    {{"id", command.id.toStdString()},
                     {"label", command.label.toStdString()},
                     {"description", command.metadata.description.toStdString()},
                     {"available",
                      command.action && command.action->isEnabled() && command.action->isVisible()},
                     {"callable",
                      !command.id.startsWith("script.") &&
                          command.metadata.risk != ShortcutManager::Risk::Unknown &&
                          command.metadata.risk != ShortcutManager::Risk::ExternalSideEffect}});
        } else if (method == "commands.run") {
            if (run->preview)
                throw std::runtime_error("Global commands cannot run in a preview");
            const auto command = QString::fromStdString(a.at("id").get<std::string>());
            // Never let a window switch its command onto whichever tab is now active.
            if (m_bindings.current().id != run->projectId)
                throw std::runtime_error(
                    "Activate this tool's project before invoking a global command");
            const auto *entry = m_shortcuts->command(command);
            if (!entry || entry->metadata.risk == ShortcutManager::Risk::Unknown ||
                entry->metadata.risk == ShortcutManager::Risk::ExternalSideEffect)
                throw std::runtime_error("Command is not exposed to extensions");
            if (command.startsWith("script."))
                throw std::runtime_error("Recursive script commands are not allowed");
            if (!m_bindings.command(command, run->projectId))
                throw std::runtime_error("Command unavailable");
            value = true;
        } else if (method.starts_with("workspace.")) {
            if (run->preview && method != "workspace.get")
                throw std::runtime_error("Workspace navigation is unavailable in a preview");
            if (method != "workspace.get" && m_bindings.current().id != run->projectId)
                throw std::runtime_error(
                    "Activate this tool's project before changing the workspace");
            if (method == "workspace.get" && run->preview)
                value = {{"preview", true},
                         {"active", false},
                         {"panels", Json::object()},
                         {"selection", run->context.value("selection", Json::object())}};
            else {
                if (!m_bindings.workspace)
                    throw std::runtime_error("Workspace integration unavailable");
                auto request = a;
                request["method"] = method;
                value = m_bindings.workspace(run->projectId, request);
            }
        } else if (method == "transport.get") {
            value = {{"playing", c.isPlaying()},
                     {"beat", c.positionSeconds() * c.tempo() / 60.0},
                     {"tempo", c.tempo()},
                     {"loop", c.isLoopEnabled()},
                     {"loopStart", c.loopStartSeconds() * c.tempo() / 60.0},
                     {"loopEnd", c.loopEndSeconds() * c.tempo() / 60.0}};
        } else if (method == "transport.set") {
            const auto finite = [&](const char *key, double low, double high) {
                double v = a.at(key);
                if (!std::isfinite(v) || v < low || v > high)
                    throw std::runtime_error("Invalid transport value");
                return v;
            };
            if (a.contains("tempo"))
                finite("tempo", 20, 999);
            if (a.contains("beat"))
                finite("beat", 0, 1e9);
            if (a.contains("loopStart") != a.contains("loopEnd"))
                throw std::runtime_error("Loop requires start and end");
            if (a.contains("loopStart") && finite("loopStart", 0, 1e9) >= finite("loopEnd", 0, 1e9))
                throw std::runtime_error("Loop end must follow its start");
            if (a.contains("tempo"))
                c.setTempo(finite("tempo", 20, 999));
            if (a.contains("beat"))
                c.seekSeconds(finite("beat", 0, 1e9) * 60 / c.tempo());
            if (a.contains("loopStart") && a.contains("loopEnd"))
                c.setLoopRangeSeconds(finite("loopStart", 0, 1e9) * 60 / c.tempo(),
                                      finite("loopEnd", 0, 1e9) * 60 / c.tempo());
            if (a.contains("loop"))
                c.setLoopEnabled(a.at("loop"));
            if (a.contains("playing")) {
                if (a.at("playing").get<bool>())
                    c.play();
                else
                    c.stop();
            }
            value = true;
        } else if (method.starts_with("ui."))
            value = uiCall(run, method, a);
        else if (method == "events.on" || method == "timers.start") {
            if (run->subscriptions.size() >= 128)
                throw std::runtime_error("Subscription limit reached");
            ScriptRun::Subscription s;
            s.id = uuid();
            s.name = QString::fromStdString(a.value("event", "timer"));
            if (!QStringList{"project", "selection", "transport", "parameter", "job", "timer"}
                     .contains(s.name))
                throw std::runtime_error("Unknown event");
            if (s.name == "project" || s.name == "selection" || s.name == "transport" ||
                s.name == "parameter")
                authorize(run, *vlt::script::apiMethod("project.get"));
            s.callback = a.at("callback").at("$callback");
            s.arguments = a;
            if (s.name == "timer")
                s.interval = std::clamp(a.value("milliseconds", 100), 30, 3600000);
            s.due = run->elapsed.elapsed() + s.interval;
            run->subscriptions.append(s);
            value = s.id.toStdString();
        } else if (method == "events.off" || method == "timers.stop") {
            const auto sid = QString::fromStdString(a.at("id").get<std::string>());
            run->subscriptions.removeIf([&](const auto &s) { return s.id == sid; });
            run->pending.removeIf([&](const auto &event) {
                return event.key == sid || event.key.startsWith(sid + "/");
            });
            value = true;
        } else if (method == "storage.get")
            value = settings(run->package.id()).value("storage", Json::object());
        else if (method == "storage.set") {
            if (a["value"].dump().size() > 256 * 1024)
                throw std::runtime_error("Settings exceed 256 KiB");
            if (!run->preview) {
                auto prefs = settings(run->package.id());
                prefs["storage"] = a["value"];
                saveSettings(run->package.id(), prefs);
            }
            value = true;
        } else if (method == "log") {
            addLog(run, QString::fromStdString(a.at("text")));
            value = true;
        } else if (method == "network.request") {
            const QUrl url(QString::fromStdString(a.at("url")));
            if (run->preview || url.scheme() != "https" || !url.userInfo().isEmpty())
                throw std::runtime_error("Only HTTPS URLs are allowed outside preview");
            auto *manager = new QNetworkAccessManager(this);
            QNetworkRequest request(url);
            request.setTransferTimeout(15000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            auto *response = manager->get(request);
            response->setReadBufferSize(1024 * 1024);
            connect(response, &QNetworkReply::downloadProgress, response,
                    [response](qint64 received, qint64 total) {
                        if (received > 1024 * 1024 || total > 1024 * 1024)
                            response->abort();
                    });
            connect(this, &ScriptService::runChanged, response,
                    [alive = run->shared_from_this(), response](const QString &changed) {
                        if (changed == alive->id && alive->complete)
                            response->abort();
                    });
            connect(
                response, &QNetworkReply::finished, this,
                [this, alive = run->shared_from_this(), id, response, manager] {
                    auto *run = alive.get();
                    if (!run->complete) {
                        try {
                            authorize(run, *vlt::script::apiMethod("network.request"));
                            if (response->error() != QNetworkReply::NoError)
                                failReply(run, id, response->errorString());
                            else
                                reply(
                                    run, id,
                                    {{"status",
                                      response->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                          .toInt()},
                                     {"body", response->readAll().toStdString()}});
                        } catch (const std::exception &e) {
                            failReply(run, id, QString::fromUtf8(e.what()));
                        }
                    }
                    manager->deleteLater();
                });
            return;
        } else {
            const auto response = ioCall(run, method, a, message.binary);
            reply(run, id, response.json, response.binary);
            return;
        }
        reply(run, id, value);
    } catch (const std::exception &e) {
        failReply(run, id, QString::fromUtf8(e.what()));
    }
}
void ScriptService::launch(ScriptRun *run) {
    if (run->started || run->complete)
        return;
    if (!run->process->property("script.ready").toBool())
        return;
    try {
        const auto action = run->command.section('/', -1).toStdString();
        std::string entry;
        for (const auto &c : run->package.manifest["commands"])
            if (c["id"] == action)
                entry = c["entry"];
        run->started = true;
        run->idle = false;
        run->process->setProperty("script.ready", false);
        run->handlerStarted = run->elapsed.elapsed();
        run->hostWaitMilliseconds = 0;
        run->hostWaitStarted = -1;
        send(run->process,
             {{{"type", "run"}, {"sources", run->package.sources}, {"entry", entry}}, {}});
    } catch (const std::exception &e) {
        finish(run, QString::fromUtf8(e.what()));
    }
}
QString ScriptService::runPackage(const ScriptPackage &suppliedPackage, const QString &action,
                                  const Json &context, bool preview,
                                  const QString &assistantMode) try {
    // Permission dialogs run a nested event loop; keep a snapshot if the library reloads.
    const ScriptPackage package = suppliedPackage;
    QString error;
    if (!context.is_object())
        return {};
    if (!ScriptLibrary::validate(package, error, false))
        return {};
    if (std::none_of(package.manifest["commands"].begin(), package.manifest["commands"].end(),
                     [&](const auto &command) { return command.at("id") == action.toStdString(); }))
        return {};
    const auto project = context.contains("project")
                             ? m_bindings.resolve(QString::fromStdString(context.at("project")))
                             : m_bindings.current();
    if (!project.controller)
        return {};
    const auto pinnedGeneration = project.controller->projectGeneration();
    if (context.contains("generation") &&
        context["generation"] != project.controller->projectGeneration())
        return {};
    if (context.contains("target")) {
        const auto &target = context["target"];
        if (target.contains("device") && target.contains("track") && target.contains("identity") &&
            target["identity"] != daw::scripting::parameterIdentity(
                                      *project.controller, target["track"], target["device"]))
            return {};
    }
    for (const auto &run : m_runs)
        if (!run->complete && run->preview == preview &&
            run->command == package.id() + "/" + action && run->projectId == project.id &&
            controller(run.get()) && !run->windows.isEmpty()) {
            for (auto widget : run->windows)
                if (widget) {
                    widget->show();
                    widget->raise();
                    widget->activateWindow();
                }
            return run->id;
        }
    if (!preview && assistantMode != "mcp" && !approvePermissions(package, assistantMode.isEmpty()))
        return {};
    const auto resolved = m_bindings.resolve(project.id);
    if (resolved.controller != project.controller || !resolved.controller ||
        resolved.controller->projectGeneration() != pinnedGeneration)
        return {};
    int active = 0;
    for (const auto &r : m_runs)
        if (!r->complete)
            ++active;
    if (active >= 16)
        return {};
    auto run = std::make_shared<ScriptRun>();
    run->id = uuid();
    run->command = package.id() + "/" + action;
    run->package = package;
    run->projectId = project.id;
    run->projectName = project.name;
    run->assistantMode = assistantMode;
    run->grants = settings(package.id());
    if (assistantMode == "mcp") {
        // Ephemeral capabilities belong to this connection, never to installed-package settings.
        run->grants = {{"permissions", {"project.read", "project.write", "files.read", "files.write"}},
                       {"paths", Json::array()}};
        for (const auto &path : context.value("fileAccess", Json::array()))
            grantPath(run.get(), QString::fromStdString(path.at("path")),
                      path.value("write", false), path.value("folder", false));
    }
    run->preview = preview;
    run->context = m_bindings.context(project.id);
    for (const auto &[key, value] : context.items())
        run->context[key] = value;
    run->context["project"] = project.id.toStdString();
    run->context["preview"] = preview;
    if ((preview || run->package.directory.isEmpty()) && !package.resources.empty()) {
        run->package.directory = run->temporary.filePath("package");
        QDir().mkpath(run->package.directory);
        for (const auto &[path, encoded] : package.resources.items()) {
            const auto destination =
                QDir(run->package.directory).filePath(QString::fromStdString(path));
            QDir().mkpath(QFileInfo(destination).path());
            QFile file(destination);
            if (!file.open(QIODevice::WriteOnly))
                return {};
            file.write(
                QByteArray::fromBase64(QByteArray::fromStdString(encoded.get<std::string>())));
        }
    }
    run->context["resourceDirectory"] = run->package.directory.toStdString();
    run->context["temporaryDirectory"] = run->temporary.path().toStdString();
    if (!preview && context.value("type", "") == "file" && context.contains("target") &&
        context["target"].contains("path")) {
        const auto path = QString::fromStdString(context["target"]["path"]);
        grantPath(run.get(), path, false, QFileInfo(path).isDir());
    }
    for (const auto &c : package.manifest["commands"])
        if (c["id"] == action.toStdString())
            run->background = c.value("background", false);
    if (preview) {
        run->previewController = std::make_shared<daw::EngineController>();
        run->previewController->initializeDocumentOnly();
        // A document copy owns separate clip content and a separate undo stack.
        run->previewController->restoreProject(project.controller->project(), "Preview fixture");
        // Copy parameter descriptions and plain values, without instantiating the
        // user's plugins or sharing their live processor with the preview.
        const auto snapshot = daw::scripting::projectSnapshot(
            *project.controller,
            {{"includeEvents", false}, {"clipLimit", 0}, {"trackLimit", 4096}});
        size_t previewParameterBytes = 0;
        const auto captureParameters = [&](const Json &channel) {
            const auto track = channel["id"].get<std::string>();
            for (const auto &device : channel["devices"]) {
                const auto id = device["id"].get<std::string>();
                if (!project.controller->hasInsert(track, id))
                    continue;
                const auto *slot = project.controller->insertModel(track, id);
                if (!slot)
                    continue;
                Json parameters = Json::array();
                for (size_t offset = 0;; offset += 128) {
                    auto page = daw::scripting::parameters(
                        *project.controller,
                        {{"track", track}, {"device", id}, {"offset", offset}, {"limit", 128}});
                    previewParameterBytes += page.dump().size();
                    if (previewParameterBytes > 8 * 1024 * 1024)
                        throw std::runtime_error("Preview parameter metadata exceeds 8 MiB");
                    const auto count = page.size();
                    parameters.insert(parameters.end(), page.begin(), page.end());
                    if (count < 128)
                        break;
                }
                const auto identity = "preview:" + run->id.toStdString() + "/" + track + "/" + id;
                for (const auto &p : parameters)
                    run->previewController->setInsertParameter(track, id, p["id"], p["value"]);
                run->previewParameters[track + "/" + id] = {
                    {"uid", slot->uid}, {"identity", identity}, {"parameters", parameters}};
            }
        };
        for (const auto &track : snapshot["tracks"])
            captureParameters(track);
        captureParameters(snapshot["master"]);
        if (run->context.contains("parameter")) {
            auto &address = run->context["parameter"];
            const auto key = address.value("track", "") + "/" + address.value("device", "");
            if (run->previewParameters.contains(key))
                address["identity"] = run->previewParameters[key]["identity"];
        }
        run->grants = {
            {"permissions", {"project.read", "project.write", "files.read", "files.write"}},
            {"paths", Json::array()}};
        const auto input = run->temporary.filePath("input"),
                   output = run->temporary.filePath("output");
        QDir().mkpath(input);
        QDir().mkpath(output);
        const auto path = QDir(input).filePath("preview.wav");
        SF_INFO info{};
        info.channels = 1;
        info.samplerate = 48000;
        info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
#ifdef Q_OS_WIN
        auto *fixture =
            sf_wchar_open(reinterpret_cast<const wchar_t *>(path.utf16()), SFM_WRITE, &info);
#else
        auto *fixture = sf_open(path.toUtf8().constData(), SFM_WRITE, &info);
#endif
        if (fixture) {
            std::vector<float> samples(48000, 0);
            for (size_t i = 4800; i < 43200; ++i)
                samples[i] = float(.25 * std::sin(2 * 3.141592653589793 * 440 * i / 48000));
            sf_writef_float(fixture, samples.data(), 48000);
            sf_close(fixture);
        }
    }
    auto *c = preview ? run->previewController.get() : project.controller;
    run->generation = c->projectGeneration();
    run->revision = c->projectRevision();
    run->elapsed.start();
    run->process = m_spare ? m_spare : prepareWorker();
    m_spare = nullptr;
    m_runs.append(run);
    launch(run.get());
    emit runChanged(run->id);
    return run->id;
} catch (const std::exception &error) {
    auto failed = std::make_shared<ScriptRun>();
    failed->id = uuid();
    failed->command = tr("Extension launch");
    failed->complete = true;
    failed->completedAt = QDateTime::currentMSecsSinceEpoch();
    failed->error = QString::fromUtf8(error.what());
    failed->log.append(failed->error);
    m_runs.append(failed);
    emit runChanged(failed->id);
    return failed->id;
}
QString ScriptService::run(const QString &command, const Json &context, const Json &arguments,
                           const QString &mode) {
    const auto normalized = command.startsWith("script.") ? command.mid(7) : command;
    const auto extension = normalized.section('/', 0, 0), action = normalized.section('/', 1);
    for (const auto &package : m_packages)
        if (package.id() == extension) {
            const auto prefs = settings(extension);
            if (!prefs.value("enabled", true))
                return {};
            auto invocation = context;
            invocation["arguments"] = arguments;
            return runPackage(package, action, invocation, false, mode);
        }
    return {};
}
ScriptService::Json ScriptService::invocationContext(daw::EngineController *controller) const {
    const auto project = m_bindings.current();
    if (!controller || project.controller != controller)
        return Json::object();
    auto context = m_bindings.context(project.id);
    context["project"] = project.id.toStdString();
    context["generation"] = controller->projectGeneration();
    return context;
}
void ScriptService::addLog(ScriptRun *run, const QString &text) {
    run->log.append(text.left(4096));
    while (run->log.size() > 128)
        run->log.removeFirst();
    run->logDirty = true;
}
void ScriptService::finish(ScriptRun *run, const QString &error, bool recycle) {
    if (run->complete)
        return;
    if (controller(run))
        for (auto binding : run->bindings)
            if (binding)
                binding->finishAll();
    run->complete = true;
    run->completedAt = QDateTime::currentMSecsSinceEpoch();
    run->error = error;
    if (!error.isEmpty())
        addLog(run, error);
    run->subscriptions.clear();
    run->pending.clear();
    const auto windows = run->windows;
    run->windows.clear();
    run->windowContents.clear();
    for (auto widget : windows)
        if (widget)
            delete widget;
    run->widgets.clear();
    run->windowCallbacks.clear();
    run->bindings.clear();
    cleanupIo(run);
    run->package.sources = Json::object();
    run->package.resources = Json::object();
    run->previewParameters = Json::object();
    run->previewController.reset();
    QProcess *process = run->process;
    run->process = nullptr;
    if (process) {
        if (recycle && run->idle && !m_shuttingDown && !m_spare &&
            process->state() == QProcess::Running) {
            m_spare = process;
            try {
                send(process, {{{"type", "reset"}}, {}});
            } catch (...) {
                m_spare = nullptr;
                process->kill();
            }
        } else
            process->kill();
    }
    if (run->autostart && !error.isEmpty()) {
        auto prefs = settings(run->package.id());
        prefs["autostartSuspended"] = true;
        saveSettings(run->package.id(), prefs);
    }
    emit runChanged(run->id);
}
void ScriptService::stop(const QString &id) {
    for (const auto &r : m_runs)
        if (r->id == id) {
            r->autostart = false;
            finish(r.get(), tr("Stopped"));
        }
}
void ScriptService::stopAll() {
    for (const auto &r : m_runs)
        if (!r->complete) {
            r->autostart = false;
            finish(r.get(), tr("Stopped"));
        }
}
void ScriptService::closeProject(const QString &id) {
    for (const auto &r : m_runs)
        if (r->projectId == id && !r->complete) {
            r->autostart = false;
            finish(r.get(), tr("Project closed"));
        }
}
ScriptService::Json ScriptService::status(const QString &id) const {
    Json result = Json::array();
    for (const auto &run : m_runs)
        if (id.isEmpty() || id == run->id) {
            Json log = Json::array();
            if (!id.isEmpty())
                for (const auto &line : run->log)
                    log.push_back(line.toStdString());
            Json item{{"id", run->id.toStdString()},
                      {"command", run->command.toStdString()},
                      {"project", run->projectName.toStdString()},
                      {"complete", run->complete},
                      {"error", run->error.toStdString()}};
            if (!id.isEmpty()) {
                item["result"] = run->result;
                item["log"] = log;
            }
            item["jobs"] = Json::array();
            if (!id.isEmpty())
                for (const auto &job : run->jobs)
                    item["jobs"].push_back({{"id", job->id.toStdString()},
                                            {"complete", job->complete},
                                            {"progress", job->progress.load()},
                                            {"result", job->result},
                                            {"error", job->error.toStdString()}});
            if (!id.isEmpty())
                return item;
            result.push_back(item);
        }
    return result;
}
ScriptService::Json ScriptService::describe(const QString &query) const {
    Json result = Json::array();
    for (const auto &p : m_packages)
        for (const auto &c : p.manifest["commands"]) {
            const auto text = p.name() + " " + QString::fromStdString(c.value("label", ""));
            if (!query.isEmpty() && !text.contains(query, Qt::CaseInsensitive))
                continue;
            auto entry = c;
            entry["id"] = "script." + p.id().toStdString() + "/" + c["id"].get<std::string>();
            entry["extension"] = p.name().toStdString();
            entry["permissions"] = p.manifest.value("permissions", Json::array());
            entry["enabled"] = settings(p.id()).value("enabled", true);
            result.push_back(entry);
        }
    return result;
}
void ScriptService::event(ScriptRun *run, int callback, const Json &value, const QString &key) {
    if (run->complete)
        return;
    if (!key.isEmpty())
        for (auto &pending : run->pending)
            if (pending.key == key && pending.callback == callback) {
                pending.value = value;
                return;
            }
    if (run->pending.size() < 128)
        run->pending.append({callback, value, key});
}
void ScriptService::tick() {
    if (m_shuttingDown)
        return;
    if (!m_spare && QDateTime::currentMSecsSinceEpoch() >= m_workerRetryAt)
        m_spare = prepareWorker();
    auto runs = m_runs;
    for (const auto &r : runs) {
        if (r->logDirty) {
            r->logDirty = false;
            emit runChanged(r->id);
        }
        auto *run = r.get();
        if (run->complete) {
            pollIo(run);
            continue;
        }
        auto *c = controller(run);
        if (!c) {
            finish(run, tr("Project closed or replaced"));
            continue;
        }
        // Independent watchdog also catches scripts which swallow Lua hook errors with pcall.
        if (run->started && !run->idle && run->hostWaitStarted < 0 &&
            run->elapsed.elapsed() - run->handlerStarted - run->hostWaitMilliseconds > 3000) {
            finish(run, tr("Lua handler exceeded its execution limit"));
            continue;
        }
        pollIo(run);
        run->bindings.removeIf([](const auto &binding) { return binding.isNull(); });
        run->widgets.removeIf(
            [](QHash<QString, QPointer<QWidget>>::iterator it) { return it.value().isNull(); });
        for (auto binding : run->bindings)
            if (binding)
                binding->refresh();
        for (auto &s : run->subscriptions) {
            if (s.name == "project" || s.name == "selection" || s.name == "transport" ||
                s.name == "parameter") {
                try {
                    authorize(run, *vlt::script::apiMethod("project.get"));
                } catch (...) {
                    continue;
                }
            }
            Json value;
            if (s.name == "timer") {
                if (run->elapsed.elapsed() < s.due)
                    continue;
                s.due = run->elapsed.elapsed() + s.interval;
                value = {{"milliseconds", run->elapsed.elapsed()}};
            } else if (s.name == "project")
                value = {{"revision", c->projectRevision()}};
            else if (s.name == "transport")
                value = {{"playing", c->isPlaying()},
                         {"beat", c->positionSeconds() * c->tempo() / 60.0}};
            else if (s.name == "selection")
                value = m_bindings.context(run->projectId);
            else if (s.name == "parameter") {
                try {
                    value = parameterCall(run, "parameters.get", s.arguments);
                } catch (...) {
                    value = nullptr;
                }
            } else
                continue;
            if (s.name == "timer" || s.last != value) {
                s.last = value;
                event(run, s.callback, value, s.id);
            }
        }
        if (run->idle && !run->pending.isEmpty()) {
            auto e = run->pending.takeFirst();
            run->idle = false;
            run->handlerStarted = run->elapsed.elapsed();
            run->hostWaitMilliseconds = 0;
            run->hostWaitStarted = -1;
            try {
                auto callbacks = activeCallbacks(run);
                callbacks.push_back(e.callback);
                send(run->process, {{{"type", "event"},
                                     {"callback", e.callback},
                                     {"value", e.value},
                                     {"callbacks", callbacks}},
                                    {}});
            } catch (const std::exception &e) {
                finish(run, QString::fromUtf8(e.what()));
            }
        }
        if (run->idle && run->pending.isEmpty() && run->windows.isEmpty() &&
            run->subscriptions.isEmpty()) {
            bool active = false;
            for (const auto &job : run->jobs)
                active |= !job->complete;
            if (!active)
                finish(run, {}, true);
        }
    }
    runs.clear();
    // Keep recent results for the manager/AI without retaining unlimited project copies.
    while (m_runs.size() > 128) {
        auto it = std::find_if(m_runs.begin(), m_runs.end(), [](const auto &r) {
            return r->complete && r.use_count() == 1 &&
                   std::all_of(r->jobs.begin(), r->jobs.end(),
                               [](const auto &j) { return j->complete; });
        });
        if (it == m_runs.end())
            break;
        m_runs.erase(it);
    }
}
} // namespace ui
