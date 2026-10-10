#include "ChannelStrip.hpp"
#include "CollaborationCommandBridge.hpp"
#include "collaboration/CommandGateway.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "MiniModuleLibrary.hpp"
#include "MiniModuleRack.hpp"
#include "MixerPreferences.hpp"
#include "MixerWidget.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Theme.hpp"
#include "model/ChannelColor.hpp"
#include <QAccessible>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

bool ChannelStrip::checkColorForTest() {
  bool ok = true;
  const auto check = [&](bool result, const char *label) {
    std::printf("%s %s\n", result ? "PASS" : "FAIL", label);
    ok &= result;
  };
  daw::collab::CommandGateway gateway;
  ::collab::CollaborationCommandBridge bridge(nullptr, &gateway);
  daw::EngineController controller{};
  if (!controller.initialize(48000, 256, false))
    return false;
  controller.attachSharedMutationSink(bridge);
  check(!controller.hasCloudProjectBinding(), "rack uses an attached local application bridge");
  auto &prefs = ui::MixerPreferences::instance();
  const bool wasVisible = prefs.colorVisible();
  prefs.setColorVisible(true);
  // Old saved collapse state must not hide controls after removing the toggle.
  QSettings().setValue("mixer/colorExpanded", false);
  const auto id = controller.addTrack(daw::TrackKind::Audio, "Mini modules");
  ChannelStrip strip(&controller, QString::fromStdString(id), false);
  strip.show();
  QApplication::processEvents();
  auto *rack = strip.findChild<ui::MiniModuleRack *>();
  check(rack && rack->findChildren<ui::Knob *>().empty(),
        "new channel shows empty rack");
  const auto depthBeforeAdd = controller.undoDepth();
  QTimer::singleShot(0, &strip, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu) return;
    for (auto *action : menu->actions())
      if (action->text() == "Color") {
        action->trigger();
        break;
      }
    menu->close();
  });
  rack->findChild<QPushButton *>("AddMiniModule")->click();
  QApplication::processEvents();
  const bool added = controller.miniModules(id).size() == 1 &&
                     controller.undoDepth() == depthBeforeAdd + 1 &&
                     rack->findChild<ui::Knob *>("drive");
  check(added, "choosing a built-in module in the real menu inserts a visible card with the app bridge");
  if (!added) return false;
  const auto color = controller.miniModules(id).front().id;
  const auto doubler =
      controller.addMiniModule(id, daw::plugins::mini::builtin("doubler"));
  controller.addMiniModule(id, daw::plugins::mini::builtin("chorus"));
  strip.syncFromModel();
  QApplication::processEvents();
  auto *drive = rack->findChild<ui::Knob *>("drive");
  auto *tone = rack->findChild<ui::Knob *>("tone");
  if (!drive || !tone)
    return false;
  check(rack->findChildren<ui::Knob *>().size() == 6 && drive->value() == -20 &&
            tone->value() == 0,
        "three cards expose six controls and mild defaults");
  const auto before = controller.undoDepth();
  QKeyEvent key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QApplication::sendEvent(drive, &key);
  check(drive->value() > -20 && controller.undoDepth() == before + 1,
        "keyboard changes one parameter with one Undo");
  drive->editValue(-37.125);
  check(std::abs(controller.insertParameter(id, color, "drive") + 37.125) <
            1e-9,
        "precise numeric edit reaches stable external parameter");
  const auto mouse = [](QWidget *widget, QEvent::Type type, int y,
                        Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPointF local(widget->width() / 2., y);
    QMouseEvent e(type, local, QPointF(widget->mapToGlobal(local.toPoint())),
                  button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &e);
  };
  const auto gesture = controller.undoDepth();
  mouse(drive, QEvent::MouseButtonPress, 20, Qt::LeftButton, Qt::LeftButton);
  mouse(drive, QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
  const double held = drive->value();
  strip.syncFromModel();
  check(drive->value() == held, "model refresh preserves active gesture");
  strip.hide();
  QApplication::processEvents();
  check(!drive->isEditing() && controller.undoDepth() == gesture + 1,
        "hiding commits active gesture exactly once");
  strip.show();
  auto *accessible = QAccessible::queryAccessibleInterface(drive);
  check(accessible && accessible->role() == QAccessible::Slider &&
            accessible->valueInterface(),
        "native knob accessibility is preserved");
  ChannelStrip inspector(&controller, QString::fromStdString(id), false);
  inspector.show();
  auto *caption = rack->findChild<QLabel *>("ChannelSectionCaption");
  check(caption && caption->text() == ui::MiniModuleRack::tr("MINI") &&
            caption->font().pixelSize() <= 10 && caption->height() == 17 &&
            !rack->findChild<QPushButton *>("MiniRackExpand"),
        "MINI is a small static section caption matching FX and sends");
  if (auto *title = rack->findChild<QPushButton *>("MiniModuleTitle")) title->click();
  QApplication::processEvents();
  check(drive->isVisible() &&
            inspector.findChild<ui::Knob *>("drive")->isVisible() &&
            rack->findChild<ui::Knob *>("width")->isVisible(),
        "all cards remain expanded in mixer and inspector, including old collapsed preferences");
  int automated = 0;
  connect(&strip, &ChannelStrip::automatePluginRequested, &strip,
          [&](const QString &, const QString &slot, const QString &parameter) {
            if (slot.toStdString() == color && parameter == "drive")
              ++automated;
          });
  QMetaObject::invokeMethod(drive, "automateRequested");
  check(automated == 1, "automation uses stable module and parameter IDs");
  {
    auto *route = rack->findChild<QComboBox *>("MiniModulePosition");
    auto *mode = rack->findChild<QComboBox *>("MiniModuleMode");
    const auto depth = controller.undoDepth();
    route->setCurrentIndex(1);
    QMetaObject::invokeMethod(route, "activated", Q_ARG(int, 1));
    mode->setCurrentIndex(2);
    QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 2));
    strip.syncFromModel();
    check(controller.miniModules(id)[0].miniModulePostFx &&
              controller.miniModules(id)[0].miniModuleMode == "tube" &&
              controller.undoDepth() == depth + 2,
          "native selectors update route and mode with one Undo each");
    controller.undo();
    controller.undo();
    strip.syncFromModel();
  }
  QTemporaryDir files;
  QString error;
  auto definition = daw::plugins::mini::builtin("chorus");
  QImage background(32, 32, QImage::Format_ARGB32);
  background.fill(QColor("#355a72"));
  QByteArray imageData;
  QBuffer imageBuffer(&imageData);
  imageBuffer.open(QIODevice::WriteOnly);
  background.save(&imageBuffer, "PNG");
  definition.appearance.backgroundImage =
      "data:image/png;base64," + imageData.toBase64().toStdString();
  definition.appearance.controlStyle = "glass";
  definition.defaultMode = "ensemble";
  const auto file = files.path() + "/Shared.vltmini";
  check(ui::MiniModuleLibrary::write(file, definition, error) &&
            ui::MiniModuleLibrary::read(file).definition == definition,
        "portable file includes modes style and embedded image");
  check(ui::MiniModuleLibrary::entries(files.path()).size() == 1,
        "dropped module file is discovered without restart");
  const auto malformed = files.path() + "/Unavailable.vltmini";
  QFile invalidFile(malformed);
  invalidFile.open(QIODevice::WriteOnly);
  invalidFile.write("{\"format\":27}");
  invalidFile.close();
  check(!ui::MiniModuleLibrary::read(malformed).error.isEmpty() &&
            ui::MiniModuleLibrary::entries(files.path()).size() == 2,
        "invalid shared file remains visible with explanation");
  const auto shots = qEnvironmentVariable("DAW_COLOR_SCREENSHOTS");
  for (int width : {75, 100, 150, 180}) {
    strip.setStripWidth(width);
    QApplication::processEvents();
    bool fits = strip.width() == width;
    if (width >= 100)
      fits &=
          drive->mapTo(rack, QPoint()).y() == tone->mapTo(rack, QPoint()).y();
    for (auto *knob : rack->findChildren<ui::Knob *>()) {
      const QPoint at = knob->mapTo(&strip, QPoint());
      fits &=
          at.x() >= 0 && at.x() + knob->width() <= width && knob->width() >= 32;
    }
    for (auto *label : rack->findChildren<QLabel *>()) {
      const int required =
          label->fontMetrics().horizontalAdvance(label->text()) + 2;
      if (required > label->width() || label->text().contains(QChar(0x2026)))
        std::printf("Label '%s' needs %d px, has %d px in %d px strip\n",
                    label->text().toUtf8().constData(), required,
                    label->width(), width);
      fits &=
          required <= label->width() && !label->text().contains(QChar(0x2026));
    }
    if (width >= 100)
      for (auto *title : rack->findChildren<QPushButton *>("MiniModuleTitle"))
        fits &= title->fontMetrics().horizontalAdvance(title->text()) <=
                title->width();
    check(fits, "all module controls and values fit readable strip widths");
    if (!shots.isEmpty()) {
      QDir().mkpath(shots);
      check(strip.grab().save(shots + QString("/mini-%1.png").arg(width)),
            "mini rack screenshot saved");
      check(rack->grab().save(shots + QString("/rack-%1.png").arg(width)),
            "inserted module well screenshot saved");
    }
  }
  const auto removalUndo = controller.undoDepth();
  mouse(drive, QEvent::MouseButtonPress, 20, Qt::LeftButton, Qt::LeftButton);
  mouse(drive, QEvent::MouseMove, 5, Qt::NoButton, Qt::LeftButton);
  controller.removeMiniModule(id, color);
  strip.syncFromModel();
  QApplication::processEvents();
  check(!rack->findChild<ui::Knob *>("drive") &&
            controller.undoDepth() == removalUndo + 1,
        "removing an active control ends the gesture without an orphan Undo");
  controller.undo();
  strip.syncFromModel();
  check(rack->findChild<ui::Knob *>("drive") != nullptr,
        "Undo restores a removed module after a gesture");
  strip.setStripWidth(100);
  for (bool accept : {false, true}) {
    const auto beforeLook =
        controller.miniModules(id)[0].miniModule->appearance;
    bool opened = false;
    QTimer::singleShot(0, &strip, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      auto *action = menu->findChild<QAction *>("MiniModuleAppearance");
      if (!action) {
        menu->close();
        return;
      }
      QTimer::singleShot(0, &strip, [&] {
        auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
          return;
        auto *theme = dialog->findChild<QComboBox *>("MiniAppearanceTheme");
        auto *control = dialog->findChild<QComboBox *>("MiniAppearanceControl");
        opened = theme && control;
        if (opened) {
          theme->setCurrentIndex(theme->findData("ivory"));
          control->setCurrentIndex(control->findData("fader"));
          QApplication::processEvents();
          const auto *preview =
              dialog->findChild<QWidget *>("MiniModuleAppearancePreview");
          opened &= preview && preview->isVisible() &&
                    preview->width() == 150 &&
                    preview->findChildren<ui::Knob *>().size() == 2;
        }
        if (opened && accept && !shots.isEmpty())
          dialog->grab().save(shots + "/appearance-dialog.png");
        dialog->done(accept ? QDialog::Accepted : QDialog::Rejected);
      });
      action->trigger();
      menu->close();
    });
    if (auto *menuButton = rack->findChild<ui::IconButton *>("MiniModuleMenu"))
      menuButton->click();
    QApplication::processEvents();
    const auto &afterLook =
        controller.miniModules(id)[0].miniModule->appearance;
    check(opened && (accept ? afterLook.theme == "ivory" &&
                                  afterLook.controlStyle == "fader"
                            : afterLook == beforeLook),
          "appearance dialog previews and accepts or cancels native edits");
  }
  for (const auto *style : {"machined", "rubber", "glass", "fader"}) {
    auto look = controller.miniModules(id)[0].miniModule->appearance;
    look.controlStyle = style;
    look.theme = "copper";
    look.backgroundImage = definition.appearance.backgroundImage;
    controller.setMiniModuleAppearance(id, color, look);
    strip.syncFromModel();
    QApplication::processEvents();
    auto *dial = rack->findChild<ui::Knob *>("drive");
    check(dial && dial->isVisible() && dial->width() == 36,
          "all visual control types keep native interaction and readable size");
    if (!shots.isEmpty())
      check(rack->grab().save(shots + "/style-" + style + ".png"),
            "custom module style screenshot saved");
  }
  for (auto kind : {daw::TrackKind::Bus, daw::TrackKind::Group,
                    daw::TrackKind::Aux, daw::TrackKind::Pattern}) {
    const auto other = controller.addTrack(kind, "Bus");
    ChannelStrip channel(&controller, QString::fromStdString(other), false);
    check(channel.findChild<ui::MiniModuleRack *>() != nullptr,
          "audio summing channels expose rack");
  }
  ChannelStrip master(&controller, QStringLiteral("master"), true);
  check(master.findChild<ui::MiniModuleRack *>() != nullptr,
        "Master exposes rack");
  {
    daw::EngineController large{};
    large.initialize(48000, 256, false);
    std::string first, last;
    for (int i = 0; i < 40; ++i) {
      last = large.addTrack(daw::TrackKind::Audio, "Channel");
      if (i == 0)
        first = last;
    }
    for (const auto *type : {"color", "doubler", "chorus"})
      large.addMiniModule(last, daw::plugins::mini::builtin(type));
    MixerWidget mixer(&large);
    mixer.resize(640, 900);
    mixer.show();
    const auto settle = [] {
      QEventLoop loop;
      QTimer::singleShot(70, &loop, &QEventLoop::quit);
      loop.exec();
    };
    settle();
    const auto find = [&](const std::string &id) -> ChannelStrip * {
      for (auto *s : mixer.findChildren<ChannelStrip *>())
        if (s->trackId().toStdString() == id)
          return s;
      return nullptr;
    };
    auto *initial = find(first);
    const int row = initial ? initial->m_rackSections[1]->height() : -1;
    const int expected = ui::MiniModuleRack::naturalHeight(
        large.project(), large.miniModules(last), mixer.channelWidth());
    check(row >= expected,
          "invisible far rack reserves height before horizontal scrolling");
    if (initial) {
      auto *empty = initial->findChild<ui::MiniModuleRack *>();
      check(empty && empty->height() == row &&
                empty->findChild<QWidget *>("SlotWell")->height() > 400,
            "empty rack well expands to the shared card area");
    }
    large.removeMiniModule(last, large.miniModules(last).back().id);
    mixer.syncFromModel({QString::fromStdString(last)});
    settle();
    initial = find(first);
    check(
        initial && initial->m_rackSections[1]->height() < row,
        "editing an unmaterialized rack updates visible alignment immediately");
    large.addMiniModule(last, daw::plugins::mini::builtin("chorus"));
    mixer.syncFromModel({QString::fromStdString(last)});
    settle();
    auto* channels = mixer.findChild<QScrollArea *>("MixerChannelsScroll");
    QScrollBar *horizontal = channels ? channels->horizontalScrollBar() : nullptr;
    if (horizontal)
      horizontal->setValue(horizontal->maximum());
    settle();
    auto *end = find(last);
    check(horizontal && end && end->m_rackSections[1]->height() == row,
          "virtualized strip materialization keeps rack alignment");
    if (horizontal)
      horizontal->setValue(0);
    settle();
    initial = find(first);
    check(initial && initial->m_rackSections[1]->height() == row,
          "returning to earlier strips keeps rack height stable");
  }
  prefs.setColorVisible(wasVisible);
  return ok;
}
