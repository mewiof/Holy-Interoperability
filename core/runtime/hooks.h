// Interposers for host system functions whose console behaviour differs:
// library loading (console library names), file access (package mount, read
// only package) and application lifecycle notifications.
#pragma once

#include <string_view>

namespace holy::rt {

struct Override {
  const char* symbol;
  void* hook;
};

// Only imports that resolve to host system libraries are ever overridden.
const Override* find_override(std::string_view symbol);

}  // namespace holy::rt
