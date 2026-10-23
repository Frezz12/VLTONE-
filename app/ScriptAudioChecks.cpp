#include "ProjectSerializer.hpp"
#include "ScriptRun.hpp"
#include "ScriptService.hpp"
#include "ShortcutManager.hpp"
#include "StripSilence.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {
bool ScriptService::runAudioCheck() {
    QTemporaryDir temp;
    qputenv("VLT_SCRIPT_LIBRARY", temp.filePath("Scripts").toUtf8());
    const auto expect = [](bool ok, const std::string &message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    const auto wait = [&](auto predicate, int timeout = 20000) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeout) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        expect(predicate(), "Timed out waiting for script audio");
    };
    const auto audioFile = [](const QString &path, int mode, SF_INFO &info) {
#ifdef Q_OS_WIN
        return sf_wchar_open(reinterpret_cast<const wchar_t *>(path.utf16()), mode, &info);
#else
        return sf_open(path.toUtf8().constData(), mode, &info);
#endif
    };
    const auto readPcm = [&](const QString &path) {
        SF_INFO info{};
        auto *file = audioFile(path, SFM_READ, info);
        expect(file && info.channels == 2, "Expected stereo clip render");
        std::vector<float> pcm(std::size_t(info.frames * info.channels));
        const auto read = sf_readf_float(file, pcm.data(), info.frames);
        sf_close(file);
        expect(read == info.frames, "Cannot read rendered clip");
        return pcm;
    };
    try {
        expect(temp.isValid(), "Cannot create test directory");
        daw::EngineController controller;
        expect(controller.initialize(48000, 256, false).isOk(), "Engine initialization failed");
        controller.setRecordDirectory(temp.filePath("renders").toStdString());
        controller.newProject();
        const auto wave = temp.filePath("attacks.wav");
        SF_INFO info{};
        info.channels = 1;
        info.samplerate = 48000;
        info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        auto *file = audioFile(wave, SFM_WRITE, info);
        expect(file, "Cannot write source fixture");
        std::vector<float> pcm(96000);
        // Include an onset inside the old script's 15..20 ms edge gap.
        for (const auto onset : {850, 12000, 36000, 60000, 84000})
            for (int i = 0; i < 2200; ++i)
                pcm[onset + i] = .75f * float(std::sin(i * .15)) * (1.f - float(i) / 2200);
        const auto written = sf_writef_float(file, pcm.data(), pcm.size());
        sf_close(file);
        expect(written == 96000, "Fixture write failed");
        const auto trackId = controller.addTrack(daw::TrackKind::Audio, "Audio slice fixture");
        const auto clipId = controller.importAudio(wave.toStdString(), trackId, 25.6);
        expect(!clipId.empty(), "Import failed");
        const auto base = controller.project();
        std::vector<std::pair<std::string, daw::ProjectModel>> cases;
        for (const auto name :
             {"plain-edge", "trimmed", "stretch", "stretch-pitch", "warp", "reverse", "loop",
              "ping-pong", "fades", "tape", "comp"}) {
            auto model = base;
            auto &clip = model.tracks.front().clips.front();
            if (std::string_view(name) == "trimmed") {
                clip.offsetSeconds = .1;
                clip.durationSeconds = 1.7;
            }
            if (std::string_view(name) == "stretch" || std::string_view(name) == "stretch-pitch") {
                clip.sampleEdit.stretchTime = 1.5;
                clip.durationSeconds = 3;
            }
            if (std::string_view(name) == "stretch-pitch") {
                clip.sampleEdit.stretchMode = daw::ClipStretchMode::Vocal;
                clip.sampleEdit.stretchPitch = 3;
            }
            if (std::string_view(name) == "fades" || std::string_view(name) == "tape") {
                clip.fadeInSeconds = .6;
                clip.fadeOutSeconds = .4;
                if (std::string_view(name) == "tape")
                    clip.fadeInMode = clip.fadeOutMode = daw::ClipFadeMode::Tape;
            }
            if (std::string_view(name) == "reverse")
                clip.sampleEdit.reverse = true;
            if (std::string_view(name) == "comp") {
                daw::TakeModel first;
                first.id = "first-take";
                first.filePath = wave.toStdString();
                first.lengthSeconds = 2;
                auto second = first;
                second.id = "second-take";
                second.gain = .6f;
                clip.takes = {first, second};
                clip.comp = {{first.id, 0, 1, "first-part"},
                             {second.id, 1, 2, "second-part"}};
            }
            if (std::string_view(name) == "loop" || std::string_view(name) == "ping-pong") {
                clip.sampleEdit.loopMode = std::string_view(name) == "loop" ? 1 : 2;
                clip.sampleEdit.loopStart = .1;
                clip.sampleEdit.loopEnd = .8;
                clip.durationSeconds = 4;
            }
            if (std::string_view(name) == "warp") {
                clip.warp.enabled = true;
                clip.warp.baselineDurationSeconds = 2;
                clip.warp.markers = {
                    {"begin", 0, 0, true}, {"middle", .5, 1.5, false}, {"end", 2, 4, true}};
                clip.durationSeconds = 4 * 60 / model.tempo;
            }
            cases.emplace_back(name, std::move(model));
        }
        const auto reproPath = qEnvironmentVariable("VLT_SCRIPT_REPRO_PROJECT");
        if (!reproPath.isEmpty()) {
            daw::ProjectModel project;
            const auto loaded = daw::ProjectSerializer::loadDocument(
                project, reproPath.toStdString(),
                QFileInfo(reproPath).dir().filePath("Content").toStdString());
            expect(loaded.isOk(), loaded.message());
            // Exercise real source/clip timing without instantiating the user's
            // unrelated instruments or writing anything into their project.
            for (const auto &track : project.tracks) {
                const auto clip =
                    std::find_if(track.clips.begin(), track.clips.end(),
                                 [](const auto &c) { return c.kind == daw::ClipKind::Audio; });
                if (clip == track.clips.end())
                    continue;
                auto model = base;
                model.tempo = project.tempo;
                model.tracks.front().clips = {*clip};
                model.tracks.front().clips.front().inserts.clear();
                cases.emplace_back("project: " + clip->name, std::move(model));
            }
        }
        QMainWindow window;
        ShortcutManager shortcuts(&window);
        Bindings bindings;
        bindings.current = [&] { return Project{"audio", "Audio fixture", &controller}; };
        bindings.resolve = [&](const QString &id) {
            return id == "audio" ? bindings.current() : Project{};
        };
        bindings.context = [&](const QString &) {
            const auto &track = controller.project().tracks.back();
            return Json{{"selection", {{"clips", Json::array({
                {{"track", track.id}, {"clip", track.clips.front().id}}})}}}};
        };
        bindings.edited = [](const QString &) {};
        ScriptService scripts(&window, &shortcuts, bindings);
        ScriptPackage slicer;
        QString error;
        expect(ScriptLibrary::readDirectory(QCoreApplication::applicationDirPath() +
                                                "/ScriptSDK/examples/transient-slicer",
                                            slicer, error),
               error.toStdString());
        scripts.saveSettings(slicer.id(), {{"permissions", slicer.manifest["permissions"]}});
        for (auto &[name, model] : cases) {
            const auto original = model.tracks.front().clips.front();
            const auto sourceTrack = model.tracks.front().id;
            auto linkedTrack = model.tracks.front();
            linkedTrack.id = daw::newUuid();
            linkedTrack.clips.front().id = daw::newUuid();
            linkedTrack.clips.front().startSeconds += original.durationSeconds + 1;
            const auto linkedTrackId = linkedTrack.id;
            model.tracks.push_back(std::move(linkedTrack));
            daw::EngineController::PreparedProject prepared;
            prepared.document = std::move(model);
            prepared.rate = 48000;
            prepared.path = temp.filePath("fixture.vlt").toStdString();
            const auto opened = controller.openPreparedProject(std::move(prepared));
            expect(opened.isOk(), opened.message());
            const auto linkedDocument = [&] {
                daw::ProjectModel document;
                document.tracks = {*controller.project().findTrack(linkedTrackId)};
                std::string serialized;
                expect(daw::ProjectSerializer::serializeDocument(document, serialized).isOk(),
                       "Cannot serialize linked clip");
                return serialized;
            };
            const auto linkedBefore = linkedDocument();
            const Json context{{"project", "audio"},
                               {"type", "clip"},
                               {"target", {{"track", sourceTrack}, {"clip", original.id}}}};
            const auto runId = scripts.runPackage(slicer, "run", context);
            std::shared_ptr<ScriptRun> run;
            wait([&] {
                for (const auto &candidate : scripts.m_runs)
                    if (candidate->id == runId && (candidate->idle || candidate->complete)) {
                        run = candidate;
                        return true;
                    }
                return false;
            });
            expect(!run->complete && run->error.isEmpty(),
                   "Slicer startup: " + run->error.toStdString());
            auto *button = qobject_cast<QPushButton *>(run->widgets.value("run").data());
            auto *status = qobject_cast<QLabel *>(run->widgets.value("status").data());
            expect(button && status, "Slicer UI missing");
            if (name == "plain-edge") {
                const auto request = Json{{"track", sourceTrack},
                                          {"clip", original.id},
                                          {"revision", controller.projectRevision()}};
                scripts.ioCall(run.get(), "audio.renderClip", request, {});
                const auto probe = run->jobs.back();
                wait([&] { return probe->complete; });
                expect(probe->error.isEmpty(), probe->error.toStdString());
                const auto beat = [&](double seconds) {
                    return (original.startSeconds + seconds) * controller.tempo() / 60;
                };
                Json slice{{"snapshot", probe->id.toStdString()},
                           {"label", "Boundary regression"},
                           {"cuts", Json::array({beat(.5), beat(.015), beat(.02), beat(.025),
                                                 beat(.025), beat(.03), beat(1.985)})}};
                const auto before = controller.undoDepth();
                auto invalid = slice;
                invalid["cuts"].push_back(beat(2.1));
                bool rejected = false;
                try {
                    scripts.ioCall(run.get(), "project.sliceAudio", invalid, {});
                } catch (...) {
                    rejected = true;
                }
                expect(rejected && controller.undoDepth() == before &&
                           controller.project().findTrack(sourceTrack)->clips.size() == 1,
                       "Out-of-range cuts left partial edits");
                run->assistantMode = "ask";
                rejected = false;
                try {
                    scripts.ioCall(run.get(), "project.sliceAudio", slice, {});
                } catch (...) {
                    rejected = true;
                }
                run->assistantMode.clear();
                expect(rejected, "Assistant read-only mode allowed slicing");
                const auto granted = run->grants;
                run->grants["permissions"] = {"project.read", "files.read"};
                rejected = false;
                try {
                    scripts.ioCall(run.get(), "project.sliceAudio", slice, {});
                } catch (...) {
                    rejected = true;
                }
                run->grants = granted;
                expect(rejected, "Revoked project.write allowed slicing");
                const auto sliced = scripts.ioCall(run.get(), "project.sliceAudio", slice, {}).json;
                expect(sliced.at("cuts") == 2 && sliced.at("skipped") == 5,
                       "Edge and duplicate cuts were not filtered before editing");
                controller.undo();
                auto cancelRequest = request;
                cancelRequest["revision"] = controller.projectRevision();
                scripts.ioCall(run.get(), "audio.renderClip", cancelRequest, {});
                const auto cancelled = run->jobs.back();
                scripts.ioCall(run.get(), "jobs.cancel", {{"id", cancelled->id.toStdString()}}, {});
                wait([&] { return cancelled->complete; });
                expect(!cancelled->error.isEmpty() &&
                           !QFile::exists(run->temporary.filePath(cancelled->id + ".wav")),
                       "Cancelled clip render published a result");
            }
            const auto undoDepth = controller.undoDepth();
            std::string beforeDocument;
            expect(daw::ProjectSerializer::serializeDocument(controller.project(), beforeDocument)
                       .isOk(),
                   "Cannot capture document before slicing");
            button->click();
            wait(
                [&] {
                    return run->complete ||
                           status->text().startsWith(QString::fromUtf8("Готово:")) ||
                           status->text().startsWith(QString::fromUtf8("Ошибка:"));
                },
                60000);
            expect(!run->complete && status->text().startsWith(QString::fromUtf8("Готово:")),
                   name + ": " + status->text().toStdString() + " " + run->error.toStdString());
            expect(controller.undoDepth() == undoDepth + 1, "Slicing must add exactly one Undo");
            expect(linkedDocument() == linkedBefore, "Slicing changed a linked clip");
            auto pieces = controller.project().findTrack(sourceTrack)->clips;
            std::sort(pieces.begin(), pieces.end(),
                      [](const auto &a, const auto &b) { return a.startSeconds < b.startSeconds; });
            expect(pieces.size() > 1, "No slices created");
            double end = original.startSeconds;
            std::vector<daw::EngineController::ClipAddress> addresses;
            for (const auto &piece : pieces) {
                expect(piece.contentId != original.contentId,
                       "Slice still shares editable content with its original");
                expect(std::abs(piece.startSeconds - end) < 1e-8 && piece.durationSeconds > .02,
                       "Slicing left gaps, overlaps or a tiny clip");
                end += piece.durationSeconds;
                addresses.push_back({sourceTrack, piece.id});
            }
            expect(std::abs(end - original.startSeconds - original.durationSeconds) < 1e-7,
                   "Slicing changed the clip's total duration");
            auto snapshot = run->jobs.back();
            const auto before = readPcm(QString::fromStdString(snapshot->result.at("path")));
            std::vector<daw::EngineController::StripSilenceSource> sources;
            const auto captured = controller.prepareStripSilence(addresses, sources);
            expect(captured.isOk(), captured.message());
            daw::engine::ClipPlayerNode::ClipList placements;
            for (const auto &source : sources)
                for (auto placement : source.placements) {
                    placement.startSample += std::llround(
                        (source.original.startSeconds - original.startSeconds) * 48000);
                    placements.push_back(std::move(placement));
                }
            const auto afterPath = temp.filePath("after.wav");
            daw::buildSilenceEnvelope(placements, original.durationSeconds, 48000, {},
                                      afterPath.toStdString());
            const auto after = readPcm(afterPath);
            expect(before.size() == after.size(), "Slicing changed the rendered duration");
            double energy = 0, difference = 0;
            for (std::size_t i = 0; i < before.size(); i += 2) {
                const auto time = double(i / 2) / 48000 + original.startSeconds;
                bool seam = false;
                for (const auto &piece : pieces)
                    seam |= std::abs(time - piece.startSeconds) < .025;
                if (seam || time > end - .025)
                    continue;
                for (int ch = 0; ch < 2; ++ch) {
                    energy += before[i + ch] * before[i + ch];
                    const auto delta = before[i + ch] - after[i + ch];
                    difference += delta * delta;
                }
            }
            const auto errorRatio = difference / std::max(1e-12, energy);
            expect(errorRatio < .002, name + ": slicing changed audible timing/audio; error=" +
                                          std::to_string(errorRatio));
            controller.undo();
            const auto *restored = controller.audioClip(sourceTrack, original.id);
            expect(restored && controller.project().findTrack(sourceTrack)->clips.size() == 1 &&
                       restored->warp == original.warp,
                   "Undo did not restore the original clip/settings");
            std::string restoredDocument;
            expect(daw::ProjectSerializer::serializeDocument(controller.project(), restoredDocument)
                           .isOk() &&
                       restoredDocument == beforeDocument,
                   "Undo changed document content or source settings");
            controller.redo();
            expect(controller.project().findTrack(sourceTrack)->clips.size() == pieces.size(),
                   "Redo lost slices");
            bool stale = false;
            try {
                scripts.ioCall(run.get(), "project.sliceAudio",
                               {{"snapshot", snapshot->id.toStdString()},
                                {"cuts", Json::array()},
                                {"label", "stale"}},
                               {});
            } catch (...) {
                stale = true;
            }
            expect(stale, "An obsolete audio snapshot was accepted");
            scripts.stop(runId);
            for (const auto &piece : controller.project().findTrack(sourceTrack)->clips)
                expect(QFile::exists(QString::fromStdString(piece.filePath)),
                       "Stopping the script deleted slice audio");
            std::fprintf(stderr, "[script-audio] PASS %s: %zu slices; PCM error %.8f\n",
                         name.c_str(), pieces.size(), errorRatio);
        }
        std::fprintf(stderr,
                     "[script-audio] PASS all %zu cases, Undo/Redo, stale "
                     "snapshots, persistent media\n",
                     cases.size());
        return true;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[script-audio] FAIL: %s\n", e.what());
        return false;
    }
}
} // namespace ui
