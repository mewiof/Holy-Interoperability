// Path model of a title process.
//
// A console title sees its package mounted read-only on a fixed drive and gets
// a few writable scratch volumes. On the host the package is an ordinary
// directory that must never be written to, and the scratch volumes are
// directories under the per-title data directory.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace holy::vfs {

struct Mount {
  wchar_t drive = 0;    // upper-case drive letter as the title spells it
  std::wstring target;  // absolute host directory
  bool read_only = false;
};

struct Config {
  std::wstring package_root;  // absolute host path of the package, no trailing separator
  std::wstring data_root;     // absolute host path of the per-title data directory
  std::vector<Mount> mounts;
};

// Standard layout: package on G:, temporary storage on T:, scratch on D:.
Config make_default_config(const std::wstring& package_root, const std::wstring& data_root);

// Rewrites a title path that starts with a mounted drive to the host path.
// Everything else (relative paths, UNC, device paths) is returned unchanged.
std::wstring translate(const Config& config, std::wstring_view path);

// True if `host_path` (absolute or relative to the current directory) resolves
// to `root` or to something below it.
bool is_inside(const std::wstring& root, const std::wstring& host_path);

// Replaces the package and data roots with placeholders so that log lines do
// not reveal where (or what) the package is.
std::wstring redact(const Config& config, std::wstring_view host_path);

}  // namespace holy::vfs
