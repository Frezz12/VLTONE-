#include "DelayPanel.hpp"
#include "native_plugin_ui_test.hpp"
#include "serialization/InsertJson.hpp"
#include <nlohmann/json.hpp>
using namespace native_test;
int main(int argc, char **argv) {
  return run<DelayPanel>(
      argc, argv, daw::plugins::delay::DelayInstance::staticDescriptor(),
      "feedback", {960, 420}, {1100, 460},
      [](auto &panel, auto &view, auto &c, const auto &track,
         const auto &insert) {
        auto saved =
            daw::serialization::insertToJson(*c.insertModel(track, insert));
        saved["name"] = "Flowers Delay";
        const auto restored = daw::serialization::insertFromJson(saved);
        check(restored.name == "Classic Delay" && restored.uid == "daw.delay" &&
                  restored.id == insert,
              "legacy Delay name migrates without changing insert identity");
        saved["name"] = "Vocal echo";
        check(daw::serialization::insertFromJson(saved).name == "Vocal echo",
              "custom Delay names survive loading");
        check(panel.template findChildren<ui::Knob *>().size() == 9,
              "nine native Delay knobs");
        check(child<QComboBox>(panel, "character")->count() == 7 &&
                  child<QComboBox>(panel, "division")->count() == 23,
              "all characters and divisions preserved");
        c.setTempo(90);
        // Under a concurrent build a visible telemetry tick may be delayed.
        // Wait for the observable result, with a bounded failure deadline.
        for (int i = 0;
             i < 50 &&
             child<QLabel>(panel, "time-display")->text() != "500.0 ms";
             ++i)
          events(10);
        check(child<QLabel>(panel, "time-display")->text() == "500.0 ms",
              "host tempo reaches native display");
        click(panel, "timeMode1");
        click(panel, "tap");
        events(420);
        click(panel, "tap");
        check(c.insertParameter(track, insert, "bpm") > 70 && c.tempo() == 90,
              "Tap changes local BPM only");
        click(panel, "timeMode2");
        child<QDoubleSpinBox>(panel, "timeMs-value")->setValue(100);
        events();
        check(c.insertParameter(track, insert, "timeMs") == 100,
              "milliseconds reach DSP");
        select(panel, "character", 3);
        check(c.insertParameter(track, insert, "character") == 3 &&
                  c.insertParameter(track, insert, "timeMs") == 100,
              "character change preserves time");
        auto *screen = child<QWidget>(panel, "echo-display");
        const auto drag = [&](QPoint from, QPoint to, const char *id) {
          const auto before = c.insertParameter(track, insert, id);
          const auto depth = c.undoDepth();
          QTest::mousePress(screen, Qt::LeftButton, Qt::NoModifier, from);
          QTest::mouseMove(screen, to);
          events(80);
          QTest::mouseRelease(screen, Qt::LeftButton, Qt::NoModifier, to);
          check(c.insertParameter(track, insert, id) != before,
                "echo canvas drag edits parameter during telemetry");
          check(c.undoDepth() == depth + 1, "echo drag is one undo gesture");
          c.undo();
          events();
          check(c.insertParameter(track, insert, id) == before,
                "echo drag undo restores parameter");
        };
        drag({50, 27}, {screen->width() - 65, 27}, "timeMs");
        drag({screen->width() - 18, 75},
             {screen->width() - 18, screen->height() - 65}, "feedback");
        drag({50, screen->height() - 25},
             {screen->width() - 65, screen->height() - 25}, "mix");
        click(panel, "timeMode0");
        const auto milliseconds = c.insertParameter(track, insert, "timeMs");
        drag({50, 27}, {screen->width() - 65, 27}, "division");
        check(c.insertParameter(track, insert, "timeMs") == milliseconds,
              "synced canvas edits division without changing free time");
        const auto feedback = c.insertParameter(track, insert, "feedback");
        QTest::keyClick(screen, Qt::Key_Up);
        events();
        check(c.insertParameter(track, insert, "feedback") == feedback + 1,
              "echo canvas supports keyboard feedback adjustment");
        QSettings().setValue("ui/reduceMotion", false);
        c.play();
        events();
        check(screen->property("echoAnimating").toBool(),
              "echo motion follows playback");
        QSettings().setValue("ui/reduceMotion", true);
        events();
        check(!screen->property("echoAnimating").toBool(),
              "reduced motion disables travelling pulses");
        QSettings().setValue("ui/reduceMotion", false);
        events();
        c.stop();
        events();
        check(!screen->property("echoAnimating").toBool(),
              "stopped transport stops echo motion");
        c.play();
        events();
        panel.hide();
        events();
        check(!screen->property("echoAnimating").toBool(),
              "hidden editor stops echo motion");
        c.stop();
        panel.show();
        events();
      });
}
