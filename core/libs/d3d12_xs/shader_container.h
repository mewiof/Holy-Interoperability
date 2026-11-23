// Console shader containers (see shader_container.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace holy::d3d12 {

enum class ShaderFormat {
  Unknown,     // not a shader container
  Desktop,     // already a desktop container; copied unchanged
  Rebuilt,     // console container with bitcode; rebuilt as a desktop container
  NativeOnly,  // console container without bitcode: only native GPU code
};

ShaderFormat rebuild_desktop_container(const void* blob, size_t size, std::vector<uint8_t>& out);

// The documented DXBC container checksum of `container` (written to digest).
void dxbc_checksum(const uint8_t* container, size_t size, uint8_t digest[16]);

}  // namespace holy::d3d12
