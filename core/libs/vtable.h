// Reporting vtables for COM-style objects handed to titles.
//
// Console libraries return interface pointers whose method layout is only
// partly established. Every slot of such a vtable starts as a reporting stub:
// a call reports the interface, the slot and the caller in the title, then
// applies the unimplemented-import policy. Implemented methods replace their
// slots. An object passed to a title must start with the vtable pointer
// followed by a pointer to its `VtableInfo`.
#pragma once

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "core/libs/support.h"

namespace holy::lib {

struct VtableInfo {
  const char* library;    // e.g. "gameinput.dll"
  const char* name;       // e.g. "IGameInput"
};

struct ReportingObject {
  void* const* vtable;
  const VtableInfo* info;
};

// True if `candidate` is an object whose vtable has `stub` in `slot`. Guarded:
// the candidate may be any value.
inline bool owns_stub(const void* candidate, int slot, const void* stub) {
  __try {
    const auto* object = static_cast<const ReportingObject*>(candidate);
    return object && object->vtable && object->vtable[slot] == stub && object->info;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Methods that return a structure are called C-style by some console
// headers: the result pointer comes first and the object second. The stub
// recognises its object in either position.
template <int Slot>
uint64_t report_unimplemented_slot(void* first, uint64_t a1, uint64_t a2, uint64_t a3) {
  const void* stub = reinterpret_cast<const void*>(&report_unimplemented_slot<Slot>);
  const ReportingObject* self = nullptr;
  const char* convention = "";
  if (owns_stub(first, Slot, stub)) {
    self = static_cast<const ReportingObject*>(first);
  } else if (owns_stub(reinterpret_cast<const void*>(a1), Slot, stub)) {
    self = reinterpret_cast<const ReportingObject*>(a1);
    convention = " (result pointer first)";
  }
  char symbol[160];
  std::snprintf(symbol, sizeof(symbol), "%s#%d", self ? self->info->name : "unknown-interface", Slot);
  HolyLog(kError, "import", "%s(%p, %llx, %llx, %llx)%s", symbol, first, a1, a2, a3, convention);
  HolyUnimplementedFrom(self ? self->info->library : "?", symbol, _ReturnAddress());
  return 0;
}

template <int... Slots>
std::array<void*, sizeof...(Slots)> reporting_slots(std::integer_sequence<int, Slots...>) {
  return {reinterpret_cast<void*>(&report_unimplemented_slot<Slots>)...};
}

// A vtable of N reporting stubs; assign implemented slots afterwards.
template <int N>
std::array<void*, N> make_reporting_vtable() {
  return reporting_slots(std::make_integer_sequence<int, N>{});
}

}  // namespace holy::lib
