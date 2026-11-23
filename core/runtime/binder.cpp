#include "core/runtime/binder.h"

#include <deque>
#include <string>

#include "core/analysis/import_report.h"
#include "core/common/boot_block.h"
#include "core/common/log.h"
#include "core/common/paths.h"
#include "core/common/strings.h"
#include "core/pe/mapped_file.h"
#include "core/runtime/api.h"
#include "core/runtime/hooks.h"
#include "core/runtime/state.h"
#include "core/runtime/thunks.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "binder";

// Records back the reporting thunks and must never move or be freed.
std::deque<ImportRecord>& records() {
  static auto* storage = new std::deque<ImportRecord>();
  return *storage;
}

bool is_traced(const std::string& library) {
  for (const std::string& pattern : state().trace_patterns) {
    if (wildcard_match(pattern, library)) return true;
  }
  return false;
}

bool is_executable(const void* address) {
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
  constexpr DWORD kExecutable =
      PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (info.Protect & kExecutable) != 0;
}

// Explains why the OS loader refused a library shipped in the package: usually
// one of its own imports names a console symbol nobody provides yet.
void diagnose_package_library(const std::wstring& path) {
  auto file = pe::MappedFile::open(path);
  if (!file) return;
  const auto image = pe::Image::parse(file->bytes(), pe::Layout::File);
  if (!image) {
    LOG_ERROR(kComponent, "  the file is not a loadable x64 image");
    return;
  }
  const analysis::ImageReport report = analysis::check_imports(
      *image, state().providers, [](std::string_view library, std::string_view symbol) {
        return HolyHasOverride(std::string(library).c_str(), std::string(symbol).c_str()) != 0;
      });
  for (const analysis::LibraryReport& library : report.libraries) {
    if (!library.found) {
      LOG_ERROR(kComponent, "  needs %s (%s), which is unavailable", library.name.c_str(),
                provider_kind_name(library.kind));
      continue;
    }
    // Symbols the runtime would interpose are not available to the OS loader.
    for (const std::string& symbol : library.unresolved) {
      LOG_ERROR(kComponent, "  needs %s!%s, which is not implemented", library.name.c_str(), symbol.c_str());
    }
  }
}

HMODULE load_provider(const Provider& provider, const std::string& library_name) {
  switch (provider.kind) {
    case ProviderKind::Runtime:
    case ProviderKind::Package: {
      HMODULE module = LoadLibraryExW(provider.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!module) {
        LOG_ERROR(kComponent, "%s library %s failed to load (error %lu)", provider_kind_name(provider.kind),
                  library_name.c_str(), GetLastError());
        if (provider.kind == ProviderKind::Package) diagnose_package_library(provider.path);
      }
      return module;
    }
    case ProviderKind::Host: {
      HMODULE module = static_cast<HMODULE>(analysis::load_host_library(library_name));
      if (!module) LOG_WARN(kComponent, "the host has no library %s", library_name.c_str());
      return module;
    }
    default:
      LOG_WARN(kComponent, "console library %s is not implemented yet", library_name.c_str());
      return nullptr;
  }
}

bool is_bare_name(const std::wstring& name) {
  return name.find_first_of(L"\\/:") == std::wstring::npos;
}

std::wstring with_default_extension(std::wstring name) {
  if (name.find(L'.') == std::wstring::npos) name += L".dll";
  return name;
}

}  // namespace

BindSummary bind_title_imports(uint8_t* image_base, uint32_t image_size, pe::DataDirectory import_directory) {
  BindSummary summary;
  std::string error;
  const auto image = pe::Image::parse({image_base, image_size}, pe::Layout::Mapped, &error);
  if (!image) {
    LOG_ERROR(kComponent, "cannot parse the title image: %s", error.c_str());
    fatal_exit(kExitRuntimeFailure);
  }

  std::vector<std::string> unimplemented;
  for (const pe::ImportLibrary& library : image->imports(&import_directory)) {
    ++summary.libraries;
    const Provider provider = locate_provider(state().providers, library.name);
    const HMODULE module = load_provider(provider, library.name);
    const bool traced = is_traced(library.name);

    uint32_t low = UINT32_MAX, high = 0;
    for (const pe::ImportSymbol& symbol : library.symbols) {
      if (symbol.iat_rva < low) low = symbol.iat_rva;
      if (symbol.iat_rva > high) high = symbol.iat_rva;
    }
    if (library.symbols.empty()) continue;
    if (static_cast<uint64_t>(high) + sizeof(void*) > image_size) {
      LOG_ERROR(kComponent, "import address table of %s lies outside the image", library.name.c_str());
      fatal_exit(kExitRuntimeFailure);
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(image_base + low, high - low + sizeof(void*), PAGE_READWRITE, &old_protection)) {
      LOG_ERROR(kComponent, "cannot make the import address table writable (error %lu)", GetLastError());
      fatal_exit(kExitRuntimeFailure);
    }

    size_t bound = 0;
    for (const pe::ImportSymbol& symbol : library.symbols) {
      ++summary.symbols;
      void* target = nullptr;
      const Override* override_entry =
          (provider.kind == ProviderKind::Host && !symbol.by_ordinal) ? find_override(symbol.name) : nullptr;
      if (override_entry) {
        target = override_entry->hook;
        ++summary.overridden;
      } else if (module) {
        target = reinterpret_cast<void*>(symbol.by_ordinal
                                             ? GetProcAddress(module, MAKEINTRESOURCEA(symbol.ordinal))
                                             : GetProcAddress(module, symbol.name.c_str()));
        // Placeholder exports are not implementations; report them like any
        // other unimplemented import.
        if (target && !symbol.by_ordinal && provider.kind == ProviderKind::Runtime &&
            GetProcAddress(module, (kPlaceholderPrefix + symbol.name).c_str())) {
          target = nullptr;
        }
      }

      void* entry = target;
      const bool report = !target || (traced && is_executable(target));
      if (report) {
        ImportRecord& record = records().emplace_back();
        record.library = library.name;
        record.symbol = symbol.display_name();
        record.target = target;
        entry = make_reporting_thunk(&record);
        if (!entry) {
          LOG_ERROR(kComponent, "out of executable memory for import thunks");
          fatal_exit(kExitRuntimeFailure);
        }
      }
      if (target) {
        ++bound;
        if (!override_entry) {
          if (provider.kind == ProviderKind::Host)
            ++summary.from_host;
          else if (provider.kind == ProviderKind::Package)
            ++summary.from_package;
          else
            ++summary.from_runtime;
        }
      } else {
        ++summary.unimplemented;
        unimplemented.push_back(library.name + "!" + symbol.display_name());
      }
      *reinterpret_cast<void**>(image_base + symbol.iat_rva) = entry;
    }
    VirtualProtect(image_base + low, high - low + sizeof(void*), old_protection, &old_protection);

    LOG_DEBUG(kComponent, "%-8s %-52s %zu/%zu%s", provider_kind_name(provider.kind), library.name.c_str(),
              bound, library.symbols.size(), traced ? " (traced)" : "");
  }

  LOG_INFO(kComponent,
           "bound %zu imports from %zu libraries: %zu host, %zu package, %zu runtime, %zu interposed, %zu "
           "unimplemented",
           summary.symbols, summary.libraries, summary.from_host, summary.from_package, summary.from_runtime,
           summary.overridden, summary.unimplemented);
  for (const std::string& name : unimplemented) LOG_INFO(kComponent, "  unimplemented: %s", name.c_str());
  return summary;
}

HMODULE load_library_for_title(const wchar_t* requested, DWORD flags) {
  if (!requested) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  const std::wstring name(requested);
  if (!is_bare_name(name)) {
    const std::wstring path = vfs::translate(state().vfs, name);
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, flags);
    LOG_DEBUG("loader", "LoadLibrary(%s) -> %p", log_path(path).c_str(), static_cast<void*>(module));
    return module;
  }

  const std::string file = narrow(with_default_extension(name));
  const Provider provider = locate_provider(state().providers, file);
  if (provider.kind == ProviderKind::Runtime) {
    HMODULE module = LoadLibraryExW(provider.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module) {
      LOG_INFO("loader", "title loaded console library %s", file.c_str());
    } else {
      LOG_ERROR("loader", "console library %s failed to load (error %lu)", file.c_str(), GetLastError());
    }
    return module;
  }
  if (provider.kind == ProviderKind::Missing) {
    LOG_ERROR("loader", "title requested console library %s, which is not implemented yet", file.c_str());
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
  }

  // Package and host libraries go through the regular search (application
  // directory first). Make sure the project's libraries stay reachable for
  // their dependencies when the title restricts the search.
  constexpr DWORD kSearchFlags = LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                 LOAD_LIBRARY_SEARCH_USER_DIRS;
  if (flags & kSearchFlags) flags |= LOAD_LIBRARY_SEARCH_USER_DIRS;
  HMODULE module = LoadLibraryExW(requested, nullptr, flags);
  const DWORD load_error = GetLastError();
  LOG_DEBUG("loader", "LoadLibrary(%s) -> %p", file.c_str(), static_cast<void*>(module));
  if (!module && provider.kind == ProviderKind::Package) {
    LOG_ERROR("loader", "package library %s failed to load (error %lu)", file.c_str(), load_error);
    diagnose_package_library(provider.path);
  }
  SetLastError(load_error);
  return module;
}

HMODULE module_handle_for_title(const wchar_t* requested) {
  if (requested) {
    const std::wstring name(requested);
    if (is_bare_name(name)) {
      const std::string file = narrow(with_default_extension(name));
      const Provider provider = locate_provider(state().providers, file);
      if (provider.kind == ProviderKind::Runtime) return GetModuleHandleW(base_name(provider.path).c_str());
      if (provider.kind == ProviderKind::Missing) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
      }
    }
  }
  return GetModuleHandleW(requested);
}

}  // namespace holy::rt
