# Run on the staged copies, before the enclosing app is signed and packaged.
include(BundleUtilities)

# The tools are shipped as a plain directory, not a .app bundle. BundleUtilities'
# macOS default would place the dylibs in "@executable_path/../Frameworks",
# which lands outside the directory fixup_bundle treats as the bundle and
# aborts. Keep every dependency next to the tools instead.
function(gp_item_default_embedded_path_override item path_var)
    set(${path_var} "@executable_path" PARENT_SCOPE)
endfunction()

fixup_bundle("${TOOLS_DIRECTORY}/daw_creator_compiler"
  "${TOOLS_DIRECTORY}/clang;${TOOLS_DIRECTORY}/wasm-ld;${TOOLS_DIRECTORY}/wamrc"
  "${LIBRARY_DIRECTORIES}")

# install_name_tool invalidates the ad-hoc signature of everything it rewrites,
# and the kernel refuses to map such pages: the process dies with SIGKILL before
# it can report anything. Re-sign the staged binaries here; the enclosing app is
# signed separately by the packaging script.
file(GLOB _staged_entries LIST_DIRECTORIES false "${TOOLS_DIRECTORY}/*")
foreach(_staged_entry IN LISTS _staged_entries)
  execute_process(COMMAND /usr/bin/file -b "${_staged_entry}"
    OUTPUT_VARIABLE _staged_kind OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _staged_kind_result)
  if(_staged_kind_result EQUAL 0 AND _staged_kind MATCHES "Mach-O")
    execute_process(COMMAND /usr/bin/codesign --force --sign - "${_staged_entry}"
      RESULT_VARIABLE _staged_sign_result)
    if(NOT _staged_sign_result EQUAL 0)
      message(FATAL_ERROR "cannot re-sign ${_staged_entry}")
    endif()
  endif()
endforeach()
