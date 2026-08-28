#include "ai/ContentCatalog.hpp"

#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using daw::ai::ContentCatalog;
using daw::ai::ContentType;

static int failures = 0;
static bool check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
    return condition;
}

namespace {

struct TempTree {
    fs::path path;
    ~TempTree() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

bool writeWave(const fs::path& path) {
    constexpr int frames = 480;
    std::vector<float> samples(frames);
    for (int i = 0; i < frames; ++i)
        samples[std::size_t(i)] =
            0.2f * std::sin(float(i) * 0.1f);
    const float* channels[] = {samples.data()};

    audio::platform::AudioFileWriter writer;
    if (!writer.open(daw::platform::pathToUtf8(path), 48000.0, 1,
                     frames)
             .isOk())
        return false;
    if (!writer.write(channels, frames).isOk()) return false;
    return writer.close().isOk();
}

bool writeMidi(const fs::path& path, std::uint8_t pitch = 60) {
    const std::vector<std::uint8_t> bytes = {
        'M',  'T',  'h',  'd',  0,    0,    0,    6,    0,    0,
        0,    1,    0,    96,   'M',  'T',  'r',  'k',  0,    0,
        0,    12,   0,    0x90, pitch, 100,  0x60, 0x80, pitch, 64,
        0,    0xff, 0x2f, 0};
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 std::streamsize(bytes.size()));
    return bool(stream);
}

bool waitForIndex(ContentCatalog& catalog,
                  std::chrono::milliseconds timeout =
                      std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (catalog.status().running() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return !catalog.status().running();
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TempTree tree{fs::temp_directory_path() / "daw-content-catalog-test"};
    std::error_code ec;
    fs::remove_all(tree.path, ec);
    const fs::path granted = tree.path / "granted";
    const fs::path outside = tree.path / "outside";
    fs::create_directories(granted, ec);
    fs::create_directories(outside, ec);

    const fs::path wave = granted / "Warm Kick.WAV";
    const fs::path midi = granted / "bassline.mid";
    check(writeWave(wave), "creates an audio fixture");
    check(writeMidi(midi), "creates a MIDI fixture");
    {
        std::ofstream ignored(granted / "notes.txt");
        ignored << "not content";
    }

    ContentCatalog catalog;
    const auto grants = catalog.setBrowserRoots(
        {daw::platform::pathToUtf8(granted),
         daw::platform::pathToUtf8(tree.path / "missing")});
    check(grants.accepted == 1 && grants.rejected == 1,
          "only existing browser directories become grants");

    const auto first = catalog.refresh();
    check(first.indexed == 2 && catalog.size() == 2,
          "indexes supported audio and MIDI, but not unrelated files");
    check(first.cacheHits == 0 && first.metadataFailures == 0,
          "first refresh probes both files");

    const auto audio = catalog.search("warm", ContentType::Audio);
    check(audio.size() == 1 && audio.front().name == "Warm Kick.WAV",
          "search is case-insensitive and can filter audio");
    if (!audio.empty()) {
        check(audio.front().audio && audio.front().audio->channels == 1 &&
                  std::llround(audio.front().audio->sampleRate) == 48000 &&
                  std::fabs(audio.front().audio->durationSeconds - 0.01) <
                      1e-6,
              "audio metadata comes from the shared probe");
        check(audio.front().audio && audio.front().audio->timbre &&
                  audio.front().audio->timbre->rmsDbfs < -10.0 &&
                  audio.front().audio->timbre->rmsDbfs > -30.0 &&
                  audio.front().audio->timbre->peakDbfs < -10.0 &&
                  audio.front().audio->timbre->brightness >= 0.0 &&
                  audio.front().audio->timbre->brightness <= 1.0 &&
                  audio.front().audio->timbre->sampledSeconds > 0.0,
              "audio indexing adds bounded lightweight timbre features");
        check(audio.front().contentId.starts_with("content_") &&
                  audio.front().contentId.find("Warm") == std::string::npos &&
                  audio.front().contentId.find("granted") ==
                      std::string::npos,
              "the model-facing id contains no filename or path");
        const auto resolved = catalog.resolvePath(audio.front().contentId);
        check(resolved && fs::equivalent(daw::platform::pathFromUtf8(*resolved),
                                         wave, ec),
              "trusted code can resolve an id inside its grant");
    }

    const auto midiMatches = catalog.search("bass", ContentType::Midi);
    check(midiMatches.size() == 1 && midiMatches.front().midi &&
              midiMatches.front().midi->noteCount == 1 &&
              std::fabs(midiMatches.front().midi->lengthBeats - 1.0) < 1e-9,
          "MIDI metadata comes from the shared MIDI parser");

    const std::string firstMidiId =
        midiMatches.empty() ? std::string() : midiMatches.front().contentId;
    const auto second = catalog.refresh();
    const auto midiAgain = catalog.search("bass", ContentType::Midi);
    check(second.cacheHits == 2 && !midiAgain.empty() &&
              midiAgain.front().contentId == firstMidiId,
          "unchanged path+mtime entries hit cache and keep deterministic ids");

    ContentCatalog sameGrant({daw::platform::pathToUtf8(granted)});
    sameGrant.refresh();
    const auto sameMidi = sameGrant.search("bass", ContentType::Midi);
    check(!sameMidi.empty() && sameMidi.front().contentId == firstMidiId,
          "ids are deterministic across catalog instances");

    ContentCatalog background({daw::platform::pathToUtf8(granted)});
    check(background.startRefresh(), "starts a background catalog refresh");
    const auto started = background.status();
    check(started.state != daw::ai::CatalogIndexState::Idle &&
              started.generation > 0,
          "background refresh exposes state and generation immediately");
    (void)background.search();
    check(waitForIndex(background) &&
              background.status().state == daw::ai::CatalogIndexState::Ready &&
              background.status().progress().value_or(-1.0) == 1.0 &&
              background.size() == 2,
          "background refresh completes while search remains callable");

    const fs::path cancelRoot = tree.path / "cancel";
    fs::create_directories(cancelRoot, ec);
    for (int i = 0; i < 512; ++i)
        writeMidi(cancelRoot / ("item-" + std::to_string(i) + ".mid"),
                  std::uint8_t(36 + i % 48));
    ContentCatalog cancellable({daw::platform::pathToUtf8(cancelRoot)});
    const bool cancellationStarted = cancellable.startRefresh();
    const bool rejectedConcurrent = !cancellable.startRefresh();
    cancellable.cancelRefresh();
    check(cancellationStarted && rejectedConcurrent &&
              waitForIndex(cancellable) &&
              cancellable.status().state ==
                  daw::ai::CatalogIndexState::Cancelled,
          "one background job runs at a time and cancellation is cooperative");

    check(writeMidi(cancelRoot / "zz-target.mid"),
          "creates a stable-id refresh fixture");
    cancellable.refresh();
    const auto targetBeforeRefresh = cancellable.search("zz-target");
    bool idStayedResolvable = !targetBeforeRefresh.empty();
    const std::string targetId = idStayedResolvable
                                     ? targetBeforeRefresh.front().contentId
                                     : std::string();
    cancellable.startRefresh();
    while (cancellable.status().running()) {
        if (!cancellable.resolvePath(targetId)) idStayedResolvable = false;
        std::this_thread::yield();
    }
    check(idStayedResolvable && cancellable.resolvePath(targetId),
          "published ids remain resolvable throughout an incremental refresh");

    const auto oldTime = fs::last_write_time(midi, ec);
    check(writeMidi(midi, 62), "updates the MIDI fixture");
    fs::last_write_time(midi, oldTime + std::chrono::seconds(2), ec);
    const auto changed = catalog.refresh();
    check(changed.cacheHits == 1,
          "a changed mtime invalidates only that file's cached metadata");

    const fs::path outsideWave = outside / "private.wav";
    check(writeWave(outsideWave), "creates an outside fixture");
    ContentCatalog outsideCatalog({daw::platform::pathToUtf8(outside)});
    outsideCatalog.refresh();
    const auto outsideItems = outsideCatalog.search();
    check(!outsideItems.empty() &&
              !catalog.resolvePath(outsideItems.front().contentId),
          "an id issued by another grant cannot resolve here");

    const fs::path escape = granted / "escape.wav";
    ec.clear();
    fs::create_symlink(outsideWave, escape, ec);
    if (!ec) {
        catalog.refresh();
        check(catalog.search("escape").empty(),
              "a file symlink cannot escape a browser grant");
    } else {
        check(true, "file symlink escape check unavailable on this machine");
    }

    const fs::path outsideFolder = outside / "secret-folder";
    fs::create_directories(outsideFolder, ec);
    check(writeMidi(outsideFolder / "secret.mid"),
          "creates an outside directory fixture");
    const fs::path directoryEscape = granted / "linked-folder";
    ec.clear();
    fs::create_directory_symlink(outsideFolder, directoryEscape, ec);
    if (!ec) {
        catalog.refresh();
        check(catalog.search("secret").empty(),
              "a directory symlink cannot escape a browser grant");
    } else {
        check(true,
              "directory symlink escape check unavailable on this machine");
    }

    std::string revokedId;
    const auto beforeRevoke = catalog.search();
    if (!beforeRevoke.empty()) revokedId = beforeRevoke.front().contentId;
    catalog.setBrowserRoots({});
    check(catalog.size() == 0 && catalog.search().empty() &&
              !catalog.resolvePath(revokedId),
          "replacing grants immediately revokes every previous id");

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
