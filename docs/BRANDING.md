# VLTONE

Название продукта — **VLTONE**. Версия задаётся в корневом `CMakeLists.txt`;
`VLTONE_NAME` и `VLTONE_VERSION` доступны приложению и хостам плагинов.
В заголовках окон используется `QApplication::applicationDisplayName()`.

| Было | Стало | Зачем |
| --- | --- | --- |
| VLT Studio Pro / VLT Studio / Studio Pro | VLTONE | Единое имя в интерфейсе, переводах, аккаунте и руководстве |
| VLT Studio Pro.app / VLT Studio Pro.exe | VLTONE.app / VLTONE.exe | Новое имя в Finder, установщиках и ярлыках |
| build/bin/daw | build/bin/VLTONE | Имя локального исполняемого файла соответствует продукту |

Имена CMake-целей `daw`, `daw_scan`, `daw_guard` и `daw_reporter` — внутренние
идентификаторы сборки. Команда сборки приложения остаётся
`cmake --build build --target daw`.

Для совместимости сохраняются Qt applicationName/organizationName,
каталоги настроек, кеша плагинов и пресетов, идентификаторы Keychain/Credential
Manager, bundle ID, Windows AppId/ProgID, схемы документов и протоколов.
Это позволяет обновить программу без сброса настроек, входа и ассоциаций `.vlt`.
Существующая папка сгенерированного аудио используется дальше; новая установка
по умолчанию создаёт `VLTONE Generated`.

Адреса серверов и имена баз данных не зависят от названия продукта: действующий
API продолжает использовать настроенный адрес `vltstudio.ru`.
