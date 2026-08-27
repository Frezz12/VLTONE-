#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QDockWidget>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

#include "SettingsDialog.h"
#include "daw/rt/RtGuard.h"
#include "core/PeakBuilder.h"
#include "views/LevelMeter.h"
#include "views/MixerView.h"
#include "views/TimelineRuler.h"
#include "views/TimelineView.h"
#include "views/TransportBar.h"

namespace {
constexpr int kTelemetryIntervalMs = 16;   // ~60 Гц

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      engine_(std::make_unique<daw::Engine>()),
      settings_(AudioSettings::load()) {

    buildMenus();
    buildUi();
    connectTransport();

    // Восстанавливаем сохранённый мониторинг ДО запуска потока, иначе
    // restartAudio() затолкал бы в движок состояние пустого чекбокса,
    // а первое же «Применить» затёрло бы сохранённое значение навсегда.
    if (settings_.inputMonitoring) {
        const QSignalBlocker blocker(monitoringBox_);
        monitoringBox_->setChecked(true);
        // Согласие уже было дано в прошлом сеансе — модальным окном на старте
        // не встречаем, но и молчать нельзя: скажем в статусной строке.
        monitoringWarningShown_ = true;
    }

    if (settings_.autoStart)
        restartAudio();

    if (settings_.inputMonitoring && engine_->device().streamInfo().hasInput())
        statusBar()->showMessage(
            tr("Мониторинг входа включён из сохранённых настроек — следите за обратной связью"),
            8000);

    updateAudioStatus();

    peakBuilder_ = new PeakBuilder(this);
    connect(peakBuilder_, &PeakBuilder::peakReady, this, &MainWindow::onPeakReady);

    telemetryTimer_ = new QTimer(this);
    connect(telemetryTimer_, &QTimer::timeout, this, &MainWindow::onTelemetryTick);
    telemetryTimer_->start(kTelemetryIntervalMs);
}

MainWindow::~MainWindow() {
    // Явно останавливаем движок до разрушения виджетов: аудио-поток не должен
    // работать, когда объекты, на которые он косвенно ссылается, уже мертвы.
    engine_->stop();
}

// ---------------------------------------------------------------------------
// Меню
// ---------------------------------------------------------------------------

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&Файл"));

    auto* openAction = fileMenu->addAction(tr("&Открыть аудиофайл…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenAudioFile);

    fileMenu->addSeparator();

    auto* settingsAction = fileMenu->addAction(tr("&Настройки…"));
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    // Роль важна для macOS: там пункт уезжает в меню приложения, как принято.
    settingsAction->setMenuRole(QAction::PreferencesRole);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onOpenSettings);

    fileMenu->addSeparator();

    auto* quitAction = fileMenu->addAction(tr("В&ыход"));
    quitAction->setShortcut(QKeySequence::Quit);
    quitAction->setMenuRole(QAction::QuitRole);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* helpMenu = menuBar()->addMenu(tr("&Справка"));
    auto* aboutAction = helpMenu->addAction(tr("&О программе"));
    aboutAction->setMenuRole(QAction::AboutRole);
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("О программе"),
                           tr("<b>DAW %1</b><br><br>"
                              "Многодорожечная студия звукозаписи.<br>"
                              "Движок на C++20, интерфейс на Qt %2.<br><br>"
                              "Лицензия GPLv3.")
                               .arg(QApplication::applicationVersion())
                               .arg(QStringLiteral(QT_VERSION_STR)));
    });
}

// ---------------------------------------------------------------------------
// Интерфейс
// ---------------------------------------------------------------------------

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("DAW"));

    // ---- Central widget: TransportBar + Timeline -------------------------
    auto* central = new QWidget(this);
    auto* root    = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    transport_ = new TransportBar(central);
    transport_->setStyleSheet(QStringLiteral("background: #2b2d30;"));
    root->addWidget(transport_);

    auto* separator = new QFrame(central);
    separator->setFrameShape(QFrame::HLine);
    separator->setStyleSheet(QStringLiteral("color: #1c1e20;"));
    root->addWidget(separator);

    timelineRuler_ = new TimelineRuler(central);
    root->addWidget(timelineRuler_);

    timelineView_ = new TimelineView(central);
    root->addWidget(timelineView_, 1);

    setCentralWidget(central);

    // ---- Right dock: meters + tone controls ------------------------------
    rightDock_ = new QDockWidget(tr("Панель"), this);
    rightDock_->setFeatures(QDockWidget::DockWidgetMovable
                          | QDockWidget::DockWidgetFloatable);
    rightDock_->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);

    auto* dockContent = new QWidget(rightDock_);
    auto* dockLayout  = new QVBoxLayout(dockContent);
    dockLayout->setContentsMargins(8, 8, 8, 8);
    dockLayout->setSpacing(8);

    auto* inputGroup = new QGroupBox(tr("Вход"), dockContent);
    auto* inputForm  = new QFormLayout(inputGroup);
    meterInL_ = new LevelMeter(inputGroup);
    meterInR_ = new LevelMeter(inputGroup);
    inputForm->addRow(QStringLiteral("L:"), meterInL_);
    inputForm->addRow(QStringLiteral("R:"), meterInR_);
    monitoringBox_ = new QCheckBox(tr("Мониторинг входа"), inputGroup);
    monitoringBox_->setToolTip(
        tr("Направляет сигнал со входа на выход.\n"
           "Через колонки это даёт обратную связь — используйте наушники."));
    inputForm->addRow(monitoringBox_);
    inputStateLabel_ = new QLabel(inputGroup);
    inputStateLabel_->setStyleSheet(QStringLiteral("color: #8a9096;"));
    inputForm->addRow(inputStateLabel_);
    dockLayout->addWidget(inputGroup);

    auto* outputGroup = new QGroupBox(tr("Выход"), dockContent);
    auto* outputForm  = new QFormLayout(outputGroup);
    meterOutL_ = new LevelMeter(outputGroup);
    meterOutR_ = new LevelMeter(outputGroup);
    outputForm->addRow(QStringLiteral("L:"), meterOutL_);
    outputForm->addRow(QStringLiteral("R:"), meterOutR_);
    dockLayout->addWidget(outputGroup);

    auto* toneGroup = new QGroupBox(tr("Тестовый генератор"), dockContent);
    auto* toneForm  = new QFormLayout(toneGroup);
    toneButton_ = new QPushButton(tr("Включить тон"), toneGroup);
    toneButton_->setCheckable(true);
    toneForm->addRow(toneButton_);
    freqSlider_ = new QSlider(Qt::Horizontal, toneGroup);
    freqSlider_->setRange(0, 1000);
    freqSlider_->setValue(500);
    freqLabel_ = new QLabel(toneGroup);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(freqSlider_, 1);
        row->addWidget(freqLabel_);
        toneForm->addRow(tr("Частота:"), row);
    }
    gainSlider_ = new QSlider(Qt::Horizontal, toneGroup);
    gainSlider_->setRange(-60, 0);
    gainSlider_->setValue(-18);
    gainLabel_ = new QLabel(toneGroup);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(gainSlider_, 1);
        row->addWidget(gainLabel_);
        toneForm->addRow(tr("Громкость:"), row);
    }
    dockLayout->addWidget(toneGroup);
    dockLayout->addStretch();

    rightDock_->setWidget(dockContent);
    addDockWidget(Qt::RightDockWidgetArea, rightDock_);

    // ---- Bottom dock: mixer ----------------------------------------------
    mixerDock_ = new QDockWidget(tr("Микшер"), this);
    mixerDock_->setFeatures(QDockWidget::DockWidgetMovable
                          | QDockWidget::DockWidgetFloatable);
    mixerDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    mixerView_ = new MixerView(mixerDock_);
    mixerDock_->setWidget(mixerView_);
    addDockWidget(Qt::BottomDockWidgetArea, mixerDock_);

    connect(mixerView_, &MixerView::trackGainChanged, this,
            [this](int idx, float linear) { engine_->setTrackGain(idx, linear); });
    connect(mixerView_, &MixerView::trackMuteChanged, this,
            [this](int idx, bool on) { engine_->setTrackMuted(idx, on); });
    connect(mixerView_, &MixerView::trackSoloChanged, this,
            [this](int idx, bool on) { engine_->setTrackSoloed(idx, on); });

    // ---- Статусная строка -------------------------------------------------
    statusDevice_ = new QPushButton(this);
    statusDevice_->setFlat(true);
    statusDevice_->setCursor(Qt::PointingHandCursor);
    statusDevice_->setToolTip(tr("Открыть настройки аудио"));
    statusDevice_->setStyleSheet(
        QStringLiteral("QPushButton { border: none; text-align: left; padding: 0 4px; }"
                       "QPushButton:hover { color: #6aa6e8; }"));
    connect(statusDevice_, &QPushButton::clicked, this, &MainWindow::onOpenSettings);

    statusLoad_       = new QLabel(this);
    statusXruns_      = new QLabel(this);
    statusViolations_ = new QLabel(this);

    statusBar()->addWidget(statusDevice_);
    statusBar()->addPermanentWidget(statusViolations_);
    statusBar()->addPermanentWidget(statusXruns_);
    statusBar()->addPermanentWidget(statusLoad_);

    connect(toneButton_,    &QPushButton::toggled, this,
            [this](bool on) { engine_->setToneEnabled(on); });
    connect(freqSlider_,    &QSlider::valueChanged, this, &MainWindow::onFrequencyChanged);
    connect(gainSlider_,    &QSlider::valueChanged, this, &MainWindow::onGainChanged);
    connect(monitoringBox_, &QCheckBox::toggled, this, &MainWindow::onToggleMonitoring);

    onFrequencyChanged(freqSlider_->value());
    onGainChanged(gainSlider_->value());

    connect(timelineView_, &TimelineView::locateRequested,
            this, &MainWindow::onLocateRequested);
    connect(timelineView_, &TimelineView::zoomChanged, this,
            [this](double z) { timelineRuler_->setZoom(z); });

    resize(1200, 720);
}

void MainWindow::connectTransport() {
    connect(transport_, &TransportBar::playToggled, this, &MainWindow::onTogglePlay);

    connect(transport_, &TransportBar::returnToZeroRequested, this,
            [this] { engine_->returnToZero(); });

    connect(transport_, &TransportBar::tempoChanged, this,
            [this](double bpm) { engine_->setTempo(bpm); });

    connect(transport_, &TransportBar::timeSignatureChanged, this,
            [this](int numerator, int denominator) {
                engine_->setTimeSignature(numerator, denominator);
            });

    connect(transport_, &TransportBar::metronomeToggled, this,
            [this](bool on) { engine_->setMetronomeEnabled(on); });

    connect(transport_, &TransportBar::metronomeGainChanged, this,
            [this](float gain) { engine_->setMetronomeGain(gain); });

    // Пробел — play/stop, как во всех DAW.
    auto* playShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(playShortcut, &QShortcut::activated, this, [this] {
        if (!engine_->isRunning())
            return;
        // Состояние читаем один раз: команда уходит в очередь и isPlaying()
        // обновится только после следующего callback'а.
        const bool next = !engine_->isPlaying();
        transport_->setPlaying(next);
        onTogglePlay(next);
    });

    // Начальные значения из панели — чтобы движок и UI не разъехались.
    engine_->setTempo(transport_->tempo());
    engine_->setMetronomeGain(dbToLinear(-12.0f));
}

// ---------------------------------------------------------------------------
// Настройки и запуск звука
// ---------------------------------------------------------------------------

void MainWindow::onOpenSettings() {
    if (settingsDialog_) {
        settingsDialog_->raise();
        settingsDialog_->activateWindow();
        return;
    }

    auto* dialog = new SettingsDialog(
        settings_, engine_->device().devices(),
        QString::fromStdString(engine_->device().currentApiName()), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    settingsDialog_ = dialog;

    connect(dialog, &SettingsDialog::settingsApplied, this, &MainWindow::onApplySettings);
    connect(dialog, &QObject::destroyed, this, [this] { settingsDialog_ = nullptr; });

    dialog->setStreamInfo(engine_->device().streamInfo(), startupWarning_);
    dialog->show();
}

void MainWindow::onApplySettings(const AudioSettings& settings) {
    settings_ = settings;
    settings_.inputMonitoring = monitoringBox_->isChecked();
    settings_.save();

    restartAudio();
    updateAudioStatus();

    if (settingsDialog_)
        settingsDialog_->setStreamInfo(engine_->device().streamInfo(), startupWarning_);
}

void MainWindow::restartAudio() {
    const bool         wasPlaying  = engine_->isPlaying();
    // Engine::prepare() обнуляет позицию транспорта, поэтому без явного
    // сохранения любое «Применить» отбрасывало бы курсор на первый такт.
    const std::int64_t wasPosition = engine_->playPosition();

    engine_->stop();
    startupWarning_.clear();

    const auto devices = engine_->device().devices();
    if (devices.empty()) {
        startupWarning_ = tr("Аудио-устройства не найдены.");
        return;
    }

    // Предупреждаем до запуска: подмена устройства должна быть заметной.
    const auto resolution = settings_.resolve(devices);
    QStringList notes;
    if (resolution.outputFallback)
        notes << tr("Устройство вывода «%1» не найдено — выбрано по умолчанию.")
                     .arg(resolution.missingOutput);
    if (resolution.inputFallback)
        notes << tr("Устройство входа «%1» не найдено — выбрано по умолчанию.")
                     .arg(resolution.missingInput);

    const auto config = settings_.toConfig(devices,
                                           engine_->device().defaultOutputDeviceId(),
                                           engine_->device().defaultInputDeviceId());

    if (!engine_->start(config)) {
        notes << tr("Не удалось запустить звук: %1")
                     .arg(QString::fromStdString(engine_->device().lastError()));
        startupWarning_ = notes.join(QLatin1Char('\n'));
        statusBar()->showMessage(startupWarning_, 8000);
        return;
    }

    const auto& warning = engine_->device().lastWarning();
    if (!warning.empty())
        notes << QString::fromStdString(warning);

    startupWarning_ = notes.join(QLatin1Char('\n'));
    if (!startupWarning_.isEmpty())
        statusBar()->showMessage(startupWarning_, 8000);

    // Восстанавливаем состояние, которое пережило перезапуск потока.
    engine_->setTempo(transport_->tempo());
    engine_->setMetronomeEnabled(transport_->isMetronomeEnabled());
    engine_->setToneEnabled(toneButton_->isChecked());
    engine_->setInputMonitoring(monitoringBox_->isChecked());

    if (wasPosition > 0)
        engine_->locate(wasPosition);

    if (wasPlaying)
        engine_->play();
}

void MainWindow::updateAudioStatus() {
    const auto& info = engine_->device().streamInfo();

    const bool hasInput = info.hasInput();
    monitoringBox_->setEnabled(hasInput);
    if (!hasInput && monitoringBox_->isChecked())
        monitoringBox_->setChecked(false);

    inputStateLabel_->setText(
        hasInput ? tr("%1 — %2 кан.")
                       .arg(QString::fromStdString(info.inputDeviceName))
                       .arg(info.inputChannels)
                 : tr("Вход не выбран. Настроить: Файл → Настройки → Аудио."));

    if (!info.isOpen) {
        statusDevice_->setText(tr("⚠  Звук не запущен — открыть настройки"));
        statusDevice_->setStyleSheet(
            QStringLiteral("QPushButton { border: none; text-align: left; padding: 0 4px;"
                           " color: #d08030; }"
                           "QPushButton:hover { color: #e8a050; }"));
    } else {
        statusDevice_->setText(
            tr("%1  |  %2 Гц  |  буфер %3  |  вход: %4")
                .arg(QString::fromStdString(info.outputDeviceName))
                .arg(info.sampleRate, 0, 'f', 0)
                .arg(info.bufferFrames)
                .arg(hasInput ? QString::fromStdString(info.inputDeviceName) : tr("нет")));
        statusDevice_->setStyleSheet(
            QStringLiteral("QPushButton { border: none; text-align: left; padding: 0 4px; }"
                           "QPushButton:hover { color: #6aa6e8; }"));
    }

    transport_->setTransportEnabled(info.isOpen);
    toneButton_->setEnabled(info.isOpen);
}

// ---------------------------------------------------------------------------
// Обработчики
// ---------------------------------------------------------------------------

void MainWindow::onTogglePlay(bool playing) {
    if (playing)
        engine_->play();
    else
        engine_->stopTransport();
}

void MainWindow::onOpenAudioFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Открыть аудиофайл"), QString(),
        tr("Аудиофайлы (*.wav *.aiff *.aif *.flac *.ogg);;Вcе файлы (*)"));

    if (path.isEmpty())
        return;

    // Загрузка идёт в UI-потоке: файл целиком читаетcя в память.
    // Диcковый cтриминг — задача §8, пока проект помещаетcя в RAM.
    auto source = std::make_shared<daw::model::Source>();
    if (!source->loadFromFile(path.toStdString())) {
        QMessageBox::warning(this, tr("Не удалоcь открыть файл"),
                             tr("Файл «%1» не удалоcь прочитать.\n\n"
                                "Возможно, формат не поддерживаетcя.")
                                 .arg(QFileInfo(path).fileName()));
        return;
    }

    // Чаcтота файла и чаcтота движка могут не cовпадать: реcемплинга ещё нет,
    // поэтому предупреждаем — иначе материал зазвучит не в том темпе.
    const double engineRate = engine_->device().streamInfo().sampleRate;
    if (engineRate > 0.0 && source->sampleRate() > 0
        && std::abs(engineRate - source->sampleRate()) > 0.5) {
        statusBar()->showMessage(
            tr("Файл запиcан на %1 Гц, движок работает на %2 Гц — "
               "выcота и темп будут неверны (реcемплинга пока нет)")
                .arg(source->sampleRate())
                .arg(engineRate, 0, 'f', 0),
            10000);
    }

    // Клип на вcю длину иcточника, cтоит в начале таймлайна.
    const std::int64_t frames = source->frames();
    auto clip = std::make_shared<daw::model::Clip>(source, 0, frames, 0);

    // Новая cеccия на каждый файл: полноценное добавление дорожек к
    // cущеcтвующему проекту — это уже M2 c командами и undo.
    auto session = std::make_shared<daw::model::Session>(
        engineRate > 0.0 ? engineRate : 48000.0);

    auto track = std::make_shared<daw::model::Track>(
        QFileInfo(path).completeBaseName().toStdString());
    track->addClip(clip);
    session->addTrack(std::move(track));

    // Публикация нового графа: транcпорт оcтанавливаем и отматываем в ноль,
    // чтобы курcор не оказалcя за концом только что загруженного клипа.
    engine_->stopTransport();
    engine_->returnToZero();
    transport_->setPlaying(false);

    engine_->setSession(session);

    lastOpenedFile_ = path;
    updateAudioStatus();

    // Показываем сессию в таймлайне
    timelineView_->setSession(session);
    timelineView_->setSampleRate(source->sampleRate());
    timelineRuler_->setSampleRate(source->sampleRate());
    timelineRuler_->setZoom(timelineView_->samplesPerPixel());

    // Полоcы микшера под дорожки новой cеccии
    mixerView_->setSession(session);

    // Фоновая сборка пиков
    peakBuilder_->start(source, path);

    const double seconds = source->durationSeconds();
    statusBar()->showMessage(
        tr("Загружено: %1 — %2 кан., %3 Гц, %4 c")
            .arg(QFileInfo(path).fileName())
            .arg(source->channels())
            .arg(source->sampleRate())
            .arg(seconds, 0, 'f', 2),
        6000);
}

void MainWindow::onToggleMonitoring(bool enabled) {
    // Обратная связь через колонки бьёт и по ушам, и по динамикам,
    // поэтому предупреждаем до того, как звук пойдёт.
    if (enabled && !monitoringWarningShown_) {
        monitoringWarningShown_ = true;
        const auto answer = QMessageBox::warning(
            this, tr("Мониторинг входа"),
            tr("Сигнал со входа пойдёт на выход.\n\n"
               "Если микрофон и колонки в одной комнате, возникнет обратная "
               "связь — резкий громкий вой. Используйте наушники.\n\n"
               "Включить мониторинг?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (answer != QMessageBox::Yes) {
            const QSignalBlocker blocker(monitoringBox_);
            monitoringBox_->setChecked(false);
            return;
        }
    }

    engine_->setInputMonitoring(enabled);
    settings_.inputMonitoring = enabled;
}

void MainWindow::onFrequencyChanged(int value) {
    // Логарифмическая шкала: 20 Гц … 20 кГц. Линейная бесполезна — почти весь
    // ход слайдера пришёлся бы на неразличимый на слух верх.
    const float t  = value / 1000.0f;
    const float hz = 20.0f * std::pow(1000.0f, t);
    engine_->setToneFrequency(hz);
    freqLabel_->setText(tr("%1 Гц").arg(hz, 0, 'f', hz < 100.0f ? 1 : 0));
    freqLabel_->setMinimumWidth(70);
}

void MainWindow::onGainChanged(int value) {
    const float db = static_cast<float>(value);
    engine_->setToneGain(value <= -60 ? 0.0f : dbToLinear(db));
    gainLabel_->setText(value <= -60 ? tr("−∞ дБ") : tr("%1 дБ").arg(value));
    gainLabel_->setMinimumWidth(70);
}

void MainWindow::onTelemetryTick() {
    // Освобождаем отставленные версии карты темпа. Делать это в аудио-потоке
    // нельзя, поэтому сбор мусора живёт здесь, в UI-таймере.
    engine_->collectRetired();

    if (!engine_->isRunning()) {
        meterInL_->setLevel(0.0f);
        meterInR_->setLevel(0.0f);
        meterOutL_->setLevel(0.0f);
        meterOutR_->setLevel(0.0f);
        statusLoad_->clear();
        statusXruns_->clear();
        return;
    }

    meterInL_->setLevel(engine_->inputPeak(0));
    meterInR_->setLevel(engine_->inputPeak(1));
    meterOutL_->setLevel(engine_->peak(0));
    meterOutR_->setLevel(engine_->peak(1));

    const auto&  map      = engine_->tempoMap();
    const auto   position = engine_->playPosition();
    const auto   tick     = map.sampleToTick(position);
    const double seconds  = map.sampleRate() > 0.0
                          ? static_cast<double>(position) / map.sampleRate()
                          : 0.0;

    transport_->setPosition(map.tickToBarBeat(tick), seconds);
    transport_->setPlaying(engine_->isPlaying());

    timelineView_->setPlayheadPosition(position);
    timelineView_->setPlaying(engine_->isPlaying());
    timelineRuler_->setTempoMap(map);

    statusLoad_->setText(tr("Нагрузка: %1%").arg(engine_->cpuLoad() * 100.0, 0, 'f', 1));

    const auto xruns = engine_->xruns();
    statusXruns_->setText(tr("Дропауты: %1").arg(xruns));
    statusXruns_->setStyleSheet(xruns > 0 ? QStringLiteral("color: #d04030;") : QString());

    // Нарушения реального времени — главный индикатор здоровья на этом этапе.
    if (daw::rt::checksEnabled()) {
        const auto v = daw::rt::violations();
        if (v.count == 0) {
            statusViolations_->setText(tr("RT: чисто"));
            statusViolations_->setStyleSheet(QStringLiteral("color: #4a9a4a;"));
        } else {
            statusViolations_->setText(
                tr("RT: %1 нарушений (%2)")
                    .arg(v.count)
                    .arg(v.last ? QString::fromLatin1(v.last) : QStringLiteral("?")));
            statusViolations_->setStyleSheet(QStringLiteral("color: #d04030;"));
        }
    } else {
        statusViolations_->setText(tr("RT-проверки выключены"));
    }
}

void MainWindow::onPeakReady(const QString& audioPath,
                             std::shared_ptr<daw::graph::PeakFile> peakFile)
{
    timelineView_->setPeakFile(audioPath.toStdString(), std::move(peakFile));
}

void MainWindow::onLocateRequested(std::int64_t sample)
{
    engine_->locate(sample);
    transport_->setPlaying(false);
    engine_->stopTransport();
}
