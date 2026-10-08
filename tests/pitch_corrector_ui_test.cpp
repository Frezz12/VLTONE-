#include "Internal/PitchCorrectorInstance.hpp"
#include "PitchCorrectorPanel.hpp"
#include "native_plugin_ui_test.hpp"
using namespace native_test;
int main(int argc, char **argv) {
  return run<PitchCorrectorPanel>(
      argc, argv,
      daw::plugins::pitch::PitchCorrectorInstance::staticDescriptor(),
      "humanize", {560, 360}, {660, 410},
      [](auto &panel, auto &view, auto &c, const auto &track,
         const auto &insert) {
        check(panel.template findChildren<ui::Knob *>().size() == 3,
              "three native pitch knobs");
        click(panel, "note-1");
        check(c.insertParameter(track, insert, "scale") == 7,
              "piano sets a custom scale");
        panel.applyFactoryPreset(3);
        events();
        check(c.insertParameter(track, insert, "tune") == 100,
              "Hard preset preserves retune mapping");
        child<QDoubleSpinBox>(panel, "tune-value")->setValue(80);
        events();
        check(std::abs(daw::plugins::pitch::retuneMilliseconds(
                           c.insertParameter(track, insert, "tune")) -
                       80) < 1.e-5,
              "retune milliseconds mapping");
        check(view.savePreset("Native vocal", false).isEmpty(),
              "user preset saves");
        emit view.factoryPreset(0);
        emit view.loadPreset("Native vocal");
        events();
        check(std::abs(daw::plugins::pitch::retuneMilliseconds(
                           c.insertParameter(track, insert, "tune")) -
                       80) < 1.e-5,
              "user preset restores values");
        const auto second = c.addInsert(
            track,
            daw::plugins::pitch::PitchCorrectorInstance::staticDescriptor());
        emit view.sendToAll();
        events();
        check(c.insertParameter(track, second, "tune") ==
                  c.insertParameter(track, insert, "tune"),
              "Send to all retained");
        c.play();
        events();
        check(!child<QComboBox>(panel, "quality")->isEnabled(),
              "quality locked during playback");
        c.stop();
        events();
      });
}
