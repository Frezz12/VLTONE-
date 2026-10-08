#pragma once
#include "Controls.hpp"
#include "EngineController.hpp"
#include "NativePluginView.hpp"
#include "PluginStyle.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Typography.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSettings>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdio>

namespace native_test {
inline int failures = 0;
inline void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  failures += !ok;
}
inline void events(int ms = 45) { QTest::qWait(ms); }
template <class T> T *child(QWidget &w, const char *name) {
  auto *c = w.findChild<T *>(name);
  check(c != nullptr, name);
  if (!c)
    std::abort();
  return c;
}
inline void select(QWidget &w, const char *name, int index) {
  auto *c = child<QComboBox>(w, name);
  c->setCurrentIndex(index);
  emit c->activated(index);
  events();
}
inline void click(QWidget &w, const char *name) {
  auto *b = child<QAbstractButton>(w, name);
  b->click();
  events();
}

template <class Panel, class Extra>
int run(int argc, char **argv, const daw::plugins::PluginDescriptor &descriptor,
        const char *parameter, QSize minimum, QSize normal, Extra extra) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QApplication app(argc, argv);
  ui::initializeApplicationFonts();
  check(ui::checkBundledFontsForTest(nullptr),
        "native UI fonts contain Latin and Cyrillic glyphs");
  QTemporaryDir temp;
  app.setProperty("dawHeadlessDataRoot", temp.path());
  app.setOrganizationName("VLTONE-Native-Test");
  app.setApplicationName(QString::fromStdString(descriptor.uid));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
  daw::EngineController controller{daw::EngineController::TestRuntime{}};
  check(bool(controller.initialize(48000, 257, false)),
        "headless engine initializes");
  audio::AudioBuffer source(2, 96000);
  for (unsigned i = 0; i < 96000; ++i)
    source.getChannel(0)[i] = source.getChannel(1)[i] =
        float(.3 * std::sin(i * .0576));
  const auto sourcePath = temp.path().toStdString() + "/source.wav";
  audio::AudioRecorder writer;
  writer.initialize(48000, 2);
  writer.writeWAVFile(sourcePath, source, 48000);
  const auto track = controller.importAudioToNewTrack(sourcePath, 0);
  const auto insert = controller.addInsert(track, descriptor);
  check(!insert.empty(), "built-in effect inserts through host");
  if (insert.empty())
    return 1;
  Panel panel(&controller, QString::fromStdString(track),
              QString::fromStdString(insert));
  panel.resize(normal);
  panel.show();
  events(120);
  auto *view = child<NativePluginView>(panel, "NativePluginView");
  bool native = true;
  for (auto *o : panel.template findChildren<QObject *>()) {
    const auto name = QByteArray(o->metaObject()->className());
    native &= !name.contains("WebEngine") && !name.contains("BrowserSurface") &&
              !name.contains("WebChannel");
  }
  check(native, "editor contains no browser surface, WebEngine or WebChannel");
  const auto read = [&](const char *id) {
    return controller.insertParameter(track, insert, id);
  };
  auto *knob = child<ui::Knob>(panel, parameter);
  const auto before = read(parameter);
  const auto depth = controller.undoDepth();
  const auto center = knob->rect().center();
  audio::AudioBuffer input(2, 257), output(2, 257);
  input.clear();
  QTimer pump;
  bool rendered = true;
  double energy = 0;
  QObject::connect(&pump, &QTimer::timeout, [&] {
    rendered &= controller.processDeviceBlockForTest(input, output, 257);
    for (unsigned i = 0; i < 257; ++i)
      energy += output.getChannel(0)[i] * output.getChannel(0)[i];
  });
  controller.play();
  pump.start(5);
  QTest::mousePress(knob, Qt::LeftButton, Qt::NoModifier, center);
  QTest::mouseMove(knob, center - QPoint(0, 15));
  events(100);
  const double moved = read(parameter);
  QTest::mouseMove(knob, center - QPoint(0, 30));
  events(100);
  check(read(parameter) > moved && moved > before,
        "native drag responds immediately across telemetry updates");
  QTest::mouseRelease(knob, Qt::LeftButton, Qt::NoModifier,
                      center - QPoint(0, 30));
  events();
  pump.stop();
  controller.stop();
  check(rendered && energy > 1,
        "audio renders continuously during native drag");
  check(controller.undoDepth() == depth + 1, "drag commits exactly one undo");
  controller.undo();
  events();
  check(std::abs(read(parameter) - before) < 1.e-6,
        "undo restores pre-drag value");
  controller.redo();
  events();
  check(read(parameter) > before, "redo restores gesture");
  controller.undo();
  events();
  QTest::keyClick(knob, Qt::Key_Up);
  events();
  check(read(parameter) > before, "keyboard edits focused native knob");
  controller.undo();
  events();
  QSignalSpy automation(&panel, &Panel::automationRequested);
  emit knob->automateRequested();
  check(automation.count() == 1 && automation.at(0)[0].toString() == parameter,
        "automation retains stable parameter ID");
  auto *accessible = QAccessible::queryAccessibleInterface(knob);
  check(accessible && accessible->valueInterface() &&
            !accessible->text(QAccessible::Name).isEmpty(),
        "native knob exposes accessible value and name");
  auto *number =
      child<QDoubleSpinBox>(panel, (std::string(parameter) + "-value").c_str());
  panel.activateWindow();
  QApplication::setActiveWindow(&panel);
  number->setFocus();
  number->selectAll();
  const QString typed =
      number->locale().toString(number->maximum() <= 0 ? -24.7 : 42.7, 'f', 1);
  QTest::keyClicks(number, typed);
  events(150);
  check(number->text().contains(typed),
        "telemetry preserves uncommitted numeric text");
  QTest::keyClick(number, Qt::Key_Return);
  events();
  const double scale =
      knob->maximumValue() != 0 ? number->maximum() / knob->maximumValue() : 1;
  check(std::abs(read(parameter) - number->locale().toDouble(typed) / scale) <
            1.e-6,
        "localized numeric edit reaches host");
  extra(panel, *view, controller, track, insert);
  const auto savedValue = read(parameter);
  controller.setInsertBypassed(track, insert, true);
  const auto path = temp.path().toStdString() + "/native.vlt";
  check(bool(controller.saveProject(path)),
        "project saves native editor state");
  {
    daw::EngineController reopened{daw::EngineController::TestRuntime{}};
    reopened.initialize(48000, 257, false);
    check(bool(reopened.openProject(path)) &&
              std::abs(reopened.insertParameter(track, insert, parameter) -
                       savedValue) < 1.e-6 &&
              reopened.insertModel(track, insert)->bypassed,
          "project reload preserves values and bypass");
  }
  controller.setInsertBypassed(track, insert, false);
  const auto copy = controller.copyChannelStrip(track, false);
  const auto other = controller.addTrack(daw::TrackKind::Audio, "Copy");
  check(controller.pasteChannelInserts(other, copy),
        "native editor effect can be copied");
  const auto copied = controller.project().findTrack(other)->inserts.front().id;
  check(std::abs(controller.insertParameter(other, copied, parameter) -
                 savedValue) < 1.e-6,
        "copy retains parameter state");
  panel.hide();
  events();
  bool stopped = true;
  for (auto *t : panel.template findChildren<QTimer *>())
    if (t->objectName().endsWith("TelemetryTimer"))
      stopped &= !t->isActive();
  check(stopped, "hidden editor stops telemetry");
  panel.show();
  events();
  const auto dir = qEnvironmentVariable("VLT_NATIVE_SCREENSHOTS");
  if (!dir.isEmpty())
    QDir().mkpath(dir);
  for (const auto size :
       {minimum, normal,
        QSize(normal.width() * 5 / 4, normal.height() * 5 / 4)}) {
    panel.resize(size);
    events(80);
    // Windows constrains top-level windows to the monitor's logical work area
    // at high scaling; still require every control to fit the resulting size.
    bool fits = app.platformName() == "offscreen" ? panel.size() == size
      : panel.width() >= minimum.width() && panel.height() >= minimum.height();
    for (auto *widget : panel.template findChildren<QWidget *>())
      if (widget->isVisibleTo(&panel) && !widget->isWindow() &&
          (qobject_cast<ui::Knob *>(widget) ||
           qobject_cast<QAbstractButton *>(widget) ||
           qobject_cast<QComboBox *>(widget) ||
           qobject_cast<QDoubleSpinBox *>(widget))) {
        const QRect bounds(widget->mapTo(&panel, QPoint()), widget->size());
        fits &= panel.rect().contains(bounds);
      }
    check(fits, "controls fit minimum, normal and enlarged editor");
    auto shot = panel.grab();
    check(!shot.isNull(), "native face paints");
    if (!dir.isEmpty())
      shot.save(dir + "/" + QString::fromStdString(descriptor.uid) +
                QString("-%1x%2.png").arg(size.width()).arg(size.height()));
  }
  const auto originalTheme = th();
  for (bool dark : {false, true}) {
    for (const auto &candidate : ThemeManager::instance().presets()) {
      if (candidate.dark != dark)
        continue;
      ThemeManager::instance().setThemeId(candidate.id, false);
      events();
      check(view->palette().color(QPalette::Window) == pluginStyle::shell() &&
                view->palette().color(QPalette::Text) == th().textPrimary,
            "open native editor follows light and dark theme changes");
      check(std::abs(read(parameter) - savedValue) < 1.e-6,
            "theme changes preserve processor parameters");
      if (!dir.isEmpty())
        panel.grab().save(dir + "/" + QString::fromStdString(descriptor.uid) +
                          (dark ? "-dark.png" : "-light.png"));
      break;
    }
  }
  ThemeManager::instance().applyCustomTheme(originalTheme, false);
  if (qEnvironmentVariableIntValue("VLT_NATIVE_TEST_WORKSPACE")) {
    ui::graphics::WorkspaceSurface surface(&panel);
    events(200);
    auto* window = surface.quickWindow();
    panel.activateWindow();
    window->requestActivate();
    events(100);
    const double old = knob->value();
    const auto pointer = [&](QEvent::Type type, QPoint offset,
                             Qt::MouseButton button, Qt::MouseButtons buttons) {
      const auto global = knob->mapToGlobal(knob->rect().center() + offset);
      QMouseEvent event(type, QPointF(window->mapFromGlobal(global)), QPointF(global),
                        button, buttons, Qt::NoModifier);
      QApplication::sendEvent(window, &event);
      events(40);
    };
    pointer(QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, {0,-12}, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, {0,-12}, Qt::LeftButton, Qt::NoButton);
    check(knob->value()>old,"GPU workspace forwards native knob input");
    if (descriptor.uid == "daw.delay") {
      auto* canvas = child<QWidget>(panel, "echo-display");
      const char* timeId = read("timeMode") == 2 ? "timeMs" : "division";
      const double oldTime = read(timeId);
      const auto depth = controller.undoDepth();
      const auto canvasPointer = [&](QEvent::Type type, QPoint point,
                                     Qt::MouseButton button, Qt::MouseButtons buttons) {
        const auto global = canvas->mapToGlobal(point);
        QMouseEvent event(type, QPointF(window->mapFromGlobal(global)), QPointF(global),
                          button, buttons, Qt::NoModifier);
        QApplication::sendEvent(window, &event);
        events(40);
      };
      canvasPointer(QEvent::MouseButtonPress, {40,27}, Qt::LeftButton, Qt::LeftButton);
      canvasPointer(QEvent::MouseMove, {canvas->width()-55,27}, Qt::NoButton, Qt::LeftButton);
      canvasPointer(QEvent::MouseButtonRelease, {canvas->width()-55,27}, Qt::LeftButton, Qt::NoButton);
      check(read(timeId) != oldTime && controller.undoDepth() == depth+1,
            "GPU workspace forwards echo canvas gesture as one undo");
    }
    const auto frame=window->grabWindow();
    check(!frame.isNull(),"GPU workspace renders native plugin face");
    if(!dir.isEmpty()) frame.save(dir+"/"+QString::fromStdString(descriptor.uid)+"-workspace.png");
  }
  controller.removeInsert(track, insert);
  events();
  check(!view->isEnabled(), "removed effect disables its editor safely");
  return failures ? 1 : 0;
}
} // namespace native_test
