#include <windows.h>
#include <intrin.h>

#include <cstring>

#include "core/common/boot_block.h"
#include "core/common/log.h"
#include "core/common/paths.h"
#include "core/common/strings.h"
#include "core/runtime/api.h"
#include "core/runtime/binder.h"
#include "core/runtime/cpu_compat.h"
#include "core/runtime/crash.h"
#include "core/runtime/hooks.h"
#include "core/runtime/memory.h"
#include "core/runtime/state.h"
#include "core/runtime/stats.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "runtime";

HMODULE g_self = nullptr;
BootBlock g_boot{};

IMAGE_NT_HEADERS64* nt_headers(uint8_t* image_base) {
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image_base);
  return reinterpret_cast<IMAGE_NT_HEADERS64*>(image_base + dos->e_lfanew);
}

// The launcher places the boot block directly in front of the import table it
// redirected the title to. Anything else means this DLL was loaded by a tool
// and stays inert.
const BootBlock* locate_boot_block(uint8_t* image_base) {
  const DWORD import_rva =
      nt_headers(image_base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (import_rva < kBootDescriptorOffset) return nullptr;
  const uint8_t* candidate = image_base + import_rva - kBootDescriptorOffset;

  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(candidate, &info, sizeof(info)) || info.State != MEM_COMMIT) return nullptr;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return nullptr;
  const uint8_t* region_end = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  if (candidate + sizeof(BootBlock) > region_end) return nullptr;

  const auto* block = reinterpret_cast<const BootBlock*>(candidate);
  if (block->magic != kBootMagic || block->version != kBootVersion ||
      block->struct_size != sizeof(BootBlock)) {
    return nullptr;
  }
  return block;
}

// Once bound, the headers get their original directories back so the image
// looks the way it does on disk to the title and to debuggers.
void restore_directories(uint8_t* image_base, const BootBlock& block) {
  IMAGE_DATA_DIRECTORY* directories = nt_headers(image_base)->OptionalHeader.DataDirectory;
  DWORD old_protection = 0;
  const SIZE_T span = sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  if (!VirtualProtect(directories, span, PAGE_READWRITE, &old_protection)) return;
  directories[IMAGE_DIRECTORY_ENTRY_IMPORT] = {block.original_import_rva, block.original_import_size};
  directories[IMAGE_DIRECTORY_ENTRY_IAT] = {block.original_iat_rva, block.original_iat_size};
  VirtualProtect(directories, span, old_protection, &old_protection);
}

void boot() {
  auto* image_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const BootBlock* block = locate_boot_block(image_base);
  if (!block) return;
  g_boot = *block;
  g_boot.package_dir[std::size(g_boot.package_dir) - 1] = L'\0';
  g_boot.data_dir[std::size(g_boot.data_dir) - 1] = L'\0';
  g_boot.trace_filter[std::size(g_boot.trace_filter) - 1] = L'\0';

  State& s = state();
  s.active = true;
  s.title_base = image_base;
  s.title_size = nt_headers(image_base)->OptionalHeader.SizeOfImage;
  s.trap_unimplemented = (g_boot.flags & kBootTrapUnimplemented) != 0;
  s.offline = (g_boot.flags & kBootOffline) != 0;
  s.console_model = g_boot.console_model;
  s.runtime_dir = parent_dir(module_path(g_self));
  s.providers.libs_dir = join_path(s.runtime_dir, L"libs");
  s.providers.package_dir = g_boot.package_dir;
  s.vfs = vfs::make_default_config(g_boot.package_dir, g_boot.data_dir);
  s.trace_patterns = split(narrow(g_boot.trace_filter), ',');

  log::set_level(static_cast<log::Level>(g_boot.log_level));
  const bool have_log_file = log::open_file(runtime_log_path(g_boot.data_dir));
  LOG_INFO(kComponent, "Holy Interoperability runtime starting (%s, %s, unimplemented imports %s)",
           (g_boot.flags & kBootSandboxed) ? "sandboxed" : "not sandboxed",
           s.offline ? "offline" : "host network", s.trap_unimplemented ? "trap" : "continue");
  if (!have_log_file) LOG_WARN(kComponent, "cannot create the runtime log file in the data directory");

  // Libraries shipped in the package link against console system libraries by
  // name; this is where the OS loader finds this project's implementations.
  SetDllDirectoryW(s.providers.libs_dir.c_str());
  AddDllDirectory(s.providers.libs_dir.c_str());

  install_crash_handlers();
  install_commit_on_demand();
  start_stats_reports(10000);
  install_cpu_compatibility();
  bind_title_imports(image_base, s.title_size, {g_boot.original_import_rva, g_boot.original_import_size});
  restore_directories(image_base, g_boot);
  LOG_INFO(kComponent, "imports bound; handing control to the title");
}

}  // namespace
}  // namespace holy::rt

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    holy::rt::g_self = module;
    holy::rt::boot();
  }
  return TRUE;
}

extern "C" {

void HolyRuntimeAnchor(void) {}

int HolyLogEnabled(uint32_t level) {
  return holy::log::enabled(static_cast<holy::log::Level>(level)) ? 1 : 0;
}

void HolyLogV(uint32_t level, const char* component, const char* fmt, va_list args) {
  holy::log::vwrite(static_cast<holy::log::Level>(level), component, fmt, args);
}

void HolyLog(uint32_t level, const char* component, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  holy::log::vwrite(static_cast<holy::log::Level>(level), component, fmt, args);
  va_end(args);
}

void HolyUnimplemented(const char* library, const char* symbol) {
  LOG_ERROR("import", "unimplemented %s!%s (called from %s)", library ? library : "?", symbol ? symbol : "?",
            holy::rt::describe_address(_ReturnAddress()).c_str());
  if (holy::rt::state().active && holy::rt::state().trap_unimplemented) {
    holy::rt::fatal_exit(holy::kExitUnimplementedImport);
  }
}

void HolyUnimplementedFrom(const char* library, const char* symbol, const void* caller) {
  LOG_ERROR("import", "unimplemented %s!%s (called from %s)", library ? library : "?", symbol ? symbol : "?",
            holy::rt::describe_address(caller).c_str());
  if (holy::rt::state().active && holy::rt::state().trap_unimplemented) {
    holy::rt::fatal_exit(holy::kExitUnimplementedImport);
  }
}

void HolyDescribeAddress(const void* address, char* buffer, size_t size) {
  if (!buffer || size == 0) return;
  const std::string text = holy::rt::describe_address(address);
  const size_t count = text.size() < size - 1 ? text.size() : size - 1;
  std::memcpy(buffer, text.data(), count);
  buffer[count] = '\0';
}

void HolyCommitOnDemand(void* base, size_t size, uint32_t protect) {
  holy::rt::register_commit_on_demand(base, size, protect);
}

void HolyCommitOnDemandReleased(void* base, size_t size) {
  holy::rt::note_release(base, size);
}

void HolySetBackingHook(HolyBackingHook hook) {
  holy::rt::set_backing_hook(hook);
}

int HolyEnsureCommitted(const void* address, size_t size) {
  return holy::rt::ensure_committed(address, size) ? 1 : 0;
}

size_t HolyTranslatePath(const wchar_t* title_path, wchar_t* out, size_t count) {
  if (!title_path || !out || count == 0) return 0;
  const std::wstring host = holy::vfs::translate(holy::rt::state().vfs, title_path);
  if (host.size() + 1 > count) return 0;
  std::memcpy(out, host.c_str(), (host.size() + 1) * sizeof(wchar_t));
  return host.size();
}

const wchar_t* HolyPackageDir(void) {
  return holy::rt::g_boot.package_dir;
}
const wchar_t* HolyDataDir(void) {
  return holy::rt::g_boot.data_dir;
}

int HolyIsOffline(void) {
  return holy::rt::state().offline ? 1 : 0;
}

volatile int64_t* HolyCounter(const char* name) {
  return holy::rt::counter(name);
}

void HolyLogStats(const char* reason) {
  holy::rt::log_stats(reason ? reason : "requested");
}

int HolyHasOverride(const char* library, const char* symbol) {
  return (library && symbol && holy::rt::find_override(library, symbol)) ? 1 : 0;
}

}  // extern "C"
