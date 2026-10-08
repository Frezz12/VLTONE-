# External production plugins use the isolated host. Local adapters remain
# available to the native helper and SDK compatibility tests.
add_library(daw_pluginprocess STATIC
    PluginProcess.cpp PluginProcessProtocol.cpp PluginProcessServer.cpp SharedProcess.cpp
    ProcessPluginInstance.cpp HostedPluginFactory.cpp)
target_include_directories(daw_pluginprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daw_pluginprocess PUBLIC daw_pluginhost)
if(MSVC)
    target_compile_options(daw_pluginprocess PRIVATE /W4)
else()
    target_compile_options(daw_pluginprocess PRIVATE -Wall -Wextra -Wpedantic)
endif()
