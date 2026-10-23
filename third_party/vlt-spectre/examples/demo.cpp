// SPDX-License-Identifier: MIT
#include <vlt/spectre/SpectrumWidget.hpp>
#include "WavFile.hpp"
#include "DemoEq.hpp"
#include "DemoControls.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

using namespace vlt::spectre;
namespace {
constexpr double tau = 6.283185307179586;

// The demo's settings reflow, while the actual SpectrumWidget remains a reusable
// canvas with no imposed toolbar, menus outside its bounds, or application model.
class SettingsRow final : public QWidget {
public:
    explicit SettingsRow(QWidget* parent = nullptr) : QWidget(parent) {
        layout = new QGridLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setHorizontalSpacing(16); layout->setVerticalSpacing(10);
    }
    QComboBox* addChoice(const QString& name, const QStringList& choices) {
        auto* cell = new QWidget(this);
        auto* stack = new QVBoxLayout(cell);
        stack->setContentsMargins(0, 0, 0, 0); stack->setSpacing(6);
        auto* label = new QLabel(name, cell); label->setProperty("muted", true);
        auto* choice = new DemoChoice(cell); choice->addItems(choices);
        choice->setAccessibleName(name); label->setBuddy(choice);
        choice->setMinimumContentsLength(11);
        choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        choice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        stack->addWidget(label); stack->addWidget(choice);
        cells.push_back(cell); reflow();
        return choice;
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); reflow(); }
private:
    void reflow() {
        const int columns = std::clamp(width() / 150, 2, 7);
        if (columns == currentColumns && int(cells.size()) == currentCount) return;
        for (auto* cell : cells) layout->removeWidget(cell);
        for (int i = 0; i < int(cells.size()); ++i) layout->addWidget(cells[std::size_t(i)], i / columns, i % columns);
        for (int i = 0; i < 7; ++i) layout->setColumnStretch(i, i < columns ? 1 : 0);
        currentColumns = columns; currentCount = int(cells.size());
    }
    QGridLayout* layout;
    std::vector<QWidget*> cells;
    int currentColumns = 0, currentCount = 0;
};

// Voss-style octave rows produce a repeatable, approximately pink demo signal.
// These samples are genuinely analyzed; no hand-drawn or animated spectrum data.
struct PinkNoise {
    std::array<double, 12> rows{};
    std::uint32_t counter = 0, random = 0x62f189a5;
    double white() {
        random = random * 1664525u + 1013904223u;
        return double(random) / 2147483648.0 - 1.0;
    }
    double next() {
        ++counter;
        unsigned row = 0, value = counter;
        while ((value & 1u) == 0 && row + 1 < rows.size()) { ++row; value >>= 1; }
        rows[row] = white();
        double sum = white();
        for (const double value : rows) sum += value;
        return sum / 13.0;
    }
};

class DemoWindow final : public QWidget {
public:
    DemoWindow() {
        setObjectName(QStringLiteral("SpectreDemo"));
        setWindowTitle(QStringLiteral("VLT Spectre — spectrum laboratory"));
        setMinimumSize(620, 460); resize(1120, 720); setAcceptDrops(true);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(24, 20, 24, 18); root->setSpacing(18);
        auto* header = new QHBoxLayout;
        auto* heading = new QVBoxLayout; heading->setSpacing(3);
        auto* title = new QLabel(QStringLiteral("VLT  /  SPECTRE"), this);
        title->setObjectName(QStringLiteral("title"));
        auto* subtitle = new QLabel(QStringLiteral("A clear view of your sound"), this);
        subtitle->setProperty("muted", true);
        heading->addWidget(title); heading->addWidget(subtitle);
        header->addLayout(heading); header->addStretch();
        auto* open = new QPushButton(QStringLiteral("Open WAV…"), this);
        open->setAccessibleName(QStringLiteral("Open a WAV file for analysis"));
        themeButton = new QPushButton(QStringLiteral("Light theme"), this);
        header->addWidget(open); header->addWidget(themeButton);
        root->addLayout(header);

        auto* controls = new SettingsRow(this);
        source = controls->addChoice(QStringLiteral("SOURCE"), {
            QStringLiteral("Musical signal"), QStringLiteral("Sine · 1 kHz"),
            QStringLiteral("Two tones · 60 / 1k"), QStringLiteral("Opposite-phase stereo"),
            QStringLiteral("Pink noise"), QStringLiteral("Silence")});
        resolution = controls->addChoice(QStringLiteral("RESOLUTION"), {
            QStringLiteral("4096"), QStringLiteral("8192"),
            QStringLiteral("16384"), QStringLiteral("32768")});
        resolution->setCurrentIndex(1);
        speed = controls->addChoice(QStringLiteral("RELEASE"), {
            QStringLiteral("Fast"), QStringLiteral("Medium"), QStringLiteral("Slow")});
        speed->setCurrentIndex(1);
        tilt = controls->addChoice(QStringLiteral("DISPLAY WEIGHT"), {
            QStringLiteral("Flat"), QStringLiteral("3 dB / oct"), QStringLiteral("4.5 dB / oct"),
            QStringLiteral("Treble +9 dB"), QStringLiteral("Treble +12 dB"), QStringLiteral("Treble +18 dB")});
        tilt->setCurrentIndex(4);
        tilt->setToolTip(QStringLiteral("Treble weighting rises smoothly from 2 to 16 kHz. It leaves 500–2000 Hz unchanged.\nDisplay only: audio and numeric dBFS readouts are unaffected."));
        range = controls->addChoice(QStringLiteral("VISIBLE RANGE"), {
            QStringLiteral("60 dB"), QStringLiteral("90 dB"), QStringLiteral("120 dB")});
        range->setCurrentIndex(1);
        smoothing = controls->addChoice(QStringLiteral("EDGE SMOOTHING"), {
            QStringLiteral("Off"), QStringLiteral("Fine"), QStringLiteral("Rounded"), QStringLiteral("Wide")});
        smoothing->setCurrentIndex(2);
        smoothing->setToolTip(QStringLiteral("Visual envelope: off, 1/12, 1/6 or 1/4 octave.\nRounded shoulders keep peaks legible. Measurements remain unsmoothed."));
        motion = controls->addChoice(QStringLiteral("MOTION"), {
            QStringLiteral("Direct"), QStringLiteral("Smooth"), QStringLiteral("Gentle")});
        motion->setCurrentIndex(1);
        root->addWidget(controls);

        auto* plate = new QFrame(this); plate->setObjectName(QStringLiteral("plate"));
        auto* plateLayout = new QVBoxLayout(plate);
        plateLayout->setContentsMargins(1, 1, 1, 1); plateLayout->setSpacing(0);
        spectrum = new SpectrumWidget(plate);
        spectrum->setMinimumHeight(210);
        plateLayout->addWidget(spectrum, 1);
        root->addWidget(plate, 1);

        // The layer controls are an example host UI, not part of SpectrumWidget.
        auto* eqRow = new QHBoxLayout;
        postEq = new DemoCheck(QStringLiteral("After EQ"), this); postEq->setChecked(true);
        eqFrequency = new DemoNumber(this);
        eqFrequency->setRange(20, 20000); eqFrequency->setDecimals(0); eqFrequency->setSingleStep(100);
        eqFrequency->setValue(6000); eqFrequency->setSuffix(QStringLiteral(" Hz"));
        eqFrequency->setKeyboardTracking(false); eqFrequency->setAccessibleName(QStringLiteral("EQ centre frequency"));
        eqGain = new QSlider(Qt::Horizontal, this);
        eqGain->setRange(-180, 180); eqGain->setValue(60); eqGain->setMinimumWidth(80);
        eqGain->setAccessibleName(QStringLiteral("EQ gain in tenths of a decibel"));
        gainLabel = new QLabel(QStringLiteral("+6.0 dB"), this); gainLabel->setFixedWidth(58);
        eqQ = new DemoNumber(this); eqQ->setRange(.2, 8); eqQ->setDecimals(1);
        eqQ->setValue(1); eqQ->setSingleStep(.1); eqQ->setKeyboardTracking(false);
        eqQ->setAccessibleName(QStringLiteral("EQ quality factor"));
        eqRow->addWidget(postEq); eqRow->addSpacing(8);
        eqRow->addWidget(eqFrequency); eqRow->addWidget(eqGain, 1); eqRow->addWidget(gainLabel);
        eqRow->addWidget(new QLabel(QStringLiteral("Q"), this)); eqRow->addWidget(eqQ);
        root->addLayout(eqRow);

        auto* actions = new QHBoxLayout;
        freeze = new QPushButton(QStringLiteral("Freeze"), this); freeze->setCheckable(true);
        freeze->setToolTip(QStringLiteral("Hold this frame for inspection. Capture continues. Shortcut: Space on the graph."));
        auto* reset = new QPushButton(QStringLiteral("Reset zoom"), this);
        peaks = new DemoCheck(QStringLiteral("Peak hold"), this);
        reference = new DemoCheck(QStringLiteral("Compare low-pass"), this);
        reference->setToolTip(QStringLiteral("Overlay the same signal after two actual 2.4 kHz low-pass stages."));
        actions->addWidget(freeze); actions->addWidget(reset); actions->addSpacing(8);
        actions->addWidget(peaks); actions->addWidget(reference); actions->addStretch();
        status = new QLabel(this); status->setProperty("muted", true); actions->addWidget(status);
        root->addLayout(actions);
        auto* footer = new QHBoxLayout;
        readout = new QLabel(this); readout->setObjectName(QStringLiteral("readout"));
        readout->setAccessibleName(QStringLiteral("Strongest measured frequency and amplitude"));
        footer->addWidget(readout, 1);
        auto* hint = new QLabel(QStringLiteral("Wheel to zoom · Space to freeze"), this);
        hint->setProperty("muted", true); footer->addWidget(hint);
        root->addLayout(footer);
        sourceInfo = new QLabel(QStringLiteral("Generated audio · analysis only, no sound output"), this);
        sourceInfo->setProperty("muted", true); sourceInfo->setWordWrap(true); root->addWidget(sourceInfo);

        connect(open, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Analyze WAV audio"), {}, QStringLiteral("WAV audio (*.wav)"));
            if (!path.isEmpty()) loadFile(path);
        });
        connect(themeButton, &QPushButton::clicked, this, [this] { setLight(!light); });
        connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { prepare(); });
        connect(resolution, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { prepare(); });
        connect(speed, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { applyBallistics(); });
        const auto updateView = [this] {
            auto view = spectrum->view();
            view.tiltDbPerOctave = tilt->currentIndex() == 1 ? 3.0 : tilt->currentIndex() == 2 ? 4.5 : 0;
            view.highFrequencyLiftDb = tilt->currentIndex() == 3 ? 9 : tilt->currentIndex() == 4 ? 12 : tilt->currentIndex() == 5 ? 18 : 0;
            view.minimumDb = -(range->currentIndex() == 0 ? 60 : range->currentIndex() == 1 ? 90 : 120);
            view.maximumDb = 0;
            view.peakHold = peaks->isChecked();
            view.smoothingOctaves = smoothing->currentIndex() == 0 ? 0 : smoothing->currentIndex() == 1 ? 1.0/12 : smoothing->currentIndex() == 2 ? 1.0/6 : 1.0/4;
            view.transitionMs = motion->currentIndex() == 0 ? 0 : motion->currentIndex() == 1 ? 180 : 280;
            spectrum->setView(view);
        };
        connect(tilt, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateView);
        connect(range, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateView);
        connect(peaks, &QCheckBox::toggled, this, updateView);
        connect(smoothing, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateView);
        connect(motion, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateView);
        connect(reference, &QCheckBox::toggled, this, [this] { refreshTraces(); });
        connect(postEq, &QCheckBox::toggled, this, [this] { refreshTraces(); });
        const auto updateEq = [this] {
            const double gain = eqGain->value() / 10.0;
            gainLabel->setText(QStringLiteral("%1%2 dB").arg(gain >= 0 ? "+" : "").arg(gain, 0, 'f', 1));
            demoEq.setParameters(eqFrequency->value(), gain, eqQ->value());
        };
        connect(eqFrequency, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateEq);
        connect(eqGain, &QSlider::valueChanged, this, updateEq);
        connect(eqQ, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, updateEq);
        connect(freeze, &QPushButton::toggled, spectrum, &SpectrumWidget::setFrozen);
        spectrum->frozenChanged = [this](bool value) { freeze->setChecked(value); freeze->setText(value ? QStringLiteral("Resume") : QStringLiteral("Freeze")); };
        connect(reset, &QPushButton::clicked, spectrum, &SpectrumWidget::resetZoom);
        spectrum->frequencySelected = [this](double frequency) {
            eqFrequency->setValue(frequency);
        };
        setLight(false); updateView(); prepare();
        clock.start();
        timer.setTimerType(Qt::PreciseTimer); timer.setInterval(16);
        connect(&timer, &QTimer::timeout, this, [this] { tick(); });
        timer.start();
    }

    void setLight(bool value) {
        light = value;
        spectrum->setTheme(light ? SpectrumTheme::light() : SpectrumTheme::dark());
        themeButton->setText(light ? QStringLiteral("Dark theme") : QStringLiteral("Light theme"));
        // Styling is demo-local. Embedding the library never sets an app stylesheet.
        const QString background = light ? "#edf0f1" : "#111518";
        const QString surface = light ? "#ffffff" : "#22292f";
        const QString text = light ? "#263039" : "#e2e9ec";
        const QString muted = light ? "#56636e" : "#a0adb7";
        const QString edge = light ? "#c6cfd4" : "#35414a";
        setStyleSheet(QStringLiteral(
            "QWidget#SpectreDemo { background: %1; } QWidget { color: %3; font-family: 'Segoe UI'; font-size: 12px; }"
            "QLabel { background: transparent; } QLabel[muted=true] { color: %4; }"
            "QLabel#title { font-size: 19px; font-weight: 600; letter-spacing: 2px; }"
            "QLabel#readout { font-size: 13px; } QFrame#plate { border: 1px solid %5; }"
            "QComboBox, QPushButton, QDoubleSpinBox { background: %2; border: 1px solid %5; border-radius: 5px; padding: 7px 11px; min-height: 17px; }"
            "QComboBox { padding-right: 26px; } QComboBox::drop-down { border: none; width: 22px; }"
            "QComboBox QAbstractItemView { background: %2; color: %3; selection-background-color: %5; padding: 4px; }"
            "QPushButton:hover { border-color: %4; } QPushButton:checked { background: #756237; color: #fff2d3; border-color: #b69b60; }"
            "QPushButton:focus, QComboBox:focus { border: 1px solid #c5a86e; }"
            "QSlider::groove:horizontal { height: 4px; background: %5; border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: #80cbaa; border-radius: 7px; }"
            "QCheckBox { spacing: 7px; } QToolTip { color: %3; background: %2; border: 1px solid %5; }")
            .arg(background, surface, text, muted, edge));
        auto palette = this->palette();
        palette.setColor(QPalette::Window, QColor(background));
        palette.setColor(QPalette::Base, QColor(surface));
        palette.setColor(QPalette::Button, QColor(surface));
        palette.setColor(QPalette::Text, QColor(text));
        palette.setColor(QPalette::ButtonText, QColor(text));
        palette.setColor(QPalette::WindowText, QColor(text));
        palette.setColor(QPalette::Light, QColor(edge));
        palette.setColor(QPalette::Mid, QColor(muted));
        palette.setColor(QPalette::Highlight, QColor("#80cbaa"));
        setAutoFillBackground(true); setPalette(palette);
        refreshTraces();
    }
    void selectTone() { source->setCurrentIndex(1); }
    void setDisplayWeight(int index) { tilt->setCurrentIndex(index); }
    bool loadFile(const QString& path) {
        QString error;
        auto audio = WavFile::load(path, error);
        if (!audio) { QMessageBox::warning(this, QStringLiteral("Cannot read WAV"), error); return false; }
        loaded = std::move(audio);
        loadedName = QFileInfo(path).fileName();
        if (source->count() == 6) source->addItem(QStringLiteral("Loaded WAV"));
        source->setCurrentIndex(6); prepare();
        return true;
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasUrls() && event->mimeData()->urls().front().isLocalFile()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        if (event->mimeData()->hasUrls()) loadFile(event->mimeData()->urls().front().toLocalFile());
    }
private:
    void prepare() {
        sampleRate = source->currentIndex() == 6 && loaded ? loaded->sampleRate : 48000.0;
        AnalyzerSettings settings;
        settings.fftSize = std::size_t(4096) << resolution->currentIndex();
        settings.pointCount = 1024;
        settings.hopSize = 256;
        input.prepare(sampleRate, settings); filtered.prepare(sampleRate, settings); processed.prepare(sampleRate, settings);
        demoEq.reset(sampleRate);
        eqFrequency->setMaximum(std::min(20000.0, sampleRate * .45));
        demoEq.setParameters(eqFrequency->value(), eqGain->value() / 10.0, eqQ->value());
        position = 0; lowpass = {}; pink = {}; elapsedRemainder = 0;
        spectrum->setFrozen(false);
        applyBallistics(); refreshTraces();
        status->setText(QStringLiteral("%1k · %2 Hz/bin").arg(sampleRate / 1000, 0, 'g', 4)
                        .arg(sampleRate / double(settings.fftSize), 0, 'f', 1));
        status->setToolTip(QStringLiteral("FFT window: %1 ms. A larger window resolves bass more precisely but responds later.")
                           .arg(1000.0 * double(settings.fftSize) / sampleRate, 0, 'f', 1));
        sourceInfo->setText(source->currentIndex() == 6 ?
            QStringLiteral("%1 · looping file analysis · no sound output").arg(loadedName) :
            QStringLiteral("Generated audio · analysis only, no sound output"));
        if (clock.isValid()) clock.restart();
    }
    void applyBallistics() {
        Ballistics motion;
        motion.releaseMs = speed->currentIndex() == 0 ? 100 : speed->currentIndex() == 1 ? 240 : 600;
        input.setBallistics(motion); filtered.setBallistics(motion); processed.setBallistics(motion);
    }
    void refreshTraces() {
        TraceStyle main;
        main.label = QStringLiteral("Before EQ");
        main.line = light ? QColor("#536b79") : QColor("#d2dce0");
        main.fill = light ? QColor(77, 116, 139, 90) : QColor(175, 196, 207, 100);
        spectrum->setTraceStyle(QStringLiteral("input"), main);
        spectrum->setTrace(QStringLiteral("input"), input.frame(), main);
        TraceStyle after;
        after.label = QStringLiteral("After EQ"); after.zOrder = 10;
        after.line = light ? QColor("#167756") : QColor("#80cbaa");
        after.fill = light ? QColor(25, 140, 102, 65) : QColor(128, 203, 170, 60);
        after.fillReference = QStringLiteral("input");
        after.visible = postEq && postEq->isChecked();
        spectrum->setTraceStyle(QStringLiteral("after"), after);
        spectrum->setTrace(QStringLiteral("after"), processed.frame(), after);
        TraceStyle comparison;
        comparison.label = QStringLiteral("Low-pass · 2.4k");
        comparison.line = light ? QColor("#a37620") : QColor("#dfc589");
        comparison.zOrder = 20;
        comparison.filled = false; comparison.penStyle = Qt::DashLine;
        comparison.visible = reference && reference->isChecked();
        spectrum->setTraceStyle(QStringLiteral("filtered"), comparison);
        spectrum->setTrace(QStringLiteral("filtered"), filtered.frame(), comparison);
    }
    void tick() {
        // The demo synthesizes on the UI thread for portability and stays silent.
        // A host should call pushStereo() from its audio callback and read() here.
        const double seconds = std::min(0.1, double(clock.nsecsElapsed()) * 1e-9);
        clock.restart();
        const double desired = seconds * sampleRate + elapsedRemainder;
        const auto count = std::size_t(desired); elapsedRemainder = desired - double(count);
        const double alpha = 1.0 - std::exp(-tau * 2400.0 / sampleRate);
        for (std::size_t i = 0; i < count; ++i, ++position) {
            const double time = double(position) / sampleRate;
            double left = 0, right = 0;
            switch (source->currentIndex()) {
            case 0: {
                const double beat = std::fmod(time, 0.5);
                left = .28 * std::sin(tau * (53 * time + .9 * (1 - std::exp(-beat * 20)))) * std::exp(-beat * 9);
                for (int harmonic = 1; harmonic <= 14; ++harmonic)
                    left += .14 / harmonic * std::sin(tau * 110 * harmonic * time) * (.62 + .38 * std::sin(tau * .31 * time));
                left += .05 * std::sin(tau * 440 * time) + .025 * std::sin(tau * 1760 * time);
                left += pink.next() * (.09 + .15 * std::exp(-std::fmod(time + .25, .5) * 35));
                right = left * .96 + .025 * std::sin(tau * 660 * time); break;
            }
            case 1: left = right = .25 * std::sin(tau * 1000 * time); break;
            case 2: left = right = .25 * std::sin(tau * 60 * time) + .125 * std::sin(tau * 1000 * time); break;
            case 3: left = .25 * std::sin(tau * 1000 * time); right = -left; break;
            case 4: left = right = pink.next() * .7; break;
            case 6:
                if (loaded && !loaded->left.empty()) {
                    const auto at = std::size_t(position % loaded->left.size());
                    left = loaded->left[at]; right = loaded->right[at];
                }
                break;
            default: break;
            }
            input.pushSample(float(left), float(right));
            const auto after = demoEq.process(float(left), float(right));
            processed.pushSample(after[0], after[1]);
            for (int channel = 0; channel < 2; ++channel) {
                lowpass[channel][0] += alpha * ((channel ? right : left) - lowpass[channel][0]);
                lowpass[channel][1] += alpha * (lowpass[channel][0] - lowpass[channel][1]);
            }
            filtered.pushSample(float(lowpass[0][1]), float(lowpass[1][1]));
        }
        const bool changed = input.read(); filtered.read(); processed.read();
        if (changed && !spectrum->frozen()) {
            refreshTraces();
            const auto& frame = input.frame();
            readout->setText(frame.strongestFrequencyHz > 0
                ? QStringLiteral("Strongest peak   %1 Hz   ·   %2 dBFS").arg(frame.strongestFrequencyHz, 0, 'f', 1)
                    .arg(frame.strongestLevelDb, 0, 'f', 1)
                : QStringLiteral("No measurable signal"));
        }
    }

    Analyzer input, filtered, processed;
    DemoEq demoEq;
    SpectrumWidget* spectrum = nullptr;
    QComboBox *source = nullptr, *resolution = nullptr, *speed = nullptr, *tilt = nullptr, *range = nullptr;
    QComboBox *smoothing = nullptr, *motion = nullptr;
    QPushButton *freeze = nullptr, *themeButton = nullptr;
    QCheckBox *peaks = nullptr, *reference = nullptr, *postEq = nullptr;
    QDoubleSpinBox *eqFrequency = nullptr, *eqQ = nullptr;
    QSlider* eqGain = nullptr;
    QLabel* gainLabel = nullptr;
    QLabel *status = nullptr, *readout = nullptr, *sourceInfo = nullptr;
    QTimer timer;
    QElapsedTimer clock;
    double sampleRate = 48000, elapsedRemainder = 0;
    std::uint64_t position = 0;
    bool light = false;
    std::array<std::array<double, 2>, 2> lowpass{};
    PinkNoise pink;
    std::optional<WavFile> loaded;
    QString loadedName;
};
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("VLT Spectre"));
    application.setStyle(QStringLiteral("Fusion"));
    QCommandLineParser options;
    options.addHelpOption();
    options.addOption({QStringLiteral("capture"), QStringLiteral("Save a PNG and exit."), QStringLiteral("path")});
    options.addOption({QStringLiteral("light"), QStringLiteral("Use the light theme.")});
    options.addOption({QStringLiteral("compact"), QStringLiteral("Open the compact layout.")});
    options.addOption({QStringLiteral("flat"), QStringLiteral("Disable display weighting.")});
    options.addOption({QStringLiteral("tone"), QStringLiteral("Analyze the 1 kHz calibration tone.")});
    options.addOption({QStringLiteral("wav"), QStringLiteral("Analyze a WAV file."), QStringLiteral("path")});
    options.process(application);
    DemoWindow window;
    if (options.isSet(QStringLiteral("light"))) window.setLight(true);
    if (options.isSet(QStringLiteral("compact"))) window.resize(680, 640);
    if (options.isSet(QStringLiteral("tone"))) window.selectTone();
    if (options.isSet(QStringLiteral("flat"))) window.setDisplayWeight(0);
    if (options.isSet(QStringLiteral("wav")) && !window.loadFile(options.value(QStringLiteral("wav")))) return 2;
    if (options.isSet(QStringLiteral("capture"))) window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    if (options.isSet(QStringLiteral("capture"))) {
        QTimer::singleShot(1200, &window, [&] {
            const bool saved = window.grab().save(options.value(QStringLiteral("capture")));
            application.exit(saved ? 0 : 3);
        });
    }
    return application.exec();
}