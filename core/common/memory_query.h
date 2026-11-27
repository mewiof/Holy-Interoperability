// What the host knows about the allocation holding an address, without the
// cost of VirtualQuery.
//
// Measured (development host, an 8 GiB reservation with 16,386 committed and
// reserved runs, local/experiments): VirtualQuery computes the extent of the
// run of pages in the same state by walking page-table entries, so one query
// takes 10 ms at the start of a touched, committed 1 GiB run and 5 ms in its
// middle, and walking the allocation run by run takes 19 ms. The host's record
// of the allocation (MemoryRegionInfo) answers in 0.15 us wherever the address
// lies: the allocation's base and size, how much of it is committed, and
// whether it is a mapped view. Hot paths ask this, and VirtualQuery only for
// what it alone knows (the state and protection of particular pages).
#pragma once

#include <windows.h>

#include <cstdint>

namespace holy {

struct AllocationExtent {
  uintptr_t base = 0;
  uint64_t size = 0;       // the whole allocation
  uint64_t committed = 0;  // bytes of it committed
  bool mapped = false;     // a view of a section (not private memory)
};

// The allocation holding `address`; false for free memory or when the host
// cannot say.
inline bool query_allocation(const void* address, AllocationExtent* out) {
  using Fn = BOOL(WINAPI*)(HANDLE, const VOID*, WIN32_MEMORY_INFORMATION_CLASS, PVOID, SIZE_T, PSIZE_T);
  static const Fn query = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "QueryVirtualMemoryInformation"));
  if (query) {
    WIN32_MEMORY_REGION_INFORMATION region{};
    SIZE_T got = 0;
    if (!query(GetCurrentProcess(), address, MemoryRegionInfo, &region, sizeof(region), &got)) return false;
    out->base = reinterpret_cast<uintptr_t>(region.AllocationBase);
    out->size = region.RegionSize;
    out->committed = region.CommitSize;
    out->mapped = !region.Private;
    return true;
  }
  // Hosts without the query: the slow walk.
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State == MEM_FREE) return false;
  const auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
  uintptr_t end = base;
  uint64_t committed = 0;
  for (;;) {
    MEMORY_BASIC_INFORMATION part{};
    if (!VirtualQuery(reinterpret_cast<const void*>(end), &part, sizeof(part))) break;
    if (reinterpret_cast<uintptr_t>(part.AllocationBase) != base || part.State == MEM_FREE) break;
    if (part.State == MEM_COMMIT) committed += part.RegionSize;
    end = reinterpret_cast<uintptr_t>(part.BaseAddress) + part.RegionSize;
  }
  out->base = base;
  out->size = end - base;
  out->committed = committed;
  out->mapped = info.Type != MEM_PRIVATE;
  return true;
}

}  // namespace holy
