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
#include "RackBuiltinView.hpp"
#include "RackSamplerView.hpp"
#include "RackDeviceCard.hpp"
#include "RackDrag.hpp"
#include "RackParameterBinding.hpp"
#include "UtilityPanel.hpp"
#include "RackWidget.hpp"
#include "RackModulationPanel.hpp"
#include "RackModulationEditor.hpp"
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
    if (!eqCard || eqCard->width() != 420)
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
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_MODULATION_ONLY")) {
        mixer.resize(1340, 370);
        QTimer::singleShot(0, [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                for (auto* action : menu->actions()) if (action->text() == "LFO") { action->trigger(); menu->close(); break; }
        });
        rack->addDevice(rack->mapToGlobal(QPoint(20, 20)));
        if (controller.rackModulation(a).sources.empty()) return fail("LFO is available in the ordinary Rack add menu");
        const auto source = controller.rackModulation(a).sources.front().id;
        const auto follower = controller.addRackModulator(a, daw::modulation::Kind::Follower);
        rack->m_modulation->setChannel(QString::fromStdString(a));
        QApplication::processEvents(); rack->refresh(); QApplication::processEvents();
        auto* control = eqCard->findChild<ui::Knob*>("RackParameter.output.gain");
        if (!control) {
            for (auto* knob : eqCard->findChildren<ui::Knob*>())
                if (knob->property("modulationEligible").toBool()) { control = knob; break; }
        }
        if (!control || !control->isVisible() || source.empty() || follower.empty()) return fail("modulation test controls available");
        const auto parameter = control->property("modulationParameter").toString().toStdString();
        const auto before = controller.insertParameter(a, eq, parameter);
        const auto begin = [&] {
            auto* assign = rack->m_modulation->findChild<QToolButton*>("ModulationAssign");
            if (!assign || !assign->isEnabled()) return false;
            assign->click(); QApplication::processEvents(); return rack->m_modulation->assigning();
        };
        if (!begin()) return fail("assignment enters selection mode");
        const QPointF point(control->rect().center());
        QMouseEvent press(QEvent::MouseButtonPress, point, QPointF(control->mapToGlobal(point.toPoint())), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, QPointF(control->mapToGlobal(point.toPoint())), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(control, &press); QApplication::sendEvent(control, &release);
        QApplication::processEvents(); rack->refresh();
        if (rack->m_modulation->assigning() || controller.rackModulation(a).routes.size() != 1 || controller.insertParameter(a, eq, parameter) != before)
            return fail("assignment click creates a route without changing the knob base");
        const auto undo = controller.undoDepth();
        if (!begin()) return fail("assignment restarts");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(control, &escape);
        if (rack->m_modulation->assigning() || controller.undoDepth() != undo) return fail("Escape cancels without Undo");
        if (!begin()) return fail("keyboard assignment starts");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(control, &enter);
        if (controller.rackModulation(a).routes.size() != 1 || controller.undoDepth() != undo) return fail("repeat keyboard assignment reuses route");
        audio::AudioBuffer input(2, 256), output(2, 256);
        controller.seekSeconds(.125); controller.play();
        for (int i = 0; i < 64; ++i) {
            if (!controller.processDeviceBlockForTest(input, output, 256)) return fail("modulation UI audio processing");
            rack->refresh(); QApplication::processEvents();
        }
        if (rack->m_modulation->parentWidget() != rack->m_chain || rack->m_modulation->findChildren<QWidget*>("RackModulatorCard").size() != 2)
            return fail("sources are individual cards inside the device chain");
        capture("modulation");
        bool parameterSearch = false;
        QTimer::singleShot(0, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                auto* search = dialog->findChild<QLineEdit*>(); auto* list = dialog->findChild<QListWidget*>();
                if (search && list) { search->setText("Frequency"); for (int i = 0; i < list->count(); ++i) parameterSearch |= !list->item(i)->isHidden(); }
                dialog->reject();
            }
        });
        rack->m_modulation->findChild<QToolButton*>("ModulationParameters")->click();
        if (!parameterSearch || controller.undoDepth() != undo) return fail("searchable parameter list cancels without changes");
        const auto collapse = [&] { rack->m_modulation->findChild<ui::IconButton*>("ModulationFold")->click(); QApplication::processEvents(); };
        collapse();
        const auto phase = controller.rackModulationValue(a, source);
        for (int i = 0; i < 20; ++i) controller.processDeviceBlockForTest(input, output, 256);
        if (controller.rackModulationValue(a, source) == phase) return fail("collapsed module continues processing");
        collapse();
        mixer.resize(700, 380); QApplication::processEvents(); capture("modulation-narrow");
        if (rack->m_scroll->horizontalScrollBar()->maximum() <= 0) return fail("narrow Rack scrolls to all module cards");
        mixer.hide(); const auto hiddenPhase = controller.rackModulationValue(a, source);
        for (int i = 0; i < 20; ++i) controller.processDeviceBlockForTest(input, output, 256);
        if (controller.rackModulationValue(a, source) == hiddenPhase) return fail("hidden Rack keeps processing");
        mixer.show(); QApplication::processEvents();
        controller.stop();
        controller.undo(); rack->refresh(); QApplication::processEvents();
        if (!controller.rackModulation(a).routes.empty()) return fail("assignment Undo after audio processing");
        controller.redo(); rack->refresh(); QApplication::processEvents();
        if (controller.rackModulation(a).routes.size() != 1) return fail("assignment Redo");
        const auto deviceCount = controller.channelInserts(a)->size();
        rack->selectDevices({QString::fromStdString(eq)});
        auto* sourceTitle = rack->m_modulation->findChild<QToolButton*>("ModulationSourceTitle");
        sourceTitle->setFocus();
        QKeyEvent duplicateKey(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier); QApplication::sendEvent(sourceTitle, &duplicateKey);
        QApplication::processEvents(); rack->refresh(); QApplication::processEvents();
        if (controller.rackModulation(a).sources.size() != 3 || controller.channelInserts(a)->size() != deviceCount)
            return fail("duplicate shortcut acts on the focused module, not a stale device selection");
        controller.undo(); rack->refresh(); QApplication::processEvents();
        sourceTitle = rack->m_modulation->findChild<QToolButton*>("ModulationSourceTitle"); sourceTitle->setFocus();
        QKeyEvent deleteKey(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier); QApplication::sendEvent(sourceTitle, &deleteKey);
        QApplication::processEvents(); rack->refresh(); QApplication::processEvents();
        if (controller.rackModulation(a).sources.size() != 1 || !controller.hasInsert(a, eq))
            return fail("Delete acts on the focused module and preserves the plugin");
        controller.undo();
        rack->refresh(); QApplication::processEvents();
        std::vector<std::string> newSources;
        for (auto kind : {daw::modulation::Kind::Shaper, daw::modulation::Kind::Steps, daw::modulation::Kind::Random}) {
            newSources.push_back(controller.addRackModulator(a, kind));
            if (newSources.back().empty()) return fail("new source creation");
        }
        rack->refresh(); QApplication::processEvents(); rack->refresh(); QApplication::processEvents();
        const auto cardFor = [&](const std::string& sourceId) -> QWidget* {
            for (auto* card : rack->m_modulation->findChildren<QWidget*>("RackModulatorCard"))
                if (card->property("sourceId").toString().toStdString() == sourceId) return card;
            return nullptr;
        };
        const auto graphFor = [&](QWidget* parent) -> RackModulationEditor* {
            if (parent) for (auto* widget : parent->findChildren<QWidget*>("ModulationGraph"))
                if (auto* editor = dynamic_cast<RackModulationEditor*>(widget)) return editor;
            return nullptr;
        };
        if (rack->m_modulation->findChildren<QWidget*>("RackModulatorCard").size() != 5) return fail("all five module kinds have individual cards");
        mixer.resize(1340, 370); QApplication::processEvents();
        for (auto* card : rack->m_modulation->findChildren<QWidget*>("RackModulatorCard")) {
            card->setFixedHeight(220); QApplication::processEvents();
            auto* area = card->findChild<QScrollArea*>("ModulationAssignments");
            if (!area || area->viewport()->height() < 140) return fail("every module reserves five route rows at 220px");
            auto* controls = graphFor(card)->parentWidget();
            for (auto* control : controls->findChildren<QWidget*>())
                if (control->isVisible() && !control->isWindow() && !card->rect().contains(QRect(control->mapTo(card, QPoint()), control->size())))
                    return fail(QString("controls fit the minimum card: %1").arg(card->accessibleName()));
            for (auto* combo : controls->findChildren<QComboBox*>()) if (combo->isVisible() && !combo->currentText().isEmpty()) {
                QStyleOptionComboBox option; option.initFrom(combo); option.currentText = combo->currentText(); option.frame = combo->hasFrame();
                const auto textRect = combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
                if (combo->fontMetrics().horizontalAdvance(combo->currentText()) > textRect.width())
                    return fail(QString("closed combo label fits: %1 (%2 px available)").arg(combo->currentText()).arg(textRect.width()));
            }
            card->setMaximumHeight(QWIDGETSIZE_MAX); card->setMinimumHeight(220); QApplication::processEvents();
        }
        const auto paintGesture = [&](RackModulationEditor* graph) {
            graph->window()->activateWindow(); QApplication::setActiveWindow(graph->window());
            graph->setFocus();
            const QPointF from(graph->width() * .25, graph->height() * .6), to(graph->width() * .68, graph->height() * .25);
            const auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
                QMouseEvent event(type, at, QPointF(graph->mapToGlobal(at.toPoint())), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(graph, &event);
            };
            mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 16; ++i) mouse(QEvent::MouseMove, from + (to - from) * (i / 16.), Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
            rack->refresh(); QApplication::processEvents();
        };
        for (int i = 0; i < 2; ++i) {
            auto* card = cardFor(newSources[i]); auto* graph = graphFor(card);
            if (!card || !graph || card->width() != 560) return fail("horizontal card geometry and editable graph");
            rack->m_scroll->ensureWidgetVisible(card); QApplication::processEvents();
            const auto before = controller.rackModulation(a);
            const auto depth = controller.undoDepth(); QPointer<QWidget> originalGraph(graph);
            QElapsedTimer drawingTimer; drawingTimer.start();
            paintGesture(graph);
            std::fprintf(stderr, "BENCH %s 16-move draw gesture: %.3f ms\n", i == 0 ? "Shaper" : "Steps", drawingTimer.nsecsElapsed() / 1000000.);
            if (controller.rackModulation(a) == before || controller.undoDepth() != std::min(depth + 1, controller.undoLimit())) return fail("drawing is one undoable gesture");
            if (originalGraph.isNull() || graphFor(cardFor(newSources[i])) != graph || QApplication::focusWidget() != graph)
                return fail(QString("drawing preserves editor and focus: alive=%1 same=%2 focus=%3")
                    .arg(!originalGraph.isNull()).arg(graphFor(cardFor(newSources[i])) == graph)
                    .arg(QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : "none"));
            controller.undo(); rack->refresh(); QApplication::processEvents();
            if (controller.rackModulation(a) != before) return fail("one Undo restores the entire shape gesture");
            controller.redo(); rack->refresh(); QApplication::processEvents();
            const auto beforeKeys = controller.rackModulation(a); const auto keyDepth = controller.undoDepth();
            for (int repeat = 0; repeat < 8; ++repeat) {
                QKeyEvent key(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier, {}, repeat > 0);
                QApplication::sendEvent(graph, &key);
            }
            QKeyEvent keyRelease(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier); QApplication::sendEvent(graph, &keyRelease);
            if (controller.undoDepth() != std::min(keyDepth + 1, controller.undoLimit())) return fail("held editing key is one Undo");
            controller.undo(); rack->refresh(); QApplication::processEvents();
            if (controller.rackModulation(a) != beforeKeys) return fail("keyboard gesture Undo restores values");
            QKeyEvent deletePoint(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier); QApplication::sendEvent(graph, &deletePoint);
            QKeyEvent deleteRelease(QEvent::KeyRelease, Qt::Key_Delete, Qt::NoModifier); QApplication::sendEvent(graph, &deleteRelease);
            rack->refresh(); QApplication::processEvents();
            if (controller.rackModulation(a).sources.size() != 5 || controller.rackModulation(a) == beforeKeys)
                return fail("Delete edits the focused graph without removing its module");
            controller.undo(); rack->refresh(); QApplication::processEvents();
            if (controller.rackModulation(a) != beforeKeys) return fail("Undo restores the deleted graph value");
            auto* expand = card->findChild<ui::IconButton*>("ModulationExpand"); if (!expand) return fail("enlarge editor action");
            expand->click(); QApplication::processEvents();
            auto* dialog = rack->m_modulation->findChild<QDialog*>("ModulationExpandedEditor");
            auto* large = graphFor(dialog);
            if (!dialog || dialog->isModal() || !large) return fail("enlarged editor is nonmodal");
            paintGesture(large);
            if (large->source() != graph->source()) return fail("inline and enlarged editor share the same source");
            if (!screenshot.isEmpty()) dialog->grab().save(screenshot.left(screenshot.lastIndexOf('.')) + (i == 0 ? "-shaper-editor.png" : "-steps-editor.png"));
            dialog->close(); QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents();
            mixer.activateWindow(); QApplication::setActiveWindow(&mixer);
        }
        const auto parameters = controller.insertParameters(a, eq);
        std::vector<std::string> eligible;
        for (const auto& info : parameters) if (info.isAutomatable && !info.isReadOnly && info.minValue < info.maxValue) eligible.push_back(info.id);
        if (eligible.size() < 32) return fail("assignment density fixture");
        const auto routeId = controller.assignRackModulation(a, newSources[0], eq, eligible[0]);
        auto routed = controller.rackModulation(a);
        const auto prototype = *std::find_if(routed.routes.begin(), routed.routes.end(), [&](const auto& r) { return r.id == routeId; });
        std::erase_if(routed.routes, [&](const auto& r) { return r.sourceId == newSources[0]; });
        for (unsigned count : {0, 1, 5, 8, 32}) {
            auto next = routed;
            for (unsigned i = 0; i < count; ++i) { auto route = prototype; route.id = daw::newUuid(); route.parameterId = eligible[i]; next.routes.push_back(route); }
            if (!controller.setRackModulation(a, next)) return fail("assignment density setup");
            rack->refresh(); QApplication::processEvents(); rack->refresh(); QApplication::processEvents();
            auto* card = cardFor(newSources[0]); rack->m_scroll->ensureWidgetVisible(card); QApplication::processEvents();
            const auto rows = card->findChildren<QWidget*>("ModulationRoute");
            if (rows.size() != count) return fail("all assignment rows are present");
            for (auto* row : rows) if (row->height() != 28) return fail("assignment row is 28 logical pixels");
            auto* area = card->findChild<QScrollArea*>("ModulationAssignments");
            if (count == 5) {
                card->setFixedHeight(220); QApplication::processEvents();
                if (area->viewport()->height() < 140 || area->verticalScrollBar()->maximum() != 0) return fail("five connections fit minimum card height");
                if (!screenshot.isEmpty()) card->grab().save(screenshot.left(screenshot.lastIndexOf('.')) + "-minimum-five.png");
                card->setMaximumHeight(QWIDGETSIZE_MAX); card->setMinimumHeight(220); QApplication::processEvents();
            }
            if (count == 32) {
                area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum() / 2);
                QApplication::processEvents();
                QDoubleSpinBox* spin = nullptr;
                for (auto* field : card->findChildren<QDoubleSpinBox*>("ModulationDepth"))
                    if (area->viewport()->rect().contains(field->mapTo(area->viewport(), field->rect().center()))) { spin = field; break; }
                if (!spin) return fail("scrolled connection has a visible depth control");
                QPointer<QDoubleSpinBox> original(spin); spin->setFocus(); QApplication::processEvents();
                const int scroll = area->verticalScrollBar()->value();
                spin->setValue(35); QMetaObject::invokeMethod(spin, "editingFinished");
                rack->refresh(); QApplication::processEvents();
                if (original.isNull() || area->verticalScrollBar()->value() != scroll || QApplication::focusWidget() != spin)
                    return fail(QString("assignment edit stability: alive=%1 scroll=%2/%3 focus=%4")
                        .arg(!original.isNull()).arg(area->verticalScrollBar()->value()).arg(scroll)
                        .arg(QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : "none"));
            }
            if (!screenshot.isEmpty()) card->grab().save(screenshot.left(screenshot.lastIndexOf('.')) + QString("-connections-%1.png").arg(count));
        }
        for (const auto& id : {source, follower, newSources[0], newSources[1], newSources[2]}) {
            auto* card = cardFor(id); rack->m_scroll->ensureWidgetVisible(card); QApplication::processEvents();
            if (!screenshot.isEmpty()) card->grab().save(screenshot.left(screenshot.lastIndexOf('.')) + "-" + QString::fromStdString(graphFor(card)->source().name).toLower() + ".png");
        }
        {
            const auto beforePreset = controller.rackModulation(a); const auto depth = controller.undoDepth();
            auto* presets = cardFor(newSources[0])->findChild<QComboBox*>("ModulationShapePreset");
            presets->setCurrentIndex(4); QMetaObject::invokeMethod(presets, "activated", Q_ARG(int, 4));
            if (controller.rackModulation(a).routes != beforePreset.routes || controller.rackModulation(a) == beforePreset ||
                controller.undoDepth() != std::min(depth + 1, controller.undoLimit())) return fail("shape preset preserves routes in one Undo");
            controller.undo(); rack->refresh(); QApplication::processEvents();
            if (controller.rackModulation(a) != beforePreset) return fail("shape preset Undo restores the complete source");
        }
        {
            const auto beforePattern = controller.rackModulation(a); const auto depth = controller.undoDepth();
            bool generated = false;
            QTimer::singleShot(0, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); if (!menu) return;
                menu->setActiveAction(menu->actions().back());
                QTimer::singleShot(0, [&] {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); if (!dialog) return;
                    const auto fields = dialog->findChildren<QSpinBox*>(); if (fields.size() != 2) { dialog->reject(); return; }
                    fields[0]->setValue(5); fields[1]->setValue(3); generated = true; dialog->accept();
                });
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(menu, &enter);
            });
            cardFor(newSources[1])->findChild<QToolButton*>("ModulationPattern")->click();
            const auto& values = controller.rackModulation(a).sources[3].steps;
            if (!generated || std::count(values.begin(), values.end(), 1.) != 5 || controller.rackModulation(a).routes != beforePattern.routes ||
                controller.undoDepth() != std::min(depth + 1, controller.undoLimit())) return fail("Euclidean generator applies one undoable pattern without changing routes");
            controller.undo(); rack->refresh(); QApplication::processEvents();
            if (controller.rackModulation(a) != beforePattern) return fail("Euclidean pattern Undo restores previous steps");
        }
        auto* randomCard = cardFor(newSources[2]); auto* reseed = randomCard->findChild<QToolButton*>("ModulationReseed");
        const auto seedBefore = graphFor(randomCard)->source().seed;
        reseed->click(); rack->refresh(); QApplication::processEvents();
        if (graphFor(randomCard)->source().seed == seedBefore) return fail("Random reseed produces a new pattern");
        controller.undo(); rack->refresh(); QApplication::processEvents();
        if (graphFor(randomCard)->source().seed != seedBefore) return fail("Random seed is undoable");
        QElapsedTimer timer; timer.start();
        controller.seekSeconds(0); controller.play();
        for (int i = 0; i < 200; ++i) { controller.processDeviceBlockForTest(input, output, 256); rack->refresh(); QApplication::processEvents(); }
        controller.stop();
        std::fprintf(stderr, "BENCH modulation Rack refresh + 256-frame process: %.3f ms/iteration\n", timer.nsecsElapsed() / 200000000.);
        if (!screenshot.isEmpty()) randomCard->grab().save(screenshot.left(screenshot.lastIndexOf('.')) + "-random.png");
        mixer.resize(700, 380); QApplication::processEvents(); capture("five-modules-narrow");
        if (rack->m_scroll->horizontalScrollBar()->maximum() <= 0) return fail("five modules remain horizontally reachable");
        std::fprintf(stderr, "PASS modulation modules UI: five kinds, horizontal cards, 28px routes, shape gestures, one Undo, nonmodal editor, persistent focus/scroll, density 0/1/5/8/32, reseed\n");
        return true;
    }
    {
        auto* display = eqCard->findChild<QWidget*>("RackPlot");
        rack->refresh();
        const auto analyzerEnabled = [&] {
            const auto state = controller.equalizerSnapshot(a, eq);
            return state && state->analyzer.enabled;
        };
        if (!display || !analyzerEnabled()) return fail("visible rack starts the EQ analyzer");
        eqCard->hide();
        if (analyzerEnabled()) return fail("hidden EQ views stop analyzer processing");
        EqualizerPanel editor(&controller, QString::fromStdString(a), QString::fromStdString(eq));
        editor.resize(1080, 640);
        editor.show();
        QApplication::processEvents();
        if (!analyzerEnabled()) return fail("full editor starts analyzer while rack is hidden");
        eqCard->show();
        editor.hide();
        if (!analyzerEnabled()) return fail("closing editor must keep rack spectrum live");

        auto config = controller.equalizerSnapshot(a, eq)->analyzer;
        config.pre = config.post = true;
        config.frozen = false;
        controller.setEqualizerAnalyzer(a, eq, config);
        controller.setTrackInputRouting(a, 0, 1, true);
        controller.setTrackMonitor(a, true);
        rack->refresh();
        const auto silent = display->grab().toImage();
        audio::AudioBuffer input(2, 256), output(2, 256);
        unsigned sample = 0;
        std::uint32_t noise = 42;
        const auto feed = [&](double frequency) {
            for (int block = 0; block < 64; ++block) {
                for (unsigned i = 0; i < 256; ++i, ++sample) {
                    const double phase = 6.283185307179586 * sample / 48000.;
                    noise = noise * 1664525u + 1013904223u;
                    input.getChannel(0)[i] = input.getChannel(1)[i] = float(
                        .18 * std::sin(phase * frequency) + .10 * std::sin(phase * 2400.) +
                        .06 * std::sin(phase * 6200.) + .025 * (double(noise) / 2147483648. - 1.));
                }
                if (!controller.processDeviceBlockForTest(input, output, 256)) return false;
            }
            rack->refresh();
            QApplication::processEvents();
            return true;
        };
        if (!feed(180.)) return fail("rack spectrum audio callback");
        const auto active = controller.equalizerSnapshot(a, eq);
        if (!active || *std::max_element(active->telemetry.pre.begin(), active->telemetry.pre.end()) < -40.f ||
            display->grab().toImage() == silent)
            return fail("rack spectrum must paint incoming audio without opening the editor");
        capture("eq-spectrum");
        if (!feed(880.) || controller.equalizerSnapshot(a, eq)->telemetry.pre == active->telemetry.pre)
            return fail("rack spectrum must update when incoming frequencies change");
        controller.setTrackMonitor(a, false);
        eqCard->hide();
        if (analyzerEnabled()) return fail("last hidden EQ view releases analyzer");
        eqCard->show();
    }
    mixer.activateWindow();
    QApplication::setActiveWindow(&mixer);
    rack->setFocus();
    if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_EQ_ONLY")) {
        namespace eqp = daw::plugins::equalizer;
        for (unsigned band = 0; band < eqp::kBandCount; ++band)
            controller.setInsertParameter(a, eq,
                eqp::parameterId(eqp::bandParameter(band, eqp::BandParam::Enabled)), 0.);
        controller.pumpPreviewPluginEvents();
        rack->refresh();
        auto* view = eqCard->findChild<RackBuiltinView*>();
        if (!view || !view->checkEqualizerForTest(error))
            return fail(error ? *error : "rack EQ gestures");
        std::fprintf(stderr, "PASS rack EQ: spectrum, neutral cuts, manual Q, drag and undo\n");
        return true;
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
        auto* open = card->findChild<ui::IconButton*>("RackDeviceOpen");
        auto* fold = card->findChild<ui::IconButton*>("RackDeviceFold");
        if (!header || !body || !footer || !power || !open || !fold || !fold->isHidden())
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
        QObject::connect(card, &RackDeviceCard::editorRequested, &receiver,
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
        if (open->y() <= power->y() || fold->y() <= open->y()) return false;
        for (auto* button : header->findChildren<ui::IconButton*>())
            if (button->isVisible() && !card->rect().contains(QRect(button->mapTo(card, QPoint()), button->size())))
                return false;
        capture("collapsed-" + suffix);
        power->click();
        if (!controller.insertModel(a, slot)->bypassed || !card->collapsed()) return false;
        power->click();
        open->click();
        if (editorRequests != 1 || !card->collapsed()) return false;
        {
            RackDeviceCard restored(&controller, QString::fromStdString(a), card->slotId());
            if (!restored.collapsed() || restored.width() != 36) return false;
        }
        doubleClick();
        if (card->collapsed() || !fold->isHidden() || expand->isHidden() != expandHidden || card->width() != width || body->isHidden() || footer->isHidden() ||
            card->expanded() != expanded || editorRequests != 1 ||
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
        const bool universal = QStringList{"daw.1073", "daw.cla2a", "daw.gravity"}.contains(QString::fromUtf8(uid));
        if (!card || card->width() != (universal ? 288 : RackBuiltinView::preferredWidth(uid)))
            return fail(QString("native width: ") + uid);
        if (universal && (card->findChild<RackBuiltinView*>() || card->findChildren<ui::Knob*>().size() !=
                std::min<std::size_t>(8, controller.insertParameters(gallery, slot).size())))
            return fail(QString("universal compact parameter card: ") + uid);
        // A card can require more than the requested 280 px. The panel must
        // propagate that minimum without growing beyond it or clipping controls.
        if (mixer.height() > std::max(280, mixer.minimumSizeHint().height()) || card->mapTo(rack->m_scroll->viewport(), QPoint()).y() + card->height() >
                                         rack->m_scroll->viewport()->height())
            return fail(QString("native group exceeds minimum panel height: %1 (panel %2, card bottom %3, viewport %4, chain min %5, hint %6, scroll min %7)")
                .arg(uid).arg(mixer.height()).arg(card->mapTo(rack->m_scroll->viewport(), QPoint()).y() + card->height())
                .arg(rack->m_scroll->viewport()->height()).arg(rack->m_chain->minimumHeight())
                .arg(rack->m_chain->minimumSizeHint().height()).arg(rack->m_scroll->minimumHeight()));
        rack->m_scroll->ensureWidgetVisible(card, 0, 0);
        rack->refresh();
        QApplication::processEvents();
        for (auto* panel : card->findChildren<UtilityPanel*>()) panel->refresh();
        for (auto* knob : card->findChildren<ui::Knob*>())
            if (knob->isVisible() && ((!knob->isEnabled() &&
                    !(QString::fromUtf8(uid) == "daw.reverse" && knob->objectName() == "RackParameter.timeMs")) || !card->rect().contains(QRect(
                                                                knob->mapTo(card, QPoint()), knob->size()))))
                return fail(QString("native control unavailable or clipped: ") + uid + "/" +
                            knob->objectName());
        if (QString::fromUtf8(uid) == "daw.sampler") {
            auto* sampler = card->findChild<RackSamplerView*>();
            capture("sampler");
            if (!sampler || !sampler->checkForTest(error)) return fail(error ? *error : "rack sampler interactions");
            capture("sampler-envelope");
            sampler->findChild<QAbstractButton*>("RackSamplerEnvelope")->click();
            rack->refresh();
            if (qEnvironmentVariableIsSet("DAW_SELFTEST_RACK_SAMPLER_ONLY")) {
                capture("sampler");
                std::fprintf(stderr, "PASS rack Sampler: file drops, replacement undo/redo, envelope gestures, keyboard, bounds, layout\n");
                return true;
            }
        }
        if (QString::fromUtf8(uid) == "daw.reverb") {
            auto* mode = card->findChild<QComboBox*>("RackReverbSpace");
            auto* mix = card->findChild<ui::Knob*>("RackParameter.mix");
            if (!mode || mode->count() != 6 || !mix) return fail("Reverb compact space and Mix controls");
            mix->editValue(41);
            if (controller.insertParameter(gallery, slot, "mix") != 41) return fail("Reverb rack Mix reaches instance");
            controller.undo();card->refresh();
            if (mix->value() != 25) return fail("Reverb rack Mix follows undo");
            controller.setInsertParameter(gallery, slot, "mode", 3);card->refresh();
            if (mode->currentIndex() != 3) return fail("Reverb rack space follows editor changes");
            mode->setCurrentIndex(0);QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 0));
            if (controller.insertParameter(gallery, slot, "mode") != 0) return fail("Reverb rack space selection reaches instance");
        }
        if (QString::fromUtf8(uid) == "daw.compressor") {
            auto* display = card->findChild<QWidget*>("RackCompressorDisplay");
            auto* threshold = card->findChild<QSlider*>("RackParameter.threshold");
            auto* readout = card->findChild<QLabel*>("RackParameterValue.threshold");
            if (!display || !threshold || !readout || display->width() != display->height())
                return fail("compressor display stays square at the minimum rack height");
            for (const auto* id : {"threshold", "ratio", "knee", "attack", "release", "makeup"}) {
                auto* slider = card->findChild<QSlider*>(QStringLiteral("RackParameter.") + id);
                if (!slider || slider->height() < 24 || slider->accessibleName().isEmpty() ||
                    slider->mapTo(card, QPoint()).x() <= display->mapTo(card, display->rect().topRight()).x() ||
                    !card->rect().contains(QRect(slider->mapTo(card, QPoint()), slider->size())))
                    return fail("compressor sliders fit to the right of the display");
            }
            const double original = controller.insertParameter(gallery, slot, "threshold");
            const QString originalReading = readout->text();
            const auto undoMark = controller.undoDepth();
            const auto pointer = [&](QEvent::Type type, double fraction) {
                const QPointF at(threshold->width() * fraction, threshold->height() / 2.);
                QMouseEvent event(type, at, threshold->mapToGlobal(at),
                    type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                    type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(threshold, &event);
            };
            pointer(QEvent::MouseButtonPress, .25);
            pointer(QEvent::MouseMove, .45);
            const double edited = controller.insertParameter(gallery, slot, "threshold");
            if (edited == original || controller.undoDepth() != undoMark)
                return fail("compressor slider updates audio continuously without splitting undo");
            pointer(QEvent::MouseButtonRelease, .45);
            if (controller.undoDepth() != std::min(undoMark + 1, controller.undoLimit()))
                return fail("compressor slider drag commits once");
            controller.undo();
            card->refresh();
            if (controller.insertParameter(gallery, slot, "threshold") != original ||
                threshold->value() != int(std::lround(original * 10)) || readout->text() != originalReading)
                return fail("compressor slider and readout follow undo");
            controller.redo();
            card->refresh();
            if (controller.insertParameter(gallery, slot, "threshold") != edited ||
                threshold->value() != int(std::lround(edited * 10)))
                return fail("compressor slider follows redo");
            controller.undo();
            card->refresh();
            mixer.resize(1340, 340);
            QApplication::processEvents();
            capture("compressor-tall");
            mixer.resize(1340, 280);
            QApplication::processEvents();
            if (mixer.height() != std::max(280, mixer.minimumSizeHint().height()) || display->width() != display->height())
                return fail("compressor can return to the compact rack height after resizing");
        }
        if (QStringList{"daw.chorus", "daw.flanger", "daw.phaser", "daw.doubler"}.contains(uid)) {
            auto* display = card->findChild<QWidget*>("RackModulationDisplay." + QString::fromUtf8(uid).mid(4));
            const QString parameter = QString::fromUtf8(uid) == "daw.doubler" ? "width" : "depth";
            auto* number = card->findChild<QDoubleSpinBox*>("RackNumber." + parameter);
            if (!display || !number || display->accessibleName().isEmpty()) return fail("modulation display and precise controls");
            const auto before = display->grab().toImage();
            controller.setInsertParameter(gallery, slot, parameter.toStdString(), .9);
            card->refresh();
            if (display->grab().toImage() == before || std::abs(number->value() - 90.) > .001)
                return fail("modulation display and percentage readout follow the processor");
            const auto depth = controller.undoDepth();
            number->setValue(25);
            card->finishEdits();
            if (std::abs(controller.insertParameter(gallery, slot, parameter.toStdString()) - .25) > .001 ||
                controller.undoDepth() != std::min(depth + 1, controller.undoLimit()))
                return fail("percentage input commits one parameter edit within the history limit");
            card->refresh();
        }
        if (QString::fromUtf8(uid) == "daw.modulation") {
            auto* chorus = card->findChild<QWidget*>("RackModulationDisplay.chorus");
            auto* flanger = card->findChild<QWidget*>("RackModulationDisplay.flanger");
            auto* earlier = card->findChild<ui::IconButton*>("RackModuleMove.chorus.left");
            auto* later = card->findChild<ui::IconButton*>("RackModuleMove.chorus.right");
            if (!chorus || !flanger || !earlier || !later || earlier->isEnabled() || !later->isEnabled())
                return fail("modulation module headers expose valid reorder directions");
            const auto chorusBefore = chorus->grab().toImage(), flangerBefore = flanger->grab().toImage();
            controller.setInsertParameter(gallery, slot, "flanger.depth", .95);
            card->refresh();
            if (chorus->grab().toImage() != chorusBefore || flanger->grab().toImage() == flangerBefore)
                return fail("each modulation display reads only its own prefixed parameters");
            later->click();
            card->refresh();
            QApplication::processEvents();
            if (controller.insertParameter(gallery, slot, "order") == 0 || !earlier->isEnabled())
                return fail("modulation header reorders the actual processing chain");
        }
        if (QString::fromUtf8(uid) == "daw.delay") {
            auto* division = card->findChild<QComboBox*>("RackChoice.division");
            auto* milliseconds = card->findChild<QDoubleSpinBox*>("RackNumber.timeMs");
            auto* tempo = card->findChild<QDoubleSpinBox*>("RackNumber.bpm");
            auto* display = card->findChild<QWidget*>("RackDelayDisplay");
            if (!division || !milliseconds || !tempo || !display || division->count() != 23 ||
                !division->isVisible() || milliseconds->isVisible() || tempo->isVisible())
                return fail("delay starts with note choices and only the active timing controls");
            const auto before = display->grab().toImage();
            controller.setInsertParameter(gallery, slot, "timeMode", 1);
            card->refresh();
            if (!tempo->isVisible() || !division->isVisible()) return fail("delay exposes local tempo");
            controller.setInsertParameter(gallery, slot, "timeMode", 2);
            controller.setInsertParameter(gallery, slot, "mode", 1);
            card->refresh();
            milliseconds->setValue(450);
            card->finishEdits();
            card->refresh();
            if (!milliseconds->isVisible() || division->isVisible() || tempo->isVisible() ||
                controller.insertParameter(gallery, slot, "timeMs") != 450 || display->grab().toImage() == before)
                return fail("delay milliseconds and ping-pong preview follow edits");
            QApplication::processEvents();
            capture("delay-ms");
            controller.setInsertParameter(gallery, slot, "timeMode", 0);
            card->refresh();
        }
        QApplication::processEvents();
        for (auto* number : card->findChildren<QDoubleSpinBox*>())
            if (number->isVisible() && !card->rect().contains(QRect(number->mapTo(card, QPoint()), number->size())))
                return fail(QString("numeric control clipped: ") + uid + "/" + number->objectName());
        capture(QString::fromUtf8(uid).mid(4));
        if (auto* panel = card->findChild<UtilityPanel*>()) {
            const auto utilityFits = [&] {
                for (auto* control : panel->findChildren<QWidget*>()) {
                    if (!control->isVisible() || (!qobject_cast<QSlider*>(control) &&
                        !qobject_cast<QDoubleSpinBox*>(control) && !qobject_cast<QAbstractButton*>(control))) continue;
                    if (!panel->rect().contains(QRect(control->mapTo(panel, QPoint()), control->size()))) return false;
                }
                return true;
            };
            if (!utilityFits()) return fail(QString("utility controls fit minimum rack height: ") + uid);
            if (QString::fromUtf8(uid) == "daw.stereo-utility") {
                controller.setInsertParameter(gallery, slot, "mode", 2);
                panel->refresh();
                auto* width = panel->findChild<QSlider*>("RackParameter.width");
                if (!width || width->isEnabled()) return fail("stereo width disabled outside Stereo mode");
                controller.setInsertParameter(gallery, slot, "mode", 0);
                panel->refresh();
                if (!width->isEnabled()) return fail("stereo width enabled in Stereo mode");
            }
            if (QString::fromUtf8(uid) == "daw.reverse") {
                controller.setInsertParameter(gallery, slot, "timeMode", 1);
                panel->refresh();
                if (!panel->findChild<QSlider*>("RackParameter.timeMs")->isEnabled() ||
                    panel->findChild<QWidget*>("RackChoice.division")->isEnabled())
                    return fail("reverse switches milliseconds and tempo controls");
                QApplication::processEvents();
                if (!utilityFits()) return fail("reverse millisecond controls fit the rack");
                capture("reverse-ms");
            }
            RackParameterBinding binding(&controller, QString::fromStdString(gallery), QString::fromStdString(slot), nullptr);
            UtilityPanel editor(&binding, QString::fromUtf8(uid));
            editor.resize(editor.sizeHint());
            editor.show();
            QApplication::processEvents();
            if (!screenshot.isEmpty()) editor.grab().save(screenshot + "." + QString::fromUtf8(uid).mid(4) + "-editor.png");
            editor.hide();
        }
        if (!instrument)
            controller.removeRackSelection(gallery, {slot, neighbour});
    }
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
    auto* eqView = effectCard->findChild<RackBuiltinView*>();
    rack->m_scroll->ensureWidgetVisible(effectCard);
    QApplication::processEvents();
    if (!eqView || !eqView->checkEqualizerForTest(error))
        return fail(error ? *error : "rack EQ gestures");
    QApplication::processEvents();
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
        "PASS rack UI: groups, focus, virtual controls, synchronization, narrow layout, browser plugins, single instrument, mixed picker, EQ gestures, hidden telemetry\n");
    return true;
}
