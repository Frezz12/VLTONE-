# Проверка исправлений прокрутки

macOS arm64, Qt 6.11.2, RelWithDebInfo. Сборка и нагрузочные серии выполнялись последовательно. Финальные исходники рабочего дерева идентифицированы в [source-sha256.json](source-sha256.json); файлы содержат также сохранённые изменения других задач. Исходное ревью и его пробы находятся в [соседней папке](../timeline-scroll-evidence/README.md).

Исправления значительно уменьшают стоимость рисования. Строгий порог 16,67 мс не проходит стабильно во всех повторах: ниже сохранены успешные и неуспешные серии, а также оставшиеся задержки. Аппаратное устройство и пользовательский проект с плагинами не запускались.

## Изолированная отрисовка волн

8 волн, холст 1600×960, высота волны 68, шаг ряда 80, DPR 2. Старый контрольный опыт: 30 итераций, новый: 60. Это стоимость renderer, не FPS приложения. Новая отдельная проба использует обычную настройку Qt; политика пула в main приложения к ней не применяется.

| Renderer / режим | Медиана, мс | p95, мс |
|---|---:|---:|
| Исходный QPainterPath, контрольный опыт ревью | 78,967 | 82,007 |
| Новый renderer, повторная отрисовка | 2,599 | 3,525 |
| Новый renderer, прокрутка | 2,580 | 3,974 |

Стоимость этого этапа снизилась примерно в 30 раз. Измеряются построение тайлов, копирование, ось и очистка холста. Источник новых чисел — [ctest.txt](ctest.txt), waveform_paint_test. Старые исходники и методика сохранены в evidence ревью.

## Функциональные проверки

[Основные 9 CTest](ctest.txt) прошли: controller_test, recording_preview_test, waveform_paint_test, ui_frame_clock_test, engine_graph_test, platform_test, recording_commit_planner_test, audio_realtime_test, miditools_test.

Проверены транзиенты после двух секунд без чтения UI, Input 3/mono/No Input, согласованная смена маршрута на границе аудиоблока, 302 loop-прохода с сохранением всех takes, совпадение пикселей при cached/uncached отрисовке, швы и лимит raster-памяти, сохранение тяжёлого fan-out в audio graph и обнаружение 320-мс GUI stall.

После последнего изменения политики Qt прошли [навигация](final-policy-navigation.txt), [pattern](final-policy-pattern.txt) и [редакторы](final-policy-editor.txt). Навигационный тест явно включает Zoom Focus для focus-assertions; настройка приложения по умолчанию не менялась. FrameClock после добавления slow logging [проверен отдельно](frame-clock-final.txt). Финальная сборка daw прошла. Сводка всех кодов завершения, включая неуспешные измерительные серии: [verification.json](verification.json).

## Прокрутка одновременно с аудиотрактом

Восемь WAV-дорожек, окно 1600×960, 48 кГц; playback, затем playback + capture Input 3. Реальные device callback, graph, recorder и realtime-регистрация 7 helpers и вызывающего потока, без device workgroup. Перед жестом 250 мс проверяется повторное использование статической подложки. Жест длится 1,5 секунды, wheel приходит примерно каждые 8 мс, FrameClock настроен на 60 Гц.

Ниже финальная полная матрица после изменения политики Qt. Числа относятся только к активному жесту. Все времена в мс; порог p95 paint явно установлен в 16,67 мс. Из 16 комбинаций прошли 15; 32 + recording, DPR 2, дал p95 17,984 мс. Все аудиоколбэки этой матрицы уложились в срок.

| DPR | Буфер | Запись | Paint p50 / p95 / max | Interval p95 / max | Callback max | Callback сверх срока / всего |
|---:|---:|:---:|---:|---:|---:|---:|
| 1.0 | 32 | нет | 2.596 / 6.722 / 201.057 | 23.140 / 215.638 | 0.222 | 0 / 2753 |
| 1.0 | 32 | да | 3.431 / 5.766 / 6.064 | 23.069 / 26.387 | 0.051 | 0 / 2747 |
| 1.0 | 64 | нет | 3.404 / 16.359 / 33.407 | 31.986 / 40.073 | 0.063 | 0 / 1381 |
| 1.0 | 64 | да | 4.287 / 15.589 / 149.815 | 27.059 / 165.754 | 0.253 | 0 / 1386 |
| 1.0 | 128 | нет | 2.772 / 3.952 / 7.903 | 22.311 / 23.997 | 0.039 | 0 / 693 |
| 1.0 | 128 | да | 3.052 / 13.024 / 34.778 | 27.064 / 42.928 | 0.063 | 0 / 696 |
| 1.0 | 512 | нет | 4.254 / 4.926 / 8.831 | 22.416 / 26.400 | 0.063 | 0 / 174 |
| 1.0 | 512 | да | 3.183 / 8.422 / 22.106 | 22.775 / 41.348 | 0.218 | 0 / 175 |
| 2.0 | 32 | нет | 5.408 / 9.130 / 15.019 | 22.398 / 41.880 | 0.043 | 0 / 2754 |
| 2.0 | 32 | да | 6.796 / 17.984 / 35.963 | 26.626 / 50.875 | 0.269 | 0 / 2761 |
| 2.0 | 64 | нет | 5.188 / 7.882 / 12.569 | 20.055 / 27.710 | 0.059 | 0 / 1383 |
| 2.0 | 64 | да | 5.817 / 7.646 / 11.040 | 21.057 / 28.614 | 0.037 | 0 / 1381 |
| 2.0 | 128 | нет | 5.348 / 7.923 / 13.581 | 20.869 / 34.887 | 0.057 | 0 / 693 |
| 2.0 | 128 | да | 6.847 / 16.352 / 51.183 | 27.668 / 69.880 | 0.189 | 0 / 693 |
| 2.0 | 512 | нет | 4.925 / 5.798 / 12.299 | 19.461 / 24.668 | 0.065 | 0 / 174 |
| 2.0 | 512 | да | 6.185 / 11.371 / 24.555 | 24.541 / 37.336 | 0.061 | 0 / 174 |

Источники: [финальный DPR 1](final-policy-dpr1.txt), [финальный DPR 2](final-policy-dpr2.txt). Скриншоты: [DPR 1](final-policy-dpr1.png), [DPR 2](final-policy-dpr2.png). Попиксельная проверка переносимой подложки прошла при обоих DPR, включая разворот прокрутки, дробные координаты и скруглённые края.

После добавления счётчика page-ins запущен зарегистрированный [audio_timeline_scroll_test](audio-scroll-final-ctest.txt). Он прошёл с исходным порогом CTest 33,4 мс; фактический p95 всех восьми DPR 2 сценариев оказался ниже 16,67 мс. Для 32 + recording: paint p50 6,209 мс, p95 8,921 мс, max 14,114 мс. В этом повторе один из 2756 callbacks сценария 32 + recording занял 0,740 мс при сроке 0,667 мс. Это не аппаратный xrun, но заявлять отсутствие всех превышений после всех повторов нельзя. Порог теста по результатам не ослаблялся.

JSON scopes содержат p50/p95/p99/max и input-to-paint: [финальный DPR 1](final-policy-dpr1.json), [финальный DPR 2](final-policy-dpr2.json), [последний CTest](audio-scroll-final-ctest.json). Эти агрегаты включают подготовку сцен и проверочные grab; их нельзя выдавать за статистику только непрерывного жеста. Paint и interval не измеряют фактическое представление кадра дисплеем.

## Дополнительная причина: синхронные ожидания raster-пула Qt

[Профиль стеков собственного тестового процесса](qt-raster-stacks.txt) показывает QPainter → blend_* → QLatch::waitInternal и пробуждения QThreadPool при отрисовке waveform, gain handle и playhead. Значит, GUI зависит от завершения вспомогательных paint-задач. Смысл QT_NO_GUI_THREADPOOL проверен в [официальном исходнике Qt 6.11.2](https://github.com/qt/qtbase/blob/v6.11.2/src/gui/kernel/qguiapplication.cpp#L4613): наличие переменной отключает отдельный GUI-пул.

В последовательном сравнении median paint снизился примерно на 15–25% в большинстве сценариев. Например, 32 + recording: 6,248 → 4,994 мс, 64 + recording: 5,932 → 5,023 мс. Поэтому main на macOS устанавливает QT_NO_GUI_THREADPOOL до QApplication. Для диагностики прежнего поведения можно задать VLT_QT_GUI_THREADPOOL=1, оставив QT_NO_GUI_THREADPOOL незаданной. Аудиопул этим не меняется.

Все сравнения сохранены: [пул включён](qt-pool-enabled-repeat.txt), [отключён](qt-pool-disabled-repeat.txt), [первый опыт отключения](qt-pool-disabled-first.txt). Отключение пула не устранило все редкие задержки; в первом опыте отключения строгий тест тоже не прошёл. Профилирование заметно остановило процесс и нарушило сроки — [его неуспешный запуск](qt-raster-sampled-run.txt) используется только для стеков, не для оценки FPS и callback deadlines.

## Оставшиеся ожидания

До изменения политики Qt встречался кадр 90,316 мс при 7,065 мс CPU GUI ([лог](diagnostic-final.txt)). После изменения в последнем CTest зарегистрирован кадр 55,432 мс при 11,319 мс CPU GUI и нулевом приросте process_pageins. Массовая растеризация waveform устранена, Qt latch-путь отключён, однако конкретная причина остальных ожиданий или вытеснения GUI не установлена. Нулевые page-ins не доказывают отсутствие любой задержки памяти. Гарантированные 60 FPS при произвольной внешней нагрузке не заявляются.

VLT_UI_PROFILE включает scopes; VLT_UI_SLOW_LOG дополнительно печатает медленные стадии. В обычном запуске логирование выключено. --audio-scroll-check печатает SLOW_SCROLL для paint >30 мс: wall time, CPU GUI, page-ins всего процесса и активность raster cache. Метрики, недоступные на платформе, обозначаются NaN. Page-ins относятся ко всему процессу, не только GUI-потоку.

## Аудиопланировщик

Paired audio_engine_bench: 8/64 дорожек со встроенным EQ, 32/512 frames, 400 paced blocks, RT helpers, fusion off/on. Все эти отдельные серии завершились без превышения срока и без PCM cache miss. Начальный wake теперь учитывает реально готовые roots и fused tasks. На широких графах совокупный CPU менялся в обе стороны; устойчивое ускорение всего аудиодвижка не установлено. Realtime-приоритеты и прежний бюджет ожидания сохранены.

| Дорожки / буфер | Исходный лог | Новый лог |
|---|---|---|
| 8 / 32 | [before](audio-before-8-32.txt) | [after](audio-after-8-32.txt) |
| 8 / 512 | [before](audio-before-8-512.txt) | [after](audio-after-8-512.txt) |
| 64 / 32 | [before](audio-before-64-32.txt) | [after](audio-after-64-32.txt) |
| 64 / 512 | [before](audio-before-64-512.txt) | [after](audio-after-64-512.txt) |

## Исторический основной прогон

До дополнительного изменения Qt-политики все 16 строгих сценариев прошли, 20021 callbacks без превышений; 32 + recording, DPR 2, p95 paint 7,594 мс. Сохранены [DPR 1](audio-scroll-dpr1.txt), [DPR 2](audio-scroll-dpr2.txt), JSON и скриншоты с теми же именами. Эти хорошие числа не заменяют последующие повторные измерения с выбросами.

## Воспроизведение

Из корня репозитория в настроенном build с тестами:

```sh
cmake --build build -j4 --target daw waveform_paint_test recording_preview_test ui_frame_clock_test engine_graph_test controller_test audio_engine_bench
ctest --test-dir build --output-on-failure -R '^(controller_test|recording_preview_test|waveform_paint_test|ui_frame_clock_test|engine_graph_test|platform_test|recording_commit_planner_test|audio_realtime_test|miditools_test|audio_timeline_scroll_test)$'
QT_QPA_PLATFORM=offscreen build/bin/VLTONE --uiperfcheck
QT_QPA_PLATFORM=offscreen build/bin/VLTONE --patterncheck
QT_QPA_PLATFORM=offscreen build/bin/VLTONE --editorcheck
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1 VLT_AUDIO_SCROLL_P95_MS=16.67 VLT_UI_PROFILE=/tmp/vlt-audio-scroll-dpr1.json build/bin/VLTONE --audio-scroll-check
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 VLT_AUDIO_SCROLL_P95_MS=16.67 VLT_UI_PROFILE=/tmp/vlt-audio-scroll-dpr2.json VLT_UI_SLOW_LOG=1 build/bin/VLTONE --audio-scroll-check
build/bin/audio_engine_bench --tracks 8 --frames 32 --blocks 400 --rounds 1 --clips --heavy --paced --realtime-workers
```

Для остальных audio benchmark комбинаций поменять tracks на 64 и/или frames на 512. CTest использует порог p95 33,4 мс, строгий ручной прогон — 16,67 мс. Ни один из этих порогов не означает отсутствия длинных кадров: max, SLOW_SCROLL и сроки callback сохраняются отдельно.
