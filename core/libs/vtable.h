// Reporting vtables for COM-style objects handed to titles.
//
// Console libraries return interface pointers whose method layout is only
// partly established. Every slot of such a vtable starts as a reporting stub:
// a call reports the interface, the slot and the caller in the title, then
// applies the unimplemented-import policy. Implemented methods replace their
// slots. An object passed to a title must start with the vtable pointer
// followed by a pointer to its `VtableInfo`.
#pragma once

#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "core/libs/support.h"

namespace holy::lib {

struct VtableInfo {
  const char* library;    // e.g. "gameinput.dll"
  const char* interface;  // e.g. "IGameInput"
};

struct ReportingObject {
  void* const* vtable;
  const VtableInfo* info;
};

template <int Slot>
uint64_t report_unimplemented_slot(ReportingObject* self, uint64_t a1, uint64_t a2, uint64_t a3) {
  char symbol[160];
  std::snprintf(symbol, sizeof(symbol), "%s#%d", self->info->interface, Slot);
  HolyLog(kError, "import", "%s(%llx, %llx, %llx)", symbol, a1, a2, a3);
  HolyUnimplementedFrom(self->info->library, symbol, _ReturnAddress());
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
