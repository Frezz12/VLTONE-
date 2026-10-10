#pragma once

#include <QDialog>
#include <QImage>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
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

    void setTimelinePreview(const QImage& image);
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
    QImage m_timelinePreview;
    QLabel* m_previewHint = nullptr;
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
    void updateSelection();
    void openSelected();
    void openLocation();
    void applyTheme();

    QString m_selectedPath;
    QPushButton* m_browse = nullptr;
    QListWidget* m_projectList = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_location = nullptr;
    QLabel* m_selection = nullptr;
};

struct ProjectTemplateSaveOptions {
    QString name;
    QString artworkPath;
};

class ProjectTemplateSaveDialog final : public QDialog {
    Q_OBJECT
public:
    ProjectTemplateSaveDialog(const QString& name,
                              const QString& artworkPath = {},
                              QWidget* parent = nullptr);

    ProjectTemplateSaveOptions options() const;
    bool checkForTest() const;

private:
    void chooseArtwork();
    void setArtworkPath(const QString& path);
    void updateState();
    void applyTheme();

    QLineEdit* m_name = nullptr;
    QWidget* m_preview = nullptr;
    QLabel* m_mediaName = nullptr;
    QLabel* m_destination = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_removeArtwork = nullptr;
    QPushButton* m_save = nullptr;
    QString m_artworkPath;
};

class ProjectTemplateOpenDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ProjectTemplateOpenDialog(const QStringList& templatePaths,
                                       QWidget* parent = nullptr);

    QString selectedPath() const { return m_selectedPath; }
    bool libraryChanged() const { return m_libraryChanged; }
    bool checkForTest() const;

private:
    void updateSelection();
    void deleteSelected();
    void applyTheme();

    QString m_selectedPath;
    QListWidget* m_templates = nullptr;
    QWidget* m_preview = nullptr;
    QLabel* m_name = nullptr;
    QLabel* m_mediaName = nullptr;
    QLabel* m_empty = nullptr;
    QPushButton* m_create = nullptr;
    QPushButton* m_delete = nullptr;
    bool m_libraryChanged = false;
};

/// Local, optional artwork kept beside the manifest; never replaces a custom cover.
QString projectPreviewPath(const QString& packagePath);
bool saveProjectPreview(const QString& packagePath, const QImage& image);

QStringList recentProjectPaths();
QString projectDisplayName(const QString& packagePath);
void rememberRecentProject(const QString& packagePath);
bool checkProjectDialogsForTest(QWidget* parent = nullptr);

} // namespace ui
