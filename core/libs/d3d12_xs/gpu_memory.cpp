#include "core/libs/d3d12_xs/gpu_memory.h"

#include <d3d12.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

struct HeapRecord {
  ID3D12Heap* heap = nullptr;
  ID3D12Resource* whole = nullptr;  // a buffer spanning the heap
  uint64_t host_gpu_va = 0;
  uint64_t size = 0;
  uint32_t slot = 0;  // first memory slot
  // Committed memory: the base of the allocation the heap was opened over,
  // telling memory released and allocated again at the same address apart.
  uint64_t allocation = 0;
};

std::mutex g_lock;

// --- memory map (gpu_memory.h) ---------------------------------------------------------

// Level 2 tables: 1 GiB windows of title addresses holding mapped pages.
constexpr uint32_t kPageTables = 256;
constexpr size_t kPageTableBytes = kPageTableLevel1Bytes + static_cast<size_t>(kPageTables) * kPageTableTableBytes;

struct Slot {
  ID3D12Resource* buffer = nullptr;
  uint64_t first = 0;  // byte offset in the buffer
  uint64_t bytes = 0;
};

struct MemoryMap {
  uint8_t* table = nullptr;  // level 1, then the level 2 tables
  uint64_t host_va = 0;      // the table's host GPU address
  uint32_t tables = 0;       // level 2 tables handed out
  bool failed = false;
  std::vector<Slot> slots = std::vector<Slot>(1);  // slot 0: the null view
  std::vector<uint32_t> free_slots;                 // of released heaps, for heaps of one slot
  std::vector<DescriptorHeap*> heaps;               // shader-visible heaps holding the views
  Device* device = nullptr;
};
MemoryMap g_map;  // under g_lock
std::atomic<const uint8_t*> g_table{nullptr};
std::atomic<uint64_t> g_table_va{0};

const lib::Counter& c_slots() {
  static const lib::Counter c("d3d12.memory_slots");
  return c;
}
const lib::Counter& c_page_tables() {
  static const lib::Counter c("d3d12.page_tables");
  return c;
}

void write_slot_view(Device* device, const DescriptorHeap* heap, uint32_t slot) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  ID3D12Resource* buffer = nullptr;
  if (slot) {
    const Slot& s = g_map.slots[slot];
    buffer = s.buffer;
    desc.Buffer.FirstElement = s.first / 4;
    desc.Buffer.NumElements = static_cast<UINT>(s.bytes / 4);
  }
  const uint64_t index = heap->host_count - kReservedHostViews + slot;
  device->host->CreateUnorderedAccessView(buffer, nullptr, &desc,
                                          {static_cast<SIZE_T>(heap->host_cpu + index * heap->increment)});
}

// The slots of a heap's buffer (g_lock held); 0 when there are no more.
uint32_t add_slots(Device* device, const HeapRecord& record) {
  const uint64_t count = slot_count(record.size);
  if (count == 1 && !g_map.free_slots.empty()) {
    const uint32_t slot = g_map.free_slots.back();
    g_map.free_slots.pop_back();
    g_map.slots[slot] = Slot{record.whole, 0, record.size};
    for (const DescriptorHeap* heap : g_map.heaps) write_slot_view(device, heap, slot);
    c_slots().add(1);
    LIB_LOG(lib::kDebug, "d3d12", "memory slot %u (reused) for a heap of 0x%llx bytes", slot,
            static_cast<unsigned long long>(record.size));
    return slot;
  }
  const uint32_t first = static_cast<uint32_t>(g_map.slots.size());
  if (first + count > kMemoryViews) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      LIB_LOG(lib::kError, "d3d12",
              "more than %u memory slots: further title memory is not reachable by translated programs",
              kMemoryViews - 1);
    }
    return 0;
  }
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t at = i * kSlotStride;
    g_map.slots.push_back(Slot{record.whole, at, std::min(kSlotBytes, record.size - at)});
    for (const DescriptorHeap* heap : g_map.heaps) write_slot_view(device, heap, static_cast<uint32_t>(first + i));
  }
  c_slots().add(static_cast<int64_t>(count));
  LIB_LOG(lib::kDebug, "d3d12", "memory slots %u-%llu for a heap of 0x%llx bytes", first,
          static_cast<unsigned long long>(first + count - 1), static_cast<unsigned long long>(record.size));
  return first;
}

// The level 2 entry of the page holding `title_va` (g_lock held), its table
// made when `make`; null if there is none (or no table is left).
volatile uint32_t* page_entry(uint64_t title_va, bool make) {
  if (!g_map.table || (title_va >> 47)) return nullptr;
  auto* level1 = reinterpret_cast<volatile uint32_t*>(g_map.table);
  const uint64_t window = title_va >> 30;
  uint32_t table = level1[window];
  if (!table) {
    if (!make) return nullptr;
    if (g_map.tables == kPageTables) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        LIB_LOG(lib::kError, "d3d12", "title memory spreads over more than %u GiB windows: the page table is full",
                kPageTables);
      }
      return nullptr;
    }
    table = ++g_map.tables;
    c_page_tables().add(1);
    level1[window] = table;  // the table is zero (no page mapped) until entries are written
  }
  return reinterpret_cast<volatile uint32_t*>(g_map.table + kPageTableLevel1Bytes +
                                              static_cast<uint64_t>(table - 1) * kPageTableTableBytes +
                                              ((title_va >> 16) & 0x3FFF) * 8);
}

// Enters [begin, end) of title addresses, at heap offset `heap_offset` from
// `begin` on in the heap whose first slot is `first_slot` (g_lock held).
void map_pages(uint64_t begin, uint64_t end, uint32_t first_slot, uint64_t heap_offset) {
  if (!first_slot) return;
  LIB_LOG(lib::kTrace, "d3d12", "pages %p-%p: slot %u from heap offset 0x%llx", reinterpret_cast<void*>(begin),
          reinterpret_cast<void*>(end), first_slot, static_cast<unsigned long long>(heap_offset));
  for (uint64_t page = begin & ~0xFFFFull; page < end; page += 0x10000) {
    // The heap offset of the page's start: before the heap for a first page
    // that starts before it (those addresses then fall outside the view).
    const int64_t at = static_cast<int64_t>(heap_offset) + static_cast<int64_t>(page - begin);
    uint64_t index = 0;
    uint32_t view_offset = 0;
    page_slot(at, &index, &view_offset);
    const uint32_t slot = first_slot + static_cast<uint32_t>(index);
    if (slot >= g_map.slots.size() || g_map.slots[slot].buffer != g_map.slots[first_slot].buffer) continue;
    volatile uint32_t* entry = page_entry(page, true);
    if (!entry) return;
    entry[1] = view_offset;
    _ReadWriteBarrier();
    entry[0] = slot;
  }
}

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

// Physical page mappings as xmem changes them (HolyXMemObserveMappings),
// queued (the observer runs under xmem's lock, which this file takes inside
// its own) and applied under g_lock (apply_mapping_events).
struct MappingEvent {
  uintptr_t granule;
  HANDLE section;  // null: unmapped
  uint64_t offset;
};
std::mutex g_events_lock;
std::vector<MappingEvent> g_events;
std::atomic<bool> g_events_pending{false};

// Bumped whenever a title address may stop translating as before (heaps
// released, physical pages mapped or unmapped): translate_gpu_va's per-thread
// cache holds ranges of one generation.
std::atomic<uint64_t> g_generation{1};

void observe_mapping(void*, uintptr_t granule, HANDLE section, uint64_t offset) {
  std::lock_guard<std::mutex> lock(g_events_lock);
  g_events.push_back({granule, section, offset});
  g_events_pending.store(true, std::memory_order_release);
  g_generation.fetch_add(1, std::memory_order_acq_rel);
  texture_memory_changed(granule, 0x10000);  // other storage shows there now
}

using ObserveFn = void (*)(void (*)(void*, uintptr_t, HANDLE, uint64_t), void*);
using EnumerateFn = void (*)(HANDLE, uint64_t, uint64_t, void (*)(void*, uintptr_t, uint64_t), void*);

EnumerateFn enumerate_mapped() {
  static const auto fn = reinterpret_cast<EnumerateFn>(
      GetProcAddress(GetModuleHandleW(L"xmem.dll"), "HolyXMemEnumerateMapped"));
  return fn;
}

bool finish_record(Device* device, ID3D12Heap* heap, HeapRecord* record, bool slots = true) {
  record->heap = heap;
  // Buffers take whole 64 KiB units of a heap (measured, the host's debug
  // layer: heaps over committed stretches can end inside one).
  record->size = heap->GetDesc().SizeInBytes & ~0xFFFFull;
  if (!record->size) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) {
      LIB_LOG(lib::kError, "d3d12", "title memory of 0x%llx bytes is less than a 64 KiB unit: the GPU cannot reach it",
              static_cast<unsigned long long>(heap->GetDesc().SizeInBytes));
    }
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
  if (slots) record->slot = add_slots(device, *record);
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

const lib::Counter& c_heaps_released() {
  static const lib::Counter c("d3d12.gpu_heaps_released");
  return c;
}

// The title releases or decommits [address, address + size) (the runtime's
// release hook): heaps opened over committed memory there are let go of
// first, as the host requires, their pages leaving the page table and their
// slots becoming null views for reuse. Titles release memory once the GPU is
// done with it, as on the console.
// Takes a heap over committed memory out of the memory map (g_lock held):
// its pages leave the page table, its slots become null views for reuse. The
// record goes to `retired` (its heap and buffer released by the caller).
std::map<uintptr_t, std::pair<uintptr_t, HeapRecord>>::iterator retire_locked(
    std::map<uintptr_t, std::pair<uintptr_t, HeapRecord>>::iterator it, std::vector<HeapRecord>* retired) {
  const uintptr_t base = it->second.first;
  const HeapRecord record = it->second.second;
  const uint64_t slots = record.slot ? slot_count(record.size) : 0;
  for (uint64_t page = base & ~0xFFFFull; page < base + record.size; page += 0x10000) {
    volatile uint32_t* entry = page_entry(page, false);
    if (entry && entry[0] >= record.slot && entry[0] < record.slot + slots) entry[0] = 0;
  }
  for (uint64_t i = 0; i < slots; ++i) {
    const auto slot = static_cast<uint32_t>(record.slot + i);
    g_map.slots[slot] = Slot{};
    for (const DescriptorHeap* heap : g_map.heaps) write_slot_view(g_map.device, heap, slot);
    g_map.free_slots.push_back(slot);
  }
  c_slots().add(-static_cast<int64_t>(slots));
  LIB_LOG(lib::kDebug, "d3d12", "heap over %p (0x%llx bytes, slot %u) released with the memory",
          reinterpret_cast<void*>(base), static_cast<unsigned long long>(record.size), record.slot);
  retired->push_back(record);
  g_generation.fetch_add(1, std::memory_order_acq_rel);
  return region_heaps().erase(it);
}

void release_retired(const std::vector<HeapRecord>& retired) {
  for (const HeapRecord& record : retired) {
    record.whole->Release();
    record.heap->Release();
  }
  if (!retired.empty()) c_heaps_released().add(static_cast<int64_t>(retired.size()));
}

void release_title_memory(const void* address, size_t size) {
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const uintptr_t end = begin + size;
  texture_memory_released(begin, size);
  std::vector<HeapRecord> retired;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = region_heaps().upper_bound(begin);
    while (it != region_heaps().end() && it->second.first < end) it = retire_locked(it, &retired);
  }
  release_retired(retired);
}

// The page table (g_lock held), made on first use: a buffer in system memory
// the host allocates, write-back and mapped for the CPU's entries, which work
// submitted after them sees. Measured (PLATFORM_NOTES.md): the GPU caches
// such memory, not process memory a heap is opened over, and every translated
// memory access reads two entries. Returns its host GPU address.
uint64_t ensure_page_table(Device* device) {
  if (g_map.table || g_map.failed) return g_map.host_va;
  g_map.failed = true;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_CUSTOM;
  heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kPageTableBytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* buffer = nullptr;
  void* memory = nullptr;
  HRESULT hr = device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&buffer));
  const D3D12_RANGE nothing_read{0, 0};
  if (SUCCEEDED(hr)) hr = buffer->Map(0, &nothing_read, &memory);  // kept mapped (zeroed: no page mapped)
  if (FAILED(hr) || !memory) {
    LIB_LOG(lib::kError, "d3d12", "no page table for translated programs (0x%08lX)", static_cast<unsigned long>(hr));
    if (buffer) buffer->Release();
    return 0;
  }
  buffer->SetName(L"holy page table");
  g_map.failed = false;
  g_map.table = static_cast<uint8_t*>(memory);
  g_map.host_va = buffer->GetGPUVirtualAddress();
  g_table.store(g_map.table, std::memory_order_release);
  g_table_va.store(g_map.host_va, std::memory_order_release);
  g_map.device = device;
  HolySetReleaseHook(&release_title_memory);
  if (const auto observe = reinterpret_cast<ObserveFn>(
          GetProcAddress(GetModuleHandleW(L"xmem.dll"), "HolyXMemObserveMappings"))) {
    observe(&observe_mapping, nullptr);
  }
  return g_map.host_va;
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
  // Every title address already showing storage of the span.
  if (const auto enumerate = enumerate_mapped()) {
    struct Visit {
      uint32_t slot;
      uint64_t first;
    } visit{heap_record.slot, first};
    enumerate(
        section, first, first + heap_record.size,
        [](void* context, uintptr_t granule, uint64_t offset) {
          const auto* v = static_cast<const Visit*>(context);
          map_pages(granule, granule + 0x10000, v->slot, offset - v->first);
        },
        &visit);
  }
  return &record.spans.back();
}

// Applies the queued mapping events (g_lock held): granules showing storage
// a span covers are entered, others (unmapped, or remapped to storage no span
// covers yet) leave the page table.
void apply_mapping_events() {
  if (!g_events_pending.load(std::memory_order_acquire)) return;
  std::vector<MappingEvent> events;
  {
    std::lock_guard<std::mutex> lock(g_events_lock);
    events.swap(g_events);
    g_events_pending.store(false, std::memory_order_relaxed);
  }
  for (const MappingEvent& event : events) {
    const std::pair<uint64_t, HeapRecord>* covering = nullptr;
    if (event.section) {
      const auto found = sections().find(event.section);
      if (found != sections().end()) {
        for (const auto& span : found->second.spans) {
          if (event.offset >= span.first && event.offset + 0x10000 <= span.first + span.second.size) {
            covering = &span;
            break;
          }
        }
      }
    }
    if (covering) {
      map_pages(event.granule, event.granule + 0x10000, covering->second.slot, event.offset - covering->first);
    } else if (volatile uint32_t* entry = page_entry(event.granule, false)) {
      entry[0] = 0;
    }
  }
}

}  // namespace

bool locate(Device* device, uint64_t title_va, Backing* out, uint64_t size) {
  return locate_range(device, title_va, size, out);
}

bool locate_range(Device* device, uint64_t title_va, uint64_t size, Backing* out) {
  std::lock_guard<std::mutex> lock(g_lock);
  ID3D12Device3* d3 = device3(device);
  if (!d3) return false;
  ensure_page_table(device);  // before any heap, so every heap's pages are entered
  apply_mapping_events();

  // Physical pages the title mapped: `run` bytes from the address are pages
  // laid out in order in one section.
  HANDLE section = nullptr;
  uint64_t offset = 0, run = 0;
  if (query_physical() && query_physical()(reinterpret_cast<const void*>(title_va), &section, &offset, &run)) {
    const uint64_t wanted = std::max<uint64_t>(std::min(size ? size : 1, run), 1);
    const auto* span = section_span(device, section, offset, offset + wanted);
    if (!span) return false;
    const uint64_t at = offset - span->first;
    const uint64_t contiguous = std::min(run, span->second.size - at);
    *out = Backing{span->second.heap, at, contiguous, span->second.host_gpu_va + at,
                   span->second.whole, 0, span->second.size, span->second.slot};
    map_pages(title_va, title_va + contiguous, span->second.slot, at);
    return true;
  }

  // Committed memory the title allocated.
  auto region = region_heaps().upper_bound(title_va);
  if (region != region_heaps().end() && title_va >= region->second.first) {
    // Still the allocation the heap was opened over? Memory released without
    // the runtime hearing of it and allocated again at the same address would
    // otherwise reach the old heap's pages (the GPU then reads and writes
    // memory the title no longer has).
    MEMORY_BASIC_INFORMATION now{};
    if (VirtualQuery(reinterpret_cast<void*>(title_va), &now, sizeof(now)) && now.State == MEM_COMMIT &&
        reinterpret_cast<uint64_t>(now.AllocationBase) == region->second.second.allocation) {
      const uint64_t at = title_va - region->second.first;
      *out = Backing{region->second.second.heap, at, region->first - title_va, region->second.second.host_gpu_va + at,
                     region->second.second.whole, region->second.first, region->second.second.size,
                     region->second.second.slot};
      return true;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      LIB_LOG(lib::kError, "d3d12",
              "the GPU heap over %p was still recorded after its memory was released (allocation %p, now %p, "
              "state 0x%lx): opened again",
              reinterpret_cast<void*>(region->second.first),
              reinterpret_cast<void*>(region->second.second.allocation), now.AllocationBase, now.State);
    }
    std::vector<HeapRecord> retired;
    retire_locked(region, &retired);
    release_retired(retired);
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
  record.allocation = allocation;
  region_heaps()[base + record.size] = std::make_pair(base, record);
  map_pages(base, base + record.size, record.slot, 0);
  const uint64_t at = title_va - base;
  *out = Backing{heap, at, record.size - at, record.host_gpu_va + at, record.whole, base, record.size, record.slot};
  return true;
}

// Called for the system constants of every translated call, vertex buffers
// and root views: ranges translated before are found without the lock in a
// small per-thread cache (cleared by g_generation).
uint64_t translate_gpu_va(Device* device, uint64_t title_va) {
  if (!title_va) return 0;
  struct Range {
    uint64_t begin = 0, end = 0, host = 0, generation = 0;
  };
  thread_local Range cache[8];
  thread_local uint32_t next = 0;
  const uint64_t generation = g_generation.load(std::memory_order_acquire);
  for (const Range& range : cache) {
    if (range.generation == generation && title_va >= range.begin && title_va < range.end) {
      return range.host + (title_va - range.begin);
    }
  }
  static const lib::Counter slow("d3d12.address_translations_locked");
  slow.add(1);
  Backing backing;
  if (!locate(device, title_va, &backing, 1)) return 0;
  if (backing.contiguous) cache[next++ % 8] = Range{title_va, title_va + backing.contiguous, backing.host_gpu_va, generation};
  return backing.host_gpu_va;
}

uint64_t page_table_address(Device* device) {
  const uint64_t made = g_table_va.load(std::memory_order_acquire);
  if (made && !g_events_pending.load(std::memory_order_acquire)) return made;
  std::lock_guard<std::mutex> lock(g_lock);
  const uint64_t address = ensure_page_table(device);
  apply_mapping_events();
  return address;
}

bool page_mapped(uint64_t title_va) {
  const uint8_t* table = g_table.load(std::memory_order_acquire);
  if (!table || (title_va >> 47)) return false;
  const uint32_t number = reinterpret_cast<const volatile uint32_t*>(table)[title_va >> 30];
  if (!number) return false;
  const auto* entry = reinterpret_cast<const volatile uint32_t*>(
      table + kPageTableLevel1Bytes + static_cast<uint64_t>(number - 1) * kPageTableTableBytes +
      ((title_va >> 16) & 0x3FFF) * 8);
  return entry[0] != 0;
}

void reach(Device* device, uint64_t title_va) {
  if (!title_va || page_mapped(title_va)) return;
  if (!page_table_address(device)) return;
  Backing backing;
  locate(device, title_va, &backing, 64ull << 20);
}

bool map_alias(Device* device, uint64_t alias, uint64_t size, uint64_t target) {
  if (!page_table_address(device)) return false;
  for (uint64_t at = 0; at < size;) {
    Backing backing;
    if (!locate(device, target + at, &backing, size - at) || !backing.contiguous) {
      LIB_LOG(lib::kError, "d3d12", "alias %p of 0x%llx bytes: memory at %p (+0x%llx) has no GPU heap",
              reinterpret_cast<void*>(alias), static_cast<unsigned long long>(size),
              reinterpret_cast<void*>(target), static_cast<unsigned long long>(at));
      return false;
    }
    const uint64_t n = std::min(backing.contiguous, size - at);
    {
      std::lock_guard<std::mutex> lock(g_lock);
      map_pages(alias + at, alias + at + n, backing.slot, backing.heap_offset);
    }
    at += n;
  }
  return true;
}

bool map_host_buffer(Device* device, ID3D12Resource* buffer, uint64_t alias, uint64_t size) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!ensure_page_table(device) || !buffer || (alias & 0xFFFF)) return false;
  HeapRecord record;
  record.whole = buffer;
  record.size = size;
  const uint32_t slot = add_slots(device, record);
  if (!slot) return false;
  map_pages(alias, alias + size, slot, 0);
  return true;
}

void unmap_range(uint64_t title_va, uint64_t size) {
  std::lock_guard<std::mutex> lock(g_lock);
  for (uint64_t page = title_va & ~0xFFFFull; page < title_va + size; page += 0x10000) {
    if (volatile uint32_t* entry = page_entry(page, false)) entry[0] = 0;
  }
}

void add_view_heap(Device* device, DescriptorHeap* heap) {
  if (!heap || heap->host_count <= kReservedHostViews) return;
  std::lock_guard<std::mutex> lock(g_lock);
  for (uint32_t slot = 0; slot < g_map.slots.size(); ++slot) write_slot_view(device, heap, slot);
  g_map.heaps.push_back(heap);
}

void remove_view_heap(DescriptorHeap* heap) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_map.heaps.erase(std::remove(g_map.heaps.begin(), g_map.heaps.end(), heap), g_map.heaps.end());
}

uint64_t memory_views(const DescriptorHeap* heap) {
  return heap->host_gpu + static_cast<uint64_t>(heap->host_count - kReservedHostViews) * heap->increment;
}

}  // namespace holy::d3d12
