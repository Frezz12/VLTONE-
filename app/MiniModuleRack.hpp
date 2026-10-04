#pragma once
#include "model/Document.hpp"
#include <QWidget>
#include <functional>
#include <vector>
class QVBoxLayout;
namespace daw {
class EngineController;
}
namespace ui {
class MiniModuleCard;
class MiniModuleRack final : public QWidget {
  Q_OBJECT
public:
  MiniModuleRack(daw::EngineController *, QString channel,
                 QWidget *parent = nullptr);
  ~MiniModuleRack() override;
  void sync(bool automation);
  void finishEdits();
  void setStripWidth(int width);
  void setRackHeight(int height);
  int naturalHeight() const;
  static QWidget *createPreview(const daw::plugins::mini::MiniModuleDefinition &,
                                int stripWidth, QWidget *parent = nullptr);
  static int naturalHeight(const daw::ProjectModel &,
                           const std::vector<daw::InsertModel> &,
                           int stripWidth);
signals:
  void edited(bool undoable = true);
  void automateRequested(const QString &module, const QString &parameter);
  void layoutChanged();

protected:
  void hideEvent(QHideEvent *) override;
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;

private:
  void rebuild();
  void addMenu(const QPoint &, const QString &replace = {});
  void cardMenu(const QString &, const QPoint &);
  void appearanceDialog(const QString &);
  daw::EngineController *m_controller;
  QString m_channel, m_project;
  QVBoxLayout *m_column;
  QWidget *m_well;
  int m_stripWidth = 100;
  int m_assignedHeight = 0;
  bool m_syncing = false;
  std::vector<daw::InsertModel> m_models;
  std::vector<MiniModuleCard *> m_cards;
};
} // namespace ui
