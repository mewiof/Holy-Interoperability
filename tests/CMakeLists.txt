# --- unit tests ---------------------------------------------------------------

add_executable(holy_unit_tests
  unit/main.cpp
  unit/test_manifest.cpp
  unit/test_pe.cpp
  unit/test_provider.cpp
  unit/test_strings.cpp
  unit/test_vfs.cpp
  unit/test_xml.cpp)
holy_target_defaults(holy_unit_tests)
target_link_libraries(holy_unit_tests PRIVATE holy_common holy_pe holy_package holy_analysis)
add_test(NAME unit COMMAND holy_unit_tests)

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

add_executable(sample_title WIN32 sample_title/main.cpp)
holy_target_defaults(sample_title)
target_link_libraries(sample_title PRIVATE holy_lib_xmem sample_stub_lifecycle sample_stub_missing advapi32)
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

add_test(NAME e2e.unimplemented_traps COMMAND holy run "${HOLY_SAMPLE_DIR}" --timeout 60 -- --call-missing)
set_tests_properties(e2e.unimplemented_traps PROPERTIES
  PASS_REGULAR_EXPRESSION "unimplemented import holy_sample_missing.dll!HolySampleMissingFunction")

add_test(NAME e2e.unimplemented_continues
  COMMAND holy run "${HOLY_SAMPLE_DIR}" --unimplemented continue --timeout 60 -- --call-missing)
set_tests_properties(e2e.unimplemented_continues PROPERTIES PASS_REGULAR_EXPRESSION "SAMPLE TITLE: PASS")
