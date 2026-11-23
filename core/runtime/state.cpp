#include "core/runtime/state.h"

#include <windows.h>

#include <cstdio>

#include "core/common/log.h"
#include "core/common/paths.h"
#include "core/common/strings.h"

namespace holy::rt {

State& state() {
  static State instance;
  return instance;
}

std::string describe_address(const void* address) {
  char buffer[320];
  const State& s = state();
  const auto value = reinterpret_cast<uintptr_t>(address);
  const auto title = reinterpret_cast<uintptr_t>(s.title_base);
  if (s.title_base && value >= title && value < title + s.title_size) {
    std::snprintf(buffer, sizeof(buffer), "title+0x%llx", static_cast<unsigned long long>(value - title));
    return buffer;
  }
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          static_cast<LPCWSTR>(address), &module) &&
      module) {
    const std::string name = narrow(base_name(module_path(module)));
    std::snprintf(buffer, sizeof(buffer), "%s+0x%llx", name.c_str(),
                  static_cast<unsigned long long>(value - reinterpret_cast<uintptr_t>(module)));
    return buffer;
  }
  std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
  return buffer;
}

std::string log_path(const std::wstring& host_path) {
  return narrow(vfs::redact(state().vfs, host_path));
}

void fatal_exit(uint32_t exit_code) {
  LOG_ERROR("runtime", "terminating the title (exit code 0x%08X)", exit_code);
  log::close_file();
  TerminateProcess(GetCurrentProcess(), exit_code);
  for (;;) Sleep(INFINITE);
}

}  // namespace holy::rt
