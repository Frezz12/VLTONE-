#pragma once
#include <QDialog>
#include <QIcon>

class QComboBox;
class QLineEdit;
class QListWidget;
class QLabel;

class TrackIconPicker final : public QDialog {
    Q_OBJECT
public:
    TrackIconPicker(const QString& currentId, const QIcon& defaultIcon,
                    QWidget* parent = nullptr);
    void popup(const QPoint& globalPosition);
signals:
    void iconSelected(const QString& id);
    void addIconRequested();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void reload();
    void filter();
    QString m_currentId;
    QIcon m_defaultIcon;
    QLineEdit* m_search;
    QComboBox* m_category;
    QListWidget* m_list;
    QLabel* m_empty;
};
