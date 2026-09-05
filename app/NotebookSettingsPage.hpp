#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;

class NotebookSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit NotebookSettingsPage(QWidget* parent = nullptr);

signals:
    void changed();

private:
    void refreshBackground();
    void refreshFonts();

    QLineEdit* m_background = nullptr;
    QPushButton* m_clearBackground = nullptr;
    QSlider* m_visibility = nullptr;
    QLabel* m_visibilityValue = nullptr;
    QListWidget* m_fonts = nullptr;
    QPushButton* m_removeFont = nullptr;
};
