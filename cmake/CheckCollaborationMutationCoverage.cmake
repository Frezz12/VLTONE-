# Production collaboration keeps an explicit classification for every public
# EngineController operation that can mutate state.  Const methods are proven
# read-only by their declarations; the short allowlist below covers legacy
# query APIs that cannot be const because their platform backends are not.
set(VLT_ENGINE_CONTROLLER_HEADER
    "${CMAKE_SOURCE_DIR}/controller/EngineController.hpp")
set(VLT_MUTATION_LEDGER
    "${CMAKE_SOURCE_DIR}/controller/collaboration/MutationCapabilityLedger.def")
foreach(VLT_REQUIRED_FILE VLT_ENGINE_CONTROLLER_HEADER VLT_MUTATION_LEDGER)
    if(NOT EXISTS "${${VLT_REQUIRED_FILE}}")
        message(FATAL_ERROR
            "Collaboration release gate failed: ${${VLT_REQUIRED_FILE}} missing")
    endif()
endforeach()

file(READ "${VLT_ENGINE_CONTROLLER_HEADER}" VLT_ENGINE_CONTROLLER_TEXT)
string(FIND "${VLT_ENGINE_CONTROLLER_TEXT}" "class EngineController {"
       VLT_CLASS_BEGIN)
if(VLT_CLASS_BEGIN EQUAL -1)
    message(FATAL_ERROR
        "Collaboration release gate failed: EngineController declaration missing")
endif()
string(SUBSTRING "${VLT_ENGINE_CONTROLLER_TEXT}" ${VLT_CLASS_BEGIN} -1
       VLT_ENGINE_CONTROLLER_CLASS)
string(FIND "${VLT_ENGINE_CONTROLLER_CLASS}" "public:" VLT_PUBLIC_BEGIN)
if(VLT_PUBLIC_BEGIN EQUAL -1)
    message(FATAL_ERROR
        "Collaboration release gate failed: EngineController public API missing")
endif()
string(SUBSTRING "${VLT_ENGINE_CONTROLLER_CLASS}" ${VLT_PUBLIC_BEGIN} -1
       VLT_ENGINE_CONTROLLER_PUBLIC_TAIL)
string(FIND "${VLT_ENGINE_CONTROLLER_PUBLIC_TAIL}" "
private:" VLT_PRIVATE_BEGIN)
if(VLT_PRIVATE_BEGIN EQUAL -1)
    message(FATAL_ERROR
        "Collaboration release gate failed: EngineController private boundary missing")
endif()
math(EXPR VLT_PUBLIC_BODY_LENGTH "${VLT_PRIVATE_BEGIN} - 7")
string(SUBSTRING "${VLT_ENGINE_CONTROLLER_PUBLIC_TAIL}" 7
       ${VLT_PUBLIC_BODY_LENGTH} VLT_ENGINE_CONTROLLER_PUBLIC)
string(REGEX REPLACE "//[^\r\n]*" ""
       VLT_ENGINE_CONTROLLER_PUBLIC "${VLT_ENGINE_CONTROLLER_PUBLIC}")

# CMake has no C++ parser, but the public API follows one useful invariant:
# EngineController declarations are at brace depth zero while nested public
# structs and inline method bodies are not.  This tiny lexer extracts the
# method identifier before the first top-level body/semicolon.
macro(vlt_consider_engine_controller_declaration VLT_DECLARATION)
    string(STRIP "${VLT_DECLARATION}" VLT_DECLARATION_STRIPPED)
    if(NOT VLT_DECLARATION_STRIPPED MATCHES "^(using|typedef|struct|class|enum)([ \t\r\n]|$)")
        string(REGEX MATCH "[A-Za-z_~][A-Za-z0-9_~]*[ \t\r\n]*\\("
               VLT_METHOD_TOKEN "${VLT_DECLARATION_STRIPPED}")
        if(VLT_METHOD_TOKEN)
            string(REGEX REPLACE "[ \t\r\n]*\\($" "" VLT_METHOD_NAME
                   "${VLT_METHOD_TOKEN}")
            if(NOT VLT_METHOD_NAME STREQUAL "EngineController" AND
               NOT VLT_METHOD_NAME STREQUAL "~EngineController")
                if(NOT VLT_DECLARATION_STRIPPED MATCHES
                   "\\)[ \t\r\n]*(const([ \t\r\n]+noexcept)?|noexcept[ \t\r\n]+const)([ \t\r\n]|\\{|;|$)")
                    list(APPEND VLT_PUBLIC_NONCONST_METHODS "${VLT_METHOD_NAME}")
                endif()
            endif()
        endif()
    endif()
endmacro()

set(VLT_PUBLIC_NONCONST_METHODS)
set(VLT_DECLARATION "")
set(VLT_BRACE_DEPTH 0)
set(VLT_PAREN_DEPTH 0)
string(LENGTH "${VLT_ENGINE_CONTROLLER_PUBLIC}" VLT_PUBLIC_LENGTH)
set(VLT_PUBLIC_OFFSET 0)
while(VLT_PUBLIC_OFFSET LESS VLT_PUBLIC_LENGTH)
    string(SUBSTRING "${VLT_ENGINE_CONTROLLER_PUBLIC}"
           ${VLT_PUBLIC_OFFSET} 1 VLT_CHAR)
    if(VLT_BRACE_DEPTH EQUAL 0)
        string(APPEND VLT_DECLARATION "${VLT_CHAR}")
        if(VLT_CHAR STREQUAL "(")
            math(EXPR VLT_PAREN_DEPTH "${VLT_PAREN_DEPTH} + 1")
        elseif(VLT_CHAR STREQUAL ")")
            math(EXPR VLT_PAREN_DEPTH "${VLT_PAREN_DEPTH} - 1")
        elseif(VLT_CHAR STREQUAL "{" AND VLT_PAREN_DEPTH EQUAL 0)
            vlt_consider_engine_controller_declaration("${VLT_DECLARATION}")
            set(VLT_DECLARATION "")
            set(VLT_BRACE_DEPTH 1)
        elseif(VLT_CHAR STREQUAL ";" AND VLT_PAREN_DEPTH EQUAL 0)
            vlt_consider_engine_controller_declaration("${VLT_DECLARATION}")
            set(VLT_DECLARATION "")
        endif()
    elseif(VLT_CHAR STREQUAL "{")
        math(EXPR VLT_BRACE_DEPTH "${VLT_BRACE_DEPTH} + 1")
    elseif(VLT_CHAR STREQUAL "}")
        math(EXPR VLT_BRACE_DEPTH "${VLT_BRACE_DEPTH} - 1")
    endif()
    math(EXPR VLT_PUBLIC_OFFSET "${VLT_PUBLIC_OFFSET} + 1")
endwhile()
if(NOT VLT_BRACE_DEPTH EQUAL 0 OR NOT VLT_PAREN_DEPTH EQUAL 0)
    message(FATAL_ERROR
        "Collaboration release gate failed: cannot parse EngineController public API")
endif()
list(REMOVE_DUPLICATES VLT_PUBLIC_NONCONST_METHODS)

# These are observational despite legacy non-const platform/engine APIs.
# Every exception is validated below so this list cannot silently go stale.
set(VLT_PROVEN_NONCONST_GETTERS
    enumerateOutputDevices
    enumerateInputDevices
    currentOutputDeviceUid
    currentInputDeviceUid
    recordingPreview
    clipSampleData
    clipSampleParameter)

file(READ "${VLT_MUTATION_LEDGER}" VLT_MUTATION_LEDGER_TEXT)
if(VLT_MUTATION_LEDGER_TEXT MATCHES "Unclassified")
    message(FATAL_ERROR
        "Collaboration release gate failed: capability ledger contains Unclassified")
endif()
string(REGEX MATCHALL
       "VLT_MUTATION\\([A-Za-z_][A-Za-z0-9_]*, (SharedCommand|LocalOnly|BlockedV1)\\)"
       VLT_MUTATION_ROWS "${VLT_MUTATION_LEDGER_TEXT}")
string(REGEX MATCHALL "VLT_MUTATION\\(" VLT_LEDGER_ROW_PREFIXES
       "${VLT_MUTATION_LEDGER_TEXT}")
list(LENGTH VLT_MUTATION_ROWS VLT_VALID_LEDGER_ROW_COUNT)
list(LENGTH VLT_LEDGER_ROW_PREFIXES VLT_LEDGER_ROW_COUNT)
if(NOT VLT_VALID_LEDGER_ROW_COUNT EQUAL VLT_LEDGER_ROW_COUNT)
    message(FATAL_ERROR
        "Collaboration release gate failed: malformed capability ledger row")
endif()
set(VLT_LEDGER_METHODS)
set(VLT_DUPLICATE_LEDGER_METHODS)
foreach(VLT_MUTATION_ROW IN LISTS VLT_MUTATION_ROWS)
    string(REGEX REPLACE "^VLT_MUTATION\\(([A-Za-z_][A-Za-z0-9_]*),.*$"
           "\\1" VLT_LEDGER_METHOD "${VLT_MUTATION_ROW}")
    if(VLT_LEDGER_METHOD IN_LIST VLT_LEDGER_METHODS)
        list(APPEND VLT_DUPLICATE_LEDGER_METHODS "${VLT_LEDGER_METHOD}")
    else()
        list(APPEND VLT_LEDGER_METHODS "${VLT_LEDGER_METHOD}")
    endif()
endforeach()
if(VLT_DUPLICATE_LEDGER_METHODS)
    list(REMOVE_DUPLICATES VLT_DUPLICATE_LEDGER_METHODS)
    list(JOIN VLT_DUPLICATE_LEDGER_METHODS ", " VLT_DUPLICATE_LEDGER_TEXT)
    message(FATAL_ERROR
        "Collaboration release gate failed: duplicate capability rows: "
        "${VLT_DUPLICATE_LEDGER_TEXT}")
endif()

set(VLT_UNCLASSIFIED_MUTATORS)
foreach(VLT_METHOD IN LISTS VLT_PUBLIC_NONCONST_METHODS)
    if(NOT VLT_METHOD IN_LIST VLT_PROVEN_NONCONST_GETTERS AND
       NOT VLT_METHOD IN_LIST VLT_LEDGER_METHODS)
        list(APPEND VLT_UNCLASSIFIED_MUTATORS "${VLT_METHOD}")
    endif()
endforeach()
if(VLT_UNCLASSIFIED_MUTATORS)
    list(JOIN VLT_UNCLASSIFIED_MUTATORS ", " VLT_UNCLASSIFIED_MUTATORS_TEXT)
    message(FATAL_ERROR
        "Collaboration release gate failed: public EngineController mutations "
        "lack capability rows: ${VLT_UNCLASSIFIED_MUTATORS_TEXT}")
endif()

set(VLT_STALE_LEDGER_METHODS)
foreach(VLT_METHOD IN LISTS VLT_LEDGER_METHODS)
    if(NOT VLT_METHOD IN_LIST VLT_PUBLIC_NONCONST_METHODS)
        list(APPEND VLT_STALE_LEDGER_METHODS "${VLT_METHOD}")
    endif()
endforeach()
if(VLT_STALE_LEDGER_METHODS)
    list(JOIN VLT_STALE_LEDGER_METHODS ", " VLT_STALE_LEDGER_TEXT)
    message(FATAL_ERROR
        "Collaboration release gate failed: capability rows are not public "
        "EngineController mutations: ${VLT_STALE_LEDGER_TEXT}")
endif()

set(VLT_STALE_GETTER_EXCEPTIONS)
foreach(VLT_METHOD IN LISTS VLT_PROVEN_NONCONST_GETTERS)
    if(NOT VLT_METHOD IN_LIST VLT_PUBLIC_NONCONST_METHODS OR
       VLT_METHOD IN_LIST VLT_LEDGER_METHODS)
        list(APPEND VLT_STALE_GETTER_EXCEPTIONS "${VLT_METHOD}")
    endif()
endforeach()
if(VLT_STALE_GETTER_EXCEPTIONS)
    list(JOIN VLT_STALE_GETTER_EXCEPTIONS ", " VLT_STALE_GETTER_TEXT)
    message(FATAL_ERROR
        "Collaboration release gate failed: stale/non-getter exclusions: "
        "${VLT_STALE_GETTER_TEXT}")
endif()

foreach(VLT_CAPABILITY SharedCommand LocalOnly BlockedV1)
    if(NOT VLT_MUTATION_LEDGER_TEXT MATCHES
       "VLT_MUTATION\\([^,]+, ${VLT_CAPABILITY}\\)")
        message(FATAL_ERROR
            "Collaboration release gate failed: no ${VLT_CAPABILITY} entries")
    endif()
endforeach()

file(READ "${CMAKE_SOURCE_DIR}/controller/EngineController.cpp"
     VLT_ENGINE_CONTROLLER_CPP)
foreach(VLT_REQUIRED_SEAM submitSharedMutation cloudProjectBound)
    if(NOT VLT_ENGINE_CONTROLLER_CPP MATCHES "${VLT_REQUIRED_SEAM}")
        message(FATAL_ERROR
            "Collaboration release gate failed: ${VLT_REQUIRED_SEAM} seam missing")
    endif()
endforeach()
