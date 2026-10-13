#include "EngineController.hpp"
#include <cmath>

namespace daw {
std::optional<MorphSnapshot>
EngineController::morphSnapshot(const std::string &channel,
                                const std::string &insert) const {
  return m_runtime->morphSnapshot(pluginAddress(channel, insert));
}
bool EngineController::applyMorphState(const std::string &channel,
                                       const std::string &insert,
                                       const plugins::morph::State &state,
                                       const std::string &label, bool history,
                                       const plugins::morph::State *previous) {
  if (cloudProjectBound() || !plugins::morph::validProgram(state.canvas))
    return false;
  const auto before = morphSnapshot(channel, insert);
  if (!before)
    return false;
  for (const auto &info : plugins::morph::parameterTable()) {
    const double value = state.values[info.index];
    if (!std::isfinite(value) || value < info.minValue || value > info.maxValue)
      return false;
  }
  unfreezeTrack(channel, false);
  const auto identity = insertIdentity(channel, insert);
  const auto address = pluginAddress(channel, insert, identity.instance);
  if (!m_runtime->setMorphCanvas(address, state.canvas, state.category))
    return false;
  // Audio may still be consuming an older preset's events. Include the last
  // requested UI state in the comparison, without resending unchanged values
  // on every Canvas brush movement.
  for (const auto &info : plugins::morph::parameterTable())
    if (before->state.values[info.index] != state.values[info.index] ||
        (previous && previous->values[info.index] != state.values[info.index]))
      setInsertParameter(channel, insert, info.id, state.values[info.index]);
  m_runtime->setInsertPresetReference(address, state.presetKind,
                                      state.presetName);
  if (history) {
    auto old = std::make_shared<plugins::morph::State>(
        previous ? *previous : before->state);
    auto next = std::make_shared<plugins::morph::State>(state);
    const auto project = m_projectGeneration;
    m_undo.push(
        label,
        [this, channel, insert, old, next, project] {
          if (m_projectGeneration == project)
            applyMorphState(channel, insert, *old, "Change Morph", false,
                            next.get());
        },
        [this, channel, insert, old, next, project] {
          if (m_projectGeneration == project)
            applyMorphState(channel, insert, *next, "Change Morph", false,
                            old.get());
        });
  }
  return true;
}
} // namespace daw
