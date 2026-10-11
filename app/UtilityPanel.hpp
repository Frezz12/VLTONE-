#pragma once
#include <QWidget>
#include <QString>
#include <memory>

class RackParameterBinding;
class QTimer;

// The same controls are embedded in the rack and the floating editor.
class UtilityPanel final : public QWidget {
    Q_OBJECT
public:
    UtilityPanel(RackParameterBinding*, QString uid, QWidget* parent = nullptr, bool embedded = false);
    ~UtilityPanel() override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void refresh();
protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    RackParameterBinding* m_binding;
    QString m_uid;
    QTimer* m_timer;
    bool m_embedded;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
