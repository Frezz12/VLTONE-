#include "PluginPickerMenu.hpp"

#include "EngineController.hpp"
#include "PluginFormatPreference.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTimer>
#include <QWidgetAction>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace ui {

namespace {

/// How many matches a search shows before it stops. Past this the list is not
/// an answer any more, and the query is what wants narrowing.
constexpr int kMaxMatches = 40;
constexpr auto kSearchFilterObjectName = "PluginPickerSearchFilter";

[[maybe_unused]] const char* const kTranslatableCategoryNames[] = {
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Reverb"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Delay"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "EQ"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Filter"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Dynamics"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Distortion"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Modulation"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Pitch"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Analyzer"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Mastering"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Restoration"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Spatial"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Generator"),
    QT_TRANSLATE_NOOP("PluginPickerMenu", "Utility"),
};

QString formatLabel(daw::plugins::Format format) {
    switch (format) {
        case daw::plugins::Format::Vst3: return QStringLiteral("VST3");
        case daw::plugins::Format::Vst: return QStringLiteral("VST");
        case daw::plugins::Format::AudioUnit: return QStringLiteral("AU");
        case daw::plugins::Format::Clap: return QStringLiteral("CLAP");
        case daw::plugins::Format::Internal: return QObject::tr("Built-in");
        case daw::plugins::Format::Unknown: break;
    }
    return {};
}

/// A coarse, human bucket for a plugin.
///
/// The formats disagree about what a category is: VST3 gives a subcategory
/// list ("Fx|Reverb|Stereo"), CLAP gives features ("reverb"), and Audio Units
/// give "Audio Effect" and nothing else — which is why the plugin's own
/// metadata is read first and its *name* second. Guessing from a name is a
/// heuristic, but "Studio Reverb" in a Reverb group beats every Audio Unit
/// piled into Other.
QString categoryOf(const daw::plugins::PluginDescriptor& descriptor) {
    if (descriptor.isInstrument) return QObject::tr("Instruments");

    struct Bucket { const char* key; const char* name; bool nameToo; };
    // Order matters: the first hit wins, so the specific keys come first.
    static const Bucket kBuckets[] = {
        {"reverb", "Reverb", true},        {"room", "Reverb", true},
        {"hall", "Reverb", false},         {"plate", "Reverb", false},
        {"delay", "Delay", true},          {"echo", "Delay", true},
        {"equalizer", "EQ", true},         {"eq", "EQ", false},
        {"filter", "Filter", true},
        {"compressor", "Dynamics", true},  {"limiter", "Dynamics", true},
        {"dynamics", "Dynamics", true},    {"gate", "Dynamics", true},
        {"expander", "Dynamics", true},    {"deesser", "Dynamics", true},
        {"distortion", "Distortion", true},{"saturat", "Distortion", true},
        {"overdrive", "Distortion", true}, {"fuzz", "Distortion", true},
        {"guitar", "Distortion", false},
        {"chorus", "Modulation", true},    {"flanger", "Modulation", true},
        {"phaser", "Modulation", true},    {"tremolo", "Modulation", true},
        {"modulation", "Modulation", true},{"vibrato", "Modulation", true},
        {"pitch", "Pitch", true},          {"harmon", "Pitch", true},
        {"vocoder", "Pitch", true},        {"tuner", "Analyzer", true},
        {"analy", "Analyzer", true},       {"meter", "Analyzer", true},
        {"spectrum", "Analyzer", true},    {"scope", "Analyzer", false},
        {"mastering", "Mastering", true},
        {"restoration", "Restoration", true}, {"denois", "Restoration", true},
        {"declick", "Restoration", true},  {"dereverb", "Restoration", true},
        {"spatial", "Spatial", true},      {"imager", "Spatial", true},
        {"surround", "Spatial", true},     {"panner", "Spatial", true},
        {"generator", "Generator", false}, {"synth", "Generator", false},
        {"utility", "Utility", true},      {"tools", "Utility", false},
    };

    const QString category =
        QString::fromStdString(descriptor.category).toLower();
    for (const Bucket& bucket : kBuckets) {
        if (category.contains(QLatin1String(bucket.key)))
            return QCoreApplication::translate("PluginPickerMenu", bucket.name);
    }
    const QString name = QString::fromStdString(descriptor.name).toLower();
    for (const Bucket& bucket : kBuckets) {
        if (bucket.nameToo && name.contains(QLatin1String(bucket.key)))
            return QCoreApplication::translate("PluginPickerMenu", bucket.name);
    }
    return QObject::tr("Other");
}

QString vendorOf(const daw::plugins::PluginDescriptor& descriptor) {
    return descriptor.vendor.empty() ? QObject::tr("Unknown")
                                     : QString::fromStdString(descriptor.vendor);
}

/// What a row says: the plugin, and the format it comes in. The tab is not
/// decoration — QMenu draws everything past it in the shortcut column, right
/// aligned and dimmed, which is exactly where a format badge belongs.
QString rowText(const daw::plugins::PluginDescriptor& descriptor) {
    return QString::fromStdString(descriptor.name) + QLatin1Char('\t') +
           formatLabel(descriptor.format);
}

/// Feeds the menu's keystrokes to its search field.
///
/// A QMenu grabs the keyboard while it is open and answers the keys itself, so
/// a QLineEdit inside it never hears a letter. Anything that is not a menu
/// gesture — the arrows, Enter, Escape, Tab — is handed to the field instead.
class MenuSearchFilter : public QObject {
public:
    MenuSearchFilter(QMenu* menu, QLineEdit* edit)
        : QObject(menu), m_edit(edit) {}

    void watch(QMenu* menu) {
        if (menu) menu->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        const bool shortcutOverride = event->type() == QEvent::ShortcutOverride;
        if (!shortcutOverride && event->type() != QEvent::KeyPress) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Escape:
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Left:
            case Qt::Key_Right:
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                return false;   // the menu's own navigation
            default:
                break;
        }
        const bool find = key->matches(QKeySequence::Find);
        const bool editingShortcut =
            key->matches(QKeySequence::Copy) ||
            key->matches(QKeySequence::Cut) ||
            key->matches(QKeySequence::Paste) ||
            key->matches(QKeySequence::SelectAll) ||
            key->matches(QKeySequence::Undo) ||
            key->matches(QKeySequence::Redo);
        const bool typing = key->key() == Qt::Key_Backspace ||
                            key->key() == Qt::Key_Delete ||
                            (!key->text().isEmpty() && key->text().at(0).isPrint());
        const bool modifiedCommand =
            key->modifiers().testAnyFlags(Qt::ControlModifier |
                                          Qt::AltModifier |
                                          Qt::MetaModifier);

        // Claim printable keys before QAction's shortcut machinery sees them.
        // Otherwise a single-key DAW shortcut can run while this popup owns
        // the keyboard, close the picker, and make the search look as though
        // it reset. The following KeyPress is forwarded to the field below.
        if (shortcutOverride) {
            if (!typing && !editingShortcut && !find && !modifiedCommand)
                return false;
            key->accept();
            return true;
        }

        if (find) {
            m_edit->setFocus(Qt::ShortcutFocusReason);
            m_edit->selectAll();
            key->accept();
            return true;
        }
        if (!typing && !editingShortcut) {
            // A modified DAW command was already claimed above. Keep it from
            // mutating the project behind a popup which visibly owns input.
            return modifiedCommand;
        }

        // Hovering a vendor/category submenu makes that QMenu the key target.
        // Put the caret back in the root menu's field and preserve the actual
        // text from the current keyboard layout (including Cyrillic input).
        m_edit->setFocus(Qt::ShortcutFocusReason);
        QApplication::sendEvent(m_edit, key);
        key->accept();
        return true;
    }

private:
    QLineEdit* m_edit = nullptr;
};

void applyDarkPluginMenuStyle(QMenu* menu) {
    if (!menu) return;
    const Theme& theme = th();
    const QColor hover = mixColors(theme.well(), theme.textPrimary, 0.14);
    const QColor selected = mixColors(theme.well(), theme.accent, 0.20);
    menu->setObjectName(QStringLiteral("PluginPickerMenu"));
    menu->setStyleSheet(QString(R"(
QMenu { background: %1; color: %2; border: 1px solid %3;
        border-radius: 10px; padding: 5px; }
QMenu::item { min-height: 18px; padding: 4px 22px 4px 9px;
              border-radius: 5px; background: transparent; }
QMenu::item:selected { background: %4; color: %2; }
QMenu::item:disabled { color: %5; }
QMenu::separator { height: 1px; background: %3; margin: 4px 6px; }
QMenu::scroller { height: 14px; background: %1; }
QLineEdit { color: %2; background: %6; border: 1px solid %3;
            border-radius: 6px; padding: 4px 8px; }
QLineEdit:hover { background: %7; }
QLineEdit:focus { border-color: %8; }
)")
        .arg(theme.well().name(), theme.textPrimary.name(),
             theme.separator().name(), selected.name(),
             theme.textSecondary.name(), theme.surface.name(), hover.name(),
             theme.accent.name()));
}

using PickCallback =
    std::function<void(const daw::plugins::PluginDescriptor&)>;
using SharedPickCallback = std::shared_ptr<PickCallback>;

void scheduleSearchFocus(QMenu* menu, QLineEdit* edit, QAction* editAction) {
    // Run after the popup has taken its grab. The visibility guard also makes
    // this harmless when the menu was closed again in the same event turn.
    QTimer::singleShot(0, edit, [menu, edit, editAction] {
        if (!menu->isVisible()) return;
        menu->setActiveAction(editAction);
        edit->setFocus(Qt::PopupFocusReason);
    });
}

void clearPluginMenu(QMenu* menu) {
    if (!menu) return;

    // MenuSearchFilter is parented to the root rather than represented by an
    // action, so QMenu::clear() cannot remove it. Delete it first to avoid
    // stacking event filters every time a lazy picker is reopened.
    const QObjectList children = menu->children();
    for (QObject* child : children) {
        if (child->objectName() == QLatin1String(kSearchFilterObjectName))
            delete child;
    }
    menu->clear();
}

void populatePluginMenu(QMenu* menu, QWidget* callbackContext,
                        daw::EngineController* controller, bool instruments,
                        const SharedPickCallback& onPick, bool openingNow) {
    if (!menu || !controller) return;

    std::vector<daw::plugins::PluginDescriptor> found =
        instruments ? controller->pluginManager().instruments()
                    : controller->pluginManager().effects();
    found = daw::preferredPluginVariants(std::move(found),
                                         ui::preferredPluginFormat());

    if (found.empty()) {
        // Say which of the two situations this is. "No plugins" while a scan is
        // running looks like a bug; saying so does not.
        QAction* empty = menu->addAction(
            controller->pluginManager().isScanning()
                ? QObject::tr("Scanning for plugins…")
                : QObject::tr("No plugins found — scan in Settings ▸ Plugin Manager"));
        empty->setEnabled(false);
        return;
    }

    // ── The search field, at the top ──
    auto* edit = new QLineEdit(menu);
    edit->setObjectName(QStringLiteral("PluginPickerSearch"));
    edit->setPlaceholderText(QObject::tr("Search plugins…"));
    edit->setAccessibleName(QObject::tr("Plugin search"));
    edit->setClearButtonEnabled(true);
    edit->setMinimumWidth(230);
    auto* editAction = new QWidgetAction(menu);
    editAction->setDefaultWidget(edit);
    menu->addAction(editAction);
    menu->addSeparator();
    auto* searchFilter = new MenuSearchFilter(menu, edit);
    searchFilter->setObjectName(QLatin1String(kSearchFilterObjectName));
    searchFilter->watch(menu);
    if (openingNow) {
        scheduleSearchFocus(menu, edit, editAction);
    } else {
        QObject::connect(menu, &QMenu::aboutToShow, edit,
                         [menu, edit, editAction] {
                             scheduleSearchFocus(menu, edit, editAction);
                         });
    }

    // ── Grouped, by whatever the user asked for ──
    const bool byCategory =
        QSettings().value(QStringLiteral("plugins/menuGrouping"),
                          QStringLiteral("vendor")).toString() ==
        QLatin1String("category");

    std::map<QString, std::vector<daw::plugins::PluginDescriptor>> groups;
    for (const daw::plugins::PluginDescriptor& descriptor : found) {
        groups[byCategory ? categoryOf(descriptor) : vendorOf(descriptor)]
            .push_back(descriptor);
    }

    // The group submenus, and — hidden until something is typed — one flat row
    // per plugin. Two lists rather than a rebuild: a menu cannot be repopulated
    // while it is open without closing the popup the user is typing into.
    std::vector<QAction*> groupActions;
    QWidget* const context = callbackContext ? callbackContext : menu;
    for (auto& [name, entries] : groups) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        QMenu* submenu = menu->addMenu(
            QObject::tr("%1  (%2)").arg(name).arg(entries.size()));
        applyDarkPluginMenuStyle(submenu);
        searchFilter->watch(submenu);
        groupActions.push_back(submenu->menuAction());
        for (const daw::plugins::PluginDescriptor& descriptor : entries) {
            QAction* action = submenu->addAction(rowText(descriptor));
            QObject::connect(action, &QAction::triggered, context,
                             [onPick, descriptor] {
                                 if (*onPick) (*onPick)(descriptor);
                             });
        }
    }

    std::vector<daw::plugins::PluginDescriptor> flat(found.begin(), found.end());
    std::sort(flat.begin(), flat.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    std::vector<QAction*> matchActions;
    matchActions.reserve(flat.size());
    for (const daw::plugins::PluginDescriptor& descriptor : flat) {
        QAction* action = menu->addAction(rowText(descriptor));
        action->setVisible(false);
        QObject::connect(action, &QAction::triggered, context,
                         [onPick, descriptor] {
                             if (*onPick) (*onPick)(descriptor);
                         });
        matchActions.push_back(action);
    }
    QAction* noMatch = menu->addAction(QObject::tr("Nothing matches"));
    noMatch->setEnabled(false);
    noMatch->setVisible(false);

    QObject::connect(edit, &QLineEdit::textChanged, menu,
                     [groupActions, matchActions, noMatch, flat](const QString& query) {
        const QString needle = query.trimmed();
        const bool searching = !needle.isEmpty();
        for (QAction* group : groupActions) group->setVisible(!searching);

        int shown = 0;
        for (std::size_t i = 0; i < matchActions.size(); ++i) {
            bool hit = false;
            if (searching && shown < kMaxMatches) {
                // Name or vendor: "waves" and "reverb" are both things a person
                // types into this box, and only one of them is the name.
                const QString name = QString::fromStdString(flat[i].name);
                const QString vendor = QString::fromStdString(flat[i].vendor);
                hit = name.contains(needle, Qt::CaseInsensitive) ||
                      vendor.contains(needle, Qt::CaseInsensitive);
                if (hit) ++shown;
            }
            matchActions[i]->setVisible(hit);
        }
        noMatch->setVisible(searching && shown == 0);
    });
}

} // namespace

QMenu* buildPluginMenu(QWidget* parent, daw::EngineController* controller,
                       bool instruments,
                       std::function<void(const daw::plugins::PluginDescriptor&)> onPick) {
    auto* menu = new QMenu(parent);
    applyDarkPluginMenuStyle(menu);
    populatePluginMenu(
        menu, parent, controller, instruments,
        std::make_shared<PickCallback>(std::move(onPick)), /*openingNow=*/false);
    return menu;
}

QMenu* buildLazyPluginMenu(
    QWidget* parent, daw::EngineController* controller, bool instruments,
    std::function<void(const daw::plugins::PluginDescriptor&)> onPick) {
    auto* menu = new QMenu(parent);
    applyDarkPluginMenuStyle(menu);
    menu->setProperty("pluginPickerLazy", true);
    menu->setProperty("pluginPickerInstruments", instruments);

    const auto callback =
        std::make_shared<PickCallback>(std::move(onPick));
    QObject::connect(menu, &QMenu::aboutToShow, menu,
                     [menu, parent, controller, instruments, callback] {
        // Rebuild from the current scan and grouping preference, not from the
        // state that existed when the channel strip was constructed.
        clearPluginMenu(menu);
        populatePluginMenu(menu, parent, controller, instruments, callback,
                           /*openingNow=*/true);
    });
    QObject::connect(menu, &QMenu::aboutToHide, menu, [menu] {
        // Defer destruction until QAction::triggered and QMenu's own hide path
        // have unwound. Reopening synchronously wins over this cleanup.
        QTimer::singleShot(0, menu, [menu] {
            if (!menu->isVisible()) clearPluginMenu(menu);
        });
    });
    return menu;
}

} // namespace ui
