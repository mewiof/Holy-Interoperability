// Descriptor heaps (see descriptors.cpp).
#pragma once

#include "core/libs/d3d12_xs/device.h"

namespace holy::d3d12 {

// Bytes per descriptor in title memory (and the increment titles are told).
inline constexpr UINT kTitleDescriptorBytes = 32;

struct DescriptorHeap {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12DescriptorHeap* host = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  UINT increment = 0;
  UINT host_count = 0;
  uint64_t title_cpu = 0;  // CPU start as the title sees it
  uint64_t title_bytes = 0;
  uint64_t host_cpu = 0;
  uint64_t host_gpu = 0;
};

HRESULT create_descriptor_heap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, void** out);

struct DescriptorLocation {
  DescriptorHeap* heap = nullptr;
  uint32_t index = 0;   // in the heap
  uint64_t title = 0;   // the descriptor's address in title memory
};
bool find_descriptor(uint64_t title_handle, DescriptorLocation* out);

// The host CPU handle for a title CPU handle; zero if it is in no heap.
D3D12_CPU_DESCRIPTOR_HANDLE host_cpu_handle(uint64_t title_handle);
// The host GPU handle for a title GPU handle (an address in a shader-visible
// heap's memory); zero if it is in none.
D3D12_GPU_DESCRIPTOR_HANDLE host_gpu_handle(uint64_t title_handle);

ID3D12DescriptorHeap* host_heap(const void* heap);

}  // namespace holy::d3d12
