// The title's GPU address space on the host.
//
// On the console a title's GPU virtual addresses are its CPU addresses: it
// allocates memory itself (committed pools, physical pages it maps) and
// places resources at addresses inside it. On the host, each such piece of
// memory gets a Direct3D 12 heap opened over the same physical memory (from
// the section behind mapped physical pages, or from the address of committed
// memory), plus one buffer spanning the heap. A title address then translates
// to a host GPU address inside that buffer, and resources placed at a title
// address are placed in the heap at the matching offset.
#pragma once

#include <cstdint>

struct ID3D12Heap;
struct ID3D12Resource;

namespace holy::d3d12 {

struct Device;

struct Backing {
  ID3D12Heap* heap = nullptr;
  uint64_t heap_offset = 0;  // of the title address
  uint64_t contiguous = 0;   // bytes from the title address on in the same heap
  uint64_t host_gpu_va = 0;  // host GPU address of the title address
  ID3D12Resource* whole = nullptr;  // a buffer spanning the heap from offset 0
};

// Finds (creating heaps on first use) what backs `title_va`.
bool locate(Device* device, uint64_t title_va, Backing* out);

// Host GPU address for a title GPU address, or 0 if the address lies in no
// memory the title owns.
uint64_t translate_gpu_va(Device* device, uint64_t title_va);

}  // namespace holy::d3d12
