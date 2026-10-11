#include "CompressorPanel.hpp"
#include "native_plugin_ui_test.hpp"
using namespace native_test;
namespace {
class PaintCounter final : public QObject {
public:
  int paints = 0;
protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Paint)
      ++paints;
    return false;
  }
};
}
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
        // Freeze meters so their decay cannot hide a missed parameter repaint.
        auto *timer = child<QTimer>(panel, "CompressorTelemetryTimer");
        timer->stop();
        events();
        PaintCounter curve;
        graph->installEventFilter(&curve);
        for (const char *id : {"threshold", "ratio", "knee"}) {
          auto *knob = child<ui::Knob>(panel, id);
          const auto initial = c.insertParameter(track, insert, id);
          const auto depth = c.undoDepth();
          const auto center = knob->rect().center();
          QTest::mousePress(knob, Qt::LeftButton, Qt::NoModifier, center);
          events();
          const auto paints = curve.paints;
          QTest::mouseMove(knob, center - QPoint(0, 16));
          events();
          const auto raised = c.insertParameter(track, insert, id);
          check(raised > initial && curve.paints > paints,
                "curve repaints while knob is held with frozen meters");
          const auto forwardPaints = curve.paints;
          QTest::mouseMove(knob, center - QPoint(0, 8));
          events();
          check(c.insertParameter(track, insert, id) < raised &&
                    curve.paints > forwardPaints,
                "curve repaints immediately when knob direction reverses");
          check(c.undoDepth() == depth, "live curve does not commit mid-drag");
          QTest::mouseRelease(knob, Qt::LeftButton, Qt::NoModifier,
                              center - QPoint(0, 8));
          events();
          check(c.undoDepth() == depth + 1,
                "live curve gesture still commits exactly one undo");
          c.undo();
          events();
          check(c.insertParameter(track, insert, id) == initial,
                "undo restores live curve parameter");
        }
        graph->removeEventFilter(&curve);
        timer->start();
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
