#include "core/runtime/hooks.h"

#include <windows.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "core/common/log.h"
#include "core/common/strings.h"
#include "core/runtime/binder.h"
#include "core/runtime/crash.h"
#include "core/runtime/memory.h"
#include "core/runtime/network.h"
#include "core/runtime/state.h"

namespace holy::rt {
namespace {

// --- helpers ----------------------------------------------------------------

std::wstring from_ansi(const char* text) {
  if (!text || !*text) return {};
  const int needed = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
  if (needed <= 1) return {};
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), needed);
  out.resize(static_cast<size_t>(needed) - 1);
  return out;
}

std::wstring host_path(const wchar_t* title_path) {
  return vfs::translate(state().vfs, title_path);
}

bool in_package(const std::wstring& path) {
  return vfs::is_inside(state().vfs.package_root, path);
}

// The package is read-only on the console and must stay untouched on the host.
bool deny_package_write(const std::wstring& path, const char* operation) {
  if (!in_package(path)) return false;
  LOG_WARN("vfs", "denied %s inside the read-only package: %s", operation, log_path(path).c_str());
  SetLastError(ERROR_ACCESS_DENIED);
  return true;
}

// --- library loading --------------------------------------------------------

HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE, DWORD flags) {
  return load_library_for_title(name, flags);
}

HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE, DWORD flags) {
  if (!name) return load_library_for_title(nullptr, flags);
  return load_library_for_title(from_ansi(name).c_str(), flags);
}

HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name) {
  return load_library_for_title(name, 0);
}

HMODULE WINAPI hook_LoadLibraryA(LPCSTR name) {
  return hook_LoadLibraryExA(name, nullptr, 0);
}

HMODULE WINAPI hook_GetModuleHandleW(LPCWSTR name) {
  return module_handle_for_title(name);
}

HMODULE WINAPI hook_GetModuleHandleA(LPCSTR name) {
  if (!name) return module_handle_for_title(nullptr);
  return module_handle_for_title(from_ansi(name).c_str());
}

// Pass-through, but a failed lookup of a console entry point is the single
// most useful line in a bring-up log, so lookups are recorded.
FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name) {
  const FARPROC result = GetProcAddress(module, name);
  const DWORD error = GetLastError();
  const bool by_ordinal = (reinterpret_cast<uintptr_t>(name) >> 16) == 0;
  const std::string where = describe_address(module);
  if (by_ordinal) {
    HOLY_LOG(result ? log::Level::Debug : log::Level::Warn, "loader", "GetProcAddress(%s, #%u) -> %s",
             where.c_str(), static_cast<unsigned>(reinterpret_cast<uintptr_t>(name)),
             result ? "found" : "MISSING");
  } else {
    HOLY_LOG(result ? log::Level::Debug : log::Level::Warn, "loader", "GetProcAddress(%s, %s) -> %s",
             where.c_str(), name, result ? "found" : "MISSING");
  }
  SetLastError(error);
  return result;
}

// --- files ------------------------------------------------------------------

constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER |
                               FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD |
                               FILE_WRITE_ATTRIBUTES;

HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE template_file) {
  if (!name) return CreateFileW(name, access, share, security, disposition, flags, template_file);
  const std::wstring path = host_path(name);
  if (in_package(path)) {
    const bool modifies = (access & kWriteAccess) != 0 || disposition == CREATE_NEW ||
                          disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING ||
                          (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    if (modifies) {
      deny_package_write(path, "write access");
      return INVALID_HANDLE_VALUE;
    }
    if (disposition == OPEN_ALWAYS) disposition = OPEN_EXISTING;
    // Reading must never lock other readers out of the user's package.
    share |= FILE_SHARE_READ;
  }
  const HANDLE handle = CreateFileW(path.c_str(), access, share, security, disposition, flags, template_file);
  if (log::enabled(log::Level::Debug)) {
    const DWORD error = GetLastError();
    if (handle == INVALID_HANDLE_VALUE) {
      LOG_DEBUG("vfs", "open %s -> error %lu", log_path(path).c_str(), error);
    } else {
      LOG_DEBUG("vfs", "open %s", log_path(path).c_str());
    }
    SetLastError(error);
  }
  return handle;
}

HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE template_file) {
  if (!name) return CreateFileA(name, access, share, security, disposition, flags, template_file);
  return hook_CreateFileW(from_ansi(name).c_str(), access, share, security, disposition, flags,
                          template_file);
}

DWORD WINAPI hook_GetFileAttributesW(LPCWSTR name) {
  if (!name) return GetFileAttributesW(name);
  return GetFileAttributesW(host_path(name).c_str());
}

DWORD WINAPI hook_GetFileAttributesA(LPCSTR name) {
  if (!name) return GetFileAttributesA(name);
  return GetFileAttributesW(host_path(from_ansi(name).c_str()).c_str());
}

BOOL WINAPI hook_GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
  if (!name) return GetFileAttributesExW(name, level, info);
  return GetFileAttributesExW(host_path(name).c_str(), level, info);
}

BOOL WINAPI hook_SetFileAttributesW(LPCWSTR name, DWORD attributes) {
  if (!name) return SetFileAttributesW(name, attributes);
  const std::wstring path = host_path(name);
  if (deny_package_write(path, "attribute change")) return FALSE;
  return SetFileAttributesW(path.c_str(), attributes);
}

HANDLE WINAPI hook_FindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
  if (!name) return FindFirstFileW(name, data);
  return FindFirstFileW(host_path(name).c_str(), data);
}

HANDLE WINAPI hook_FindFirstFileExW(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                                    FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
  if (!name) return FindFirstFileExW(name, level, data, search, filter, flags);
  return FindFirstFileExW(host_path(name).c_str(), level, data, search, filter, flags);
}

BOOL WINAPI hook_DeleteFileW(LPCWSTR name) {
  if (!name) return DeleteFileW(name);
  const std::wstring path = host_path(name);
  if (deny_package_write(path, "delete")) return FALSE;
  return DeleteFileW(path.c_str());
}

BOOL WINAPI hook_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security) {
  if (!name) return CreateDirectoryW(name, security);
  const std::wstring path = host_path(name);
  if (deny_package_write(path, "directory creation")) return FALSE;
  return CreateDirectoryW(path.c_str(), security);
}

BOOL WINAPI hook_RemoveDirectoryW(LPCWSTR name) {
  if (!name) return RemoveDirectoryW(name);
  const std::wstring path = host_path(name);
  if (deny_package_write(path, "directory removal")) return FALSE;
  return RemoveDirectoryW(path.c_str());
}

BOOL WINAPI hook_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  if (!from) return MoveFileExW(from, to, flags);
  const std::wstring source = host_path(from);
  if (deny_package_write(source, "move")) return FALSE;
  if (!to) return MoveFileExW(source.c_str(), nullptr, flags);
  const std::wstring target = host_path(to);
  if (deny_package_write(target, "move")) return FALSE;
  return MoveFileExW(source.c_str(), target.c_str(), flags);
}

// The kernel reports a read into title memory that is committed on demand as
// an error rather than faulting it in, so the buffer is committed first.
BOOL WINAPI hook_ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) {
  const DWORD error = GetLastError();
  ensure_committed(buffer, size);
  SetLastError(error);
  return ReadFile(file, buffer, size, read, overlapped);
}

BOOL WINAPI hook_GetDiskFreeSpaceExW(LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total,
                                     PULARGE_INTEGER total_free) {
  if (!directory) return GetDiskFreeSpaceExW(directory, available, total, total_free);
  return GetDiskFreeSpaceExW(host_path(directory).c_str(), available, total, total_free);
}

// --- C runtime file functions -----------------------------------------------
//
// The title's C runtime is the host's universal CRT, a different instance from
// the one linked into this DLL, so its functions are looked up at run time and
// its FILE objects are only ever passed through.

FARPROC ucrt_export(const char* name) {
  static const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
  return ucrt ? GetProcAddress(ucrt, name) : nullptr;
}

void set_title_errno(int value) {
  using ErrnoFn = int*(__cdecl*)();
  if (const auto fn = reinterpret_cast<ErrnoFn>(ucrt_export("_errno"))) *fn() = value;
}

bool mode_writes(const wchar_t* mode) {
  for (; mode && *mode; ++mode) {
    if (*mode == L'w' || *mode == L'a' || *mode == L'+') return true;
  }
  return false;
}

void* open_stream(const std::wstring& title_path, const std::wstring& mode) {
  using WfopenFn = void*(__cdecl*)(const wchar_t*, const wchar_t*);
  const auto real = reinterpret_cast<WfopenFn>(ucrt_export("_wfopen"));
  if (!real) return nullptr;
  const std::wstring path = host_path(title_path.c_str());
  if (mode_writes(mode.c_str()) && deny_package_write(path, "stream write")) {
    set_title_errno(EACCES);
    return nullptr;
  }
  void* stream = real(path.c_str(), mode.c_str());
  LOG_DEBUG("vfs", "fopen %s (%s)%s", log_path(path).c_str(), narrow(mode).c_str(),
            stream ? "" : " -> failed");
  return stream;
}

void* __cdecl hook_fopen(const char* path, const char* mode) {
  if (!path || !mode) {
    set_title_errno(EINVAL);
    return nullptr;
  }
  return open_stream(from_ansi(path), from_ansi(mode));
}

void* __cdecl hook__wfopen(const wchar_t* path, const wchar_t* mode) {
  if (!path || !mode) {
    set_title_errno(EINVAL);
    return nullptr;
  }
  return open_stream(path, mode);
}

int __cdecl hook__wstat64(const wchar_t* path, void* buffer) {
  using StatFn = int(__cdecl*)(const wchar_t*, void*);
  const auto real = reinterpret_cast<StatFn>(ucrt_export("_wstat64"));
  if (!real || !path) {
    set_title_errno(EINVAL);
    return -1;
  }
  return real(host_path(path).c_str(), buffer);
}

int __cdecl hook_remove(const char* path) {
  using WremoveFn = int(__cdecl*)(const wchar_t*);
  const auto real = reinterpret_cast<WremoveFn>(ucrt_export("_wremove"));
  if (!real || !path) {
    set_title_errno(EINVAL);
    return -1;
  }
  const std::wstring translated = host_path(from_ansi(path).c_str());
  if (deny_package_write(translated, "delete")) {
    set_title_errno(EACCES);
    return -1;
  }
  return real(translated.c_str());
}

// --- application lifecycle ---------------------------------------------------
//
// A console title registers for suspend/resume and constrained-mode
// notifications. A desktop process is never suspended by a lifecycle manager,
// so registrations succeed and the callbacks simply never fire.

struct LifecycleRegistration {
  void* routine;
  void* context;
};

ULONG NTAPI hook_RegisterAppStateChangeNotification(void* routine, void* context, void** registration) {
  if (!routine || !registration) return ERROR_INVALID_PARAMETER;
  *registration = new LifecycleRegistration{routine, context};
  LOG_DEBUG("plm", "app state change notification registered");
  return ERROR_SUCCESS;
}

void NTAPI hook_UnregisterAppStateChangeNotification(void* registration) {
  delete static_cast<LifecycleRegistration*>(registration);
}

ULONG NTAPI hook_RegisterAppConstrainedChangeNotification(void* routine, void* context, void** registration) {
  if (!routine || !registration) return ERROR_INVALID_PARAMETER;
  *registration = new LifecycleRegistration{routine, context};
  LOG_DEBUG("plm", "app constrained change notification registered");
  return ERROR_SUCCESS;
}

void NTAPI hook_UnregisterAppConstrainedChangeNotification(void* registration) {
  delete static_cast<LifecycleRegistration*>(registration);
}

// --- console identity ---------------------------------------------------------
//
// Platform code linked into titles identifies the console model by reading the
// ConsoleMode value under an OEM key in the machine hive; when the key is
// absent it concludes it is running on a PC. The runtime answers that one
// query from its configuration. All other registry access reaches the host.

constexpr wchar_t kOemKeyPath[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
constexpr wchar_t kConsoleModeValue[] = L"ConsoleMode";

int g_oem_key_sentinel = 0;
HKEY oem_key() {
  return reinterpret_cast<HKEY>(&g_oem_key_sentinel);
}

bool is_oem_key(HKEY root, LPCWSTR path) {
  return root == HKEY_LOCAL_MACHINE && path && iequals(std::wstring_view(path), kOemKeyPath);
}

LSTATUS WINAPI hook_RegOpenKeyW(HKEY root, LPCWSTR path, PHKEY result) {
  if (result && is_oem_key(root, path)) {
    *result = oem_key();
    return ERROR_SUCCESS;
  }
  return RegOpenKeyW(root, path, result);
}

LSTATUS WINAPI hook_RegOpenKeyExW(HKEY root, LPCWSTR path, DWORD options, REGSAM access, PHKEY result) {
  if (result && is_oem_key(root, path)) {
    *result = oem_key();
    return ERROR_SUCCESS;
  }
  return RegOpenKeyExW(root, path, options, access, result);
}

LSTATUS WINAPI hook_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                     LPDWORD size) {
  if (key != oem_key()) return RegQueryValueExW(key, name, reserved, type, data, size);
  if (!name || !iequals(std::wstring_view(name), kConsoleModeValue)) return ERROR_FILE_NOT_FOUND;
  const DWORD value = state().console_model;
  if (type) *type = REG_DWORD;
  if (!size) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
  const DWORD capacity = *size;
  *size = sizeof(value);
  if (!data) return ERROR_SUCCESS;
  if (capacity < sizeof(value)) return ERROR_MORE_DATA;
  std::memcpy(data, &value, sizeof(value));
  LOG_DEBUG("identity", "console model query answered with 0x%lX", value);
  return ERROR_SUCCESS;
}

LSTATUS WINAPI hook_RegCloseKey(HKEY key) {
  if (key == oem_key()) return ERROR_SUCCESS;
  return RegCloseKey(key);
}

// --- error handling ----------------------------------------------------------

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hook_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  return exchange_title_filter(filter);
}

#define HOLY_OVERRIDE(name)                      \
  Override {                                     \
    #name, reinterpret_cast<void*>(&hook_##name) \
  }

const Override kOverrides[] = {
    HOLY_OVERRIDE(LoadLibraryExW),
    HOLY_OVERRIDE(LoadLibraryExA),
    HOLY_OVERRIDE(LoadLibraryW),
    HOLY_OVERRIDE(LoadLibraryA),
    HOLY_OVERRIDE(GetModuleHandleW),
    HOLY_OVERRIDE(GetModuleHandleA),
    HOLY_OVERRIDE(GetProcAddress),
    HOLY_OVERRIDE(CreateFileW),
    HOLY_OVERRIDE(CreateFileA),
    HOLY_OVERRIDE(GetFileAttributesW),
    HOLY_OVERRIDE(GetFileAttributesA),
    HOLY_OVERRIDE(GetFileAttributesExW),
    HOLY_OVERRIDE(SetFileAttributesW),
    HOLY_OVERRIDE(FindFirstFileW),
    HOLY_OVERRIDE(FindFirstFileExW),
    HOLY_OVERRIDE(DeleteFileW),
    HOLY_OVERRIDE(CreateDirectoryW),
    HOLY_OVERRIDE(RemoveDirectoryW),
    HOLY_OVERRIDE(MoveFileExW),
    HOLY_OVERRIDE(GetDiskFreeSpaceExW),
    HOLY_OVERRIDE(ReadFile),
    HOLY_OVERRIDE(fopen),
    HOLY_OVERRIDE(_wfopen),
    HOLY_OVERRIDE(_wstat64),
    HOLY_OVERRIDE(remove),
    HOLY_OVERRIDE(RegisterAppStateChangeNotification),
    HOLY_OVERRIDE(UnregisterAppStateChangeNotification),
    HOLY_OVERRIDE(RegisterAppConstrainedChangeNotification),
    HOLY_OVERRIDE(UnregisterAppConstrainedChangeNotification),
    HOLY_OVERRIDE(RegOpenKeyW),
    HOLY_OVERRIDE(RegOpenKeyExW),
    HOLY_OVERRIDE(RegQueryValueExW),
    HOLY_OVERRIDE(RegCloseKey),
    HOLY_OVERRIDE(SetUnhandledExceptionFilter),
};

#undef HOLY_OVERRIDE

}  // namespace

const Override* find_override(std::string_view library, std::string_view symbol) {
  if (const Override* network = find_network_override(library, symbol)) return network;
  for (const Override& entry : kOverrides) {
    if (symbol == entry.symbol) return &entry;
  }
  return nullptr;
}

}  // namespace holy::rt
