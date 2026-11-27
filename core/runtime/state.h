#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common/provider.h"
#include "core/common/vfs.h"

namespace holy::rt {

struct State {
  bool active = false;  // true once booted inside a title process
  uint8_t* title_base = nullptr;
  uint32_t title_size = 0;
  bool trap_unimplemented = true;
  bool offline = true;  // network switch: offline unless the launcher allows the host network
  bool local_user = false;  // a local user is present (signed in to no service)
  uint32_t console_model = 0;  // holy::ConsoleModel value reported to the title
  std::wstring runtime_dir;
  ProviderContext providers;
  vfs::Config vfs;
  std::vector<std::string> trace_patterns;
};

State& state();

// "title+0x1234", "module.dll+0x1234" or a bare address. The title image is
// never referred to by its file name.
std::string describe_address(const void* address);

// Redacted, UTF-8 form of a host path for log output.
std::string log_path(const std::wstring& host_path);

[[noreturn]] void fatal_exit(uint32_t exit_code);

}  // namespace holy::rt
