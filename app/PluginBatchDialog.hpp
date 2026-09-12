#pragma once

#include "EngineController.hpp"
#include <QDialog>
#include <QHash>
#include <QPointer>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;
namespace ui { class SelectionModel; }

/// Configures independent copies in a disposable, audible project draft.
class PluginBatchDialog final : public QDialog {
    Q_OBJECT
public:
    using Target = daw::EngineController::PluginBatchTarget;
    PluginBatchDialog(daw::EngineController&, std::vector<Target>, QWidget* parent = nullptr);
    ~PluginBatchDialog() override;
    void done(int result) override;
    static bool checkForTest(const QString& screenshotPath = {});
    static std::vector<Target> selectedTargets(const ui::SelectionModel&);
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    bool switchSource(int index);
    void rebuildRack();
    void addPlugin(const daw::plugins::PluginDescriptor&, bool open = true);
    void openEditor(const QString&);
    void closeEditors();
    void stopAudition();
    void toggleAudition();
    void apply();
    void showError(const QString&);
    void refreshAvailability();
    daw::EngineController& m_controller;
    std::vector<Target> m_targets;
    std::shared_ptr<daw::EngineController> m_draft;
    std::vector<std::string> m_slots;
    QHash<QString, QPointer<QDialog>> m_editors;
    QComboBox* m_source = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_listen = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_apply = nullptr;
    QVBoxLayout* m_rack = nullptr;
    QTimer* m_pump = nullptr;
    int m_sourceIndex = -1;
    bool m_listening = false;
};
