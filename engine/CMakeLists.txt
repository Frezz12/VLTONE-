# daw_engine — the graph-based audio engine.
#
# Dependency graph + task graph + work-stealing job system. Depends on nothing
# but the standard library: no UI, no device layer, no project model. The
# device/transport/document layers plug in by owning nodes.
file(GLOB_RECURSE DAW_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# Retired prototype sources use a different public API and device backend.
list(FILTER DAW_ENGINE_SOURCES EXCLUDE REGEX "/src/")

add_library(daw_engine STATIC ${DAW_ENGINE_SOURCES})

target_include_directories(daw_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_engine PUBLIC cxx_std_23)

find_package(Threads REQUIRED)
target_link_libraries(daw_engine PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(daw_engine PRIVATE /W4 /arch:AVX2)
else()
    target_compile_options(daw_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # -ffast-math is deliberately NOT used: denormal handling and reproducible
    # summation matter more than the last few percent, and the renderer must
    # match the realtime path sample for sample.
endif()
