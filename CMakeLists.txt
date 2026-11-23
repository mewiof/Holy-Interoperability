cmake_minimum_required(VERSION 3.25)

project(holy
  VERSION 0.1.0
  DESCRIPTION "Compatibility layer that runs x64 console title packages as native Windows processes"
  LANGUAGES CXX)

if(NOT (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8))
  message(FATAL_ERROR "Only 64-bit Windows hosts are supported at the moment (see docs/BUILD.md).")
endif()

option(HOLY_BUILD_TESTS "Build the unit tests and the self-authored sample title" ON)
option(HOLY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/HolyTargets.cmake)

add_subdirectory(core)
add_subdirectory(tools)

if(HOLY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
