#pragma once

#include "EngineController.hpp"
#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;
class QToolButton;
class QTimer;
class QVBoxLayout;
class PluginEditorWindow;

/// A device-free draft rack; accepting clones its state to independent tracks.
class CreateTracksDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CreateTracksDialog(daw::EngineController& controller, QWidget* parent = nullptr);
    ~CreateTracksDialog() override;
    const std::vector<std::string>& createdTrackIds() const { return m_createdIds; }
    void reject() override;
    static bool checkForTest(daw::EngineController& controller, const QString& screenshotPath);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    daw::TrackKind kind() const;
    bool summing() const;
    void syncType();
    void rebuildInputs();
    void rebuildRack();
    QWidget* slotRow(int index, const QString& slotId, QWidget* parent);
    void pickPlugin(int index, QWidget* anchor);
    void addPlugin(int index, const daw::plugins::PluginDescriptor& descriptor);
    void removePlugin(int index);
    void moveSlot(int index, int direction);
    void openEditor(const QString& slotId);
    void closeEditors();
    void updateSummary();
    void applyTheme();
    void create();
    bool capture(daw::EngineController::TrackCreationRequest& request);
    void showError(const QString& message);

    daw::EngineController& m_controller;
    daw::EngineController m_draft;
    std::string m_draftTrack;
    QStringList m_slots{QString(), QString(), QString()};
    QHash<QString, QPointer<QDialog>> m_editors;
    std::vector<std::string> m_createdIds;
    QComboBox* m_kind = nullptr;
    QSpinBox* m_count = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_channels = nullptr;
    QComboBox* m_input = nullptr;
    QComboBox* m_output = nullptr;
    QWidget* m_form = nullptr;
    QWidget* m_audioSection = nullptr;
    QWidget* m_channelField = nullptr;
    QWidget* m_inputField = nullptr;
    QWidget* m_pluginSection = nullptr;
    QWidget* m_instrumentSection = nullptr;
    QVBoxLayout* m_instrumentLayout = nullptr;
    QWidget* m_rack = nullptr;
    QVBoxLayout* m_rackLayout = nullptr;
    QLabel* m_typeHint = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_create = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_addSlot = nullptr;
    QTimer* m_pump = nullptr;
    bool m_ready = false;
    bool m_creating = false;
};
