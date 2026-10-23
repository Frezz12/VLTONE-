#pragma once
#include <QWidget>
class QTreeWidget;
class QTabWidget;
class QCloseEvent;
class QPlainTextEdit;
class QCheckBox;
class QKeySequenceEdit;
class QLabel;
class QListWidget;
class QLineEdit;
class QAbstractButton;
namespace ui {
class ScriptService;
class ScriptManagerWindow final : public QWidget {
    Q_OBJECT
  public:
    explicit ScriptManagerWindow(ScriptService *, QWidget *parent = nullptr);
    void refresh();

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    void selection();
    void refreshRuns();
    void refreshLog();
    void filterPackages();
    QString extension() const;
    QString command() const;
    ScriptService *m_service;
    QTreeWidget *m_packages;
    QTreeWidget *m_runs;
    QPlainTextEdit *m_log;
    QCheckBox *m_enabled, *m_autostart, *m_pinned;
    QKeySequenceEdit *m_shortcut;
    QLabel *m_permissions, *m_selectionHeading, *m_selectionHint;
    QTabWidget *m_pages;
    QListWidget *m_contexts;
    QLineEdit *m_search;
    QAbstractButton *m_runButton, *m_stopButton, *m_errorButton;
    QList<QAbstractButton *> m_packageActions;
    bool m_refreshing = false;
};
} // namespace ui
