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

/// Optional target of a replacement picker. Empty targets are add-only.
/// IDs are resolved at activation time, never pointers into a mutable chain.
struct PluginPickerTarget {
    QString channelId;
    QString slotId;
    std::function<void()> onChanged;
};

/// Shared with the context-panel finder; call only after a successful load.
void rememberRecentPlugin(const daw::plugins::PluginDescriptor& descriptor);

/// Prepare immutable grouping/search data after a scan, while the startup or
/// scanner progress UI is already active. Opening a picker then only creates
/// its small first-level QAction set.
void preparePluginPickerMenus(daw::EngineController* controller);

bool checkPluginPickerForTest(QString* error = nullptr,
                              const QString& screenshotPath = {});

/// A compact menu of scanned plugins, grouped by category/vendor, filtered to instruments or to
/// effects. Shared by the insert slots and the instrument slot so the two can
/// never present different lists. This eager form is for callers which are
/// already responding to a click and will show the returned menu immediately.
QMenu* buildPluginMenu(QWidget* parent, daw::EngineController* controller,
                       bool instruments,
                       std::function<void(const daw::plugins::PluginDescriptor&)> onPick,
                       PluginPickerTarget target = {});

/// A lightweight menu suitable for QToolButton::setMenu(). Its QAction tree is
/// populated only while the popup is opening and discarded after it closes.
/// That keeps a strip full of empty slots cheap, while still reading the latest
/// scan results every time the user opens one of them.
QMenu* buildLazyPluginMenu(
    QWidget* parent, daw::EngineController* controller, bool instruments,
    std::function<void(const daw::plugins::PluginDescriptor&)> onPick);

} // namespace ui
