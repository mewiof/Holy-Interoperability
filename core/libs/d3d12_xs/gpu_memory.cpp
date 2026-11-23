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
      ID3D12Heap* heap = nullptr;
      const HRESULT hr = d3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heap));
      if (FAILED(hr)) {
        LIB_LOG(lib::kError, "d3d12", "cannot open a GPU heap over physical pages (0x%08lX)",
                static_cast<unsigned long>(hr));
        return false;
      }
      HeapRecord record;
      if (!finish_record(device, heap, &record)) return false;
      found = section_heaps().emplace(section, record).first;
    }
    *out = Backing{found->second.heap, offset, run, found->second.host_gpu_va + offset, found->second.whole};
    return true;
  }

  // Committed memory the title allocated.
  auto region = region_heaps().upper_bound(title_va);
  if (region != region_heaps().end() && title_va >= region->second.first) {
    const uint64_t at = title_va - region->second.first;
    *out = Backing{region->second.second.heap, at, region->first - title_va, region->second.second.host_gpu_va + at,
                   region->second.second.whole};
    return true;
  }
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(title_va), &info, sizeof(info)) || info.State == MEM_FREE) return false;
  const auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
  uintptr_t end = base;
  for (;;) {
    MEMORY_BASIC_INFORMATION part;
    if (!VirtualQuery(reinterpret_cast<void*>(end), &part, sizeof(part))) break;
    if (reinterpret_cast<uintptr_t>(part.AllocationBase) != base || part.State == MEM_FREE) break;
    end = reinterpret_cast<uintptr_t>(part.BaseAddress) + part.RegionSize;
  }
  if (!HolyEnsureCommitted(reinterpret_cast<void*>(base), end - base)) return false;
  ID3D12Heap* heap = nullptr;
  const HRESULT hr = d3->OpenExistingHeapFromAddress(reinterpret_cast<void*>(base), IID_PPV_ARGS(&heap));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "cannot open a GPU heap over title memory at %p (0x%08lX)",
            reinterpret_cast<void*>(base), static_cast<unsigned long>(hr));
    return false;
  }
  HeapRecord record;
  if (!finish_record(device, heap, &record)) return false;
  region_heaps().emplace(base + record.size, std::make_pair(base, record));
  const uint64_t at = title_va - base;
  *out = Backing{heap, at, record.size - at, record.host_gpu_va + at, record.whole};
  return true;
}

uint64_t translate_gpu_va(Device* device, uint64_t title_va) {
  if (!title_va) return 0;
  Backing backing;
  return locate(device, title_va, &backing) ? backing.host_gpu_va : 0;
}

}  // namespace holy::d3d12
