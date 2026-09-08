# Network-capable diagnostics courier. The watchdog remains deliberately
# network-free; this process receives a telemetry-only token over local IPC.
qt_add_executable(daw_reporter reporter_main.cpp)
daw_add_windows_metadata(daw_reporter "daw_reporter.exe" "daw_reporter"
                         "VLTONE diagnostics reporter")
target_link_libraries(daw_reporter PRIVATE Qt6::Core Qt6::Network)
if(DAW_BUILD_TESTS)
    add_executable(crash_reporter_test ../tests/crash_reporter_test.cpp)
    target_link_libraries(crash_reporter_test PRIVATE Qt6::Core Qt6::Network)
    target_compile_definitions(crash_reporter_test PRIVATE DAW_REPORTER_PATH="$<TARGET_FILE:daw_reporter>")
    add_dependencies(crash_reporter_test daw_reporter)
    add_test(NAME crash_reporter_test COMMAND crash_reporter_test)
    set_tests_properties(crash_reporter_test PROPERTIES TIMEOUT 60)
    if(WIN32)
        set_tests_properties(crash_reporter_test PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
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
