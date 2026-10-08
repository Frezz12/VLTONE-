#include "ModulationPanel.hpp"
#include "native_plugin_ui_test.hpp"
using namespace native_test;
using Rack = daw::plugins::modulation::ModulationRackInstance;
int main(int argc, char **argv) {
  return run<ModulationPanel>(
      argc, argv, Rack::staticDescriptor(), "chorus.depth", {800, 490},
      {980, 540},
      [](auto &panel, auto &view, auto &c, const auto &track,
         const auto &insert) {
        check(panel.template findChildren<ui::Knob *>().size() == 15,
              "all rack knobs use native controls");
        emit view.reorder(0, 3);
        events();
        check(Rack::decodeOrder(unsigned(c.insertParameter(
                  track, insert, "order"))) == Rack::Order{1, 2, 3, 0},
              "reorder preserves module identities");
        c.undo();
        events();
        click(panel, "eq-toggle");
        select(panel, "eq-band", 2);
        child<QDoubleSpinBox>(panel, "eq-gain")->setValue(7);
        events();
        check(c.insertParameter(
                  track, insert,
                  Rack::parameterTable()[Rack::eqOffset + 2 * 4 + 2].id) == 7,
              "EQ uses rack parameter indices");
        click(panel, "eq-toggle");
        check(view.savePreset("Native rack", false).isEmpty(),
              "rack preset saves");
        panel.applyFactoryPreset(0);
        emit view.loadPreset("Native rack");
        events();
        check(c.insertParameter(
                  track, insert,
                  Rack::parameterTable()[Rack::eqOffset + 2 * 4 + 2].id) == 7,
              "rack preset restores EQ");
        check(view.renamePreset("Native rack", "Renamed rack").isEmpty(),
              "preset rename retained");
        for (int i = 0; i < daw::plugins::modulation::kindCount; ++i) {
          const auto kind = daw::plugins::modulation::Kind(i);
          const auto &descriptor =
              daw::plugins::modulation::descriptorFor(kind);
          const auto id = c.addInsert(track, descriptor);
          ModulationPanel single(&c, QString::fromStdString(track),
                                 QString::fromStdString(id));
          single.resize(380, 560);
          single.show();
          events();
          check(single.findChildren<ui::Knob *>().size() ==
                    daw::plugins::modulation::parameterTable(kind).size(),
                "standalone effect exposes every native parameter");
          const auto dir = qEnvironmentVariable("VLT_NATIVE_SCREENSHOTS");
          if (!dir.isEmpty())
            single.grab().save(dir + "/" +
                               QString::fromStdString(descriptor.uid) + ".png");
        }
      });
}
