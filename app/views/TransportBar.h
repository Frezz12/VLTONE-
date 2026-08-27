#pragma once
//
// Транспортная панель. Первый шаг к раскладке в духе Logic Pro: кнопки
// перемотки слева, крупный дисплей позиции по центру, темп и размер справа.
//
// Виджет ничего не знает о движке — он только показывает и испускает сигналы.
// Связывание с Engine происходит в MainWindow, чтобы граница UI ↔ движок
// оставалась в одном месте.
//
#include <QWidget>

#include "daw/time/TempoMap.h"

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

class TransportBar : public QWidget {
    Q_OBJECT

public:
    explicit TransportBar(QWidget* parent = nullptr);

    void setPosition(const daw::time::BarBeat& position, double seconds);
    void setPlaying(bool playing);
    void setTransportEnabled(bool enabled);

    double tempo() const;
    bool   isMetronomeEnabled() const;

signals:
    void playToggled(bool playing);
    void returnToZeroRequested();
    void tempoChanged(double bpm);
    void timeSignatureChanged(int numerator, int denominator);
    void metronomeToggled(bool enabled);
    void metronomeGainChanged(float linear);

private:
    void buildUi();
    void emitTimeSignature();

    QPushButton*    toStartButton_  = nullptr;
    QPushButton*    playButton_     = nullptr;
    QLabel*         barsLabel_      = nullptr;
    QLabel*         timeLabel_      = nullptr;
    QDoubleSpinBox* tempoSpin_      = nullptr;
    QSpinBox*       sigNumerator_   = nullptr;
    QSpinBox*       sigDenominator_ = nullptr;
    QPushButton*    metronomeButton_ = nullptr;
    QSlider*        metronomeGain_  = nullptr;
};
