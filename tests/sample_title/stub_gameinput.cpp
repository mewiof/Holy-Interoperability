// Import-library donor for the console input library, which titles import
// by ordinal (see stub_gameinput.def). The DLL itself is never loaded.
#include <windows.h>

extern "C" HRESULT WINAPI GameInputCreate(void**) {
  return E_NOTIMPL;
}
