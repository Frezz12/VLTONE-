#include "AudioRuntimeDurableEdits.hpp"
#include "Internal/ChannelColorInstance.hpp"

#include <algorithm>
#include <stdexcept>

namespace daw::audio_rpc {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}
const AudioPluginSpec* spec(const AudioSessionPacket& packet, const AudioPluginAddress& address) {
    for (const auto& chain : packet.session.pluginChains) if (chain.channelId == address.channelId)
        for (const auto& value : chain.slots) if (value.id == address.slotId) return &value;
    return nullptr;
}
bool sameSide(const AudioPluginAddress& a, const AudioPluginAddress& b) {
    return a.channelId == b.channelId && a.slotId == b.slotId && a.right == b.right;
}
bool sameSlot(const AudioPluginCheckpoint& checkpoint, const AudioPluginAddress& address) {
    return checkpoint.channelId == address.channelId && checkpoint.slotId == address.slotId;
}
template<Method Id> auto decodeInputs(std::span<const std::uint8_t> payload,
    const std::filesystem::path& directory) {
    auto [values] = audio_value::decodeResources<typename Binding<Id>::Inputs>(payload, directory);
    return values;
}
}

DurableEdits::Identity DurableEdits::identity(const AudioSessionPacket& packet, const AudioPluginAddress& address) {
    const auto* value = spec(packet, address);
    require(value && (!address.right || value->channelMode == PluginChannelMode::DualMono),
        "Durable plugin target is absent from the acknowledged session.");
    return {address, value->uid, value->descriptor.version, value->descriptor.parameterFingerprint, value->requiredFormat};
}
bool DurableEdits::matches(const Identity& value, const AudioSessionPacket& packet) {
    const auto* current = spec(packet, value.address);
    return current && current->uid == value.uid && current->requiredFormat == value.format &&
        (!value.address.right || current->channelMode == PluginChannelMode::DualMono) &&
        (!current->requireExactVersion || (current->requiredVersion == value.version &&
         (current->requiredParameterFingerprint.empty() || current->requiredParameterFingerprint == value.fingerprint)));
}

DurableEdits::Prepared DurableEdits::prepare(Method method, std::span<const std::uint8_t> payload,
    const std::filesystem::path& directory, const AudioSessionPacket& packet) const {
    Prepared result; result.method = method;
    const auto host = [&]<Method Id>(typename Binding<Id>::Inputs values, Identity target = {}) {
        Entry entry{std::move(target), Call<Id>{std::move(values)}};
        const auto& address = entry.identity.address;
        const Key key{Id, address.channelId, address.slotId, address.right};
        const auto previous = entries.find(key);
        if constexpr (Id == Method::setPluginControls) {
            auto& controls = std::get<2>(std::get<Call<Id>>(entry.host).values);
            if (previous != entries.end() && matches(previous->second.identity, packet)) {
                const auto& old = std::get<2>(std::get<Call<Id>>(previous->second.host).values);
                if (!controls.mix) controls.mix = old.mix;
                if (!controls.bypassed) controls.bypassed = old.bypassed;
            }
        }
        // PCM is immutable mapped storage with its own resource limits. Count
        // owned control data and container overhead, not the mapped file size.
        entry.bytes = sizeof(Entry) + sizeof(Key) + 128 + payload.size() +
            address.channelId.size() * 2 + address.slotId.size() * 2 + entry.identity.uid.size() +
            entry.identity.version.size() + entry.identity.fingerprint.size();
        if constexpr (Id == Method::configureChannelColor) {
            const auto& parameters = std::get<2>(std::get<Call<Id>>(entry.host).values);
            entry.bytes += parameters.size() * sizeof(InsertParameter);
            for (const auto& parameter : parameters) entry.bytes += parameter.id.size();
        }
        const auto replaced = previous == entries.end() ? 0 : previous->second.bytes;
        require(entry.bytes <= maxBytes && bytes - replaced <= maxBytes - entry.bytes &&
            (previous != entries.end() || entries.size() < maxEntries), "Durable audio controls exceed their bounded capacity.");
        result.staged.emplace(key, std::move(entry));
    };
    const auto source = [&](const AudioPluginAddress& address) {
        result.source = identity(packet, address);
        require(result.source->format == plugins::Format::Internal &&
            (result.source->uid == "daw.sampler" || result.source->uid == "daw.slicer"),
            "Durable source edits require a built-in sampler or slicer.");
    };
    switch (method) {
    case Method::setPluginControls: {
        auto values = decodeInputs<Method::setPluginControls>(payload, directory);
        auto target = identity(packet, {std::get<0>(values), std::get<1>(values)});
        host.template operator()<Method::setPluginControls>(std::move(values), std::move(target)); break;
    }
    case Method::setPluginSlide: {
        auto values = decodeInputs<Method::setPluginSlide>(payload, directory);
        auto target = identity(packet, std::get<0>(values));
        host.template operator()<Method::setPluginSlide>(std::move(values), std::move(target)); break;
    }
    case Method::configureChannelColor: {
        auto values = decodeInputs<Method::configureChannelColor>(payload, directory);
        auto target = identity(packet, std::get<0>(values));
        require(target.format == plugins::Format::Internal && target.uid == plugins::channel_color::ChannelColorInstance::uid(),
            "Durable channel color controls require the channel color processor.");
        host.template operator()<Method::configureChannelColor>(std::move(values), std::move(target)); break;
    }
    case Method::setMetronomeEnabled:
        host.template operator()<Method::setMetronomeEnabled>(decodeInputs<Method::setMetronomeEnabled>(payload, directory)); break;
    case Method::setMetronomeSample:
        host.template operator()<Method::setMetronomeSample>(decodeInputs<Method::setMetronomeSample>(payload, directory)); break;
    case Method::loadInstrumentSample:
        source(std::get<0>(decodeInputs<Method::loadInstrumentSample>(payload, directory))); break;
    case Method::clearInstrumentSample:
        source(std::get<0>(decodeInputs<Method::clearInstrumentSample>(payload, directory))); break;
    case Method::restoreSlicerState:
        source(std::get<0>(decodeInputs<Method::restoreSlicerState>(payload, directory))); break;
    case Method::setSlicerSlices:
        source(std::get<0>(decodeInputs<Method::setSlicerSlices>(payload, directory))); break;
    case Method::setSlicerAnalysis:
        source(std::get<0>(decodeInputs<Method::setSlicerAnalysis>(payload, directory))); break;
    default: break;
    }
    return result;
}

bool DurableEdits::accepted(const Prepared& prepared, std::span<const std::uint8_t> payload,
    const std::filesystem::path& directory, ProcessAudioResources::Cache& cache) {
    if (!prepared.tracked()) return false;
    if (prepared.method == Method::setMetronomeEnabled || prepared.method == Method::setMetronomeSample) {
        audio_value::decodeResources<Binding<Method::setMetronomeEnabled>::Reply>(payload, directory, &cache);
        return true;
    }
    // The remaining whitelist consists exclusively of bool-returning setters
    // without output references. Decode the full reply, including its manifest.
    auto [reply] = audio_value::decodeResources<Binding<Method::setPluginControls>::Reply>(payload, directory, &cache);
    return std::get<0>(reply);
}
void DurableEdits::acceptHost(Prepared&& prepared) {
    if (prepared.staged.empty()) return;
    auto node = prepared.staged.extract(prepared.staged.begin());
    if (const auto old = entries.find(node.key()); old != entries.end()) {
        bytes -= old->second.bytes;
        entries.erase(old);
    }
    bytes += node.mapped().bytes;
    entries.insert(std::move(node)); // allocated before dispatch
}

std::vector<AudioPluginStateRequest> DurableEdits::sourceRequests(const Prepared& prepared,
    const AudioSessionPacket& packet) {
    require(prepared.source.has_value() && matches(*prepared.source, packet), "Source postimage target changed.");
    const auto& address = prepared.source->address;
    std::vector<AudioPluginStateRequest> requests{{address, true, std::nullopt, AudioPluginSnapshotPurpose::Exact}};
    const auto prior = std::find_if(packet.checkpoints.begin(), packet.checkpoints.end(),
        [&](const auto& value) { return sameSlot(value, address); });
    if (spec(packet, address)->channelMode == PluginChannelMode::DualMono && prior == packet.checkpoints.end()) {
        auto other = address; other.right = !other.right; other.instance = 0;
        requests.push_back({other, true, std::nullopt, AudioPluginSnapshotPurpose::Exact});
    }
    return requests;
}

void DurableEdits::acceptSource(const Prepared& prepared, std::span<const AudioPluginStateSnapshot> snapshots,
    AudioSessionPacket& packet) {
    const auto requests = sourceRequests(prepared, packet);
    require(snapshots.size() == requests.size(), "Incomplete source postimage reply.");
    auto restores = packet.restores;
    auto checkpoints = packet.checkpoints;
    const auto& target = *prepared.source;
    auto checkpoint = std::find_if(checkpoints.begin(), checkpoints.end(),
        [&](const auto& value) { return sameSlot(value, target.address); });
    if (checkpoint == checkpoints.end()) {
        AudioPluginCheckpoint value;
        value.channelId = target.address.channelId; value.slotId = target.address.slotId;
        value.uid = target.uid; value.format = target.format;
        if (spec(packet, target.address)->channelMode == PluginChannelMode::DualMono) value.right.emplace();
        checkpoints.push_back(std::move(value)); checkpoint = std::prev(checkpoints.end());
    }
    require(checkpoint->uid == target.uid && checkpoint->format == target.format,
        "Source postimage checkpoint belongs to another processor.");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& expected = requests[i].address;
        const auto& snapshot = snapshots[i];
        require(sameSide(snapshot.address, expected) && (!expected.instance || snapshot.address.instance == expected.instance) &&
            snapshot.exists && !snapshot.failed && !snapshot.isolated && snapshot.ownsSample && snapshot.supportsState &&
            snapshot.stateCaptured && snapshot.descriptor.uid == target.uid && snapshot.descriptor.format == target.format &&
            (snapshot.sample || snapshot.samplePath.empty()), "Cannot retain a complete, healthy source postimage.");
        AudioPluginStateEdit edit;
        edit.address = expected; edit.address.instance = 0;
        edit.state.state = snapshot.state; edit.state.sourcePath = snapshot.samplePath; edit.state.source = snapshot.sample;
        edit.parameters = snapshot.parameters;
        const auto prior = std::find_if(restores.begin(), restores.end(), [&](const auto& value) { return sameSide(value.address, expected); });
        if (prior == restores.end()) restores.push_back(std::move(edit)); else *prior = std::move(edit);
        AudioPluginCheckpoint::Side side;
        side.hasState = true; side.state = snapshot.state; side.pending = snapshot.pending;
        if (expected.right) {
            require(checkpoint->right.has_value(), "Source postimage has no matching right side.");
            *checkpoint->right = std::move(side);
        } else checkpoint->left = std::move(side);
    }
    // All validation and allocation finished. Never leave an old native chunk
    // overriding the new source, or replace the healthy opposite-side chunk.
    packet.restores.swap(restores);
    packet.checkpoints.swap(checkpoints);
}

void DurableEdits::projected(const AudioSessionPacket& packet) {
    std::erase_if(entries, [&](const auto& item) {
        const auto method = std::get<0>(item.first);
        const bool keep = method == Method::setMetronomeSample ||
            (method == Method::setPluginSlide && item.second.identity.address.right && matches(item.second.identity, packet));
        if (!keep) bytes -= item.second.bytes;
        return !keep;
    });
}
audio::Result DurableEdits::replay(AudioRuntimeProcess& process, std::uint64_t id,
    const AudioSessionPacket& packet) const {
    for (const auto& [key, entry] : entries) {
        const auto method = std::get<0>(key);
        if (method != Method::setMetronomeEnabled && method != Method::setMetronomeSample && !matches(entry.identity, packet)) continue;
        const auto status = std::visit([&](const auto& command) {
            using T = std::decay_t<decltype(command)>;
            auto values = command.values;
            if constexpr (T::method == Method::setPluginSlide || T::method == Method::configureChannelColor)
                std::get<0>(values).instance = 0;
            auto [result, reply] = call<T::method>(process, id, std::move(values));
            if (!result) return result;
            if constexpr (std::is_same_v<typename Binding<T::method>::Result, bool>)
                if (!std::get<0>(reply)) return audio::Result::fail(audio::EngineError::PluginLoadFailed,
                    "Recovered plugin rejected an acknowledged host control.");
            return audio::Result::ok();
        }, entry.host);
        if (!status) return status;
    }
    return audio::Result::ok();
}

} // namespace daw::audio_rpc
