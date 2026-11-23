// Sample title: a stand-in for a console title, written for this repository.
//
// It is linked like a console title (console memory library, lifecycle API
// set, a library nobody provides) and checks, from the inside, that the
// environment the launcher builds behaves like the one a title expects.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "tests/sample_title/checks.h"

extern "C" {
__declspec(dllimport) void* XMemAlloc(SIZE_T size, unsigned long long attributes);
__declspec(dllimport) void XMemFree(void* address, unsigned long long attributes);
__declspec(dllimport) void* XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type,
                                             unsigned long long allocator_id, DWORD protect);
__declspec(dllimport) ULONG NTAPI RegisterAppStateChangeNotification(void (*routine)(BOOLEAN, void*),
                                                                     void* context, void** registration);
__declspec(dllimport) void NTAPI UnregisterAppStateChangeNotification(void* registration);
__declspec(dllimport) int HolySampleMissingFunction(int value);
__declspec(dllimport) HRESULT WINAPI GameInputCreate(void** game_input);
__declspec(dllimport) BOOL XMemAllocatePhysicalPages(unsigned long long attributes, ULONG_PTR* count,
                                                     ULONG_PTR* pages);
__declspec(dllimport) BOOL XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages);
__declspec(dllimport) void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* pages);
__declspec(dllimport) HRESULT WINAPI DStorageGetFactory(REFIID riid, void** factory);
__declspec(dllimport) int SamplePackageResolve(const char* host);
__declspec(dllimport) int SamplePackageLoadsConsoleRuntime();
}

namespace {
int g_failures = 0;
}  // namespace

bool g_expect_online = false;

void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAILED", what);
  if (!ok) ++g_failures;
}

namespace {

thread_local int t_value = 42;

std::string read_all(HANDLE file) {
  std::string out;
  char buffer[256];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) out.append(buffer, read);
  return out;
}

DWORD WINAPI thread_main(void* parameter) {
  // A fresh thread must see the initial value of the thread-local, not the
  // main thread's modified copy.
  *static_cast<int*>(parameter) = t_value;
  return 0;
}

void lifecycle_callback(BOOLEAN, void*) {}

void test_thread_local_storage() {
  check(t_value == 42, "static TLS is initialised on the main thread");
  t_value = 7;
  int seen = 0;
  HANDLE thread = CreateThread(nullptr, 0, &thread_main, &seen, 0, nullptr);
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  check(seen == 42, "static TLS is initialised on new threads");
}

void test_exceptions() {
  bool caught = false;
  try {
    throw std::runtime_error("expected");
  } catch (const std::runtime_error& error) {
    caught = std::strcmp(error.what(), "expected") == 0;
  }
  check(caught, "C++ exceptions unwind through the title image");
}

void test_memory() {
  auto* small = static_cast<unsigned char*>(XMemAlloc(100, 0));
  check(small != nullptr && small[0] == 0 && small[99] == 0, "XMemAlloc returns zeroed heap memory");
  if (small) std::memset(small, 0xAB, 100);
  XMemFree(small, 0);

  const SIZE_T large_size = 8 * 1024 * 1024;
  auto* large = static_cast<unsigned char*>(XMemAlloc(large_size, 0));
  check(large != nullptr && (reinterpret_cast<uintptr_t>(large) & 0xFFF) == 0,
        "XMemAlloc returns page-aligned memory for large requests");
  if (large) large[large_size - 1] = 1;
  XMemFree(large, 0);

  // 64 KiB alignment requested through the attribute word (log2 = 16 in bits 24..28).
  auto* aligned = XMemAlloc(4096, 16ull << 24);
  check(aligned != nullptr && (reinterpret_cast<uintptr_t>(aligned) & 0xFFFF) == 0,
        "XMemAlloc honours the alignment attribute");
  XMemFree(aligned, 16ull << 24);
}

void test_package_files() {
  HANDLE mounted = CreateFileW(L"G:\\data\\hello.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  check(mounted != INVALID_HANDLE_VALUE, "the package is mounted on G:");
  if (mounted != INVALID_HANDLE_VALUE) {
    check(read_all(mounted).rfind("hello from the package", 0) == 0,
          "package files are readable through the mount");
    CloseHandle(mounted);
  }

  FILE* stream = std::fopen("data\\hello.txt", "rb");
  check(stream != nullptr, "package files are readable relative to the current directory");
  if (stream) std::fclose(stream);

  check(GetFileAttributesW(L"G:\\data") != INVALID_FILE_ATTRIBUTES,
        "directory queries work through the mount");
}

void test_package_is_read_only() {
  HANDLE created = CreateFileW(L"G:\\holy_write_probe.tmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  const DWORD error = GetLastError();
  check(created == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED,
        "creating a file in the package is denied");
  if (created != INVALID_HANDLE_VALUE) CloseHandle(created);

  HANDLE reopened = CreateFileW(L"data\\hello.txt", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  check(reopened == INVALID_HANDLE_VALUE, "opening a package file for writing is denied");
  if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);

  FILE* stream = std::fopen("holy_write_probe2.tmp", "w");
  check(stream == nullptr, "fopen for writing in the package is denied");
  if (stream) std::fclose(stream);

  check(!DeleteFileW(L"G:\\data\\hello.txt") && GetLastError() == ERROR_ACCESS_DENIED,
        "deleting a package file is denied");
  check(!CreateDirectoryW(L"G:\\holy_dir_probe", nullptr), "creating a directory in the package is denied");
}

void test_scratch_storage() {
  HANDLE file = CreateFileW(L"T:\\sample_scratch.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(file != INVALID_HANDLE_VALUE, "temporary storage on T: is writable");
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  check(WriteFile(file, "scratch", 7, &written, nullptr) && written == 7,
        "writes to temporary storage succeed");
  SetFilePointer(file, 0, nullptr, FILE_BEGIN);
  check(read_all(file) == "scratch", "temporary storage reads back what was written");
  CloseHandle(file);
  check(DeleteFileW(L"T:\\sample_scratch.bin") != 0, "temporary files can be deleted");

  // Host caches (graphics driver and runtime) live in the title's data
  // directory: the process's profile path points there.
  wchar_t profile[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  const bool private_profile = length > 5 && length < MAX_PATH && wcscmp(profile + length - 5, L"\\host") == 0 &&
                               (GetFileAttributesW(profile) & FILE_ATTRIBUTE_DIRECTORY) != 0;
  check(private_profile, "the host cache directory is the process's profile path");
}

void test_library_loading() {
  HMODULE xmem = LoadLibraryW(L"xmem.dll");
  check(xmem != nullptr, "LoadLibrary resolves a console library by name");
  check(xmem && GetProcAddress(xmem, "XMemAlloc") != nullptr, "the console library exports its entry points");
  check(GetModuleHandleW(L"xmem.dll") == xmem && xmem != nullptr, "GetModuleHandle agrees with LoadLibrary");

  // This console library shares its name with an unrelated host system DLL.
  // The request must either fail or be answered by this project's own
  // implementation, never by the host's file.
  HMODULE colliding = LoadLibraryW(L"xgameruntime.dll");
  bool safe = colliding == nullptr;
  if (colliding) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(colliding, path, MAX_PATH);
    safe = std::wcsstr(path, L"\\libs\\holy_") != nullptr;
  }
  check(safe, "a console library name is never satisfied by a same-named host DLL");
}

// The sandbox must hold even when the runtime's path layer is not involved:
// reach the host's CreateFileW directly, the way code outside the title image
// (a packaged DLL, the C runtime) would, and try to write into the package.
void test_sandbox_is_enforced_by_the_os() {
  DWORD integrity_rid = 0;
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    BYTE buffer[256];
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
      const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
      integrity_rid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    }
    CloseHandle(token);
  }
  check(integrity_rid == SECURITY_MANDATORY_LOW_RID, "the title runs at low integrity");

  using CreateFileFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
  const auto direct_create =
      reinterpret_cast<CreateFileFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW"));
  check(direct_create != nullptr, "the host CreateFileW is reachable directly");
  if (!direct_create) return;

  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring probe(exe_path);
  probe.resize(probe.find_last_of(L'\\') + 1);
  const std::wstring existing = probe + L"data\\hello.txt";
  probe += L"holy_direct_write_probe.tmp";

  HANDLE created =
      direct_create(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(created == INVALID_HANDLE_VALUE, "the OS refuses to create a file in the package");
  if (created != INVALID_HANDLE_VALUE) {
    CloseHandle(created);
    DeleteFileW(probe.c_str());
  }
  HANDLE modified = direct_create(existing.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  check(modified == INVALID_HANDLE_VALUE, "the OS refuses write access to an existing package file");
  if (modified != INVALID_HANDLE_VALUE) CloseHandle(modified);
}

// Same query the console platform code makes to find out which model it is on.
void test_console_identity() {
  HKEY key = nullptr;
  const LSTATUS opened =
      RegOpenKeyW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM", &key);
  check(opened == ERROR_SUCCESS, "the console identity key can be opened");
  if (opened != ERROR_SUCCESS) return;
  DWORD mode = 0, size = sizeof(mode), type = 0;
  const LSTATUS queried =
      RegQueryValueExW(key, L"ConsoleMode", nullptr, &type, reinterpret_cast<LPBYTE>(&mode), &size);
  check(queried == ERROR_SUCCESS && type == REG_DWORD && (mode == 0x64 || mode == 0x60),
        "the title is told it runs on a current-generation console");
  RegCloseKey(key);
}

void test_virtual_memory() {
  // Reserve with 64 KiB pages (MEM_RESERVE | MEM_LARGE_PAGES | MEM_PHYSICAL), then commit part of it.
  const SIZE_T size = 64 * 1024 * 1024;
  const auto virtual_alloc = &XMemVirtualAlloc;
  auto* region = static_cast<unsigned char*>(virtual_alloc(nullptr, size, 0x20402000, 0x21, PAGE_READWRITE));
  check(region != nullptr && (reinterpret_cast<uintptr_t>(region) & 0xFFFF) == 0,
        "XMemVirtualAlloc reserves address space with console page-size flags");
  if (!region) return;
  auto* committed =
      static_cast<unsigned char*>(virtual_alloc(region, 0x10000, 0x20401000, 0x21, PAGE_READWRITE));
  check(committed == region, "XMemVirtualAlloc commits inside a reservation");
  if (committed) {
    committed[0] = 1;
    committed[0xFFFF] = 2;
  }
  // GPU-only protection bits must not make a reservation fail.
  void* graphics = virtual_alloc(nullptr, size, 0x20402000, 3, 0x801);
  check(graphics != nullptr, "XMemVirtualAlloc accepts graphics protection flags");
  if (graphics) VirtualFree(graphics, 0, MEM_RELEASE);
  VirtualFree(region, 0, MEM_RELEASE);
}

void test_lifecycle() {
  void* registration = nullptr;
  const ULONG status = RegisterAppStateChangeNotification(&lifecycle_callback, nullptr, &registration);
  check(status == ERROR_SUCCESS && registration != nullptr, "lifecycle notifications can be registered");
  if (registration) UnregisterAppStateChangeNotification(registration);
}

// Physical pages: the same page mapped at two addresses is one piece of memory.
void test_physical_pages() {
  ULONG_PTR pages[2] = {};
  ULONG_PTR count = 2;
  check(XMemAllocatePhysicalPages(0x280, &count, pages) && count == 2 && pages[0] && pages[1],
        "XMemAllocatePhysicalPages allocates 64 KiB pages");
  auto* first = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x40000, 0x20402000, 0x23, PAGE_READWRITE));
  auto* second = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x40000, 0x20402000, 0x23, PAGE_READWRITE));
  check(first && second, "reservations for physical mappings can be made");
  if (!first || !second) return;
  check(XMemMapPhysicalPages(first, 2, pages) == first, "physical pages map into a reservation");
  ULONG_PTR swapped[2] = {pages[1], pages[0]};
  check(XMemMapPhysicalPages(second + 0x10000, 2, swapped) == second + 0x10000,
        "the same pages map at a second address");
  first[0x10] = 0x5A;
  first[0x10000 + 0x10] = 0xA5;
  check(second[0x20000 + 0x10] == 0x5A && second[0x10000 + 0x10] == 0xA5,
        "writes through one mapping are visible through the other");
  check(XMemMapPhysicalPages(first, 2, nullptr) == first, "physical pages can be unmapped");
  XMemMapPhysicalPages(second + 0x10000, 2, nullptr);
  check(XMemFreePhysicalPages(2, pages) != FALSE, "physical pages can be freed");
}

// Fast storage: files open through the title's paths.
void test_fast_storage() {
  constexpr GUID kFactory = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
  void* factory = nullptr;
  check(DStorageGetFactory(kFactory, &factory) == S_OK && factory, "the fast-storage factory is available");
  if (!factory) return;
  using OpenFileFn = HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, REFIID, void**);
  const auto open = reinterpret_cast<OpenFileFn>((*static_cast<void***>(factory))[4]);
  void* file = nullptr;
  check(open(factory, L"G:\\data\\hello.txt", kFactory, &file) == S_OK && file, "fast storage opens package files");
  if (file) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(file))[2])(file);
  void* missing = nullptr;
  check(open(factory, L"G:\\data\\missing.bin", kFactory, &missing) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        "fast storage reports missing files");
}

// The console input library, with no input device attached.
void test_input() {
  void* input = nullptr;
  check(GameInputCreate(&input) == S_OK && input != nullptr, "GameInputCreate returns the input interface");
  if (!input) return;
  using GetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void*, void**);
  using RegisterDeviceCallbackFn =
      HRESULT(STDMETHODCALLTYPE*)(void*, void*, uint32_t, uint32_t, uint32_t, void*, void*, uint64_t*);
  void** vtable = *static_cast<void***>(input);
  void* reading = reinterpret_cast<void*>(1);
  const HRESULT hr = reinterpret_cast<GetCurrentReadingFn>(vtable[4])(input, 0x40000, nullptr, &reading);
  check(hr == static_cast<HRESULT>(0x838A0003) && reading == nullptr,
        "with no device attached there is no gamepad reading");
  static int s_device_callbacks = 0;
  uint64_t token = 0;
  const auto callback = +[](uint64_t, void*, void*, uint64_t, uint32_t, uint32_t) { ++s_device_callbacks; };
  const HRESULT registered = reinterpret_cast<RegisterDeviceCallbackFn>(vtable[9])(
      input, nullptr, 0x40000, 1, 2, nullptr, reinterpret_cast<void*>(callback), &token);
  check(registered == S_OK && token != 0 && s_device_callbacks == 0, "blocking device enumeration finds no devices");
}

// The offline switch, seen from the title image and from a DLL the package
// ships.
void test_offline() {
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
  addrinfo* result = nullptr;
  const int status = getaddrinfo("example.com", "80", nullptr, &result);
  check(status == WSAHOST_NOT_FOUND && result == nullptr, "offline, host names do not resolve");
  if (result) freeaddrinfo(result);
  result = nullptr;
  check(getaddrinfo("localhost", "80", nullptr, &result) == 0, "offline, the local host still resolves");
  if (result) freeaddrinfo(result);

  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(80);
  inet_pton(AF_INET, "192.0.2.1", &remote.sin_addr);
  const int connected = connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
  check(connected == SOCKET_ERROR && WSAGetLastError() == WSAENETUNREACH,
        "offline, connections to other hosts are unreachable");
  closesocket(s);

  check(SamplePackageResolve("example.com") == WSAHOST_NOT_FOUND, "offline applies to DLLs the package ships");
  check(SamplePackageLoadsConsoleRuntime() != 0,
        "a packaged DLL loading a console library gets this project's implementation");
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR command_line, int) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("SAMPLE TITLE: starting\n");

  test_thread_local_storage();
  test_exceptions();
  test_memory();
  test_package_files();
  test_package_is_read_only();
  test_scratch_storage();
  test_library_loading();
  test_lifecycle();
  test_console_identity();
  test_virtual_memory();
  g_expect_online = command_line && std::strstr(command_line, "--expect-online");
  test_game_runtime();
  test_input();
  test_physical_pages();
  test_fast_storage();
  test_graphics();
  if (!(command_line && std::strstr(command_line, "--expect-online"))) test_offline();
  if (command_line && std::strstr(command_line, "--expect-sandbox")) test_sandbox_is_enforced_by_the_os();

  if (command_line && std::strstr(command_line, "--call-missing")) {
    // Traps by default; with `--unimplemented continue` the call returns zero.
    const int result = HolySampleMissingFunction(5);
    check(result == 0, "a call into an unimplemented import returns zero in continue mode");
  }

  std::printf(g_failures == 0 ? "SAMPLE TITLE: PASS\n" : "SAMPLE TITLE: FAIL (%d)\n", g_failures);
  return g_failures;
}
