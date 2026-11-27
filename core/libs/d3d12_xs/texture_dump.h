// A debugging aid: the contents of every live texture resource of the title
// at one chosen frame (HOLY_DUMP_TARGETS=<present number>), written as
// images to <data>/frames/textures-<n>/ with per-channel minimum and maximum
// in the log. It shows which passes of a frame produce anything.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace holy::d3d12 {

struct Device;
struct Resource;

// Live title resources (resource.cpp keeps the registry).
void register_live_resource(Resource* resource);
void unregister_live_resource(Resource* resource);

// The layout a texture was last moved to by a recorded barrier (approximate:
// as recorded, not as executed); dumps start from it.
void note_texture_layout(Resource* resource, D3D12_BARRIER_LAYOUT layout);

// The present number to dump at, 0 if none was asked for.
uint64_t texture_dump_frame();

// Dumps every live texture after the work submitted to `queue` so far.
void dump_textures(Device* device, ID3D12CommandQueue* queue, uint64_t present);

}  // namespace holy::d3d12
