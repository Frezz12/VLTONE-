#include "Controls.hpp"
#include "EngineController.hpp"
#include "MorphPanel.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Typography.hpp"
#include "cloud/PublishPreflight.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <QAccessible>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <algorithm>
#include <cstdio>
#include <numbers>

using namespace daw::plugins::morph;
namespace {
class MeasuredApplication : public QApplication {
public:
  using QApplication::QApplication;
  QWidget *measured = nullptr;
  std::vector<double> paintMs;
  unsigned knobPaints = 0;
  bool notify(QObject *object, QEvent *event) override {
    if (measured && event->type() == QEvent::Paint &&
        qobject_cast<ui::Knob *>(object))
      ++knobPaints;
    if (object != measured || event->type() != QEvent::UpdateRequest)
      return QApplication::notify(object, event);
    QElapsedTimer elapsed;
    elapsed.start();
    const bool result = QApplication::notify(object, event);
    paintMs.push_back(elapsed.nsecsElapsed() / 1.e6);
    return result;
  }
};
int failures = 0;
void check(bool ok, const char *text) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
  failures += !ok;
}
bool same(const State &a, const State &b) {
  std::vector<std::uint8_t> first, second;
  return encodeState(a, first) && encodeState(b, second) && first == second;
}
void move(QWidget *w, QPoint at) {
  QMouseEvent event(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(w, &event);
}
QPushButton *button(QWidget *panel, const QString &caption) {
  for (auto *b : panel->findChildren<QPushButton *>())
    if (b->text() == caption)
      return b;
  return nullptr;
}
bool menuAction(QPushButton *button, const QStringList &path) {
  if (!button)
    return false;
  bool found = false;
  QTimer::singleShot(0, button, [&] {
    auto *root = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!root)
      return;
    QMenu *menu = root;
    QAction *chosen = nullptr;
    for (const auto &name : path) {
      chosen = nullptr;
      if (!menu)
        break;
      for (auto *action : menu->actions())
        if (action->text() == name) {
          chosen = action;
          break;
        }
      menu = chosen ? chosen->menu() : nullptr;
    }
    root->close();
    if (chosen) {
      found = true;
      chosen->trigger();
    }
  });
  button->click();
  return found;
}
} // namespace
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  ui::registerFontUrlScheme();
  MeasuredApplication app(argc, argv);
  ui::initializeApplicationFonts();
  QTemporaryDir temporary;
  app.setProperty("dawHeadlessDataRoot", temporary.path());
  app.setOrganizationName("MorphTest");
  app.setApplicationName("MorphTest");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     temporary.path());
  daw::EngineController controller;
  const bool hardwareSmoke = app.arguments().contains("--hardware-smoke");
  const bool hardware =
      hardwareSmoke || app.arguments().contains("--hardware-soak");
  check(bool(controller.initialize(48000, hardware ? 128 : 257, hardware)),
        "engine initializes");
  if (hardware && !controller.isDeviceOpen()) {
    check(false, "hardware device available");
    return 1;
  }
  auto desc = controller.pluginManager().find(daw::plugins::Format::Internal,
                                              "daw.morph");
  check(desc.has_value(), "Morph in builtin catalog");
  if (!desc)
    return 1;
  audio::AudioBuffer audio(2, 48000);
  for (unsigned i = 0; i < 48000; ++i)
    audio.getChannel(0)[i] = audio.getChannel(1)[i] =
        float(.2 * std::sin(2 * std::numbers::pi * 220 * i / 48000));
  audio::AudioRecorder writer;
  writer.initialize(48000, 2);
  auto path = temporary.path().toStdString() + "/input.wav";
  writer.writeWAVFile(path, audio, 48000);
  auto track = controller.importAudioToNewTrack(path, 0),
       insert = controller.addInsert(track, *desc);
  check(!insert.empty(), "Morph inserts through normal host");
  if (insert.empty())
    return 1;
  check(
      !daw::cloud::isSupportedBuiltinV1(*controller.insertModel(track, insert)),
      "Morph remains local-only");
  MorphPanel panel(&controller, QString::fromStdString(track),
                   QString::fromStdString(insert));
  panel.resize(1536, 864);
  panel.show();
  QTest::qWait(80);
  {
    panel.resize(panel.sizeHint());
    const auto resizeConnection = QObject::connect(
        &panel, &MorphPanel::preferredHeightChanged, &panel,
        [&panel](int height) { panel.resize(panel.width(), height); });
    auto *balance = panel.findChild<QPushButton *>("morphBalanceDetails");
    auto *details = panel.findChild<QPushButton *>("morphModulationDetails");
    check(balance && details, "Balance disclosure is available");
    if (balance && details) {
      for (int repeat = 0; repeat < 3; ++repeat) {
        balance->click();
        QApplication::processEvents();
        check(panel.height() == qRound(1098 * .75) && details->isChecked(),
              "Balance opens with its content height");
        details->click();
        QApplication::processEvents();
        check(panel.height() == 648 && !balance->isChecked() &&
                  panel.findChild<QScrollArea *>()->verticalScrollBar()->maximum() == 0,
              "Balance closes without empty space or accumulated height");
      }
    }
    QObject::disconnect(resizeConnection);
    panel.resize(1536, 864);
  }
  auto *depth = panel.findChild<ui::Knob *>("chorus.depth");
  check(depth, "dedicated native editor");
  if (!depth)
    return 1;
  if (app.arguments().contains("--ui-bench")) {
    panel.applyFactoryPreset(25);
    controller.play();
    audio::AudioBuffer in(2, 257), out(2, 257);
    in.clear();
    QTest::qWait(100);
    for (const bool drag : {false, true}) {
      app.paintMs.clear();
      app.knobPaints = 0;
      app.measured = &panel;
      QElapsedTimer duration;
      duration.start();
      if (drag)
        QTest::mousePress(depth, Qt::LeftButton, Qt::NoModifier,
                          depth->rect().center());
      for (unsigned frame = 0; frame < 180; ++frame) {
        for (unsigned block = 0; block < 3; ++block)
          controller.processDeviceBlockForTest(in, out, 257);
        if (drag)
          move(depth, depth->rect().center() +
                          QPoint(0, int(35 * std::sin(frame * .08))));
        QTest::qWait(16);
      }
      if (drag)
        QTest::mouseRelease(depth, Qt::LeftButton);
      app.measured = nullptr;
      auto times = app.paintMs;
      std::sort(times.begin(), times.end());
      if (!times.empty())
        std::printf("UI_BENCH drag=%d dpi=%.2f updates=%zu knob_paints=%u "
                    "paint_p50_ms=%.3f paint_p95_ms=%.3f elapsed_ms=%lld\n",
                    drag, panel.devicePixelRatioF(), times.size(),
                    app.knobPaints, times[times.size() / 2],
                    times[times.size() * 95 / 100], duration.elapsed());
    }
    controller.stop();
    return 0;
  }
  auto *access = QAccessible::queryAccessibleInterface(depth);
  check(access && access->valueInterface(), "accessible native knob values");
  {
    auto *routing = panel.findChild<QComboBox *>("morph.routing");
    const auto before = panel.currentState();
    const auto history = controller.undoDepth();
    QSignalSpy activated(routing, &QComboBox::activated);
    QTest::mouseClick(routing, Qt::LeftButton);
    QTest::qWait(30);
    check(routing->view()->isVisible() &&
              routing->view()->objectName() == "morphChoiceList",
          "routing opens the custom light popup");
    const int next = (routing->currentIndex() + 1) % routing->count();
    const auto row = routing->model()->index(next, routing->modelColumn());
    QTest::mouseMove(routing->view()->viewport(),
                     routing->view()->visualRect(row).center());
    // Qt suppresses the opening mouse release. Move into the popup and allow
    // its double-click guard to expire before issuing a separate selection.
    QTest::qWait(QApplication::doubleClickInterval());
    QTest::mouseClick(routing->view()->viewport(), Qt::LeftButton,
                      Qt::NoModifier, routing->view()->visualRect(row).center());
    QTest::qWait(40);
    check(!routing->view()->isVisible() &&
              panel.currentState().values[Routing] == next &&
              controller.undoDepth() == history + 1 && activated.size() == 1,
          "custom popup selection updates routing with one undo");
    if (controller.undoDepth() > history)
      controller.undo();
    check(QTest::qWaitFor([&] { return same(before, panel.currentState()); }),
          "custom popup selection follows controller undo");
    routing->showPopup();
    QTest::keyClick(routing->view(), Qt::Key_Escape);
    check(!routing->view()->isVisible() && same(before, panel.currentState()),
          "Escape dismisses the popup without changing the sound");

    panel.findChild<QPushButton *>("morphModulationDetails")->click();
    QComboBox *destination = nullptr;
    for (auto *combo : panel.findChildren<QComboBox *>())
      if (combo->accessibleName() == "Modulation destination 1")
        destination = combo;
    check(destination, "modulation destination is accessible by name");
    if (destination) {
      const auto destinationHistory = controller.undoDepth();
      destination->showPopup();
      QTest::qWait(30);
      check(destination->view()->verticalScrollBar()->maximum() > 0,
            "long custom menus can scroll to every destination");
      QTest::keyClick(destination->view(), Qt::Key_End);
      QTest::keyClick(destination->view(), Qt::Key_Return);
      check(!destination->view()->isVisible() &&
                panel.currentState().values[matrixTarget(0)] ==
                    destination->itemData(destination->count() - 1).toUInt(),
            "custom menus retain End and Enter keyboard selection");
      if (controller.undoDepth() > destinationHistory)
        controller.undo();
      QTest::qWait(40);
    }
    panel.findChild<QPushButton *>("morphModulationDetails")->click();
    panel.findChild<QScrollArea *>()->verticalScrollBar()->setValue(0);
  }
  {
    QTest::qWait(60);
    app.knobPaints = 0;
    app.measured = &panel;
    QTest::qWait(120);
    app.measured = nullptr;
    check(app.knobPaints == 0,
          "unchanged controls do not repaint on telemetry ticks");
    auto *random = panel.findChild<QPushButton *>("morphRandom");
    auto *preset = panel.findChild<QPushButton *>("morphPresetMenu");
    auto *gain = panel.findChild<ui::Knob *>("output.gain");
    auto *automatic = panel.findChild<QPushButton *>("output.autogain");
    auto *clip = panel.findChild<QPushButton *>("output.softclip");
    check(random && preset && std::abs(random->y() - preset->y()) < 3 &&
              !panel.findChild<QPushButton *>("morphBottomFavorite"),
          "randomization shares the single header preset bar");
    check(gain && gain->isVisible() && gain->maximumValue() == 12 &&
              automatic && automatic->isVisible() && clip && clip->isVisible(),
          "manual gain and level compensation are visible without opening "
          "drawers");
    QSignalSpy edits(&panel, &MorphPanel::projectEdited);
    const auto before = depth->value();
    QTest::mousePress(depth, Qt::LeftButton, Qt::NoModifier,
                      depth->rect().center());
    move(depth, depth->rect().center() - QPoint(0, 10));
    move(depth, depth->rect().center() - QPoint(0, 20));
    check(edits.empty() && depth->value() != before,
          "drag updates audio directly without repainting the host timeline");
    QTest::mouseRelease(depth, Qt::LeftButton);
    check(edits.size() == 1, "a completed drag notifies the host once");
    controller.undo();
    check(QTest::qWaitFor([&] { return depth->value() == before; }, 1000),
          "optimized drag keeps normal Undo behavior");
  }
  {
    const auto before = panel.currentState();
    bool iconsNamed = true;
    for (auto *action : panel.findChildren<QPushButton *>())
      if (action->property("morphIcon").toBool())
        iconsNamed &=
            !action->accessibleName().isEmpty() && !action->toolTip().isEmpty();
    check(iconsNamed, "icon actions retain accessible names and tooltips");
    bool disclosed = true;
    for (unsigned module = 0; module < 4; ++module) {
      auto *more = panel.findChild<QPushButton *>(
          QString("morphModuleDetails%1").arg(module));
      more->click();
      unsigned visible = 0;
      for (auto *control : panel.findChildren<ui::Knob *>())
        for (unsigned parameter = 1; parameter <= 6; ++parameter)
          if (control->objectName() ==
              QString::fromStdString(
                  parameterTable()[moduleOffsets[module] + parameter].id))
            visible += control->isVisible();
      disclosed &= more->isChecked() && visible == 6;
      more->click();
    }
    check(disclosed && same(before, panel.currentState()),
          "module disclosure exposes all six controls without editing audio "
          "state");
  }
  auto undo = controller.undoDepth();
  auto old = depth->value();
  QTest::keyClick(depth, Qt::Key_Right);
  check(controller.undoDepth() == undo + 1 &&
            controller.insertParameter(track, insert, "chorus.depth") != old,
        "keyboard edit has one undo");
  controller.undo();
  QTest::qWait(40);
  for (auto id : moduleOffsets) {
    QPushButton *power = nullptr;
    for (auto *candidate : panel.findChildren<QPushButton *>(
             QString::fromStdString(parameterTable()[id].id)))
      if (candidate->text() == QString::fromUtf8("⏻"))
        power = candidate;
    const auto before = panel.currentState().values[id];
    const auto history = controller.undoDepth();
    if (power) {
      QTest::keyClick(power, Qt::Key_Space);
      check(power->isChecked() == (before < .5) &&
                panel.currentState().values[id] != before &&
                controller.undoDepth() == history + 1,
            "vector power button is keyboard accessible and commits one undo");
      controller.undo();
      check(QTest::qWaitFor([&] { return power->isChecked() == (before > .5); },
                            1000),
            "power indicator follows undo state");
    } else {
      check(false, "module has a vector power button");
    }
  }
  check(panel.applyFactoryPreset(27), "factory preset loads");
  auto saved = panel.currentState();
  controller.undo();
  QTest::qWait(40);
  controller.redo();
  check(QTest::qWaitFor(
            [&] {
              return panel.currentState().canvas.seed == saved.canvas.seed &&
                     panel.currentState().values == saved.values;
            },
            1500),
        "preset undo/redo restores full Canvas state");
  auto mixed = panel.findChildren<ui::Knob *>("output.mix");
  check(mixed.size() == 2, "two mix knobs share parameter ID");
  if (mixed.size() == 2) {
    mixed[0]->editValue(.42);
    mixed[0]->finishEditing();
    QTest::qWait(40);
    check(mixed[1]->value() == .42, "mix controls stay synchronized");
  }
  auto *random = panel.findChild<QPushButton *>("morphRandom");
  check(random, "random sound button is available");
  if (random && mixed.size() == 2) {
    const auto beforeRandom = panel.currentState();
    const auto users = QSettings().value("Morph/users.v1");
    const auto history = controller.undoDepth();
    random->click();
    const auto firstRandom = panel.currentState();
    random->click();
    const auto secondRandom = panel.currentState();
    check(firstRandom.values != beforeRandom.values &&
              secondRandom.values != firstRandom.values &&
              firstRandom.values[Mix] == 1 && secondRandom.values[Mix] == 1 &&
              mixed[0]->value() == 1 && mixed[1]->value() == 1 &&
              secondRandom.presetKind == "custom" &&
              QSettings().value("Morph/users.v1") == users &&
              controller.undoDepth() == history + 2,
          "each Random click changes the sound at 100% mix without saving a "
          "preset");
    controller.undo();
    check(QTest::qWaitFor(
              [&] { return same(panel.currentState(), firstRandom); }, 1500),
          "one Undo restores the previous random sound");
    controller.undo();
    check(QTest::qWaitFor(
              [&] { return same(panel.currentState(), beforeRandom); }, 1500),
          "Random undo restores the original full state");
  }
  QImage image(320, 180, QImage::Format_ARGB32);
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      image.setPixelColor(x, y,
                          QColor::fromHsv(x * 359 / image.width(), 180,
                                          80 + y * 170 / image.height()));
  auto imagePath = temporary.path() + "/photo.png";
  image.save(imagePath);
  QSignalSpy spy(&panel, &MorphPanel::imageImportFinished);
  const auto beforeImage = panel.currentState();
  const auto imageHistory = controller.undoDepth();
  const auto userPresetsBeforeImage = QSettings().value("Morph/users.v1");
  panel.importImage(imagePath);
  check(QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000) &&
            !spy.empty() && spy.last()[0].toBool(),
        "image decoded and analyzed off GUI thread");
  auto imported = panel.currentState();
  unsigned changedParameters = 0;
  for (unsigned i = 0; i < MatrixStart; ++i)
    changedParameters += imported.values[i] != beforeImage.values[i];
  check(changedParameters >= 24 && imported.values[Mix] == 1 &&
            imported.values[Output] == beforeImage.values[Output] &&
            imported.canvas.seed == canvasFingerprint(imported.canvas) &&
            imported.presetKind == "custom" &&
            imported.presetName == "Image sound" &&
            QSettings().value("Morph/users.v1") == userPresetsBeforeImage,
        "photo creates a complete local sound without saving a user preset");
  check(controller.undoDepth() == imageHistory + 1,
        "image and generated parameters share one undo step");
  controller.undo();
  check(QTest::qWaitFor([&] { return same(beforeImage, panel.currentState()); },
                        1500),
        "image generation undo restores the previous sound and Canvas");
  controller.redo();
  check(QTest::qWaitFor([&] { return same(imported, panel.currentState()); },
                        1500),
        "image generation redo restores the prepared sound");
  panel.importImage(imagePath);
  check(QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000) &&
            same(imported, panel.currentState()),
        "loading the same photo again reproduces its complete sound");
  button(&panel, "Variation")->click();
  check(panel.currentState().values != imported.values &&
            panel.currentState().canvas.seed != imported.canvas.seed &&
            panel.currentState().canvas.rgba == imported.canvas.rgba &&
            panel.currentState().values[Mix] == 1,
        "image variation changes sound and seed while preserving the picture");
  controller.undo();
  check(QTest::qWaitFor([&] { return same(imported, panel.currentState()); },
                        1500),
        "image variation undoes as one operation");
  QFile::remove(imagePath);
  check(imported.canvas.painted && imported.canvas.previewWidth == 128 &&
            imported.canvas.previewHeight == 72,
        "embedded Canvas preserves image aspect");
  std::vector<std::uint8_t> bytes;
  encodeState(imported, bytes);
  auto clone = decodeState(bytes);
  check(clone && clone->canvas.rgba == imported.canvas.rgba,
        "preset works without original photo");
  auto beforeBad = panel.currentState();
  panel.importImage(temporary.path() + "/missing.png");
  check(QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000),
        "invalid import completes");
  check(panel.currentState().canvas.rgba == beforeBad.canvas.rgba,
        "bad import leaves program unchanged");
  if (!hardware) {
    for (const auto &format : {"jpg", "webp"}) {
      const auto file = temporary.path() + "/photo." + format;
      check(image.save(file), "image format test fixture saves");
      panel.importImage(file);
      check(
          QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000) &&
              spy.last()[0].toBool() &&
              panel.currentState().canvas.previewHeight == 72,
          "JPEG/WebP import preserves aspect");
    }
    {
      QFile jpeg(temporary.path() + "/photo.jpg");
      check(jpeg.open(QIODevice::ReadOnly), "orientation fixture opens");
      auto rotated = jpeg.readAll();
      // EXIF Orientation = 6 (rotate 90 degrees clockwise), little-endian TIFF.
      rotated.insert(
          2, QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010"
                                 "300010000000600000000000000"));
      QFile fixture(temporary.path() + "/rotated.jpg");
      check(fixture.open(QIODevice::WriteOnly) &&
                fixture.write(rotated) == rotated.size(),
            "EXIF orientation fixture saves");
      fixture.close();
      panel.importImage(fixture.fileName());
      check(
          QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000) &&
              panel.currentState().canvas.previewWidth == 72 &&
              panel.currentState().canvas.previewHeight == 128,
          "EXIF orientation is applied before Canvas interpretation");
      const auto portrait = panel.currentState();
      std::vector<std::uint8_t> portraitBytes;
      encodeState(portrait, portraitBytes);
      const auto reopened = decodeState(portraitBytes);
      check(
          (portrait.canvas.rgba[127 * 128 + 71] >> 24) == 255 && reopened &&
              reopened->canvas.rgba == portrait.canvas.rgba,
          "portrait image has no missing rows after analysis and project save");
    }
    QImage blue(64, 64, QImage::Format_ARGB32);
    blue.fill(QColor("#45c9ff"));
    const auto bluePath = temporary.path() + "/blue.png";
    blue.save(bluePath);
    panel.importImage(temporary.path() + "/photo.jpg");
    panel.importImage(bluePath);
    check(QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000) &&
              panel.currentState().canvas.rgba[0] == 0xff45c9ff,
          "latest image wins over an obsolete queued import");
    panel.importImage(bluePath);
    panel.applyFactoryPreset(27);
    const auto cancelled = panel.currentState();
    QTest::qWait(150);
    check(same(cancelled, panel.currentState()),
          "manual edit invalidates pending image job");
    button(&panel, "CANVAS")->click();
    auto *canvas = panel.findChild<QWidget *>("morphCanvas");
    auto before = panel.currentState();
    auto depthBefore = controller.undoDepth();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier,
                      canvas->rect().center());
    move(canvas, canvas->rect().center() + QPoint(24, 18));
    move(canvas, canvas->rect().center() + QPoint(39, -14));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier,
                        canvas->rect().center() + QPoint(45, -12));
    check(controller.undoDepth() == depthBefore + 1 &&
              before.canvas.rgba != panel.currentState().canvas.rgba,
          "a brush stroke is one undo operation");
    controller.undo();
    check(QTest::qWaitFor([&] { return same(before, panel.currentState()); },
                          1500),
          "brush undo restores map and picture");
    auto *ab = panel.findChild<QPushButton *>("morphAB");
    ab->click();
    check(!same(before, panel.currentState()), "A/B switches complete state");
    ab->click();
    check(same(before, panel.currentState()),
          "A/B restores Canvas, routing and parameters");
    panel.findChild<QPushButton *>("morphModulationDetails")->click();
    auto *pad = panel.findChild<QWidget *>("morphPad");
    auto *record = panel.findChild<QPushButton *>("morphRecord");
    depthBefore = controller.undoDepth();
    record->click();
    QElapsedTimer eventClock;
    eventClock.start();
    QTest::mousePress(pad, Qt::LeftButton, Qt::NoModifier, QPoint(158, 95), 0);
    QTest::qWait(30);
    const auto firstEventMs = eventClock.elapsed();
    move(pad, QPoint(194, 114));
    check(panel.currentState().values[MotionOn] == 0,
          "held recording follows direct input");
    QTest::qWait(120);
    const auto secondEventMs = eventClock.elapsed();
    move(pad, QPoint(128, 130));
    QTest::mouseRelease(pad, Qt::LeftButton, Qt::NoModifier, QPoint(128, 130));
    auto recorded = panel.currentState();
    check(controller.undoDepth() == depthBefore + 1 &&
              recorded.canvas.recorded && recorded.values[MotionOn] == 1 &&
              recorded.canvas.pointCount >= 3,
          "motion recording creates one undo and starts playback on release");
    if (recorded.canvas.pointCount >= 3) {
      // qWait can take longer while a high-DPI surface is first painted.
      // Verify the actual event intervals, not the requested wait durations.
      const double loopMs = 1000. * 4 * controller.timeSigNumerator() /
                            std::max(1, controller.timeSigDenominator()) *
                            (1u << unsigned(recorded.values[MotionBars])) * 60 /
                            std::max(1., controller.tempo());
      const double first =
          (recorded.canvas.path[1].time - recorded.canvas.path[0].time) *
          loopMs;
      const double second =
          (recorded.canvas.path[2].time - recorded.canvas.path[1].time) *
          loopMs;
      std::printf("MEASURE motion_ms events=%lld/%lld recorded=%.1f/%.1f\n",
                  static_cast<long long>(firstEventMs),
                  static_cast<long long>(secondEventMs - firstEventMs), first,
                  second);
      check(std::abs(first - firstEventMs) < 20 &&
                std::abs(second - (secondEventMs - firstEventMs)) < 20,
            "motion recording preserves the gesture rhythm");
    }
    controller.undo();
    check(QTest::qWaitFor([&] { return same(before, panel.currentState()); },
                          1500),
          "record undo restores original route");
    depthBefore = controller.undoDepth();
    record->click();
    QTest::mousePress(pad, Qt::LeftButton, Qt::NoModifier, QPoint(158, 90));
    panel.hide();
    check(controller.undoDepth() == depthBefore + 1 &&
              panel.currentState().canvas.pointCount >= 2 &&
              panel.currentState().values[MotionOn] == 1,
          "closing during a gesture commits a valid route once");
    panel.show();
    controller.undo();
    QTest::qWait(40);

    panel.findChild<QPushButton *>("morphModulationDetails")->click();
    panel.findChild<QScrollArea *>()->verticalScrollBar()->setValue(0);
    panel.applyFactoryPreset(30);
    check(menuAction(button(&panel, "Route"), {"Draw / edit route"}),
          "route editor is available from the menu");
    depthBefore = controller.undoDepth();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
    for (unsigned i = 0; i < 340; ++i)
      move(canvas, canvas->rect().center() +
                       QPoint(int(std::sin(i * .07) * canvas->width() * .35),
                              int(std::cos(i * .05) * canvas->height() * .25)));
    const QPoint end(canvas->width() - 20, canvas->height() - 20);
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, end);
    const auto longPath = panel.currentState().canvas;
    check(longPath.pointCount == MorphCanvasProgram::maxPoints &&
              std::abs(longPath.path[longPath.pointCount - 1].x -
                       (2. * end.x() / canvas->width() - 1)) < 1.e-4 &&
              controller.undoDepth() == depthBefore + 1 &&
              validProgram(longPath),
          "long gestures keep their ending in a bounded route and one undo");
    controller.undo();
    panel.applyFactoryPreset(27);

    // Exercise the same modal commands used by the editor, with all files and
    // preferences confined to the temporary test profile.
    QStringList answers;
    QString chosenFile;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout, &panel, [&] {
      if (auto *input =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
          input && !answers.empty()) {
        input->setTextValue(answers.takeFirst());
        input->accept();
      } else if (auto *file = qobject_cast<QFileDialog *>(
                     QApplication::activeModalWidget());
                 file && !chosenFile.isEmpty()) {
        file->selectFile(chosenFile);
        chosenFile.clear();
        QMetaObject::invokeMethod(file, "accept");
      }
    });
    dialogs.start(20);
    auto *presets = panel.findChild<QPushButton *>("morphPresetMenu");
    answers = {"Canvas test", "Keys"};
    check(menuAction(presets, {"Save user preset…"}),
          "save preset command is available");
    auto userState = panel.currentState();
    check(userState.presetKind == "user" &&
              userState.presetName == "Canvas test" &&
              userState.category == "Keys",
          "user preset saves its category");
    check(
        menuAction(presets, {"User presets", "Keys / Canvas test", "Favorite"}),
        "user preset can be marked as favorite");
    panel.applyFactoryPreset(0);
    check(menuAction(presets, {"Favorites", "Canvas test"}) &&
              same(userState, panel.currentState()),
          "favorite restores complete user preset");
    const auto presetFile = temporary.path() + "/portable.vltmorph";
    chosenFile = presetFile;
    check(menuAction(presets, {"Export .vltmorph…"}) &&
              QFile::exists(presetFile),
          "portable preset exports");
    for (const auto &expectedName : {"Canvas test (2)", "Canvas test (3)"}) {
      chosenFile = presetFile;
      check(menuAction(presets, {"Import .vltmorph…"}) &&
                panel.currentState().presetName == expectedName &&
                panel.currentState().canvas.rgba == userState.canvas.rgba,
            "repeated portable imports preserve state and use unique names");
    }
    answers = {"Renamed canvas"};
    check(menuAction(presets,
                     {"User presets", "Keys / Canvas test", "Rename…"}) &&
              menuAction(presets, {"Favorites", "Renamed canvas"}) &&
              panel.currentState().presetName == "Renamed canvas",
          "rename keeps favorite linked to its preset");
    check(menuAction(presets,
                     {"User presets", "Keys / Renamed canvas", "Delete"}),
          "user preset deletes");
    const auto entries = QJsonDocument::fromJson(
                             QSettings().value("Morph/users.v1").toByteArray())
                             .array();
    check(entries.size() == 2 && !QSettings()
                                      .value("Morph/favorites")
                                      .toStringList()
                                      .contains("user:Renamed canvas"),
          "delete removes only the selected user preset and favorite");
    dialogs.stop();
    panel.applyFactoryPreset(27);
    panel.importImage(temporary.path() + "/photo.jpg");
    check(QTest::qWaitFor([&] { return !panel.imageImportPending(); }, 5000),
          "Canvas ready for visual check");
  }
  const auto out = qEnvironmentVariable("MORPH_SCREENSHOTS");
  if (!out.isEmpty()) {
    QDir().mkpath(out);
    // Capture genuine engine telemetry, including the envelope and meters.
    // Do not substitute decorative values for the processor readout.
    auto tick = [&] {
      controller.seekSeconds(0);
      controller.play();
      audio::AudioBuffer silent(2, 257), rendered(2, 257);
      silent.clear();
      for (unsigned i = 0; i < 64; ++i) {
        controller.processDeviceBlockForTest(silent, rendered, 257);
        if (i % 4 == 0)
          QTest::qWait(34);
      }
      controller.stop();
      QTest::qWait(40);
    };
    if (!hardware)
      tick();
    panel.grab().save(out + "/morph-canvas.png");
    panel.applyFactoryPreset(25);
    for (auto *b : panel.findChildren<QPushButton *>())
      if (b->text() == "FLOW")
        b->click();
    if (!hardware)
      tick();
    QTest::qWait(60);
    panel.grab().save(out + "/morph-flow.png");
    panel.resize(panel.sizeHint());
    QTest::qWait(40);
    panel.grab().save(out + "/morph-default.png");
    panel.resize(1536, 864);
    QTest::qWait(40);
    auto *routing = panel.findChild<QComboBox *>("morph.routing");
    routing->showPopup();
    QTest::qWait(30);
    routing->view()->window()->grab().save(out + "/morph-dropdown.png");
    routing->hidePopup();
    QTimer::singleShot(30, &panel, [&] {
      if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        menu->grab().save(out + "/morph-presets.png");
        menu->close();
      }
    });
    panel.findChild<QPushButton *>("morphPresetMenu")->click();
    auto *advanced = panel.findChild<QPushButton *>("morphAdvanced");
    advanced->click();
    QTest::qWait(40);
    panel.findChild<QWidget *>("morphSurface")
        ->grab()
        .save(out + "/morph-effects.png");
    panel.findChild<QPushButton *>("morphModulationDetails")->click();
    for (unsigned m = 0; m < 4; ++m)
      panel.findChild<QPushButton *>(QString("morphModuleDetails%1").arg(m))
          ->click();
    QTest::qWait(40);
    panel.findChild<QWidget *>("morphSurface")
        ->grab()
        .save(out + "/morph-expanded.png");
    for (unsigned m = 0; m < 4; ++m)
      panel.findChild<QPushButton *>(QString("morphModuleDetails%1").arg(m))
          ->click();
    panel.findChild<QPushButton *>("morphModulationDetails")->click();
    advanced->click();
    panel.findChild<QScrollArea *>()->verticalScrollBar()->setValue(0);
  }
  if (hardware) {
    // Opt-in ten-minute device test. Keep the generated test tone inaudible;
    // the complete Morph chain still runs before the master output fader.
    controller.setMasterVolume(0);
    panel.applyFactoryPreset(29);
    controller.setLoopRangeSeconds(0, 1);
    controller.setLoopEnabled(true);
    daw::AutomationTarget target;
    target.kind = daw::AutomationTargetKind::PluginParameter;
    target.channelId = track;
    target.slotId = insert;
    target.parameterId = "morph.interaction";
    auto lane = controller.addAutomationLane(track, target);
    auto clip = controller.addAutomationClip(lane, target, 0, 1);
    controller.setAutomationPoints(lane, clip, {{0, .1}, {.5, .9}, {1, .1}});
    image.save(imagePath);
    controller.play();
    QTest::qWait(1000);
    const auto xruns = controller.audioXruns();
    const auto first = controller.audioDeviceDiagnostics();
    QElapsedTimer clock;
    clock.start();
    unsigned imports = 0;
    const int duration = hardwareSmoke ? 10000 : 600000;
    while (clock.elapsed() < duration) {
      QTest::qWait(50);
      controller.pumpPluginEvents();
      if (clock.elapsed() / 2000 > imports) {
        ++imports;
        panel.importImage(imagePath);
      }
      if (!controller.isDeviceOpen())
        break;
    }
    const auto last = controller.audioDeviceDiagnostics();
    const auto end = controller.audioXruns();
    controller.stop();
    std::printf(
        "HARDWARE duration_ms=%lld sample_rate=%.0f buffer=%u callbacks=%llu "
        "xruns=%llu/%llu/%llu/%llu failed=%llu\n",
        static_cast<long long>(clock.elapsed()), last.sampleRate,
        last.bufferSize,
        static_cast<unsigned long long>(last.callbacks - first.callbacks),
        static_cast<unsigned long long>(end[0] - xruns[0]),
        static_cast<unsigned long long>(end[1] - xruns[1]),
        static_cast<unsigned long long>(end[2] - xruns[2]),
        static_cast<unsigned long long>(end[3] - xruns[3]),
        static_cast<unsigned long long>(controller.failedAudioBlocks()));
    check(clock.elapsed() >= duration && last.callbacks > first.callbacks &&
              end == xruns && !controller.failedAudioBlocks(),
          hardwareSmoke
              ? "hardware smoke test with editor, automation and image imports"
              : "ten-minute hardware test with editor, automation and image "
                "imports");
  } else {
    for (double scale : {1., 1.5, 2.}) {
      QSettings().setValue("Morph/zoom", scale);
      panel.resize(1535, 864);
      panel.resize(1536, 864);
      QTest::qWait(40);
      auto *surface = panel.findChild<QWidget *>("morphSurface");
      auto *scroll = panel.findChild<QScrollArea *>();
      bool inside = true;
      for (auto *knob : panel.findChildren<ui::Knob *>())
        if (knob->isVisible())
          inside &= surface->rect().contains(knob->geometry());
      check(inside && surface->width() == int(1536 * scale) &&
                (scale == 1 || (scroll->horizontalScrollBar()->maximum() > 0 &&
                                scroll->verticalScrollBar()->maximum() > 0)),
            "100/150/200 percent layout preserves all controls with scrolling");
      if (!out.isEmpty() && scale > 1)
        surface->grab().save(
            out + QString("/morph-scale-%1.png").arg(int(scale * 100)));
    }
    QSettings().setValue("Morph/zoom", 0.);
    panel.resize(800, 600);
    QTest::qWait(40);
    check(panel.findChild<QScrollArea *>()->horizontalScrollBar()->maximum() >
              0,
          "small editor scrolls instead of rearranging sections");
    panel.resize(1536, 864);
    QTranslator russian;
    check(russian.load(QDir(QCoreApplication::applicationDirPath())
                           .filePath("../app/vlt_ru.qm")),
          "Russian translation loads");
    app.installTranslator(&russian);
    {
      MorphPanel translated(&controller, QString::fromStdString(track),
                            QString::fromStdString(insert));
      translated.resize(1536, 864);
      translated.show();
      translated.findChild<QPushButton *>("morphCanvasTab")->click();
      QTest::qWait(50);
      check(button(&translated, "Brush") && button(&translated, "Route"),
            "Canvas tools stay English with a Russian DAW translator");
      check(
          button(&translated, "CANVAS") && button(&translated, "Random") &&
              translated.findChild<ui::Knob *>("chorus.depth")
                      ->accessibleName() ==
                  QString::fromStdString(parameterTable()[ChorusDepth].name) &&
              translated.findChild<QComboBox *>("morph.routing")->itemText(1) ==
                  "Serial",
          "Morph controls, names and modes stay English in a Russian DAW");
      if (!out.isEmpty())
        translated.grab().save(out + "/morph-ru.png");
      translated.findChild<QPushButton *>("morphFlowTab")->click();
      QTest::qWait(40);
      if (!out.isEmpty())
        translated.grab().save(out + "/morph-ru-flow.png");
    }
    app.removeTranslator(&russian);
    auto expected = controller.morphSnapshot(track, insert);
    std::vector<std::string> copies;
    check(bool(controller.transferRackSelection(track, {insert}, track, 1, true,
                                                {}, &copies)) &&
              copies.size() == 1,
          "Morph copies through the normal insert rack");
    if (copies.size() == 1) {
      const auto copy = controller.morphSnapshot(track, copies[0]);
      check(copy && expected && same(copy->state, expected->state),
            "duplicate preserves the full Canvas program and preset");
      if (copy) {
        auto changed = copy->state;
        ++changed.canvas.seed;
        controller.applyMorphState(track, copies[0], changed, "Edit duplicate");
        check(same(controller.morphSnapshot(track, insert)->state,
                   expected->state),
              "duplicate owns an independent Canvas program");
      }
      controller.removeRackSelection(track, copies);
    }
    const auto package = temporary.path().toStdString() + "/Morph.vlt";
    controller.pumpPluginEvents();
    check(bool(controller.saveProject(package)), "local project saves Morph");
    daw::EngineController restored;
    restored.initialize(48000, 257, false);
    check(bool(restored.openProject(package)), "local project reopens");
    restored.pumpPluginEvents();
    auto actual = restored.morphSnapshot(track, insert);
    check(expected && actual &&
              actual->state.values == expected->state.values &&
              actual->state.canvas.rgba == expected->state.canvas.rgba &&
              actual->state.canvas.seed == expected->state.canvas.seed,
          "project restores complete Morph program");
    daw::rendering::Spec spec;
    spec.outputDir = temporary.path().toStdString();
    spec.baseName = "morph-export";
    spec.range = daw::rendering::Range::Custom;
    spec.customEndSeconds = 1;
    spec.blockSize = 257;
    spec.file.container = audio::platform::Container::Wav;
    spec.file.encoding = audio::platform::Encoding::Float32;
    daw::rendering::Report report;
    const auto exportResult = restored.renderProject(spec, {}, report);
    if (!exportResult)
      std::printf("EXPORT error: %s\n", exportResult.message().c_str());
    check(bool(exportResult) && !report.files.empty(),
          "offline export includes Morph");
    if (!report.files.empty()) {
      audio::platform::DecodedAudio exported;
      audio::platform::decodeAudioFile(report.files.front(), exported);
      auto graph = restored.routingGraph();
      for (const auto &n : graph->nodes)
        n.node->reset();
      for (const auto &delay : graph->delays)
        delay->reset();
      restored.seekSeconds(0);
      restored.play();
      audio::AudioBuffer in(2, 257), out(2, 257);
      in.clear();
      double error = exported.frames == 48000 ? 0 : 1.e9;
      for (unsigned at = 0; at < 48000; at += 257) {
        unsigned frames = std::min(257u, 48000 - at);
        restored.processDeviceBlockForTest(in, out, frames);
        if (exported.frames == 48000)
          for (unsigned f = 0; f < frames; ++f)
            for (unsigned ch = 0; ch < 2; ++ch)
              error = std::max(
                  error, std::abs(double(out.getChannel(ch)[f]) -
                                  exported.interleaved[(at + f) * 2 + ch]));
      }
      restored.stop();
      std::printf("MEASURE playback_export_max_error=%.9g\n", error);
      check(error < 2.e-5, "playback and offline export agree");
    }
  }
  panel.hide();
  check(!panel.visualUpdatesActive(), "hidden editor stops visual updates");
  check(failures == 0, "Morph editor integration complete");
  return failures ? 1 : 0;
}
