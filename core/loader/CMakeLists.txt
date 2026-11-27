add_library(holy_loader STATIC
  launch.cpp
  launch.h
  profiler.cpp
  profiler.h)
holy_target_defaults(holy_loader)
target_link_libraries(holy_loader
  PUBLIC holy_common holy_pe holy_package
  PRIVATE ntdll advapi32 shell32 ole32 dbghelp psapi winmm)
