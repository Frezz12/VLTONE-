function(daw_add_windows_metadata target original_filename internal_name description)
    if(NOT WIN32)
        return()
    endif()

    set(DAW_RC_ORIGINAL_FILENAME "${original_filename}")
    set(DAW_RC_INTERNAL_NAME "${internal_name}")
    set(DAW_RC_FILE_DESCRIPTION "${description}")
    set(DAW_RC_ICON "${CMAKE_SOURCE_DIR}/app/resources/windows/daw.ico")

    set(DAW_RC_MANIFEST
        "${CMAKE_CURRENT_BINARY_DIR}/${target}.exe.manifest")
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/windows/VLTStudioPro.manifest.in"
        "${DAW_RC_MANIFEST}"
        @ONLY)

    set(DAW_RC_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}_windows.rc")
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/windows/WindowsResources.rc.in"
        "${DAW_RC_OUTPUT}"
        @ONLY)
    target_sources(${target} PRIVATE "${DAW_RC_OUTPUT}")
    if(MSVC)
        target_link_options(${target} PRIVATE /MANIFEST:NO)
    endif()
endfunction()
