#include "EngineController.hpp"
#include "WorkspaceAudioHost.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "recovery/RecoveryJournal.hpp"
#if DAW_ENABLE_VST3
#include "Vst3/Vst3Module.hpp"
#endif
#include <nlohmann/json.hpp>
#include <fstream>
#include "fixtures/test_clap/RecoveryTestControl.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#endif

namespace {
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value; return value;
}
void process(daw::EngineController& controller) {
    audio::AudioBuffer input(2, 64), output(2, 64);
    controller.processDeviceBlockForTest(input, output, 64);
}

void midiEngineLifecycle(const std::filesystem::path& root) {
    using namespace daw;
    const auto wave = (root / "midi-engine-tone.wav").string();
    audio::AudioBuffer tone(2, 48000);
    for (unsigned f = 0; f < 48000; ++f)
        for (unsigned c = 0; c < 2; ++c) tone.getChannel(c)[f] = .2f * std::sin(float(f) * .07f);
    audio::AudioRecorder recorder; recorder.initialize(48000, 2);
    recorder.writeWAVFile(wave, tone, 48000);
    EngineController controller;
    controller.initialize(48000, 64, false);
    controller.setTempo(158.);
    const auto track = controller.addTrack(TrackKind::Midi, "MIDI lifecycle");
    check(controller.loadInstrumentSampler(track, wave), "lifecycle sampler loads");
    const auto clip = controller.addMidiClip(track, 0., 2.);
    controller.addNote(track, clip, 60, 0., 1.);
    const auto peak = [&] {
        controller.seekSeconds(0.); controller.play();
        audio::AudioBuffer input(2, 64), output(2, 64);
        float maximum = 0.f;
        for (int block = 0; block < 64; ++block) {
            controller.processDeviceBlockForTest(input, output, 64);
            for (unsigned c = 0; c < 2; ++c) for (unsigned f = 0; f < 64; ++f)
                maximum = std::max(maximum, std::abs(output.getChannel(c)[f]));
        }
        controller.pause();
        return maximum;
    };
    check(peak() > .01f, "MIDI produces audio before power cycle");
    audio::AudioDeviceConfig config; config.sampleRate = 48000; config.bufferSize = 64;
    const auto project = (root / "MidiLifecycle.vlt").string();
    check(bool(controller.saveProject(project)), "save MIDI lifecycle project");
    for (int cycle = 0; cycle < 3; ++cycle) {
        check(bool(controller.deactivateAudio()), "deactivate MIDI engine");
        if (cycle == 1) check(bool(controller.openProject(project)), "reopen MIDI document while inactive");
        check(bool(controller.activateAudio(config, false)), "reactivate MIDI engine");
        check(peak() > .01f, "MIDI produces audio after power cycle and reopen");
    }
}

void transferResourceLifetime(const std::filesystem::path& root) {
    using namespace daw;
    EngineController document;
    document.initializeDocumentOnly();
    const auto saved = (root / "ResourceLifetime.vlt").string();
    check(bool(document.saveProject(saved)), "save empty document for resource lifetime check");
    for (int mode = 0; mode < 4; ++mode) {
        const auto channel = document.addTrack(TrackKind::Audio, "Transfer lifetime");
        auto snapshot = document.copyChannelStrip(channel, true);
        snapshot.volume = .7f;
        const auto directory = root / ("OwnedTransfer" + std::to_string(mode));
        std::filesystem::create_directories(directory);
        snapshot.resources = std::make_shared<ProjectTransferResources>(directory);
        snapshot.sourceDocument = document.documentIdentity();
        std::weak_ptr<ProjectTransferResources> owned = snapshot.resources;
        if (mode < 2) {
            check(document.pasteChannelStrip(channel, snapshot), "channel paste retains owned resources");
            snapshot = {};
            check(!owned.expired(), "pasted resources stay alive while document and undo use them");
            check(bool(mode == 0 ? document.newProject() : document.openProject(saved)), "replace document with pasted resources");
            check(owned.expired() && !std::filesystem::exists(directory), "replacing document releases old transfer resources and temporary files");
        } else {
            const bool applied = mode == 2 ? document.pasteChannelStrip("missing-channel", snapshot)
                                          : document.pasteChannelInserts("missing-channel", snapshot);
            check(!applied, "invalid channel refuses transfer");
            snapshot = {};
            check(owned.expired() && !std::filesystem::exists(directory), "failed channel paste does not retain resources");
        }
    }
}

void transfers(const std::filesystem::path& root) {
    using namespace daw;
    namespace fs = std::filesystem;
    const auto wave = (root / "source.wav").string();
    audio::AudioBuffer tone(2, 48000);
    for (unsigned f=0; f<48000; ++f) for (unsigned c=0; c<2; ++c)
        tone.getChannel(c)[f] = .1f * std::sin(float(f)*.07f);
    audio::AudioRecorder recorder; recorder.initialize(48000, 2);
    recorder.writeWAVFile(wave, tone, 48000);
    ProjectTransferBundle clips, group, pattern, warped, automation, region;
    std::string originalMidi, originalNote;
    {
        EngineController source; source.initializeDocumentOnly(); source.setTempo(120);
        const auto folder = source.addFolder(true, "Band");
        const auto audio = source.addTrack(TrackKind::Audio, "Audio");
        const auto audioClip = source.importAudio(wave, audio, 2.);
        const auto midi = source.addTrack(TrackKind::Instrument, "Sampler");
        check(source.loadInstrumentSampler(midi, wave), "load sampler media while engine off");
        const auto slot = source.project().findTrack(midi)->instrument.id;
        check(source.samplerSnapshot(midi, slot).path == wave && source.loadedPluginCount()==0,
              "sampler model retains file without creating an instance");
        source.undo(); source.redo();
        check(source.samplerSnapshot(midi, slot).path == wave, "offline sampler sample undo and redo");
        originalMidi = source.addMidiClip(midi, 3., 4.);
        originalNote = source.addNote(midi, originalMidi, 64, 1., 2.);
        source.moveTrackToFolder(audio, folder); source.moveTrackToFolder(midi, folder);
        const auto aux = source.addTrack(TrackKind::Audio, "Internal send"); source.moveTrackToFolder(aux, folder);
        source.addSend(audio, aux);
        const auto external = source.addTrack(TrackKind::Audio, "External send"); source.addSend(audio, external);
        check(bool(source.copyProjectObjects({}, {{audio, audioClip}, {midi, originalMidi}}, clips)), "copy mixed audio and MIDI clips");
        check(bool(source.copyProjectObjects({folder}, {}, group)), "copy group, routing and sampler state");
        const auto patternId = source.addPattern("Pattern");
        const auto patternClip = source.addPatternClip(patternId, 0., 2.);
        check(!source.addPatternSample(patternId, wave, 0., patternClip).empty(), "pattern sample without DSP");
        check(bool(source.copyProjectObjects({}, {{patternId, patternClip}}, pattern)), "copy pattern with owned instruments");
        check(source.setChannelClipboard(source.copyChannelStrip(midi, true)), "channel clipboard owns sampler media");
        check(bool(source.copyProjectRegion({{audio,audioClip}}, 2.25, 2.75, region)), "copy time range without mutating source");
        check(source.audioClip(audio, audioClip)->durationSeconds==1., "range capture leaves original duration unchanged");
        ClipWarpModel warp; warp.enabled=true; warp.baselineDurationSeconds=1.;
        warp.markers={{newUuid(),0.,0.,true},{newUuid(),1.,2.,true}};
        check(source.setClipWarp(audio, audioClip, warp), "edit warp while engine off");
        check(bool(source.copyProjectObjects({}, {{audio,audioClip}}, warped)), "copy warped audio");
        AutomationTarget parameter; parameter.kind=AutomationTargetKind::TrackVolume; parameter.channelId=audio;
        const auto [lane,curve] = source.ensureAutomation(parameter);
        check(bool(source.copyProjectObjects({}, {{lane,curve}}, automation)), "capture standalone automation");
    }
    fs::remove(wave);
    EngineController dest; dest.initializeDocumentOnly(); dest.setTempo(60);
    const auto target = dest.addTrack(TrackKind::Audio, "Destination");
    std::vector<std::pair<std::string,std::string>> inserted;
    std::string notice;
    const auto depth = dest.undoDepth(), count = dest.project().tracks.size();
    auto pasted = dest.pasteProjectObjects(clips, target, 10., inserted, notice);
    if (!check(bool(pasted), "paste after closing source and removing its original media")) {
        std::printf("  %s\n", pasted.message().c_str()); return;
    }
    check(inserted.size()==2 && inserted.front().first==target, "reuse first compatible lane and create remaining lanes");
    const auto* audio = dest.audioClip(inserted[0].first, inserted[0].second);
    const auto& midiClips = dest.project().findTrack(inserted[1].first)->clips;
    const auto* midi = &*std::find_if(midiClips.begin(), midiClips.end(), [&](const auto& c) { return c.id==inserted[1].second; });
    check(audio && std::abs(audio->startSeconds-10.)<1e-6 && std::abs(audio->durationSeconds-1.)<1e-6,
          "plain audio preserves seconds at a different tempo");
    check(midi && std::abs(midi->startSeconds-12.)<1e-6 && std::abs(midi->durationSeconds-8.)<1e-6 &&
          midi->notes.size()==1 && midi->notes[0].id!=originalNote && midi->notes[0].startBeats==1. && midi->notes[0].lengthBeats==2.,
          "MIDI preserves beats and receives independent identities");
    check(dest.tempo()==60 && dest.loadedPluginCount()==0 && dest.undoDepth()==depth+1, "one atomic paste undo without tempo or DSP changes");
    dest.undo(); check(dest.project().tracks.size()==count && dest.project().findTrack(target)->clips.empty(), "undo removes every pasted object");
    dest.redo(); check(dest.project().tracks.size()==count+1, "redo restores the entire transfer");
    const auto groupBegin = dest.project().tracks.size();
    pasted = dest.pasteProjectObjects(group, {}, 20., inserted, notice);
    if (check(bool(pasted), "paste full group after source closes")) {
        const auto& tracks = dest.project().tracks;
        const auto& folder = tracks[groupBegin];
        const auto& busSource = tracks[groupBegin+1];
        const auto& sampler = tracks[groupBegin+2];
        const auto& aux = tracks[groupBegin+3];
        check(busSource.parentId==folder.id && sampler.parentId==folder.id && busSource.sends.size()==1 &&
              busSource.sends[0].destinationTrackId==aux.id && !notice.empty(), "internal group and send links survive; external send removed with notice");
        const auto sample = dest.samplerSnapshot(sampler.id, sampler.instrument.id).path;
        check(!sample.empty() && fs::is_regular_file(sample), "transferred sampler owns surviving media");
    } else std::printf("  %s\n", pasted.message().c_str());
    pasted = dest.pasteProjectObjects(pattern, {}, 40., inserted, notice);
    check(bool(pasted) && inserted.size()>=2, "pattern brings child clips and instruments");
    if (!pasted) std::printf("  %s\n", pasted.message().c_str());
    const auto channel = dest.addTrack(TrackKind::Instrument, "Pasted channel");
    check(dest.pasteChannelStrip(channel, dest.channelClipboard()), "paste global channel settings into inactive project");
    check(dest.loadedPluginCount()==0, "all transfers keep inactive DSP empty");
    check(bool(dest.pasteProjectObjects(warped, target, 50., inserted, notice)), "paste warped audio at destination tempo");
    if (!inserted.empty()) check(std::abs(dest.audioClip(inserted[0].first,inserted[0].second)->durationSeconds-2.)<1e-6, "warp preserves beat duration at half tempo");
    const auto automationUndo=dest.undoDepth();
    check(!dest.pasteProjectObjects(automation, {}, 0., inserted, notice) && dest.undoDepth()==automationUndo,
          "standalone automation requires an explicit compatible target");
    AutomationTarget newTarget; newTarget.kind=AutomationTargetKind::TrackVolume; newTarget.channelId=target;
    auto missingTarget = newTarget; missingTarget.channelId = "closed-source-track";
    check(!dest.pasteProjectObjects(automation, {}, 0., inserted, notice, &missingTarget) && inserted.empty() && dest.undoDepth()==automationUndo,
          "nonexistent automation target refuses the complete paste without output or undo changes");
    check(bool(dest.pasteProjectObjects(automation, {}, 0., inserted, notice, &newTarget)), "paste automation to explicitly chosen target");
    check(bool(dest.pasteProjectObjects(region, target, 60., inserted, notice)), "paste independent copied region");
    if (!inserted.empty()) check(std::abs(dest.audioClip(inserted[0].first,inserted[0].second)->durationSeconds-.5)<1e-6, "audio region preserves its length in seconds");
    auto complex = warped;
    auto& ownedClip = complex.snapshot.project.tracks.front().clips.front();
    ownedClip.audioEdit.initialized = true; ownedClip.audioEdit.frames = 48000; ownedClip.audioEdit.channels = 2;
    ownedClip.audioEdit.revision = "source-edit";
    ownedClip.audioEdit.sources.push_back({"source-file", ownedClip.filePath, 48000, 48000, 2});
    ownedClip.audioEdit.regions.push_back({"source-region", "source-file", 0, 48000});
    TakeModel take; take.id="source-take"; take.filePath=ownedClip.filePath; take.lengthSeconds=1.;
    ownedClip.takes.push_back(take); ownedClip.comp.push_back({take.id,0.,1.,"source-comp"});
    ownedClip.offlineHistory.push_back({"source-version",{},"Source",captureClipAudioVersion(ownedClip)});
    ownedClip.offlineHistory.push_back({"render-version","source-version","Rendered",captureClipAudioVersion(ownedClip)});
    ownedClip.offlineVersionId="render-version";
    auto linked = ownedClip; linked.id=newUuid(); linked.startSeconds+=2.;
    complex.snapshot.project.tracks.front().clips.push_back(std::move(linked));
    check(bool(dest.pasteProjectObjects(complex, target, 70., inserted, notice)), "paste layered, edited audio with version history and linked content");
    if (inserted.size()==2) {
        const auto* first=dest.audioClip(inserted[0].first,inserted[0].second);
        const auto* second=dest.audioClip(inserted[1].first,inserted[1].second);
        check(first && second && first->contentId==second->contentId && first->contentId!=complex.snapshot.project.tracks.front().clips.front().contentId,
              "internal content links survive with a new independent content identity");
        if (first) {
            check(first->audioEdit.sources.front().id!="source-file" && first->audioEdit.regions.front().sourceId==first->audioEdit.sources.front().id &&
                  first->takes.front().id!="source-take" && first->comp.front().takeId==first->takes.front().id &&
                  first->warp.markers.front().id!=warped.snapshot.project.tracks.front().clips.front().warp.markers.front().id,
                  "nested audio, warp and take identities are independent with their references intact");
            check(first->offlineHistory.size()==2 && first->offlineHistory[1].parentId==first->offlineHistory[0].id &&
                  first->offlineVersionId==first->offlineHistory[1].id && first->offlineVersionId!="render-version" &&
                  first->offlineHistory[0].source.comp.front().takeId==first->offlineHistory[0].source.takes.front().id,
                  "offline history receives new version identities and retains its ancestry and comp links");
        }
    }
    check(bool(dest.saveProject((root/"Transfer.vlt").string())), "save independently owned transfer media and state");
    clips = {}; group = {}; pattern = {};
    EngineController reopen; reopen.initializeDocumentOnly();
    check(bool(reopen.openProject((root/"Transfer.vlt").string())), "reopen transferred objects from vlt");
    const auto* reopenedChannel = reopen.project().findTrack(channel);
    check(reopenedChannel && fs::is_regular_file(reopen.samplerSnapshot(channel, reopenedChannel->instrument.id).path), "saved channel sample resolves inside receiver package");

    // Failure must leave both the document and its undo history unchanged.
    ProjectTransferBundle missing;
    check(bool(dest.copyProjectObjects({target}, {}, missing)), "capture failure-test transfer");
    for (const auto& t : missing.snapshot.project.tracks) for (const auto& clip : t.clips)
        if (!clip.filePath.empty()) fs::remove(clip.filePath);
    const auto beforeFailure = dest.project().tracks.size(), beforeUndo = dest.undoDepth();
    check(!dest.pasteProjectObjects(missing, {}, 0., inserted, notice) && dest.project().tracks.size()==beforeFailure && dest.undoDepth()==beforeUndo,
          "missing media refuses paste without partial changes");

    recovery::RecoveryJournal journal;
    check(journal.start((root/"Recovery").string(), "test", std::chrono::milliseconds(0)), "one workspace journal starts");
    auto snapshot = dest.captureRecoverySnapshot(0);
    for (const auto* controller : {&dest, &reopen}) {
        auto doc = std::make_shared<recovery::WorkspaceRecoveryDocument>();
        doc->id = controller->documentIdentity(); doc->name = controller->projectName(); doc->dirty = true;
        doc->snapshot = const_cast<EngineController*>(controller)->captureRecoverySnapshot(0);
        snapshot.documents.push_back(std::move(doc));
    }
    snapshot.selectedDocument = reopen.documentIdentity();
    auto nextSnapshot = snapshot;
    journal.requestWrite(std::move(snapshot)); journal.flush();
    const auto session = fs::path(journal.sessionDir());
    std::ifstream manifestFile(session/"workspace.json");
    const auto manifest = nlohmann::json::parse(manifestFile, nullptr, false);
    manifestFile.close();
    if (!check(manifest.is_object() && manifest["documents"].size()==2 && manifest["selected"]==reopen.documentIdentity(), "atomic recovery manifest preserves all documents and selection")) {
        std::printf("Recovery writes: %llu; manifest: %s\n", (unsigned long long)journal.writeCount(), manifest.dump().c_str());
        fs::copy(session, root/"FailedRecovery", fs::copy_options::recursive);
    }
    nextSnapshot.selectedDocument = dest.documentIdentity();
    journal.requestWrite(std::move(nextSnapshot)); journal.flush();
    std::ifstream updatedFile(session/"workspace.json");
    const auto updated = nlohmann::json::parse(updatedFile, nullptr, false); updatedFile.close();
    check(updated.is_object() && updated["selected"]==dest.documentIdentity() && updated["generation"]!=manifest["generation"],
          "subsequent atomic recovery generation replaces the complete workspace manifest");
    journal.stop(); check(!fs::exists(session), "normal exit removes recovery workspace");
}
}

int main(int argc, char** argv) {
    using namespace daw;
    namespace fs = std::filesystem;
#ifdef _WIN32
    if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--memory") {
        const auto privateMiB = [] {
            PROCESS_MEMORY_COUNTERS_EX memory{};
            if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
                return -1.;
            return double(memory.PrivateUsage) / (1024. * 1024.);
        };
        PluginManager catalog;
        { EngineController warmup; warmup.useWorkspacePluginManager(catalog); warmup.initializeDocumentOnly(); warmup.newProject(true); }
        const auto baseline = privateMiB();
        std::printf("Private memory baseline: %.3f MiB\n", baseline);
        const int count = argc == 3 ? 4 : 16;
        fs::path package = argc == 3 ? argv[2] : "";
        if (!package.empty() && fs::is_regular_file(package)) package = package.parent_path();
        for (int cycle = 0; cycle < (argc == 3 ? 12 : 6); ++cycle) {
            std::vector<std::unique_ptr<EngineController>> documents;
            bool inactive = true;
            for (int i = 0; i < count; ++i) {
                auto document = std::make_unique<EngineController>();
                document->useWorkspacePluginManager(catalog);
                document->initializeDocumentOnly();
                const auto opened = argc == 3 ? document->openProject(package.string()) : document->newProject(true);
                if (!opened) { std::fprintf(stderr, "Memory probe open failed: %s\n", opened.message().c_str()); return 1; }
                inactive &= document->documentOnly() && document->loadedPluginCount() == 0 && !document->routingGraph();
                documents.push_back(std::move(document));
            }
            const auto opened = privateMiB();
            documents.clear();
            std::printf("Cycle %d: %d inactive documents %.3f MiB; closed %.3f MiB; %.3f MiB/document above baseline\n",
                        cycle + 1, count, opened, privateMiB(), (opened - baseline) / count);
            check(inactive, "all measured inactive documents have no plugins or graph");
        }
        return failures ? 1 : 0;
    }
#endif
    if (argc == 3 && std::string_view(argv[1]) == "--benchmark-copy") {
        EngineController controller; controller.initializeDocumentOnly();
        if (const auto opened = controller.openProject(argv[2]); !opened) return 1;
        std::vector<std::pair<std::string, std::string>> clips;
        for (const auto& track : controller.project().tracks) for (const auto& clip : track.clips)
            if (clip.kind == ClipKind::Midi) clips.push_back({track.id, clip.id});
        double slowest = 0., elapsed = 0.;
        for (int i = 0; i < 20; ++i) {
            ProjectTransferBundle bundle;
            const auto start = std::chrono::steady_clock::now();
            const auto result = controller.copyProjectObjects({}, clips, bundle);
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
            if (!result) { std::printf("Copy failed: %s\n", result.message().c_str()); return 1; }
            elapsed += ms; slowest = std::max(slowest, ms);
        }
        std::printf("Copy %zu MIDI clips: mean %.3f ms, max %.3f ms (20 runs)\n", clips.size(), elapsed/20., slowest);
        return 0;
    }
    // Optional local regression probe. Never writes the supplied project.
    if (argc == 3 && std::string_view(argv[1]) == "--project") {
#ifdef _WIN32
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* error) -> LONG {
            HMODULE module = nullptr;
            const auto address = error->ExceptionRecord->ExceptionAddress;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<const char*>(address), &module);
            char path[MAX_PATH]{};
            if (module) GetModuleFileNameA(module, path, MAX_PATH);
            std::fprintf(stderr, "Native probe exception %08lx in %s + 0x%llx\n",
                error->ExceptionRecord->ExceptionCode, path,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)-reinterpret_cast<std::uintptr_t>(module)));
            std::fflush(stderr);
            return EXCEPTION_EXECUTE_HANDLER;
        });
#endif
        EngineController controller; controller.initializeDocumentOnly(44100, 1024);
        const auto opened = controller.openProject(argv[2]);
        if (!opened) { std::printf("Open failed: %s\n", opened.message().c_str()); return 1; }
        std::vector<std::pair<std::string, bool>> nonMidi;
        for (const auto& track : controller.project().tracks)
            if (!trackAccepts(track.kind, ClipKind::Midi)) nonMidi.emplace_back(track.id, track.muted);
        for (const auto& [id, muted] : nonMidi) controller.setTrackMuted(id, true);
        for (const auto& track : controller.project().tracks)
            std::printf("Track %s: %zu clips, first notes %zu, muted %d\n", track.name.c_str(), track.clips.size(),
                track.clips.empty() ? 0 : track.clips.front().notes.size(), int(track.muted));
        audio::AudioDeviceConfig config; config.sampleRate = 44100; config.bufferSize = 1024;
        for (int cycle = 0; cycle < 3; ++cycle) {
            const auto enabled = controller.activateAudio(config, false);
            if (!enabled) { std::printf("Activation failed: %s\n", enabled.message().c_str()); return 1; }
            if (cycle == 0) for (const auto& track : controller.project().tracks) {
                if (std::getenv("DAW_PROBE_PARAMETERS")) for (const auto& info : controller.insertParameters(track.id, track.instrument.id))
                    std::printf("Parameter %s %s :: %s = %.6f (default %.6f)\n", track.name.c_str(), info.id.c_str(),
                        info.name.c_str(), controller.insertParameter(track.id, track.instrument.id, info.id), info.defaultValue);
                if (const auto* id = std::getenv("DAW_PROBE_PARAMETER_ID"))
                    controller.setInsertParameter(track.id, track.instrument.id, id, 1.);
            }
            if (const auto* cc = std::getenv("DAW_PROBE_RESET_MIDI_CC"); cc && cycle == 0) for (const auto& track : controller.project().tracks)
                if (trackAccepts(track.kind, ClipKind::Midi)) {
                    controller.liveMidiEvent(track.id, 0xb0, std::uint8_t(std::atoi(cc)), 127);
                }
            controller.seekSeconds(0.); controller.play();
            audio::AudioBuffer input(2, 1024), output(2, 1024);
            float peak = 0.f;
            for (int block = 0; block < 4700; ++block) {
                controller.pumpPluginEvents();
                controller.processDeviceBlockForTest(input, output, 1024);
                for (unsigned c = 0; c < 2; ++c) for (unsigned f = 0; f < 1024; ++f)
                    peak = std::max(peak, std::abs(output.getChannel(c)[f]));
            }
            std::printf("Cycle %d MIDI peak %.6f, plugins %zu, position %.3f playing %d\n", cycle, peak, controller.loadedPluginCount(), controller.positionSeconds(), int(controller.isPlaying()));
            for (const auto& track : controller.project().tracks) {
                const auto status = controller.insertRuntimeStatus(track.id, track.instrument.id);
                std::printf("  %s peak %.6f status %d %s\n", track.name.c_str(), controller.trackPeak(track.id), int(status.state), status.detail.c_str());
            }
            std::fflush(stdout);
            check(peak > .001f, "real project MIDI survives activation");
            if (const auto* output = std::getenv("DAW_PROBE_SAVE_COPY"); output && cycle == 0) {
                for (const auto& [id, muted] : nonMidi) controller.setTrackMuted(id, muted);
                check(bool(controller.saveProject(output)), "save diagnostic copy with restored MIDI volume");
                for (const auto& [id, muted] : nonMidi) controller.setTrackMuted(id, true);
            }
            const auto disabled = controller.deactivateAudio();
            if (!disabled) { std::printf("Deactivation failed: %s\n", disabled.message().c_str()); return 1; }
        }
#if DAW_ENABLE_VST3
        daw::plugins::Vst3Module::releaseUnusedModules();
#endif
        return failures ? 1 : 0;
    }
    const auto root = fs::temp_directory_path() / ("vlt-workspace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
#ifdef _WIN32
    const auto library = LoadLibraryA(DAW_TEST_RECOVERY_CLAP_PATH);
    const auto getControl = library ? reinterpret_cast<RecoveryTestControl*(*)()>(GetProcAddress(library, "recoveryTestControl")) : nullptr;
#else
    const auto library = dlopen(DAW_TEST_RECOVERY_CLAP_PATH, RTLD_NOW | RTLD_LOCAL);
    const auto getControl = library ? reinterpret_cast<RecoveryTestControl*(*)()>(dlsym(library, "recoveryTestControl")) : nullptr;
#endif
    if (!check(getControl != nullptr, "load fixture")) return 1;
    auto& fixture = *getControl();
    const auto descriptors = plugins::factoryFor(plugins::Format::Clap)->inspect(DAW_TEST_RECOVERY_CLAP_PATH);
    if (!check(!descriptors.empty(), "inspect fixture")) return 1;
    EngineController a, b;
    check(bool(a.initialize(48000, 64, false)), "initial project has audio");
    check(bool(b.initializeDocumentOnly(48000, 64)), "second project is document only");
    audio::AudioDeviceConfig config; config.sampleRate = 48000; config.bufferSize = 64;
    WorkspaceAudioHost host; host.adoptInitial(a, config, false);
    const auto trackA = a.addTrack(TrackKind::Audio, "A");
    const auto slotA = a.addInsert(trackA, descriptors.front());
    if (!check(!slotA.empty(), "load source processor")) return 1;
    const auto parameter = a.insertParameters(trackA, slotA).front().id;
    a.setInsertParameter(trackA, slotA, parameter, 1.25);
    a.seekSeconds(4.); a.play(); process(a);
    const auto trackB = b.addTrack(TrackKind::Audio, "B");
    const auto slotB = b.addInsert(trackB, descriptors.front());
    check(!slotB.empty() && !b.insertInstance(trackB, slotB), "offline insertion creates a model without an instance");
    b.undo(); b.redo(); b.seekSeconds(7.);
    check(a.isPlaying() && host.active() == &a, "editing another document preserves playback and owner");
    check(b.loadedPluginCount() == 0 && !b.liveNoteOn(trackB, 60), "inactive document has no processors or MIDI input");
    audio::AudioBuffer inactiveInput(2,64), inactiveOutput(2,64);
    check(!b.processDeviceBlockForTest(inactiveInput, inactiveOutput, 64) && !b.routingGraph(),
          "inactive controller rejects audio processing and owns no compiled graph");
    b.play(); check(!b.isPlaying() && a.isPlaying(), "play cannot activate an inactive controller");
    fixture.failSave = true;
    const auto phraseTrack = a.addTrack(TrackKind::Midi, "Copy without unrelated state capture");
    const auto phraseClip = a.addMidiClip(phraseTrack, 0., 2.);
    ProjectTransferBundle phrase;
    check(bool(a.copyProjectObjects({}, {{phraseTrack, phraseClip}}, phrase)),
          "copying a phrase does not serialize unrelated failing plugins");
    check(!fs::exists(phrase.resources->root), "plain MIDI copy performs no temporary project or media writes");
    check(!a.copyProjectObjects({trackA}, {}, phrase) && phrase.snapshot.project.tracks.front().id==phraseTrack,
          "failed device capture leaves the previous complete clipboard intact");
    check(!host.activate(&b) && host.active() == &a && a.loadedPluginCount() == 1,
          "failed source state capture keeps its processor and cancels transfer");
    fixture.failSave = false;
    check(bool(host.activate(&b)), "transfer A to B");
    check(a.documentOnly() && a.loadedPluginCount() == 0 && b.loadedPluginCount() == 1,
          "only the owner holds plugin instances");
    check(!b.isPlaying() && std::abs(b.positionSeconds()-7.) < 1e-6, "activation preserves cursor and does not play");
    a.setInsertParameter(trackA, slotA, parameter, .75);
    a.commitInsertParameterEdit(trackA, slotA, parameter, 1.25, "Offline gain");
    check(bool(a.saveProject((root / "Dormant.vlt").string())), "save dormant document without activating it");
    check(a.loadedPluginCount() == 0, "save does not load plugins");
    check(bool(host.activate(&a)), "reactivate A"); process(a);
    check(std::abs(a.insertInstance(trackA, slotA)->parameterValue(0)-.75) < 1e-6, "offline parameter edit overrides checkpoint");
    a.undo(); process(a);
    check(std::abs(a.insertParameter(trackA, slotA, parameter)-1.25) < 1e-6, "independent undo survives audio transfer");
    const auto failedWave = (root / "failed-activation.wav").string();
    audio::AudioBuffer failedTone(2, 4096);
    audio::AudioRecorder failedRecorder; failedRecorder.initialize(48000, 2);
    failedRecorder.writeWAVFile(failedWave, failedTone, 48000);
    const auto decodedTrack = b.addTrack(TrackKind::Audio, "Activation PCM lifetime");
    check(!b.importAudio(failedWave, decodedTrack, 0.).empty(), "prepare audio for failed activation");
    fixture.failCreate = true;
    check(!host.activate(&b) && !host.active() && a.loadedPluginCount() == 0 && b.loadedPluginCount() == 0,
          "failed destination activation leaves both documents open with engine off");
    check(b.documentOnly() && !b.cachedSourceSamples(failedWave), "failed activation releases decoded source audio");
    fixture.failCreate = false;
    check(bool(host.activate(&b)), "retry destination after activation failure");
    for (int i=0; i<8; ++i) {
        check(bool(host.activate(i%2 ? &b : &a)), "repeated cold switch");
        check(a.loadedPluginCount()+b.loadedPluginCount() == 1, "no accumulated processors");
    }
    host.activate(&a); a.seekSeconds(0.);
    const auto recordingTrack = a.addTrack(TrackKind::Midi, "Background recording");
    check(a.startRecording(recordingTrack), "start recording in audio owner");
    auto stamp=a.midiInputStamp(); a.liveMidiInput(recordingTrack, 0x90, 60, 100, 1, stamp);
    b.setTempo(90); b.addTrack(TrackKind::Audio, "Edited while A records");
    stamp.transportBeats+=2; stamp.timeNs=a.midiInputStamp().timeNs;
    a.liveMidiInput(recordingTrack, 0x80, 60, 50, 1, stamp);
    check(a.isRecording() && bool(host.activate(&b)) && !a.isRecording(), "engine transfer finalizes source recording");
    check(!a.project().findTrack(recordingTrack)->clips.empty() && !b.project().findTrack(recordingTrack), "recording lands only in its original document");
    check(bool(host.activate(nullptr)) && !a.audioEnabled() && !b.audioEnabled(), "power off releases all processors");
    AutomationTarget pluginTarget; pluginTarget.kind=AutomationTargetKind::PluginParameter;
    pluginTarget.channelId=trackA; pluginTarget.slotId=slotA; pluginTarget.parameterId=parameter;
    const auto [automationLane, automationClip] = a.ensureAutomation(pluginTarget);
    ProjectTransferBundle pluginCurve;
    check(bool(a.copyProjectObjects({}, {{automationLane,automationClip}}, pluginCurve)), "copy plugin automation with target identity");
    std::vector<std::pair<std::string,std::string>> curveInserted; std::string curveNotice;
    auto receiverTarget = pluginTarget; receiverTarget.channelId=trackB; receiverTarget.slotId=slotB;
    auto wrongTarget = receiverTarget; wrongTarget.slotId="unrelated-device";
    check(!b.pasteProjectObjects(pluginCurve, {}, 0., curveInserted, curveNotice, &wrongTarget) && curveInserted.empty(),
          "plugin automation cannot bind to an unrelated or missing device");
    check(bool(b.pasteProjectObjects(pluginCurve, {}, 0., curveInserted, curveNotice, &receiverTarget)) && b.loadedPluginCount()==0,
          "plugin automation binds to an explicit compatible dormant device");
    EngineController reopened; reopened.initializeDocumentOnly();
    check(bool(reopened.openProject((root / "Dormant.vlt").string())) && reopened.loadedPluginCount()==0,
          "reopen saved inactive project without DSP");
    check(bool(reopened.activateAudio(config, false)), "activate reopened project"); process(reopened);
    check(std::abs(reopened.insertInstance(trackA, slotA)->parameterValue(0)-.75)<1e-6, "saved offline parameters survive reopen");
    reopened.deactivateAudio();
    transferResourceLifetime(root);
    transfers(root);
    midiEngineLifecycle(root);
    std::error_code error;
    if (failures) std::printf("Workspace test artifacts: %s\n", root.string().c_str());
    else fs::remove_all(root, error);
    return failures ? 1 : 0;
}
