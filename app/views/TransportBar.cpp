#include "TransportBar.h"

#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace {

// Знаменатель размера — это длительность ноты, поэтому только степени двойки.
const int kDenominators[] = {1, 2, 4, 8, 16, 32};

QString formatTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const int minutes = static_cast<int>(seconds) / 60;
    const int secs    = static_cast<int>(seconds) % 60;
    const int millis  = static_cast<int>((seconds - std::floor(seconds)) * 1000.0);
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs,    2, 10, QLatin1Char('0'))
        .arg(millis,  3, 10, QLatin1Char('0'));
}

} // namespace

TransportBar::TransportBar(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void TransportBar::buildUi() {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(14);

    // ---- кнопки перемотки -------------------------------------------------
    toStartButton_ = new QPushButton(QStringLiteral("⏮"), this);
    toStartButton_->setToolTip(QStringLiteral("В начало"));
    toStartButton_->setFixedSize(40, 34);

    playButton_ = new QPushButton(QStringLiteral("▶"), this);
    playButton_->setToolTip(QStringLiteral("Воспроизведение / стоп  (пробел)"));
    playButton_->setCheckable(true);
    playButton_->setFixedSize(48, 34);
    playButton_->setStyleSheet(QStringLiteral(
        "QPushButton:checked { background: #3a8a3a; color: white; }"));

    layout->addWidget(toStartButton_);
    layout->addWidget(playButton_);

    // ---- дисплей позиции --------------------------------------------------
    auto* lcd = new QFrame(this);
    lcd->setFrameShape(QFrame::StyledPanel);
    lcd->setStyleSheet(QStringLiteral(
        "QFrame { background: #16181a; border: 1px solid #3a3d40; border-radius: 4px; }"));

    auto* lcdLayout = new QVBoxLayout(lcd);
    lcdLayout->setContentsMargins(14, 5, 14, 5);
    lcdLayout->setSpacing(0);

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    barsLabel_ = new QLabel(QStringLiteral("1 . 1 . 000"), lcd);
    mono.setPointSize(19);
    mono.setBold(true);
    barsLabel_->setFont(mono);
    barsLabel_->setStyleSheet(QStringLiteral("color: #e8e8e8; border: none;"));
    barsLabel_->setAlignment(Qt::AlignCenter);

    timeLabel_ = new QLabel(QStringLiteral("00:00.000"), lcd);
    mono.setPointSize(9);
    mono.setBold(false);
    timeLabel_->setFont(mono);
    timeLabel_->setStyleSheet(QStringLiteral("color: #7d8489; border: none;"));
    timeLabel_->setAlignment(Qt::AlignCenter);

    lcdLayout->addWidget(barsLabel_);
    lcdLayout->addWidget(timeLabel_);
    layout->addWidget(lcd);

    // ---- темп и размер ----------------------------------------------------
    tempoSpin_ = new QDoubleSpinBox(this);
    tempoSpin_->setRange(20.0, 999.0);
    tempoSpin_->setDecimals(3);
    tempoSpin_->setSingleStep(1.0);
    tempoSpin_->setValue(120.0);
    tempoSpin_->setSuffix(QStringLiteral(" BPM"));
    tempoSpin_->setToolTip(QStringLiteral("Темп в четвертях за минуту"));
    tempoSpin_->setFixedWidth(110);

    sigNumerator_ = new QSpinBox(this);
    sigNumerator_->setRange(1, 32);
    sigNumerator_->setValue(4);
    sigNumerator_->setFixedWidth(48);

    sigDenominator_ = new QSpinBox(this);
    sigDenominator_->setRange(1, 32);
    sigDenominator_->setValue(4);
    sigDenominator_->setFixedWidth(48);
    // Знаменатель может быть только степенью двойки — шаг спинбокса
    // перескакивает по допустимым значениям.
    sigDenominator_->setSingleStep(1);

    auto* sigLayout = new QHBoxLayout();
    sigLayout->setSpacing(2);
    sigLayout->addWidget(sigNumerator_);
    sigLayout->addWidget(new QLabel(QStringLiteral("/"), this));
    sigLayout->addWidget(sigDenominator_);

    auto* tempoBlock = new QVBoxLayout();
    tempoBlock->setSpacing(3);
    tempoBlock->addWidget(tempoSpin_);
    tempoBlock->addLayout(sigLayout);
    layout->addLayout(tempoBlock);

    // ---- метроном ---------------------------------------------------------
    metronomeButton_ = new QPushButton(QStringLiteral("Метроном"), this);
    metronomeButton_->setCheckable(true);
    metronomeButton_->setFixedHeight(28);
    metronomeButton_->setStyleSheet(QStringLiteral(
        "QPushButton:checked { background: #b8863a; color: white; }"));

    metronomeGain_ = new QSlider(Qt::Horizontal, this);
    metronomeGain_->setRange(-40, 0);
    metronomeGain_->setValue(-12);
    metronomeGain_->setFixedWidth(90);
    metronomeGain_->setToolTip(QStringLiteral("Громкость клика"));

    auto* metronomeBlock = new QVBoxLayout();
    metronomeBlock->setSpacing(3);
    metronomeBlock->addWidget(metronomeButton_);
    metronomeBlock->addWidget(metronomeGain_);
    layout->addLayout(metronomeBlock);

    layout->addStretch();

    // ---- сигналы ----------------------------------------------------------
    connect(playButton_, &QPushButton::toggled, this, &TransportBar::playToggled);
    connect(toStartButton_, &QPushButton::clicked, this, &TransportBar::returnToZeroRequested);
    connect(tempoSpin_, &QDoubleSpinBox::valueChanged, this, &TransportBar::tempoChanged);
    connect(sigNumerator_, &QSpinBox::valueChanged, this, [this](int) { emitTimeSignature(); });
    connect(sigDenominator_, &QSpinBox::valueChanged, this, [this](int value) {
        // Прижимаем к ближайшей степени двойки.
        int best = kDenominators[0];
        for (int d : kDenominators)
            if (std::abs(d - value) < std::abs(best - value))
                best = d;
        if (best != value) {
            QSignalBlocker blocker(sigDenominator_);
            sigDenominator_->setValue(best);
        }
        emitTimeSignature();
    });
    connect(metronomeButton_, &QPushButton::toggled, this, &TransportBar::metronomeToggled);
    connect(metronomeGain_, &QSlider::valueChanged, this, [this](int db) {
        emit metronomeGainChanged(db <= -40 ? 0.0f : std::pow(10.0f, db / 20.0f));
    });
}

void TransportBar::emitTimeSignature() {
    emit timeSignatureChanged(sigNumerator_->value(), sigDenominator_->value());
}

void TransportBar::setPosition(const daw::time::BarBeat& position, double seconds) {
    barsLabel_->setText(QStringLiteral("%1 . %2 . %3")
                            .arg(position.bar)
                            .arg(position.beat)
                            // Тики приводим к трёхзначному виду, как в Logic:
                            // доля делится на 1000, а не на 15360.
                            .arg(static_cast<int>(position.tickInBeat * 1000
                                                  / daw::time::kTicksPerQuarter),
                                 3, 10, QLatin1Char('0')));
    timeLabel_->setText(formatTime(seconds));
}

void TransportBar::setPlaying(bool playing) {
    QSignalBlocker blocker(playButton_);
    playButton_->setChecked(playing);
    playButton_->setText(playing ? QStringLiteral("⏸") : QStringLiteral("▶"));
}

void TransportBar::setTransportEnabled(bool enabled) {
    playButton_->setEnabled(enabled);
    toStartButton_->setEnabled(enabled);
    if (!enabled)
        setPlaying(false);
}

double TransportBar::tempo() const { return tempoSpin_->value(); }
bool   TransportBar::isMetronomeEnabled() const { return metronomeButton_->isChecked(); }
