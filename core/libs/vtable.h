// Reporting vtables for COM-style objects handed to titles.
//
// Console libraries return interface pointers whose method layout is only
// partly established. Every slot of such a vtable starts as a reporting stub:
// a call reports the interface, the slot and the caller in the title, then
// applies the unimplemented-import policy. Implemented methods replace their
// slots. An object passed to a title must start with the vtable pointer
// followed by a pointer to its `VtableInfo`, or have its vtable named with
// name_vtable (objects that keep something else there).
#pragma once

#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
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

// Vtables named for the reports of objects that do not keep their
// `VtableInfo` after the vtable (a few per library, named once).
struct NamedVtable {
  void* const* vtable;
  const VtableInfo* info;
};
inline NamedVtable g_named_vtables[8] = {};
inline std::atomic<int> g_named_vtable_count{0};

inline void name_vtable(void* const* vtable, const VtableInfo* info) {
  const int index = g_named_vtable_count.fetch_add(1);
  if (index < 8) g_named_vtables[index] = {vtable, info};
}

// Reads `size` bytes at `address` into `out`; false where the memory cannot
// be read. Used to probe values that may be any number: structured exception
// handling does not cover plain loads in clang-cl builds (only faults in
// calls), so the probe goes through the host, which checks the address.
inline bool probe_read(const void* address, void* out, size_t size) {
  SIZE_T read = 0;
  return address && ReadProcessMemory(GetCurrentProcess(), address, out, size, &read) && read == size;
}

// The named `VtableInfo` of `candidate`'s vtable, or null. Guarded: the
// candidate may be any value.
inline const VtableInfo* named_vtable_info(const void* candidate) {
  void* const* vtable = nullptr;
  if (!probe_read(candidate, &vtable, sizeof(vtable)) || !vtable) return nullptr;
  const int count = g_named_vtable_count.load() < 8 ? g_named_vtable_count.load() : 8;
  for (int i = 0; i < count; ++i) {
    if (g_named_vtables[i].vtable == vtable) return g_named_vtables[i].info;
  }
  return nullptr;
}

// True if `candidate` is an object whose vtable has `stub` in `slot`. Guarded:
// the candidate may be any value.
inline bool owns_stub(const void* candidate, int slot, const void* stub) {
  ReportingObject object{};
  if (!probe_read(candidate, &object, sizeof(object)) || !object.vtable || !object.info) return false;
  const void* in_slot = nullptr;
  return probe_read(object.vtable + slot, &in_slot, sizeof(in_slot)) && in_slot == stub;
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
  const VtableInfo* info = self ? self->info : named_vtable_info(first);
  char symbol[160];
  std::snprintf(symbol, sizeof(symbol), "%s#%d", info ? info->name : "unknown-interface", Slot);
  HolyLog(kError, "import", "%s(%p, %llx, %llx, %llx)%s", symbol, first, a1, a2, a3, convention);
  if (!info) {
    // What the object looks like (its vtable and the word after it), to
    // tell whose it is.
    uint64_t words[2] = {};
    SIZE_T read = 0;
    ReadProcessMemory(GetCurrentProcess(), first, words, sizeof(words), &read);
    HolyLog(kError, "import", "  the object starts with %016llx %016llx", static_cast<unsigned long long>(words[0]),
            static_cast<unsigned long long>(words[1]));
  }
  HolyUnimplementedFrom(info ? info->library : "?", symbol, _ReturnAddress());
  return 0;
}

// Methods that return a structure. Observed: titles pass the object first and
// the result pointer second (the compiler's convention for member functions,
// as the desktop C++ declarations give), and on some calls the result pointer
// first. Both leading arguments are valid pointers either way; the object is
// the one whose `VtableInfo` is `info` (or `other`, for objects that change
// identity, such as released resources).
template <typename Object, typename Result>
void struct_return_arguments(void* first, void* second, const VtableInfo* info, Object** self, Result** result,
                             const VtableInfo* other = nullptr) {
  const auto* candidate = static_cast<const ReportingObject*>(first);
  const bool first_is_object = candidate && (candidate->info == info || (other && candidate->info == other));
  *self = static_cast<Object*>(first_is_object ? first : second);
  *result = static_cast<Result*>(first_is_object ? second : first);
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
