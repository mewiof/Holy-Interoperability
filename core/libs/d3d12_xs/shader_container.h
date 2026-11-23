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

// The native program of a console shader container.
struct NativeProgram {
  std::vector<uint8_t> block;    // the decompressed native program block
  size_t code_offset = 0;        // machine code start within `block`
  uint32_t root_parameters = 0;  // matches the root signature's parameter count
};
bool native_program(const void* blob, size_t size, NativeProgram* out);

// A compute shader's native program and its thread group size.
struct NativeComputeProgram : NativeProgram {
  uint32_t threads[3] = {};
};
bool native_compute_program(const void* blob, size_t size, NativeComputeProgram* out);

// The documented DXBC container checksum of `container` (written to digest).
void dxbc_checksum(const uint8_t* container, size_t size, uint8_t digest[16]);

}  // namespace holy::d3d12
