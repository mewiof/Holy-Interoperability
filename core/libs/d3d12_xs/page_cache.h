// Copies in video memory of title memory translated programs read.
//
// Measured (PLATFORM_NOTES.md): the GPU reads title memory over the bus
// without its second-level cache, about 20 times slower than video memory
// once data misses the first-level caches. Pages that calls read through
// bound buffers (vertex buffers, buffers of descriptor tables, root views) get
// a copy in video memory, and the page table sends translated reads there
// (shader/translate.h: cached pages). Title memory stays the memory the title
// and its GPU work write:
// - translated stores and atomics go to title memory and set the page's flag,
//   after which translated reads take title memory; a flagged page leaves the
//   cache for good;
// - other GPU writes into title memory (copies, query results, immediate
//   writes, unordered-access views and root views of desktop programs) take
//   the pages out of the cache before the submission that records them;
// - the CPU's writes are found by write watching before each submission and
//   the copies refreshed first (the submitting queue waits for the copies);
//   pages written that often leave the cache.
// Pages of memory the host does not watch (mapped views: physical pages) are
// not cached. HOLY_BUFFER_COPIES sets the budget in MiB (0 or unset: off).
#pragma once

#include <cstdint>

struct ID3D12CommandQueue;

namespace holy::d3d12 {

struct Device;

bool page_cache_enabled();
// [address, address + size) is read through a buffer a call binds.
void page_cache_want(uint64_t address, uint64_t size);
// Before a submission to `queue`.
void page_cache_before_submit(Device* device, ID3D12CommandQueue* queue);
// The GPU will write [address, address + size) other than through translated
// programs.
void page_cache_gpu_writes(uint64_t address, uint64_t size);
// The CPU wrote [page, page + bytes) (another write watch scan found it).
void page_cache_written(uint64_t page, uint64_t bytes);

}  // namespace holy::d3d12
