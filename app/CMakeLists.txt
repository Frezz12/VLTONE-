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
    Widgets Network WebEngineWidgets SerialPort LinguistTools)
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

# Headers are listed too so AUTOMOC always sees the Q_OBJECT classes that live
# in them, even for files added after the last configure.
file(GLOB DAW_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

qt_add_executable(daw ${DAW_APP_SOURCES})

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
daw_add_windows_metadata(daw "VLT Studio Pro.exe" "VLTStudioPro"
                         "VLT Studio Pro digital audio workstation")

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
                                  Qt6::WebEngineWidgets Qt6::SerialPort
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
elseif(WIN32)
    target_link_libraries(daw PRIVATE Advapi32 Psapi)
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/reporter"
                 "${CMAKE_BINARY_DIR}/reporter")

if(WIN32)
    set_target_properties(daw PROPERTIES OUTPUT_NAME "VLT Studio Pro")
endif()
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

        # A shipped app is "VLT Studio Pro", not "daw": the bundle's name is what the
        # Finder, the Dock and the menu bar all read.
        set(DAW_BUNDLE_NAME "VLT Studio Pro")
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
