#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/gpu_memory.h"

#include <d3d12.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

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

// Physical page storage: sections whose pages take commit as they are used
// (xmem). The GPU reaches a section through heaps opened over committed spans
// of a view of it kept here: spans of whole kBlock blocks covering what was
// asked for, committed when first asked for. Measured host behaviour
// (development host): a heap opened over a committed stretch of a view of a
// reserved section works, and the GPU and the CPU see each other's writes; a
// heap opened from the section handle itself (OpenExistingHeapFromFileMapping)
// leaves the whole section's commit charged to the system after the process
// ends, and the host opens it only over fully committed sections.
constexpr uint64_t kBlock = 64ull << 20;

struct SectionRecord {
  uint8_t* view = nullptr;  // the whole section
  uint64_t size = 0;
  std::vector<std::pair<uint64_t, HeapRecord>> spans;  // (section offset, heap), a heap per span
};
std::map<HANDLE, SectionRecord>& sections() {
  static auto* storage = new std::map<HANDLE, SectionRecord>();
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
  // Buffers take whole 64 KiB units of a heap (measured, the host's debug
  // layer: heaps over committed stretches can end inside one).
  record->size = heap->GetDesc().SizeInBytes & ~0xFFFFull;
  if (!record->size) {
    heap->Release();
    return false;
  }
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
    heap->Release();
    record->heap = nullptr;
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

namespace {

const lib::Counter& c_block_commit() {
  static const lib::Counter c("d3d12.gpu_block_commit_bytes");
  return c;
}

// The heap over the blocks of `section` covering [begin, end), opened (and
// the blocks committed) if no span covers them yet.
const std::pair<uint64_t, HeapRecord>* section_span(Device* device, HANDLE section, uint64_t begin, uint64_t end) {
  SectionRecord& record = sections()[section];
  if (!record.view) {
    MEMORY_BASIC_INFORMATION info{};
    record.view = static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0));
    if (!record.view || !VirtualQuery(record.view, &info, sizeof(info))) {
      LIB_LOG(lib::kError, "d3d12", "cannot map physical storage for the GPU (error %lu)", GetLastError());
      if (record.view) UnmapViewOfFile(record.view);
      record.view = nullptr;
      return nullptr;
    }
    // The view's extent: the reservation of the whole section.
    uint64_t size = 0;
    for (uint8_t* at = record.view;;) {
      MEMORY_BASIC_INFORMATION part{};
      if (!VirtualQuery(at, &part, sizeof(part)) || part.AllocationBase != record.view) break;
      size += part.RegionSize;
      at += part.RegionSize;
    }
    record.size = size;
  }
  for (const auto& span : record.spans) {
    if (begin >= span.first && end <= span.first + span.second.size) return &span;
  }
  const uint64_t first = begin & ~(kBlock - 1);
  const uint64_t last = std::min((end + kBlock - 1) & ~(kBlock - 1), record.size);
  if (last <= first) return nullptr;
  if (!VirtualAlloc(record.view + first, last - first, MEM_COMMIT, PAGE_READWRITE)) {
    LIB_LOG(lib::kError, "d3d12", "cannot commit 0x%llx bytes of physical storage for the GPU (error %lu)",
            static_cast<unsigned long long>(last - first), GetLastError());
    return nullptr;
  }
  c_block_commit().add(static_cast<int64_t>(last - first));
  ID3D12Device13* d13 = device13(device);
  ID3D12Heap* heap = nullptr;
  const HRESULT hr = d13 ? d13->OpenExistingHeapFromAddress1(record.view + first, last - first, IID_PPV_ARGS(&heap))
                         : E_NOINTERFACE;
  check_point("GPU heap over physical pages");
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "cannot open a GPU heap over physical pages (0x%08lX)", static_cast<unsigned long>(hr));
    return nullptr;
  }
  HeapRecord heap_record;
  if (!finish_record(device, heap, &heap_record)) return nullptr;
  LIB_LOG(lib::kDebug, "d3d12", "GPU heap over physical pages: section %p + 0x%llx, 0x%llx bytes, host %p", section,
          static_cast<unsigned long long>(first), static_cast<unsigned long long>(heap_record.size),
          reinterpret_cast<void*>(heap_record.host_gpu_va));
  record.spans.emplace_back(first, heap_record);
  return &record.spans.back();
}

}  // namespace

bool locate(Device* device, uint64_t title_va, Backing* out, uint64_t size) {
  return locate_range(device, title_va, size, out);
}

bool locate_range(Device* device, uint64_t title_va, uint64_t size, Backing* out) {
  std::lock_guard<std::mutex> lock(g_lock);
  ID3D12Device3* d3 = device3(device);
  if (!d3) return false;

  // Physical pages the title mapped: `run` bytes from the address are pages
  // laid out in order in one section.
  HANDLE section = nullptr;
  uint64_t offset = 0, run = 0;
  if (query_physical() && query_physical()(reinterpret_cast<const void*>(title_va), &section, &offset, &run)) {
    const uint64_t wanted = std::max<uint64_t>(std::min(size ? size : 1, run), 1);
    const auto* span = section_span(device, section, offset, offset + wanted);
    if (!span) return false;
    const uint64_t at = offset - span->first;
    *out = Backing{span->second.heap, at, std::min(run, span->second.size - at), span->second.host_gpu_va + at,
                   span->second.whole, 0, span->second.size};
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
  // Measured (the host's debug layer): heaps are opened only over pages of one
  // protection (titles hand thread stacks, whose guard pages differ, to the
  // GPU too), so stretches also end where the protection changes.
  uintptr_t base = allocation, end = allocation_end;
  bool whole = true;
  DWORD protection = 0;
  for (uintptr_t at = allocation; at < allocation_end;) {
    MEMORY_BASIC_INFORMATION part;
    if (!VirtualQuery(reinterpret_cast<void*>(at), &part, sizeof(part))) break;
    if (part.State != MEM_COMMIT || (protection && part.Protect != protection)) whole = false;
    if (!protection) protection = part.Protect;
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
          part.State != MEM_COMMIT || part.Protect != info.Protect) {
        break;
      }
      base = reinterpret_cast<uintptr_t>(part.BaseAddress);
    }
    for (;;) {
      MEMORY_BASIC_INFORMATION part;
      if (end >= allocation_end || !VirtualQuery(reinterpret_cast<void*>(end), &part, sizeof(part)) ||
          part.State != MEM_COMMIT || part.Protect != info.Protect) {
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
  return locate(device, title_va, &backing, 1) ? backing.host_gpu_va : 0;
}

}  // namespace holy::d3d12
