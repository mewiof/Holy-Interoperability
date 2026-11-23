#include "core/common/provider.h"

#include "core/common/paths.h"
#include "core/common/strings.h"

namespace holy {
namespace {

struct Alias {
  const char* import_name;
  const wchar_t* file_name;
};

// Console libraries that share a file name with an unrelated host system DLL.
constexpr Alias kAliases[] = {
    {"gameinput.dll", L"holy_gameinput.dll"},
    {"xgameruntime.dll", L"holy_xgameruntime.dll"},
};

// Console-only library names. If no implementation exists they are reported as
// missing instead of being looked up on the host.
constexpr const char* kConsoleOnly[] = {
    "acphal.dll",  "chad.dll",         "d3d12_xs.dll", "d3d12_x.dll", "gameinput.dll", "xapu.dll",
    "xcrdapi.dll", "xgameruntime.dll", "xg_xs.dll",    "xg_x.dll",    "xmem.dll",
};

}  // namespace

const char* provider_kind_name(ProviderKind kind) {
  switch (kind) {
    case ProviderKind::Runtime:
      return "runtime";
    case ProviderKind::Package:
      return "package";
    case ProviderKind::Host:
      return "host";
    default:
      return "missing";
  }
}

bool is_plain_library_name(std::string_view name) {
  if (name.empty() || name.size() > 255) return false;
  if (name == "." || name == "..") return false;
  for (const char c : name) {
    if (c == '\\' || c == '/' || c == ':' || c == '\0' || static_cast<unsigned char>(c) < 0x20) return false;
  }
  return true;
}

std::wstring runtime_file_name(std::string_view import_name) {
  for (const Alias& alias : kAliases) {
    if (iequals(import_name, alias.import_name)) return alias.file_name;
  }
  return to_lower(widen(import_name));
}

bool is_console_only_library(std::string_view import_name) {
  for (const char* name : kConsoleOnly) {
    if (iequals(import_name, name)) return true;
  }
  return false;
}

Provider locate_provider(const ProviderContext& context, std::string_view import_name) {
  Provider provider;
  if (!is_plain_library_name(import_name)) return provider;

  if (!context.libs_dir.empty()) {
    const std::wstring candidate = join_path(context.libs_dir, runtime_file_name(import_name));
    if (file_exists(candidate)) {
      provider.kind = ProviderKind::Runtime;
      provider.path = candidate;
      return provider;
    }
  }
  if (is_console_only_library(import_name)) return provider;

  if (!context.package_dir.empty()) {
    const std::wstring candidate = join_path(context.package_dir, widen(import_name));
    if (file_exists(candidate)) {
      provider.kind = ProviderKind::Package;
      provider.path = candidate;
      return provider;
    }
  }
  provider.kind = ProviderKind::Host;
  provider.path = widen(import_name);
  return provider;
}

}  // namespace holy
