#pragma once
#include <functional>
#include <nlohmann/json.hpp>
class QMenu;
namespace daw {
class EngineController;
}
namespace ui::scripts {
// Optional shell integration: leaf widgets and their standalone tests do not
// depend on the extension runtime, Creator, or a particular main window.
inline std::function<void(QMenu *, daw::EngineController *, nlohmann::json)> contextAppender;
inline void appendContextMenu(QMenu *menu, daw::EngineController *controller,
                              nlohmann::json context) {
    if (contextAppender)
        contextAppender(menu, controller, std::move(context));
}
} // namespace ui::scripts
