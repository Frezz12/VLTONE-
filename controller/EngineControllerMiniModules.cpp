#include "EngineController.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "MiniModuleUpdate.hpp"
#include "model/ChannelColor.hpp"
#include "model/MiniModules.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace daw {
namespace {
bool sameMiniAudio(const plugins::mini::MiniModuleDefinition &a,
                   std::string_view am,
                   const plugins::mini::MiniModuleDefinition &b,
                   std::string_view bm) {
  return plugins::mini::sameAudioGraph(a, am, b, bm);
}
} // namespace
bool MiniModuleUpdate::prepare() {
  error = plugins::mini::validate(definition);
  if (!error.empty())
    return false;
  try {
    for (auto &target : targets) {
      if (!target.audioChanged)
        continue;
      auto instance = std::make_unique<plugins::mini::MiniModuleInstance>();
      if (!instance->configure(definition,
                               parseChannelColorSeed(target.after.profileSeed),
                               target.after.miniModuleMode)) {
        error = instance->error();
        return false;
      }
      for (const auto &parameter : target.after.parameters) {
        const auto index = instance->parameterIndexForId(parameter.id);
        if (index >= 0)
          instance->setParameterFromHost(unsigned(index), parameter.value);
      }
      auto node = std::make_shared<plugins::PluginNode>(target.after.name,
                                                        std::move(instance));
      node->setPreferredChannelCount(std::uint16_t(target.channels));
      node->setBypassed(target.after.bypassed);
      node->setMix(1);
      node->prepare(info);
      node->markPrepared(info);
      if (!node->isReady()) {
        error = "Could not prepare mini module: " + target.after.name;
        return false;
      }
      node->reset();
      target.prepared = std::move(node);
    }
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
std::shared_ptr<MiniModuleUpdate> EngineController::planMiniModuleUpdate(
    const plugins::mini::MiniModuleDefinition &definition) const {
  auto result = std::make_shared<MiniModuleUpdate>();
  result->definition = definition;
  result->projectId = m_project.miniModuleProjectId;
  result->info = {m_engine.sampleRate(), m_engine.maxBlockSize(),
                  m_engine.channels()};
  const auto collect = [&](const std::string &channel, const auto &modules,
                           unsigned channels) {
    for (const auto &slot : modules)
      if (slot.miniModule && slot.miniModule->id == definition.id) {
        MiniModuleUpdate::Target target;
        target.channel = channel;
        target.before = target.after = slot;
        target.channels = channels;
        target.after.name = definition.name;
        target.after.miniModule = definition;
        if (!target.after.miniModuleMode.empty() &&
            std::none_of(definition.modes.begin(), definition.modes.end(),
                         [&](const auto &m) {
                           return m.id == target.after.miniModuleMode;
                         }))
          target.after.miniModuleMode = definition.defaultMode;
        target.after.parameters.clear();
        for (const auto &control : definition.controls) {
          double value = control.initial;
          for (const auto &parameter : slot.parameters)
            if (parameter.id == control.id)
              value =
                  std::clamp(parameter.value, control.minimum, control.maximum);
          target.after.parameters.push_back({control.id, value});
        }
        target.audioChanged =
            !sameMiniAudio(*slot.miniModule, slot.miniModuleMode, definition,
                           target.after.miniModuleMode);
        result->targets.push_back(std::move(target));
      }
  };
  collect({}, m_project.masterMiniModules, 2);
  for (const auto &track : m_project.tracks)
    collect(track.id, track.miniModules, track.mono ? 1 : 2);
  return result;
}
bool EngineController::miniModuleUpdateCurrent(
    const MiniModuleUpdate &update) const {
  if (m_project.miniModuleProjectId != update.projectId ||
      m_engine.sampleRate() != update.info.sampleRate ||
      m_engine.maxBlockSize() != update.info.maxBlockSize ||
      m_engine.channels() != update.info.channels)
    return false;
  const auto current = planMiniModuleUpdate(update.definition);
  if (current->targets.size() != update.targets.size())
    return false;
  for (unsigned i = 0; i < current->targets.size(); ++i) {
    const auto &a = current->targets[i], &b = update.targets[i];
    if (a.channel != b.channel || a.before.id != b.before.id ||
        a.before.miniModule != b.before.miniModule ||
        a.before.miniModuleMode != b.before.miniModuleMode ||
        a.before.profileSeed != b.before.profileSeed ||
        a.channels != b.channels)
      return false;
  }
  return true;
}
void EngineController::fadeMiniModuleUpdate(const MiniModuleUpdate &update) {
  if (cloudProjectBound() || !sharedEditingAllowed()) return;
  for (const auto &target : update.targets) {
    if (!target.prepared)
      continue;
    if (auto *current = dynamic_cast<plugins::mini::MiniModuleInstance *>(
            insertInstance(target.channel, target.before.id)))
      if (current->latencySamples() != target.prepared->latencySamples() &&
          !target.before.bypassed)
        current->requestUpdateFadeOut();
  }
}
void EngineController::cancelMiniModuleUpdateFade(const MiniModuleUpdate &update) {
  for (const auto &target : update.targets) {
    if (!target.prepared) continue;
    if (auto *current = dynamic_cast<plugins::mini::MiniModuleInstance *>(insertInstance(target.channel, target.before.id)))
      if (current->latencySamples() != target.prepared->latencySamples() && !target.before.bypassed)
        current->cancelUpdateFadeOut();
  }
}
bool EngineController::miniModuleUpdateFaded(
    const MiniModuleUpdate &update) const {
  if (!isPlaying())
    return true;
  for (const auto &target : update.targets) {
    if (!target.prepared || target.before.bypassed)
      continue;
    auto *instance = const_cast<EngineController *>(this)->insertInstance(
        target.channel, target.before.id);
    auto *current = dynamic_cast<plugins::mini::MiniModuleInstance *>(instance);
    if (current &&
        current->latencySamples() != target.prepared->latencySamples() &&
        !current->updateFadeOutFinished())
      return false;
  }
  return true;
}
bool EngineController::applyMiniModuleUpdate(
    const std::shared_ptr<MiniModuleUpdate> &update, std::string &error) {
  error = update ? update->error : "No compiled update";
  if (!update || !error.empty())
    return false;
  // The application keeps its collaboration bridge attached in local projects
  // too. Only an actual cloud binding excludes local mini-module edits.
  if (m_exportInProgress || cloudProjectBound() || !sharedEditingAllowed()) {
    error = "Project is not available for a local module update";
    return false;
  }
  if (!miniModuleUpdateCurrent(*update)) {
    error = "The project changed during compilation";
    return false;
  }
  std::map<std::string, std::vector<InsertModel>> before, after;
  std::set<std::string> audioChannels;
  for (const auto &target : update->targets) {
    if (!sharedGestureAllowed("plugin:" + target.before.id)) {
      error = "A module is being edited";
      return false;
    }
    if (!before.contains(target.channel))
      before[target.channel] = after[target.channel] =
          miniModules(target.channel);
    auto &list = after[target.channel];
    for (auto &slot : list)
      if (slot.id == target.before.id) {
        auto parameters = slot.parameters;
        const auto bypass = slot.bypassed, post = slot.miniModulePostFx;
        slot = target.after;
        slot.bypassed = bypass;
        slot.miniModulePostFx = post;
        for (auto &p : slot.parameters)
          for (const auto &old : parameters)
            if (p.id == old.id) {
              const auto &controls = slot.miniModule->controls;
              auto control =
                  std::find_if(controls.begin(), controls.end(),
                               [&](const auto &c) { return c.id == p.id; });
              p.value =
                  std::clamp(old.value, control->minimum, control->maximum);
            }
      }
    if (target.audioChanged)
      audioChannels.insert(target.channel);
    if (target.audioChanged && !target.prepared) {
      error = "Mini-module DSP was not prepared";
      return false;
    }
  }
  if (update->targets.empty())
    return true;
  for (const auto &target : update->targets)
    if (target.prepared)
      m_preparedMiniModules[target.channel + "\n" + target.before.id] =
          target.prepared;
  const auto apply = [this, audioChannels](const auto &state) {
    for (const auto &[id, slots] : state) {
      if (audioChannels.contains(id))
        unfreezeTrack(id, false);
      if (auto *destination = miniModulesFor(m_project, id))
        *destination = slots;
    }
    return rebuildGraph();
  };
  const auto published = apply(after);
  m_preparedMiniModules.clear();
  if (!published) {
    (void)apply(before);
    error = published.message();
    return false;
  }
  m_undo.push(
      "Recompile " + update->definition.name,
      [apply, before] { apply(before); }, [apply, after] { apply(after); });
  return true;
}
bool EngineController::updateMiniModuleDefinition(
    const plugins::mini::MiniModuleDefinition &definition, std::string &error) {
  auto update = planMiniModuleUpdate(definition);
  if (!update->prepare()) {
    error = update->error;
    return false;
  }
  return applyMiniModuleUpdate(update, error);
}
engine::NodeId EngineController::connectMiniModules(
    engine::AudioGraph &graph, TrackChannel &channel,
    const std::vector<InsertModel> &models, bool postFx, engine::NodeId head,
    engine::NodeId *first) {
  for (auto &slot : channel.miniModules) {
    const auto model =
        std::find_if(models.begin(), models.end(),
                     [&](const auto &m) { return m.id == slot.slotId; });
    if (model == models.end() || model->miniModulePostFx != postFx)
      continue;
    std::vector<engine::NodeId> ids;
    head = connectSlots(graph, std::span(&slot, 1), ids, head);
    if (first && *first == engine::kInvalidNode && !ids.empty())
      *first = ids.front();
  }
  return head;
}
const std::vector<InsertModel> &
EngineController::miniModules(const std::string &id) const {
  static const std::vector<InsertModel> empty;
  const auto *list = miniModulesFor(m_project, id);
  return list ? *list : empty;
}
void EngineController::applyMiniModules(const std::string &id,
                                        const std::vector<InsertModel> &modules,
                                        bool affectsAudio) {
  if (modules.size() > plugins::mini::kMaxModules ||
      !miniModulesFor(m_project, id))
    return;
  if (affectsAudio)
    unfreezeTrack(id, false);
  *miniModulesFor(m_project, id) = modules;
  rebuildGraph();
}
std::string
EngineController::addMiniModule(const std::string &id,
                                const plugins::mini::MiniModuleDefinition &d) {
  const auto *list = miniModulesFor(m_project, id);
  if (!list || list->size() >= plugins::mini::kMaxModules ||
      !plugins::mini::validate(d).empty() || cloudProjectBound() ||
      !sharedEditingAllowed())
    return {};
  auto module = makeMiniModule(d);
  const auto before = *list;
  auto after = before;
  after.push_back(module);
  applyMiniModules(id, after);
  m_undo.push(
      "Add " + module.name,
      [this, id, before] { applyMiniModules(id, before); },
      [this, id, after] { applyMiniModules(id, after); });
  return module.id;
}
bool EngineController::removeMiniModule(const std::string &id,
                                        const std::string &moduleId) {
  const auto before = miniModules(id);
  auto after = before;
  if (!sharedGestureAllowed("plugin:" + moduleId) || cloudProjectBound() ||
      !sharedEditingAllowed() ||
      !std::erase_if(after, [&](const auto &m) { return m.id == moduleId; }))
    return false;
  applyMiniModules(id, after);
  m_undo.push(
      "Remove mini module",
      [this, id, before] { applyMiniModules(id, before); },
      [this, id, after] { applyMiniModules(id, after); });
  return true;
}
bool EngineController::moveMiniModule(const std::string &id,
                                      const std::string &moduleId,
                                      int position) {
  const auto before = miniModules(id);
  auto after = before;
  const auto at = std::find_if(after.begin(), after.end(),
                               [&](const auto &m) { return m.id == moduleId; });
  if (at == after.end() || cloudProjectBound() || !sharedEditingAllowed() ||
      !sharedGestureAllowed("plugin:" + moduleId))
    return false;
  position = std::clamp(position, 0, int(after.size()) - 1);
  if (position == std::distance(after.begin(), at))
    return false;
  auto module = *at;
  after.erase(at);
  after.insert(after.begin() + position, module);
  applyMiniModules(id, after);
  m_undo.push(
      "Move mini module", [this, id, before] { applyMiniModules(id, before); },
      [this, id, after] { applyMiniModules(id, after); });
  return true;
}
bool EngineController::replaceMiniModule(
    const std::string &id, const std::string &moduleId,
    const plugins::mini::MiniModuleDefinition &d) {
  if (!plugins::mini::validate(d).empty() || cloudProjectBound() ||
      !sharedEditingAllowed() || !sharedGestureAllowed("plugin:" + moduleId))
    return false;
  const auto before = miniModules(id);
  auto after = before;
  auto at = std::find_if(after.begin(), after.end(),
                         [&](const auto &m) { return m.id == moduleId; });
  if (at == after.end())
    return false;
  *at = makeMiniModule(d);
  at->id = moduleId;
  applyMiniModules(id, after);
  m_undo.push(
      "Replace mini module",
      [this, id, before] { applyMiniModules(id, before); },
      [this, id, after] { applyMiniModules(id, after); });
  return true;
}
bool EngineController::updateMiniModule(const std::string &channel,
                                        const InsertModel &module,
                                        const std::string &label,
                                        bool affectsAudio) {
  if (!module.miniModule ||
      !plugins::mini::validate(*module.miniModule, module.miniModuleMode)
           .empty() ||
      cloudProjectBound() || !sharedEditingAllowed() ||
      !sharedGestureAllowed("plugin:" + module.id))
    return false;
  const auto before = miniModules(channel);
  auto after = before;
  const auto at = std::find_if(after.begin(), after.end(), [&](const auto &m) {
    return m.id == module.id;
  });
  if (at == after.end())
    return false;
  *at = module;
  applyMiniModules(channel, after, affectsAudio);
  m_undo.push(
      label,
      [this, channel, before, affectsAudio] {
        applyMiniModules(channel, before, affectsAudio);
      },
      [this, channel, after, affectsAudio] {
        applyMiniModules(channel, after, affectsAudio);
      });
  return true;
}
bool EngineController::setMiniModulePostFx(const std::string &channel,
                                           const std::string &id, bool postFx) {
  const auto *model = insertModel(channel, id);
  if (!model || !model->miniModule || model->miniModulePostFx == postFx)
    return false;
  auto copy = *model;
  copy.miniModulePostFx = postFx;
  return updateMiniModule(channel, copy, "Change mini module position");
}
bool EngineController::setMiniModuleMode(const std::string &channel,
                                         const std::string &id,
                                         const std::string &mode) {
  const auto *model = insertModel(channel, id);
  if (!model || !model->miniModule || model->miniModuleMode == mode)
    return false;
  auto copy = *model;
  copy.miniModuleMode = mode;
  return updateMiniModule(channel, copy, "Change mini module mode");
}
bool EngineController::setMiniModuleAppearance(
    const std::string &channel, const std::string &id,
    const plugins::mini::Appearance &appearance) {
  const auto *model = insertModel(channel, id);
  if (!model || !model->miniModule ||
      model->miniModule->appearance == appearance)
    return false;
  auto copy = *model;
  copy.miniModule->appearance = appearance;
  copy.miniModule->version = std::max(2u, copy.miniModule->version);
  return updateMiniModule(channel, copy, "Change mini module appearance",
                          false);
}
} // namespace daw
