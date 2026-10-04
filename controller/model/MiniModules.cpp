#include "MiniModules.hpp"
#include "ChannelColor.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
InsertModel makeMiniModule(const plugins::mini::MiniModuleDefinition &d) {
  InsertModel m;
  m.id = newUuid();
  m.name = d.name;
  m.format = PluginFormat::Internal;
  m.uid = m.path = std::string(plugins::mini::kUid);
  m.vendor = "VLTONE";
  m.pluginVersion = "1.0";
  m.stateSchemaVersion = 1;
  m.miniModule = d;
  m.profileSeed = defaultChannelColor(m.id).profileSeed;
  for (const auto &c : d.controls)
    m.parameters.push_back({c.id, c.initial});
  return m;
}
InsertModel migrateColorToMiniModule(const InsertModel &old) {
  auto m = makeMiniModule(plugins::mini::builtin("color"));
  m.id = old.id;
  m.name = "Color";
  m.bypassed = old.bypassed;
  m.profileSeed = old.profileSeed;
  m.parameters = old.parameters;
  return m;
}
std::vector<InsertModel> *miniModulesFor(ProjectModel &p,
                                         const std::string &id) {
  if (id.empty() || id == "master")
    return &p.masterMiniModules;
  auto *t = p.findTrack(id);
  return t && carriesAudio(*t) ? &t->miniModules : nullptr;
}
const std::vector<InsertModel> *miniModulesFor(const ProjectModel &p,
                                               const std::string &id) {
  return miniModulesFor(const_cast<ProjectModel &>(p), id);
}
bool validMiniModule(const InsertModel &m) {
  if (m.uid != plugins::mini::kUid || m.format != PluginFormat::Internal ||
      !m.miniModule || !plugins::mini::validate(*m.miniModule,m.miniModuleMode).empty() ||
      m.mix != 1 || m.channelMode != PluginChannelMode::Auto ||
      !m.sidechainTrackIds.empty() || !m.assetBindings.empty() ||
      !m.rightParameters.empty() || m.parameters.size() > 2)
    return false;
  std::vector<std::string> ids;
  for (const auto &p : m.parameters) {
    const auto &controls = m.miniModule->controls;
    auto c = std::find_if(controls.begin(), controls.end(),
                          [&](const auto &c) { return c.id == p.id; });
    if (c == controls.end() || !std::isfinite(p.value) ||
        p.value < c->minimum || p.value > c->maximum ||
        std::find(ids.begin(), ids.end(), p.id) != ids.end())
      return false;
    ids.push_back(p.id);
  }
  return true;
}
void migrateMiniModules(ProjectModel &p, bool recoverVirtualColor) {
  const auto migrate = [&](std::vector<TrackModel> &tracks) {
    for (auto &t : tracks) {
      if (t.channelColor && t.miniModules.empty()) {
        t.miniModules.push_back(migrateColorToMiniModule(*t.channelColor));
        t.freeze = {};
      }
      t.channelColor.reset();
    }
  };
  // Automation may address the old virtual, never explicitly saved slot.
  for (auto &t : p.tracks)
    if (recoverVirtualColor && supportsChannelColor(t.kind) &&
        !t.channelColor && t.miniModules.empty()) {
      const auto id = channelColorSlotId(t.id);
      bool referenced = false;
      for (const auto &owner : p.tracks)
        for (const auto &clip : owner.clips)
          if (clip.kind == ClipKind::Automation &&
              clip.automation.target.slotId == id)
            referenced = true;
      if (referenced)
        t.channelColor = defaultChannelColor(t.id);
    }
  migrate(p.tracks);
  for (auto &entry : p.clipLibrary)
    migrate(entry.tracks);
}
} // namespace daw
