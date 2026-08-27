#pragma once
//
// Простейший индикатор уровня. Первый кусок собственной отрисовки: пока на
// QPainter, потому что перерисовывается один прямоугольник. Всё, что рисует
// клипы и волновые формы, поедет на QRhiWidget (M1) — QPainter там не вытянет.
//
#include <QWidget>

class LevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(QWidget* parent = nullptr);

    // Линейное значение 0..1 (не в децибелах).
    void setLevel(float linear);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    static float linearToNormalizedDb(float linear);

    float level_ = 0.0f;   // сглаженное, 0..1 по шкале дБ
    float hold_  = 0.0f;   // пиковый маркер
    int   holdCountdown_ = 0;
};
