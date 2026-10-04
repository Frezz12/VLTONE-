#include "Compiler.hpp"
#include "Creator/CompilerProcess.hpp"
#include <iostream>

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  using namespace creator;
  if (argc != 3)
    return 2;
  const auto path = [](const auto *argument) {
#ifdef _WIN32
    return std::filesystem::path(argument);
#else
    return fromUtf8(argument);
#endif
  };
  json reply;
  try {
    auto request =
        json::parse(readCreatorText(path(argv[1]), 32 * 1024 * 1024));
    const auto tools = fromUtf8(request.at("tools").get<std::string>());
    const auto work = path(argv[1]).parent_path();
    const auto action = request.at("action").get<std::string>();
    if (action == "analyze") {
      auto result = analyze(functionFromJson(request.at("function")), tools);
      reply = {{"ok", result.error.empty()},
               {"error", result.error},
               {"function", functionToJson(result.function)},
               {"diagnostics", result.diagnostics},
               {"details", result.details}};
    } else if (action == "extract")
      reply = extractFunction(request, tools);
    else if (action == "bind")
      reply = bindFunction(request, tools);
    else if (action == "compile")
      reply = compile(request, tools, work);
    else if (action == "prepare")
      reply = prepare(request, tools, work);
    else
      throw std::runtime_error("Unknown compiler operation");
  } catch (const std::exception &e) {
    reply = {{"ok", false}, {"error", e.what()}};
  }
  return writeCreatorText(path(argv[2]), reply.dump()) ? 0 : 3;
}
