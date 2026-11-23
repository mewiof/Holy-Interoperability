// Binds a title image's imports inside the title process.
//
// Every import is resolved by this project rather than by the OS loader, which
// is what lets a title that links against console-only system libraries start
// on a desktop host at all. For each imported library the binder picks a
// provider (see core/common/provider.h), and for each symbol either an
// interposer, the provider's export, or a reporting thunk that states exactly
// which function is still unimplemented when the title calls it.
#pragma once

#include <windows.h>

#include <cstdint>

#include "core/pe/image.h"

namespace holy::rt {

struct BindSummary {
  size_t libraries = 0;
  size_t symbols = 0;
  size_t from_host = 0;
  size_t from_package = 0;
  size_t from_runtime = 0;
  size_t overridden = 0;
  size_t unimplemented = 0;
};

BindSummary bind_title_imports(uint8_t* image_base, uint32_t image_size, pe::DataDirectory import_directory);

// LoadLibrary as a title sees it: bare console library names resolve to this
// project's implementations, mounted drive paths are translated.
HMODULE load_library_for_title(const wchar_t* requested, DWORD flags);

// Applies the runtime's interposers to the host imports of every DLL loaded
// from the package that has not been processed yet. The OS loader binds
// those DLLs; without this, code the package ships would reach the host's
// file, library-loading and network functions directly.
void rebind_package_modules();
HMODULE module_handle_for_title(const wchar_t* requested);

}  // namespace holy::rt
