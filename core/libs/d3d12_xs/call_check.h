// HOLY_D3D_CHECK=1: finding the call after which the host removed the device.
//
// A host device removed for an invalid call (DXGI_ERROR_INVALID_CALL) says
// nothing about the call, and the debug layer, which would, changes what the
// host does enough that the removal may not happen under it. With the check
// on, every implemented method of the device, its queues and its command
// lists is wrapped: after each call the host device is asked whether it was
// removed, and the first time it was, the call is logged with the calls just
// before it. Off (the default), the method tables hold the plain functions.
//
// HOLY_D3D_TRACE=<n>: the same wrappers log every call made between presents
// n-1 and n, and draws, clears and state calls log what they were given
// (HOLY_TRACE below): a readable account of one frame.
#pragma once

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "core/libs/support.h"

namespace holy::d3d12 {

// Object names for reports (template arguments, so with linkage).
inline constexpr char kDeviceObject[] = "device";
inline constexpr char kQueueObject[] = "queue";
inline constexpr char kCommandListObject[] = "command list";

// The title's call into the method being run, for reports that should name
// the title's code rather than a wrapper (null outside wrapped calls).
extern thread_local const void* t_api_caller;

// Whether the method tables are wrapped: HOLY_D3D_CHECK or HOLY_D3D_TRACE is
// set (read once).
bool call_check_enabled();

// Whether calls are being traced now (HOLY_D3D_TRACE's frame).
extern std::atomic<bool> g_frame_traced;
inline bool frame_traced() { return g_frame_traced.load(std::memory_order_relaxed); }
// Called after each present with its number: starts and ends the trace.
void trace_present(uint64_t present);

// Records a finished call of method `slot` of a `object` (title return
// address `caller`) and checks the host device after it.
void check_call(const char* object, int slot, const void* caller);

// The same for this runtime's own host calls (pipeline compiles, uploads,
// presents, heap opening), named `what`: nothing when the check is off.
void check_point(const char* what);

template <const char* Object, int Slot, auto F>
struct checked;
template <const char* Object, int Slot, typename R, typename Self, typename... Args, R (*F)(Self, Args...)>
struct checked<Object, Slot, F> {
  static R call(Self self, Args... args) {
    const void* outer = t_api_caller;
    if (!outer) t_api_caller = _ReturnAddress();
    const void* caller = t_api_caller;
    if constexpr (std::is_void_v<R>) {
      F(self, args...);
      t_api_caller = outer;
      check_call(Object, Slot, caller);
    } else {
      R result = F(self, args...);
      t_api_caller = outer;
      check_call(Object, Slot, caller);
      return result;
    }
  }
};

}  // namespace holy::d3d12

// Logs a detail of the traced frame.
#define HOLY_TRACE(...)                                                               do {                                                                                  if (::holy::d3d12::frame_traced()) LIB_LOG(::holy::lib::kInfo, "trace", __VA_ARGS__);   } while (0)

// Sets method `index` of a method table to `function`, wrapped by `checked`
// when the call check is on.
#define HOLY_D3D_SLOT(table, object, index, function)                                                      \
  (table)[index] = ::holy::d3d12::call_check_enabled()                                                     \
                       ? reinterpret_cast<void*>(&::holy::d3d12::checked<object, index, &function>::call) \
                       : reinterpret_cast<void*>(&function)
