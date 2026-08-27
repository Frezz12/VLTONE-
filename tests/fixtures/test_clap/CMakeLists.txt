# A real CLAP plugin, used as the fixture for the host tests.
#
# On macOS a CLAP is a *bundle directory*, not a bare dylib, and the loader
# looks for the binary under Contents/MacOS. Building a plain MODULE here would
# still load through an explicit path but would not exercise the bundle handling
# the scanner depends on — so the tests would be passing on the wrong shape.

add_library(daw_test_clap MODULE TestClapPlugin.cpp)

target_link_libraries(daw_test_clap PRIVATE daw_clap_headers)
target_compile_features(daw_test_clap PRIVATE cxx_std_23)

set_target_properties(daw_test_clap PROPERTIES
    OUTPUT_NAME "DawTestGain"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins_test)

if(APPLE)
    set_target_properties(daw_test_clap PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        MACOSX_BUNDLE_BUNDLE_NAME "DawTestGain"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.daw.test.gain"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)
else()
    set_target_properties(daw_test_clap PROPERTIES SUFFIX ".clap")
endif()

# Where the tests should look. A generator expression, because the bundle's
# path is not something to guess at.
if(APPLE)
    set(DAW_TEST_CLAP_PATH "${CMAKE_BINARY_DIR}/plugins_test/DawTestGain.clap")
else()
    set(DAW_TEST_CLAP_PATH "$<TARGET_FILE:daw_test_clap>")
endif()
set(DAW_TEST_CLAP_PATH "${DAW_TEST_CLAP_PATH}" PARENT_SCOPE)
