#pragma once
#include "EngineController.hpp"
#include <QPointer>
#include <QWidget>
#include <functional>

class QHBoxLayout;
class QDialog;
class RackModulationPanel final : public QWidget {
    Q_OBJECT
public:
    RackModulationPanel(daw::EngineController*, QWidget* rack, QString slot = {});
    ~RackModulationPanel() override;
    void setChannel(const QString&);
    void refresh();
    void addSource(daw::modulation::Kind);
    bool command(const QString& action);
    bool assigning() const { return m_assigning; }
    void finishEdits();
signals:
    void edited(bool structural = true);
    void layoutChanged();
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void rebuild();
    void synchronize();
    void expandEditor(const QString& id);
    void beginGesture();
    void finishGesture();
    const daw::modulation::Source* source(const QString& id) const;
    void change(const std::function<void(daw::modulation::Rack&)>&);
    void editSource(const QString& id, const std::function<void(daw::modulation::Source&)>&);
    void sourceMenu(QString id, const QPoint&);
    void advanced(const QString& id);
    void setAssigning(bool);
    void assign(const QString& slot, const QString& parameter);
    void highlight();
    void automate(const std::string& id, bool depth);
    bool includes(const daw::modulation::Source&) const;
    QWidget* sourceControls(const QString& id, QWidget* parent, bool detailed,
                            std::vector<std::function<void()>>& refreshers);
    void addMenu();
    void finishDepthGesture(bool cancel = false);
    bool depthEvent(QWidget*, QEvent*);
    QString key(const char*) const;
    daw::EngineController* m_controller;
    QPointer<QWidget> m_rack;
    QString m_channel, m_slot, m_selected, m_route;
    daw::modulation::Rack m_shown;
    QHBoxLayout* m_layout;
    bool m_assigning = false, m_pending = false;
    std::uint64_t m_touch = 0;
    std::uint64_t m_structureRevision = 0;
    QPointer<QWidget> m_consumed;
    std::vector<std::function<void()>> m_refreshers;
    std::vector<std::function<void()>> m_expandedRefreshers;
    QPointer<QWidget> m_depthControl;
    QString m_depthRoute;
    double m_depthY = 0, m_depthBefore = 0;
    daw::EngineController::UndoGroup m_gesture;
    QPointer<QDialog> m_expanded;
    QString m_expandedSource;
};
