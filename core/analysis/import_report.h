// Static prediction of how an image's imports will bind.
//
// Package and runtime libraries are only parsed, never loaded, so analysing a
// title executes none of its code. Host libraries are loaded from the system
// directory to look at their export tables.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common/provider.h"
#include "core/pe/image.h"

namespace holy::analysis {

struct LibraryReport {
  std::string name;
  ProviderKind kind = ProviderKind::Missing;
  bool delay_loaded = false;
  bool found = false;  // the providing file exists / loads
  size_t total = 0;
  size_t resolved = 0;
  std::vector<std::string> unresolved;  // symbol display names
};

struct ImageReport {
  std::vector<LibraryReport> libraries;
  size_t total = 0;
  size_t resolved = 0;
  // Symbols that only this project can provide (runtime or missing libraries).
  size_t console_total = 0;
  size_t console_resolved = 0;
};

// Returns true if the runtime interposes `symbol` of host library `library`.
using OverrideQuery = std::function<bool(std::string_view library, std::string_view symbol)>;

ImageReport check_imports(const pe::Image& image, const ProviderContext& context,
                          const OverrideQuery& has_override = {});

// Loads a host system library by bare name from the system directory only,
// with fallbacks for extension API sets that some hosts do not carry.
// Returns a module handle (as void*) or nullptr.
void* load_host_library(std::string_view import_name);

}  // namespace holy::analysis
