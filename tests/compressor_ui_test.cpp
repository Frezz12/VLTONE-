#include "CompressorPanel.hpp"
#include "native_plugin_ui_test.hpp"
using namespace native_test;
int main(int argc, char **argv) {
  return run<CompressorPanel>(
      argc, argv,
      daw::plugins::compressor::CompressorInstance::staticDescriptor(),
      "threshold", {800, 340}, {920, 360},
      [](auto &panel, auto &, auto &c, const auto &track, const auto &insert) {
        check(panel.template findChildren<ui::Knob *>().size() == 7,
              "seven native compressor knobs");
        click(panel, "mode1");
        click(panel, "autoGain");
        check(c.insertParameter(track, insert, "mode") == 1 &&
                  c.insertParameter(track, insert, "autoGain") == 1,
              "Punch and Auto Gain reach DSP");
        auto *graph = child<QWidget>(panel, "transfer-curve");
        const auto before = c.insertParameter(track, insert, "threshold");
        QTest::mousePress(graph, Qt::LeftButton, Qt::NoModifier, {55, 90});
        QTest::mouseMove(graph, {80, 90});
        QTest::mouseRelease(graph, Qt::LeftButton, Qt::NoModifier, {80, 90});
        events();
        check(c.insertParameter(track, insert, "threshold") != before,
              "native graph edits threshold");
        c.undo();
        events();
        check(c.insertParameter(track, insert, "threshold") == before,
              "graph gesture undoes once");
        check(c.insertSupportsSidechain(track, insert),
              "sidechain support retained");
      });
}
