#include "core/libs/d3d12_xs/gpu_memory.h"

#include <d3d12.h>

#include <map>
#include <mutex>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

struct HeapRecord {
  ID3D12Heap* heap = nullptr;
  ID3D12Resource* whole = nullptr;  // a buffer spanning the heap
  uint64_t host_gpu_va = 0;
  uint64_t size = 0;
};

std::mutex g_lock;
std::map<HANDLE, HeapRecord>& section_heaps() {
  static auto* storage = new std::map<HANDLE, HeapRecord>();
  return *storage;
}
// Keyed by the end address of the allocation, for upper_bound lookups.
std::map<uintptr_t, std::pair<uintptr_t, HeapRecord>>& region_heaps() {
  static auto* storage = new std::map<uintptr_t, std::pair<uintptr_t, HeapRecord>>();
  return *storage;
}

using QueryPhysicalFn = BOOL (*)(const void*, HANDLE*, uint64_t*, uint64_t*);

QueryPhysicalFn query_physical() {
  static const auto fn = reinterpret_cast<QueryPhysicalFn>(
      GetProcAddress(GetModuleHandleW(L"xmem.dll"), "HolyXMemQueryPhysical"));
  return fn;
}

bool finish_record(Device* device, ID3D12Heap* heap, HeapRecord* record) {
  record->heap = heap;
  record->size = heap->GetDesc().SizeInBytes;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = record->size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const HRESULT hr = device->host->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&record->whole));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "cannot span a heap of 0x%llx bytes with a buffer (0x%08lX)",
            static_cast<unsigned long long>(record->size), static_cast<unsigned long>(hr));
    return false;
  }
  record->host_gpu_va = record->whole->GetGPUVirtualAddress();
  return true;
}

ID3D12Device13* device13(Device* device) {
  static ID3D12Device13* cached = [&] {
    ID3D12Device13* d13 = nullptr;
    device->host->QueryInterface(IID_PPV_ARGS(&d13));
    return d13;
  }();
  return cached;
}

ID3D12Device3* device3(Device* device) {
  static ID3D12Device3* cached = [&] {
    ID3D12Device3* d3 = nullptr;
    device->host->QueryInterface(IID_PPV_ARGS(&d3));
    return d3;
  }();
  return cached;
}

}  // namespace

bool locate(Device* device, uint64_t title_va, Backing* out) {
  std::lock_guard<std::mutex> lock(g_lock);
  ID3D12Device3* d3 = device3(device);
  if (!d3) return false;

  // Physical pages the title mapped.
  HANDLE section = nullptr;
  uint64_t offset = 0, run = 0;
  if (query_physical() && query_physical()(reinterpret_cast<const void*>(title_va), &section, &offset, &run)) {
    auto found = section_heaps().find(section);
    if (found == section_heaps().end()) {
      // Measured host behaviour (development host): a heap opened from the
      // section itself (OpenExistingHeapFromFileMapping) keeps the section's
      // commit charged to the system after the process ends, the whole
      // section each time, released heap or not; a heap opened over a mapped
      // view of it (OpenExistingHeapFromAddress) does not. So the heap is
      // opened over a view of the whole section kept for it (address space
      // only; the storage is the section's).
      void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0);
      ID3D12Heap* heap = nullptr;
      const HRESULT hr = view ? d3->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap)) : E_OUTOFMEMORY;
      if (FAILED(hr)) {
        LIB_LOG(lib::kError, "d3d12", "cannot open a GPU heap over physical pages (0x%08lX, error %lu)",
                static_cast<unsigned long>(hr), view ? 0ul : GetLastError());
        if (view) UnmapViewOfFile(view);
        return false;
      }
      HeapRecord record;
      if (!finish_record(device, heap, &record)) return false;
      LIB_LOG(lib::kDebug, "d3d12", "GPU heap over physical pages: section %p, 0x%llx bytes, host %p", section,
              static_cast<unsigned long long>(record.size), reinterpret_cast<void*>(record.host_gpu_va));
      found = section_heaps().emplace(section, record).first;
    }
    *out = Backing{found->second.heap, offset, run, found->second.host_gpu_va + offset, found->second.whole, 0,
                   found->second.size};
    return true;
  }

  // Committed memory the title allocated.
  auto region = region_heaps().upper_bound(title_va);
  if (region != region_heaps().end() && title_va >= region->second.first) {
    const uint64_t at = title_va - region->second.first;
    *out = Backing{region->second.second.heap, at, region->first - title_va, region->second.second.host_gpu_va + at,
                   region->second.second.whole, region->second.first, region->second.second.size};
    return true;
  }
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(title_va), &info, sizeof(info)) || info.State == MEM_FREE) return false;
  const auto allocation = reinterpret_cast<uintptr_t>(info.AllocationBase);
  uintptr_t allocation_end = allocation;
  for (;;) {
    MEMORY_BASIC_INFORMATION part;
    if (!VirtualQuery(reinterpret_cast<void*>(allocation_end), &part, sizeof(part))) break;
    if (reinterpret_cast<uintptr_t>(part.AllocationBase) != allocation || part.State == MEM_FREE) break;
    allocation_end = reinterpret_cast<uintptr_t>(part.BaseAddress) + part.RegionSize;
  }
  if (!HolyEnsureCommitted(reinterpret_cast<void*>(allocation), allocation_end - allocation)) return false;
  // Observed: titles commit stretches of a reservation and leave reserved
  // gaps between them. The host opens heaps from an address only over a
  // wholly committed allocation, or (ID3D12Device13) over a committed stretch
  // given with its size: such allocations get a heap per stretch.
  uintptr_t base = allocation, end = allocation_end;
  bool whole = true;
  for (uintptr_t at = allocation; at < allocation_end;) {
    MEMORY_BASIC_INFORMATION part;
    if (!VirtualQuery(reinterpret_cast<void*>(at), &part, sizeof(part))) break;
    if (part.State != MEM_COMMIT) whole = false;
    at = reinterpret_cast<uintptr_t>(part.BaseAddress) + part.RegionSize;
  }
  ID3D12Heap* heap = nullptr;
  HRESULT hr = E_FAIL;
  if (whole) {
    hr = d3->OpenExistingHeapFromAddress(reinterpret_cast<void*>(base), IID_PPV_ARGS(&heap));
  } else {
    if (!VirtualQuery(reinterpret_cast<void*>(title_va), &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    end = base + info.RegionSize;
    for (;;) {  // committed regions before and after (protection changes split regions)
      MEMORY_BASIC_INFORMATION part;
      if (base <= allocation || !VirtualQuery(reinterpret_cast<void*>(base - 1), &part, sizeof(part)) ||
          part.State != MEM_COMMIT) {
        break;
      }
      base = reinterpret_cast<uintptr_t>(part.BaseAddress);
    }
    for (;;) {
      MEMORY_BASIC_INFORMATION part;
      if (end >= allocation_end || !VirtualQuery(reinterpret_cast<void*>(end), &part, sizeof(part)) ||
          part.State != MEM_COMMIT) {
        break;
      }
      end = reinterpret_cast<uintptr_t>(part.BaseAddress) + part.RegionSize;
    }
    if (ID3D12Device13* d13 = device13(device)) {
      hr = d13->OpenExistingHeapFromAddress1(reinterpret_cast<void*>(base), end - base, IID_PPV_ARGS(&heap));
    }
  }
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "cannot open a GPU heap over title memory at %p, 0x%llx bytes%s (0x%08lX)",
            reinterpret_cast<void*>(base), static_cast<unsigned long long>(end - base),
            whole ? "" : " (a committed stretch)", static_cast<unsigned long>(hr));
    return false;
  }
  HeapRecord record;
  if (!finish_record(device, heap, &record)) return false;
  LIB_LOG(lib::kDebug, "d3d12", "GPU heap over title memory at %p: 0x%llx bytes (allocation %p + 0x%llx), host %p",
          reinterpret_cast<void*>(base), static_cast<unsigned long long>(record.size),
          reinterpret_cast<void*>(allocation), static_cast<unsigned long long>(allocation_end - allocation),
          reinterpret_cast<void*>(record.host_gpu_va));
  region_heaps()[base + record.size] = std::make_pair(base, record);
  const uint64_t at = title_va - base;
  *out = Backing{heap, at, record.size - at, record.host_gpu_va + at, record.whole, base, record.size};
  return true;
}

uint64_t translate_gpu_va(Device* device, uint64_t title_va) {
  if (!title_va) return 0;
  Backing backing;
  return locate(device, title_va, &backing) ? backing.host_gpu_va : 0;
}

}  // namespace holy::d3d12
