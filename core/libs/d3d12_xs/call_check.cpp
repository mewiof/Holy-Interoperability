#include "core/libs/d3d12_xs/call_check.h"

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/support.h"

namespace holy::d3d12 {

thread_local const void* t_api_caller = nullptr;

namespace {

bool removal_check_enabled() {
  static const bool enabled = GetEnvironmentVariableW(L"HOLY_D3D_CHECK", nullptr, 0) > 0;
  return enabled;
}

// The present HOLY_D3D_TRACE names (0: none).
uint64_t traced_present() {
  static const uint64_t present = [] {
    wchar_t text[32] = {};
    return GetEnvironmentVariableW(L"HOLY_D3D_TRACE", text, 32) ? static_cast<uint64_t>(_wtoi64(text)) : 0ull;
  }();
  return present;
}

}  // namespace

std::atomic<bool> g_frame_traced{false};

bool call_check_enabled() {
  static const bool enabled = removal_check_enabled() || traced_present() > 0;
  return enabled;
}

void trace_present(uint64_t present) {
  const uint64_t wanted = traced_present();
  if (!wanted) return;
  if (present + 1 == wanted || (wanted == 1 && present == 0)) {
    LIB_LOG(lib::kInfo, "trace", "tracing the calls up to present %llu", static_cast<unsigned long long>(wanted));
    g_frame_traced.store(true);
  } else if (present == wanted) {
    g_frame_traced.store(false);
    LIB_LOG(lib::kInfo, "trace", "end of the trace (present %llu)", static_cast<unsigned long long>(present));
  }
}

namespace {

struct Call {
  const char* object;
  int slot;
  const void* caller;
  DWORD thread;
  uint64_t tick;
};

constexpr uint32_t kRecent = 32;
std::mutex g_lock;
Call g_recent[kRecent] = {};
uint64_t g_calls = 0;
std::atomic<bool> g_found{false};

void describe(const Call& call, const char* label) {
  char where[160] = "this runtime";
  if (call.caller) HolyDescribeAddress(call.caller, where, sizeof(where));
  if (call.slot < 0) {
    LIB_LOG(lib::kError, "d3d12", "  %s %s (thread %lu, %llu ms)", label, call.object,
            static_cast<unsigned long>(call.thread), static_cast<unsigned long long>(call.tick));
    return;
  }
  LIB_LOG(lib::kError, "d3d12", "  %s %s method %d from %s (thread %lu, %llu ms)", label, call.object, call.slot, where,
          static_cast<unsigned long>(call.thread), static_cast<unsigned long long>(call.tick));
}

}  // namespace

void check_call(const char* object, int slot, const void* caller) {
  if (frame_traced()) {
    char where[160] = "this runtime";
    if (caller) HolyDescribeAddress(caller, where, sizeof(where));
    if (slot >= 0) {
      LIB_LOG(lib::kInfo, "trace", "%s method %d from %s (thread %lu)", object, slot, where,
              static_cast<unsigned long>(GetCurrentThreadId()));
    } else {
      LIB_LOG(lib::kInfo, "trace", "%s (thread %lu)", object, static_cast<unsigned long>(GetCurrentThreadId()));
    }
  }
  if (!removal_check_enabled() || g_found.load(std::memory_order_relaxed)) return;
  ID3D12Device* device = host_device();
  const Call call{object, slot, caller, GetCurrentThreadId(), GetTickCount64()};
  std::lock_guard<std::mutex> hold(g_lock);
  g_recent[g_calls++ % kRecent] = call;
  if (!device || device->GetDeviceRemovedReason() == S_OK || g_found.exchange(true)) return;
  LIB_LOG(lib::kError, "d3d12", "call check: the host device was found removed (0x%08lX) after call %llu:",
          static_cast<unsigned long>(device->GetDeviceRemovedReason()), static_cast<unsigned long long>(g_calls));
  describe(call, "this call:");
  const uint64_t first = g_calls > kRecent ? g_calls - kRecent : 0;
  for (uint64_t i = first; i + 1 < g_calls; ++i) describe(g_recent[i % kRecent], "before:");
}

void check_point(const char* what) {
  if (call_check_enabled()) check_call(what, -1, nullptr);
}

}  // namespace holy::d3d12
