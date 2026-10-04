#pragma once
#include "model/Document.hpp"

namespace daw {
InsertModel makeMiniModule(const plugins::mini::MiniModuleDefinition &);
InsertModel migrateColorToMiniModule(const InsertModel &);
void migrateMiniModules(ProjectModel &, bool recoverVirtualColor = true);
std::vector<InsertModel> *miniModulesFor(ProjectModel &,
                                         const std::string &channelId);
const std::vector<InsertModel> *miniModulesFor(const ProjectModel &,
                                               const std::string &channelId);
bool validMiniModule(const InsertModel &);
} // namespace daw
