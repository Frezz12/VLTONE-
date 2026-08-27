#pragma once

#include "AiPrefs.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

/// Settings ▸ AI: administrator-provided models, local custom connections and
/// agent limits. Local credentials are written to the operating-system vault.
///
/// Writes through on every change, like the other settings pages — there is no
/// OK button anywhere in this window. Everything goes through `ui::aiprefs`, so
/// the panel and this page cannot disagree about where a value lives. The
/// Managed credentials never reach this page; custom keys are write-only.
class AiSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AiSettingsPage(QWidget* parent = nullptr);

signals:
    /// Anything on this page changed; the shell reloads the panel.
    void changed();

private:
    void loadModels();
    void loadCustomEditor(int row);
    void clearCustomEditor();
    void saveCustomModel();
    void removeCustomModel();
    /// The music endpoint's own group. Its own key, its own model, its own
    /// server: none of the chat settings apply to it.
    QWidget* buildMusicGroup();
    void loadMusic();

    QListWidget* m_managedModels = nullptr;
    QListWidget* m_customModels = nullptr;
    QLineEdit* m_customName = nullptr;
    QComboBox* m_customProvider = nullptr;
    QLineEdit* m_customModel = nullptr;
    QLineEdit* m_customEndpoint = nullptr;
    QLineEdit* m_customKey = nullptr;
    QPushButton* m_revealCustomKey = nullptr;
    QLabel* m_customKeyNote = nullptr;
    QLabel* m_customStatus = nullptr;
    QPushButton* m_saveCustom = nullptr;
    QPushButton* m_removeCustom = nullptr;
    QString m_editingCustomId;
    QSpinBox* m_maxIterations = nullptr;
    QSpinBox* m_historyLimit = nullptr;
    QCheckBox* m_streaming = nullptr;
    void refreshPromptStatus();
    QLabel* m_promptStatus = nullptr;
    QPushButton* m_promptRefresh = nullptr;

    QLineEdit* m_musicUrl = nullptr;
    QLineEdit* m_musicModel = nullptr;
    QLineEdit* m_musicKey = nullptr;
    QPushButton* m_forgetMusicKey = nullptr;
    QComboBox* m_musicFormat = nullptr;
    QSpinBox* m_musicSampleRate = nullptr;
    QSpinBox* m_musicBitrate = nullptr;
    QLineEdit* m_musicFolder = nullptr;
    QSpinBox* m_musicTimeout = nullptr;
    QLabel* m_musicNote = nullptr;
    /// Set while model/editor controls are filled so their signals do not
    /// write partially loaded data back to preferences.
    bool m_loading = false;
};
