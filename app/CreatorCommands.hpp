#pragma once
#include "CreatorProject.hpp"
#include <nlohmann/json.hpp>
namespace ui {
std::string connectionError(const daw::plugins::mini::MiniModuleDefinition &,
                            const daw::plugins::mini::Connection &);
QPointF creatorFreePosition(const CreatorProject &, const QString &id);
/// Operates on a private copy. The caller commits exactly once after success.
bool creatorApplyBatch(CreatorProject &, const nlohmann::json &,
                       QString &error);
bool creatorApplyFunction(CreatorProject &, const QString &node,
                          const QString &operation, const nlohmann::json &reply,
                          QString &error);
nlohmann::json
creatorNodeDescription(const daw::plugins::mini::NodeDefinition &,
                       const daw::plugins::mini::MiniModuleDefinition &);
} // namespace ui
