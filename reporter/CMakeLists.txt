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
else()
    # Installed into Contents/MacOS beside the app (see app/CMakeLists.txt), so
    # it needs the bundle's own search path for the Qt frameworks macdeployqt
    # puts in Contents/Frameworks. macdeployqt rewrites the application binary
    # but leaves the helper executables alone, and without this the courier
    # dies at launch with "no LC_RPATH's found" -- taking crash diagnostics
    # with it, silently, on every machine but the build one.
    set_target_properties(daw_reporter PROPERTIES
        INSTALL_RPATH "@executable_path/../Frameworks")
endif()
