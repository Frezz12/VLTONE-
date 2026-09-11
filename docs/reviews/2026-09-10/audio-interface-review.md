# Ревью аудиотракта VLTONE — 10 сентября 2026

Ревью текущей рабочей копии, включая незакоммиченные оптимизации. Исправления производственного кода не вносились. Найдены 14 пунктов: ошибки мониторинга, жизненного цикла устройства и записи, а также лишняя работа и ограничения диагностики. Это перечень обнаруженного, а не гарантия отсутствия других дефектов.

## Что связано с описанными симптомами

**Новая дорожка молчит до выбора Input 1 — воспроизведено точно.** Новая проверка `monitor && inputEnabled` стала учитывать отключённый вход, но конструкторы дорожек и старые документы остались с `inputEnabled=false`. В исходном `/Users/nikolay/Documents/Стекла.vlt/Стекла.vlt` все десять аудиодорожек имеют отключённый вход; у `LEAD 2` одновременно сохранён `monitor=true`. Это объясняет и регрессию при открытии этого проекта.

**Микрофон периодически прерывается при остановленном транспорте — найден и воспроизведён независимый механизм.** Фоновое сохранение состояния плагина раз в секунду допускается во время мониторинга и на время вызова глушит весь граф. Продолжительность зависит от плагина.

**После сбоя устройства звук не восстанавливается или ведёт себя неправильно — воспроизведено с имитацией отказа драйвера.** Приложение может считать неактивный поток работающим; аварийное переключение может оставить частоты устройства и движка разными.

Установить, какой именно механизм сработал вчера, по одному описанию нельзя. Сохранённые сейчас настройки указывают на встроенные микрофон и динамики Mac, 48 кГц, буфер 32, вход устройства включён. Автоматическое включение мониторинга при записи выключено. Это состояние настроек на момент ревью, не журнал ночного сбоя. Оно не отменяет ручное включение мониторинга.

## Обнаруженные дефекты

### A01 · P1 · Новая проверка входа нарушила готовность новой и ранее сохранённой дорожки к мониторингу

Код: [EngineController.cpp:3491](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:3491), [Document.hpp:707](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/model/Document.hpp:707), [CreateTracksDialog.cpp:294](/Users/nikolay/Documents/code/VLTONE/VLTONE-/app/CreateTracksDialog.cpp:294), [ProjectSerializer.cpp:745](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/ProjectSerializer.cpp:745).

В текущем diff `setEnabled(track.monitor)` заменён на `setEnabled(track.monitor && track.inputEnabled)`. При этом `newProject(true)`, `addTrack`, стандартный TrackCreationRequest и диалог создания оставляют вход выключенным. Нажатие Monitor меняет только `monitor`. Старые сохранённые проекты с этим сочетанием флагов тоже становятся немыми.

Проверка через реальный DeviceCallback без устройства: входной сигнал 0.25, новая дорожка, Monitor → выход **0.000000**. Включение первого входа → **0.250000**. Это подтверждённая регрессия относительно HEAD.

Исправлять нужно согласование нового поведения с созданием дорожек и миграцией старых настроек. Простое удаление проверки снова нарушит явный выбор No Input. Нужна отдельная стратегия для старого неоднозначного сохранённого `false`.

### A02 · P1 · Автосохранение периодически заглушает живой мониторинг

Код: [MainWindow.cpp:7868](/Users/nikolay/Documents/code/VLTONE/VLTONE-/app/MainWindow.cpp:7868), [EngineController.cpp:4339](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:4339), [RealtimeEngine.cpp:213](/Users/nikolay/Documents/code/VLTONE/VLTONE-/engine/Engine/RealtimeEngine.cpp:213).

Таймер восстановления работает раз в секунду. Его `transportActive` проверяет воспроизведение, запись и отсчёт, но не живой аудио-/MIDI-ввод. Поэтому при остановленном транспорте, включённом мониторинге и отпущенной мыши вызывается `refreshRecoveryPluginStates(1)`. Вызов `saveState()` находится внутри RenderGate. Пока gate закрыт, callback выдаёт нули во все выходные каналы.

В тесте с настоящим CLAP-модулем, которому в частной копии добавлена задержка сохранения 20 мс, каждый вызов дал **31–39 тихих блоков** при 48 кГц / 32. Пять вызовов дали 178 gated blocks. Это измерение поведения хоста при контролируемой задержке, не измерение скорости конкретного Waves/FabFilter.

Первая мера — исключить фоновую сериализацию из всех режимов живого ввода. Дальше нужен безопасный кэш состояния и правила сериализации по контрактам форматов. Нельзя просто убрать RenderGate и допустить гонку `saveState/process`.

### A03 · P1 · Фактическая остановка устройства не меняет состояние хоста

Код: [AudioDeviceManager.hpp:93](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.hpp:93), [AudioDeviceManager.cpp:620](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:620), [AudioDeviceManager.cpp:1047](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:1047).

`isRunning()` читает флаг, установленный после `Pa_StartStream`. Он меняется только при явных операциях самого приложения. Интерфейс уведомлений устройства объявлен, но обработчики не вызываются и потребитель не зарегистрирован. Нет контроля пропавших callbacks и автоматического восстановления остановившегося потока. Обычные Play/Monitor перестраивают модель/граф, но не возобновляют устройство.

Имитация драйвера: поток стал неактивным, а менеджер продолжил возвращать **running=1, state=Running**. Это подтверждает отсутствие реакции хоста; реальное отключение USB и sleep/wake не воспроизводились. Для проверки живости нужно учитывать фактическое состояние потока и поступление callbacks; [PortAudio описывает отдельный запрос Pa_IsStreamActive](https://portaudio.com/docs/v19-doxydocs/portaudio_8h.html).

### A04 · P1 · Аварийное восстановление оставляет разные частоты устройства и движка

Код: [AudioDeviceManager.cpp:460](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:460), [EngineController.cpp:18240](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:18240).

Если новый и прежний потоки не открываются, менеджер пробует системный выход с отключённым входом. При неподдерживаемой частоте `openStream()` переходит на native rate. Менеджер возвращает ошибку исходного переключения даже при успешном fallback. Контроллер на этой ветке сразу возвращает callback и выходит, не согласовав граф с фактически открытой конфигурацией.

В fault-injection: **устройство 44100 Гц, движок и проект 48000 Гц, running=1, inputEnabled=0**. Следствия — другая скорость/высота воспроизведения, неверная временная шкала, потеря микрофона. В [AudioSettingsPage.cpp:455](/Users/nikolay/Documents/code/VLTONE/VLTONE-/app/AudioSettingsPage.cpp:455) при этом написано, что прежнее устройство всё ещё активно, хотя оно может быть заменено или вообще не работать.

Нужен результат переключения, отдельно описывающий ошибку запроса и фактическое восстановленное состояние. Согласование engine/device обязательно и на ошибочной ветке.

### A05 · P1 · Смена sample rate во время записи повреждает временной масштаб дубля

Код: [EngineController.cpp:18219](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:18219), [EngineController.cpp:16166](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:16166), [EngineControllerRender.cpp:689](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineControllerRender.cpp:689).

Применение аудионастроек разрешено при активной записи. Движок переводится на новую частоту, а отдельные recorder-объекты в `m_captures` продолжают писать файл с частотой, зафиксированной при старте. Вызов `m_recorder->initialize(...)` касается другого объекта; существующие capture-recorders он не меняет.

Проверка: старт при 48 кГц → Apply 96 кГц → 9600 входных кадров. Устройство передало **0.100 с**, WAV получил частоту **48000** и длительность **0.200 с**. Нужен запрет смены формата в незавершённом дубле либо корректное завершение/разделение записи с явной временной привязкой.

### A06 · P1 · Отключение входа устройства внутри дубля вырезает время без отметки потери

Код: [EngineController.cpp:1118](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:1118), [RecordingEngine.cpp:367](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Recording/RecordingEngine.cpp:367).

Когда поток становится output-only, `ctx.inputBuffer` отсутствует и recorder вообще не вызывается. Транспорт продолжает идти. После включения входа данные дописываются вплотную, без тишины на пропущенном месте и без завершения повреждённого сегмента.

Проверка реального пути `applyAudioConfiguration`, с подменой только PortAudio: 100 блоков с входом, 100 без входа, 100 снова с входом. Из ожидаемых **9600** кадров записано **6400**, `droppedFrames=0`. Это сценарий отключения/возврата входа, в том числе fallback; он не предполагает, что обычный PortAudio input underflow обязательно даёт nullptr — PortAudio обычно подставляет нулевые отсчёты.

### A07 · P1 · Локальное завершение записи игнорирует потерю данных и ошибку записи файла

Код: [RecordingEngine.cpp:392](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Recording/RecordingEngine.cpp:392), [EngineController.cpp:16836](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:16836), [EngineController.cpp:16932](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:16932).

При переполнении кольца recorder учитывает `droppedFrames`, но не сохраняет положение пропущенного участка: следующие данные становятся рядом с предыдущими. Ошибки файлового writer также учитываются. Однако локальный `stopRecording()` принимает любой читаемый префикс, не проверяя `fileWriteSucceeded` и `droppedFrames`, и добавляет его в проект обычным дублем. При полной нечитаемости материал просто пропускается. Облачный путь классифицирует эти ошибки отдельно, локальный — нет.

Подтверждено чтением всех веток финализации; реальный отказ диска в этом ревью не вызывался. Нужны предупреждение о повреждённой записи, сохранение восстановимого материала и позиции разрывов. Нельзя маскировать потерю сокращённой длиной файла.

### A08 · P2 · Smart Monitoring считает заглушённую дорожку слышимым источником

Код: [EngineController.cpp:16618](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:16618).

Проверяется лишь совпадение начального номера входа и флаги `monitor/inputEnabled`. Не учитываются mute/solo и слышимость выходной цепи. Заглушённая дорожка с Monitor не позволяет открыть автоматический мониторинг новой записываемой дорожки. Кроме того, сравнение только первого канала не описывает перекрытие mono/stereo входов.

В тесте: первая дорожка с Monitor + Mute, вторая пишет тот же вход → **recording=1, new_monitor=0, output=0**. Ручное включение Monitor второй дорожки возвращает выход **0.25**. Проверка сделана с Auto Monitor on Record=true; сохранённая сейчас пользовательская настройка false, поэтому именно этот автоматический сценарий не следует считать доказанной причиной ночного сбоя.

### A09 · P2 · Запись не привязана к аппаратному времени входного сигнала

Код: [AudioDeviceManager.cpp:267](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:267), [IAudioCallback.hpp:9](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Core/IAudioCallback.hpp:9), [EngineController.cpp:16098](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:16098), [EngineController.cpp:16216](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:16216).

В callback передаётся время выхода для playhead, но `inputBufferAdcTime` отбрасывается. `sampleTime` остаётся нулём. Положение начала capture читается на GUI-потоке до подготовки recorders и перестройки графа; публикация recorders, начало транспорта и первый принятый блок не являются одной операцией на границе callback. При punch-in в движущийся транспорт подготовка тоже занимает время.

Нет достаточных данных, чтобы компенсировать аппаратную задержку входа и точно привязать первый блок к слышимому сопровождению. Это ошибка точности записи, а не доказанная причина отсутствия звука. Абсолютный сдвиг здесь не измерен: нужен физический loopback. [PortAudio определяет inputBufferAdcTime как время захвата первого входного отсчёта, а outputBufferDacTime — время его выхода](https://portaudio.com/docs/v19-doxydocs/structPaStreamCallbackTimeInfo.html).

### A10 · P2 · Одно применение аудионастроек запускает поток три раза

Код: [AudioDeviceManager.cpp:1039](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:1039), [EngineController.cpp:18237](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:18237).

`setAudioCallback(nullptr)` останавливает и снова запускает текущий поток. Затем `applyConfiguration` закрывает/открывает и запускает новый. Затем `setAudioCallback(callback)` опять останавливает и запускает его. Первые два запуска идут без production callback. Даже Apply без изменения настроек проходит все эти операции. Возвраты `stop/start` внутри setter отбрасываются.

Счётчик mock PortAudio подтвердил **три Pa_StartStream на один Apply**, включая полностью неизменную конфигурацию. Это лишние разрывы, повторная настройка realtime workers и лишние возможности отказа драйвера. Нужна одна транзакция stop → configure/prepare → attach → start и возврат всех ошибок; no-op должен обходить переоткрытие.

### A11 · P2 · Выбор входа многократно перестраивает весь проект; создание дорожек глушит граф на всю подготовку

Код: [ChannelStrip.cpp:1684](/Users/nikolay/Documents/code/VLTONE/VLTONE-/app/ChannelStrip.cpp:1684), [EngineController.cpp:7563](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:7563), [EngineController.cpp:3795](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:3795), [EngineController.cpp:6707](/Users/nikolay/Documents/code/VLTONE/VLTONE-/controller/EngineController.cpp:6707).

Один выбор в меню вызывает отдельные setter для first channel, width и enabled. Каждый изменившийся параметр на monitored/armed дорожке вызывает rebuildGraph. Он заново синхронизирует клипы, MIDI и автоматизацию всех дорожек, хотя меняется один маршрут. В тесте один выбор дал **три rebuild**. Не каждый обычный rebuild закрывает RenderGate — приписывать им всем обязательную тишину было бы неверно.

Отдельно `createTracks()` удерживает RenderGate вокруг всей вставки, компиляции, загрузки плагинов и восстановления их состояния. Поэтому время подготовки новой дорожки становится тишиной на уже работающих выходах. На пустых дорожках тест поймал один тихий блок; длительность тяжёлого plugin preset этим тестом не измерялась. Подготовку новых объектов стоит выполнять до короткой безопасной публикации, а изменение входа делать одним согласованным обновлением.

### A12 · P2 · На CoreAudio и других non-ASIO backend доступны только первые два входа

Код: [AudioDeviceManager.cpp:543](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:543).

Для non-ASIO поток открывается с `min(2, maxInputChannels)`. Интерфейс на 4/8/16 входов не даст выбрать и записать вход 3 и далее; сохранённая на другой конфигурации дорожка с таким индексом получит нули. Это существующее ограничение реализации, не объяснение тишины встроенного первого входа. Для полноценной многоканальной записи нужны выбранные физические каналы и согласованное отображение их индексов.

### A13 · P2 · «UID» устройства зависит от его имени

Код: [AudioDeviceManager.cpp:64](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:64), [AudioDeviceManager.cpp:867](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:867).

Идентификатор строится как `host API + name`; поиск возвращает первое совпадение. Два одинаково названных интерфейса неразличимы, а переименование/смена отображаемого имени делает сохранённый выбор неразрешимым и запускает fallback с отключённым входом. Это подтверждённое свойство кода, но смена имени на этом Mac не установлена. Нужны native persistent IDs и миграция сохранённых name-based значений.

### A14 · P2 · Диагностика скрывает некоторые причины тишины

Код: [AudioDeviceManager.cpp:709](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:709), [AudioDeviceManager.cpp:769](/Users/nikolay/Documents/code/VLTONE/VLTONE-/core/Device/AudioDeviceManager.cpp:769), [RealtimeEngine.cpp:252](/Users/nikolay/Documents/code/VLTONE/VLTONE-/engine/Engine/RealtimeEngine.cpp:252).

`diagRenderCallCount` увеличивается даже при отсутствующем callback, `diagLastRenderStatus` всегда получает 0, `diagRenderFailCount` нигде не увеличивается. Ошибка обработки графа превращается в тишину без сохранения её кода. При исчезнувшем input buffer последние значения входных peak/RMS не сбрасываются.

При этом отдельные счётчики PortAudio xruns и gated blocks уже есть — считать всю диагностику отсутствующей неверно. Не хватает согласованного состояния: callback жив/мертв, причина заглушения, ошибка графа, причина смены конфигурации и нарушения конкретного дубля. Длительность CPU обработки сама по себе не отличает корректный тихий проект от аварийной выдачи нулей.

## Производительность и проверенные ограничения

- Самые конкретные лишние операции: три перезапуска на Apply, до трёх полных rebuild на один выбор входа, сериализация состояния при живом мониторинге, широкий RenderGate создания дорожек. При 48 кГц / 32 бюджет блока — около 0.667 мс, и даже короткое закрытие gate может затронуть слышимый блок.
- У каждого активного recorder собственный writer thread с polling раз в 5 мс. Это до 200 пробуждений в секунду на дорожку; для многодорожечной записи стоит измерить общую файловую очередь с ограниченным числом workers. Для одной дорожки это не подтверждённый bottleneck.
- Граф и recorder list читаются через hazard-pointer публикацию, без mutex на обычном пути. Acquire-петли не имеют фиксированного числа попыток; их стоит включить в стресс-тесты частых публикаций. Зависания этих петель в ревью не наблюдалось.
- Input/output wrapper не выделяет память на каждом callback. InputNode копирует planar audio без промежуточного interleave. Spectrum уже отключается при отсутствии consumers. Эти места не следует объявлять причиной без измерений.
- Проба загрузки копии «Стекла» из отдельного headless executable не дала живых AU inserts, поэтому её результаты не использованы для утверждений о скорости сторонних плагинов. Предупреждения vendor runtime о повторных Objective-C class names также не доказывают причину исчезновения звука.
- Физические устройства, аппаратные xruns, ASIO на Windows, длительная запись и loopback в этом ревью не проверялись. Все fault-injection проверки выполняли текущий код VLTONE с синтетическими буферами и подменой PortAudio; системные аудионастройки не менялись.

## Проверки и дальнейший порядок исправлений

Пять существующих наборов прошли: platform_test, controller_test, engine_graph_test, recording_preview_test, audio_presentation_clock_test. Они не покрывают перечисленные аварийные сценарии. [Полный результат](/Users/nikolay/Documents/code/VLTONE/VLTONE-/docs/reviews/2026-09-10/audio-evidence/ctest-output.txt).

Сохранены [probe мониторинга](/Users/nikolay/Documents/code/VLTONE/VLTONE-/docs/reviews/2026-09-10/audio-evidence/review_probe.cpp), [подмена PortAudio](/Users/nikolay/Documents/code/VLTONE/VLTONE-/docs/reviews/2026-09-10/audio-evidence/device_probe.cpp), [probe автосохранения](/Users/nikolay/Documents/code/VLTONE/VLTONE-/docs/reviews/2026-09-10/audio-evidence/slow_recovery_probe.cpp), их числовые результаты и SHA-256 просмотренных исходников. [Скрипт повторного запуска](/Users/nikolay/Documents/code/VLTONE/VLTONE-/docs/reviews/2026-09-10/audio-evidence/run-probes.py) использует существующую macOS-сборку и создаёт отдельные временные исполняемые файлы; он не открывает аудиоустройство. Перед повторением на изменённом коде нужно пересобрать controller_test и CLAP fixture.

Порядок работы: A01 и A02 возвращают ожидаемый мониторинг; A03–A05 и A10 исправляют жизненный цикл устройства; A06–A09 обеспечивают сохранность и привязку записи; A11 снимает лишние пересборки; A12–A14 завершают многоканальность, идентификацию и диагностику. После этого — аппаратная матрица 32/64/128/512, новые/старые проекты, Monitor при Stop и Play, 30 минут записи, USB reconnect, sleep/wake, смена устройств и подтверждение смещения записи через loopback.
