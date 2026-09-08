#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;

class QuickImportSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit QuickImportSettingsPage(QWidget* parent = nullptr);

    void refresh();
    void showConfigurationError(const QString& message);

private:
    void refreshTracks(const QString& preferredTrackId = {});
    void persist();

    QComboBox* m_template = nullptr;
    QComboBox* m_track = nullptr;
    QCheckBox* m_detectTempo = nullptr;
    QCheckBox* m_detectKey = nullptr;
    QLabel* m_error = nullptr;
};
