# Проверяет, что в исходниках движка нет ни одного включения Qt.
#
# Это принцип №1 из ARCHITECTURE.md. Он держится только на автоматической
# проверке: стоит один раз «временно» подключить QString в движке, и через
# полгода движок нельзя будет ни протестировать оффлайн, ни собрать без Qt.
#
# Запуск: cmake -DENGINE_DIR=<путь> -P CheckNoQt.cmake

if(NOT DEFINED ENGINE_DIR)
    message(FATAL_ERROR "Не задан ENGINE_DIR")
endif()

file(GLOB_RECURSE sources
     "${ENGINE_DIR}/include/*.h"
     "${ENGINE_DIR}/include/*.hpp"
     "${ENGINE_DIR}/src/*.h"
     "${ENGINE_DIR}/src/*.hpp"
     "${ENGINE_DIR}/src/*.cpp")

set(violations "")

foreach(file ${sources})
    file(STRINGS "${file}" matches REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]Q|^[ \t]*#[ \t]*include[ \t]*[<\"]qt")
    if(matches)
        file(RELATIVE_PATH rel "${ENGINE_DIR}" "${file}")
        foreach(m ${matches})
            string(APPEND violations "  ${rel}: ${m}\n")
        endforeach()
    endif()
endforeach()

if(NOT violations STREQUAL "")
    message(FATAL_ERROR
        "Движок не должен зависеть от Qt. Найдены включения:\n${violations}"
        "\nСм. ARCHITECTURE.md, принцип №1.")
endif()

list(LENGTH sources count)
message(STATUS "Проверено файлов движка: ${count}. Зависимостей от Qt нет.")
