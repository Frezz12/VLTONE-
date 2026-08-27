# Real AEffect fixtures: a VST2 shell with an effect and an instrument, plus a
# VST1-style module that exports only the legacy platform entry point.

add_library(daw_test_vst_shell MODULE TestVstShell.cpp)
add_library(daw_test_vst1 MODULE TestVst1.cpp)
foreach(target daw_test_vst_shell daw_test_vst1)
    target_link_libraries(${target} PRIVATE daw_fst_headers)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins_test)
endforeach()
set_target_properties(daw_test_vst_shell PROPERTIES OUTPUT_NAME "DawTestVstShell")
set_target_properties(daw_test_vst1 PROPERTIES OUTPUT_NAME "DawTestVst1")

if(APPLE)
    foreach(target daw_test_vst_shell daw_test_vst1)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "vst"
            MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)
    endforeach()
    set_target_properties(daw_test_vst_shell PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "DawTestVstShell"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.vlt.test.vst-shell")
    set_target_properties(daw_test_vst1 PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "DawTestVst1"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.vlt.test.vst1")
else()
    set_target_properties(daw_test_vst_shell daw_test_vst1 PROPERTIES SUFFIX ".dll")
endif()

if(MSVC)
    # MSVC reserves the source-level name `main`; export the legacy spelling as
    # an alias without defining a CRT entry point.
    target_link_options(daw_test_vst1 PRIVATE "/EXPORT:main=LegacyVstMain")
endif()

if(APPLE)
    set(DAW_TEST_VST_SHELL_PATH
        "${CMAKE_BINARY_DIR}/plugins_test/DawTestVstShell.vst")
    set(DAW_TEST_VST1_PATH
        "${CMAKE_BINARY_DIR}/plugins_test/DawTestVst1.vst")
else()
    set(DAW_TEST_VST_SHELL_PATH "$<TARGET_FILE:daw_test_vst_shell>")
    set(DAW_TEST_VST1_PATH "$<TARGET_FILE:daw_test_vst1>")
endif()
set(DAW_TEST_VST_SHELL_PATH "${DAW_TEST_VST_SHELL_PATH}" PARENT_SCOPE)
set(DAW_TEST_VST1_PATH "${DAW_TEST_VST1_PATH}" PARENT_SCOPE)
