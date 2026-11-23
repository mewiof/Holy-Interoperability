// Presentation: frame tokens, PresentX and captured frames (see present.cpp).
#pragma once

#include <windows.h>

#include <cstdint>

namespace holy::d3d12 {

struct CommandQueue;

// Observed (queue slot 32) PresentX(plane_count, planes, parameters). One plane
// is 0x48 bytes, the layout of the public D3D12XBOX_PRESENT_PLANE_PARAMETERS:
struct PresentPlane {
  uint64_t token;
  uint32_t resource_count;
  uint32_t reserved;
  void* const* resources;
  const RECT* source_rects;
  const void* destination;  // placement (not used on the host)
  uint32_t color_space;
  uint32_t rest[7];
};
static_assert(sizeof(PresentPlane) == 0x48);

HRESULT present_x(CommandQueue* queue, uint32_t plane_count, const PresentPlane* planes, const void* parameters);

// Frame tokens (device slot 80, WaitFrameEventX). A token names a frame from
// the frame event that started it; frame event 2 of a token is reached once
// that frame's present has been carried out by the GPU (assumed meaning).
uint64_t next_frame_token();
HRESULT wait_for_presented(uint64_t token, uint32_t timeout_ms);

}  // namespace holy::d3d12
