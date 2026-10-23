#include "Internal/SamplerInstance.hpp"
#include "Internal/SlicerInstance.hpp"
#include "RenderWorker.hpp"
#include "ScriptRun.hpp"
#include "ScriptService.hpp"
#include "StripSilence.hpp"
#include "model/ClipLibrary.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sndfile.h>

namespace ui {
namespace {
using Json = vlt::script::Json;
QString uniqueId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString text(const Json &a, const char *key) {
    return QString::fromStdString(a.at(key).get<std::string>());
}
QString canonical(const QString &path, bool writing) {
    const QFileInfo info(path);
    if (!info.isAbsolute() || path.contains(QChar(0)))
        return {};
    if (info.exists())
        return info.canonicalFilePath();
    if (!writing)
        return {};
    const auto parent = QFileInfo(info.path()).canonicalFilePath();
    return parent.isEmpty() ? QString() : QDir(parent).filePath(info.fileName());
}
bool within(const QString &path, const QString &root, bool folder) {
    if (path.isEmpty() || root.isEmpty())
        return false;
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    const auto prefix = root.endsWith('/') ? root : root + "/";
    return path.compare(root, sensitivity) == 0 || (folder && path.startsWith(prefix, sensitivity));
}
SNDFILE *openAudio(const QString &path, int mode, SF_INFO *info) {
#ifdef Q_OS_WIN
    return sf_wchar_open(reinterpret_cast<const wchar_t *>(path.utf16()), mode, info);
#else
    return sf_open(path.toUtf8().constData(), mode, info);
#endif
}
struct AudioFile {
    SNDFILE *file = nullptr;
    ~AudioFile() {
        if (file)
            sf_close(file);
    }
};
struct StagedFile {
    QString path;
    bool released = false;
    ~StagedFile() {
        if (!released && !path.isEmpty())
            QFile::remove(path);
    }
};
} // namespace
ScriptAudio::~ScriptAudio() {
    if (file)
        sf_close(file);
    if (!staged.isEmpty())
        QFile::remove(staged);
}
const QSet<QString> &ScriptService::projectFiles(ScriptRun *run) const {
    auto *c = controller(run);
    if (!c || run->preview) {
        run->projectFiles.clear();
        return run->projectFiles;
    }
    if (run->projectFilesRevision == c->projectRevision())
        return run->projectFiles;
    run->projectFiles.clear();
    const auto add = [&](const std::string &path) {
        if (path.empty())
            return;
        const auto resolved = canonical(QString::fromStdString(path), false);
        if (!resolved.isEmpty() && QFileInfo(resolved).isFile())
            run->projectFiles.insert(resolved);
    };
    const auto audioEdit = [&](const daw::AudioEditDocument &edit) {
        for (const auto &source : edit.sources)
            add(source.filePath);
    };
    const auto clipFiles = [&](const auto &clip) {
        add(clip.filePath);
        audioEdit(clip.audioEdit);
        for (const auto &take : clip.takes)
            add(take.filePath);
    };
    const auto tracks = [&](const auto &items, bool live) {
        for (const auto &track : items) {
            add(track.freeze.filePath);
            for (const auto &clip : track.clips) {
                clipFiles(clip);
                add(clip.offlineProcess.renderedFilePath);
                for (const auto &version : clip.offlineHistory)
                    clipFiles(version.source);
            }
            daw::visitStoredPlugins(track, [&](const auto &slot) {
                audioEdit(slot.audioEdit);
                if (live && slot.uid == "daw.sampler") {
                    if (auto *instance = c->samplerInstance(track.id, slot.id))
                        add(instance->samplePath());
                } else if (live && slot.uid == "daw.slicer") {
                    if (auto *instance = c->slicerInstance(track.id, slot.id))
                        add(instance->samplePath());
                }
            });
        }
    };
    tracks(c->project().tracks, true);
    for (const auto &entry : c->project().clipLibrary)
        tracks(entry.tracks, false);
    for (const auto &slot : c->project().masterInserts)
        audioEdit(slot.audioEdit);
    for (const auto &slot : c->project().masterMiniModules)
        audioEdit(slot.audioEdit);
    run->projectFilesRevision = c->projectRevision();
    return run->projectFiles;
}
bool ScriptService::permittedPath(ScriptRun *run, const QString &path, bool writing) const {
    const auto resolved = canonical(path, writing);
    if (resolved.isEmpty())
        return false;
    // Scratch outputs are always local to this run; original media is read-only.
    if (within(resolved, QFileInfo(run->temporary.path()).canonicalFilePath(), true))
        return true;
    if (run->preview)
        return false;
    const auto declared = run->package.manifest.value("permissions", Json::array());
    const auto granted = run->grants.value("permissions", Json::array());
    if (!writing && std::find(declared.begin(), declared.end(), "project.read") != declared.end() &&
        std::find(granted.begin(), granted.end(), "project.read") != granted.end()) {
        const auto &files = projectFiles(run);
        if (files.contains(resolved))
            return true;
        for (const auto &source : files)
            if (within(resolved, source, false))
                return true;
    }
    if (!writing && !run->package.directory.isEmpty() &&
        within(resolved, QFileInfo(run->package.directory).canonicalFilePath(), true))
        return true;
    for (const auto &grant : run->grants.value("paths", Json::array())) {
        if (writing && !grant.value("write", false))
            continue;
        const auto root = canonical(QString::fromStdString(grant.value("path", "")), writing);
        if (!root.isEmpty() && within(resolved, root, grant.value("folder", false)))
            return true;
    }
    return false;
}
QString ScriptService::persistImport(ScriptRun *run, const QString &path, QStringList &created) {
    const auto resolved = canonical(path, false);
    if (run->preview ||
        !within(resolved, QFileInfo(run->temporary.path()).canonicalFilePath(), true))
        return path;
    // Imported results outlive the worker and its temporary directory, including
    // Undo/Redo.
    const auto folder = ScriptLibrary::directory() + "/.media";
    if (!QDir().mkpath(folder))
        throw std::runtime_error("Cannot create script media folder");
    const auto destination = QDir(folder).filePath(uniqueId() + "-" + QFileInfo(path).fileName());
    created.append(destination);
    if (!QFile::copy(resolved, destination))
        throw std::runtime_error("Cannot preserve imported script audio");
    return destination;
}
void ScriptService::grantPath(ScriptRun *run, const QString &path, bool writing, bool folder) {
    const auto resolved = canonical(path, writing);
    if (resolved.isEmpty())
        throw std::runtime_error("Invalid resource path");
    if (!run->grants.contains("paths"))
        run->grants["paths"] = Json::array();
    run->grants["paths"].push_back(
        {{"path", resolved.toStdString()}, {"write", writing}, {"folder", folder}});
}
vlt::script::Message ScriptService::ioCall(ScriptRun *run, const std::string &method, const Json &a,
                                           const std::vector<char> &binary) {
    const auto requirePath = [&](const QString &path, bool writing) {
        if (!permittedPath(run, path, writing))
            throw std::runtime_error(
                "File permission denied: choose the resource or grant its folder");
    };
    const auto requireWrite = [&] { authorize(run, *vlt::script::apiMethod("files.write")); };
    if (method == "project.files") {
        auto paths = projectFiles(run).values();
        std::sort(paths.begin(), paths.end());
        const auto offset = a.value("offset", std::int64_t(0));
        const auto limit = a.value("limit", std::int64_t(256));
        if (offset < 0 || offset > 1000000 || limit < 1 || limit > 4096)
            throw std::runtime_error("Invalid project file page");
        Json entries = Json::array();
        size_t bytes = 0;
        for (auto i = offset; i < std::min<std::int64_t>(paths.size(), offset + limit); ++i) {
            const QFileInfo file(paths[i]);
            entries.push_back({{"path", paths[i].toStdString()},
                               {"name", file.fileName().toStdString()},
                               {"bytes", file.size()}});
            bytes += entries.back().dump().size();
            if (bytes > 1536 * 1024)
                throw std::runtime_error("Project file page exceeds 1.5 MiB; use a smaller limit");
        }
        return {{{"revision", controller(run)->projectRevision()},
                 {"total", paths.size()},
                 {"offset", offset},
                 {"files", entries}},
                {}};
    }
    if (method == "files.list") {
        const auto path = text(a, "path");
        // A source folder lists only resources the run can read, never its
        // siblings.
        bool containsProjectFile = false;
        const auto resolved = canonical(path, false);
        for (const auto &source : projectFiles(run))
            if (within(QFileInfo(source).path(), resolved, false) &&
                permittedPath(run, source, false)) {
                containsProjectFile = true;
                break;
            }
        if (!containsProjectFile)
            requirePath(path, false);
        Json entries = Json::array();
        for (const auto &file : QDir(path).entryInfoList(
                 QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (file.isSymLink() || !permittedPath(run, file.filePath(), false))
                continue;
            entries.push_back({{"name", file.fileName().toStdString()},
                               {"path", file.filePath().toStdString()},
                               {"directory", file.isDir()},
                               {"bytes", file.size()}});
            if (entries.size() >= 4096)
                break;
        }
        return {entries, {}};
    }
    if (method == "files.read") {
        const auto path = text(a, "path");
        requirePath(path, false);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 512 * 1024)
            throw std::runtime_error("Cannot read file or file exceeds 512 KiB text limit");
        return {file.readAll().toStdString(), {}};
    }
    if (method == "files.write") {
        const auto path = text(a, "path");
        requirePath(path, true);
        QSaveFile file(path);
        const auto bytes = text(a, "text").toUtf8();
        if (bytes.size() > 512 * 1024 || !file.open(QIODevice::WriteOnly) ||
            file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot write file");
        return {true, {}};
    }
    if (method == "audio.open" || method == "audio.create") {
        if (run->audio.size() >= 32)
            throw std::runtime_error("Too many audio streams");
        const bool writing = method == "audio.create";
        const auto path = text(a, "path");
        requirePath(path, writing);
        auto audio = std::make_unique<ScriptAudio>();
        SF_INFO info{};
        audio->source = path;
        audio->sourceBytes = QFileInfo(path).size();
        audio->sourceModified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        if (writing) {
            if (QFileInfo::exists(path))
                throw std::runtime_error("Choose a new output file; source files are preserved");
            info.channels = a.at("channels");
            info.samplerate = a.at("sampleRate");
            info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
            if (info.channels < 1 || info.channels > 32 || info.samplerate < 8000 ||
                info.samplerate > 768000)
                throw std::runtime_error("Invalid audio format");
            audio->destination = path;
            audio->staged = path + ".vlt-" + uniqueId() + ".tmp";
        }
        audio->file =
            openAudio(writing ? audio->staged : path, writing ? SFM_WRITE : SFM_READ, &info);
        if (!audio->file)
            throw std::runtime_error(sf_strerror(nullptr));
        audio->writing = writing;
        audio->channels = info.channels;
        audio->sampleRate = info.samplerate;
        audio->frames = info.frames;
        const auto id = uniqueId().toStdString();
        run->audio[id] = std::move(audio);
        return {{{"id", id},
                 {"channels", info.channels},
                 {"sampleRate", info.samplerate},
                 {"frames", info.frames}},
                {}};
    }
    if (method == "audio.read" || method == "audio.write" || method == "audio.close") {
        const auto id = a.at("id").get<std::string>();
        const auto found = run->audio.find(id);
        if (found == run->audio.end())
            throw std::runtime_error("Audio stream is closed");
        auto &audio = *found->second;
        if (method == "audio.close") {
            if (audio.writing && a.value("publish", false)) {
                requireWrite();
                requirePath(audio.destination, true);
                sf_write_sync(audio.file);
                const auto code = sf_close(audio.file);
                audio.file = nullptr;
                if (code != 0 || !QFile::rename(audio.staged, audio.destination))
                    throw std::runtime_error("Could not publish completed audio");
                audio.staged.clear();
            }
            run->audio.erase(found);
            return {true, {}};
        }
        if (method == "audio.read") {
            if (audio.writing)
                throw std::runtime_error("Stream is write-only");
            requirePath(audio.source, false);
            if (QFileInfo(audio.source).size() != audio.sourceBytes ||
                QFileInfo(audio.source).lastModified().toMSecsSinceEpoch() != audio.sourceModified)
                throw std::runtime_error("Source file changed; reopen the stream");
            const auto frame = a.at("frame").get<sf_count_t>(),
                       frames = a.at("frames").get<sf_count_t>();
            if (frame < 0 || frame > audio.frames || frames < 1 || frames > 65536 ||
                frames * audio.channels * 4 > vlt::script::maxBinaryBytes)
                throw std::runtime_error("Invalid PCM block range");
            if (sf_seek(audio.file, frame, SEEK_SET) < 0)
                throw std::runtime_error("Cannot seek audio");
            std::vector<float> samples(size_t(frames * audio.channels));
            const auto count = sf_readf_float(audio.file, samples.data(), frames);
            std::vector<char> bytes(size_t(count * audio.channels * sizeof(float)));
            std::memcpy(bytes.data(), samples.data(), bytes.size());
            return {
                {{"frames", count}, {"channels", audio.channels}, {"sampleRate", audio.sampleRate}},
                std::move(bytes)};
        }
        if (!audio.writing || binary.empty() || binary.size() % (sizeof(float) * audio.channels))
            throw std::runtime_error("Invalid PCM output block");
        requirePath(audio.destination, true);
        std::vector<float> samples(binary.size() / sizeof(float));
        std::memcpy(samples.data(), binary.data(), binary.size());
        for (const auto value : samples)
            if (!std::isfinite(value))
                throw std::runtime_error("Non-finite audio sample");
        const auto frames = sf_count_t(samples.size() / audio.channels);
        if (sf_writef_float(audio.file, samples.data(), frames) != frames)
            throw std::runtime_error("Audio write failed");
        audio.frames += frames;
        return {audio.frames, {}};
    }
    if (method == "jobs.get" || method == "jobs.cancel") {
        const auto id = text(a, "id");
        for (const auto &job : run->jobs)
            if (job->id == id) {
                if (method == "jobs.cancel")
                    job->cancel = true;
                return {{{"id", id.toStdString()},
                         {"complete", job->complete},
                         {"progress", job->progress.load()},
                         {"cancelled", job->cancel.load()},
                         {"error", job->error.toStdString()},
                         {"result", job->result}},
                        {}};
            }
        throw std::runtime_error("Unknown job");
    }
    if (method == "project.sliceAudio") {
        // The caller identifies a completed host snapshot, never an arbitrary
        // file or a live controller object. Revalidate at the actual edit.
        authorize(run, *vlt::script::apiMethod("project.sliceAudio"));
        const auto id = text(a, "snapshot");
        const auto found = std::find_if(run->jobs.begin(), run->jobs.end(),
                                        [&](const auto &job) { return job->id == id; });
        if (found == run->jobs.end() || !(*found)->clipSource || !(*found)->complete ||
            (*found)->cancel || !(*found)->error.isEmpty())
            throw std::runtime_error("A completed audio.renderClip snapshot is required");
        const auto &job = **found;
        auto *c = controller(run);
        if (c->projectRevision() != job.revision)
            throw std::runtime_error("Project changed during analysis; create a new clip snapshot");
        auto source = *job.clipSource;
        if (c->isClipContentEditLocked(source.address.trackId, source.address.clipId) ||
            c->isTrackEditLocked(source.address.trackId))
            throw std::runtime_error("Clip or track is locked");
        const auto &values = a.at("cuts");
        if (!values.is_array() || values.size() > 4096)
            throw std::runtime_error("Expected at most 4096 cut positions in timeline beats");
        std::vector<double> cuts;
        const double frame = 1.0 / source.sampleRate;
        for (const auto &value : values) {
            if (!value.is_number())
                throw std::runtime_error("Cut positions must be numbers in timeline beats");
            const double at =
                value.get<double>() * 60.0 / source.tempo - source.original.startSeconds;
            if (!std::isfinite(at) || at < -frame || at > source.durationSeconds + frame)
                throw std::runtime_error("Cut position is outside the captured clip");
            cuts.push_back(at);
        }
        std::sort(cuts.begin(), cuts.end());
        std::vector<daw::SilenceRegion> regions;
        double previous = 0;
        const double minimum = daw::EngineController::minimumClipDurationSeconds + frame;
        for (const auto at : cuts) {
            if (at - previous < minimum || source.durationSeconds - at < minimum)
                continue;
            regions.push_back({previous, at});
            previous = at;
        }
        const auto accepted = regions.size();
        if (accepted == 0)
            return {{{"cuts", 0}, {"skipped", cuts.size()}, {"created", Json::array()}}, {}};
        regions.push_back({previous, source.durationSeconds});
        source.preparedRenderPath = job.result.at("path");
        std::vector<daw::EngineController::ClipAddress> created;
        const auto applied = c->applyScriptTransaction(
            job.revision, a.at("label"),
            [&] {
                daw::StripSilenceSettings settings;
                settings.fadeMs = 0;
                const auto result =
                    c->applyStripSilence({source}, {regions}, settings, created, false);
                if (!result)
                    throw std::runtime_error(result.message());
            },
            !source.original.inserts.empty());
        if (!applied)
            throw std::runtime_error(applied.message());
        if (!run->preview && m_bindings.edited)
            m_bindings.edited(run->projectId);
        Json ids = Json::array();
        for (const auto &address : created)
            ids.push_back(address.clipId);
        return {{{"cuts", accepted}, {"skipped", cuts.size() - accepted}, {"created", ids}}, {}};
    }
    if (method == "audio.process" || method == "audio.render" || method == "audio.renderClip" ||
        method == "process.start") {
        int active = 0;
        for (const auto &job : run->jobs)
            if (!job->complete)
                ++active;
        if (active >= 4)
            throw std::runtime_error("At most four offline jobs per instance");
        while (run->jobs.size() >= 128 && run->jobs.front()->complete)
            run->jobs.removeFirst();
        if (run->jobs.size() >= 128)
            throw std::runtime_error("Job history limit reached; wait for older jobs");
        auto job = std::make_shared<ScriptJob>();
        job->id = uniqueId();
        if (method == "audio.renderClip") {
            authorize(run, *vlt::script::apiMethod("audio.renderClip"));
            auto *c = controller(run);
            job->revision = a.at("revision").get<std::uint64_t>();
            if (c->projectRevision() != job->revision)
                throw std::runtime_error("Project changed; refresh the clip before rendering");
            std::vector<daw::EngineController::StripSilenceSource> sources;
            const auto captured = c->prepareStripSilence(
                {{a.at("track").get<std::string>(), a.at("clip").get<std::string>()}}, sources);
            if (!captured)
                throw std::runtime_error(captured.message());
            job->clipSource = std::move(sources.front());
            job->publishMethod = "audio.renderClip";
            const auto path = QDir(run->temporary.path()).filePath(job->id + ".wav");
            const auto staged = path + ".tmp";
            job->future = std::async(std::launch::async, [job, path, staged] {
                StagedFile temporary{staged};
                const auto &source = *job->clipSource;
                // Reuse the engine's offline playback, including warp, loops,
                // reverse, trims, sample edits and comp timing, before live FX.
                const auto blocks =
                    std::max(1.0, std::ceil(source.durationSeconds * source.sampleRate / 2048));
                double block = 0;
                daw::buildSilenceEnvelope(
                    source.placements, source.durationSeconds, source.sampleRate,
                    [&] {
                        job->progress = std::min(.99, block++ / blocks);
                        return !job->cancel.load();
                    },
                    staged.toStdString());
                if (job->cancel)
                    throw std::runtime_error("Cancelled");
                Json result{
                    {"_staged", staged.toStdString()},
                    {"path", path.toStdString()},
                    {"snapshot", job->id.toStdString()},
                    {"revision", job->revision},
                    {"start", source.original.startSeconds * source.tempo / 60.0},
                    {"length", source.durationSeconds * source.tempo / 60.0},
                    {"secondsPerBeat", 60.0 / source.tempo},
                    {"minimumSliceSeconds", daw::EngineController::minimumClipDurationSeconds}};
                temporary.released = true;
                return result;
            });
        } else if (method == "audio.render") {
            requireWrite();
            const auto path = text(a, "path");
            requirePath(path, true);
            if (QFileInfo::exists(path))
                throw std::runtime_error("Choose a new output file");
            daw::rendering::Spec spec;
            spec.outputDir = run->temporary.path().toStdString();
            spec.baseName = job->id.toStdString();
            if (a.contains("start") != a.contains("end"))
                throw std::runtime_error("Render range requires both start and end");
            if (a.contains("start") && a.contains("end")) {
                const double start = a["start"], end = a["end"];
                if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start ||
                    end > 1e9)
                    throw std::runtime_error("Invalid render range");
                spec.range = daw::rendering::Range::Custom;
                spec.customStartSeconds = a["start"].get<double>() * 60 / controller(run)->tempo();
                spec.customEndSeconds = a["end"].get<double>() * 60 / controller(run)->tempo();
            }
            daw::RenderSessionSpec session;
            const auto captured = controller(run)->captureRenderSession(spec, session);
            if (!captured)
                throw std::runtime_error(captured.message());
            const auto staged = path + ".vlt-" + uniqueId() + ".tmp";
            job->future = std::async(std::launch::async, [job, session, path, staged] {
                StagedFile temporary{staged};
                daw::rendering::Report report;
                const auto result = daw::RenderWorker::render(
                    session,
                    [job](const auto &p) {
                        job->progress = p.fraction;
                        return !job->cancel.load();
                    },
                    report);
                if (!result)
                    throw std::runtime_error(result.message());
                if (job->cancel || report.cancelled)
                    throw std::runtime_error("Cancelled");
                if (report.files.empty() ||
                    !QFile::copy(QString::fromStdString(report.files.front()), staged))
                    throw std::runtime_error("Cannot stage render output");
                Json published{{"_staged", staged.toStdString()}, {"path", path.toStdString()}};
                temporary.released = true;
                return published;
            });
        } else if (method == "process.start") {
            if (run->preview)
                throw std::runtime_error("External processes are unavailable in preview");
            const auto executable = text(a, "executable");
            if (!QFileInfo(executable).isAbsolute())
                throw std::runtime_error("Executable must be an absolute path");
            const auto allowed = run->grants.value("executables", Json::array());
            if (std::find(allowed.begin(), allowed.end(),
                          canonical(executable, false).toStdString()) == allowed.end())
                throw std::runtime_error("Executable is not explicitly granted");
            if (QStringList{"cmd", "powershell", "pwsh", "sh", "bash", "zsh", "fish"}.contains(
                    QFileInfo(executable).baseName().toLower()))
                throw std::runtime_error("Command shells cannot be launched");
            QStringList arguments;
            for (const auto &arg : a.at("arguments"))
                arguments.append(QString::fromStdString(arg.get<std::string>()));
            job->future = std::async(std::launch::async, [job, executable, arguments] {
                QProcess process;
                process.start(executable, arguments);
                if (!process.waitForStarted(3000))
                    throw std::runtime_error("External process did not start");
                QByteArray output;
                QElapsedTimer elapsed;
                elapsed.start();
                while (!process.waitForFinished(50)) {
                    output += process.readAllStandardOutput();
                    output += process.readAllStandardError();
                    if (job->cancel || elapsed.elapsed() > 60000 || output.size() > 512 * 1024) {
                        process.kill();
                        process.waitForFinished(250);
                        throw std::runtime_error(
                            "External process cancelled or exceeded its limit");
                    }
                }
                output += process.readAllStandardOutput();
                output += process.readAllStandardError();
                return Json{{"exitCode", process.exitCode()},
                            {"output", output.left(512 * 1024).toStdString()}};
            });
        } else {
            const auto path = text(a, "path");
            requirePath(path, false);
            const auto sourceBytes = QFileInfo(path).size(),
                       sourceModified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
            const auto operation = a.at("operation").get<std::string>();
            if (operation != "analyze" && operation != "trim" && operation != "copy" &&
                operation != "reverse" && operation != "gain" && operation != "fade" &&
                operation != "split")
                throw std::runtime_error("Unknown offline operation");
            QString destination, staged;
            if (operation != "analyze") {
                requireWrite();
                destination = text(a, "output");
                requirePath(destination, true);
                if (QFileInfo::exists(destination))
                    throw std::runtime_error(
                        "Choose a new output file; source files are preserved");
                staged = destination + ".vlt-" + uniqueId() + ".tmp";
            }
            job->future = std::async(std::launch::async, [job, path, operation, a, destination,
                                                          staged, sourceBytes, sourceModified] {
                StagedFile temporary{staged};
                SF_INFO info{};
                AudioFile input{openAudio(path, SFM_READ, &info)};
                if (!input.file || info.channels < 1 || info.channels > 64)
                    throw std::runtime_error("Cannot open input audio");
                constexpr sf_count_t block = 8192;
                std::vector<float> samples(size_t(block * info.channels));
                double peak = 0, sum = 0;
                sf_count_t position = 0, first = info.frames, last = 0;
                const double threshold = std::pow(10.0, a.value("thresholdDb", -42.0) / 20.0);
                daw::SilenceEnvelope envelope;
                if (operation == "trim") {
                    envelope.durationSeconds = double(info.frames) / info.samplerate;
                    envelope.stepSeconds = std::max(.001, envelope.durationSeconds / 1000000);
                    envelope.peaks.resize(
                        size_t(std::ceil(envelope.durationSeconds / envelope.stepSeconds)) + 1, 0);
                }
                while (position < info.frames) {
                    if (job->cancel)
                        throw std::runtime_error("Cancelled");
                    const auto count = sf_readf_float(input.file, samples.data(), block);
                    if (count <= 0)
                        break;
                    for (sf_count_t f = 0; f < count; ++f)
                        for (int ch = 0; ch < info.channels; ++ch) {
                            const double value = samples[size_t(f * info.channels + ch)];
                            peak = std::max(peak, std::abs(value));
                            sum += value * value;
                            if (!envelope.peaks.empty()) {
                                const auto bin =
                                    std::min(envelope.peaks.size() - 1,
                                             size_t(double(position + f) / info.samplerate /
                                                    envelope.stepSeconds));
                                envelope.peaks[bin] =
                                    std::max(envelope.peaks[bin], float(std::abs(value)));
                            }
                            if (std::abs(value) >= threshold) {
                                first = std::min(first, position + f);
                                last = position + f + 1;
                            }
                        }
                    position += count;
                    job->progress = .45 * double(position) / std::max<sf_count_t>(1, info.frames);
                }
                Json result{
                    {"frames", info.frames},
                    {"sampleRate", info.samplerate},
                    {"channels", info.channels},
                    {"peak", peak},
                    {"rms",
                     std::sqrt(sum / double(std::max<sf_count_t>(1, position * info.channels)))},
                    {"firstSoundFrame", first},
                    {"lastSoundFrame", last}};
                const auto unchanged = [&] {
                    return QFileInfo(path).size() == sourceBytes &&
                           QFileInfo(path).lastModified().toMSecsSinceEpoch() == sourceModified;
                };
                if (!unchanged())
                    throw std::runtime_error("Source file changed during processing");
                if (operation == "analyze")
                    return result;
                sf_count_t begin = operation == "trim" ? std::min(first, last) : 0,
                           end = operation == "trim" ? last : info.frames;
                if (operation == "trim") {
                    daw::StripSilenceSettings settings;
                    settings.thresholdDb = a.value("thresholdDb", -42.0);
                    settings.preRollMs = a.value("preRollMs", 10.0);
                    settings.postRollMs = a.value("postRollMs", 50.0);
                    settings.minimumSoundMs = a.value("minimumSoundMs", 30.0);
                    settings.minimumSilenceMs = a.value("minimumSilenceMs", 150.0);
                    settings.splitInternal = false;
                    settings.gridBeats = 0;
                    const auto regions = daw::detectSilenceRegions(
                        envelope, daw::sanitizedStripSilenceSettings(settings), 0, 120);
                    begin =
                        regions.empty()
                            ? 0
                            : std::clamp<sf_count_t>(
                                  sf_count_t(std::floor(regions.front().begin * info.samplerate)),
                                  0, info.frames);
                    end = regions.empty()
                              ? 0
                              : std::clamp<sf_count_t>(
                                    sf_count_t(std::ceil(regions.back().end * info.samplerate)),
                                    begin, info.frames);
                }
                if (operation == "split") {
                    begin = a.value("startFrame", sf_count_t(0));
                    end = a.value("endFrame", info.frames);
                }
                if (begin < 0 || end < begin || end > info.frames)
                    throw std::runtime_error("Invalid audio range");
                SF_INFO outputInfo = info;
                outputInfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
                AudioFile output{openAudio(staged, SFM_WRITE, &outputInfo)};
                if (!output.file)
                    throw std::runtime_error("Cannot stage output audio");
                const double gain = a.value("gain", 1.0);
                if (!std::isfinite(gain) || gain < 0 || gain > 64)
                    throw std::runtime_error("Invalid gain");
                const auto length = end - begin;
                const auto fadeIn = a.value("inFrames", sf_count_t(0));
                const auto fadeOut = a.value("outFrames", sf_count_t(0));
                if (fadeIn < 0 || fadeOut < 0)
                    throw std::runtime_error("Fade lengths cannot be negative");
                for (sf_count_t done = 0; done < length;) {
                    if (job->cancel) {
                        sf_close(output.file);
                        output.file = nullptr;
                        QFile::remove(staged);
                        throw std::runtime_error("Cancelled");
                    }
                    const auto count = std::min(block, length - done);
                    sf_seek(input.file, operation == "reverse" ? end - done - count : begin + done,
                            SEEK_SET);
                    if (sf_readf_float(input.file, samples.data(), count) != count)
                        throw std::runtime_error("Audio read failed");
                    if (operation == "reverse")
                        for (sf_count_t f = 0; f < count / 2; ++f)
                            for (int ch = 0; ch < info.channels; ++ch)
                                std::swap(samples[size_t(f * info.channels + ch)],
                                          samples[size_t((count - 1 - f) * info.channels + ch)]);
                    for (sf_count_t f = 0; f < count; ++f) {
                        double scale = operation == "gain" ? gain : 1;
                        if (operation == "fade") {
                            if (fadeIn > 0)
                                scale = std::min(scale, double(done + f) / fadeIn);
                            if (fadeOut > 0)
                                scale = std::min(scale, double(length - done - f - 1) / fadeOut);
                        }
                        for (int ch = 0; ch < info.channels; ++ch)
                            samples[size_t(f * info.channels + ch)] *= float(scale);
                    }
                    if (sf_writef_float(output.file, samples.data(), count) != count)
                        throw std::runtime_error("Audio write failed");
                    done += count;
                    job->progress = .45 + .55 * double(done) / std::max<sf_count_t>(1, length);
                }
                sf_write_sync(output.file);
                if (sf_close(output.file) != 0) {
                    output.file = nullptr;
                    throw std::runtime_error("Output flush failed");
                }
                output.file = nullptr;
                result["_staged"] = staged.toStdString();
                result["path"] = destination.toStdString();
                result["frames"] = length;
                if (!unchanged())
                    throw std::runtime_error("Source file changed during processing");
                temporary.released = true;
                return result;
            });
        }
        run->jobs.append(job);
        return {{{"id", job->id.toStdString()}}, {}};
    }
    throw std::runtime_error("Unsupported IO operation");
}
void ScriptService::pollIo(ScriptRun *run) {
    for (const auto &job : run->jobs) {
        if (job->complete || !job->future.valid() ||
            job->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            continue;
        try {
            auto result = job->future.get();
            if (result.contains("_staged")) {
                const auto staged = text(result, "_staged"), destination = text(result, "path");
                StagedFile temporary{staged};
                if (job->cancel || run->complete || !controller(run) ||
                    !permittedPath(run, destination, true)) {
                    QFile::remove(staged);
                    throw std::runtime_error("Cancelled or destination permission revoked");
                }
                authorize(run, *vlt::script::apiMethod(job->publishMethod));
                if (!QFile::rename(staged, destination)) {
                    QFile::remove(staged);
                    throw std::runtime_error("Output destination changed or cannot be published");
                }
                result.erase("_staged");
            }
            if (job->cancel || run->complete)
                throw std::runtime_error("Cancelled");
            job->result = std::move(result);
            job->progress = 1;
        } catch (const std::exception &e) {
            job->error = QString::fromUtf8(e.what());
        }
        job->complete = true;
        for (const auto &s : run->subscriptions)
            if (s.name == "job")
                event(run, s.callback,
                      {{"id", job->id.toStdString()},
                       {"result", job->result},
                       {"error", job->error.toStdString()}},
                      s.id + "/" + job->id);
        emit runChanged(run->id);
    }
}
void ScriptService::cleanupIo(ScriptRun *run) {
    for (const auto &job : run->jobs)
        if (!job->complete)
            job->cancel = true;
    run->audio.clear();
}
} // namespace ui
