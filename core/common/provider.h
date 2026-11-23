// Decides which file satisfies an import library name.
//
// Shared by the static inspector and the in-process binder so that what
// `holy inspect` reports is exactly what `holy run` will do.
#pragma once

#include <string>
#include <string_view>

namespace holy {

enum class ProviderKind {
  Runtime,  // clean-room reimplementation shipped with this project (libs/)
  Package,  // DLL that the title package itself ships
  Host,     // the host operating system
  Missing,  // console-only library with no implementation yet
};

const char* provider_kind_name(ProviderKind kind);

// A runtime library that exports "<prefix><symbol>" next to "<symbol>" declares
// that symbol to be a placeholder: present so dependent images load, but not
// implemented.
inline constexpr char kPlaceholderPrefix[] = "HolyPlaceholder_";

struct Provider {
  ProviderKind kind = ProviderKind::Missing;
  std::wstring path;  // full path for Runtime/Package, bare name for Host, empty for Missing
};

struct ProviderContext {
  std::wstring libs_dir;     // directory with the project's console library implementations
  std::wstring package_dir;  // root of the title package
};

// True for a bare file name: no directory components, drive or stream syntax.
// Import names come from untrusted images and are never used as paths.
bool is_plain_library_name(std::string_view name);

// File name under libs/ that implements `import_name`. Usually identical;
// differs for console libraries whose name collides with a host system DLL of
// a different ABI, which must never be shadowed on the DLL search path.
std::wstring runtime_file_name(std::string_view import_name);

// True for library names that only ever mean the console flavour, so falling
// back to a same-named host DLL would be wrong.
bool is_console_only_library(std::string_view import_name);

Provider locate_provider(const ProviderContext& context, std::string_view import_name);

}  // namespace holy
