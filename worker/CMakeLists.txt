add_executable(daw_worker main.cpp)
target_link_libraries(daw_worker PRIVATE daw_controller)
daw_add_windows_metadata(daw_worker "daw_worker.exe" "daw_worker"
                         "VLTONE isolated offline worker")
if(NOT (DAW_PACKAGE AND APPLE))
    install(TARGETS daw_worker RUNTIME DESTINATION bin)
endif()
