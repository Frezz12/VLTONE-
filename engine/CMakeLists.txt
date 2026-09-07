# daw_engine — the graph-based audio engine.
#
# Dependency graph + task graph + work-stealing job system. Depends on nothing
# but portable C++ and vendored DSP headers: no UI/device layer or model. The
# device/transport/document layers plug in by owning nodes.
file(GLOB_RECURSE DAW_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# Retired prototype sources use a different public API and device backend.
list(FILTER DAW_ENGINE_SOURCES EXCLUDE REGEX "/src/")

list(FILTER DAW_ENGINE_SOURCES EXCLUDE REGEX "/SimdAvx2\\.cpp$")
add_library(daw_engine STATIC ${DAW_ENGINE_SOURCES})

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64)$" AND
   NOT CMAKE_GENERATOR_PLATFORM MATCHES "ARM" AND
   NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    add_library(daw_simd_avx2 OBJECT DSP/SimdAvx2.cpp)
    target_include_directories(daw_simd_avx2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(daw_simd_avx2 PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION FALSE INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
    if(MSVC)
        target_compile_options(daw_simd_avx2 PRIVATE /arch:AVX2 /fp:strict)
    else()
        target_compile_options(daw_simd_avx2 PRIVATE -mavx2 -ffp-contract=off)
    endif()
    target_sources(daw_engine PRIVATE $<TARGET_OBJECTS:daw_simd_avx2>)
    target_compile_definitions(daw_engine PRIVATE DAW_RUNTIME_AVX2=1)
endif()

target_include_directories(daw_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(daw_engine PUBLIC ${CMAKE_SOURCE_DIR}/shared)
target_compile_features(daw_engine PUBLIC cxx_std_23)
target_include_directories(daw_engine SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/signalsmith)
# Keep spectral processing optimised in Clang/GCC development builds too.
# MSVC uses its configuration flags (Release /O2; Debug /RTC is incompatible).
set_source_files_properties(DSP/TimeStretch.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>")

find_package(Threads REQUIRED)
target_link_libraries(daw_engine PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(daw_engine PRIVATE avrt)
endif()

if(MSVC)
    target_compile_options(daw_engine PRIVATE /W4)
else()
    target_compile_options(daw_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # -ffast-math is deliberately NOT used: denormal handling and reproducible
    # summation matter more than the last few percent, and the renderer must
    # match the realtime path sample for sample.
endif()
