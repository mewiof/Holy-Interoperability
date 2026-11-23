# The console input library shares its file name with the host's GameInput
# DLL, so the implementation is built under an alias (see
# core/common/provider.cpp). Titles import its entry point by ordinal.
holy_add_system_library(holy_lib_gameinput
  OUTPUT_NAME holy_gameinput
  SOURCES
    gameinput.cpp
    gameinput.def)
