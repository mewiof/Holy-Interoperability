// Host textures for textures that live in title memory.
//
// Titles lay textures out in their memory as the texture layout library
// tells them (core/libs/texture_layout.h) and describe them to the graphics
// library by address. The host cannot sample that memory as a texture, so a
// host texture of the same shape is created and filled from it with
// buffer-to-texture copies, once per (address, shape).
//
// Partial: memory written after the host texture was filled is not copied
// again; textures the GPU writes in title memory are not supported.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace holy::d3d12 {

struct Device;

// The host texture for the texture described by `desc` at `address`, or null
// (logged) when it cannot be made.
ID3D12Resource* texture_at(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address);

}  // namespace holy::d3d12
