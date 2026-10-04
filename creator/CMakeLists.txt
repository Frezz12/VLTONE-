# Creator's compiler is a disposable process. LLVM/Binaryen never enter the DAW.
include(FetchContent)
if(WIN32)
  set(_creator_llvm "clang+llvm-20.1.8-x86_64-pc-windows-msvc")
else()
  set(_creator_llvm "llvm-20.1.8")
endif()
set(CREATOR_LLVM_ROOT "${CMAKE_SOURCE_DIR}/.cache/creator-tools/${_creator_llvm}" CACHE PATH "Creator LLVM development SDK")
set(CREATOR_SYSROOT "${CMAKE_SOURCE_DIR}/.cache/creator-tools/wasi-sysroot-27.0" CACHE PATH "Creator WASI C++ sysroot")
set(CREATOR_WAMRC "${CMAKE_SOURCE_DIR}/.cache/creator-tools/wamrc/wamrc${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "Pinned WAMR AOT compiler")
set(LLVM_DIR "${CREATOR_LLVM_ROOT}/lib/cmake/llvm")
set(Clang_DIR "${CREATOR_LLVM_ROOT}/lib/cmake/clang")
find_package(LLVM 20.1.8 EXACT CONFIG REQUIRED)
find_package(Clang CONFIG REQUIRED)
if(MSVC AND TARGET LLVMDebugInfoPDB)
  # The official LLVM SDK records the build machine's absolute DIA path.
  find_library(CREATOR_DIA_GUIDS diaguids HINTS "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64" REQUIRED)
  get_target_property(_pdb_links LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  string(REGEX REPLACE "[^;]*diaguids\\.lib" "${CREATOR_DIA_GUIDS}" _pdb_links "${_pdb_links}")
  set_property(TARGET LLVMDebugInfoPDB PROPERTY INTERFACE_LINK_LIBRARIES "${_pdb_links}")
endif()
find_package(OpenSSL REQUIRED)

FetchContent_Declare(creator_wamr
  URL https://github.com/wasm-micro-runtime/wasm-micro-runtime/archive/25bd7eb63e828e4bd242cc9b38d260b4b31c6605.tar.gz
  URL_HASH SHA256=4ea552d9d979f3c3d5f9e298a1594150820d2b76e8328ad68779578edea7a873
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(creator_wamr)
if(NOT creator_wamr_POPULATED)
  FetchContent_Populate(creator_wamr)
endif()
if(WIN32)
  set(WAMR_BUILD_PLATFORM windows)
elseif(APPLE)
  set(WAMR_BUILD_PLATFORM darwin)
else()
  set(WAMR_BUILD_PLATFORM linux)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
  set(WAMR_BUILD_TARGET AARCH64)
else()
  set(WAMR_BUILD_TARGET X86_64)
endif()
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_INTERP 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIBC_UVWASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_THREAD_MGR 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_SIMD 1)
set(WAMR_BUILD_DUMP_CALL_STACK 0)
set(WAMR_BUILD_PERF_PROFILING 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
set(WAMR_DISABLE_STACK_HW_BOUND_CHECK 1)
include("${creator_wamr_SOURCE_DIR}/build-scripts/runtime_lib.cmake")
add_library(daw_creator_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
set_property(TARGET daw_creator_wamr PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
target_include_directories(daw_creator_wamr PUBLIC "${creator_wamr_SOURCE_DIR}/core/iwasm/include")
target_compile_definitions(daw_creator_wamr PUBLIC WASM_RUNTIME_API_EXTERN=)
if(MSVC)
  target_compile_options(daw_creator_wamr PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/w>")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(BUILD_LLVM_DWARF OFF CACHE BOOL "" FORCE)
FetchContent_Declare(creator_binaryen
  URL https://github.com/WebAssembly/binaryen/archive/6a6e08057c07ea4792a2ddcacfffc4d49ceea7fc.tar.gz
  URL_HASH SHA256=48a6cda58c9a727014e3d7581190a7c345095998dbc93d77be01efa1a74f51b5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(creator_binaryen)
set_property(TARGET binaryen PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
target_include_directories(binaryen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
if(MSVC)
  set_property(TARGET binaryen PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_compile_options(binaryen PRIVATE /GL-)
endif()

set(CREATOR_MODEL_SOURCES
  ../plugins/Creator/FunctionDefinition.cpp ../plugins/Creator/CodeUtilities.cpp
  ../plugins/Creator/CompilerProcess.cpp)
add_library(daw_creator_model STATIC ${CREATOR_MODEL_SOURCES})
target_include_directories(daw_creator_model PUBLIC ../plugins)
target_link_libraries(daw_creator_model PUBLIC daw_json OpenSSL::Crypto)
add_library(daw_creator_compiler_model STATIC ${CREATOR_MODEL_SOURCES})
target_include_directories(daw_creator_compiler_model PUBLIC ../plugins)
target_link_libraries(daw_creator_compiler_model PUBLIC daw_json OpenSSL::Crypto)
if(MSVC)
  set_property(TARGET daw_creator_model daw_creator_wamr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  set_property(TARGET daw_creator_compiler_model PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

add_executable(daw_creator_compiler CompilerMain.cpp FunctionAnalysis.cpp CodeGeneration.cpp Instrumentation.cpp)
target_include_directories(daw_creator_compiler SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS} "${creator_binaryen_SOURCE_DIR}/src")
target_link_libraries(daw_creator_compiler PRIVATE daw_creator_compiler_model clangTooling clangFrontend clangAST clangLex clangBasic clangCodeGen LLVMWebAssemblyCodeGen LLVMWebAssemblyAsmParser binaryen)
set_property(TARGET daw_creator_compiler PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
if(MSVC)
  target_compile_options(daw_creator_compiler PRIVATE /bigobj /EHsc)
endif()
set(CREATOR_TOOLS_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/CreatorTools")
if(WIN32)
  find_file(CREATOR_CRYPTO_RUNTIME NAMES libcrypto-3-x64.dll
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin" REQUIRED)
else()
  set(CREATOR_CRYPTO_RUNTIME "$<TARGET_FILE:OpenSSL::Crypto>")
endif()
set(_creator_runtime_commands)
if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  include(InstallRequiredSystemLibraries)
  foreach(_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    list(APPEND _creator_runtime_commands
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_runtime}" "${CREATOR_TOOLS_DIRECTORY}")
  endforeach()
endif()
add_custom_target(creator_tools
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CREATOR_TOOLS_DIRECTORY}/sdk"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:daw_creator_compiler>" "${CREATOR_TOOLS_DIRECTORY}/daw_creator_compiler${CMAKE_EXECUTABLE_SUFFIX}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CREATOR_LLVM_ROOT}/bin/clang${CMAKE_EXECUTABLE_SUFFIX}" "${CREATOR_TOOLS_DIRECTORY}/clang${CMAKE_EXECUTABLE_SUFFIX}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CREATOR_LLVM_ROOT}/bin/wasm-ld${CMAKE_EXECUTABLE_SUFFIX}" "${CREATOR_TOOLS_DIRECTORY}/wasm-ld${CMAKE_EXECUTABLE_SUFFIX}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CREATOR_WAMRC}" "${CREATOR_TOOLS_DIRECTORY}/wamrc${CMAKE_EXECUTABLE_SUFFIX}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CREATOR_CRYPTO_RUNTIME}" "${CREATOR_TOOLS_DIRECTORY}"
  ${_creator_runtime_commands}
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CREATOR_TOOLS_DIRECTORY}/sdk"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${CREATOR_TOOLS_DIRECTORY}/examples"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CREATOR_TOOLS_DIRECTORY}/README.md"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CREATOR_LLVM_ROOT}/lib/clang/20" "${CREATOR_TOOLS_DIRECTORY}/lib/clang/20"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CREATOR_SYSROOT}" "${CREATOR_TOOLS_DIRECTORY}/sysroot"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/licenses" "${CREATOR_TOOLS_DIRECTORY}/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${creator_wamr_SOURCE_DIR}/LICENSE" "${CREATOR_TOOLS_DIRECTORY}/licenses/WAMR.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${creator_binaryen_SOURCE_DIR}/LICENSE" "${CREATOR_TOOLS_DIRECTORY}/licenses/Binaryen.txt"
  DEPENDS daw_creator_compiler VERBATIM)
if(APPLE)
  # Resolve dylib install names inside the copied tool directory, including
  # versioned OpenSSL IDs rather than retaining a developer's Homebrew prefix.
  add_custom_command(TARGET creator_tools POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DTOOLS_DIRECTORY=${CREATOR_TOOLS_DIRECTORY}"
      "-DLIBRARY_DIRECTORIES=$<TARGET_FILE_DIR:OpenSSL::Crypto>;${CREATOR_LLVM_ROOT}/lib"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/RelocateTools.cmake"
    VERBATIM)
endif()
if(APPLE AND DAW_PACKAGE)
  install(DIRECTORY "${CREATOR_TOOLS_DIRECTORY}/" DESTINATION "VLTONE.app/Contents/MacOS/CreatorTools" USE_SOURCE_PERMISSIONS)
else()
  install(DIRECTORY "${CREATOR_TOOLS_DIRECTORY}/" DESTINATION bin/CreatorTools USE_SOURCE_PERMISSIONS)
endif()
