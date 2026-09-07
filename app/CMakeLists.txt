# daw — the Qt Widgets front-end.
# Network serves the AI assistant; WebEngineWidgets provides the isolated
# Chromium-based browser and its explicit download API. WebSockets remains an
# opt-in dependency with the collaboration feature flag.
set(DAW_QT_VERSION "6.8.3" CACHE STRING "Supported Qt release line")
if(WIN32)
    set(DAW_EXACT_QT_DEFAULT ON)
else()
    set(DAW_EXACT_QT_DEFAULT OFF)
endif()
option(DAW_REQUIRE_EXACT_QT_VERSION
       "Reject Qt versions other than DAW_QT_VERSION" ${DAW_EXACT_QT_DEFAULT})
set(DAW_QT_COMPONENTS
    Widgets Network Multimedia WebChannel WebEngineWidgets SerialPort Svg
    LinguistTools)
if(DAW_ENABLE_COLLABORATION)
    list(APPEND DAW_QT_COMPONENTS WebSockets)
endif()
if(DAW_REQUIRE_EXACT_QT_VERSION)
    find_package(Qt6 ${DAW_QT_VERSION} EXACT REQUIRED COMPONENTS
        ${DAW_QT_COMPONENTS})
else()
    find_package(Qt6 ${DAW_QT_VERSION} REQUIRED COMPONENTS
        ${DAW_QT_COMPONENTS})
endif()
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(RtMidi CONFIG REQUIRED)
qt_standard_project_setup()   # enables AUTOMOC / AUTOUIC / AUTORCC

if(DAW_BUILD_TESTS)
    qt_add_executable(browser_navigation_test
        ../tests/browser_navigation_test.cpp
        WebBrowserPanel.cpp WebBrowserPanel.hpp BrowserBackgroundDialog.cpp BrowserBackgroundDialog.hpp
        WebVideoSource.cpp WebVideoSource.hpp WebPrefs.cpp TimelineBackgroundPrefs.cpp ThemeMediaBackground.cpp ThemeMediaBackground.hpp
        Controls.cpp Controls.hpp Icons.cpp GlassPanel.cpp GlassPanel.hpp Theme.cpp Typography.cpp
        UiFrameClock.cpp UiFrameClock.hpp UiPerformance.cpp UiPerformance.hpp)
    target_include_directories(browser_navigation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(browser_navigation_test PRIVATE
        daw_controller Qt6::Widgets Qt6::WebEngineWidgets Qt6::Multimedia Qt6::Network Qt6::Svg)
    add_test(NAME browser_navigation_test COMMAND browser_navigation_test)
    add_test(NAME browser_proxy_test COMMAND browser_navigation_test --proxy)
    set_tests_properties(browser_navigation_test browser_proxy_test PROPERTIES TIMEOUT 120
        ENVIRONMENT "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --disable-features=WebGPU")
    if(WIN32)
        set_tests_properties(browser_navigation_test browser_proxy_test PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(telemetry_snapshot_test ../tests/telemetry_snapshot_test.cpp)
    target_include_directories(telemetry_snapshot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(telemetry_snapshot_test PRIVATE daw_controller Qt6::Core)
    add_test(NAME telemetry_snapshot_test COMMAND telemetry_snapshot_test)
    set_tests_properties(telemetry_snapshot_test PROPERTIES TIMEOUT 15)
    if(WIN32)
        set_tests_properties(telemetry_snapshot_test PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(web_video_background_test
        ../tests/web_video_background_test.cpp
        WebVideoBackground.cpp WebVideoBackground.hpp WebVideoSource.cpp WebVideoSource.hpp
        WebPrefs.cpp TimelineBackgroundPrefs.cpp ThemeMediaBackground.cpp ThemeMediaBackground.hpp)
    target_include_directories(web_video_background_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(web_video_background_test PRIVATE
        DAW_TEST_WEB_VIDEO="${CMAKE_SOURCE_DIR}/tests/fixtures/video/background.webm")
    target_link_libraries(web_video_background_test PRIVATE
        Qt6::Widgets Qt6::WebEngineWidgets Qt6::Multimedia Qt6::Network)
    add_test(NAME web_video_background_test COMMAND web_video_background_test)
    set_tests_properties(web_video_background_test PROPERTIES TIMEOUT 120
        ENVIRONMENT "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --disable-webgpu")
    if(WIN32)
        set_tests_properties(web_video_background_test PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(account_restore_test
        ../tests/account_restore_test.cpp AccountService.cpp AccountService.hpp)
    target_include_directories(account_restore_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(account_restore_test PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)
    add_test(NAME account_restore_test COMMAND account_restore_test)
    set_tests_properties(account_restore_test PROPERTIES TIMEOUT 30)
    if(WIN32)
        set_tests_properties(account_restore_test PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    qt_add_executable(ui_frame_clock_test
        ../tests/ui_frame_clock_test.cpp UiFrameClock.cpp UiFrameClock.hpp
        UiPerformance.cpp UiPerformance.hpp)
    target_include_directories(ui_frame_clock_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ui_frame_clock_test PRIVATE Qt6::Widgets)
    add_test(NAME ui_frame_clock_test COMMAND ui_frame_clock_test)
    set_tests_properties(ui_frame_clock_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    qt_add_executable(render_ui_test
        ../tests/render_ui_test.cpp
        PreviewLoader.cpp PreviewLoader.hpp
        ExportDialog.cpp ExportDialog.hpp ExportPrefs.cpp
        SelectionModel.cpp SelectionModel.hpp Theme.cpp Typography.cpp GlassPanel.cpp GlassPanel.hpp)
    target_include_directories(render_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(render_ui_test PRIVATE daw_controller Qt6::Widgets Qt6::WebEngineCore)
    add_test(NAME render_ui_test COMMAND render_ui_test)
    set_tests_properties(render_ui_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

# Headers are listed too so AUTOMOC always sees the Q_OBJECT classes that live
# in them, even for files added after the last configure.
file(GLOB DAW_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

qt_add_executable(daw ${DAW_APP_SOURCES})

# Ship the complete family inside the executable on every platform. No font
# installation, source checkout or system Inter installation is required.
file(GLOB VLTONE_UI_FONTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/fonts/Inter-*.ttf")
qt_add_resources(daw "vltone_fonts"
    PREFIX "/fonts"
    BASE "${CMAKE_SOURCE_DIR}/fonts"
    FILES ${VLTONE_UI_FONTS} "${CMAKE_SOURCE_DIR}/fonts/OFL.txt")

if(MSVC)
    # MainWindow hosts the feature-gated desktop composition root and its Qt
    # metacode; the collaboration build can exceed COFF's default section cap.
    target_compile_options(daw PRIVATE /bigobj)
endif()

file(GLOB DAW_TOOLBAR_ICONS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/icons/*.svg")
qt_add_resources(daw "daw_toolbar_icons"
    PREFIX "/icons"
    BASE "${CMAKE_SOURCE_DIR}/icons"
    FILES ${DAW_TOOLBAR_ICONS})

# lupdate must see only the active desktop sources. The executable links the
# engine and plugin SDK targets, some of which expose documentation files as
# interface sources; feeding those through Linguist produces parser warnings.
add_custom_target(daw_translation_sources SOURCES ${DAW_APP_SOURCES})

set(VLT_TRANSLATION_TS
    "${CMAKE_CURRENT_SOURCE_DIR}/translations/vlt_ru.ts")
qt_add_translations(daw
    SOURCE_TARGETS daw_translation_sources
    TS_FILES ${VLT_TRANSLATION_TS}
    LUPDATE_OPTIONS -no-obsolete -locations none -extensions cpp,h,hpp)

# The editable JSON template is generated at runtime from the same catalog
# translators maintain. Embedding the TS source prevents a second list of
# English strings from drifting away from lupdate's source of truth.
qt_add_resources(daw "vlt_translation_catalog"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/translations"
    FILES ${VLT_TRANSLATION_TS})
qt_add_resources(daw "vlt_startup_assets"
    PREFIX "/vlt"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon-1024.png")
if(APPLE)
    target_sources(daw PRIVATE PluginEditorWindowMac.mm)
    target_link_libraries(daw PRIVATE "-framework AppKit" "-framework CoreMIDI")
endif()
daw_add_windows_metadata(daw "VLTONE.exe" "VLTONE"
                         "VLTONE digital audio workstation")

target_compile_definitions(daw PRIVATE
    VLT_DEFAULT_API_ORIGIN=\"${VLT_DEFAULT_API_ORIGIN}\")
if(DAW_ENABLE_COLLABORATION)
    # The root dogfood/release-gate option owns enablement. All session UI and
    # service injection is compiled out unless that explicit gate is ON.
    target_compile_definitions(daw PRIVATE DAW_ENABLE_COLLABORATION=1)
    target_link_libraries(daw PRIVATE Qt6::WebSockets)
endif()

target_include_directories(daw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daw PRIVATE daw_controller Qt6::Widgets Qt6::Network
                                  Qt6::Multimedia Qt6::WebChannel
                                  Qt6::WebEngineWidgets
                                  Qt6::SerialPort Qt6::Svg
                                  OpenSSL::Crypto RtMidi::rtmidi)
if(DAW_BUILD_TESTS)
    add_test(NAME collaboration_client_selftest
             COMMAND $<TARGET_FILE:daw> --collaboration-selftest)
    set_tests_properties(collaboration_client_selftest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 30)
    if(WIN32)
        # CTest does not inherit the Qt SDK's bin directory on clean Windows
        # runners. Without this, the loader opens an invisible error dialog for
        # a missing Qt6Core.dll and the headless test appears to time out.
        set_tests_properties(collaboration_client_selftest PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endif()
if(APPLE)
    target_link_libraries(daw PRIVATE "-framework Security")
    if(DAW_BUILD_TESTS)
        add_executable(secure_storage_mac_test ../tests/secure_storage_mac_test.cpp)
        target_link_libraries(secure_storage_mac_test PRIVATE Qt6::Core
            "-framework Security" "-framework CoreFoundation")
        add_test(NAME secure_storage_mac_test COMMAND secure_storage_mac_test)
        set_tests_properties(secure_storage_mac_test PROPERTIES TIMEOUT 15)
    endif()
elseif(WIN32)
    target_link_libraries(daw PRIVATE Advapi32 Psapi)
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/reporter"
                 "${CMAKE_BINARY_DIR}/reporter")

set_target_properties(daw PROPERTIES OUTPUT_NAME "VLTONE")
# Dev builds stay a plain console executable (keeps --selftest/--screenshot and
# stderr logging simple). Packaging builds become a proper GUI app: a .app
# bundle on macOS and a windowed (no console) exe on Windows.
if(DAW_PACKAGE)
    set_target_properties(daw PROPERTIES
        MACOSX_BUNDLE ON
        WIN32_EXECUTABLE ON)

    if(APPLE)
        # The icon travels as a bundle resource; CMake copies anything with this
        # source property into Contents/Resources.
        set(DAW_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/daw.icns")
        target_sources(daw PRIVATE ${DAW_ICNS})
        set_source_files_properties(${DAW_ICNS} PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)

        # A shipped app is "VLTONE", not "daw": the bundle's name is what the
        # Finder, the Dock and the menu bar all read.
        set(DAW_BUNDLE_NAME "VLTONE")
        set_target_properties(daw PROPERTIES
            OUTPUT_NAME "${DAW_BUNDLE_NAME}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${DAW_BUNDLE_NAME}"
            MACOSX_BUNDLE_EXECUTABLE_NAME "${DAW_BUNDLE_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.vltstudio.pro"
            MACOSX_BUNDLE_ICON_FILE "daw.icns"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    endif()
endif()

# Keep third-party notices with distributed binaries as well as the source tree.
if(DAW_PACKAGE AND APPLE)
    set(DAW_STRETCH_LICENSE_DIR "${DAW_BUNDLE_NAME}.app/Contents/Resources/licenses/signalsmith")
    set(VLTONE_FONT_LICENSE_DIR "${DAW_BUNDLE_NAME}.app/Contents/Resources/licenses/inter")
else()
    set(DAW_STRETCH_LICENSE_DIR "bin/licenses/signalsmith")
    set(VLTONE_FONT_LICENSE_DIR "bin/licenses/inter")
endif()
install(FILES "${CMAKE_SOURCE_DIR}/third_party/signalsmith/LICENSE-stretch.txt"
        DESTINATION "${DAW_STRETCH_LICENSE_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/third_party/signalsmith/signalsmith-linear/LICENSE.txt"
        DESTINATION "${DAW_STRETCH_LICENSE_DIR}" RENAME "LICENSE-linear.txt")
install(FILES "${CMAKE_SOURCE_DIR}/fonts/OFL.txt"
        DESTINATION "${VLTONE_FONT_LICENSE_DIR}")

install(TARGETS daw
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin)

# Only when packaging: bundle the Qt runtime next to the app at install time.
# This expands to macdeployqt on macOS and windeployqt on Windows.
if(DAW_PACKAGE)
    if(APPLE)
        # The scanner and the watchdog are found *next to the executable*
        # (`applicationDirPath` / `executableDirectory`), so in a bundle they
        # belong in Contents/MacOS. Installed from here rather than from their
        # own directories so they land after the bundle has been laid down and
        # before macdeployqt walks it.
        install(TARGETS daw_scan daw_guard daw_reporter
                RUNTIME DESTINATION "${DAW_BUNDLE_NAME}.app/Contents/MacOS")
    endif()
    if(WIN32)
        # Qt 6.8's app helper emits TARGET_FILE without quotes, so an executable
        # name containing spaces is split into extra CMake arguments at install.
        qt_generate_deploy_script(
            TARGET daw
            OUTPUT_SCRIPT DAW_DEPLOY_SCRIPT
            CONTENT "
qt6_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:daw>\"
    GENERATE_QT_CONF
    DEPLOY_TOOL_OPTIONS --include-plugins qoffscreen
)")
    else()
        qt_generate_deploy_app_script(
            TARGET daw
            OUTPUT_SCRIPT DAW_DEPLOY_SCRIPT
            NO_UNSUPPORTED_PLATFORM_ERROR)
    endif()
    install(SCRIPT ${DAW_DEPLOY_SCRIPT})
endif()
