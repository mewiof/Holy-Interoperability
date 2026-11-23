// Reader for the game configuration manifest found at the root of a title
// package. Only the fields the launcher needs are extracted.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace holy::package {

inline constexpr wchar_t kManifestFileName[] = L"MicrosoftGame.config";

struct Executable {
  std::string name;  // file name relative to the package root
  std::string id;
  std::string target_device_family;  // e.g. "Scarlett", "XboxOne", "PC"
  bool dev_only = false;
};

struct Manifest {
  std::wstring root;  // absolute package directory (empty when parsed from text)
  std::string identity_name;
  std::string identity_publisher;
  std::string identity_version;
  std::string title_id;
  std::string store_id;
  std::vector<Executable> executables;

  // The executable a retail launch would start: the first non-dev console
  // executable, else the first non-dev one, else the first.
  const Executable* default_executable() const;

  // Stable, filesystem-safe key for per-title host data. Derived from ids so
  // that it does not spell out what the title is.
  std::string storage_key() const;
};

std::optional<Manifest> parse(std::string_view xml_text, std::string* error = nullptr);
std::optional<Manifest> load(const std::wstring& package_dir, std::string* error = nullptr);

// True for device families whose executables are console builds.
bool is_console_family(std::string_view target_device_family);

}  // namespace holy::package
