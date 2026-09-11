#include "PluginPickerMenu.hpp"

#include "EngineController.hpp"
#include "PluginFormatPreference.hpp"
#include "Theme.hpp"
#include "Icons.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QFontMetrics>
#include <QHash>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QActionGroup>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleOption>
#include <QTimer>
#include <QWheelEvent>
#include <QWidgetAction>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace ui {

namespace {

/// How many matches a search shows before it stops. Past this the list is not
/// an answer any more, and the query is what wants narrowing.
constexpr int kMaxMatches = 40;
constexpr auto kSearchFilterObjectName = "PluginPickerSearchFilter";

/// Native QMenu otherwise turns a tall plugin group into several columns on
/// platforms whose style disables menu scrolling. A plugin catalogue is much
/// easier to scan as Logic's adjacent one-column lists, with the standard menu
/// scrollers keeping each column inside the screen.
class ScrollablePluginMenuStyle final : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption* option,
                  const QWidget* widget,
                  QStyleHintReturn* returnData = nullptr) const override {
        if (hint == QStyle::SH_Menu_Scrollable) return 1;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        if (metric == QStyle::PM_MenuScrollerHeight) return 10;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widget = nullptr) const override {
        const bool verticalArrow = element == QStyle::PE_IndicatorArrowUp ||
                                   element == QStyle::PE_IndicatorArrowDown;
        const bool submenuArrow = element == QStyle::PE_IndicatorArrowLeft ||
                                  element == QStyle::PE_IndicatorArrowRight;
        if (option && (verticalArrow || submenuArrow)) {
            QStyleOption compact(*option);
            compact.rect = QStyle::alignedRect(
                Qt::LeftToRight, Qt::AlignCenter,
                verticalArrow ? QSize(6, 4) : QSize(4, 6), option->rect);
            QProxyStyle::drawPrimitive(element, &compact, painter, widget);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

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

/// Feeds the menu's keystrokes to its search field.
///
/// A QMenu grabs the keyboard while it is open and answers the keys itself, so
/// a QLineEdit inside it never hears a letter. Anything that is not a menu
/// gesture — the arrows, Enter, Escape, Tab — is handed to the field instead.
class MenuSearchFilter : public QObject {
public:
    MenuSearchFilter(QMenu* menu, QLineEdit* edit)
        : QObject(menu), m_edit(edit) {
        if (m_edit) m_edit->installEventFilter(this);
    }

    void watch(QMenu* menu) {
        if (menu) menu->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Wheel) {
            auto* menu = qobject_cast<QMenu*>(watched);
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (!menu) return false;
            const int delta = !wheel->pixelDelta().isNull()
                                  ? wheel->pixelDelta().y()
                                  : wheel->angleDelta().y();
            if (!delta) return false;

            std::vector<QAction*> rows;
            rows.reserve(std::size_t(menu->actions().size()));
            for (QAction* action : menu->actions()) {
                if (!action->isVisible() || !action->isEnabled() ||
                    action->isSeparator() ||
                    qobject_cast<QWidgetAction*>(action)) {
                    continue;
                }
                rows.push_back(action);
            }
            if (rows.empty()) return false;

            const int divisor = wheel->pixelDelta().isNull() ? 120 : 24;
            const int steps = std::max(1, std::abs(delta) / divisor);
            // Drive QMenu through its keyboard navigation path. Besides moving
            // the active row, that path updates its private scroll offset and
            // therefore works on macOS styles where wheelEvent itself does not.
            const int key = delta < 0 ? Qt::Key_Down : Qt::Key_Up;
            for (int step = 0; step < steps; ++step) {
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
                QApplication::sendEvent(menu, &press);
            }
            wheel->accept();
            return true;
        }

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

        // When the editor itself owns the event, let QLineEdit insert the
        // printable text normally. Its ShortcutOverride above has already
        // prevented application-wide DAW commands such as bare R from firing.
        if (watched == m_edit) {
            if (find) {
                m_edit->selectAll();
                key->accept();
                return true;
            }
            return false;
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
    if (!menu->property("pluginPickerScrollable").toBool()) {
        auto* style = new ScrollablePluginMenuStyle;
        style->setParent(menu);
        menu->setStyle(style);
        menu->setProperty("pluginPickerScrollable", true);
    }
    const Theme& theme = th();
    const QColor background(8, 8, 8, 255);
    const QColor ink(238, 238, 238);
    const QColor hover = mixColors(background, ink, 0.08);
    const QColor selected = mixColors(background, ink, 0.16);
    const QColor border = mixColors(background, ink, 0.22);
    menu->setAttribute(Qt::WA_TranslucentBackground, false);
    menu->setWindowFlag(Qt::FramelessWindowHint);
    menu->setWindowOpacity(1.0);
    menu->setMinimumWidth(210);
    menu->setToolTipsVisible(true);
    menu->setObjectName(QStringLiteral("PluginPickerMenu"));
    menu->setStyleSheet(QString(R"(
QMenu { background: %1; color: %2; border: 1px solid %3;
        border-radius: 8px; padding: 4px; font-size: 11px; }
QMenu::item { min-height: 16px; padding: 2px 18px 2px 8px;
              border-radius: 5px; background: transparent; }
QMenu::item:selected { background: %4; color: %2; }
QMenu::item:disabled { color: %5; }
QMenu::separator { height: 1px; background: %3; margin: 3px 6px; }
QMenu::scroller { height: 10px; background: %1; }
QLineEdit { color: %2; background: %6; border: 1px solid %3;
            border-radius: 5px; padding: 3px 7px; font-size: 11px; }
QLineEdit:hover { background: %7; }
QLineEdit:focus { border-color: %8; }
)")
        .arg(background.name(QColor::HexArgb), ink.name(),
             border.name(), selected.name(),
             mixColors(background, ink, 0.55).name(), hover.name(), hover.name(),
             theme.accent.name()));
}

using PickCallback =
    std::function<void(const daw::plugins::PluginDescriptor&)>;
using SharedPickCallback = std::shared_ptr<PickCallback>;

struct SearchablePlugin {
    daw::plugins::PluginDescriptor descriptor;
    QString label;
    QString haystack;
};

struct PreparedPluginCatalogue {
    std::uint64_t managerId = 0;
    std::uint64_t revision = 0;
    daw::plugins::Format preferredFormat = daw::plugins::Format::Unknown;
    bool instruments = false;
    std::vector<daw::plugins::PluginDescriptor> catalogue;
    std::vector<daw::plugins::PluginDescriptor> preferred;
    std::map<QString, std::vector<daw::plugins::PluginDescriptor>> categories;
    std::map<QString, std::vector<daw::plugins::PluginDescriptor>> vendors;
    std::vector<SearchablePlugin> searchable;
    QHash<QString, std::size_t> byUid;
};

std::shared_ptr<const PreparedPluginCatalogue> preparedCatalogue(
    daw::EngineController* controller, bool instruments) {
    const std::uint64_t managerId =
        controller->pluginManager().instanceId();
    const std::uint64_t revision =
        controller->pluginManager().catalogueRevision();
    const daw::plugins::Format preferredFormat = ui::preferredPluginFormat();
    struct CacheEntry {
        std::uint64_t managerId = 0;
        std::uint64_t revision = 0;
        daw::plugins::Format preferredFormat = daw::plugins::Format::Unknown;
        bool instruments = false;
        std::shared_ptr<const PreparedPluginCatalogue> catalogue;
    };
    static std::vector<CacheEntry> cache;
    for (const auto& entry : cache) {
        if (entry.managerId == managerId && entry.revision == revision &&
            entry.preferredFormat == preferredFormat &&
            entry.instruments == instruments) {
            return entry.catalogue;
        }
    }

    std::vector<daw::plugins::PluginDescriptor> catalogue =
        instruments ? controller->pluginManager().instruments()
                    : controller->pluginManager().effects();
    auto prepared = std::make_shared<PreparedPluginCatalogue>();
    prepared->managerId = managerId;
    prepared->revision = revision;
    prepared->preferredFormat = preferredFormat;
    prepared->instruments = instruments;
    prepared->catalogue = std::move(catalogue);
    prepared->preferred = daw::preferredPluginVariants(
        prepared->catalogue, preferredFormat);
    prepared->byUid.reserve(prepared->catalogue.size());
    for (std::size_t index = 0; index < prepared->catalogue.size(); ++index) {
        const auto& descriptor = prepared->catalogue[index];
        prepared->byUid.insert(QString::fromStdString(descriptor.uid), index);
    }
    prepared->searchable.reserve(prepared->preferred.size());
    for (const auto& descriptor : prepared->preferred) {
        prepared->categories[categoryOf(descriptor)].push_back(descriptor);
        prepared->vendors[vendorOf(descriptor)].push_back(descriptor);
        SearchablePlugin item;
        item.descriptor = descriptor;
        item.label = QString::fromStdString(descriptor.name);
        item.haystack = item.label + QLatin1Char('\n') +
                        QString::fromStdString(descriptor.vendor);
        prepared->searchable.push_back(std::move(item));
    }
    const auto byName = [](const auto& a, const auto& b) {
        return QString::compare(QString::fromStdString(a.name),
                                QString::fromStdString(b.name),
                                Qt::CaseInsensitive) < 0;
    };
    for (auto* groups : {&prepared->categories, &prepared->vendors}) {
        for (auto& [name, entries] : *groups) {
            Q_UNUSED(name);
            std::sort(entries.begin(), entries.end(), byName);
        }
    }
    std::sort(prepared->searchable.begin(), prepared->searchable.end(),
              [](const auto& a, const auto& b) {
                  return QString::compare(a.label, b.label,
                                          Qt::CaseInsensitive) < 0;
              });
    cache.push_back(
        {managerId, revision, preferredFormat, instruments, prepared});
    // A rescan or preferred-format change leaves one old immutable snapshot.
    // Bound those generations; open menus retain their own shared copy safely.
    if (cache.size() > 8) cache.erase(cache.begin());
    return prepared;
}

bool sameProduct(const daw::plugins::PluginDescriptor& a,
                 const daw::plugins::PluginDescriptor& b) {
    return daw::samePluginProduct(a, b);
}

void removeTarget(daw::EngineController* controller,
                  const PluginPickerTarget& target) {
    const auto channel = target.channelId.toStdString();
    const auto id = target.slotId.toStdString();
    if (!controller->insertModel(channel, id)) return;
    if (const auto* track = controller->project().findTrack(channel)) {
        if (track->instrument.id == id) {
            controller->setTrackInstrumentPlugin(channel, {});
        } else if (std::any_of(track->samplerFx.inserts.begin(),
                               track->samplerFx.inserts.end(),
                               [&](const auto& slot) { return slot.id == id; })) {
            const auto samplerId = track->instrument.id;
            controller->removeSamplerFxInsert(channel, samplerId, id);
        } else {
            std::string clipId;
            for (const auto& clip : track->clips)
                for (const auto& slot : clip.inserts)
                    if (slot.id == id) clipId = clip.id;
            if (!clipId.empty()) controller->removeClipFxInsert(channel, clipId, id);
            else controller->removeInsert(channel, id);
        }
    } else {
        controller->removeInsert(channel, id);
    }
    if (target.onChanged) target.onChanged();
}

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
    // QMenu::clear removes actions, but submenus are independently parented
    // widgets. Retire those too when a lazy catalogue is closed/rebuilt.
    for (auto* submenu : menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly))
        delete submenu;
    menu->clear();
}

void populatePluginMenu(QMenu* menu, QWidget* callbackContext,
                        daw::EngineController* controller, bool instruments,
                        const SharedPickCallback& onPick, bool openingNow,
                        const PluginPickerTarget& target = {}) {
    if (!menu || !controller) return;

    const auto prepared = preparedCatalogue(controller, instruments);
    const auto& catalogue = prepared->catalogue;
    const auto& found = prepared->preferred;
    QWidget* context = callbackContext;
    // A replacement picker can itself be inside an auto-deleting context
    // menu. Queued mutations must outlive that popup, but not its owning strip.
    while (context && qobject_cast<QMenu*>(context)) context = context->parentWidget();
    QObject* const lifetime = context ? static_cast<QObject*>(context) : qApp;

    // ── The search field, at the top ──
    auto* edit = new QLineEdit(menu);
    edit->setObjectName(QStringLiteral("PluginPickerSearch"));
    edit->setPlaceholderText(QObject::tr("Search"));
    edit->setAccessibleName(QObject::tr("Plugin search"));
    edit->setClearButtonEnabled(true);
    edit->setMinimumWidth(208);
    edit->addAction(icons::icon(icons::Glyph::Search, QColor(220, 220, 220), 16),
                    QLineEdit::LeadingPosition);
    auto* editAction = new QWidgetAction(menu);
    editAction->setDefaultWidget(edit);
    menu->addAction(editAction);
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

    // Actions outside the search results disappear as a unit while typing.
    std::vector<QAction*> groupActions;
    auto pickAction = [lifetime, onPick](QMenu* owner, const QString& label,
                                       const daw::plugins::PluginDescriptor& d) {
        auto* action = owner->addAction(label);
        action->setProperty("pluginUid", QString::fromStdString(d.uid));
        action->setToolTip(QString::fromStdString(d.name + "\n" + d.vendor +
                                                 "\n" + d.version));
        QObject::connect(action, &QAction::triggered, lifetime, [lifetime, onPick, d] {
            // Slot updates may rebuild and delete the menu's anchor. Let Qt
            // finish releasing its popup before changing the document.
            QTimer::singleShot(0, lifetime, [onPick, d] {
                if (*onPick) (*onPick)(d);
            });
        });
        return action;
    };
    if (const auto* slot = controller->insertModel(target.channelId.toStdString(),
                                                   target.slotId.toStdString());
        slot && slot->isLoaded()) {
        daw::plugins::PluginDescriptor current;
        current.uid = slot->uid;
        current.name = slot->name;
        current.vendor = slot->vendor;
        current.isInstrument = instruments;
        current.format = daw::plugins::formatFromString(daw::toString(slot->format));
        auto* currentMenu = menu->addMenu(menu->fontMetrics().elidedText(
            QString::fromStdString(slot->name), Qt::ElideRight, 250).replace('&', QStringLiteral("&&")));
        currentMenu->menuAction()->setToolTip(QString::fromStdString(slot->name));
        currentMenu->menuAction()->setObjectName(QStringLiteral("PluginPickerCurrent"));
        applyDarkPluginMenuStyle(currentMenu);
        searchFilter->watch(currentMenu);
        groupActions.push_back(currentMenu->menuAction());
        auto* modes = new QActionGroup(currentMenu);
        for (auto mode : {daw::PluginChannelMode::Auto, daw::PluginChannelMode::Mono,
                          daw::PluginChannelMode::Stereo, daw::PluginChannelMode::DualMono}) {
            if (instruments && mode == daw::PluginChannelMode::DualMono) continue;
            const char* label = mode == daw::PluginChannelMode::Auto ? QT_TR_NOOP("Auto") :
                                mode == daw::PluginChannelMode::Mono ? QT_TR_NOOP("Mono") :
                                mode == daw::PluginChannelMode::Stereo ? QT_TR_NOOP("Stereo") : QT_TR_NOOP("Dual Mono");
            auto* action = currentMenu->addAction(QObject::tr(label));
            action->setData(int(mode));
            action->setCheckable(true);
            action->setChecked(slot->channelMode == mode);
            modes->addAction(action);
            QObject::connect(action, &QAction::triggered, lifetime,
                             [controller, context, lifetime, target, mode] {
                QTimer::singleShot(0, lifetime, [controller, context, target, mode] {
                    if (controller->setInsertChannelMode(target.channelId.toStdString(),
                                                          target.slotId.toStdString(), mode)) {
                        if (target.onChanged) target.onChanged();
                    } else {
                        QMessageBox::warning(context, QObject::tr("Plugin channel mode"),
                            QObject::tr("This plugin cannot use the selected channel mode"));
                    }
                });
            });
        }
        currentMenu->addSeparator();
        bool hasVariant = false;
        for (const auto& variant : catalogue) {
            if (!sameProduct(current, variant)) continue;
            hasVariant = true;
            QString label = formatLabel(variant.format);
            if (!variant.version.empty()) label += QStringLiteral(" · %1").arg(QString::fromStdString(variant.version));
            auto* action = pickAction(currentMenu, label, variant);
            action->setCheckable(true);
            action->setChecked(variant.uid == current.uid && variant.format == current.format);
            if (action->isChecked() && variant.version == slot->pluginVersion)
                action->setEnabled(false); // Selecting the current build must not reset its state.
        }
        if (!hasVariant) currentMenu->addAction(QObject::tr("No installed variants"))->setEnabled(false);
        groupActions.push_back(menu->addSeparator());
        auto* remove = menu->addAction(QObject::tr("No Plug-in"));
        remove->setObjectName(QStringLiteral("PluginPickerRemove"));
        QObject::connect(remove, &QAction::triggered, lifetime, [controller, lifetime, target] {
            QTimer::singleShot(0, lifetime, [controller, target] { removeTarget(controller, target); });
        });
        groupActions.push_back(remove);
        groupActions.push_back(menu->addSeparator());
    }

    const auto recent = QSettings().value("contextPanel/pluginRecent").toStringList();
    std::vector<daw::plugins::PluginDescriptor> recentPlugins;
    for (const auto& uid : recent) {
        const auto foundUid = prepared->byUid.constFind(uid);
        if (foundUid == prepared->byUid.cend()) continue;
        const auto& descriptor = catalogue[foundUid.value()];
        if (std::any_of(recentPlugins.begin(), recentPlugins.end(),
                        [&](const auto& existing) {
                            return sameProduct(existing, descriptor);
                        })) {
            continue;
        }
        recentPlugins.push_back(descriptor);
        if (recentPlugins.size() == 5) break;
    }
    auto* recentHeading = menu->addAction(QObject::tr("Recent"));
    recentHeading->setObjectName(QStringLiteral("PluginPickerRecent"));
    recentHeading->setEnabled(false);
    groupActions.push_back(recentHeading);
    for (const auto& d : recentPlugins) {
        const QString label = menu->fontMetrics().elidedText(
            QString::fromStdString(d.name), Qt::ElideRight, 250)
                                  .replace('&', QStringLiteral("&&"));
        groupActions.push_back(pickAction(menu, label, d));
    }
    if (recentPlugins.empty()) {
        auto* empty = menu->addAction(QObject::tr("No recent plugins"));
        empty->setEnabled(false);
        groupActions.push_back(empty);
    }
    groupActions.push_back(menu->addSeparator());

    // Only the first level is materialized while the popup opens. Each category
    // and manufacturer pays for its plugin actions only when it is opened.
    const auto addLazyPluginList =
        [searchFilter, pickAction,
         prepared](QMenu* owner, const QString& title,
                   const std::vector<daw::plugins::PluginDescriptor>* entries) {
            auto* submenu = owner->addMenu(title);
            applyDarkPluginMenuStyle(submenu);
            searchFilter->watch(submenu);
            QObject::connect(
                submenu, &QMenu::aboutToShow, submenu,
                [submenu, entries, pickAction, prepared] {
                    if (submenu->property("pluginPickerPopulated").toBool())
                        return;
                    submenu->setProperty("pluginPickerPopulated", true);
                    for (const auto& descriptor : *entries) {
                        const QString label = submenu->fontMetrics().elidedText(
                            QString::fromStdString(descriptor.name),
                            Qt::ElideRight, 250)
                                                  .replace(
                                                      '&',
                                                      QStringLiteral("&&"));
                        pickAction(submenu, label, descriptor);
                    }
                });
            return submenu;
        };
    for (auto& [name, entries] : prepared->categories) {
        auto* category = addLazyPluginList(menu, name, &entries);
        category->setObjectName(QStringLiteral("PluginPickerCategory"));
        groupActions.push_back(
            category->menuAction());
    }

    // The final branch is a single manufacturer catalogue. Its entries already
    // contain the preferred variant chosen in Settings (with the established
    // per-product fallback when that format is unavailable).
    if (!prepared->vendors.empty()) {
        groupActions.push_back(menu->addSeparator());
        auto* manufacturers = menu->addMenu(
            QCoreApplication::translate("PluginPickerMenu", "Manufacturers"));
        applyDarkPluginMenuStyle(manufacturers);
        manufacturers->setObjectName(
            QStringLiteral("PluginPickerManufacturers"));
        searchFilter->watch(manufacturers);
        groupActions.push_back(manufacturers->menuAction());
        QObject::connect(
            manufacturers, &QMenu::aboutToShow, manufacturers,
            [manufacturers, vendors = &prepared->vendors,
             addLazyPluginList, prepared] {
                if (manufacturers->property("pluginPickerPopulated").toBool())
                    return;
                manufacturers->setProperty("pluginPickerPopulated", true);
                for (const auto& [vendor, entries] : *vendors) {
                    auto* vendorMenu = addLazyPluginList(
                        manufacturers,
                        QString(vendor).replace('&', QStringLiteral("&&")),
                        &entries);
                    vendorMenu->setObjectName(
                        QStringLiteral("PluginPickerManufacturer"));
                }
            });
    }

    // Search owns a fixed number of reusable actions. A 2,000-plugin setup now
    // opens with 40 dormant rows rather than 2,000 product submenus (and their
    // variants). Even those 40 rows are deferred until the first character so
    // opening the popup has no search-result construction on its critical path.
    auto searchMatches =
        std::make_shared<std::vector<daw::plugins::PluginDescriptor>>();
    auto matchActions = std::make_shared<std::vector<QAction*>>();
    QAction* noMatch = menu->addAction(QObject::tr("Nothing matches"));
    noMatch->setEnabled(false);
    noMatch->setVisible(false);
    if (found.empty()) {
        auto* empty = menu->addAction(controller->pluginManager().isScanning()
            ? QObject::tr("Scanning for plugins…") : QObject::tr("No plugins found"));
        empty->setToolTip(QObject::tr("Scan in Settings ▸ Plugin Manager"));
        empty->setEnabled(false);
        groupActions.push_back(empty);
    }

    QObject::connect(edit, &QLineEdit::textChanged, menu,
                     [menu, lifetime, onPick, groupActions, matchActions,
                      noMatch, prepared, searchMatches](const QString& query) {
        const QString needle = query.trimmed();
        const bool searching = !needle.isEmpty();
        for (QAction* group : groupActions) group->setVisible(!searching);

        if (searching && matchActions->empty()) {
            matchActions->reserve(kMaxMatches);
            for (int index = 0; index < kMaxMatches; ++index) {
                auto* action = new QAction(menu);
                action->setVisible(false);
                menu->insertAction(noMatch, action);
                matchActions->push_back(action);
                QObject::connect(
                    action, &QAction::triggered, lifetime,
                    [lifetime, onPick, searchMatches, index] {
                        if (index < 0 || index >= static_cast<int>(
                                                   searchMatches->size()))
                            return;
                        const auto descriptor = searchMatches->at(
                            static_cast<std::size_t>(index));
                        QTimer::singleShot(0, lifetime,
                                           [onPick, descriptor] {
                            if (*onPick) (*onPick)(descriptor);
                        });
                    });
            }
        }

        searchMatches->clear();
        if (searching) {
            searchMatches->reserve(kMaxMatches);
            for (const auto& plugin : prepared->searchable) {
                if (!plugin.haystack.contains(needle, Qt::CaseInsensitive))
                    continue;
                searchMatches->push_back(plugin.descriptor);
                if (searchMatches->size() == kMaxMatches) break;
            }
        }
        for (std::size_t i = 0; i < matchActions->size(); ++i) {
            QAction* action = matchActions->at(i);
            const bool visible = i < searchMatches->size();
            if (visible) {
                const auto& descriptor = searchMatches->at(i);
                const QString label = QString::fromStdString(descriptor.name)
                                          .replace('&', QStringLiteral("&&"));
                action->setText(label);
                action->setToolTip(QString::fromStdString(
                    descriptor.name + "\n" + descriptor.vendor + "\n" +
                    descriptor.version));
                action->setProperty(
                    "pluginUid", QString::fromStdString(descriptor.uid));
            } else {
                action->setProperty("pluginUid", QVariant());
            }
            action->setVisible(visible);
        }
        noMatch->setVisible(searching && searchMatches->empty());
    });
}

} // namespace

QMenu* buildPluginMenu(QWidget* parent, daw::EngineController* controller,
                       bool instruments,
                       std::function<void(const daw::plugins::PluginDescriptor&)> onPick,
                       PluginPickerTarget target) {
    auto* menu = new QMenu(parent);
    applyDarkPluginMenuStyle(menu);
    populatePluginMenu(
        menu, parent, controller, instruments,
        std::make_shared<PickCallback>(std::move(onPick)), /*openingNow=*/false, target);
    return menu;
}

void preparePluginPickerMenus(daw::EngineController* controller) {
    if (!controller) return;
    (void)preparedCatalogue(controller, /*instruments=*/false);
    (void)preparedCatalogue(controller, /*instruments=*/true);
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
        applyDarkPluginMenuStyle(menu);
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

void rememberRecentPlugin(const daw::plugins::PluginDescriptor& descriptor) {
    if (descriptor.uid.empty()) return;
    QStringList recent = QSettings().value("contextPanel/pluginRecent").toStringList();
    const QString uid = QString::fromStdString(descriptor.uid);
    recent.removeAll(uid);
    recent.prepend(uid);
    while (recent.size() > 5) recent.removeLast();
    QSettings().setValue("contextPanel/pluginRecent", recent);
}

} // namespace ui
