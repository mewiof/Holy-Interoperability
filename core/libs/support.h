// Shared helpers for the console library implementations.
#pragma once

#include <cstdint>

#include "core/runtime/api.h"

#define HOLY_EXPORT extern "C" __declspec(dllexport)

namespace holy::lib {
// Mirrors holy::log::Level without pulling the logger into every library.
inline constexpr uint32_t kTrace = 0;
inline constexpr uint32_t kDebug = 1;
inline constexpr uint32_t kInfo = 2;
inline constexpr uint32_t kWarn = 3;
inline constexpr uint32_t kError = 4;
}  // namespace holy::lib

#define LIB_LOG(level, component, ...)                                 \
  do {                                                                 \
    if (HolyLogEnabled(level)) HolyLog(level, component, __VA_ARGS__); \
  } while (0)

// Exports `name` as a placeholder for a function whose contract has not been
// established yet. It exists so that images which import the symbol still
// load; calling it reports the register arguments and applies the
// unimplemented-import policy.
//
// A companion marker export (kPlaceholderPrefix + name, see
// core/common/provider.h) tells the binder and the inspector that the symbol
// is not an implementation, so placeholders never count as coverage.
#define HOLY_UNIMPLEMENTED_EXPORT(library, name)                                                             \
  HOLY_EXPORT const char HolyPlaceholder_##name = 1;                                                         \
  HOLY_EXPORT uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {                            \
    HolyLog(::holy::lib::kError, "import", "%s!%s(%llx, %llx, %llx, %llx)", library, #name, a0, a1, a2, a3); \
    HolyUnimplemented(library, #name);                                                                       \
    return 0;                                                                                                \
  }
