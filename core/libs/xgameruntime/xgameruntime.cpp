// xgameruntime.dll: the console game runtime (users, storage, packages,
// lifecycle, task queues and the other platform services titles reach through
// the runtime's statically linked client library).
//
// Present state: a skeleton. The library exists so that a title's request for
// the runtime is answered by this project and never by the host's same-named
// DLL, and so that the entry points the client library looks up show in the
// log. The provider interfaces behind those entry points are the next
// subsystem to implement.
#include <windows.h>

#include "core/libs/support.h"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    LIB_LOG(holy::lib::kInfo, "xgameruntime",
            "game runtime skeleton loaded (no provider interfaces implemented yet)");
  }
  return TRUE;
}
