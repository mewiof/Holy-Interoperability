// Import-library donor for the application lifecycle API set. Building this
// DLL yields a .lib that makes the sample title import these functions under
// the API set's name; the DLL itself is never loaded.
#include <windows.h>

extern "C" {

__declspec(dllexport) ULONG NTAPI RegisterAppStateChangeNotification(void*, void*, void**) {
  return ERROR_NOT_SUPPORTED;
}

__declspec(dllexport) void NTAPI UnregisterAppStateChangeNotification(void*) {}

}  // extern "C"
