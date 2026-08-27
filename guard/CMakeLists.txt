# daw_guard — the crash watchdog.
#
# Deliberately tiny: it links nlohmann/json and nothing else. No Qt, no audio,
# no plugin code. A watchdog with dependencies is a watchdog that can crash, and
# then it is worse than none. See guard_main.cpp for what it can and cannot do.
add_executable(daw_guard guard_main.cpp)
daw_add_windows_metadata(daw_guard "daw_guard.exe" "daw_guard"
                         "VLT Studio Pro recovery watchdog")

target_link_libraries(daw_guard PRIVATE daw_json)
# Only for recovery/SessionFile.hpp, which is header-only precisely so this
# target does not have to link daw_controller to speak the protocol.
target_include_directories(daw_guard PRIVATE
    ${CMAKE_SOURCE_DIR}/controller
    ${CMAKE_SOURCE_DIR}/core)

if(MSVC)
    target_compile_options(daw_guard PRIVATE /W4)
else()
    target_compile_options(daw_guard PRIVATE -Wall -Wextra)
endif()

# The DAW spawns this at startup, from its own directory.
# A packaged macOS build installs this inside the bundle instead (see
# app/CMakeLists.txt) — next to the executable, which is where it is looked for.
if(NOT (DAW_PACKAGE AND APPLE))
    install(TARGETS daw_guard RUNTIME DESTINATION bin)
endif()
