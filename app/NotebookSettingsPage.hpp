#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QCheckBox;

class NotebookSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit NotebookSettingsPage(QWidget* parent = nullptr);
    /// Re-read appearance values after a portable theme is applied.
    void refresh();

signals:
    void changed();

private:
    void refreshBackground();
    void refreshFonts();

    QLineEdit* m_background = nullptr;
    QPushButton* m_clearBackground = nullptr;
    QSlider* m_visibility = nullptr;
    QLabel* m_visibilityValue = nullptr;
    QCheckBox* m_animate = nullptr;
    QListWidget* m_fonts = nullptr;
    QPushButton* m_removeFont = nullptr;
};
