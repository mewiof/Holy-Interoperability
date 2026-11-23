# Shared target conventions.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Everything the project injects into a title process links the CRT statically:
# a title package usually ships its own C/C++ runtime DLLs and the runtime must
# neither depend on nor interfere with them.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

get_property(HOLY_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(HOLY_MULTI_CONFIG)
  set(HOLY_BIN_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
else()
  set(HOLY_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

# Reimplemented console system libraries live next to the runtime in `libs/`.
set(HOLY_LIBS_DIR "${HOLY_BIN_DIR}/libs")

function(holy_target_defaults target)
  target_compile_definitions(${target} PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  if(HOLY_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE /WX)
  endif()
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HOLY_BIN_DIR}"
    PDB_OUTPUT_DIRECTORY "${HOLY_BIN_DIR}")
endfunction()

# holy_add_system_library(<target> OUTPUT_NAME <dll base name> SOURCES ... [LIBS ...])
#
# Declares a clean-room reimplementation of a console system library. The DLL
# is emitted into `libs/` under the name titles import it by (or under an alias
# registered in core/common/provider.cpp when that name collides with a host
# system DLL).
function(holy_add_system_library target)
  cmake_parse_arguments(ARG "" "OUTPUT_NAME" "SOURCES;LIBS" ${ARGN})
  add_library(${target} SHARED ${ARG_SOURCES})
  holy_target_defaults(${target})
  target_link_libraries(${target} PRIVATE holyrt ${ARG_LIBS})
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "${ARG_OUTPUT_NAME}"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${HOLY_LIBS_DIR}"
    PDB_OUTPUT_DIRECTORY "${HOLY_LIBS_DIR}")
endfunction()
