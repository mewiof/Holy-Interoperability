# The console's game runtime shares its file name with an unrelated host DLL,
# so the implementation is built under an alias (see core/common/provider.cpp).
holy_add_system_library(holy_lib_xgameruntime
  OUTPUT_NAME holy_xgameruntime
  SOURCES xgameruntime.cpp)
