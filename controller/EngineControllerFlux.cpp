#include "EngineController.hpp"
#include <algorithm>

namespace daw {
namespace {
double fluxValue(const plugins::flux::State &state, unsigned id) {
  using namespace plugins::flux;
  if (id < GlobalCount)
    return state.globals[id];
  const unsigned slot = (id - GlobalCount) / bankSize,
                 local = (id - GlobalCount) % bankSize;
  unsigned selected = unsigned(state.globals[SceneSelect]);
  const auto &scene =
      state.scenes[selected < state.scenes.size() ? selected : 0];
  const auto &module = scene.modules[slot];
  switch (local) {
  case Enabled:
    return module.enabled;
  case SlotMix:
    return module.mix;
  case SlotOutput:
    return module.output;
  case Solo:
    return module.solo;
  case DepthA:
  case DepthB:
    return module.curves[local - DepthA].depth;
  case RateA:
  case RateB: {
    auto &c = module.curves[local - RateA];
    return c.sync ? c.bars : c.hz;
  }
  default:
    break;
  }
  const auto kind = Kind(1 + (local - CommonCount) / fieldCount);
  return kind == state.modules[slot].kind
             ? module.values[(local - CommonCount) % fieldCount]
             : parameterTable()[id].defaultValue;
}
} // namespace
std::optional<FluxSnapshot>
EngineController::fluxSnapshot(const std::string &channel,
                               const std::string &insert) const {
  return m_runtime->fluxSnapshot(pluginAddress(channel, insert));
}
std::optional<plugins::flux::FluxTelemetry>
EngineController::fluxTelemetry(const std::string &channel,
                                const std::string &insert) const {
  return m_runtime->fluxTelemetry(pluginAddress(channel, insert));
}
bool EngineController::applyFluxState(const std::string &channel,
                                      const std::string &insert,
                                      const plugins::flux::State &state,
                                      const std::string &label, bool history,
                                      const plugins::flux::State *previous) {
  if (cloudProjectBound() || !plugins::flux::validState(state))
    return false;
  auto before = fluxSnapshot(channel, insert);
  if (!before)
    return false;
  unfreezeTrack(channel, false);
  if (!m_runtime->setFluxState(pluginAddress(channel, insert), state))
    return false;
  // Keep the project's parameter snapshot and pending host events in agreement
  // with the prepared state, including undo after a live drag gesture.
  using namespace plugins::flux;
  auto sync = [&](unsigned id) {
    double value = fluxValue(state, id);
    if (fluxValue(before->state, id) != value ||
        (previous && fluxValue(*previous, id) != value))
      setInsertParameter(channel, insert, parameterTable()[id].id, value);
  };
  for (unsigned id = 0; id < GlobalCount; ++id)
    sync(id);
  for (unsigned slot = 0; slot < slotCount; ++slot) {
    for (unsigned field = 0; field < CommonCount; ++field)
      sync(commonParam(slot, field));
    if (state.modules[slot].kind != Kind::Empty)
      for (unsigned field = 0; field < fields(state.modules[slot].kind).size();
           ++field)
        sync(effectParam(slot, state.modules[slot].kind, field));
  }
  if (history) {
    auto old = std::make_shared<plugins::flux::State>(previous ? *previous
                                                               : before->state);
    auto next = std::make_shared<plugins::flux::State>(state);
    auto project = m_projectGeneration;
    m_undo.push(
        label,
        [this, channel, insert, old, project] {
          if (m_projectGeneration == project)
            applyFluxState(channel, insert, *old, "Change Flux", false);
        },
        [this, channel, insert, next, project] {
          if (m_projectGeneration == project)
            applyFluxState(channel, insert, *next, "Change Flux", false);
        });
  }
  return true;
}
} // namespace daw
