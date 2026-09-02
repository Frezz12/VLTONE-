# Production collaboration must not coexist with the closure/snapshot undo
# paths: they cannot be serialized and can overwrite a later remote edit.
file(READ "${CMAKE_SOURCE_DIR}/controller/EngineController.cpp"
     VLT_ENGINE_CONTROLLER_CPP)
file(READ "${CMAKE_SOURCE_DIR}/controller/EngineController.hpp"
     VLT_ENGINE_CONTROLLER_HPP)

string(REGEX MATCHALL "m_undo[.]push" VLT_LEGACY_UNDO_PUSHES
       "${VLT_ENGINE_CONTROLLER_CPP}")
list(LENGTH VLT_LEGACY_UNDO_PUSHES VLT_LEGACY_UNDO_COUNT)

set(VLT_FORBIDDEN_SNAPSHOT_SYMBOLS
    "pushProjectSnapshotUndo"
    "commitProjectGesture"
    "restoreProject")
set(VLT_SNAPSHOT_SYMBOL_COUNT 0)
foreach(VLT_SYMBOL IN LISTS VLT_FORBIDDEN_SNAPSHOT_SYMBOLS)
    string(REGEX MATCHALL "${VLT_SYMBOL}" VLT_SYMBOL_MATCHES
           "${VLT_ENGINE_CONTROLLER_CPP};${VLT_ENGINE_CONTROLLER_HPP}")
    list(LENGTH VLT_SYMBOL_MATCHES VLT_SYMBOL_COUNT)
    math(EXPR VLT_SNAPSHOT_SYMBOL_COUNT
         "${VLT_SNAPSHOT_SYMBOL_COUNT} + ${VLT_SYMBOL_COUNT}")
endforeach()

if(VLT_LEGACY_UNDO_COUNT GREATER 0 OR VLT_SNAPSHOT_SYMBOL_COUNT GREATER 0)
    message(FATAL_ERROR
        "Collaboration release gate failed: EngineController still contains "
        "${VLT_LEGACY_UNDO_COUNT} closure undo pushes and "
        "${VLT_SNAPSHOT_SYMBOL_COUNT} snapshot-undo references. Move every "
        "shared mutation through CommandGateway/ProjectReducer before release.")
endif()

