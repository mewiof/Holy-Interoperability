#include "core/analysis/import_report.h"

#include <windows.h>

#include <unordered_set>

#include "core/common/strings.h"
#include "core/pe/mapped_file.h"

namespace holy::analysis {
namespace {

struct ExportSet {
  bool loaded = false;
  std::unordered_set<std::string> names;
  std::unordered_set<uint32_t> ordinals;
};

ExportSet read_exports(const std::wstring& path) {
  ExportSet set;
  auto file = pe::MappedFile::open(path);
  if (!file) return set;
  const auto image = pe::Image::parse(file->bytes(), pe::Layout::File);
  if (!image) return set;
  set.loaded = true;
  for (const pe::ExportSymbol& symbol : image->exports()) {
    if (!symbol.name.empty()) set.names.insert(symbol.name);
    set.ordinals.insert(symbol.ordinal);
  }
  return set;
}

struct HostFallback {
  const char* prefix;
  const wchar_t* library;
};

// Extension API sets are optional per Windows edition. Where a host lacks one,
// the classic DLL that implements the same functions is used instead.
constexpr HostFallback kHostFallbacks[] = {
    {"ext-ms-win-rtcore-ntuser-", L"user32.dll"},
    {"ext-ms-win-ntuser-", L"user32.dll"},
    {"ext-ms-win-rtcore-gdi-", L"gdi32.dll"},
};

}  // namespace

void* load_host_library(std::string_view import_name) {
  if (!is_plain_library_name(import_name)) return nullptr;
  const UINT previous_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  HMODULE module = LoadLibraryExW(widen(import_name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) {
    for (const HostFallback& fallback : kHostFallbacks) {
      if (istarts_with(import_name, fallback.prefix)) {
        module = LoadLibraryExW(fallback.library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        break;
      }
    }
  }
  SetErrorMode(previous_mode);
  return module;
}

ImageReport check_imports(const pe::Image& image, const ProviderContext& context,
                          const OverrideQuery& has_override) {
  ImageReport report;
  std::vector<pe::ImportLibrary> libraries = image.imports();
  for (pe::ImportLibrary& delayed : image.delay_imports()) libraries.push_back(std::move(delayed));

  for (const pe::ImportLibrary& library : libraries) {
    LibraryReport entry;
    entry.name = library.name;
    entry.delay_loaded = library.delay_loaded;
    entry.total = library.symbols.size();

    const Provider provider = locate_provider(context, library.name);
    entry.kind = provider.kind;

    ExportSet exports;
    HMODULE host = nullptr;
    if (provider.kind == ProviderKind::Runtime || provider.kind == ProviderKind::Package) {
      exports = read_exports(provider.path);
      entry.found = exports.loaded;
    } else if (provider.kind == ProviderKind::Host) {
      host = static_cast<HMODULE>(load_host_library(library.name));
      entry.found = host != nullptr;
    }

    for (const pe::ImportSymbol& symbol : library.symbols) {
      bool ok = false;
      if (provider.kind == ProviderKind::Host) {
        if (host) {
          ok = symbol.by_ordinal ? GetProcAddress(host, MAKEINTRESOURCEA(symbol.ordinal)) != nullptr
                                 : GetProcAddress(host, symbol.name.c_str()) != nullptr;
        }
        if (!ok && !symbol.by_ordinal && has_override) ok = has_override(library.name, symbol.name);
      } else if (exports.loaded) {
        ok = symbol.by_ordinal ? exports.ordinals.count(symbol.ordinal) != 0
                               : exports.names.count(symbol.name) != 0;
        if (ok && !symbol.by_ordinal && provider.kind == ProviderKind::Runtime &&
            exports.names.count(kPlaceholderPrefix + symbol.name) != 0) {
          ok = false;
        }
      }
      if (ok) {
        ++entry.resolved;
      } else {
        entry.unresolved.push_back(symbol.display_name());
      }
    }

    report.total += entry.total;
    report.resolved += entry.resolved;
    if (entry.kind == ProviderKind::Runtime || entry.kind == ProviderKind::Missing) {
      report.console_total += entry.total;
      report.console_resolved += entry.resolved;
    }
    report.libraries.push_back(std::move(entry));
  }
  return report;
}

}  // namespace holy::analysis
