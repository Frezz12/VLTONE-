# Зависимости через FetchContent.
#
# Почему не vcpkg: на M0 внешняя зависимость ровно одна (RtAudio), а FetchContent
# работает одинаково на Windows и macOS без предварительной установки чего-либо.
# Переезд на vcpkg — когда появится libsndfile со своим набором кодеков (M1).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# Qt 6 — ищется в системе, не скачивается
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
# RtAudio — кроссплатформенный аудио I/O
#
# ВАЖНО про ASIO: RtAudio умеет ASIO, но SDK от Steinberg сюда не подключён
# намеренно. Его лицензия несовместима с распространением GPL-бинарников
# (по этой же причине Audacity не поставляет ASIO-сборки). На Windows идём
# через WASAPI; эксклюзивный режим даёт приемлемую латентность. Пользователь,
# которому нужен ASIO, может собрать сам — см. docs/BUILD_ASIO.md.
# ---------------------------------------------------------------------------
set(RTAUDIO_BUILD_TESTING     OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio_uninstall" CACHE STRING "" FORCE)

# Backend'ы включаются строго по платформе. Включённый CoreAudio на Windows
# заставляет RtAudio искать macOS-фреймворки и валит генерацию.
if(WIN32)
    set(RTAUDIO_API_WASAPI ON  CACHE BOOL "" FORCE)
    set(RTAUDIO_API_ASIO   OFF CACHE BOOL "" FORCE)  # см. комментарий про лицензию выше
    set(RTAUDIO_API_DS     OFF CACHE BOOL "" FORCE)  # DirectSound — легаси
    set(RTAUDIO_API_CORE   OFF CACHE BOOL "" FORCE)
elseif(APPLE)
    set(RTAUDIO_API_CORE   ON  CACHE BOOL "" FORCE)
    set(RTAUDIO_API_WASAPI OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_API_ASIO   OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_API_DS     OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG        6.0.1
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(rtaudio)

# ---------------------------------------------------------------------------
# libsndfile — чтение и запись аудиофайлов (WAV, AIFF, FLAC, Ogg, …)
#
# На M1 — только WAV/AIFF через FetchContent (без внешних кодеков).
# Когда понадобятся FLAC/Ogg/Opus — переезд на vcpkg.
# ---------------------------------------------------------------------------
set(BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_MPEG         OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libsndfile
    GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libsndfile)
set(CMAKE_POLICY_VERSION_MINIMUM)

# ---------------------------------------------------------------------------
# Catch2 — тесты
# ---------------------------------------------------------------------------
if(DAW_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(Catch2)
    # При подключении через FetchContent (в отличие от find_package) Catch2
    # не добавляет свой extras/ в CMAKE_MODULE_PATH сам, и include(Catch) падает.
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
