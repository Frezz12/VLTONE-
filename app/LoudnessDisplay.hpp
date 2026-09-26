#pragma once
#include "DSP/LoudnessMeter.hpp"
#include <QAbstractButton>

/// Recessed master readout. Clicking (or Space) resets the measurement.
class LoudnessDisplay final : public QAbstractButton {
    Q_OBJECT
public:
    explicit LoudnessDisplay(QWidget* parent = nullptr);
    void setLevels(daw::engine::LoudnessLevels levels);
    static bool checkForTest(const QString& screenshotPrefix);
    QSize sizeHint() const override { return {88, 68}; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    daw::engine::LoudnessLevels m_levels;
    QString m_summary;
};
