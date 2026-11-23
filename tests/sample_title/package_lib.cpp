// A DLL the sample package ships next to its executable, like the helper
// libraries real packages carry. The OS loader binds its imports, so it checks
// that the runtime's policies reach code the package brings as well as the
// title image itself.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cwchar>

extern "C" {

// Result of resolving `host` through this DLL's own Winsock import.
__declspec(dllexport) int SamplePackageResolve(const char* host) {
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
  addrinfo* result = nullptr;
  const int status = getaddrinfo(host, "80", nullptr, &result);
  if (result) freeaddrinfo(result);
  return status;
}

// Non-zero when a console library loaded through this DLL's own
// LoadLibraryExW import is the project's implementation, never the host's
// same-named DLL.
__declspec(dllexport) int SamplePackageLoadsConsoleRuntime() {
  HMODULE module = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) return 0;
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::wcsstr(path, L"\\libs\\holy_") != nullptr;
}

}  // extern "C"
