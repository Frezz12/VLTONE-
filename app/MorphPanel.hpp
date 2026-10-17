#pragma once
#include "Internal/MorphProgram.hpp"
#include <QWidget>
#include <memory>

namespace daw {
class EngineController;
}
// The editor owns no audio buffers. All edits go through the controller's
// parameter/state paths so automation, local projects and history see them.
class MorphPanel final : public QWidget {
  Q_OBJECT
public:
  MorphPanel(daw::EngineController *, QString channel, QString insert,
             QWidget *parent = nullptr);
  ~MorphPanel() override;
  QSize sizeHint() const override { return {1152, 648}; }
  QSize minimumSizeHint() const override { return {800, 450}; }
  bool applyFactoryPreset(unsigned index);
  void importImage(const QString &path);
  bool imageImportPending() const;
  bool visualUpdatesActive() const;
  daw::plugins::morph::State currentState() const;
signals:
  void projectEdited();
  void preferredHeightChanged(int height);
  void automationRequested(const QString &parameterId);
  void imageImportFinished(bool success);

protected:
  bool eventFilter(QObject *, QEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};
