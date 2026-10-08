#include "AudioRuntime.hpp"
#include "AudioMiniModuleCompiler.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/ChannelColorInstance.hpp"

#include <map>
#include <mutex>

namespace daw {

class AudioRuntime::MiniModuleCompiler final : public AudioMiniModuleCompiler {
public:
    struct Prepared {
        AudioMiniModuleCompileRequest request;
        std::vector<std::shared_ptr<plugins::PluginNode>> nodes;
        std::string error;
    };
    std::uint64_t start(AudioMiniModuleCompileRequest request) override {
        const std::lock_guard lock(m_mutex);
        prune();
        if (m_jobs.size() >= 16 || request.targets.empty()) return 0;
        static std::atomic<std::uint64_t> next{1};
        const auto id = next.fetch_add(1, std::memory_order_relaxed);
        Entry job;
        job.result = std::async(std::launch::async, [request = std::move(request)]() mutable {
            Prepared result;
            result.request = std::move(request);
            try {
                for (const auto& target : result.request.targets) {
                    const auto& spec = target.spec;
                    auto instance = std::make_unique<plugins::mini::MiniModuleInstance>();
                    if (!spec.miniModule || !instance->configure(*spec.miniModule, spec.profileSeed, spec.miniModuleMode)) {
                        result.error = instance->error().empty() ? "Missing or invalid mini-module definition" : instance->error();
                        break;
                    }
                    for (const auto& parameter : spec.parameters) {
                        const auto index = instance->parameterIndexForId(parameter.id);
                        if (index >= 0) instance->setParameterFromHost(unsigned(index), parameter.value);
                    }
                    auto node = std::make_shared<plugins::PluginNode>(spec.name, std::move(instance));
                    node->setPreferredChannelCount(spec.preferredChannels);
                    node->setBypassed(spec.bypassed); node->setMix(spec.mix);
                    node->prepare(result.request.info);
                    if (!node->isReady()) { result.error = "Could not prepare mini module: " + spec.name; break; }
                    node->markPrepared(result.request.info);
                    node->reset();
                    result.nodes.push_back(std::move(node));
                }
            } catch (const std::exception& error) { result.error = error.what(); }
            catch (...) { result.error = "Mini-module preparation failed"; }
            return result;
        });
        m_jobs.emplace(id, std::move(job));
        return id;
    }
    AudioMiniModuleCompileStatus poll(std::uint64_t id) override {
        const std::lock_guard lock(m_mutex);
        prune();
        const auto found = m_jobs.find(id);
        if (found == m_jobs.end() || found->second.forgotten) return {};
        auto& entry = found->second;
        if (!resolve(entry)) return {AudioMiniModuleCompileStatus::State::Pending};
        return entry.status;
    }
    void forget(std::uint64_t id) override {
        const std::lock_guard lock(m_mutex);
        if (const auto found = m_jobs.find(id); found != m_jobs.end()) found->second.forgotten = true;
        // Destroying a pending std::async future blocks. Retain forgotten jobs
        // until completion and reclaim them on the next control-thread request.
        prune();
    }
    std::optional<Prepared> take(std::uint64_t id) {
        const std::lock_guard lock(m_mutex);
        const auto found = m_jobs.find(id);
        if (found == m_jobs.end() || found->second.forgotten || found->second.taken || !resolve(found->second) ||
            !found->second.ready->error.empty()) return std::nullopt;
        found->second.taken = true;
        auto result = std::move(found->second.ready);
        // The prepared owners are consumed once. Keep only value metadata so
        // a failed publication can still cancel the old processor's fade.
        return result;
    }
private:
    struct Entry {
        std::future<Prepared> result;
        std::optional<Prepared> ready;
        AudioMiniModuleCompileStatus status;
        bool forgotten = false, taken = false;
    };
    static bool resolve(Entry& entry) {
        if (entry.ready) return true;
        if (entry.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        try { entry.ready = entry.result.get(); }
        catch (const std::exception& error) { entry.ready.emplace(); entry.ready->error = error.what(); }
        catch (...) { entry.ready.emplace(); entry.ready->error = "Mini-module compilation failed"; }
        entry.status.state = entry.ready->error.empty() ? AudioMiniModuleCompileStatus::State::Ready
                                                       : AudioMiniModuleCompileStatus::State::Failed;
        entry.status.error = entry.ready->error;
        for (std::size_t i = 0; i < entry.ready->nodes.size(); ++i)
            entry.status.targets.push_back({entry.ready->request.targets[i].address,
                                            entry.ready->nodes[i]->latencySamples()});
        return true;
    }
    void prune() {
        for (auto it = m_jobs.begin(); it != m_jobs.end();)
            if (it->second.forgotten && resolve(it->second)) it = m_jobs.erase(it);
            else ++it;
    }
    std::mutex m_mutex;
    std::map<std::uint64_t, Entry> m_jobs;
};

engine::PrepareInfo AudioRuntime::preparation() const { return engine.prepareInfo(); }

std::weak_ptr<AudioMiniModuleCompiler> AudioRuntime::miniModuleCompiler() const {
    if (!miniCompiler) miniCompiler = std::make_shared<MiniModuleCompiler>();
    return miniCompiler;
}

bool AudioRuntime::stageMiniModulePreparation(std::uint64_t id) {
    if (!miniCompiler) return false;
    auto compiled = miniCompiler->take(id);
    if (!compiled || compiled->request.info != engine.prepareInfo() ||
        compiled->nodes.size() != compiled->request.targets.size()) return false;
    auto staged = preparedMiniModules;
    for (std::size_t i = 0; i < compiled->nodes.size(); ++i) {
        const auto& address = compiled->request.targets[i].address;
        if (!address.instance || pluginInstanceId(address) != address.instance) return false;
        const auto key = (address.channelId == AudioGraphSpec::masterChannelId ? std::string{} : address.channelId)
            + "\n" + address.slotId;
        staged[key] = std::move(compiled->nodes[i]);
    }
    preparedMiniModules = std::move(staged);
    return true;
}

void AudioRuntime::clearMiniModulePreparation() { preparedMiniModules.clear(); }

void AudioRuntime::fadeMiniModulePreparation(std::uint64_t id, bool cancel) {
    if (!miniCompiler) return;
    for (const auto& target : miniCompiler->poll(id).targets) {
        auto* node = pluginNode(target.address);
        auto* current = node ? dynamic_cast<plugins::mini::MiniModuleInstance*>(node->instance()) : nullptr;
        if (!current || current->latencySamples() == target.latency) continue;
        if (cancel) current->cancelUpdateFadeOut();
        else if (!node->isBypassed()) current->requestUpdateFadeOut();
    }
}

bool AudioRuntime::miniModulePreparationFaded(std::uint64_t id) const {
    if (!miniCompiler) return true;
    for (const auto& target : miniCompiler->poll(id).targets) {
        const auto* node = pluginNode(target.address);
        const auto* current = node ? dynamic_cast<const plugins::mini::MiniModuleInstance*>(node->instance()) : nullptr;
        if (current && !node->isBypassed() && current->latencySamples() != target.latency &&
            !current->updateFadeOutFinished()) return false;
    }
    return true;
}

bool AudioRuntime::configureChannelColor(const AudioPluginAddress& address, std::uint64_t seed,
    std::span<const InsertParameter> parameters, bool bypassed) {
    auto* node = pluginNode(address);
    auto* color = node ? dynamic_cast<plugins::channel_color::ChannelColorInstance*>(node->instance()) : nullptr;
    if (!node || !node->instance()) return false;
    if (color && color->profileSeed() != seed) {
        const engine::RealtimeEngine::RenderGate gate(engine);
        color->setProfileSeed(seed);
    }
    applyStoredParameters(*node, parameters);
    node->setBypassed(bypassed);
    return true;
}

} // namespace daw
