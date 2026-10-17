#pragma once
#include "Internal/Vlt1073Instance.hpp"
#include <QPixmap>
#include <QWidget>
#include <array>
#include <optional>
#include <string>
#include <vector>

class Vlt1073Control;
class QAbstractButton;
class QTimer;
namespace daw { class EngineController; }
class Vlt1073Panel final : public QWidget {
    Q_OBJECT
public:
    Vlt1073Panel(daw::EngineController*, QString channel, QString insert, QWidget* parent = nullptr);
    ~Vlt1073Panel() override;
    QSize sizeHint() const override { return {315,743}; }
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void changeEvent(QEvent*) override;
private:
    friend class Vlt1073Control;
    bool available() const;
    double read(unsigned) const;
    void write(unsigned,double);
    void finish(unsigned);
    void finishAll();
    void refresh();
    void cacheFace();
    void layoutControls();
    QRectF faceRect() const;
    daw::EngineController* m_controller;
    std::string m_channel, m_insert;
    std::array<Vlt1073Control*,daw::plugins::vlt1073::kParameterCount> m_controls{};
    std::array<std::optional<double>,daw::plugins::vlt1073::kParameterCount> m_gestures{};
    struct Label { QWidget* widget; QRectF rect; };
    std::vector<Label> m_labels;
    QAbstractButton* m_power = nullptr;
    QTimer* m_timer = nullptr;
    QPixmap m_face;
};
