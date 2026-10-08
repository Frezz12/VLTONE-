#include "AudioInputClock.hpp"
#include "AudioRuntimeProcess.hpp"
#include "Transport/Transport.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace daw;
int failures = 0;
bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
    return value;
}
constexpr auto farFuture = std::numeric_limits<std::uint64_t>::max();
bool near(double left, double right) { return std::abs(left - right) < 1e-10; }

void scalarClock() {
    engine::InputClock clock;
    clock.setState(engine::TransportState::Playing);
    clock.publish(1'000'000'000, 10, .02, 120);
    check(near(clock.inputBeatsAt(0), 10) && near(clock.inputBeatsAt(1'010'000'000), 10.02) &&
          near(clock.inputBeatsAt(farFuture), 10.04),
          "input time clamps to the actual audio block instead of extrapolating stale UI state");
    clock.setState(engine::TransportState::Paused);
    check(near(clock.inputBeatsAt(0), 10.04), "paused input clock returns the completed unwrapped beat");

    engine::Transport transport;
    engine::InputClock mirror;
    transport.setSampleRate(48000); transport.setTempo(120);
    transport.setLoopRange(0, 64); transport.setLoopEnabled(true);
    transport.bindInputClock(&mirror); transport.play();
    for (int i = 0; i < 4; ++i) transport.advance(64);
    const auto timestamp = std::uint64_t(engine::presentationNowNs());
    check(transport.position() == 0 && near(mirror.inputBeatsAt(farFuture), 4.0 * 64 / 48000 * 2) &&
          mirror.inputBeatsAt(timestamp) == transport.inputBeatsAt(timestamp),
          "local and shared algorithms agree exactly while the timeline wraps repeatedly");
    transport.pause(); transport.seek(10000); transport.setTempo(90);
    check(mirror.inputBeatsAt(0) == transport.inputBeatsAt(0),
          "pause and seek preserve the existing unwrapped input-time semantics");
    transport.play(); transport.advance(64);
    check(mirror.inputBeatsAt(farFuture) == transport.inputBeatsAt(farFuture),
          "tempo changes use the same new block in the local and shared clocks");
    transport.bindInputClock(nullptr);
    const auto detached = mirror.inputBeatsAt(farFuture);
    transport.advance(64);
    check(mirror.inputBeatsAt(farFuture) == detached, "detached clock has no remaining audio writer");
}

void identities() {
    auto slot = std::make_shared<audioipc::InputClockSlot>();
    slot->sessionId.store(7); slot->clock.publish(0, 1, .5, 120); slot->generation.store(1);
    AudioInputClockReader first(slot, 7, 1);
    check(first.valid() && near(first.inputBeatsAt(0), 2), "reader binds a precise session generation");
    slot->generation.store(0, std::memory_order_release);
    check(!first.valid() && first.inputBeatsAt(0) == 0, "retired generation never returns a stale timestamp");
    slot->sessionId.store(8); slot->clock.publish(0, 40, 1, 60); slot->generation.store(1, std::memory_order_release);
    AudioInputClockReader next(slot, 8, 1);
    check(!first.valid() && next.valid() && near(next.inputBeatsAt(0), 41),
          "slot reuse cannot expose another audition session to an old reader");
    const std::weak_ptr<const audioipc::InputClockSlot> lifetime = slot;
    slot.reset();
    check(!lifetime.expired() && near(next.inputBeatsAt(0), 41), "reader retains its scalar mapping owner");
    first = {}; next = {};
    check(lifetime.expired(), "retired scalar owner releases after its final reader");
}

void presentationClock() {
    engine::Transport transport;
    transport.setSampleRate(48000); transport.play();
    // Attach after rendering: a replacement device may already have queued
    // audio when the process publishes its session identity to the parent.
    for (int block = 0; block < 2; ++block) {
        transport.setPresentationTiming(1'000'000'000 + block * 20'000'000,
            engine::PresentationClockSource::DeviceTimestamp);
        transport.advance(960, true);
        transport.finishPresentationBlock(480);
    }
    auto slot = std::make_shared<audioipc::InputClockSlot>();
    slot->sessionId.store(9);
    transport.bindInputClock(&slot->clock, &slot->presentation);
    slot->generation.store(1, std::memory_order_release);
    AudioInputClockReader reader(slot, 9, 1);
    double seconds = -1;
    bool smooth = true;
    for (int frame = 0; frame < 8; ++frame) {
        const auto at = 1'012'000'000LL + frame * 4'000'000LL;
        engine::AudioPresentationSnapshot native;
        smooth &= reader.readPresentation(at, seconds) &&
            transport.presentationSnapshot(at, native) && near(seconds, native.secondsAt(at)) &&
            near(seconds, .002 + frame * .004);
    }
    check(smooth, "mapped presentation advances every display frame using audible DAC history without an IPC poll");
    check(reader.readPresentation(9'000'000'000LL, seconds) && near(seconds, .04),
          "mapped presentation stops at the last rendered sample when the device stalls");
    transport.seek(48000);
    check(!reader.readPresentation(1'020'000'000LL, seconds),
          "seek immediately invalidates the old mapped presentation generation");
    transport.advance(960); // callback timing still belongs to the previous seek generation
    check(!reader.readPresentation(1'030'000'000LL, seconds),
          "an in-flight block from before seek cannot revive old presentation history");
    transport.setLoopRange(48000, 49920); transport.setLoopEnabled(true);
    transport.setPresentationTiming(2'000'000'000LL, engine::PresentationClockSource::DeviceTimestamp);
    transport.advance(960);
    check(reader.readPresentation(2'015'000'000LL, seconds) && near(seconds, 1.035) &&
          reader.readPresentation(2'030'000'000LL, seconds) && near(seconds, 1.0),
          "mapped presentation uses the current loop and wraps only at the audible edge");
    transport.pause();
    check(!reader.readPresentation(2'015'000'000LL, seconds), "pause immediately rejects moving presentation");
    transport.play();
    check(!reader.readPresentation(2'015'000'000LL, seconds), "resume waits for its own rendered generation");
    transport.setPresentationTiming(3'000'000'000LL, engine::PresentationClockSource::DeviceTimestamp);
    transport.advance(960);
    check(reader.readPresentation(3'005'000'000LL, seconds), "resume publishes fresh mapped presentation");
    transport.setSampleRate(48000);
    check(!reader.readPresentation(3'005'000'000LL, seconds), "same-rate device restart retires previous DAC history");
    slot->generation.store(0, std::memory_order_release);
    transport.bindInputClock(nullptr);
    check(!reader.readPresentation(3'005'000'000LL, seconds), "retired mapping cannot supply stale presentation");
}

AudioSessionPacket session(double tempo = 120) {
    AudioSessionPacket packet; packet.generation = packet.revision = 1; packet.blockSize = 64;
    AudioGraphSpec::Channel channel; channel.id = "source"; packet.session.graph.channels.push_back(channel);
    AudioTransportCommand rate; rate.action = AudioTransportCommand::Action::Tempo; rate.value = tempo;
    AudioTransportCommand loop; loop.action = AudioTransportCommand::Action::LoopRange; loop.end = 64;
    AudioTransportCommand enabled; enabled.action = AudioTransportCommand::Action::LoopEnabled; enabled.enabled = true;
    packet.transport = {rate, loop, enabled, {AudioTransportCommand::Action::StartPlayback}};
    return packet;
}

void processClocks(const std::string& executable) {
    AudioRuntimeProcess process(executable);
    auto packet = session();
    if (!check(bool(process.replaceSession(packet)), "start audio child with shared native clock")) return;
    const auto primary = process.inputClock();
    if (!check(primary.valid() && bool(process.advanceForTest(64, 4)) &&
        near(primary.inputBeatsAt(farFuture), 4.0 * 64 / 48000 * 2),
        "primary mapped clock advances without any UI snapshot poll")) return;
    double presentation = -1;
    check(primary.readPresentation(std::numeric_limits<std::int64_t>::max(), presentation) && near(presentation, 0),
          "real child publishes its audible loop position directly into the shared clock");
    std::uint64_t id = 0;
    if (!check(bool(process.createSession(session(90), id)), "create separate audition clock")) return;
    const auto secondary = process.inputClock(id);
    check(secondary.valid() && bool(process.advanceForTest(64, 3, id)) &&
          near(secondary.inputBeatsAt(farFuture), 3.0 * 64 / 48000 * 1.5),
          "secondary timestamp uses its own unwrapped tempo and generation");
    check(secondary.readPresentation(std::numeric_limits<std::int64_t>::max(), presentation) && near(presentation, 0),
          "audition session has an independent presentation clock");
    const auto primaryBefore = primary.inputBeatsAt(farFuture);
    const auto secondaryBefore = secondary.inputBeatsAt(farFuture);
    check(bool(process.startAudition(id)) && bool(process.advanceForTest(64, 2)) &&
          near(primary.inputBeatsAt(farFuture), primaryBefore) &&
          near(secondary.inputBeatsAt(farFuture), secondaryBefore + 2.0 * 64 / 48000 * 1.5),
          "audition updates its clock from Transport::advance while bypassing its DeviceCallback");
    check(bool(process.closeSession(id)) && !secondary.valid(),
          "closing an active audition drains and retires its mapped clock");
    check(!secondary.readPresentation(engine::presentationNowNs(), presentation),
          "closing an audition also retires its presentation reader");
    std::uint64_t another = 0;
    check(bool(process.createSession(session(), another)) && another != id &&
          process.inputClock(another).valid() && !secondary.valid(),
          "new session reuses capacity without reviving a retired reader");
    packet.generation = 2;
    check(bool(process.replaceSession(packet)) && !primary.valid() && process.inputClock().valid(),
          "same-child generation replacement retires the previous clock identity");
    const auto replaced = process.inputClock();
    check(bool(process.restart()) && !replaced.valid() && process.inputClock().valid(),
          "whole-child restart publishes a new clock and safely retains old reader mappings");
    const auto current = process.inputClock();
    process.close();
    check(!current.valid() && current.inputBeatsAt(farFuture) == 0 && !primary.valid() && !secondary.valid(),
          "held readers remain safe after process shutdown without returning another generation's clock");
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    scalarClock(); identities(); presentationClock(); processClocks(argv[1]);
    return failures ? 1 : 0;
}
