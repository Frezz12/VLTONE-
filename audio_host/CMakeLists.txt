add_executable(daw_audio main.cpp)
target_link_libraries(daw_audio PRIVATE daw_controller)
add_dependencies(daw_audio daw_worker)
daw_add_windows_metadata(daw_audio "daw_audio.exe" "daw_audio"
                         "VLTONE isolated audio engine")
if(NOT (DAW_PACKAGE AND APPLE))
    install(TARGETS daw_audio RUNTIME DESTINATION bin)
endif()
