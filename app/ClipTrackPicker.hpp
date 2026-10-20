#pragma once

#include <QDialog>
#include <QStringList>

namespace daw { struct ProjectModel; }
class QListWidget;
class QListWidgetItem;

class ClipTrackPicker final : public QDialog {
    Q_OBJECT
public:
    ClipTrackPicker(const daw::ProjectModel& project, const QStringList& tracks,
                    QWidget* parent = nullptr);
    void popup(const QPoint& globalPosition);
signals:
    void trackSelected(const QString& trackId);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void choose(QListWidgetItem* item);
    QListWidget* m_list = nullptr;
};
