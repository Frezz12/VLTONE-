Диагностические проверки для ревью 2026-09-05

Проверки демонстрируют поведение текущего кода и не являются исправлениями или новыми production-тестами. Не требуют аудиоустройства. Создают только собственные данные в `/tmp/vlt-review-20260905`. Замеры зависят от машины; controller_probe ограничен восемью stereo-файлами по 30 секунд. Эти команды рассчитаны на использованную macOS/Homebrew сборку.

Из корня репозитория сначала собрать библиотеки:

```sh
cmake --build build --target sampler_test engine_graph_test -j 4
```

Для gate и стоимости компиляции графа:

```sh
c++ -std=c++23 -O2 -I engine docs/reviews/2026-09-05/probes/gate_probe.cpp build/engine/libdaw_engine.a -o /tmp/vlt-gate-probe
/tmp/vlt-gate-probe
c++ -std=c++23 -O2 -I engine docs/reviews/2026-09-05/probes/graph_scaling.cpp build/engine/libdaw_engine.a -o /tmp/vlt-graph-probe
/tmp/vlt-graph-probe
```

`compile_probe.py` извлекает параметры линковки из существующей Ninja-сборки `sampler_test`. Чтобы не оставлять бинарники в исходниках, передать ему копию нужной пробы в `/tmp`:

```sh
cp docs/reviews/2026-09-05/probes/controller_probe.cpp /tmp/vlt-controller-probe.cpp
python3 docs/reviews/2026-09-05/probes/compile_probe.py /tmp/vlt-controller-probe.cpp
/tmp/vlt-controller-probe
cp docs/reviews/2026-09-05/probes/manifest_probe.cpp /tmp/vlt-manifest-probe.cpp
python3 docs/reviews/2026-09-05/probes/compile_probe.py /tmp/vlt-manifest-probe.cpp
/tmp/vlt-manifest-probe
```

`qt_probe.cpp` проверяет, что disabled MainWindow отключает дочерний диалог, но не свой таймер. Собрать с флагами `pkg-config --cflags --libs Qt6Widgets`, запустить с `QT_QPA_PLATFORM=offscreen`. Это минимальная репродукция иерархии виджетов, не запуск полного ExportDialog.

Зафиксированные результаты:

```text
outer gate, before inner: live calls=0
outer gate, after inner: live calls=1 (expected 0)
offline render status=1, unexpected live calls=2, offline calls=2
disabled main: dialog enabled=0, cancel enabled=0, timer ticks=1
rate=48000 first callback=0.35 ms total incl. restore=0.47 ms peak RSS=118.0 MiB status=1
rate=96000 first callback=959.95 ms total incl. restore=1009.35 ms peak RSS=316.0 MiB status=1
previous successful output exists=1 size=39212
after cancelling next export: cancelled=1 previous output exists=0
tracks=500 nodes=1501 compile=0.86 ms ancestors=0.3 MiB status=1
tracks=1000 nodes=3001 compile=1.86 ms ancestors=1.1 MiB status=1
tracks=2000 nodes=6001 compile=8.98 ms ancestors=4.3 MiB status=1
tracks=4000 nodes=12001 compile=41.30 ms ancestors=17.2 MiB status=1
project.vlt: save=1 load=0
Project.vlt: save=1 load=1
session.vlt: save=1 load=1
invalid format ESCAPED exception: [json.exception.type_error.302] type must be string, but is number
```

`peak RSS` — накопленный максимум памяти процесса, не точный размер живых объектов на первом callback. Время первой отмены включает подготовку и один обработанный блок; полное время также включает восстановление live-конфигурации.
