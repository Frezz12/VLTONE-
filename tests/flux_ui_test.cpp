#include "EngineController.hpp"
#include "FluxPanel.hpp"
#include "FluxWidgets.hpp"
#include "Typography.hpp"
#include <QAccessible>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace daw::plugins::flux;
namespace {
unsigned failures = 0;
void check(bool value, const char *message) {
  std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
  failures += !value;
}
} // namespace
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  QCoreApplication::setOrganizationName("FluxTest");
  QCoreApplication::setApplicationName("FluxTest");
  QSettings().setValue("ui/language", "en");
  ui::initializeApplicationFonts();
  FluxPanel panel(nullptr, {}, {});
  panel.resize(panel.sizeHint());
  panel.show();
  QTest::qWait(30);
  check(panel.visualUpdatesActive(), "visible editor updates telemetry");
  check(panel.applyFactoryPreset(94), "factory preset loads in native panel");
  QTest::qWait(30);
  QElapsedTimer searchTime;
  searchTime.start();
  if (auto *search = panel.findChild<QLineEdit *>()) {
    for (unsigned i = 0; i < 20; ++i)
      search->setText(i % 2 ? "gate" : "bass");
    search->clear();
  }
  std::printf("BENCH preset search 21 updates: %.3f ms\n",
              searchTime.nsecsElapsed() / 1.e6);
  QSignalSpy edits(&panel, &FluxPanel::projectEdited);
  auto before = panel.currentState();
  auto *random = panel.findChild<QPushButton *>("flux.random");
  check(random && random->isVisible(), "Random is visible in the top bar");
  check(random && random->text() == "Random" && !random->icon().isNull(),
        "Random has a readable caption and dice icon");
  if (random)
    QTest::mouseClick(random, Qt::LeftButton);
  auto first = panel.currentState();
  if (random)
    QTest::mouseClick(random, Qt::LeftButton);
  auto second = panel.currentState();
  check(first.presetId == "random" && second.presetId == "random" &&
            first.scenes.size() >= 4 && first.modules != before.modules &&
            !first.scenes[0].routes.empty() && !first.path.empty(),
        "Random generates an unsaved complete sound, topology, scenes and "
        "modulation");
  check(first != second && first.seed != second.seed,
        "every click creates a new complete sound");
  check(edits.count() >= 2, "Random is a project edit");
  auto *randomAccessibility = QAccessible::queryAccessibleInterface(random);
  check(randomAccessibility &&
            !randomAccessibility->text(QAccessible::Name).isEmpty(),
        "Random has an accessible name");
  if (auto *undo = panel.findChild<QPushButton *>("flux.undo"))
    undo->click();
  check(panel.currentState() == first,
        "Undo restores the entire previous random sound");
  QTemporaryDir files;
  QString path = files.filePath("roundtrip.fluxpreset");
  check(panel.exportPreset(path), "native preset export");
  panel.applyFactoryPreset(0);
  check(panel.importPreset(path), "native preset import");
  check(panel.currentState().name == first.name, "preset browser round trip");
  QFile bad(files.filePath("bad.fluxpreset"));
  bad.open(QIODevice::WriteOnly);
  bad.write("{broken}");
  bad.close();
  auto retained = panel.currentState();
  check(!panel.importPreset(bad.fileName()) &&
            panel.currentState().name == retained.name,
        "invalid import leaves editor state intact");
  panel.applyFactoryPreset(94);
  auto *graph = panel.findChild<QWidget *>("flux.motion");
  check(graph && graph->isVisible(), "Motion editor is visible");
  if (graph) {
    graph->setFocus();
    QTest::mouseClick(graph, Qt::LeftButton, Qt::NoModifier,
                      QPoint(graph->width() / 3, graph->height() / 2));
    QTest::keyClick(graph, Qt::Key_Up);
    check(validState(panel.currentState()),
          "mouse and keyboard curve edits produce a valid program");
    auto *curve = dynamic_cast<fluxui::CurveEditor *>(graph);
    QTest::keyClick(graph, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(graph, Qt::Key_C, Qt::ControlModifier);
    const auto source = panel.currentState().scenes[0].modules[0].curves[0];
    curve->curve = 1;
    QTest::keyClick(graph, Qt::Key_V, Qt::ControlModifier);
    const auto pasted = panel.currentState().scenes[0].modules[0].curves[1];
    bool sameCurve = source.points.size() == pasted.points.size();
    for (unsigned i = 0; i < 100; ++i)
      sameCurve &= std::abs(evaluate(source, i / 100.) -
                            evaluate(pasted, i / 100.)) < 1e-6;
    check(sameCurve && validState(panel.currentState()),
          "copy/paste transfers the entire curve without shifting points");
    curve->curve = 0;
  }
  QString output = qEnvironmentVariable("FLUX_TEST_ARTIFACTS");
  if (output.isEmpty())
    output = QDir::currentPath() + "/artifacts/flux";
  QDir().mkpath(output);
  panel.applyFactoryPreset(94);
  QTest::qWait(40);
  check(panel.grab().save(output + "/flux-en.png"),
        "English panel screenshot saved");
  if (auto *target = panel.findChild<QComboBox *>("flux.motion.target")) {
    QSignalSpy changes(target, &QComboBox::activated);
    target->showPopup();
    QTest::qWait(30);
    auto *view = target->view();
    check(view->isVisible() && view->objectName() == "fluxChoiceList",
          "custom dropdown exposes the native selectable list");
    view->window()->grab().save(output + "/flux-dropdown.png");
    const auto row = view->model()->index(1, 0);
    const auto centre = view->visualRect(row).center();
    QTest::mouseMove(view->viewport(), centre);
    QTest::qWait(QApplication::doubleClickInterval());
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, centre);
    check(target->currentIndex() == 1 && changes.count() == 1 && !view->isVisible(),
          "custom dropdown selects with the pointer and commits once");
    target->showPopup();
    QTest::keyClick(view, Qt::Key_Down);
    QTest::keyClick(view, Qt::Key_Escape);
    check(target->currentIndex() == 1 && changes.count() == 1 && !view->isVisible(),
          "Escape cancels the dropdown without editing the sound");
    target->setFocus();
    QTest::keyClick(target, Qt::Key_Down);
    check(target->currentIndex() == 2 && changes.count() == 2,
          "custom dropdown retains keyboard selection");
    panel.applyFactoryPreset(94);
  }
  for (unsigned page = 1; page <= 2; ++page) {
    auto *tab =
        panel.findChild<QPushButton *>(QString("flux.page.%1").arg(page));
    if (tab) {
      tab->click();
      QTest::qWait(20);
      check(tab->isChecked() && !graph->isVisible(),
            "selected tab switches the main editor");
      panel.grab().save(
          output + (page == 1 ? "/flux-sequence.png" : "/flux-routing.png"));
    }
  }
  panel.applyFactoryPreset(8);
  QTest::qWait(20);
  panel.grab().save(output + "/flux-multiband.png");
  if (auto *tab = panel.findChild<QPushButton *>("flux.page.0"))
    tab->click();
  panel.applyFactoryPreset(94);
  if (auto *modulation = panel.findChild<QPushButton *>("flux.modulation")) {
    QTimer::singleShot(40, [&] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        dialog->grab().save(output + "/flux-modulation.png");
        dialog->reject();
      }
    });
    modulation->click();
  }
  panel.hide();
  check(!panel.visualUpdatesActive(), "hidden editor stops visual timer");
  QSettings().setValue("ui/language", "ru");
  FluxPanel russian(nullptr, {}, {});
  russian.resize(russian.sizeHint());
  russian.applyFactoryPreset(99);
  russian.show();
  QTest::qWait(40);
  russian.grab().save(output + "/flux-ru.png");
  russian.resize(russian.minimumSizeHint());
  QTest::qWait(20);
  russian.grab().save(output + "/flux-compact.png");
  auto *browser = russian.findChild<QWidget *>("flux.presets");
  auto *narrowRandom = russian.findChild<QPushButton *>("flux.random");
  check(browser && !browser->isVisible() && narrowRandom &&
            russian.rect().contains(
                narrowRandom->mapTo(&russian, narrowRandom->rect().center())),
        "narrow editor collapses the browser and keeps Random visible");
  russian.hide();
  panel.show();
  QEvent language(QEvent::LanguageChange);
  QApplication::sendEvent(&panel, &language);
  QTest::qWait(30);
  bool translated = false;
  for (auto *base : panel.findChildren<ui::Knob *>()) {
    auto *knob = dynamic_cast<fluxui::Knob *>(base);
    translated |= knob && knob->caption == QString::fromUtf8("Режим");
  }
  check(translated && panel.currentState().name == "Neon Machine",
        "language change translates parameters without changing the sound");
  panel.hide();
  auto locked = factoryState(94);
  locked.modules[0].locked = true;
  locked.parameterLocks.push_back(effectParam(1, Kind::Repeat, 0));
  auto old = locked;
  randomize(locked, 45);
  check(locked.scenes[0].modules[0].values == old.scenes[0].modules[0].values &&
            locked.scenes[0].modules[1].values[0] ==
                old.scenes[0].modules[1].values[0],
        "randomization respects module and parameter locks");
  check(QDir(files.path()).entryList({"*.fluxpreset"}, QDir::Files).size() == 2,
        "Random did not create preset files");
  app.setProperty("dawHeadlessDataRoot", settings.path());
  {
    daw::EngineController controller;
    check(bool(controller.initialize(48000, 257, false)),
          "host initializes without an audio device");
    const auto track =
        controller.addTrack(daw::TrackKind::Audio, "Flux integration");
    const auto insert =
        controller.addInsert(track, FluxInstance::staticDescriptor());
    check(!insert.empty(), "Flux inserts through the native host");
    if (!insert.empty()) {
      FluxPanel hosted(&controller, QString::fromStdString(track),
                       QString::fromStdString(insert));
      hosted.resize(hosted.sizeHint());
      hosted.show();
      check(hosted.applyFactoryPreset(94),
            "factory state applies through controller");
      auto baseline = controller.fluxSnapshot(track, insert);
      QTest::qWait(50);
      if (auto *target = hosted.findChild<QComboBox *>("flux.motion.target")) {
        QSignalSpy resets(target->model(), &QAbstractItemModel::rowsRemoved);
        QTest::qWait(300);
        std::printf("BENCH idle target rebuilds in 300 ms: %lld\n",
                    static_cast<long long>(resets.count()));
        check(resets.count() == 0,
              "idle editor does not rebuild parameter controls");
      }
      controller.setInsertParameter(track, insert, parameterTable()[Mix].id,
                                    .4321);
      QTest::qWait(50);
      check(std::abs(hosted.currentState().globals[Mix] - .4321) < 1e-9,
            "host parameter changes still reach the editor without polling "
            "full snapshots");
      controller.setInsertParameter(track, insert, parameterTable()[Mix].id,
                                    baseline->state.globals[Mix]);
      QTest::qWait(50);
      auto depth = controller.undoDepth();
      QElapsedTimer randomTime;
      randomTime.start();
      hosted.randomizeParameters(20261013);
      std::printf("BENCH hosted complete Random: %.3f ms\n",
                  randomTime.nsecsElapsed() / 1.e6);
      auto randomized = controller.fluxSnapshot(track, insert);
      check(baseline && randomized &&
                randomized->state.scenes[0].modules[0].values !=
                    baseline->state.scenes[0].modules[0].values &&
                controller.undoDepth() == depth + 1,
            "hosted Random makes one undo transaction");
      if (randomized) {
        auto id = parameterTable()[effectParam(
                                       0, randomized->state.modules[0].kind, 0)]
                      .id;
        check(std::abs(controller.insertParameter(track, insert, id) -
                       randomized->state.scenes[0].modules[0].values[0]) < 1e-9,
              "Random updates the project parameter snapshot");
      }
      controller.undo();
      auto undone = controller.fluxSnapshot(track, insert);
      check(undone && baseline &&
                undone->state.scenes[0].modules[0].values ==
                    baseline->state.scenes[0].modules[0].values,
            "host undo restores pre-Random parameters");
      controller.redo();
      if (randomized) {
        auto reordered = randomized->state;
        std::swap(reordered.order[0], reordered.order[1]);
        check(
            controller.applyFluxState(track, insert, reordered, "Reorder Flux"),
            "module reorder applies");
        auto id = parameterTable()[effectParam(
                                       0, randomized->state.modules[0].kind, 0)]
                      .id;
        check(std::abs(controller.insertParameter(track, insert, id) -
                       randomized->state.scenes[0].modules[0].values[0]) < 1e-9,
              "module automation ID survives reorder");
      }
      auto expected = controller.fluxSnapshot(track, insert);
      const auto package = files.path().toStdString() + "/flux-project.vlt";
      check(bool(controller.saveProject(package)),
            "project saves complete Flux state");
      daw::EngineController restored;
      restored.initialize(48000, 257, false);
      check(bool(restored.openProject(package)), "project reopens with Flux");
      auto recovered = restored.fluxSnapshot(track, insert);
      check(recovered && expected &&
                recovered->state.order == expected->state.order &&
                recovered->state.scenes[0].modules[0].values ==
                    expected->state.scenes[0].modules[0].values &&
                recovered->state.seed == expected->state.seed,
            "project restores Random sound, seed and routing order");
      restored.shutdown();
      hosted.hide();
    }
    controller.shutdown();
  }
  std::printf("Flux UI validation: %u failures\n", failures);
  return failures ? 1 : 0;
}
