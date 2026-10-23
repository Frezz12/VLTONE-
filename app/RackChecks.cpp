#include "Controls.hpp"
#include "ScrollMotion.hpp"
#include <QScopeGuard>
#include <QWheelEvent>
#include "EqualizerPanel.hpp"
#include "FileTypes.hpp"
#include "PluginPickerMenu.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/EqualizerParams.hpp"
#include "Internal/MiniModuleDefinition.hpp"
#include "Internal/SamplerInstance.hpp"
#include "MixerWidget.hpp"
#include "MiniModuleRack.hpp"
#include "RackDeviceCard.hpp"
#include "RackBuiltinView.hpp"
#include "RackSamplerView.hpp"
#include "Internal/Cla2aInstance.hpp"
#include "RackDrag.hpp"
#include "RackParameterBinding.hpp"
#include "UtilityPanel.hpp"
#include "RackWidget.hpp"
#include "RackModulationPanel.hpp"
#include "RackModulationEditor.hpp"
#include "MainWindow.hpp"
#include "PluginEditorWindow.hpp"
#include "serialization/ModulationJson.hpp"
#include "PatternWindow.hpp"
#include "Theme.hpp"
#include "plugins/PluginManager.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyleOptionComboBox>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <cstdio>
#include <cmath>

bool RackWidget::checkForTest(QString* error, const QString& screenshot) {
    const auto fail = [&](const QString& text) {
        if (error)
            *error = text;
        std::fprintf(stderr, "FAIL rack: %s\n", text.toUtf8().constData());
        return false;
    };
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false))
        return fail("initialize");
    const auto a = controller.addTrack(daw::TrackKind::Audio, "Lead vocal");
    const auto b = controller.addTrack(daw::TrackKind::Audio, "Backing vocal");
    const auto eq = controller.addInsert(a, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    const auto shapeEqualizer = [&](const std::string& channel, const std::string& slot) {
        namespace eqp = daw::plugins::equalizer;
        const auto set = [&](unsigned band, eqp::BandParam field, double value) {
            controller.setInsertParameter(channel, slot, eqp::parameterId(eqp::bandParameter(band, field)),
                                          value);
        };
        set(0, eqp::BandParam::Enabled, 1);
        set(0, eqp::BandParam::Frequency, 180);
        set(0, eqp::BandParam::Gain, 5);
        set(1, eqp::BandParam::Enabled, 1);
        set(1, eqp::BandParam::Frequency, 2400);
        set(1, eqp::BandParam::Gain, -4);
        controller.pumpPreviewPluginEvents();
    };
    shapeEqualizer(a, eq);
    const auto comp =
        controller.addInsert(a, daw::plugins::compressor::CompressorInstance::staticDescriptor());
    controller.setRackParameters(a, eq, {"output.gain"});
    QStringList external;
    const auto fixture = qEnvironmentVariable("DAW_TEST_RACK_CLAP_PATH");
    QTemporaryDir catalog;
    if (!fixture.isEmpty()) {
        daw::PluginManager plugins((catalog.path() + "/plugins.json").toStdString());
        const auto scanner = qEnvironmentVariable(
            "DAW_TEST_SCAN_PATH", QDir(QApplication::applicationDirPath()).filePath("daw_scan"));
        plugins.setScannerPath(scanner.toStdString());
        plugins.setSearchPaths(daw::plugins::Format::Clap, {QFileInfo(fixture).absolutePath().toStdString()});
        plugins.startScan();
        plugins.waitForScan();
        if (!plugins.lastScanError().empty())
            return fail(QString::fromStdString(plugins.lastScanError()));
        controller.pluginManager().copyCatalogFrom(plugins);
        for (const auto* id :
             {"com.daw.test.rack-large", "com.daw.test.rack-small", "com.daw.test.rack-empty"}) {
            const auto descriptor = controller.pluginManager().find(daw::plugins::Format::Clap, id);
            if (!descriptor)
                return fail("fixture catalog");
            const auto slot = controller.addInsert(a, *descriptor);
            if (slot.empty())
                return fail("load external fixture");
            external.push_back(QString::fromStdString(slot));
        }
    }
    MixerWidget mixer(&controller);
    const auto capture = [&](const QString& suffix = {}) {
        if (screenshot.isEmpty())
            return;
        QDir().mkpath(QFileInfo(screenshot).absolutePath());
        const QString path = suffix.isEmpty()
                                 ? screenshot
                                 : screenshot.left(screenshot.lastIndexOf('.')) + "-" + suffix + ".png";
        mixer.grab().save(path);
    };
    mixer.resize(1340, 320);
    mixer.setSelectedTrack(QString::fromStdString(a));
    mixer.setRackMode(true);
    mixer.show();
    mixer.activateWindow();
    QApplication::processEvents();
    QApplication::setActiveWindow(&mixer);
    auto* rack = mixer.rack();
    if (!rack->m_mini || rack->m_mini->findChildren<QPushButton*>("AddMiniModule").size() != 1)
        return fail("one insertion area for empty mini modules");
    rack->setFocus();
    QApplication::processEvents();
    auto* eqCard = rack->m_cards.value(QString::fromStdString(eq)).data();
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_MODULES_ONLY") || qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_MODULATION_ONLY") || qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_EQ_ONLY")) {
        if (!eqCard || eqCard->page() != RackDeviceCard::Page::Editor) return fail("module starts on the editor page");
        mixer.resize(1380, 280); QApplication::processEvents();
        auto* face = eqCard->findChild<RackBuiltinView*>();
        if (!face || eqCard->findChild<QAbstractButton*>("RackDeviceOpen")) return fail("equalizer restores its native rack face");
        QString nativeError;
        if (!face->checkEqualizerForTest(&nativeError)) return fail(nativeError);
        capture("home");
        mixer.resize(1380, 240); QApplication::processEvents();
        auto* eqGraph = face->findChild<QWidget*>("RackEqualizerGraph");
        if (mixer.height() > 240 || !eqGraph || eqGraph->height() < face->height() - 8)
            return fail("EQ keeps its full graph at the minimum rack height");
        capture("compact-eq");
        mixer.resize(1380, 280); QApplication::processEvents();
        const auto mouse = [](QWidget* widget, QEvent::Type type, QPoint at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QMouseEvent event(type, at, widget->mapToGlobal(at),
                type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
            QApplication::sendEvent(widget, &event);
        };
        for (const auto page : {RackDeviceCard::Page::Editor, RackDeviceCard::Page::Parameters, RackDeviceCard::Page::Modulation}) {
            eqCard->setPage(page); eqCard->setExpanded(true); QApplication::processEvents();
            if (!eqCard->findChild<QLineEdit*>("RackParameterSearch") || !eqCard->findChild<QScrollArea*>("RackAllParameters"))
                return fail("searchable parameter expansion remains available on every page");
            eqCard->setExpanded(false);
        }
        eqCard->setPage(RackDeviceCard::Page::Parameters); QApplication::processEvents(); eqCard->refresh();
        auto controls = eqCard->findChild<QWidget*>("RackRecentParameters")->findChildren<ui::Knob*>();
        if (controls.size() < 2) return fail("compact parameter controls");
        ui::Knob* touched = nullptr;
        const auto metadata = controller.insertParameters(a, eq);
        for (auto* knob : controls) {
            const auto id = knob->property("modulationParameter").toString().toStdString();
            const auto info = std::find_if(metadata.begin(), metadata.end(), [&](const auto& p) { return p.id == id; });
            if (knob->property("recentParameterIndex").toInt() > 0 && info != metadata.end() && !info->isStepped &&
                info->isAutomatable && !info->isReadOnly && controller.insertParameter(a, eq, id) < info->maxValue)
                touched = knob;
        }
        if (!touched) return fail("a continuous parameter is available for the recent-control gesture");
        const auto touchedId = touched->property("modulationParameter").toString();
        touched->setFocus();
        const QPoint start(touched->width() / 2, 30);
        mouse(touched, QEvent::MouseButtonPress, start); mouse(touched, QEvent::MouseMove, start + QPoint(0, -15));
        eqCard->refresh();
        if (touched->property("recentParameterIndex").toInt() == 0) return fail("parameter order stays stable during a drag");
        mouse(touched, QEvent::MouseButtonRelease, start + QPoint(0, -15)); eqCard->refresh(); QApplication::processEvents();
        auto* first = eqCard->findChild<ui::Knob*>("RackParameter." + touchedId);
        if (!first || first->property("recentParameterIndex").toInt() != 0) return fail("last touched parameter becomes the first control");
        if (QApplication::focusWidget() != first) return fail("keyboard focus follows the recent parameter after reordering");
        capture("parameters");
        eqCard->setPage(RackDeviceCard::Page::Modulation); QApplication::processEvents();
        auto* panel = eqCard->findChild<RackModulationPanel*>();
        if (!panel || !panel->findChild<QAbstractButton*>("RackAddModulation") || panel->width() > 40)
            return fail("empty modulation page starts with a narrow add rail");
        panel->addSource(daw::modulation::Kind::Lfo); QApplication::processEvents(); eqCard->refresh();
        if (controller.rackModulation(a).sources.size() != 1 || controller.rackModulation(a).sources.front().ownerSlotId != eq)
            return fail("LFO belongs to its plugin");
        const auto sourceId = controller.rackModulation(a).sources.front().id;
        if (!panel->findChild<QSlider*>("ModulationRate") || !panel->findChild<QSlider*>("ModulationPhase") ||
            panel->findChild<QScrollArea*>("ModulationAssignments") || panel->findChild<QDoubleSpinBox*>("ModulationDepth"))
            return fail("LFO uses a graph and sliders without the old assignment form");
        auto* target = eqCard->findChild<ui::Knob*>("RackParameter." + touchedId);
        if (!target) return fail("recent parameter remains available for modulation");
        const double base = controller.insertParameter(a, eq, touchedId.toStdString());
        auto* assign = panel->findChild<QAbstractButton*>("ModulationAssign"); assign->click();
        const QPoint at(target->width() / 2, 30);
        mouse(target, QEvent::MouseButtonPress, at); mouse(target, QEvent::MouseButtonRelease, at);
        QApplication::processEvents(); eqCard->refresh();
        if (panel->assigning() || controller.rackModulation(a).routes.size() != 1 ||
            controller.insertParameter(a, eq, touchedId.toStdString()) != base) return fail("icon assignment preserves the parameter base");
        const auto route = controller.rackModulation(a).routes.front();
        const auto depth = [&] { for (const auto& r : controller.rackModulation(a).routes) if (r.id == route.id) return r.depth; return -2.; };
        const auto history = controller.undoDepth();
        mouse(target, QEvent::MouseButtonPress, at);
        mouse(target, QEvent::MouseMove, at + QPoint(0, -30));
        if (depth() <= route.depth) return fail("upward drag increases modulation depth");
        const auto raised = depth();
        mouse(target, QEvent::MouseMove, at + QPoint(0, -15));
        if (depth() >= raised) return fail("downward drag decreases modulation depth immediately");
        mouse(target, QEvent::MouseButtonRelease, at + QPoint(0, -15));
        const auto changedDepth = depth();
        if (controller.undoDepth() != history + 1 || controller.insertParameter(a, eq, touchedId.toStdString()) != base)
            return fail("one depth gesture creates one Undo and leaves the base unchanged");
        controller.undo(); if (std::abs(depth() - route.depth) > 1e-9) return fail("depth Undo restores the range");
        controller.redo(); if (std::abs(depth() - changedDepth) > 1e-9) return fail("depth Redo restores the range");
        const auto escapeDepth = controller.undoDepth();
        mouse(target, QEvent::MouseButtonPress, at); mouse(target, QEvent::MouseMove, at + QPoint(0, -20));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(target, &escape);
        mouse(target, QEvent::MouseButtonRelease, at + QPoint(0, -20));
        if (std::abs(depth() - changedDepth) > 1e-9 || controller.undoDepth() != escapeDepth)
            return fail("Escape cancels the depth gesture without an Undo entry");
        eqCard->refresh(); QApplication::processEvents(); capture("modulation");
        auto* expand = panel->findChild<QAbstractButton*>("ModulationExpand"); expand->click(); QApplication::processEvents();
        auto* dialog = panel->findChild<QDialog*>("ModulationExpandedEditor");
        if (!dialog || !dialog->isVisible() || !dialog->findChild<QSlider*>("ModulationSmoothing"))
            return fail("LFO expands into precise visual controls");
        if (!screenshot.isEmpty()) dialog->grab().save(screenshot + ".lfo.png");
        dialog->close(); QApplication::processEvents();
        const auto lfoOnly = controller.rackModulation(a);
        for (const auto kind : {daw::modulation::Kind::Follower, daw::modulation::Kind::Shaper,
                               daw::modulation::Kind::Steps, daw::modulation::Kind::Random}) {
            const auto id = controller.addRackModulator(a, kind, eq);
            panel->refresh(); QApplication::processEvents(); eqCard->refresh(); QApplication::processEvents();
            QWidget* sourceCard = nullptr;
            for (auto* candidate : panel->findChildren<QWidget*>("RackModulatorCard"))
                if (candidate->property("sourceId").toString().toStdString() == id) sourceCard = candidate;
            if (!sourceCard || sourceCard->width() != 210) return fail("all source kinds have compact embedded cards");
            RackModulationEditor* graph = nullptr;
            for (auto* candidate : sourceCard->findChildren<QWidget*>())
                if (auto* editor = dynamic_cast<RackModulationEditor*>(candidate)) graph = editor;
            if (!graph) return fail("every source kind has a visual preview");
            if (kind == daw::modulation::Kind::Steps) {
                const auto mark = controller.undoDepth();
                mouse(graph, QEvent::MouseButtonPress, QPoint(28, 20));
                mouse(graph, QEvent::MouseMove, QPoint(110, 45));
                mouse(graph, QEvent::MouseButtonRelease, QPoint(110, 45));
                if (controller.undoDepth() != mark + 1) return fail("step drawing remains one Undo gesture");
                controller.undo(); controller.redo();
            }
            sourceCard->findChild<QAbstractButton*>("ModulationExpand")->click(); QApplication::processEvents();
            QDialog* expanded = nullptr;
            for (auto* candidate : panel->findChildren<QDialog*>("ModulationExpandedEditor"))
                if (candidate->isVisible()) expanded = candidate;
            if (!expanded || !expanded->isVisible()) return fail("every source kind expands for detailed editing");
            expanded->close(); QApplication::processEvents();
            if (!controller.setRackModulation(a, lfoOnly)) return fail("restore the LFO fixture");
            panel->refresh(); QApplication::processEvents(); eqCard->refresh(); QApplication::processEvents();
        }
        const auto unassigned = controller.addRackModulator(a, daw::modulation::Kind::Lfo, eq);
        daw::EngineController::ChannelSnapshot copied;
        if (!controller.captureRackSelection(a, {eq}, copied)) return fail("capture plugin with modulation");
        std::vector<std::string> pasted;
        if (!controller.pasteRackSelection(b, copied, 0, {}, &pasted) || pasted.size() != 1) return fail("paste plugin with modulation");
        const auto& cloned = controller.rackModulation(b);
        if (cloned.sources.size() != 2 || cloned.routes.size() != 1 ||
            std::any_of(cloned.sources.begin(), cloned.sources.end(), [&](const auto& s) { return s.ownerSlotId != pasted.front() || s.id == sourceId || s.id == unassigned; }))
            return fail("plugin copy carries assigned and unassigned sources with fresh identities");
        const auto saved = nlohmann::json(cloned).get<daw::modulation::Rack>();
        if (saved != cloned) return fail("modulation ownership survives serialization");
        eqCard->setPage(RackDeviceCard::Page::Modulation); QApplication::processEvents();
        auto* deletionPanel = eqCard->findChild<RackModulationPanel*>();
        deletionPanel->refresh(); QApplication::processEvents();
        RackModulationEditor* lfoGraph = nullptr;
        QToolButton* sourceTitle = nullptr;
        for (auto* widget : deletionPanel->findChildren<QWidget*>()) {
            if (widget->property("sourceId").toString() != QString::fromStdString(unassigned)) continue;
            if (auto* graph = dynamic_cast<RackModulationEditor*>(widget)) lfoGraph = graph;
            if (widget->objectName() == "ModulationSourceTitle") sourceTitle = qobject_cast<QToolButton*>(widget);
        }
        if (!lfoGraph || !sourceTitle) return fail("deletion source is present");
        QApplication::setActiveWindow(&mixer);
        mouse(lfoGraph, QEvent::MouseButtonPress, lfoGraph->rect().center());
        mouse(lfoGraph, QEvent::MouseButtonRelease, lfoGraph->rect().center());
        if (QApplication::focusWidget() != lfoGraph) return fail("clicking an LFO preview keeps focus on its source");
        QKeyEvent removeSource(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(lfoGraph, &removeSource); QApplication::processEvents();
        if (controller.rackModulation(a).sources.size() != 1) return fail("Delete on an LFO removes the source");
        controller.undo(); deletionPanel->refresh(); QApplication::processEvents();
        sourceTitle = nullptr;
        for (auto* title : deletionPanel->findChildren<QToolButton*>("ModulationSourceTitle"))
            if (title->property("sourceId").toString() == QString::fromStdString(unassigned)) sourceTitle = title;
        if (!sourceTitle) return fail("Undo restores deleted source");
        QTimer::singleShot(30, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); if (!menu) return;
            for (auto* action : menu->actions()) if (action->objectName() == "DeleteModulationSource") {
                const QPoint at = menu->actionGeometry(action).center();
                mouse(menu, QEvent::MouseButtonPress, at); mouse(menu, QEvent::MouseButtonRelease, at); return;
            }
            menu->close();
        });
        QMetaObject::invokeMethod(sourceTitle, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(6, 6)));
        QApplication::processEvents();
        if (controller.rackModulation(a).sources.size() != 1) return fail("source menu Delete removes a modulation node");
        controller.undo(); rack->sync(); QApplication::processEvents();
        std::string layerGroup;
        if (!controller.createRackGroup(a, {eq, comp}, "Parallel processing", &layerGroup, true)) return fail("create parallel layers");
        rack->sync(); QApplication::processEvents(); rack->refresh();
        auto solos = rack->findChildren<QToolButton*>("RackLayerSolo");
        if (solos.size() != 2) return fail("layers expose two Solo controls");
        mouse(solos.front(), QEvent::MouseButtonPress, solos.front()->rect().center());
        mouse(solos.front(), QEvent::MouseButtonRelease, solos.front()->rect().center());
        if (!controller.rackGroups(a).front().layers.front().solo) return fail("Solo control updates the audible branch");
        capture("layers");
        std::string outer;
        if (!controller.createRackGroup(a, {eq, comp}, "Processing", &outer)) return fail("nested group wraps layers");
        rack->sync(); QApplication::processEvents();
        if (!rack->m_groups.value(QString::fromStdString(outer))->isAncestorOf(rack->m_groups.value(QString::fromStdString(layerGroup))))
            return fail("nested groups have distinct visual boundaries");
        capture("nested");
        auto* addLayer = rack->findChild<QToolButton*>("RackAddLayer");
        if (!addLayer) return fail("nested layers expose an add action");
        mouse(addLayer, QEvent::MouseButtonPress, addLayer->rect().center());
        mouse(addLayer, QEvent::MouseButtonRelease, addLayer->rect().center());
        QApplication::processEvents();
        std::string emptyLayer;
        for (const auto& group : controller.rackGroups(a)) if (group.id == layerGroup && group.layers.size() == 3)
            emptyLayer = group.layers.back().id;
        if (emptyLayer.empty() || !rack->insertPlugin(daw::plugins::equalizer::EqualizerInstance::staticDescriptor(), -1, QString::fromStdString(emptyLayer)))
            return fail("add an effect to a new nested layer");
        bool populated = false;
        for (const auto& group : controller.rackGroups(a)) for (const auto& layer : group.layers)
            if (layer.id == emptyLayer) populated = layer.insertIds.size() == 1 && !layer.muted;
        if (!populated) return fail("adding an effect activates its empty layer");
        controller.undo();
        if (controller.channelInserts(a)->size() != 2) return fail("one Undo removes the effect and its layer membership");
        mixer.hide();
        MainWindow window(false); auto* appController = window.collaborationEngineController();
        const auto track = appController->addTrack(daw::TrackKind::Audio, "Toggle editor");
        const auto slot = appController->addInsert(track, daw::plugins::cla2a::Cla2aInstance::staticDescriptor());
        auto* appMixer = window.findChild<MixerWidget*>();
        daw::EngineController otherProject;
        appMixer->setProjectController(&otherProject);
        appMixer->setProjectController(appController);
        window.resize(1200, 800); window.show(); window.setMixerShownForShot(true);
        window.selectTracksForShot(QStringLiteral("Toggle editor"));
        appMixer->setSelectedTrack(QString::fromStdString(track)); appMixer->setRackMode(true); appMixer->rack()->sync();
        QApplication::processEvents();
        window.setMixerHeightForShot(700); QApplication::processEvents();
        auto* bottom = window.findChild<QWidget*>("BottomEditorPanel");
        if (!bottom || bottom->maximumHeight() != 280 || bottom->height() > 280 || bottom->minimumHeight() != 240)
            return fail("rack height stays within 240–280 px");
        const auto currentToggle = [&]() -> QAbstractButton* {
            auto* card = appMixer->rack()->m_cards.value(QString::fromStdString(slot)).data();
            if (!card) return nullptr;
            card->refresh();
            return card->findChild<QAbstractButton*>("RackDeviceOpen");
        };
        if (!currentToggle()) return fail("application exposes the large editor toggle");
        const auto settle = [] { QEventLoop loop; QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec(); };
        const auto clickToggle = [&] {
            QPointer<QAbstractButton> toggle = currentToggle();
            if (!toggle) return false;
            mouse(toggle, QEvent::MouseButtonPress, toggle->rect().center());
            if (!toggle) return false;
            mouse(toggle, QEvent::MouseButtonRelease, toggle->rect().center());
            return true;
        };
        if (!clickToggle()) return fail("click plugin editor toggle");
        settle();
        if (!currentToggle() || !currentToggle()->isChecked()) return fail("large button opens the real plugin editor");
        if (!clickToggle()) return fail("click open plugin editor toggle");
        settle();
        if (!currentToggle() || currentToggle()->isChecked()) return fail("second click closes the real plugin editor");
        if (!clickToggle()) return fail("reopen plugin editor toggle");
        settle();
        auto editors = window.findChildren<PluginEditorWindow*>();
        for (auto* editor : editors) if (!editor->isClosing()) editor->close();
        settle();
        if (!currentToggle() || currentToggle()->isChecked()) return fail("closing the editor directly clears the toggle");
        for (const auto& [uid, panelName] : {std::pair{"daw.equalizer", "EqualizerPanel"},
                                            std::pair{"daw.compressor", "CompressorPanel"}}) {
            const auto descriptor = appController->pluginManager().find(daw::plugins::Format::Internal, uid);
            if (!descriptor) return fail("builtin editor fixture is available");
            if (!appMixer->rack()->insertPlugin(*descriptor, -1, {}))
                return fail("add a built-in plugin through the rack");
            const auto nativeSlot = appController->channelInserts(track)->back().id;
            settle();
            appMixer->rack()->sync();
            if (auto* card = appMixer->rack()->m_cards.value(QString::fromStdString(nativeSlot)).data())
                appMixer->rack()->m_scroll->ensureWidgetVisible(card);
            QApplication::processEvents();
            const auto headerToggle = [&]() -> QAbstractButton* {
                auto* card = appMixer->rack()->m_cards.value(QString::fromStdString(nativeSlot)).data();
                if (!card) return nullptr;
                card->refresh();
                return card->findChild<QAbstractButton*>("RackDeviceOpenEditor");
            };
            const auto pressHeader = [&] {
                QPointer<QAbstractButton> button = headerToggle();
                if (!button || !button->isVisible() || !button->isEnabled()) return false;
                mouse(button, QEvent::MouseButtonPress, button->rect().center());
                if (!button) return false;
                mouse(button, QEvent::MouseButtonRelease, button->rect().center());
                return true;
            };
            if (!pressHeader()) {
                auto* button = headerToggle();
                return fail(QString("built-in full-editor header: %1, exists=%2, visible=%3, enabled=%4, channel=%5")
                    .arg(QString::fromUtf8(uid)).arg(button != nullptr).arg(button && button->isVisible())
                    .arg(button && button->isEnabled()).arg(appMixer->rack()->channelId()));
            }
            settle();
            bool nativeVisible = false;
            for (auto* editor : window.findChildren<PluginEditorWindow*>())
                if (!editor->isClosing()) {
                    auto* panel = editor->findChild<QWidget*>(QString::fromUtf8(panelName));
                    nativeVisible |= panel && panel->isVisible();
                }
            if (!nativeVisible || !headerToggle() || !headerToggle()->isChecked())
                return fail("header opens the built-in plugin's full native interface");
            auto* card = appMixer->rack()->m_cards.value(QString::fromStdString(nativeSlot)).data();
            card->setPage(RackDeviceCard::Page::Parameters); QApplication::processEvents();
            if (!pressHeader()) return fail("full-editor toggle stays available on the parameter page");
            settle();
            if (!headerToggle() || headerToggle()->isChecked()) return fail("header toggle closes the full built-in editor");
        }
        std::fprintf(stderr, "PASS rack modules: default launcher, three pages, recent controls, embedded LFO, direct depth, Undo, copy, save and real editor toggle\n");
        return true;
    }
    if (!eqCard || !eqCard->findChild<RackBuiltinView*>())
        return fail("compact equalizer width");
    {
        auto& prefs = ui::ScrollPreferences::instance();
        const bool smooth = prefs.enabled();
        const auto restore = qScopeGuard([&] { prefs.setEnabled(smooth); });
        prefs.setEnabled(false);
        const QSize originalSize = mixer.size();
        mixer.resize(760, mixer.height());
        QApplication::processEvents();
        auto* bar = rack->m_scroll->horizontalScrollBar();
        if (bar->maximum() <= 0) return fail("rack wheel fixture must overflow horizontally");
        bar->setValue(0);
        const auto wheel = [&](int angle) {
            auto* viewport = rack->m_scroll->viewport();
            const QPointF at = viewport->rect().center();
            QWheelEvent event(at, viewport->mapToGlobal(at), {}, {0, angle},
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &event);
        };
        wheel(-120);
        if (bar->value() <= 0) return fail("ordinary mouse wheel scrolls the rack horizontally");
        const int right = bar->value();
        wheel(120);
        if (bar->value() >= right) return fail("rack wheel reverses without Shift");
        mixer.resize(originalSize);
        QApplication::processEvents();
    }
    const auto identity = controller.insertIdentity(a, eq);
    auto* unchanged = eqCard;
    controller.setInsertParameter(a, eq, "output.gain", -3);
    rack->sync();
    if (rack->m_cards.value(QString::fromStdString(eq)) != unchanged)
        return fail("parameter edit reconstructed the card");
    mixer.setRackMode(false);
    mixer.setRackMode(true);
    QApplication::processEvents();
    if (controller.insertIdentity(a, eq) != identity)
        return fail("view switch reconstructed processor");
    const auto checkCollapse = [&](RackDeviceCard* card, const QString& suffix) {
        auto* header = card->findChild<QWidget*>("RackDeviceHeader");
        auto* body = card->findChild<QWidget*>("RackDeviceBody");
        auto* footer = card->findChild<QWidget*>("RackDeviceFooter");
        auto* power = card->findChild<ui::IconButton*>("RackDevicePower");

        auto* fold = card->findChild<ui::IconButton*>("RackDeviceFold");
        if (!header || !body || !footer || !power || !fold || !fold->isHidden())
            return false;
        const auto slot = card->slotId().toStdString();
        const auto processor = controller.insertIdentity(a, slot);
        const int width = card->width();
        const int mixerHeight = mixer.height();
        const bool expanded = card->expanded();
        auto* expand = card->findChild<ui::IconButton*>("RackDeviceExpandParameters");
        if (!expand) return false;
        const bool expandHidden = expand->isHidden();
        int editorRequests = 0;
        QObject receiver;
        QObject::connect(card, &RackDeviceCard::editorToggleRequested, &receiver,
                         [&](const QString&, const QString&) { ++editorRequests; });
        const auto doubleClick = [&] {
            for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                              QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
                const QPointF at = header->rect().center();
                QMouseEvent event(type, at, header->mapToGlobal(at.toPoint()), Qt::LeftButton,
                                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(header, &event);
            }
            QApplication::processEvents();
        };
        doubleClick();
        mixer.resize(mixer.width(), 280);
        rack->sync();
        rack->refresh();
        rack->m_scroll->ensureWidgetVisible(card);
        QApplication::processEvents();
        if (!card->collapsed() || fold->isHidden() || !expand->isHidden() || card->width() != 36 || !body->isHidden() || !footer->isHidden() ||
            editorRequests || card->findChild<QWidget*>("RackDeviceBody") != body)
            return false;
        if (fold->y() <= power->y()) return false;
        for (auto* button : header->findChildren<ui::IconButton*>())
            if (button->isVisible() && !card->rect().contains(QRect(button->mapTo(card, QPoint()), button->size())))
                return false;
        capture("collapsed-" + suffix);
        power->click();
        if (!controller.insertModel(a, slot)->bypassed || !card->collapsed()) return false;
        power->click();
        if (editorRequests || !card->collapsed()) return false;
        {
            RackDeviceCard restored(&controller, QString::fromStdString(a), card->slotId());
            if (!restored.collapsed() || restored.width() != 36) return false;
        }
        doubleClick();
        if (card->collapsed() || !fold->isHidden() || expand->isHidden() != expandHidden || card->width() != width || body->isHidden() || footer->isHidden() ||
            card->expanded() != expanded || editorRequests ||
            controller.insertIdentity(a, slot) != processor)
            return false;
        doubleClick();
        if (!card->collapsed() || fold->isHidden()) return false;
        fold->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(fold, &press);
        QApplication::sendEvent(fold, &release);
        if (card->collapsed() || !fold->isHidden() || QApplication::focusWidget() != card) return false;
        footer->findChild<QDoubleSpinBox*>()->setFocus();
        card->setCollapsed(true);
        if (QApplication::focusWidget() != fold) return false;
        card->setCollapsed(false);
        mixer.resize(mixer.width(), mixerHeight);
        QApplication::processEvents();
        return true;
    };
    if (!checkCollapse(eqCard, "equalizer"))
        return fail("device collapse preserves controls, editor action, processor and saved state");
    rack->selectDevices({QString::fromStdString(eq), QString::fromStdString(comp)});
    rack->setFocus();
    rack->command("group");
    rack->sync();
    QApplication::processEvents();
    if (controller.rackGroups(a).size() != 1 || rack->m_groups.size() != 1)
        return fail("group represented in rack");
    mixer.rebuild();
    mixer.setRackMode(false);
    QApplication::processEvents();
    if (mixer.findChildren<QToolButton*>("MixerRackGroup").isEmpty())
        return fail("group represented in mixer");
    mixer.setRackMode(true);
    QApplication::processEvents();
    rack->setFocus();
    const auto before = controller.project().findTrack(a)->inserts.size(), depth = controller.undoDepth();
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_D, Qt::ControlModifier);
    QApplication::sendEvent(rack, &overrideEvent);
    QKeyEvent duplicate(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier);
    QApplication::sendEvent(rack, &duplicate);
    rack->sync();
    QApplication::processEvents();
    if (controller.project().findTrack(a)->inserts.size() != before + 2 ||
        controller.undoDepth() != depth + 1)
        return fail("Cmd/Ctrl D duplicates selected group as one operation");
    rack->command("undo");
    rack->sync();
    QApplication::processEvents();
    if (controller.project().findTrack(a)->inserts.size() != before)
        return fail("rack undo");
    auto* spin = eqCard->findChild<QDoubleSpinBox*>();
    if (spin) {
        spin->setFocus();
        if (rack->command("delete"))
            return fail("numeric input lost keyboard priority");
    }
    if (!external.isEmpty()) {
        auto* card = rack->m_cards.value(external[0]).data();
        if (!card || card->width() != 288)
            return fail("external compact width");
        card->setPage(RackDeviceCard::Page::Parameters);
        const auto parameters = controller.insertParameters(a, external[0].toStdString());
        if (parameters.size() != 1033)
            return fail("large plugin catalog");
        if (card->findChildren<ui::Knob*>().size() != 8)
            return fail("exactly eight compact controls");
        card->setExpanded(true);
        rack->m_row->activate();
        rack->m_scroll->ensureWidgetVisible(card);
        QApplication::processEvents();
        if (card->width() != 560)
            return fail("external expanded width");
        if (card->findChildren<ui::Knob*>().size() > 32)
            return fail("expanded parameter list is not virtualized");
        capture("expanded");
        if (!checkCollapse(card, "external"))
            return fail("expanded external device restores its controls and width after collapse");
        auto* search = card->findChild<QLineEdit*>("RackParameterSearch");
        if (!search)
            return fail("expanded search");
        search->setText("1033");
        QApplication::processEvents();
        if (card->findChildren<ui::Knob*>().size() != 9)
            return fail("search materializes only matching controls");
        search->clear();
        auto* grid = card->findChild<QScrollArea*>("RackAllParameters");
        if (!grid)
            return fail("parameter grid");
        grid->verticalScrollBar()->setValue(grid->verticalScrollBar()->maximum());
        QApplication::processEvents();
        if (card->findChildren<ui::Knob*>().size() > 32)
            return fail("virtualized scrolling");
        const auto pins =
            std::vector<std::string>{parameters[1020].id, parameters[3].id, "", parameters[1].id};
        controller.setRackParameters(a, external[0].toStdString(), pins);
        card->sync();
        QApplication::processEvents();
        auto* edited =
            card->findChild<ui::Knob*>("RackParameter." + QString::fromStdString(parameters[1020].id));
        if (!edited)
            return fail("assigned control uses stable ID");
        controller.setInsertParameter(a, external[0].toStdString(), parameters[1020].id, .63);
        controller.pumpPreviewPluginEvents();
        card->refresh();
        if (std::abs(edited->value() - .63) > .001)
            return fail("external editor changes reach rack knob");
        card->setExpanded(false);
        rack->m_cards.value(external[1])->setPage(RackDeviceCard::Page::Parameters);
        rack->m_cards.value(external[2])->setPage(RackDeviceCard::Page::Parameters);
        if (rack->m_cards.value(external[1])->findChildren<ui::Knob*>().size() != 5 ||
            !rack->m_cards.value(external[2])->findChildren<ui::Knob*>().isEmpty())
            return fail("plugins with fewer than eight and zero parameters");
    }
    {
        QMimeData payload;
        payload.setData(ui::rack::mime,
                        ui::rack::encode(QString::fromStdString(a),
                                         {QString::fromStdString(eq), QString::fromStdString(comp)}));
        auto* add = rack->findChild<QWidget*>("RackAddDevice");
        rack->m_scroll->ensureWidgetVisible(add);
        QApplication::processEvents();
        const QPoint at = add->mapTo(rack->m_scroll->viewport(), add->rect().center());
        QDragEnterEvent enter(at, Qt::MoveAction | Qt::CopyAction, &payload, Qt::LeftButton, Qt::AltModifier);
        QApplication::sendEvent(rack->m_scroll->viewport(), &enter);
        QDropEvent drop(at, Qt::MoveAction | Qt::CopyAction, &payload, Qt::LeftButton, Qt::AltModifier);
        QApplication::sendEvent(rack->m_scroll->viewport(), &drop);
        QApplication::processEvents();
        if (!drop.isAccepted() || drop.dropAction() != Qt::CopyAction ||
            controller.rackGroups(a).size() != 2 || rack->selectedDevices().size() != 2)
            return fail("Alt drop copies a whole group and selects its new devices");
        rack->setFocus();
        rack->command("undo");
        QApplication::processEvents();
    }
    rack->selectDevices({QString::fromStdString(eq), QString::fromStdString(comp)});
    rack->setFocus();
    rack->transferTo(QString::fromStdString(b), false);
    rack->sync();
    QApplication::processEvents();
    if (rack->selectedDevices().size() != 2 || controller.rackGroups(b).size() != 1)
        return fail("destination selection after transfer");
    mixer.setSelectedTrack(QString::fromStdString(a));
    rack->sync();
    QApplication::processEvents();
    mixer.resize(680, 280);
    QApplication::processEvents();
    if (rack->m_left->geometry().intersects(rack->m_sends->geometry()) ||
        rack->m_scroll->viewport()->width() < 60)
        return fail("fixed edges in narrow window");
    capture("narrow");
    mixer.resize(1340, 320);
    mixer.setSelectedTrack(QString::fromStdString(b));
    int miniCount = 0;
    for (const auto* module : {"color", "doubler", "chorus"}) {
        controller.addMiniModule(b, daw::plugins::mini::builtin(module));
        rack->sync();
        QApplication::processEvents();
        const auto cards = rack->m_mini->findChildren<QWidget*>("MiniModuleCard");
        if (cards.size() != ++miniCount || cards.front()->y() != 1)
            return fail("mini modules fill from the top in insertion order");
        for (int i = 1; i < cards.size(); ++i)
            if (cards[i]->y() != cards[i - 1]->geometry().bottom() + 2)
                return fail("mini modules remain adjacent without empty gaps");
        const auto* add = rack->m_mini->findChild<QPushButton*>("AddMiniModule");
        if (miniCount < 3 && (!add || add->y() <= cards.back()->geometry().bottom()))
            return fail("mini module insertion area stays below loaded cards");
        capture(QStringLiteral("mini-%1").arg(miniCount));
    }
    const auto sendTrack = controller.addTrack(daw::TrackKind::Aux, "Reverb");
    const auto sendId = controller.addSend(b, sendTrack);
    for (const auto* name : {"Delay", "Room"})
        controller.addSend(b, controller.addTrack(daw::TrackKind::Aux, name));
    mixer.setSelectedTrack(QString::fromStdString(b));
    QApplication::processEvents();
    rack->refresh();
    QApplication::processEvents();
    if (rack->m_cards.size() != 2 || rack->m_groups.size() != 1 ||
        !rack->m_groups.begin().value()->isVisible())
        return fail("group remains visible when returning to its channel");
    const auto miniCards = rack->m_mini->findChildren<QWidget*>("MiniModuleCard");
    if (miniCards.size() != 3 || !rack->m_mini->findChildren<QPushButton*>("AddMiniModule").isEmpty())
        return fail("three mini modules occupy the shared insertion area");
    for (const int height : {320, 480, 280}) {
        mixer.resize(1340, height);
        QApplication::processEvents();
        if (miniCards.front()->y() != 1)
            return fail("full mini rack remains anchored at the top when resized");
        for (int i = 1; i < miniCards.size(); ++i)
            if (miniCards[i]->y() != miniCards[i - 1]->geometry().bottom() + 2 ||
                std::abs(miniCards[i]->height() - miniCards.front()->height()) > 1)
                return fail("three mini modules evenly fill the height without gaps");
        const auto* last = miniCards.back();
        if (std::abs(last->mapTo(rack, QPoint(0, last->height())).y() - (rack->height() - 7)) > 2)
            return fail("last mini module stays on the bottom edge");
        if (rack->m_sendScroll->verticalScrollBar()->maximum() != 0 ||
            rack->m_sendScroll->horizontalScrollBar()->maximum() <= 0)
            return fail("vertical sends fit compact height and scroll horizontally");
        if (height == 480) capture("mini-expanded");
    }
    const auto sendLevel = [&] { return controller.project().findTrack(b)->sends.front().level; };
    const auto fader = [&] { return rack->m_sendFaders.value(QString::fromStdString(sendId)).data(); };
    const auto dragSend = [&] {
        auto* control = fader();
        if (!control) return false;
        const QPoint press = control->rect().center(), move = press - QPoint(0, 16);
        QMouseEvent down(QEvent::MouseButtonPress, press, control->mapToGlobal(press),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(control, &down);
        QMouseEvent drag(QEvent::MouseMove, move, control->mapToGlobal(move),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(control, &drag);
        return control->isEditing();
    };
    const float sendBefore = sendLevel();
    const auto sendUndo = controller.undoDepth();
    if (!dragSend() || sendLevel() <= sendBefore || controller.undoDepth() != sendUndo)
        return fail("send fader changes level live during drag");
    const auto draggedLevel = sendLevel();
    // Switching channels must finish the old route's gesture before its widget is removed.
    mixer.setSelectedTrack(QString::fromStdString(a));
    if (controller.undoDepth() != std::min(sendUndo + 1, controller.undoLimit()))
        return fail("send fader commits once when switching channels");
    controller.undo();
    if (std::abs(sendLevel() - sendBefore) > 1e-5)
        return fail("send fader undo");
    controller.redo();
    if (std::abs(sendLevel() - draggedLevel) > 1e-5)
        return fail("send fader redo");
    mixer.setSelectedTrack(QString::fromStdString(b));
    rack->refresh();
    QApplication::processEvents();
    if (!fader() || std::abs(fader()->gain() - sendLevel()) > 1e-5)
        return fail("send fader restores current route level");
    QKeyEvent maximum(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QApplication::sendEvent(fader(), &maximum);
    if (std::abs(20.0 * std::log10(sendLevel()) - 6.0) > 1e-4 ||
        sendLevel() > daw::EngineController::kMaxSendLevel)
        return fail("keyboard send edit respects maximum gain");
    controller.undo();
    rack->refresh();
    bool automated = false;
    const auto automation = QObject::connect(rack, &RackWidget::sendAutomationRequested,
        [&](const QString& channel, const QString& route) {
            automated = channel == QString::fromStdString(b) && route == QString::fromStdString(sendId);
        });
    const bool creation = ui::automationCreationMode();
    ui::setAutomationCreationMode(true);
    const QPoint center = fader()->rect().center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, center, fader()->mapToGlobal(center),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(fader(), &doubleClick);
    ui::setAutomationCreationMode(creation);
    QObject::disconnect(automation);
    if (!automated || std::abs(sendLevel() - draggedLevel) > 1e-5)
        return fail("send automation gesture preserves the level");
    capture();
    mixer.resize(680, 280);
    QApplication::processEvents();
    rack->m_sendScroll->horizontalScrollBar()->setValue(rack->m_sendScroll->horizontalScrollBar()->maximum());
    const auto sendFrames = rack->m_sendScroll->findChildren<QFrame*>("RackSend");
    if (sendFrames.size() != 3 ||
        sendFrames.back()->mapTo(rack->m_sendScroll->viewport(), sendFrames.back()->rect().bottomRight()).x() >=
            rack->m_sendScroll->viewport()->width())
        return fail("last send remains reachable in a narrow rack");
    capture("sends-narrow");
    const auto gallery = controller.addTrack(daw::TrackKind::Instrument, "Device gallery");
    const QString samplePath = catalog.path() + "/rack-voice.wav";
    {
        QFile file(samplePath);
        if (!file.open(QIODevice::WriteOnly))
            return fail("waveform fixture");
        QDataStream out(&file);
        out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("RIFF", 4);
        out << quint32(36 + 96000);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2)
            << quint16(16);
        out.writeRawData("data", 4);
        out << quint32(96000);
        for (int frame = 0; frame < 48000; ++frame) {
            const double envelope = std::exp(-double(frame % 12000) / 2600.);
            out << qint16(std::sin(frame * .02879793266) * envelope * 26000.);
        }
    }
    QStringList compactLayoutErrors;
    for (const auto* uid :
         {"daw.gain", "daw.stereo-utility", "daw.reverse",
          "daw.equalizer", "daw.compressor", "daw.cla2a", "daw.1073", "daw.delay",
#if DAW_ENABLE_REVERB
          "daw.reverb",
#endif
          "daw.pitch-corrector", "daw.graphit",
          "daw.gravity", "daw.modulation", "daw.doubler", "daw.doubler-pro", "daw.chorus", "daw.flanger",
          "daw.phaser", "daw.sampler", "daw.slicer"}) {
        if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_SAMPLER_ONLY") && QString::fromUtf8(uid) != "daw.sampler")
            continue;
        const auto descriptor = controller.pluginManager().find(daw::plugins::Format::Internal, uid);
        if (!descriptor)
            return fail(QString("missing builtin ") + uid);
        const bool instrument = descriptor->isInstrument;
        std::string slot, neighbour;
        if (instrument) {
            if (!controller.setTrackInstrumentPlugin(gallery, *descriptor))
                return fail("gallery instrument");
            slot = controller.project().findTrack(gallery)->instrument.id;
            if (QString::fromUtf8(uid) == "daw.sampler")
                controller.loadSamplerSample(gallery, slot, samplePath.toStdString());
            else
                controller.loadSlicerSample(gallery, slot, samplePath.toStdString());
        } else {
            slot = controller.addInsert(gallery, *descriptor);
            if (QString::fromUtf8(uid) == "daw.equalizer")
                shapeEqualizer(gallery, slot);
            neighbour =
                controller.addInsert(gallery, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
            controller.createRackGroup(gallery, {slot, neighbour}, "Rack group");
        }
        mixer.setSelectedTrack(QString::fromStdString(gallery));
        rack->sync();
        // Minimum-size changes propagate through the scroll area and mixer in
        // queued layout requests. Measure after Qt completes that layout turn.
        QEventLoop layoutTurn;
        QTimer::singleShot(10, &layoutTurn, &QEventLoop::quit);
        layoutTurn.exec();
        // The previous card's minimum no longer applies after layout settles.
        // QWidget does not shrink itself when that minimum is lowered.
        mixer.resize(1340, 280);
        QApplication::processEvents();
        if (!rack->m_sendFaders.isEmpty())
            return fail("previous channel sends leaked into an empty destination");
        auto* card = rack->m_cards.value(QString::fromStdString(slot)).data();
        if (!card || card->page() != RackDeviceCard::Page::Editor ||
            (RackBuiltinView::supports(uid) ? !card->findChild<RackBuiltinView*>() : !card->findChild<QAbstractButton*>("RackDeviceOpen")))
            return fail(QString("default plugin face: ") + uid);
        auto* fullEditor = card->findChild<QAbstractButton*>("RackDeviceOpenEditor");
        if (!fullEditor || !fullEditor->isVisible() || !fullEditor->isEnabled())
            return fail(QString("built-in full editor remains available in the header: ") + uid);
        const auto fitRack = [&](const QSize& size) {
            mixer.resize(size);
            QEventLoop turn;
            QTimer::singleShot(10, &turn, &QEventLoop::quit);
            turn.exec();
        };
        const auto checkCompact = [&](const char* page) {
            auto* body = card->findChild<QScrollArea*>("RackDeviceBodyScroll");
            const auto record = [&](const QString& detail) {
                const QString message = QString("%1 / %2 at %3x%4: %5")
                    .arg(uid, page).arg(mixer.width()).arg(mixer.height()).arg(detail);
                compactLayoutErrors.push_back(message);
                std::fprintf(stderr, "CLIPPED rack: %s\n", message.toUtf8().constData());
            };
            if (!body) { record("missing body viewport"); return; }
            if (body->verticalScrollBar()->maximum() || body->horizontalScrollBar()->maximum())
                record(QString("unexpected inner scroll %1 x %2")
                    .arg(body->horizontalScrollBar()->maximum()).arg(body->verticalScrollBar()->maximum()));
            for (auto* control : body->widget()->findChildren<QWidget*>()) {
                if (!control->isVisible() ||
                    !(qobject_cast<ui::Knob*>(control) || qobject_cast<QComboBox*>(control) ||
                      qobject_cast<QAbstractButton*>(control) || qobject_cast<QDoubleSpinBox*>(control)))
                    continue;
                const QRect bounds(control->mapTo(body->viewport(), QPoint()), control->size());
                if (!body->viewport()->rect().contains(bounds))
                    record(QString("%1 outside viewport (%2,%3 %4x%5 in %6x%7)")
                        .arg(control->objectName()).arg(bounds.x()).arg(bounds.y())
                        .arg(bounds.width()).arg(bounds.height())
                        .arg(body->viewport()->width()).arg(body->viewport()->height()));
            }
        };
        if (QString::fromUtf8(uid) != "daw.equalizer") {
            for (const QSize size : {QSize(1340, 240), QSize(900, 240), QSize(1340, 260), QSize(1340, 280)}) {
                fitRack(size);
                checkCompact("native");
                if (size == QSize(1340, 240))
                    capture(QString("compact-") + QString::fromUtf8(uid).mid(4));
            }
        }
        capture(QString("native-") + QString::fromUtf8(uid).mid(4));
        if (QString::fromUtf8(uid) == "daw.sampler") {
            fitRack(QSize(1340, 240));
            auto* sampler = card->findChild<RackSamplerView*>();
            if (!sampler || !sampler->checkForTest(error)) return fail(error ? *error : "rack sampler interactions");
            capture("sampler-envelope");
            fitRack(QSize(1340, 280));
            if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_SAMPLER_ONLY")) {
                if (!compactLayoutErrors.isEmpty()) return fail(compactLayoutErrors.join('\n'));
                std::fprintf(stderr, "PASS rack Sampler: native face, sample replacement, envelope gestures, keyboard and compact layout\n");
                return true;
            }
        }
        const auto processor = controller.insertIdentity(gallery, slot);
        card->setPage(RackDeviceCard::Page::Parameters);
        QApplication::processEvents(); card->refresh();
        if (card->findChildren<ui::Knob*>().size() != std::min<std::size_t>(8, controller.insertParameters(gallery, slot).size()))
            return fail(QString("compact parameter controls: ") + uid);
        for (auto* knob : card->findChildren<ui::Knob*>())
            if (knob->isVisible() && !card->rect().contains(QRect(knob->mapTo(card, QPoint()), knob->size())))
                return fail(QString("parameter control clipped: ") + uid + "/" + knob->objectName());
        fitRack(QSize(900, 240));
        checkCompact("parameters");
        fitRack(QSize(1340, 280));
        card->setExpanded(true); QApplication::processEvents();
        if (!card->findChild<QLineEdit*>("RackParameterSearch") || card->width() != 560)
            return fail(QString("searchable expansion: ") + uid);
        card->setExpanded(false);
        card->setPage(RackDeviceCard::Page::Modulation); QApplication::processEvents();
        if (!card->findChild<QAbstractButton*>("RackAddModulation") || controller.insertIdentity(gallery, slot) != processor)
            return fail(QString("modulation page preserves the processor: ") + uid);
        card->setPage(RackDeviceCard::Page::Editor); QApplication::processEvents();
        capture(QString::fromUtf8(uid).mid(4));
        if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_SAMPLER_ONLY")) {
            std::fprintf(stderr, "PASS rack Sampler: launcher, parameter page, searchable expansion and modulation page preserve the loaded sample processor\n");
            return true;
        }
        if (!instrument)
            controller.removeRackSelection(gallery, {slot, neighbour});
    }
    if (!compactLayoutErrors.isEmpty())
        return fail("compact module layout:\n" + compactLayoutErrors.join('\n'));
    const auto insertionTrack = controller.addTrack(daw::TrackKind::Instrument, "Rack insertion");
    rack->setChannel(QString::fromStdString(insertionTrack));
    QApplication::processEvents();
    auto* addDevice = rack->findChild<ui::IconButton*>("RackAddDevice");
    if (!addDevice || rack->findChildren<ui::IconButton*>("RackAddDevice").size() != 1 ||
        addDevice->size() != QSize(32, 32) || addDevice->x() < 8 || addDevice->accessibleName().isEmpty())
        return fail("one compact, accessible add button with an inset on an empty MIDI rack");
    capture("empty");
    const auto browserDrop = [&](const QString& uid, QWidget* target = nullptr) {
        QMimeData payload;
        payload.setData(ui::kPluginDragMime, ui::encodePluginRef(int(daw::plugins::Format::Internal), uid));
        auto* surface = rack->m_scroll->viewport();
        auto* destination = target ? target : rack->findChild<QWidget*>("RackAddDevice");
        rack->m_scroll->ensureWidgetVisible(destination);
        QApplication::processEvents();
        const QPoint at = destination->mapTo(surface, target ? QPoint(4, target->height() / 2)
                                                             : destination->rect().center());
        QDragEnterEvent enter(at, Qt::CopyAction, &payload, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &enter);
        if (!enter.isAccepted()) return false;
        QDropEvent drop(at, Qt::CopyAction, &payload, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &drop);
        rack->sync();
        QApplication::processEvents();
        return drop.isAccepted();
    };
    if (!browserDrop("daw.equalizer") || !browserDrop("daw.sampler"))
        return fail("browser plugins drop into the rack");
    const auto* insertedTrack = controller.project().findTrack(insertionTrack);
    const auto effectId = QString::fromStdString(insertedTrack->inserts.front().id);
    auto* effectCard = rack->m_cards.value(effectId).data();
    auto* instrumentCard = rack->m_cards.value(QString::fromStdString(insertedTrack->instrument.id)).data();
    if (insertedTrack->inserts.size() != 1 || !instrumentCard ||
        instrumentCard->geometry().right() >= effectCard->geometry().left())
        return fail("an instrument added after effects must be the first device");
    if (!browserDrop("daw.slicer") || controller.project().findTrack(insertionTrack)->inserts.size() != 1 ||
        controller.project().findTrack(insertionTrack)->instrument.uid != "daw.slicer" || rack->m_cards.size() != 2)
        return fail("adding another instrument replaces the single instrument slot");
    effectCard->setPage(RackDeviceCard::Page::Parameters);
    effectCard->setExpanded(true);
    rack->m_scroll->ensureWidgetVisible(effectCard);
    QApplication::processEvents();
    if (!effectCard->findChild<QLineEdit*>("RackParameterSearch"))
        return fail("inserted EQ exposes searchable parameters");
    effectCard->setExpanded(false);
    capture("insertion");
    if (!browserDrop("daw.compressor", effectCard) ||
        controller.project().findTrack(insertionTrack)->inserts.front().uid != "daw.compressor")
        return fail("browser effects respect the indicated insertion point");
    for (const auto filter : {std::optional<bool>(false), std::optional<bool>(true), std::optional<bool>{}}) {
        std::unique_ptr<QMenu> picker(ui::buildPluginMenu(rack, &controller, filter, [](const auto&) {}));
        auto* search = picker->findChild<QLineEdit*>("PluginPickerSearch");
        if (!search) return fail("plugin picker search");
        for (const auto* uid : {"daw.sampler", "daw.equalizer"}) {
            search->setText(QString::fromUtf8(uid) == QStringLiteral("daw.equalizer")
                                ? QStringLiteral("EQ") : QString::fromUtf8(uid).mid(4));
            QApplication::processEvents();
            bool found = false;
            for (auto* action : picker->findChildren<QAction*>())
                found |= action->isVisible() && action->property("pluginUid").toString() == uid;
            const bool expected = !filter.has_value() || *filter == (QString::fromUtf8(uid) == "daw.sampler");
            if (found != expected) return fail("mixed and single-kind plugin picker search");
        }
    }
    rack->setChannel(QString::fromStdString(a));
    QApplication::processEvents();
    if (browserDrop("daw.sampler") || browserDrop("missing-plugin"))
        return fail("audio racks reject instruments and stale browser references");
    const auto pattern = controller.addPattern("Drum pattern");
    const auto firstPattern = controller.project().findTrack(pattern)->clips.front().id;
    const auto secondPattern = controller.addPatternClip(pattern, 8.0);
    const auto sampler = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    if (!sampler) return fail("Pattern Sampler descriptor");
    const auto kick = controller.addPatternInstrument(pattern, *sampler, 0.0, firstPattern);
    const auto hat = controller.addPatternInstrument(pattern, *sampler, 8.0, secondPattern);
    controller.renameTrack(kick, "Kick");
    controller.renameTrack(hat, "Hi-hat");
    const auto patternFx = controller.addInsert(pattern, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    // The gallery above opens independent editor windows. Re-establish the
    // keyboard target explicitly after those test windows have been destroyed.
    QApplication::setActiveWindow(&mixer);
    rack->setPattern(QString::fromStdString(pattern), QString::fromStdString(firstPattern));
    QApplication::processEvents();
    auto* module = rack->patternEditor();
    const auto sourceRows = [&] {
        QList<QWidget*> rows;
        for (auto* row : module->findChildren<QWidget*>("PatternSourceRow"))
            if (row->isVisible()) rows.push_back(row);
        return rows;
    };
    auto* patternEffect = rack->m_cards.value(QString::fromStdString(patternFx)).data();
    capture("pattern-initial");
    if (!module || module->isWindow() || !module->isVisible() || !rack->isAncestorOf(module) ||
        sourceRows().size() != 1 || !patternEffect ||
        module->geometry().right() >= patternEffect->geometry().left()) {
        std::fprintf(stderr, "Pattern layout: window=%d visible=%d child=%d rows=%lld right=%d fx=%d clip=%s\n",
            module->isWindow(), module->isVisible(), rack->isAncestorOf(module), qlonglong(sourceRows().size()),
            module->geometry().right(), patternEffect ? patternEffect->geometry().left() : -1,
            module->patternClipId().toUtf8().constData());
        return fail("Pattern is an embedded source module before its bus effects, filtered to the selected clip");
    }
    auto* picker = module->findChild<QComboBox*>("PatternClipSelector");
    if (!picker || picker->count() != 3) return fail("Pattern clip selector");
    picker->setCurrentIndex(0);
    QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 0));
    QApplication::processEvents();
    if (sourceRows().size() != 2) return fail("all MIDI shows sounds from every Pattern clip");
    auto* row = sourceRows().front();
    const auto source = row->property("trackId").toString();
    const auto originalSlot = controller.project().findTrack(source.toStdString())->instrument.id;
    const auto originalNotes = controller.project().findTrack(source.toStdString())->clips.front().notes;
    auto* level = row->findChild<ui::FaderWidget*>("PatternSourceLevel");
    const auto beforeLevel = controller.undoDepth();
    level->setGain(.65);
    QMetaObject::invokeMethod(level, "gainChanged", Q_ARG(double, .65));
    QMetaObject::invokeMethod(level, "editFinished");
    rack->sync();
    if (std::abs(controller.project().findTrack(source.toStdString())->volume - .65) > .001 ||
        controller.undoDepth() != std::min(beforeLevel + 1, controller.undoLimit()) || sourceRows().front() != row)
        return fail("Pattern volume edits the existing sound without recreating rows");
    QString openedTrack, openedClip;
    connect(module, &PatternWindow::openPianoRollRequested, &mixer,
        [&](const QString& track, const QString& clip) { openedTrack = track; openedClip = clip; });
    row->findChild<QAbstractButton*>("PatternSourceSketch")->click();
    if (openedTrack != source || openedClip != QString::fromStdString(controller.project().findTrack(source.toStdString())->clips.front().id))
        return fail("Pattern MIDI opens the individual editable clip");
    row->findChild<QAbstractButton*>("PatternSourceRack")->click();
    QApplication::processEvents();
    if (rack->channelId() != source || module->isVisible() || !rack->m_patternBack->isVisible() ||
        !rack->m_cards.contains(QString::fromStdString(originalSlot)))
        return fail("sound rack opens its instrument and retains a return to Pattern");
    rack->m_patternBack->click();
    QApplication::processEvents();
    if (rack->channelId() != QString::fromStdString(pattern) || !module->isVisible())
        return fail("return from sound rack");
    rack->setPattern(QString::fromStdString(pattern), QString::fromStdString(secondPattern));
    QApplication::processEvents();
    QMimeData sound;
    sound.setUrls({QUrl::fromLocalFile(samplePath)});
    const auto dropAt = sourceRows().front()->mapTo(module, sourceRows().front()->rect().center());
    const auto tracksBefore = controller.project().tracks.size(), beforeDrop = controller.undoDepth();
    QDragEnterEvent enterSound(dropAt, Qt::CopyAction, &sound, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(module, &enterSound);
    QDropEvent dropSound(dropAt, Qt::CopyAction, &sound, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(module, &dropSound);
    QApplication::processEvents();
    const auto newSource = module->selectedSourceTrackId().toStdString();
    const auto* added = controller.project().findTrack(newSource);
    if (!enterSound.isAccepted() || !dropSound.isAccepted() || !added ||
        controller.project().tracks.size() != tracksBefore + 1 ||
        controller.undoDepth() != std::min(beforeDrop + 1, controller.undoLimit()) ||
        added->parentId != pattern || added->outputBusId != pattern || added->instrument.uid != "daw.sampler" ||
        added->clips.empty() || added->clips.front().patternClipId != secondPattern ||
        controller.project().findTrack(source.toStdString())->instrument.id != originalSlot ||
        controller.project().findTrack(source.toStdString())->clips.front().notes != originalNotes)
        return fail("audio dropped on a Pattern row adds a routed sound in the selected clip without replacing another sound");
    module->setFocus();
    QApplication::setActiveWindow(&mixer);
    if (!rack->command("undo")) return fail("Pattern undo routing");
    QApplication::processEvents();
    if (controller.project().findTrack(newSource) || controller.project().tracks.size() != tracksBefore)
        return fail("Pattern audio drop undoes in one step");
    if (!rack->command("redo")) return fail("Pattern redo routing");
    QApplication::processEvents();
    if (!controller.project().findTrack(newSource)) return fail("Pattern audio drop redo");
    QMimeData instrument;
    instrument.setData(ui::kPluginDragMime,
        ui::encodePluginRef(int(daw::plugins::Format::Internal), "daw.sampler"));
    const auto instrumentAt = sourceRows().front()->mapTo(module, QPoint(10, 4));
    QDragEnterEvent enterInstrument(instrumentAt, Qt::CopyAction, &instrument, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(module, &enterInstrument);
    QDropEvent dropInstrument(instrumentAt, Qt::CopyAction, &instrument, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(module, &dropInstrument);
    const auto droppedInstrument = module->selectedSourceTrackId().toStdString();
    const auto* droppedTrack = controller.project().findTrack(droppedInstrument);
    if (!enterInstrument.isAccepted() || !dropInstrument.isAccepted() || !droppedTrack ||
        droppedTrack->parentId != pattern || droppedTrack->clips.front().patternClipId != secondPattern)
        return fail("instrument dropped directly on the Pattern module creates a sound in its selected MIDI clip");
    controller.undo();
    rack->sync();
    QApplication::processEvents();
    if (controller.project().findTrack(droppedInstrument) || !controller.project().findTrack(newSource))
        return fail("inserting a Pattern instrument undoes in one step even at the history limit");
    if (!browserDrop("daw.sampler") || module->selectedSourceTrackId().isEmpty())
        return fail("browser instrument drop adds a new Pattern sound");
    rack->selectDevices({QString::fromStdString(patternFx)});
    module->setFocus();
    QKeyEvent sourceDelete(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    const auto deleted = module->selectedSourceTrackId().toStdString();
    const auto deletedClip = controller.project().findTrack(deleted)->clips.front().id;
    QApplication::sendEvent(module, &sourceDelete);
    QApplication::processEvents();
    const auto hasDeletedClip = [&] {
        const auto* track = controller.project().findTrack(deleted);
        return track && std::any_of(track->clips.begin(), track->clips.end(),
            [&](const auto& clip) { return clip.id == deletedClip; });
    };
    // A specific Pattern clip removes only its part; All MIDI removes the
    // whole source. The module retains the existing per-clip editing semantics.
    if (hasDeletedClip() || !controller.project().findTrack(deleted) ||
        !controller.insertModel(pattern, patternFx))
        return fail("Delete in a Pattern clip removes the selected MIDI part, preserving the instrument and bus effects");
    capture("pattern");
    mixer.resize(860, 280);
    QApplication::processEvents();
    auto* patternScroll = module->findChild<QScrollArea*>("PatternScroll");
    if (module->width() > 620 || patternScroll->horizontalScrollBar()->maximum() != 0 ||
        module->height() > rack->m_scroll->viewport()->height())
        return fail("compact Pattern module fits the rack height and scrolls vertically");
    capture("pattern-narrow");
    mixer.resize(1340, 320);
    controller.undo();
    rack->sync();
    QApplication::processEvents();
    if (!hasDeletedClip()) return fail("Pattern source deletion undo");
    controller.removeTrack(pattern);
    rack->sync();
    QApplication::processEvents();
    if (module->isVisible()) return fail("deleted Pattern leaves no stale rack module");
    const auto folder = controller.addTrack(daw::TrackKind::Folder, "Folder");
    rack->setChannel(QString::fromStdString(folder));
    QApplication::processEvents();
    if (rack->m_left->isVisible() || rack->m_sends->isVisible())
        return fail("non-audio track empty state");
    rack->setChannel(daw::EngineController::kMasterChannelId);
    QApplication::processEvents();
    if (rack->m_sends->isVisible())
        return fail("master has no sends");
    mixer.hide();
    if (rack->m_timer->isActive())
        return fail("hidden rack polls telemetry");
    std::fprintf(
        stderr,
        "PASS rack UI: groups, focus, virtual controls, synchronization, narrow layout, browser plugins, single instrument, mixed picker, plugin pages, hidden telemetry\n");
    return true;
}
