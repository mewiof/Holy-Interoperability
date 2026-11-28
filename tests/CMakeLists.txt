# --- unit tests ---------------------------------------------------------------

add_executable(holy_unit_tests
  unit/main.cpp
  unit/test_manifest.cpp
  unit/test_memory_map.cpp
  unit/test_pe.cpp
  unit/test_provider.cpp
  unit/test_shader_container.cpp
  unit/test_shader_execute.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/geometry.cpp
  unit/test_shader_translate.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader_container.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/dxbc.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/inflate.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/lz.cpp
  unit/test_lz.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/rdna.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/shader/translate.cpp
  unit/test_strings.cpp
  unit/test_texture_layout.cpp
  unit/test_gpu_layout.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/gpu_layout.cpp
  unit/test_texture_codec.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/texture_codec.cpp
  unit/test_vfs.cpp
  unit/test_write_watch.cpp
  ${PROJECT_SOURCE_DIR}/core/libs/d3d12_xs/write_watch.cpp
  unit/test_xml.cpp)
holy_target_defaults(holy_unit_tests)
target_link_libraries(holy_unit_tests PRIVATE holy_common holy_pe holy_package holy_analysis d3d12 dxgi)
add_test(NAME unit COMMAND holy_unit_tests)

# The run report (tools/run_report): its parser and verdicts on built-in logs.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME tools.run_report
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/run_report/run_report.py" --self-test)
endif()

# --- sample title ---------------------------------------------------------------
#
# A small program written for this repository that is built the way a console
# title is linked: against console library names that do not exist on a desktop
# host. It cannot start on its own; it only runs through the launcher, which
# makes it an end-to-end test of the loader that needs no third-party binary.

set(HOLY_SAMPLE_DIR "${HOLY_BIN_DIR}/sample_title")
set(HOLY_STUB_DIR "${CMAKE_BINARY_DIR}/stubs")

# Import-library donors. Only the .lib files are used, to make the sample
# import these library names; the DLLs themselves are never loaded.
function(holy_add_import_stub target output_name source)
  add_library(${target} SHARED ${source})
  holy_target_defaults(${target})
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "${output_name}"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${HOLY_STUB_DIR}"
    PDB_OUTPUT_DIRECTORY "${HOLY_STUB_DIR}")
endfunction()

holy_add_import_stub(sample_stub_lifecycle "api-ms-win-core-psm-appnotify-l1-1-0" sample_title/stub_lifecycle.cpp)
holy_add_import_stub(sample_stub_missing "holy_sample_missing" sample_title/stub_missing.cpp)
holy_add_import_stub(sample_stub_gameinput "GameInput" sample_title/stub_gameinput.cpp)
target_sources(sample_stub_gameinput PRIVATE sample_title/stub_gameinput.def)

# A helper DLL shipped inside the sample package, next to the executable.
add_library(sample_package_lib SHARED sample_title/package_lib.cpp)
holy_target_defaults(sample_package_lib)
target_link_libraries(sample_package_lib PRIVATE ws2_32)
set_target_properties(sample_package_lib PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "${HOLY_SAMPLE_DIR}"
  PDB_OUTPUT_DIRECTORY "${HOLY_SAMPLE_DIR}")

add_executable(sample_title WIN32 sample_title/alloc_hooks.cpp sample_title/checks.h sample_title/graphics_checks.cpp
  sample_title/graphics_resource_checks.cpp sample_title/main.cpp sample_title/runtime_checks.cpp)
holy_target_defaults(sample_title)
target_link_libraries(sample_title PRIVATE holy_lib_xmem holy_lib_xcrdapi holy_lib_d3d12_xs holy_lib_acphal holy_lib_xapu holy_lib_chad
  sample_stub_lifecycle sample_stub_missing
  sample_stub_gameinput sample_package_lib advapi32 ws2_32)
set_target_properties(sample_title PROPERTIES
  # Like a real title, the sample uses the DLL C runtime.
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "${HOLY_SAMPLE_DIR}"
  PDB_OUTPUT_DIRECTORY "${HOLY_SAMPLE_DIR}")
add_custom_command(TARGET sample_title POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/sample_title/package/MicrosoftGame.config" "${HOLY_SAMPLE_DIR}/MicrosoftGame.config"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${HOLY_SAMPLE_DIR}/data"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/sample_title/package/data/hello.txt" "${HOLY_SAMPLE_DIR}/data/hello.txt"
  VERBATIM)
add_dependencies(sample_title holy)

add_test(NAME e2e.inspect COMMAND holy inspect "${HOLY_SAMPLE_DIR}")
set_tests_properties(e2e.inspect PROPERTIES
  PASS_REGULAR_EXPRESSION "holy_sample_missing.dll!HolySampleMissingFunction")

add_test(NAME e2e.run_sandboxed COMMAND holy run "${HOLY_SAMPLE_DIR}" --timeout 60 -- --expect-sandbox)
set_tests_properties(e2e.run_sandboxed PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLE TITLE: PASS")

add_test(NAME e2e.run_unsandboxed COMMAND holy run "${HOLY_SAMPLE_DIR}" --no-sandbox --timeout 60)
set_tests_properties(e2e.run_unsandboxed PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLE TITLE: PASS")

add_test(NAME e2e.run_online COMMAND holy run "${HOLY_SAMPLE_DIR}" --network host --timeout 60 -- --expect-online)
set_tests_properties(e2e.run_online PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLE TITLE: PASS")

add_test(NAME e2e.run_local_user COMMAND holy run "${HOLY_SAMPLE_DIR}" --user local --timeout 60 -- --expect-local-user)
set_tests_properties(e2e.run_local_user PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLE TITLE: PASS")

add_test(NAME e2e.unimplemented_traps COMMAND holy run "${HOLY_SAMPLE_DIR}" --timeout 60 -- --call-missing)
set_tests_properties(e2e.unimplemented_traps PROPERTIES
  PASS_REGULAR_EXPRESSION "unimplemented import holy_sample_missing.dll!HolySampleMissingFunction")

add_test(NAME e2e.unimplemented_continues
  COMMAND holy run "${HOLY_SAMPLE_DIR}" --unimplemented continue --timeout 60 -- --call-missing)
set_tests_properties(e2e.unimplemented_continues PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLE TITLE: PASS")
# Full sample runs measure host memory going back after release (system-wide
# commit): run them one at a time so other runs' allocations stay out of it.
set_tests_properties(e2e.run_sandboxed e2e.run_unsandboxed e2e.run_online e2e.run_local_user
                     e2e.unimplemented_continues PROPERTIES RESOURCE_LOCK sample_title_memory)
