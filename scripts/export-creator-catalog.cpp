// Website metadata snapshot. Link against daw_pluginhost from a native build.
// Prose lives in web/lib/creator; numeric contracts come from the editor itself.
#include "Internal/MiniNodeRegistry.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

int main() {
  using namespace daw::plugins::mini;
  using nlohmann::json;
  auto nodes = json::array();
  for (const auto &registered : nodeRegistry()) {
    auto model = makeNode(registered.id, "example");
    auto description = describeNode(model);
    if (description.id.empty()) description = registered;
    const auto ports = [](const auto &list) {
      auto result = json::array();
      for (const auto &p : list)
        result.push_back({{"id", p.id}, {"name", p.name}, {"type", portTypeId(p.type)},
          {"required", p.required}, {"parameter", p.parameter},
          {"signature", p.signature}, {"capacity", p.capacity}});
      return result;
    };
    auto parameters = json::array();
    for (const auto &p : description.parameters)
      parameters.push_back({{"id", p.id}, {"name", p.name}, {"unit", p.unit},
        {"minimum", p.minimum}, {"maximum", p.maximum}, {"initial", p.initial},
        {"logarithmic", p.logarithmic}, {"modulatable", p.modulatable}, {"choices", p.choices}});
    nodes.push_back({{"id", description.id}, {"name", description.name},
      {"category", description.category}, {"version", description.version},
      {"inputs", ports(description.inputs)}, {"outputs", ports(description.outputs)},
      {"parameters", parameters}});
  }
  std::cout << nodes.dump(2) << '\n';
}
