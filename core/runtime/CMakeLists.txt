# holyrt.dll: the part of the project that lives inside the title process.
add_library(holyrt SHARED
  api.h
  binder.cpp
  binder.h
  cpu_compat.cpp
  cpu_compat.h
  crash.cpp
  crash.h
  dllmain.cpp
  hooks.cpp
  hooks.h
  memory.cpp
  memory.h
  network.cpp
  network.h
  state.cpp
  state.h
  stats.cpp
  stats.h
  thunks.cpp
  thunks.h
  watch.cpp
  watch.h)
holy_target_defaults(holyrt)
target_compile_definitions(holyrt PRIVATE HOLYRT_BUILD)
target_link_libraries(holyrt PRIVATE holy_common holy_pe holy_analysis psapi)
