# Network-capable diagnostics courier. The watchdog remains deliberately
# network-free; this process receives a telemetry-only token over local IPC.
qt_add_executable(daw_reporter reporter_main.cpp)
daw_add_windows_metadata(daw_reporter "daw_reporter.exe" "daw_reporter"
                         "VLT Studio Pro diagnostics reporter")
target_link_libraries(daw_reporter PRIVATE Qt6::Core Qt6::Network)
if(MSVC)
    target_compile_options(daw_reporter PRIVATE /W4)
else()
    target_compile_options(daw_reporter PRIVATE -Wall -Wextra)
endif()
if(NOT (DAW_PACKAGE AND APPLE))
    install(TARGETS daw_reporter RUNTIME DESTINATION bin)
endif()
