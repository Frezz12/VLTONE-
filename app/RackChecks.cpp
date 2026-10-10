#include "Controls.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/EqualizerParams.hpp"
#include "Internal/MiniModuleDefinition.hpp"
#include "Internal/SamplerInstance.hpp"
#include "MixerWidget.hpp"
#include "RackBuiltinView.hpp"
#include "RackDeviceCard.hpp"
#include "RackDrag.hpp"
#include "RackParameterBinding.hpp"
#include "RackWidget.hpp"
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
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cstdio>

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
    auto* rack = mixer.rack();
    rack->setFocus();
    QApplication::processEvents();
    auto* eqCard = rack->m_cards.value(QString::fromStdString(eq)).data();
    if (!eqCard || eqCard->width() != 420)
        return fail("compact equalizer width");
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
    for (const auto* module : {"color", "doubler", "chorus"})
        controller.addMiniModule(b, daw::plugins::mini::builtin(module));
    const auto sendTrack = controller.addTrack(daw::TrackKind::Aux, "Reverb");
    controller.addSend(b, sendTrack);
    mixer.setSelectedTrack(QString::fromStdString(b));
    QApplication::processEvents();
    rack->refresh();
    QApplication::processEvents();
    if (rack->m_cards.size() != 2 || rack->m_groups.size() != 1 ||
        !rack->m_groups.begin().value()->isVisible())
        return fail("group remains visible when returning to its channel");
    capture();
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
         {"daw.equalizer", "daw.compressor", "daw.cla2a", "daw.delay", "daw.pitch-corrector", "daw.graphit",
          "daw.gravity", "daw.modulation", "daw.doubler", "daw.doubler-pro", "daw.chorus", "daw.flanger",
          "daw.phaser", "daw.sampler", "daw.slicer"}) {
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
        mixer.resize(1340, 280);
        rack->sync();
        QApplication::processEvents();
        if (!rack->m_sendKnobs.isEmpty())
            return fail("previous channel sends leaked into an empty destination");
        auto* card = rack->m_cards.value(QString::fromStdString(slot)).data();
        if (!card || card->width() != RackBuiltinView::preferredWidth(uid))
            return fail(QString("native width: ") + uid);
        if (mixer.height() != 280 || card->mapTo(rack->m_scroll->viewport(), QPoint()).y() + card->height() >
                                         rack->m_scroll->viewport()->height())
            return fail(QString("native group exceeds minimum panel height: ") + uid);
        rack->m_scroll->ensureWidgetVisible(card, 0, 0);
        rack->refresh();
        QApplication::processEvents();
        for (auto* knob : card->findChildren<ui::Knob*>())
            if (knob->isVisible() && (!knob->isEnabled() || !card->rect().contains(QRect(
                                                                knob->mapTo(card, QPoint()), knob->size()))))
                return fail(QString("native control unavailable or clipped: ") + uid + "/" +
                            knob->objectName());
        capture(QString::fromUtf8(uid).mid(4));
        if (!instrument)
            controller.removeRackSelection(gallery, {slot, neighbour});
    }
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
        "PASS rack UI: groups, focus, virtual controls, synchronization, narrow layout, hidden telemetry\n");
    return true;
}
