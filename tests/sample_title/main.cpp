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
#include <vector>
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
__declspec(dllimport) HRESULT ApuCreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes);
__declspec(dllimport) HRESULT ApuAlloc(void** address, uint32_t* apu_address, uint32_t bytes, uint32_t alignment,
                                       uint32_t flags);
__declspec(dllimport) HRESULT ApuFree(void* address);
__declspec(dllimport) bool ApuIsVirtualAddressValid(const void* address, uint32_t bytes);
__declspec(dllimport) uint32_t ApuMapVirtualAddress(const void* address);
__declspec(dllimport) void* ApuMapApuAddress(uint32_t apu_address);
__declspec(dllimport) HRESULT AcpHalAllocateShapeContexts(void* contexts);
__declspec(dllimport) void AcpHalReleaseShapeContexts();
__declspec(dllimport) HRESULT AcpHalCreate(void** out);
__declspec(dllimport) HRESULT XApuConnect(const uint32_t* config, void** memory, void** out);
__declspec(dllimport) HRESULT XApuEnqueueCommand(void* handle, const uint32_t* command, uint32_t flags);
__declspec(dllimport) uint32_t XApuGetResultCount(void* handle, uint32_t);
__declspec(dllimport) void XApuDisconnect(void* handle);
__declspec(dllimport) HRESULT XDspConnectWithMaximumStreamLimit(void* memory, uint32_t bytes, uint32_t streams,
                                                                uint32_t a, uint32_t b, void** out);
__declspec(dllimport) HRESULT XDspActivate(void* handle, void* a, void* b, void* c);
__declspec(dllimport) HRESULT XDspSubmitCommand(void* handle, const void* command);
__declspec(dllimport) HRESULT XDspDeactivate(void* handle);
__declspec(dllimport) HRESULT XDspDisconnect(void* handle);
}

namespace {
int g_failures = 0;
}  // namespace

bool g_expect_online = false;
bool g_expect_local_user = false;

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

// Console-CPU instructions the host may lack (core/runtime/cpu_compat.h):
// CLZERO zeroes the aligned 64-byte line around the address in rAX, both on
// hosts that have it and through the runtime on hosts that do not.
void test_cpu_compatibility() {
  // mov rax, rcx; clzero; ret
  static const unsigned char kCode[] = {0x48, 0x89, 0xC8, 0x0F, 0x01, 0xFC, 0xC3};
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  check(page != nullptr, "executable page for the CPU checks");
  if (!page) return;
  std::memcpy(page, kCode, sizeof(kCode));
  FlushInstructionCache(GetCurrentProcess(), page, sizeof(kCode));
  alignas(64) static unsigned char line[192];
  std::memset(line, 0xAB, sizeof(line));
  reinterpret_cast<void (*)(void*)>(page)(line + 64 + 17);  // inside the second line
  bool zeroed = true, kept = true;
  for (int i = 0; i < 64; ++i) zeroed = zeroed && line[64 + i] == 0;
  for (int i = 0; i < 64; ++i) kept = kept && line[i] == 0xAB && line[128 + i] == 0xAB;
  check(zeroed, "CLZERO zeroes the cache line around its address");
  check(kept, "CLZERO leaves the neighbouring lines alone");
  // mov rax, rcx; xor ecx, ecx; xor edx, edx; monitorx; xor ebx, ebx;
  // mwaitx; mov eax, 7; ret: the wait returns.
  static const unsigned char kWait[] = {0x48, 0x89, 0xC8, 0x31, 0xC9, 0x31, 0xD2, 0x0F, 0x01, 0xFA, 0x53,
                                        0x31, 0xDB, 0x0F, 0x01, 0xFB, 0x5B, 0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
  std::memcpy(page, kWait, sizeof(kWait));
  FlushInstructionCache(GetCurrentProcess(), page, sizeof(kWait));
  check(reinterpret_cast<int (*)(void*)>(page)(line) == 7, "MONITORX and MWAITX return to the caller");
  // SSE4a bit fields on the low quadwords of XMM registers loaded from and
  // stored back to rcx's array (xmm8 and xmm9, saved and restored around).
  static const unsigned char kFields[] = {
      0x48, 0x83, 0xEC, 0x28,                    // sub rsp, 0x28
      0xF3, 0x44, 0x0F, 0x7F, 0x04, 0x24,        // movdqu [rsp], xmm8
      0xF3, 0x44, 0x0F, 0x7F, 0x4C, 0x24, 0x10,  // movdqu [rsp+0x10], xmm9
      0xF3, 0x0F, 0x6F, 0x01,                    // movdqu xmm0, [rcx]
      0xF3, 0x0F, 0x6F, 0x49, 0x10,              // movdqu xmm1, [rcx+0x10]
      0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10,        // insertq xmm0, xmm1, 16, 16
      0xF3, 0x0F, 0x7F, 0x01,                    // movdqu [rcx], xmm0
      0xF3, 0x0F, 0x6F, 0x51, 0x20,              // movdqu xmm2, [rcx+0x20]
      0x66, 0x0F, 0x78, 0xC2, 0x08, 0x04,        // extrq xmm2, 8, 4
      0xF3, 0x0F, 0x7F, 0x51, 0x20,              // movdqu [rcx+0x20], xmm2
      0xF3, 0x44, 0x0F, 0x6F, 0x41, 0x30,        // movdqu xmm8, [rcx+0x30]
      0xF3, 0x44, 0x0F, 0x6F, 0x49, 0x40,        // movdqu xmm9, [rcx+0x40]
      0xF2, 0x45, 0x0F, 0x78, 0xC8, 0x08, 0x00,  // insertq xmm9, xmm8, 8, 0
      0xF3, 0x44, 0x0F, 0x7F, 0x49, 0x40,        // movdqu [rcx+0x40], xmm9
      0xF3, 0x44, 0x0F, 0x6F, 0x04, 0x24,        // movdqu xmm8, [rsp]
      0xF3, 0x44, 0x0F, 0x6F, 0x4C, 0x24, 0x10,  // movdqu xmm9, [rsp+0x10]
      0x48, 0x83, 0xC4, 0x28,                    // add rsp, 0x28
      0xC3};                                     // ret
  std::memcpy(page, kFields, sizeof(kFields));
  FlushInstructionCache(GetCurrentProcess(), page, sizeof(kFields));
  alignas(16) uint64_t fields[10] = {0x1111222233334444ull, 0x5555666677778888ull, 0xBEEF, 0, 0x12345, 0xFFFF,
                                     0xAB, 0, 0x1234567890ABCDEFull, 0x42};
  reinterpret_cast<void (*)(uint64_t*)>(page)(fields);
  check(fields[0] == 0x11112222BEEF4444ull && fields[4] == 0x34 && fields[8] == 0x1234567890ABCDABull,
        "SSE4a's INSERTQ and EXTRQ move bit fields (REX-extended registers too)");
  VirtualFree(page, 0, MEM_RELEASE);
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
  // The console's memory, whatever the host has (titles size pools by it).
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  SetLastError(1234);
  const BOOL got = GlobalMemoryStatusEx(&memory);
  const uint64_t expected = mode == 0x60 ? (10ull << 30) : (16ull << 30);
  check(got && GetLastError() == 1234 && memory.ullTotalPhys == expected && memory.ullAvailPhys <= expected &&
            memory.ullTotalPageFile == expected,
        "GlobalMemoryStatusEx reports the console's memory and no page file");
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

// Waits are fine-grained, as on the console: titles' audio engines sleep a
// millisecond or so between mixing steps (at the host's default 15.6 ms
// resolution one fell a third short of real time).
void test_timer_resolution() {
  LARGE_INTEGER frequency{}, start{}, end{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  for (int i = 0; i < 50; ++i) Sleep(1);
  QueryPerformanceCounter(&end);
  const double ms = 1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
  std::printf("    (50 one-millisecond sleeps took %.1f ms)\n", ms);
  check(ms < 250.0, "a one-millisecond sleep lasts about a millisecond");
}

// Physical pages: the same page mapped at two addresses is one piece of memory.
// Large commits are taken on first touch: they read as zero and accept
// writes, and title arrays in such memory can be passed to the physical
// page functions.
void test_deferred_commit() {
  auto* pool = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 256ull << 20, MEM_RESERVE | MEM_COMMIT, 0x21,
                                                            PAGE_READWRITE));
  check(pool != nullptr, "a 256 MiB commit succeeds");
  if (!pool) return;
  const bool zero = pool[0] == 0 && pool[(256ull << 20) - 1] == 0;
  pool[(128ull << 20) + 7] = 0x42;
  check(zero && pool[(128ull << 20) + 7] == 0x42, "large commits read as zero and accept writes");
  // File reads into untouched memory of the pool, and into memory the title
  // decommitted after the runtime committed it (the kernel does not fault
  // such memory in; the runtime commits it first).
  {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const HANDLE file = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    unsigned char* target = pool + (220ull << 20) + 0x1000;
    DWORD got = 0;
    const bool first = file != INVALID_HANDLE_VALUE && ReadFile(file, target, 0x30000, &got, nullptr) && got > 2 &&
                       target[0] == 'M' && target[1] == 'Z';
    check(first, "file reads into memory committed on first touch succeed");
    const bool decommitted = VirtualFree(pool + (220ull << 20), 0x20000, MEM_DECOMMIT) != FALSE;
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    got = 0;
    const bool second = decommitted && ReadFile(file, target, 0x30000, &got, nullptr) && got > 2 && target[0] == 'M';
    check(second, "file reads into on-demand memory the title decommitted succeed");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
  // A page array in untouched memory of the pool.
  auto* ids = reinterpret_cast<ULONG_PTR*>(pool + (200ull << 20));
  ULONG_PTR count = 1;
  ULONG_PTR page = 0;
  auto* region = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x10000, 0x20402000, 0x23, PAGE_READWRITE));
  const bool allocated = XMemAllocatePhysicalPages(0x280, &count, &page) && region;
  if (allocated) ids[0] = page;
  check(allocated && XMemMapPhysicalPages(region, 1, ids) == region && region[5] == 0,
        "page arrays in memory committed on first touch map pages");
  if (allocated) {
    XMemMapPhysicalPages(region, 1, nullptr);
    XMemFreePhysicalPages(1, &page);
  }
  // Page numbers count 4 KiB frames: titles store them shifted right by 4
  // and rebuild them shifted left.
  ULONG_PTR numbers[3] = {};
  count = 3;
  const bool numbered = XMemAllocatePhysicalPages(0x280, &count, numbers) != FALSE;
  check(numbered && numbers[0] % 16 == 0 && numbers[1] % 16 == 0 && numbers[2] % 16 == 0 &&
            numbers[0] != numbers[1] && numbers[1] != numbers[2],
        "physical page numbers are distinct multiples of 16");
  ULONG_PTR odd = numbers[0] + 1;
  auto* other = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x10000, 0x20402000, 0x23, PAGE_READWRITE));
  check(numbered && other && XMemMapPhysicalPages(other, 1, &odd) == nullptr,
        "a page number that was not handed out is refused");
  if (numbered) XMemFreePhysicalPages(3, numbers);
}

// The texture layout library: swizzle mode suggestion, and copies into the
// layout it reports (desktop copyable footprints: rows 256 bytes apart).
void test_texture_layout() {
  HMODULE xg = LoadLibraryW(L"xg_xs.dll");
  using SuggestFn = HRESULT(WINAPI*)(const void*, uint32_t, uint32_t*);
  using CreateFn = HRESULT(WINAPI*)(const void*, void**);
  const auto suggest = xg ? reinterpret_cast<SuggestFn>(GetProcAddress(xg, "XGSuggestSwizzleMode")) : nullptr;
  const auto create = xg ? reinterpret_cast<CreateFn>(GetProcAddress(xg, "XGCreateTextureComputer")) : nullptr;
  if (!suggest || !create) {
    check(false, "the texture layout library is available");
    return;
  }
  // D3D12_RESOURCE_DESC layout: a 4x2 R8_UNORM 2D texture.
  uint8_t desc[56] = {};
  const uint32_t dimension = 3, height = 2, format = 61, samples = 1;
  const uint64_t width = 4;
  const uint16_t one = 1;
  std::memcpy(desc + 0, &dimension, 4);
  std::memcpy(desc + 16, &width, 8);
  std::memcpy(desc + 24, &height, 4);
  std::memcpy(desc + 28, &one, 2);
  std::memcpy(desc + 30, &one, 2);
  std::memcpy(desc + 32, &format, 4);
  std::memcpy(desc + 36, &samples, 4);
  uint32_t mode = 0xFFFFFFFF;
  check(suggest(desc, 0, &mode) == S_OK && mode == 0, "the layout library suggests a swizzle mode");
  void* computer = nullptr;
  check(create(desc, &computer) == S_OK && computer, "the layout library creates texture computers");
  if (!computer) return;
  using CopyFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
  const uint8_t texels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  std::vector<uint8_t> memory(512, 0xEE);
  const HRESULT copied =
      reinterpret_cast<CopyFn>((*static_cast<void***>(computer))[8])(computer, memory.data(), 0, 0, texels, 4, 8);
  check(copied == S_OK && memory[0] == 1 && memory[3] == 4 && memory[4] == 0xEE && memory[256] == 5 &&
            memory[259] == 8,
        "texture computers copy texels into rows 256 bytes apart");
  // Slot 6: the byte offset of a texel, in the same layout.
  using OffsetFn = uint64_t(STDMETHODCALLTYPE*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t,
                                                uint32_t*);
  const auto offset = reinterpret_cast<OffsetFn>((*static_cast<void***>(computer))[6]);
  check(offset(computer, 0, 0, 3, 1, 0, 0, nullptr) == 259 && offset(computer, 0, 0, 0, 0, 0, 0, nullptr) == 0,
        "texture computers give texel offsets in the layout they copy into");
  // Slot 2: the layout, as titles read it (a header of 0x18 bytes, plane
  // records of 0x6c8, level records of 0x70 from 0x38 in a plane: bytes,
  // offset, 2D slice bytes, row pitch at 0x1c; array pitch at 0x10).
  std::vector<uint8_t> layout(0x1b38, 0xCC);
  reinterpret_cast<void(STDMETHODCALLTYPE*)(void*, void*)>((*static_cast<void***>(computer))[2])(computer,
                                                                                                layout.data());
  uint64_t level_bytes = 0, array_pitch = 0;
  uint32_t row_pitch = 0;
  std::memcpy(&level_bytes, layout.data() + 0x18 + 0x38, 8);
  std::memcpy(&row_pitch, layout.data() + 0x18 + 0x38 + 0x1c, 4);
  std::memcpy(&array_pitch, layout.data() + 0x18 + 0x10, 8);
  check(level_bytes == 512 && row_pitch == 256 && array_pitch == 512 && layout[0x1b37] == 0,
        "texture computers describe their layout (slot 2)");
  reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(computer))[1])(computer);

  // Slot 10, resource tiling, in the console's order: a 512x512 BC7 texture
  // of 10 levels in 2 array slices takes 6 tiles a slice (level 0: 4, level
  // 1: 1, levels 2-9 in the mip tail: 1), the mip tail first, level 0 last.
  const uint32_t bc7 = 98;
  const uint64_t size = 512;
  const uint32_t tall = 512;
  const uint16_t slices = 2, levels = 10;
  std::memcpy(desc + 16, &size, 8);
  std::memcpy(desc + 24, &tall, 4);
  std::memcpy(desc + 28, &slices, 2);
  std::memcpy(desc + 30, &levels, 2);
  std::memcpy(desc + 32, &bc7, 4);
  computer = nullptr;
  check(create(desc, &computer) == S_OK && computer, "the layout library creates computers for arrays");
  if (!computer) return;
  // The desktop D3D12_PACKED_MIP_INFO, D3D12_TILE_SHAPE and
  // D3D12_SUBRESOURCE_TILING layouts.
  struct PackedMips {
    uint8_t NumStandardMips, NumPackedMips;
    uint32_t NumTilesForPackedMips, StartTileIndexInOverallResource;
  };
  struct TileShape {
    uint32_t WidthInTexels, HeightInTexels, DepthInTexels;
  };
  struct SubresourceTiling {
    uint32_t WidthInTiles;
    uint16_t HeightInTiles, DepthInTiles;
    uint32_t StartTileIndexInOverallResource;
  };
  constexpr uint32_t kPackedTile = 0xFFFFFFFF;
  using TilingFn = void(STDMETHODCALLTYPE*)(void*, UINT*, PackedMips*, TileShape*, UINT*, UINT, SubresourceTiling*);
  UINT tiles = 0, tilings_count = 12;
  PackedMips packed{};
  TileShape shape{};
  SubresourceTiling tilings[12] = {};
  reinterpret_cast<TilingFn>((*static_cast<void***>(computer))[10])(computer, &tiles, &packed, &shape,
                                                                    &tilings_count, 0, tilings);
  check(tiles == 12 && packed.NumStandardMips == 2 && packed.NumPackedMips == 8 && packed.NumTilesForPackedMips == 1 &&
            shape.WidthInTexels == 256 && shape.HeightInTexels == 256,
        "resource tiling counts tiles, standard levels and the mip tail");
  check(tilings_count == 12 && tilings[0].StartTileIndexInOverallResource == 2 && tilings[0].WidthInTiles == 2 &&
            tilings[1].StartTileIndexInOverallResource == 1 &&
            tilings[2].StartTileIndexInOverallResource == kPackedTile &&
            tilings[10].StartTileIndexInOverallResource == 6 + 2,
        "resource tiling places the mip tail first and level 0 last in each slice");
  reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(computer))[1])(computer);
}

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
  // Pages allocated after others were freed read as zero where the freed
  // ones held data.
  ULONG_PTR recycled[2] = {};
  count = 2;
  if (XMemAllocatePhysicalPages(0x280, &count, recycled) && XMemMapPhysicalPages(first, 2, recycled) == first) {
    check(first[0x10] == 0 && first[0x10000 + 0x10] == 0, "recycled physical pages read as zero");
    XMemMapPhysicalPages(first, 2, nullptr);
    XMemFreePhysicalPages(2, recycled);
  } else {
    check(false, "physical pages are allocated again after a free");
  }

  // Fresh pages read as zero, and the kernel can write into pages nobody has
  // touched yet (file reads straight into mapped physical memory).
  ULONG_PTR fresh[2] = {};
  count = 2;
  auto* third = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x20000, 0x20402000, 0x23, PAGE_READWRITE));
  if (!third || !XMemAllocatePhysicalPages(0x280, &count, fresh) || XMemMapPhysicalPages(third, 2, fresh) != third) {
    check(false, "fresh physical pages map");
    return;
  }
  check(third[0x10000 + 0x123] == 0, "fresh physical pages read as zero");
  const HANDLE file = CreateFileW(L"G:\\data\\hello.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD got = 0;
  const BOOL read = file != INVALID_HANDLE_VALUE && ReadFile(file, third, 5, &got, nullptr);
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  check(read && got == 5 && std::memcmp(third, "hello", 5) == 0,
        "files read straight into untouched physical pages");
  XMemMapPhysicalPages(third, 2, nullptr);
  XMemFreePhysicalPages(2, fresh);

  // Page numbers fit the 21 bits titles keep of `number >> 4`, also after
  // more pages than that were allocated over time (freed numbers are given
  // again once fresh ones run out). Pages get no storage until used, so
  // this costs only their records.
  std::vector<ULONG_PTR> batch(1 << 16);
  bool fit = true;
  for (int round = 0; round < 40 && fit; ++round) {
    ULONG_PTR n = batch.size();
    fit = XMemAllocatePhysicalPages(0x280, &n, batch.data()) && n == batch.size();
    for (ULONG_PTR page : batch) fit = fit && page != 0 && (page & 15) == 0 && (page >> 4) < (1u << 21);
    XMemFreePhysicalPages(batch.size(), batch.data());
  }
  check(fit, "physical page numbers stay within 21 bits as pages are allocated and freed");
}

// Fast storage: files open through the title's paths.
// The audio processor heap.
void test_audio_heap() {
  check(ApuCreateHeap(0x400000, 0x40000) == S_OK, "the audio processor heap is created");
  check(FAILED(ApuCreateHeap(0x400000, 0x40000)), "the audio processor heap is created only once");
  void* cached = nullptr;
  uint32_t apu = 0;
  check(ApuAlloc(&cached, &apu, 0x1000, 0x800, 1) == S_OK && cached &&
            (reinterpret_cast<uintptr_t>(cached) & 0x7FF) == 0 && apu != 0,
        "audio processor memory is allocated with its alignment and an audio address");
  if (!cached) return;
  static_cast<unsigned char*>(cached)[0xFFF] = 0x5A;
  check(ApuMapVirtualAddress(cached) == apu && ApuMapApuAddress(apu) == cached &&
            ApuMapVirtualAddress(static_cast<char*>(cached) + 0x10) == apu + 0x10,
        "audio addresses and virtual addresses map both ways");
  check(ApuIsVirtualAddressValid(cached, 0x1000) && !ApuIsVirtualAddressValid(cached, 0x1001),
        "audio memory is valid within its allocation only");
  void* non_cached = nullptr;
  check(ApuAlloc(&non_cached, nullptr, 0x30000, 0x800, 2) == S_OK && non_cached,
        "non-cached audio memory is allocated");
  void* too_much = nullptr;
  check(ApuAlloc(&too_much, nullptr, 0x20000, 0x800, 2) == E_OUTOFMEMORY && !too_much,
        "an exhausted audio heap refuses allocations");
  check(ApuAlloc(&too_much, nullptr, 0x100, 0x800, 3) == E_INVALIDARG, "unknown audio heap flags are refused");
  check(ApuFree(cached) == S_OK && !ApuIsVirtualAddressValid(cached, 1) && ApuFree(cached) == E_INVALIDARG,
        "audio memory is freed once");
  void* again = nullptr;
  check(ApuAlloc(&again, nullptr, 0x1000, 0x800, 1) == S_OK && again == cached, "freed audio memory is reused");
  ApuFree(again);
  ApuFree(non_cached);
}

// The audio processors, as titles start them (a silent device on the host).
void test_audio_processors() {
  struct {
    uint32_t counts[6];
    void* contexts[6];
  } shapes{{0, 0, 0, 0, 3, 0}, {}};
  check(AcpHalAllocateShapeContexts(&shapes) == S_OK && shapes.contexts[4] && !shapes.contexts[0],
        "shape contexts are allocated for the kinds the title asks for");
  void* processor = nullptr;
  check(AcpHalCreate(&processor) == S_OK && processor, "the audio control processor is created");
  if (processor) {
    using InitFn = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, uint32_t);
    using SubmitFn = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
    void** vtable = *static_cast<void***>(processor);
    const uint32_t payload = 0xC0;
    check(reinterpret_cast<InitFn>(vtable[0])(processor, 0x1000, 0x1000) == S_OK &&
              reinterpret_cast<SubmitFn>(vtable[2])(processor, 2, 0xF0F0F0F0, 0xFFFFFFFF, &payload, 0) == S_OK,
          "the control processor starts and accepts commands");
  }
  const uint32_t config[6] = {1, 0x100, 2, 0x4000, 0, 1};
  void* memory = nullptr;
  void* apu = nullptr;
  check(XApuConnect(config, &memory, &apu) == S_OK && apu && memory &&
            static_cast<volatile uint8_t*>(memory)[0x3FFF] == 0,
        "the audio processor connects with zeroed memory of the size asked for");
  const uint32_t command[16] = {1};
  check(apu && XApuEnqueueCommand(apu, command, 0) == S_OK && XApuGetResultCount(apu, 0) == 0,
        "the audio processor accepts commands");
  static uint8_t dsp_memory[0x1000];
  void* dsp = nullptr;
  check(XDspConnectWithMaximumStreamLimit(dsp_memory, sizeof(dsp_memory), 30, 0x80, 1, &dsp) == S_OK && dsp,
        "the audio DSP connects (titles require S_OK)");
  check(dsp && XDspActivate(dsp, nullptr, nullptr, nullptr) == S_OK && XDspSubmitCommand(dsp, command) == S_OK &&
            XDspDeactivate(dsp) == S_OK && XDspDisconnect(dsp) == S_OK,
        "the audio DSP activates, accepts commands and disconnects");
  if (apu) XApuDisconnect(apu);
  AcpHalReleaseShapeContexts();
}

void test_fast_storage() {
  constexpr GUID kFactory = {0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
  void* factory = nullptr;
  check(DStorageGetFactory(kFactory, &factory) == S_OK && factory, "the fast-storage factory is available");
  if (!factory) return;
  using OpenFileFn = HRESULT(STDMETHODCALLTYPE*)(void*, const wchar_t*, REFIID, void**);
  const auto open = reinterpret_cast<OpenFileFn>((*static_cast<void***>(factory))[4]);
  void* file = nullptr;
  check(open(factory, L"G:\\data\\hello.txt", kFactory, &file) == S_OK && file, "fast storage opens package files");
  void* missing = nullptr;
  check(open(factory, L"G:\\data\\missing.bin", kFactory, &missing) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        "fast storage reports missing files");

  // Status array (slot 5) and queue (slot 10); requests (queue slot 3) read a
  // file range, or inflate a zlib stream from memory, into the destination;
  // a status entry (queue slot 4) completes with the requests' result.
  using CreateStatusFn = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, const char*, REFIID, void**);
  using CreateQueueFn = HRESULT(STDMETHODCALLTYPE*)(void*, const void*, REFIID, void**);
  void** table = *static_cast<void***>(factory);
  void* status = nullptr;
  void* queue = nullptr;
  uint32_t queue_desc[10] = {};
  reinterpret_cast<CreateStatusFn>(table[5])(factory, 2, "sample", kFactory, &status);
  reinterpret_cast<CreateQueueFn>(table[10])(factory, queue_desc, kFactory, &queue);
  check(status && queue, "fast storage creates status arrays and queues");
  if (!status || !queue || !file) return;
  void** q = *static_cast<void***>(queue);
  void** st = *static_cast<void***>(status);
  using EnqueueFn = HRESULT(STDMETHODCALLTYPE*)(void*, const uint8_t*);
  using StatusFn = void(STDMETHODCALLTYPE*)(void*, void*, uint32_t);
  using CompleteFn = bool(STDMETHODCALLTYPE*)(void*, uint32_t);
  using ResultFn = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t);
  auto request = [](uint64_t options, void* destination, uint32_t destination_size, const void* source,
                    uint64_t offset, uint32_t source_size) {
    std::vector<uint8_t> r(0x40, 0);
    std::memcpy(r.data(), &options, 8);
    std::memcpy(r.data() + 8, &destination, 8);
    std::memcpy(r.data() + 0x18, &destination_size, 4);
    std::memcpy(r.data() + 0x20, &source, 8);
    std::memcpy(r.data() + 0x28, &offset, 8);
    std::memcpy(r.data() + 0x30, &source_size, 4);
    return r;
  };
  using SubmitFn = void(STDMETHODCALLTYPE*)(void*);
  auto wait_for = [&](uint32_t index) {
    for (int i = 0; i < 2000 && !reinterpret_cast<CompleteFn>(st[3])(status, index); ++i) Sleep(1);
    return reinterpret_cast<CompleteFn>(st[3])(status, index);
  };
  // Nothing happens before the queue is submitted (titles map destination
  // memory in between): the destination is untouched and the status pending.
  char text[5] = {};
  const std::vector<uint8_t> from_file = request(0, text, 4, file, 6, 4);  // "from"
  reinterpret_cast<EnqueueFn>(q[3])(queue, from_file.data());
  reinterpret_cast<StatusFn>(q[4])(queue, status, 0);
  Sleep(20);
  check(!reinterpret_cast<CompleteFn>(st[3])(status, 0) && reinterpret_cast<ResultFn>(st[4])(status, 0) == E_PENDING &&
            text[0] == 0,
        "fast storage carries nothing out before the queue is submitted");
  reinterpret_cast<SubmitFn>(q[6])(queue);
  check(wait_for(0) && reinterpret_cast<ResultFn>(st[4])(status, 0) == S_OK && std::memcmp(text, "from", 4) == 0,
        "fast storage reads file ranges and completes status entries");
  // zlib stream (one stored block) holding "packed".
  const uint8_t packed[] = {0x78, 0x01, 0x01, 0x06, 0x00, 0xF9, 0xFF, 'p', 'a', 'c', 'k', 'e', 'd',
                            0x08, 0x86, 0x02, 0x69};
  char unpacked[7] = {};
  const std::vector<uint8_t> from_memory = request((1ull << 22) | 1, unpacked, 6, packed, 0, sizeof(packed));
  reinterpret_cast<EnqueueFn>(q[3])(queue, from_memory.data());
  reinterpret_cast<StatusFn>(q[4])(queue, status, 1);
  reinterpret_cast<SubmitFn>(q[6])(queue);
  check(wait_for(1) && reinterpret_cast<ResultFn>(st[4])(status, 1) == S_OK && std::memcmp(unpacked, "packed", 6) == 0,
        "fast storage inflates zlib requests");
  // A destination given as physical pages (bit 21): a list of page numbers,
  // one per 64 KiB, which titles fill in after enqueueing; the offset into
  // the first page in the field at 0x10. The bytes reach the pages (seen here
  // through the title's mapping of them), and the list stays as it was.
  ULONG_PTR pool[2] = {};
  ULONG_PTR pool_count = 2;
  auto* shown = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x20000, 0x20402000, 0x23, PAGE_READWRITE));
  const bool mapped = XMemAllocatePhysicalPages(0x280, &pool_count, pool) && shown &&
                      XMemMapPhysicalPages(shown, 2, pool) == shown;
  check(mapped, "physical pages for a fast-storage destination are mapped");
  if (mapped) {
    ULONG_PTR list[2] = {};
    std::vector<uint8_t> to_pages = request((1ull << 22) | (1ull << 21) | 1, list, 6, packed, 0, sizeof(packed));
    const uint64_t page_offset = 0xFFFD;  // "pac" at the end of the first page, "ked" in the second
    std::memcpy(to_pages.data() + 0x10, &page_offset, 8);
    reinterpret_cast<EnqueueFn>(q[3])(queue, to_pages.data());
    reinterpret_cast<StatusFn>(q[4])(queue, status, 1);
    list[0] = pool[0];
    list[1] = pool[1];
    reinterpret_cast<SubmitFn>(q[6])(queue);
    check(wait_for(1) && reinterpret_cast<ResultFn>(st[4])(status, 1) == S_OK &&
              std::memcmp(shown + 0xFFFD, "packed", 6) == 0 && list[0] == pool[0] && list[1] == pool[1],
          "fast storage writes into the physical pages a destination list names");
    XMemMapPhysicalPages(shown, 2, nullptr);
  }
  // A request in the other compression format titles use (bit 1, not zlib;
  // not implemented): its status reports the failure, and the destination is
  // left as it was rather than given the compressed bytes.
  char untouched[7] = "------";
  const uint8_t other[] = {0x30, 0x30, 0x3C, 0x3E, 0x7A, 0xDD};
  const std::vector<uint8_t> other_format = request((1ull << 22) | 2, untouched, 6, other, 0, sizeof(other));
  reinterpret_cast<EnqueueFn>(q[3])(queue, other_format.data());
  reinterpret_cast<StatusFn>(q[4])(queue, status, 0);
  reinterpret_cast<SubmitFn>(q[6])(queue);
  check(wait_for(0) && FAILED(reinterpret_cast<ResultFn>(st[4])(status, 0)) && std::memcmp(untouched, "------", 6) == 0,
        "fast storage fails requests in a compression format it does not implement");
  reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(file))[2])(file);
}

// The console input library: one gamepad (the host's controller and keyboard).
void test_input() {
  void* input = nullptr;
  check(GameInputCreate(&input) == S_OK && input != nullptr, "GameInputCreate returns the input interface");
  if (!input) return;
  using GetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void*, void**);
  using GetNextReadingFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, uint32_t, void*, void**);
  using RegisterDeviceCallbackFn =
      HRESULT(STDMETHODCALLTYPE*)(void*, void*, uint32_t, uint32_t, uint32_t, void*, void*, uint64_t*);
  void** vtable = *static_cast<void***>(input);
  void* reading = nullptr;
  const HRESULT hr = reinterpret_cast<GetCurrentReadingFn>(vtable[4])(input, 0x40000, nullptr, &reading);
  struct {
    uint32_t buttons;
    float axes[6];
  } state{0xFFFFFFFFu, {}};
  bool got_state = false;
  if (reading) {
    void** r = *static_cast<void***>(reading);
    got_state = reinterpret_cast<bool(STDMETHODCALLTYPE*)(void*, void*)>(r[22])(reading, &state);
  }
  check(hr == S_OK && reading && got_state && state.buttons != 0xFFFFFFFFu,
        "the gamepad has a current reading with its state");
  void* keyboard = reinterpret_cast<void*>(1);
  check(reinterpret_cast<GetCurrentReadingFn>(vtable[4])(input, 0x10, nullptr, &keyboard) ==
                static_cast<HRESULT>(0x838A0003) &&
            keyboard == nullptr,
        "there is no keyboard reading");
  if (reading) {
    void* next = nullptr;
    const HRESULT later = reinterpret_cast<GetNextReadingFn>(vtable[5])(input, reading, 0x40000, nullptr, &next);
    check(later == S_OK || later == static_cast<HRESULT>(0x838A0003), "next readings are asked for after a reading");
    if (next) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(next))[2])(next);
    reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(reading))[2])(reading);
  }
  static int s_device_callbacks = 0;
  static void* s_device = nullptr;
  uint64_t token = 0;
  const auto callback = +[](uint64_t, void*, void* device, uint64_t, uint32_t status, uint32_t) {
    ++s_device_callbacks;
    if (status & 1) s_device = device;
  };
  const HRESULT registered = reinterpret_cast<RegisterDeviceCallbackFn>(vtable[9])(
      input, nullptr, 0x40000, 1, 2, nullptr, reinterpret_cast<void*>(callback), &token);
  uint32_t supported = 0;
  if (s_device) {
    const auto* info = reinterpret_cast<const uint8_t*(STDMETHODCALLTYPE*)(void*)>((*static_cast<void***>(s_device))[3])(
        s_device);
    if (info) std::memcpy(&supported, info + 104, 4);  // supportedInput
  }
  check(registered == S_OK && token != 0 && s_device_callbacks == 1 && (supported & 0x40000),
        "blocking device enumeration reports one connected gamepad");
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
  test_cpu_compatibility();
  test_package_files();
  test_package_is_read_only();
  test_scratch_storage();
  test_library_loading();
  test_lifecycle();
  test_timer_resolution();
  test_console_identity();
  test_virtual_memory();
  g_expect_online = command_line && std::strstr(command_line, "--expect-online");
  g_expect_local_user = command_line && std::strstr(command_line, "--expect-local-user");
  test_game_runtime();
  test_input();
  test_physical_pages();
  test_deferred_commit();
  test_fast_storage();
  test_audio_heap();
  test_audio_processors();
  test_texture_layout();
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
