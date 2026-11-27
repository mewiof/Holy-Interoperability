// Descriptor heaps (see descriptors.cpp).
#pragma once

#include "core/libs/d3d12_xs/device.h"

namespace holy::d3d12 {

// Bytes per descriptor in title memory (and the increment titles are told):
// 32 for resource views (observed: native code reads descriptor tables in
// 32-byte steps), 16 for samplers (observed: native code indexes sampler
// tables in 16-byte steps).
inline constexpr UINT kTitleDescriptorBytes = 32;
inline constexpr UINT kTitleSamplerBytes = 16;
UINT title_descriptor_bytes(D3D12_DESCRIPTOR_HEAP_TYPE type);

// Shader-visible heaps get GPU addresses below 4 GiB (observed: native code
// reads descriptor tables through 32-bit pointers whose high half it sets to
// zero), allocated upwards from here.
inline constexpr uint64_t kHeapGpuBase = 0x40000000;

// The top of every shader-visible CBV/SRV/UAV host heap holds the region
// views of translated calls (bindings.cpp); title descriptors there clash.
inline constexpr UINT kReservedHostViews = 65536;

struct DescriptorHeap {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12DescriptorHeap* host = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  UINT increment = 0;
  UINT stride = kTitleDescriptorBytes;  // title bytes per descriptor
  UINT host_count = 0;
  uint64_t title_cpu = 0;  // CPU start as the title sees it
  uint64_t title_bytes = 0;
  uint64_t host_cpu = 0;
  uint64_t host_gpu = 0;
  uint64_t title_gpu = 0;  // GPU start as the title sees it (shader-visible heaps)
  // Shader-visible heaps: a CPU-readable copy. The host's shader-visible heaps
  // may only be written by the CPU (copying descriptors out of them is
  // invalid, measured with the host's debug layer), while titles copy
  // descriptors out of theirs. Descriptors are written into the shadow and
  // published to the visible heap (publish_descriptors); copies read shadows.
  ID3D12DescriptorHeap* shadow = nullptr;
  uint64_t shadow_cpu = 0;
};

HRESULT create_descriptor_heap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, void** out);

struct DescriptorLocation {
  DescriptorHeap* heap = nullptr;
  uint32_t index = 0;   // in the heap
  uint64_t title = 0;   // the descriptor's address in title memory
};
bool find_descriptor(uint64_t title_handle, DescriptorLocation* out);
// The heap whose GPU range holds `title_gpu`, and the descriptor's index.
bool find_gpu_descriptor(uint64_t title_gpu, DescriptorLocation* out);

// The host CPU handle for a title CPU handle; zero if it is in no heap.
// The host CPU handle descriptors are written through and copied from: the
// shadow's for shader-visible heaps (publish them afterwards).
D3D12_CPU_DESCRIPTOR_HANDLE host_cpu_handle(uint64_t title_handle);
// Copies `count` descriptors from `title_handle` on from the shadow into the
// shader-visible heap (nothing for heaps without a shadow).
void publish_descriptors(uint64_t title_handle, uint32_t count = 1);
// The host GPU handle for a title GPU handle (in a shader-visible heap's GPU
// range); zero if it is in none.
D3D12_GPU_DESCRIPTOR_HANDLE host_gpu_handle(uint64_t title_handle);

ID3D12DescriptorHeap* host_heap(const void* heap);

}  // namespace holy::d3d12
