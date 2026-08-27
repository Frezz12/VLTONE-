# daw_scan — the out-of-process plugin scanner.
#
# A separate executable on purpose: it loads third-party code, so it is the
# process that is allowed to die. See scan_main.cpp for the reasoning.
add_executable(daw_scan scan_main.cpp)
daw_add_windows_metadata(daw_scan "daw_scan.exe" "daw_scan"
                         "VLT Studio Pro plugin scanner")

target_link_libraries(daw_scan PRIVATE daw_pluginhost daw_json)

if(MSVC)
    target_compile_options(daw_scan PRIVATE /W4)
else()
    target_compile_options(daw_scan PRIVATE -Wall -Wextra)
endif()

# The DAW has to be able to find this at runtime. Installed alongside the app so
# a packaged build can spawn it from its own directory.
# A packaged macOS build installs this inside the bundle instead (see
# app/CMakeLists.txt) — next to the executable, which is where it is looked for.
if(NOT (DAW_PACKAGE AND APPLE))
    install(TARGETS daw_scan RUNTIME DESTINATION bin)
endif()
