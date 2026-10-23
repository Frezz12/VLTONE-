#pragma once
#include "EngineController.hpp"
#include "ScriptService.hpp"
#include <QElapsedTimer>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QWidget>
#include <atomic>
#include <future>
#include <limits>
#include <optional>
#include <unordered_map>

#include <sndfile.h>
class RackParameterBinding;
namespace ui {
struct ScriptAudio {
    SNDFILE *file = nullptr;
    QString staged, destination, source;
    int channels = 0, sampleRate = 0;
    qint64 frames = 0;
    qint64 sourceBytes = 0, sourceModified = 0;
    bool writing = false;
    ~ScriptAudio();
};
struct ScriptJob {
    QString id;
    std::atomic<bool> cancel{false};
    std::atomic<double> progress{0};
    std::future<vlt::script::Json> future;
    vlt::script::Json result;
    QString error;
    std::string publishMethod = "files.write";
    std::optional<daw::EngineController::StripSilenceSource> clipSource;
    std::uint64_t revision = 0;
    bool complete = false, notified = false;
};
struct ScriptRun : std::enable_shared_from_this<ScriptRun> {
    using Json = vlt::script::Json;
    QString id, command, projectId, projectName, assistantMode;
    ScriptPackage package;
    Json context = Json::object(), grants = Json::object(), result;
    QStringList log;
    QString error;
    QPointer<QProcess> process;
    quint64 generation = 0, revision = 0;
    bool started = false, idle = false, complete = false, background = false, hadWindow = false,
         autostart = false, preview = false;
    bool logDirty = false;
    qint64 completedAt = 0;
    QElapsedTimer elapsed;
    qint64 handlerStarted = 0, hostWaitStarted = -1, hostWaitMilliseconds = 0;
    std::shared_ptr<daw::EngineController> previewController;
    Json previewParameters = Json::object();
    QHash<QString, QPointer<QWidget>> widgets, windows, windowContents;
    QHash<QString, QList<int>> windowCallbacks;
    QList<QPointer<RackParameterBinding>> bindings;
    struct Subscription {
        QString id, name;
        int callback = 0;
        Json arguments, last;
        qint64 interval = 0, due = 0;
    };
    QList<Subscription> subscriptions;
    struct Event {
        int callback = 0;
        Json value;
        QString key;
    };
    QList<Event> pending;
    std::unordered_map<std::string, std::unique_ptr<ScriptAudio>> audio;
    QList<std::shared_ptr<ScriptJob>> jobs;
    QTemporaryDir temporary;
    QSet<QString> projectFiles;
    quint64 projectFilesRevision = std::numeric_limits<quint64>::max();
    void *processJob = nullptr;
};
} // namespace ui
