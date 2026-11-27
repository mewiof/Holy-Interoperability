// Starts a title from its package directory.
//
// The title executable is started in place, from the package the user points
// at. Nothing is copied, patched on disk or written into the package: the only
// modification is made to the new process's memory before its first
// instruction runs (see core/common/boot_block.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common/boot_block.h"
#include "core/common/log.h"

namespace holy::loader {

enum class UnimplementedPolicy {
  Trap,      // stop at the first call into an unimplemented import (default)
  Continue,  // log it and return zero to the caller; for exploration only
};

struct LaunchOptions {
  std::wstring package_dir;
  std::wstring executable;   // optional: executable inside the package, overrides the manifest
  std::wstring runtime_dll;  // optional: defaults to holyrt.dll next to the current executable
  std::wstring data_dir;     // optional: defaults to a per-title directory under default_data_root()
  std::vector<std::wstring> title_args;

  // Run the title at low integrity level. The OS then refuses any write to the
  // package directory (and to the rest of the user's files) no matter which
  // code path attempts it.
  bool sandbox = true;

  // Offline (default): the title sees a host without network connectivity.
  bool offline = true;

  // A local user is present (a profile of this host, signed in to no
  // service); off by default: nobody is present.
  bool local_user = false;

  UnimplementedPolicy unimplemented = UnimplementedPolicy::Trap;
  ConsoleModel console_model = ConsoleModel::SeriesX;
  uint32_t timeout_ms = 0;  // 0 waits until the title exits
  log::Level runtime_log_level = log::Level::Info;
  std::wstring trace_filter;  // comma separated wildcard patterns of import libraries to trace
  // Sample the title's threads every this many milliseconds into
  // <data>/logs/profile.txt (core/loader/profiler.h); 0 does not profile.
  uint32_t profile_interval_ms = 0;
};

enum class LaunchStatus { Exited, TimedOut, Failed };

struct LaunchResult {
  LaunchStatus status = LaunchStatus::Failed;
  uint32_t exit_code = 0;
  std::string error;
  std::wstring data_dir;
  std::wstring log_path;
};

LaunchResult launch(const LaunchOptions& options);

// Root for per-title host data. Lives under the user's low-integrity
// application data folder so sandboxed titles can write to it.
std::wstring default_data_root();

}  // namespace holy::loader
