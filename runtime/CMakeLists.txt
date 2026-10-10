# Disposable background jobs share one process transport.
add_library(daw_process STATIC SharedProcess.cpp ProcessAudioResources.cpp)
target_include_directories(daw_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daw_process PUBLIC daw_core daw_engine)
if(MSVC)
    target_compile_options(daw_process PRIVATE /W4)
else()
    target_compile_options(daw_process PRIVATE -Wall -Wextra -Wpedantic)
endif()
