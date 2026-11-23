// What the graphics library writes into title descriptor memory (32 bytes
// per descriptor), as read by translated native code.
//
// Buffer views and constant buffers: a buffer descriptor in the layout the
// console GPU reads (RDNA 2 buffer resource): dword 0 base address bits 0-31,
// dword 1 bits 0-15 base address bits 32-47 and bits 16-29 the stride, dword
// 2 the number of records (bytes when the stride is 0, else elements),
// dword 3 zero. Native code may read and modify these fields itself.
//
// Images (texture views) and samplers: dword 0 the descriptor's index in its
// host heap, dword 1 kDescriptorMarker, dword 2 the kind. Translated image
// instructions take the index from dword 0 to address the host heap.
#pragma once

#include <cstdint>

namespace holy::d3d12 {

inline constexpr uint32_t kDescriptorMarker = 0x48594C4Fu;
enum DescriptorKind : uint32_t {
  kKindShaderResource = 1,
  kKindUnorderedAccess = 2,
  kKindSampler = 3,
};

}  // namespace holy::d3d12
