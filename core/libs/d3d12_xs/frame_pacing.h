// Frame interval and frame events (see frame_pacing.cpp).
#pragma once

#include <windows.h>

#include <cstdint>

namespace holy::d3d12 {

HRESULT set_frame_interval(uint32_t length_us, uint32_t period, uint32_t flags);
HRESULT schedule_frame_event(uint32_t type, uint64_t offset_us, uint32_t flags);
// Waits for the next occurrence of a scheduled frame event and returns a
// frame pipeline token for it.
HRESULT wait_frame_event(uint32_t type, uint32_t timeout_ms, uint64_t* token);

}  // namespace holy::d3d12
