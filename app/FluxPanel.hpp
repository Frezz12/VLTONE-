#pragma once
#include "Internal/FluxInstance.hpp"
#include <QWidget>
#include <memory>
namespace daw {
class EngineController;
}
class FluxPanel final : public QWidget {
  Q_OBJECT
public:
  FluxPanel(daw::EngineController *, QString channel, QString insert,
            QWidget *parent = nullptr);
  ~FluxPanel() override;
  QSize sizeHint() const override { return {1344, 900}; }
  QSize minimumSizeHint() const override { return {880, 660}; }
  bool applyFactoryPreset(unsigned);
  bool randomizeParameters(unsigned seed = 0,
                           unsigned scope = daw::plugins::flux::randomAll);
  bool importPreset(const QString &);
  bool exportPreset(const QString &) const;
  bool visualUpdatesActive() const;
  daw::plugins::flux::State currentState() const;
signals:
  void projectEdited();
  void automationRequested(const QString &parameterId);

protected:
  void keyPressEvent(QKeyEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;
  bool event(QEvent *) override;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
