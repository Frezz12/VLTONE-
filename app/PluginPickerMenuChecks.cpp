#include "PluginPickerMenu.hpp"

#include "EngineController.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QSettings>
#include <QStyle>
#include <QWheelEvent>
#include <memory>

namespace ui {

bool checkPluginPickerForTest(QString* error, const QString& screenshotPath) {
    auto fail = [error](const char* message) {
        if (error) *error = QString::fromLatin1(message);
        return false;
    };
    struct RestoreHistory {
        QVariant old = QSettings().value("contextPanel/pluginRecent");
        ~RestoreHistory() {
            if (old.isValid()) QSettings().setValue("contextPanel/pluginRecent", old);
            else QSettings().remove("contextPanel/pluginRecent");
        }
    } restore;
    QSettings().remove("contextPanel/pluginRecent");

    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return fail("engine initialization");
    const auto effect = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.graphit");
    const auto sampler = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    if (!effect || !sampler) return fail("missing built-in test plugins");
    auto vst = *effect;
    vst.format = daw::plugins::Format::Vst3;
    vst.name = "Test Colour VST3";
    vst.vendor = "Test Vendor GmbH";
    auto au = vst;
    au.format = daw::plugins::Format::AudioUnit;
    au.uid = "test.au";
    au.name = "Test Colour AU";
    au.vendor = "Test Vendor";
    if (!daw::samePluginProduct(vst, au)) return fail("cross-format variants were hidden");
    au.format = vst.format;
    if (daw::samePluginProduct(vst, au)) return fail("distinct shell components were merged");

    // MRU is bounded, de-duplicated, and promotes a reused entry.
    auto synthetic = *effect;
    for (int i = 0; i < 7; ++i) {
        synthetic.uid = "picker.test." + std::to_string(i);
        rememberRecentPlugin(synthetic);
    }
    synthetic.uid = "picker.test.4";
    rememberRecentPlugin(synthetic);
    const auto recent = QSettings().value("contextPanel/pluginRecent").toStringList();
    if (recent.size() != 5 || recent.front() != QLatin1String("picker.test.4") ||
        recent.count(QStringLiteral("picker.test.4")) != 1)
        return fail("recent limit or ordering");
    QSettings().remove("contextPanel/pluginRecent");
    for (const auto& d : controller.pluginManager().effects()) {
        if (d.format == daw::plugins::Format::Internal) rememberRecentPlugin(d);
    }
    rememberRecentPlugin(*effect);

    // The separate surface, including alpha, survives theme export/import.
    Theme custom = th();
    custom.pluginMenuBackground = QColor(52, 60, 68, 231);
    if (ThemeManager::fromJson(ThemeManager::toJson(custom), th()).pluginMenuBackground !=
        custom.pluginMenuBackground) return fail("theme alpha round trip");

    const auto track = controller.addTrack(daw::TrackKind::Audio, "Picker test");
    const auto keep = controller.addInsert(track, *effect);
    const auto replace = controller.addInsert(track, *effect);
    if (keep.empty() || replace.empty()) return fail("effect insertion");
    QWidget root;
    root.resize(600, 720);
    root.show();
    int changes = 0;
    int picked = 0;
    auto makeMenu = [&](const std::string& channel, const std::string& slot, bool instrument) {
        return std::unique_ptr<QMenu>(buildPluginMenu(&root, &controller, instrument,
            [&](const daw::plugins::PluginDescriptor&) { ++picked; },
            {QString::fromStdString(channel), QString::fromStdString(slot), [&] { ++changes; }}));
    };
    preparePluginPickerMenus(&controller);
    QElapsedTimer initialBuild;
    initialBuild.start();
    auto menu = makeMenu(track, replace, false);
    const qint64 initialBuildMs = initialBuild.elapsed();
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_VERBOSE")) {
        std::fprintf(stderr,
                     "Plugin picker build: %lld ms, %d plugin actions, %d root actions\n",
                     static_cast<long long>(initialBuildMs),
                     int(menu->findChildren<QAction*>().size()),
                     int(menu->actions().size()));
    }
    auto* remove = menu->findChild<QAction*>(QStringLiteral("PluginPickerRemove"));
    auto* current = menu->findChild<QAction*>(QStringLiteral("PluginPickerCurrent"));
    auto* search = menu->findChild<QLineEdit*>(QStringLiteral("PluginPickerSearch"));
    auto* manufacturers = menu->findChild<QMenu*>(
        QStringLiteral("PluginPickerManufacturers"),
        Qt::FindDirectChildrenOnly);
    auto* category = menu->findChild<QMenu*>(
        QStringLiteral("PluginPickerCategory"), Qt::FindDirectChildrenOnly);
    if (!remove || !current || !current->menu() || !search ||
        !manufacturers || !category)
        return fail("missing replacement controls");
    if (menu->testAttribute(Qt::WA_TranslucentBackground) ||
        menu->windowOpacity() != 1.0 ||
        manufacturers->style()->styleHint(QStyle::SH_Menu_Scrollable) != 1)
        return fail("plugin picker surface or manufacturer scrolling style");
    menu->popup(root.mapToGlobal(QPoint(10, 10)));
    QApplication::processEvents();
    if (!search->hasFocus()) return fail("search did not receive popup focus");
    QKeyEvent recordOverride(QEvent::ShortcutOverride, Qt::Key_R,
                             Qt::NoModifier, QStringLiteral("r"));
    recordOverride.setAccepted(false);
    QApplication::sendEvent(search, &recordOverride);
    QKeyEvent recordLetter(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier,
                           QStringLiteral("r"));
    QApplication::sendEvent(search, &recordLetter);
    if (!recordOverride.isAccepted() || search->text() != QLatin1String("r"))
        return fail("bare R escaped plugin search as an application shortcut");
    search->clear();
    if (!screenshotPath.isEmpty() && !menu->grab().save(screenshotPath))
        return fail("saving picker screenshot");
    if (!screenshotPath.isEmpty()) {
        current->menu()->adjustSize();
        if (!current->menu()->grab().save(screenshotPath + QStringLiteral(".current.png")))
            return fail("saving channel-mode screenshot");
    }
    // Search must still work when a child menu owns the keyboard, including
    // non-Latin input and application shortcut suppression.
    QKeyEvent key(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, QStringLiteral("и"));
    QApplication::sendEvent(current->menu(), &key);
    if (search->text() != QStringLiteral("и") || current->isVisible() || remove->isVisible())
        return fail("submenu search routing or replacement filtering");
    search->clear();
    if (!current->isVisible() || !remove->isVisible()) return fail("clearing search");

    // Manufacturers are one preferred-format branch and remain wheel
    // navigable even when the platform menu style would clip a long list.
    manufacturers->popup(root.mapToGlobal(QPoint(230, 10)));
    QApplication::processEvents();
    const auto vendorMenus = manufacturers->findChildren<QMenu*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (vendorMenus.empty()) return fail("manufacturer catalogue was not populated");
    if (!screenshotPath.isEmpty() &&
        !manufacturers->grab().save(
            screenshotPath + QStringLiteral(".manufacturers.png")))
        return fail("saving manufacturer screenshot");
    manufacturers->close();
    for (int index = 0; index < 8; ++index)
        manufacturers->addAction(QStringLiteral("Wheel probe %1").arg(index));
    manufacturers->popup(root.mapToGlobal(QPoint(230, 10)));
    QApplication::processEvents();
    manufacturers->setActiveAction(nullptr);
    const QPoint wheelPoint = manufacturers->rect().center();
    const QPoint wheelGlobal = manufacturers->mapToGlobal(wheelPoint);
    auto scrollDown = [&] {
        QWheelEvent wheel(QPointF(wheelPoint), QPointF(wheelGlobal), {},
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
        QApplication::sendEvent(manufacturers, &wheel);
        return std::pair{wheel.isAccepted(), manufacturers->activeAction()};
    };
    const auto [firstWheelAccepted, firstWheelAction] = scrollDown();
    const auto [secondWheelAccepted, secondWheelAction] = scrollDown();
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_VERBOSE")) {
        std::fprintf(stderr,
                     "Manufacturer wheel: accepted=%d/%d actions=%s/%s\n",
                     firstWheelAccepted, secondWheelAccepted,
                     firstWheelAction
                         ? firstWheelAction->text().toUtf8().constData()
                         : "<none>",
                     secondWheelAction
                         ? secondWheelAction->text().toUtf8().constData()
                         : "<none>");
    }
    if (!firstWheelAccepted || !secondWheelAccepted || !firstWheelAction ||
        firstWheelAction == secondWheelAction)
        return fail("manufacturer wheel navigation");
    manufacturers->close();
    menu->close();

    QAction* mono = nullptr;
    for (auto* action : current->menu()->actions())
        if (action->data().isValid() && action->data().toInt() == int(daw::PluginChannelMode::Mono))
            mono = action;
    if (!mono) return fail("missing mono mode");
    mono->trigger();
    QApplication::processEvents();
    if (controller.insertModel(track, replace)->channelMode != daw::PluginChannelMode::Mono || changes != 1)
        return fail("mono mode activation");
    remove->trigger();
    QApplication::processEvents();
    if (controller.insertModel(track, replace) || !controller.insertModel(track, keep) || changes != 2)
        return fail("No Plug-in removed the wrong slot");
    controller.undo();
    if (!controller.insertModel(track, replace)) return fail("undo removal");
    menu.reset();

    const auto midi = controller.addTrack(daw::TrackKind::Midi, "Picker instrument");
    if (!controller.setTrackInstrumentPlugin(midi, *sampler)) return fail("sampler insertion");
    const auto instrumentId = controller.project().findTrack(midi)->instrument.id;
    const auto fx = controller.addSamplerFxInsert(midi, instrumentId, *effect);
    if (fx.empty()) return fail("sampler FX insertion");
    auto fxMenu = makeMenu(midi, fx, false);
    auto* removeFx = fxMenu->findChild<QAction*>(QStringLiteral("PluginPickerRemove"));
    if (!removeFx) return fail("missing sampler FX removal");
    removeFx->trigger();
    QApplication::processEvents();
    if (controller.insertModel(midi, fx) || !controller.insertModel(midi, instrumentId)->isLoaded())
        return fail("sampler FX removal damaged instrument");
    auto instrumentMenu = makeMenu(midi, instrumentId, true);
    auto* removeInstrument = instrumentMenu->findChild<QAction*>(QStringLiteral("PluginPickerRemove"));
    if (!removeInstrument) return fail("missing instrument removal");
    removeInstrument->trigger();
    QApplication::processEvents();
    if (controller.project().findTrack(midi)->instrument.isLoaded()) return fail("instrument removal");
    controller.undo();
    if (!controller.project().findTrack(midi)->instrument.isLoaded()) return fail("undo instrument removal");

    // Real insert replacement is nested inside an auto-deleting context menu.
    // Its destruction must not cancel the queued user command.
    auto* outer = new QMenu(&root);
    auto* nested = buildPluginMenu(outer, &controller, false, {},
        {QString::fromStdString(track), QString::fromStdString(replace), {}});
    outer->addMenu(nested);
    auto* nestedRemove = nested->findChild<QAction*>(QStringLiteral("PluginPickerRemove"));
    if (!nestedRemove) { delete outer; return fail("missing nested removal"); }
    nestedRemove->trigger();
    delete outer;
    QApplication::processEvents();
    if (controller.insertModel(track, replace) || !controller.insertModel(track, keep))
        return fail("closing a parent popup cancelled removal");

    // A lazy empty slot must pick once, release its search filter on close,
    // and read a new palette each time it opens.
    std::unique_ptr<QMenu> lazy(buildLazyPluginMenu(&root, &controller, false,
        [&](const daw::plugins::PluginDescriptor&) { ++picked; }));
    for (int pass = 0; pass < 2; ++pass) {
        QApplication::processEvents();
        QElapsedTimer lazyOpen;
        lazyOpen.start();
        lazy->popup(root.mapToGlobal(QPoint(10, 10)));
        const qint64 lazyOpenMs = lazyOpen.elapsed();
        if (qEnvironmentVariableIsSet("DAW_SELFTEST_VERBOSE")) {
            std::fprintf(stderr,
                         "Plugin picker popup %d: %lld ms, %d root actions\n",
                         pass + 1, static_cast<long long>(lazyOpenMs),
                         int(lazy->actions().size()));
        }
        // aboutToShow builds the catalogue synchronously. Inspect that state
        // before processing desktop activation, which can dismiss a native
        // popup in an automated run and legitimately clear its contents.
        const auto searchCount = lazy->findChildren<QLineEdit*>(QStringLiteral("PluginPickerSearch")).size();
        if (searchCount != 1) {
            if (error) *error = QStringLiteral("lazy search field count=%1, pass=%2, visible=%3")
                .arg(searchCount).arg(pass).arg(lazy->isVisible());
            return false;
        }
        if (lazy->findChildren<QAction*>().size() > 100)
            return fail("lazy picker eagerly materialized the full catalogue");
        auto* lazySearch = lazy->findChild<QLineEdit*>(
            QStringLiteral("PluginPickerSearch"));
        if (!lazySearch) return fail("lazy search field missing");
        lazySearch->setText(QStringLiteral("Graphit"));
        QAction* pick = nullptr;
        for (auto* action : lazy->findChildren<QAction*>())
            if (action->property("pluginUid").toString() == QString::fromStdString(effect->uid)) { pick = action; break; }
        if (!pick) return fail("missing catalogue variant");
        pick->trigger();
        lazy->close();
        QApplication::processEvents();
        // QWidgetAction retires its default editor with deleteLater(). A tight
        // processEvents loop does not drain those events like the real main
        // event loop does, so do that before testing the next popup lifetime.
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!lazy->actions().empty() || picked != pass + 1) return fail("lazy cleanup or activation");
        if (!lazy->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly).empty())
            return fail("lazy submenu widgets leaked after closing");
    }
    return true;
}

} // namespace ui
