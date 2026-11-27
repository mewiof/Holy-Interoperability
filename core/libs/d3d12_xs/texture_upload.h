// Host textures for textures that live in title memory.
//
// Titles lay textures out in their memory as the texture layout library
// tells them (core/libs/texture_layout.h) and describe them to the graphics
// library by address. The host cannot sample that memory as a texture, so a
// host texture of the same shape is created and filled from it with
// buffer-to-texture copies, once per (address, shape).
//
// Textures a title places in its memory (CreatePlacedResourceX) are host
// textures of their own, registered under their address and shape: the host
// cannot place textures in the heaps over title memory (those take buffers
// and row-major textures only), and textures the GPU writes are only ever
// read through views, which then find the same host texture.
//
// Partial: memory written after the host texture was filled is not copied
// again; the CPU never sees what the GPU wrote into a placed texture.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace holy::d3d12 {

struct Device;

// The host texture for the texture described by `desc` at `address`, or null
// (logged) when it cannot be made.
ID3D12Resource* texture_at(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address);

// A host texture for a texture placed at `address`, in the initial layout
// the title gives (a new texture per placement, since a placement's contents
// start undefined; views by address find the latest one), or null (logged).
// The caller receives a reference.
ID3D12Resource* placed_texture(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address,
                               UINT initial_layout, const D3D12_CLEAR_VALUE* clear);

// Registers the host texture of a title resource at `address` (committed
// textures live in memory the runtime allocates for them, as on the console),
// so views by address find it. The registry takes a reference.
void register_texture(const D3D12_RESOURCE_DESC& desc, uint64_t address, ID3D12Resource* texture);
// The title released the resource: its registration goes if it is still the
// one for that address and shape.
void unregister_texture(const D3D12_RESOURCE_DESC& desc, uint64_t address, ID3D12Resource* texture);

// How many host textures there are and their bytes in title memory, for the log.
void host_texture_totals(uint64_t* count, uint64_t* bytes);

}  // namespace holy::d3d12
