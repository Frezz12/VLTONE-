#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace ui {

struct ProjectSaveOptions {
    QString name;
    QString author;
    QString coverPath;
    QString parentDirectory;

    QString packagePath() const;
};

class ProjectSaveDialog final : public QDialog {
    Q_OBJECT
public:
    ProjectSaveDialog(const QString& name, const QString& author,
                      const QString& coverPath, const QString& parentDirectory,
                      QWidget* parent = nullptr);

    ProjectSaveOptions options() const;
    bool checkForTest() const;

private:
    void chooseCover();
    void setCoverPath(const QString& path);
    void updateState();
    void applyTheme();

    QLineEdit* m_name = nullptr;
    QLineEdit* m_author = nullptr;
    QLineEdit* m_location = nullptr;
    QLabel* m_cover = nullptr;
    QLabel* m_destination = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_removeCover = nullptr;
    QPushButton* m_save = nullptr;
    QString m_coverPath;
};

class ProjectOpenDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ProjectOpenDialog(const QStringList& projectPaths,
                               QWidget* parent = nullptr);

    QString selectedPath() const { return m_selectedPath; }
    bool checkForTest() const;

private:
    void browse();
    void applyTheme();

    QString m_selectedPath;
    QPushButton* m_browse = nullptr;
    QWidget* m_projectList = nullptr;
};

QStringList recentProjectPaths();
void rememberRecentProject(const QString& packagePath);
bool checkProjectDialogsForTest(QWidget* parent = nullptr);

} // namespace ui
