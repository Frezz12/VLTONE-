#pragma once
#include "Creator/FunctionDefinition.hpp"
#include <QPlainTextEdit>
#include <QWidget>
#include <optional>

class QLineEdit;
class QLabel;
class QCompleter;
class QToolBar;
namespace ui {
class CreatorCodeText final : public QPlainTextEdit {
  Q_OBJECT
public:
  explicit CreatorCodeText(QWidget *parent = nullptr);
  void reveal(unsigned line, unsigned column);
  void paintNumbers(QPaintEvent *);
  void markErrors(const QList<unsigned> &lines);
signals:
  void undoRequested();
  void redoRequested();

protected:
  void resizeEvent(QResizeEvent *) override;
  void keyPressEvent(QKeyEvent *) override;

private:
  void updateGutter();
  QWidget *m_numbers;
  QCompleter *m_completer;
  QList<unsigned> m_errors;
};
class CreatorCodeEditor final : public QWidget {
  Q_OBJECT
public:
  explicit CreatorCodeEditor(QWidget *parent = nullptr);
  void setFunction(const daw::plugins::mini::FunctionDefinition &, int cursor);
  QString source() const;
  QString entry() const;
  int cursorPosition() const;
  void reveal(unsigned line, unsigned column);
  void setBusy(bool);
signals:
  void edited();
  void cursorMoved(int);
  void updateRequested();
  void createRequested();
  void extractRequested();
  void closeRequested();
  void undoRequested();
  void redoRequested();

private:
  CreatorCodeText *m_text;
  QLineEdit *m_entry, *m_find;
  QLabel *m_state;
  QToolBar *m_toolbar;
};
std::optional<daw::plugins::mini::FunctionDefinition>
createFunctionDialog(QWidget *parent);
} // namespace ui
