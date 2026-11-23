// Wrappers for interfaces the console shares with the desktop.
//
// Some console D3D12 interfaces carry desktop interface ids (fences, command
// queues) and keep the desktop method order for the slots titles have been
// seen to call. A wrapper forwards those slots to the host object with a small
// generated thunk (replace the object pointer, jump to the host's method);
// every other slot reports itself. Methods that hand objects back to the title
// are overridden so the title only ever sees wrappers.
#pragma once

#include <unknwn.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>

#include "core/libs/vtable.h"

namespace holy::d3d12 {

// Layout every wrapper starts with. The forwarding thunks read `host` at
// offset 0x10.
struct Wrapper {
  void* const* vtable;
  const lib::VtableInfo* info;
  IUnknown* host;
  std::atomic<long> refs{1};
};
static_assert(offsetof(Wrapper, host) == 0x10);

inline constexpr int kWrapperSlots = 96;

// A vtable whose slots 0..forwarded-1 forward to the host object (IUnknown
// slots excepted: the wrapper counts its own references) and whose other
// slots report themselves.
std::array<void*, kWrapperSlots> make_forwarding_vtable(int forwarded);

// Returns the host object behind a wrapper made with make_forwarding_vtable,
// or null if `object` is not one.
IUnknown* unwrap(const void* object);

template <typename T>
T* host_of(const void* object) {
  return static_cast<T*>(unwrap(object));
}

// Creates a wrapper around `host` (taking over the reference).
Wrapper* wrap(IUnknown* host, const lib::VtableInfo* info, void* const* vtable);

}  // namespace holy::d3d12
