#include "EngineController.hpp"

#include "MiniModuleUpdate.hpp"
#include "model/ChannelColor.hpp"
#include "model/MiniModules.hpp"
#include "serialization/InsertJson.hpp"
#include <nlohmann/json.hpp>
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
std::shared_ptr<MiniModuleUpdate> EngineController::planMiniModuleUpdate(
    const plugins::mini::MiniModuleDefinition &definition) const {
  auto result = std::make_shared<MiniModuleUpdate>();
  result->definition = definition;
  result->projectId = m_project.miniModuleProjectId;
  result->info = m_runtime->preparation();
  result->compiler = m_runtime->miniModuleCompiler();
  const auto collect = [&](const std::string &channel, const auto &modules,
                           unsigned channels) {
    for (const auto &slot : modules)
      if (slot.miniModule && slot.miniModule->id == definition.id) {
        MiniModuleUpdate::Target target;
        target.channel = channel;
        target.instance = insertIdentity(channel, slot.id).instance;
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
  if (m_project.miniModuleProjectId != update.projectId || m_runtime->preparation() != update.info)
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
        a.channels != b.channels || a.instance != b.instance ||
        (update.exactRestore && serialization::insertToJson(a.before) != serialization::insertToJson(b.before)))
      return false;
  }
  return true;
}
void EngineController::fadeMiniModuleUpdate(const MiniModuleUpdate &update) {
  if (cloudProjectBound() || !sharedEditingAllowed()) return;
  m_runtime->fadeMiniModulePreparation(update.preparationId, false);
}
void EngineController::cancelMiniModuleUpdateFade(const MiniModuleUpdate &update) {
  m_runtime->fadeMiniModulePreparation(update.preparationId, true);
}
bool EngineController::miniModuleUpdateFaded(const MiniModuleUpdate &update) const {
  return !isPlaying() || m_runtime->miniModulePreparationFaded(update.preparationId);
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
        if (!update->exactRestore) {
          slot.bypassed = bypass;
          slot.miniModulePostFx = post;
        }
        if (!update->exactRestore) for (auto &p : slot.parameters)
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
  if (!audioChannels.empty() && !m_runtime->stageMiniModulePreparation(update->preparationId)) {
    error = "Prepared mini-module generation is no longer available";
    return false;
  }
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
  m_runtime->clearMiniModulePreparation();
  if (!published) {
    (void)apply(before);
    error = published.message();
    return false;
  }
  for (auto& target : update->targets)
    target.instance = insertIdentity(target.channel, target.before.id).instance;
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
