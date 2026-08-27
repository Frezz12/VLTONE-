# ASIO на Windows

Windows-сборка использует `portaudio[asio]` из `vcpkg.json`. Отдельного флага
`DAW_ENABLE_ASIO` и ручной копии SDK в репозитории нет: достаточно штатного
пресета сборки.

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg
```

Vcpkg получает ASIO SDK как зависимость PortAudio. Перед распространением
бинарной сборки необходимо принять и выполнить актуальные лицензионные и
trademark-требования Steinberg; SDK не хранится в этом репозитории.

В приложении список `Driver Type` отделяет ASIO от WASAPI/MME. Для выбранного
ASIO-драйвера доступны именованные физические входы, выбор стереопары master
output, поддерживаемые размеры буфера и родная панель `Hardware Setup`.
Полноценные многоканальные аппаратные output-шины пока не реализованы.

Подробности сборки и список аппаратных проверок находятся в
[`BUILD.md`](../BUILD.md#asio-on-windows).
