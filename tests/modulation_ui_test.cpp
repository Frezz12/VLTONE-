#include "Controls.hpp"
#include "EngineController.hpp"
#include "ModulationPanel.hpp"
#include "Theme.hpp"
#include "cloud/PublishPreflight.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace daw::plugins::modulation;
namespace {
int failures = 0;
void check(bool ok, const char *text) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok)
        ++failures;
}
void events(int ms = 80) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
void chooseMenu(QPushButton *button, const QString &name, const QString &input = {}) {
    QTimer::singleShot(0, [name, input] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) {
            check(false, "preset popup opens");
            return;
        }
        for (auto *action : menu->actions())
            if (action->text() == name) {
                QTimer::singleShot(0, [input] {
                    for (auto *widget : QApplication::topLevelWidgets()) {
                        if (auto *dialog = qobject_cast<QInputDialog *>(widget);
                            dialog && dialog->isVisible()) {
                            dialog->setTextValue(input);
                            dialog->accept();
                        }
                        if (auto *dialog = qobject_cast<QMessageBox *>(widget);
                            dialog && dialog->isVisible())
                            dialog->done(QMessageBox::Yes);
                    }
                });
                action->trigger();
                menu->close();
                return;
            }
        check(false, "requested preset action exists");
        menu->close();
    });
    button->click();
    events();
}

} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    QCoreApplication::setOrganizationName("VLTONE-Modulation-Test");
    QCoreApplication::setApplicationName("Modulation");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    daw::EngineController controller;
    check(bool(controller.initialize(48000, 257, false)), "headless controller initializes");
    QString screenshotDir;
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--screenshots")) {
        screenshotDir = QString::fromLocal8Bit(argv[2]);
        QDir().mkpath(screenshotDir);
    }
    for (int k = 0; k < kindCount; ++k) {
        const auto kind = Kind(k);
        const auto &desc = descriptorFor(kind);
        std::printf("\n%s\n", desc.name.c_str());
        const auto track = controller.addTrack(daw::TrackKind::Audio, desc.name);
        const auto insert = controller.addInsert(track, desc);
        check(!insert.empty() && controller.insertInstance(track, insert),
              "effect inserts into a live controller");
        auto panel = std::make_unique<ModulationPanel>(&controller, QString::fromStdString(track),
                                                       QString::fromStdString(insert), kind);
        panel->resize(560, kind == Kind::Doubler ? 520 : 540);
        panel->show();
        events();
        auto *preset = panel->findChild<QPushButton *>("ModulationPreset");
        check(preset && preset->text().contains(
                            QString::fromUtf8(factoryPresets(kind)[0].name.data(),
                                              int(factoryPresets(kind)[0].name.size()))),
              "default preset is visible");
        const auto table = parameterTable(kind);
        panel->applyFactoryPreset(9);
        events();
        bool values = true;
        for (const auto &p : table)
            values &= std::abs(controller.insertParameter(track, insert, p.id) -
                               factoryPresets(kind)[9].values[p.index]) < 1.e-6;
        check(values, "preset applies every parameter through the controller");
        controller.undo();
        events();
        values = true;
        for (const auto &p : table)
            values &= std::abs(controller.insertParameter(track, insert, p.id) -
                               factoryPresets(kind)[0].values[p.index]) < 1.e-6;
        check(values, "one undo restores all preset parameters");
        controller.redo();
        events();
        auto *knob =
            panel->findChild<ui::Knob *>(QString::fromStdString("ModulationKnob_" + table[0].id));
        const double before = controller.insertParameter(track, insert, table[0].id);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QApplication::sendEvent(knob, &press);
        events();
        check(controller.insertParameter(track, insert, table[0].id) < before &&
                  preset->text().contains('*'),
              "keyboard input updates audio state and marks modified preset");
        controller.undo();
        events();
        check(std::abs(controller.insertParameter(track, insert, table[0].id) - before) < 1.e-6,
              "keyboard edit is undoable");
        auto *numeric = panel->findChild<QDoubleSpinBox *>(
            QString::fromStdString("ModulationValue_" + table[0].id));
        numeric->setValue(37);
        events();
        if (kind == Kind::DoublerPro) {
            auto *delay = panel->findChild<QDoubleSpinBox *>("ModulationValue_delay");
            auto *detune = panel->findChild<QDoubleSpinBox *>("ModulationValue_detune");
            auto *body = panel->findChild<QDoubleSpinBox *>("ModulationValue_body");
            check(delay && detune && body && panel->findChildren<ui::Knob *>().size() == 6,
                  "Pro exposes six knobs with independent Delay, Detune and Body controls");
            if (delay && detune && body) {
                delay->setValue(73);
                detune->setValue(9.5);
                body->setValue(61);
                events();
                check(std::abs(controller.insertParameter(track, insert, "delay") - .73) < 1.e-6 &&
                          std::abs(controller.insertParameter(track, insert, "detune") - 9.5) < 1.e-6 &&
                          std::abs(controller.insertParameter(track, insert, "body") - .61) < 1.e-6,
                      "Pro numerical entry preserves cents and percentage units");
                QStringList routed;
                const auto connection = QObject::connect(panel.get(), &ModulationPanel::automationRequested,
                                                          [&](const QString &id) { routed.append(id); });
                for (const auto *id : {"delay", "detune", "body"}) {
                    auto *control = panel->findChild<ui::Knob *>(QString("ModulationKnob_%1").arg(id));
                    if (control) QMetaObject::invokeMethod(control, "automateRequested", Qt::DirectConnection);
                }
                QObject::disconnect(connection);
                check(routed == QStringList{"delay", "detune", "body"},
                      "each new Pro knob routes its own automation parameter ID");
            }
        }
        check(std::abs(controller.insertParameter(track, insert, table[0].id) - .37) < 1.e-6,
              "numeric input uses percentage units");
        chooseMenu(preset, QString::fromUtf8("Save preset…"), QStringLiteral("My soft preset"));
        check(preset->text().contains(QStringLiteral("My soft preset")),
              "user preset saves and becomes the selected reference");
        numeric->setValue(11);
        events();
        chooseMenu(preset, QStringLiteral("My soft preset"));
        check(std::abs(controller.insertParameter(track, insert, table[0].id) - .37) < 1.e-6,
              "user preset restores its saved sound");
        chooseMenu(preset, QString::fromUtf8("Rename preset…"),
                   QStringLiteral("Renamed soft preset"));
        check(preset->text().contains(QStringLiteral("Renamed soft preset")),
              "user preset can be renamed");
        int automationSignals = 0;
        QObject::connect(panel.get(), &ModulationPanel::automationRequested,
                         [&](const QString &id) {
                             if (id == QString::fromStdString(table[0].id))
                                 ++automationSignals;
                         });
        QMetaObject::invokeMethod(knob, "automateRequested", Qt::DirectConnection);
        check(automationSignals == 1, "knob automation routes the stable parameter ID");
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::PluginParameter;
        target.channelId = track;
        target.slotId = insert;
        target.parameterId = table[0].id;
        check(!controller.ensureAutomation(target).first.empty(),
              "controller creates plugin automation");

        auto *instance =
            dynamic_cast<ModulationInstance *>(controller.insertInstance(track, insert));
        const auto chosen = instance->presetReference();
        const auto package =
            temporary.filePath(QString::fromStdString(desc.name) + ".vlt").toStdString();
        check(bool(controller.saveProject(package)), "project saves opaque plugin state");
        daw::EngineController restored;
        restored.initialize(48000, 257, false);
        check(bool(restored.openProject(package)), "project reopens with built-in modulation");
        auto *recalled = dynamic_cast<ModulationInstance *>(restored.insertInstance(track, insert));
        bool restoredValues = recalled && recalled->presetReference() == chosen;
        for (const auto &parameter : table)
            restoredValues &= std::abs(restored.insertParameter(track, insert, parameter.id) -
                                        controller.insertParameter(track, insert, parameter.id)) < 1.e-6;
        check(restoredValues,
              "parameters and preset reference survive project round trip");
        chooseMenu(preset, QString::fromUtf8("Delete preset…"));
        check(std::abs(controller.insertParameter(track, insert, table[0].id) - .37) < 1.e-6,
              "deleting a user preset retains current parameter values");
        const auto source =
            temporary.filePath(QString::fromStdString(desc.name) + "-source.wav").toStdString();
        audio::platform::AudioFileWriter writer;
        writer.open(source, 48000, 1, 48000);
        std::array<float, 48000> signal{};
        for (unsigned i = 0; i < signal.size(); ++i)
            signal[i] = float(.25 * std::sin(i * .071) + .06 * std::sin(i * .19));
        const float *sourceChannels[]{signal.data()};
        writer.write(sourceChannels, 48000);
        writer.close();
        controller.importAudio(source, track, 0);
        daw::rendering::Spec spec;
        spec.outputDir = temporary.filePath("renders").toStdString();
        spec.baseName = desc.name;
        spec.range = daw::rendering::Range::Custom;
        spec.customEndSeconds = .5;
        spec.sourceTrackIds = {track};
        daw::rendering::Report report;
        const bool rendered = bool(controller.renderProject(spec, {}, report));
        audio::platform::DecodedAudio decoded;
        const bool decodedOk =
            rendered && !report.files.empty() &&
            bool(audio::platform::decodeAudioFile(report.files.front(), decoded));
        bool finite = decodedOk, nonzero = false;
        for (float v : decoded.interleaved) {
            finite &= std::isfinite(v);
            nonzero |= std::abs(v) > .01f;
        }
        check(finite && nonzero && decoded.channels == 2,
              "mono recording renders through the stereo insert offline");
        if (kind == Kind::Doubler && decodedOk) {
            spec.baseName = "Doubler-dry";
            spec.bypassChannelInserts = true;
            daw::rendering::Report dryReport;
            audio::platform::DecodedAudio dry;
            bool exact = bool(controller.renderProject(spec, {}, dryReport)) &&
                         !dryReport.files.empty() &&
                         bool(audio::platform::decodeAudioFile(dryReport.files.front(), dry)) &&
                         dry.frames == decoded.frames;
            double error = 0;
            if (exact)
                for (std::size_t i = 0; i < dry.interleaved.size(); i += 2)
                    error = std::max(
                        error,
                        std::abs(.5 * (double(decoded.interleaved[i]) + decoded.interleaved[i + 1] -
                                       dry.interleaved[i] - dry.interleaved[i + 1])));
            check(exact && error < 1.e-6,
                  "offline wet and bypassed exports preserve the same Doubler mono sum");
        }
        const auto copy = controller.duplicateTrack(track, true);
        const auto *chain = controller.channelInserts(copy);
        check(chain && !chain->empty() && chain->front().uid == desc.uid &&
                  std::abs(controller.insertParameter(copy, chain->front().id, table[0].id) - .37) <
                      1.e-6,
              "track duplication retains independent modulation settings");
        controller.removeTrack(copy);
        check(daw::cloud::isSupportedBuiltinV1(*controller.insertModel(track, insert)),
              "cloud preflight recognises the versioned built-in");

        Theme custom = ThemeManager::instance().theme();
        custom.accent = QColor("#c07af2");
        ThemeManager::instance().applyCustomTheme(custom);
        events();
        check(panel->styleSheet().contains("#c07af2"), "open panel follows a changed theme accent");
        panel->resize(440, kind == Kind::Doubler ? 460 : 620);
        events();
        bool contained = true;
        for (auto *number : panel->findChildren<QDoubleSpinBox *>())
            contained &=
                panel->rect().contains(QRect(number->mapTo(panel.get(), QPoint()), number->size()));
        check(contained, "controls remain reachable at minimum width");
        if (k == 0) {
            controller.setInsertChannelMode(track, insert, daw::PluginChannelMode::Mono);
            events();
            check(panel->findChild<QLabel *>("ModulationMonoNotice")->isVisible(),
                  "mono routing explains why widening is unavailable");
            controller.setInsertChannelMode(track, insert, daw::PluginChannelMode::Stereo);
            events();
        }
        panel->hide();
        events();
        check(!panel->visualUpdatesActive(), "hidden panel stops visual updates");
        panel->show();
        events();
        check(panel->visualUpdatesActive(), "reopening resumes visual updates");

        if (!screenshotDir.isEmpty()) {
            panel->resize(560, kind == Kind::Doubler ? 520 : 540);
            panel->applyFactoryPreset(0);
            events();
            // Feed actual DSP output into the view while the audio device is
            // disabled. This exercises presentation without starting transport.
            auto *dsp =
                dynamic_cast<ModulationInstance *>(controller.insertInstance(track, insert));
            constexpr unsigned n = 257;
            std::array<float, n> in{}, l{}, r{};
            const float *inputs[]{in.data(), in.data()};
            float *outputs[]{l.data(), r.data()};
            daw::plugins::PluginProcessContext ctx;
            ctx.inputs = inputs;
            ctx.outputs = outputs;
            ctx.inputChannels = ctx.outputChannels = 2;
            ctx.frames = n;
            for (int b = 0; b < 120; ++b) {
                for (unsigned i = 0; i < n; ++i)
                    in[i] =
                        float(.2 * std::sin((b * n + i) * .071) + .1 * std::sin((b * n + i) * .17));
                dsp->process(ctx);
            }
            auto *field = panel->findChild<ModulationField *>();
            events(80);
            check(!field->accessibleDescription().contains("-120.0"),
                  "fresh monitoring telemetry stays visible with stopped transport");
            events(300);
            check(field->accessibleDescription().contains("-120.0"),
                  "stale audio telemetry fades when processing stops");
            for (int i = 0; i < 60; ++i)
                field->present(dsp->telemetry(), 1. / 60, false);
            if (kind == Kind::Doubler) {
                const auto t = dsp->telemetry();
                panel->findChild<QLabel *>("ModulationMeters")
                    ->setText(QString("Width %1%   ·   Correlation %2")
                                  .arg(t.width * 100, 0, 'f', 0)
                                  .arg(t.correlation, 0, 'f', 2));
            }
            panel->grab().save(screenshotDir + "/" + QString::fromStdString(desc.name) + ".png");
        }
        controller.removeInsert(track, insert);
        events();
        check(!knob->isEnabled() && !panel->visualUpdatesActive(),
              "removing an insert invalidates controls and stops telemetry safely");
        panel.reset();
        controller.removeTrack(track);
    }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
