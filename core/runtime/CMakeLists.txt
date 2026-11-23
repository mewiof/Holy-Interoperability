# holyrt.dll: the part of the project that lives inside the title process.
add_library(holyrt SHARED
  api.h
  binder.cpp
  binder.h
  crash.cpp
  crash.h
  dllmain.cpp
  hooks.cpp
  hooks.h
  state.cpp
  state.h
  thunks.cpp
  thunks.h)
holy_target_defaults(holyrt)
target_compile_definitions(holyrt PRIVATE HOLYRT_BUILD)
target_link_libraries(holyrt PRIVATE holy_common holy_pe holy_analysis)
