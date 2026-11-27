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
  // The title address of heap offset 0 when the heap covers one stretch of
  // title addresses (committed memory); 0 for physical pages, whose heap
  // offsets follow the pages, not the addresses.
  uint64_t title_base = 0;
  uint64_t heap_size = 0;
  uint32_t slot = 0;  // the heap's first memory slot (shader/translate.h), 0 if it has none
};

// Finds (creating heaps on first use) what backs `title_va`, for `size` bytes
// from it at least where the memory allows (physical pages: the GPU's heaps
// over their storage are opened, and committed, over what is asked for).
bool locate(Device* device, uint64_t title_va, Backing* out, uint64_t size);
bool locate_range(Device* device, uint64_t title_va, uint64_t size, Backing* out);

// Host GPU address for a title GPU address, or 0 if the address lies in no
// memory the title owns.
uint64_t translate_gpu_va(Device* device, uint64_t title_va);

// --- the memory map translated programs reach title memory through ---------
//
// Every heap opened here gets memory slots (shader/translate.h): a raw view of
// each 512 MiB of its buffer, written into the reserved block of every
// shader-visible CBV/SRV/UAV heap at the slot's index. Its pages enter the
// page table as they become known: committed memory when its heap is opened,
// physical pages when they are located (the run of them one heap covers),
// aliases when made. Translated programs therefore reach every piece of title
// memory the runtime has located, whatever the call binds.
//
// Partial: memory nothing has located yet (reached only through pointers the
// title wrote) reads as zero; physical pages the title maps elsewhere later
// keep their old entries until located again.

// The host GPU address of the page table (a root view), 0 if it could not be
// made.
uint64_t page_table_address(Device* device);
// Whether the 64 KiB page holding `title_va` is in the page table (no lock).
bool page_mapped(uint64_t title_va);
// Locates `title_va` (entering its memory in the page table) unless its page
// already is.
void reach(Device* device, uint64_t title_va);
// Maps the title addresses [alias, alias + size) to the memory at `target`:
// descriptor heaps, whose GPU range reads their title memory.
bool map_alias(Device* device, uint64_t alias, uint64_t size, uint64_t target);
void unmap_range(uint64_t title_va, uint64_t size);

// A slot views kSlotBytes of its heap's buffer (2^27 raw elements, the
// desktop's limit for buffer views), one every kSlotStride bytes: consecutive
// slots of a heap overlap by a page, so a page lies wholly in the slot its
// start selects.
inline constexpr uint64_t kSlotBytes = 512ull << 20;
inline constexpr uint64_t kSlotStride = kSlotBytes - 0x10000;
inline uint64_t slot_count(uint64_t heap_bytes) {
  return heap_bytes <= kSlotBytes ? 1 : (heap_bytes - kSlotBytes + kSlotStride - 1) / kSlotStride + 1;
}
// For a page whose start lies `at` bytes into its heap (negative: a first
// page that starts before the heap), the slot counted from the heap's first
// and the page start's view offset in it (wrapping: addresses before the heap
// fall outside the view).
inline void page_slot(int64_t at, uint64_t* slot, uint32_t* view_offset) {
  *slot = at > 0 ? static_cast<uint64_t>(at) / kSlotStride : 0;
  *view_offset = static_cast<uint32_t>(static_cast<uint64_t>(at) - *slot * kSlotStride);
}

struct DescriptorHeap;
// Shader-visible CBV/SRV/UAV heaps hold every slot's view in their reserved
// block (descriptors.h); added heaps get the views of the slots so far.
void add_view_heap(Device* device, DescriptorHeap* heap);
void remove_view_heap(DescriptorHeap* heap);
// The GPU handle of a heap's slot views (the translated root's memory table).
uint64_t memory_views(const DescriptorHeap* heap);

}  // namespace holy::d3d12
