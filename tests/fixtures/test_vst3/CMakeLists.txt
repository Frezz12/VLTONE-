# A real VST3 plugin, used as the fixture for the host tests.
#
# On macOS a .vst3 is a bundle directory with the binary under Contents/MacOS,
# and the host's loader depends on that shape — building a bare dylib would
# make the tests pass against something no real plugin looks like.

add_library(daw_test_vst3 MODULE TestVst3Plugin.cpp)

target_link_libraries(daw_test_vst3 PRIVATE daw_vst3_pluginterfaces)
target_compile_features(daw_test_vst3 PRIVATE cxx_std_23)

set_target_properties(daw_test_vst3 PROPERTIES
    OUTPUT_NAME "DawTestGainVst3"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins_test)

if(APPLE)
    set_target_properties(daw_test_vst3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "vst3"
        MACOSX_BUNDLE_BUNDLE_NAME "DawTestGainVst3"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.daw.test.gain.vst3"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)
    target_link_libraries(daw_test_vst3 PRIVATE "-framework CoreFoundation")
else()
    set_target_properties(daw_test_vst3 PROPERTIES SUFFIX ".vst3")
endif()

if(APPLE)
    set(DAW_TEST_VST3_PATH "${CMAKE_BINARY_DIR}/plugins_test/DawTestGainVst3.vst3")
else()
    set(DAW_TEST_VST3_PATH "$<TARGET_FILE:daw_test_vst3>")
endif()
set(DAW_TEST_VST3_PATH "${DAW_TEST_VST3_PATH}" PARENT_SCOPE)
