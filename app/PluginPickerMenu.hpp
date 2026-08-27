#pragma once

#include <QString>
#include <functional>

class QMenu;
class QWidget;

namespace daw {
class EngineController;
namespace plugins { struct PluginDescriptor; }
} // namespace daw

namespace ui {

/// A menu of scanned plugins, grouped by vendor, filtered to instruments or to
/// effects. Shared by the insert slots and the instrument slot so the two can
/// never present different lists. This eager form is for callers which are
/// already responding to a click and will show the returned menu immediately.
QMenu* buildPluginMenu(QWidget* parent, daw::EngineController* controller,
                       bool instruments,
                       std::function<void(const daw::plugins::PluginDescriptor&)> onPick);

/// A lightweight menu suitable for QToolButton::setMenu(). Its QAction tree is
/// populated only while the popup is opening and discarded after it closes.
/// That keeps a strip full of empty slots cheap, while still reading the latest
/// scan results every time the user opens one of them.
QMenu* buildLazyPluginMenu(
    QWidget* parent, daw::EngineController* controller, bool instruments,
    std::function<void(const daw::plugins::PluginDescriptor&)> onPick);

} // namespace ui
