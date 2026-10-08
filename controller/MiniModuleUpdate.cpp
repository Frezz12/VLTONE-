#include "MiniModuleUpdate.hpp"
#include "AudioGraphSpec.hpp"
#include "model/ChannelColor.hpp"

#include <thread>

namespace daw {

MiniModuleUpdate::MiniModuleUpdate(const MiniModuleUpdate& other)
    : definition(other.definition), projectId(other.projectId), info(other.info),
      targets(other.targets), compiler(other.compiler), exactRestore(other.exactRestore) {
    for (auto& target : targets) target.prepared = false;
}

MiniModuleUpdate::~MiniModuleUpdate() {
    if (preparationId) if (auto endpoint = compiler.lock()) endpoint->forget(preparationId);
}

bool MiniModuleUpdate::prepare() {
    error = exactRestore ? std::string{} : plugins::mini::validate(definition);
    if (!error.empty()) return false;
    auto endpoint = compiler.lock();
    if (preparationId && endpoint) endpoint->forget(preparationId);
    preparationId = 0;
    AudioMiniModuleCompileRequest request;
    request.info = info;
    for (auto& target : targets) {
        target.prepared = false;
        if (!target.audioChanged) continue;
        AudioPluginSpec spec;
        spec.id = target.after.id; spec.uid = target.after.uid; spec.name = target.after.name;
        spec.miniModule = target.after.miniModule; spec.miniModuleMode = target.after.miniModuleMode;
        spec.profileSeed = parseChannelColorSeed(target.after.profileSeed);
        spec.parameters = target.after.parameters; spec.bypassed = target.after.bypassed;
        spec.preferredChannels = std::uint16_t(target.channels);
        request.targets.push_back({{target.channel.empty() ? AudioGraphSpec::masterChannelId : target.channel,
            target.before.id, false, target.instance}, std::move(spec)});
    }
    if (request.targets.empty()) return true;
    if (!endpoint) { error = "Audio runtime is no longer available"; return false; }
    try {
        preparationId = endpoint->start(std::move(request));
        if (!preparationId) { error = "Audio compiler is busy"; return false; }
        for (;;) {
            auto status = endpoint->poll(preparationId);
            if (status.state == AudioMiniModuleCompileStatus::State::Pending) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (status.state != AudioMiniModuleCompileStatus::State::Ready) {
                error = status.error.empty() ? "Audio compilation was cancelled" : std::move(status.error);
                return false;
            }
            for (auto& target : targets) if (target.audioChanged) {
                const auto channel = target.channel.empty() ? AudioGraphSpec::masterChannelId : target.channel;
                for (const auto& ready : status.targets)
                    if (ready.address.channelId == channel && ready.address.slotId == target.before.id &&
                        ready.address.instance == target.instance) {
                        target.prepared = true; target.preparedLatency = ready.latency;
                    }
                if (!target.prepared) { error = "Incomplete audio compilation result"; return false; }
            }
            return true;
        }
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

} // namespace daw
