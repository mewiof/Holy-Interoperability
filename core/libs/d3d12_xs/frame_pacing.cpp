// Frame pacing.
//
// Observed: device slot 78 is SetFrameIntervalX(output, length_us, period,
// flags), called with a null output, 16667 or 8333 microseconds and a small
// period; slot 79 is ScheduleFrameEventX(type, offset_us, output, flags),
// called with type 0 (the interval origin). Usage matches the public Xbox GDK
// sample code. The frame interval is a periodic clock: frame events fire at
// origin + n * length + offset.
//
// Host model: the interval runs on the performance counter from the moment
// it is set. Waiting for a frame event sleeps until the next tick.
#include "core/libs/d3d12_xs/frame_pacing.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

std::mutex g_lock;
LARGE_INTEGER g_frequency{};
int64_t g_origin = 0;            // counter value of interval 0
int64_t g_length = 0;            // counter ticks per interval
int64_t g_event_offset[4] = {};  // per event type
bool g_event_scheduled[4] = {};
std::atomic<uint64_t> g_last_token{0};

int64_t now() {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

}  // namespace

HRESULT set_frame_interval(uint32_t length_us, uint32_t period, uint32_t flags) {
  std::lock_guard<std::mutex> lock(g_lock);
  QueryPerformanceFrequency(&g_frequency);
  g_length = static_cast<int64_t>(length_us) * g_frequency.QuadPart / 1000000;
  g_origin = now();
  LIB_LOG(lib::kInfo, "d3d12", "frame interval %u us, period %u, flags 0x%x", length_us, period, flags);
  return length_us ? S_OK : E_INVALIDARG;
}

HRESULT schedule_frame_event(uint32_t type, uint64_t offset_us, uint32_t flags) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (type >= 4 || !g_length) return E_INVALIDARG;
  g_event_offset[type] = static_cast<int64_t>(offset_us) * g_frequency.QuadPart / 1000000;
  g_event_scheduled[type] = true;
  LIB_LOG(lib::kDebug, "d3d12", "frame event %u at +%llu us (flags 0x%x)", type,
          static_cast<unsigned long long>(offset_us), flags);
  return S_OK;
}

HRESULT wait_frame_event(uint32_t type, uint32_t timeout_ms, uint64_t* token) {
  int64_t deadline;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (type >= 4 || !g_length || !g_event_scheduled[type]) return E_INVALIDARG;
    const int64_t current = now();
    const int64_t since = current - g_origin - g_event_offset[type];
    const int64_t next = since < 0 ? 0 : since / g_length + 1;
    deadline = g_origin + g_event_offset[type] + next * g_length;
  }
  const int64_t limit = timeout_ms == INFINITE ? INT64_MAX : now() + timeout_ms * g_frequency.QuadPart / 1000;
  if (deadline > limit) {
    Sleep(timeout_ms);
    return static_cast<HRESULT>(0x887A0027);  // DXGI_ERROR_WAIT_TIMEOUT
  }
  for (;;) {
    const int64_t left = deadline - now();
    if (left <= 0) break;
    const int64_t ms = left * 1000 / g_frequency.QuadPart;
    if (ms > 1) Sleep(static_cast<DWORD>(ms - 1));
    else YieldProcessor();
  }
  if (token) *token = g_last_token.fetch_add(1) + 1;
  return S_OK;
}

}  // namespace holy::d3d12
