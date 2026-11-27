# The console's game runtime shares its file name with an unrelated host DLL,
# so the implementation is built under an alias (see core/common/provider.cpp).
holy_add_system_library(holy_lib_xgameruntime
  OUTPUT_NAME holy_xgameruntime
  SOURCES
    core_interface.cpp
    display.cpp
    game_save.cpp
    local_storage.cpp
    networking.cpp
    package.cpp
    provider.cpp
    provider.h
    services.h
    speech.cpp
    store.cpp
    system.cpp
    task_queue.cpp
    task_queue.h
    user.cpp
    xgameruntime.cpp
  LIBS holy_package bcrypt advapi32 ole32 sapi)
