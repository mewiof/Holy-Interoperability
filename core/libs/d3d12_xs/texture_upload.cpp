#include "core/libs/d3d12_xs/texture_upload.h"

#include "core/common/memory_query.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/page_cache.h"
#include "core/libs/d3d12_xs/write_watch.h"
#include "core/libs/gpu_layout.h"
#include "core/libs/support.h"
#include "core/libs/texture_layout.h"

namespace holy::d3d12 {
namespace {

namespace gpu = lib::gpu;

// Microseconds the title's threads spend in this file's work (measurement).
struct TimeIn {
  const lib::Counter& counter;
  LARGE_INTEGER start;
  explicit TimeIn(const lib::Counter& c) : counter(c) { QueryPerformanceCounter(&start); }
  ~TimeIn() {
    static const int64_t frequency = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return f.QuadPart;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    counter.add((now.QuadPart - start.QuadPart) * 1000000 / frequency);
  }
};


// The layout a texture has in title memory: the console GPU's
// (gpu_layout.h), as the layout library reports it to titles and titles
// stream textures in. False where it does not cover the description (formats
// of no power-of-two element size, depth with stencil): such memory is read
// as the desktop's copyable footprints.
bool console_layout(const D3D12_RESOURCE_DESC& desc, gpu::Texture* out) {
  static const bool off = [] {
    wchar_t text[4] = {};
    return GetEnvironmentVariableW(L"HOLY_TEXTURE_DETILE", text, 4) && text[0] == L'0';
  }();
  if (off) return false;
  return gpu::texture_from_desc(static_cast<uint32_t>(desc.Dimension), desc.Width, desc.Height, desc.DepthOrArraySize,
                                desc.MipLevels, static_cast<uint32_t>(desc.Format), desc.SampleDesc.Count,
                                static_cast<uint32_t>(desc.Layout), static_cast<uint32_t>(desc.Flags), out) &&
         out->plane_count == 1 && out->planes[0].addressable;
}

const lib::Counter& c_detiled() {
  static const lib::Counter counter("d3d12.textures_detiled");
  return counter;
}
const lib::Counter& c_detiled_bytes() {
  static const lib::Counter counter("d3d12.texture_bytes_detiled");
  return counter;
}

using Key = std::tuple<uint64_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;

struct Uploader {
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  std::atomic<uint64_t> value{0};
  std::vector<ID3D12Resource*> staging;  // released once the work submitted with them is done
  // Upload memory reused for every list: mapped chunks of an upload heap,
  // handed out in order and free again once the list's work is done.
  struct Chunk {
    ID3D12Resource* buffer;
    uint8_t* cpu;
  };
  std::vector<Chunk> chunks;
  size_t chunk = 0;
  uint64_t used = 0;
  // The list takes copies of new textures until the title's next submission
  // (refresh_textures), which submits it and has the title's queue wait.
  bool open = false;
};

constexpr uint64_t kChunkBytes = 32ull << 20;

// A host texture views by address find: filled from title memory
// (texture_at), or the host texture of a resource the title placed or
// created (placed_texture, register_texture), which the GPU writes.
struct Record {
  ID3D12Resource* texture = nullptr;  // the registry's reference
  uint64_t address = 0;
  D3D12_RESOURCE_DESC desc{};
  uint64_t size = 0;         // bytes in title memory
  bool from_memory = false;  // filled from title memory: kept current
  // The state the texture rests in between fills: textures viewed by address
  // are shader resources; placed textures following memory stay common (the
  // title's barriers on them keep that layout), which copies and shader reads
  // both allow.
  D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  bool protected_watch = false;  // its memory is watched by page protection (write_watch.h)
  bool tracked = false;          // its allocation's writes are tracked by the host
  bool dirty = false;
  // What changed since the last fill, as byte spans from the texture's start;
  // `whole`: all of it (or not known).
  bool whole = true;
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  // When a copy of another shape was made over its memory (0: not): the
  // title reused the memory, so this copy is likely stale.
  uint64_t superseded_at = 0;
};

// Allocations holding textures filled from their memory whose writes the host
// tracks (xmem reserves title memory with MEM_WRITE_WATCH), by base.
struct Tracked {
  uint64_t size = 0;
  std::vector<Record*> records;
};

// The registry, its tracking and the uploader.
std::mutex g_lock;
std::map<Key, Record*>& textures() {
  static auto* map = new std::map<Key, Record*>();
  return *map;
}
std::map<uint64_t, Tracked>& tracked() {
  static auto* map = new std::map<uint64_t, Tracked>();
  return *map;
}
std::vector<Record*> g_dirty;  // under g_lock
uint64_t g_largest = 0;         // the largest texture filled from memory, for range searches
ULONGLONG g_last_scan = 0;

// Changes made without a CPU write the host tracks (pages remapped, memory
// released, writes into untracked memory announced), queued under a leaf lock
// (they are reported under other libraries' locks) and applied under g_lock.
std::mutex g_changes_lock;
struct Change {
  uint64_t begin;
  uint64_t size;
  bool released;  // the title released the memory (else it changed)
};
std::vector<Change> g_changes;
// Host textures of records dropped because their memory was released, let go
// of once the GPU finished the work submitted before (it may read them).
struct Retired {
  GpuEpoch epoch;
  ID3D12Resource* texture;
  uint64_t bytes;  // in title memory (the live count's share)
};
std::vector<Retired> g_retired;  // under g_lock, oldest first
std::vector<Record*> g_superseded;  // under g_lock, oldest first
struct Uploader;
Uploader* g_uploader = nullptr;  // once made
uint64_t submit_upload(Uploader* up);
bool upload_open();
std::atomic<bool> g_memory_pressure{false};
std::atomic<bool> g_any_change{false};

const lib::Counter& c_refreshes() {
  static const lib::Counter c("d3d12.texture_refreshes");
  return c;
}
const lib::Counter& c_untracked() {
  static const lib::Counter c("d3d12.textures_untracked");
  return c;
}
// Host textures filled from title memory the runtime holds, and their bytes
// in title memory (live: they fall as the textures are let go of).
const lib::Counter& c_live_textures() {
  static const lib::Counter c("d3d12.host_textures_from_memory");
  return c;
}
const lib::Counter& c_live_texture_bytes() {
  static const lib::Counter c("d3d12.host_texture_bytes_from_memory");
  return c;
}

// Lets go of a host texture filled from title memory (g_lock held).
void release_filled(ID3D12Resource* texture, uint64_t bytes) {
  texture->Release();
  c_live_textures().add(-1);
  c_live_texture_bytes().add(-static_cast<int64_t>(bytes));
}

// HOLY_TEXTURE_TRACKING=0 turns tracking off (textures are copied once).
bool tracking() {
  static const bool enabled = [] {
    wchar_t text[8] = {};
    const bool off = GetEnvironmentVariableW(L"HOLY_TEXTURE_TRACKING", text, 8) && text[0] == L'0';
    if (!off) {
      start_write_watch(&texture_memory_changed, [](uint64_t page) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8) {
          LIB_LOG(lib::kWarn, "d3d12",
                  "a write to %p faulted on a read-only page no watch holds (a watch's protection left behind): made "
                  "writable and taken as a write",
                  reinterpret_cast<void*>(page));
        }
      });
      // Writes that do not fault (the kernel's): watches over the memory go
      // now, before the write (a read-only page would make it fail).
      HolySetWriteHook([](const void* address, size_t size) {
        forget_memory(reinterpret_cast<uint64_t>(address), size);
        texture_memory_changed(reinterpret_cast<uint64_t>(address), size);
      });
    }
    return !off;
  }();
  return enabled;
}

// The allocation holding `address`, whether the host tracks its writes, and
// whether it is a mapped view (physical pages).
// (The host's record of the allocation, memory_query.h: VirtualQuery walks of
// large committed allocations take milliseconds.)
bool allocation_of(uint64_t address, uint64_t* base, uint64_t* size, bool* watched, bool* mapped) {
  AllocationExtent extent;
  if (!query_allocation(reinterpret_cast<const void*>(address), &extent)) return false;
  *base = extent.base;
  *size = extent.size;
  *mapped = extent.mapped;
  // Fails for memory reserved without write tracking (mapped views: physical
  // pages). One page of a tracked allocation answers as well as all of it.
  void* page = nullptr;
  ULONG_PTR count = 1;
  DWORD granularity = 0;
  *watched = !extent.mapped &&
             GetWriteWatch(0, reinterpret_cast<void*>(address & ~0xFFFull), 0x1000, &page, &count, &granularity) == 0;
  return true;
}

// Marks the bytes [begin, end) of title memory changed in `record`'s texture.
void mark(Record* record, uint64_t begin, uint64_t end) {
  if (!record->texture || !record->from_memory) return;
  if (!record->whole) {
    const uint64_t from = begin > record->address ? begin - record->address : 0;
    const uint64_t to = std::min(end - record->address, record->size);
    auto& spans = record->spans;
    if (!spans.empty() && from <= spans.back().second && to >= spans.back().first) {
      spans.back() = {std::min(spans.back().first, from), std::max(spans.back().second, to)};
    } else if (spans.size() < 64) {
      spans.emplace_back(from, to);
    } else {
      record->whole = true;  // too many pieces to be worth keeping apart
      spans.clear();
    }
  }
  if (record->dirty) return;
  record->dirty = true;
  g_dirty.push_back(record);
}

// Marks the textures filled from memory over [begin, end) (g_lock held).
void mark_range(uint64_t begin, uint64_t end) {
  auto it = textures().lower_bound(Key{begin > g_largest ? begin - g_largest : 0, 0, 0, 0, 0, 0, 0});
  for (; it != textures().end() && std::get<0>(it->first) < end; ++it) {
    Record* record = it->second;
    if (record->from_memory && record->address + record->size > begin) mark(record, begin, end);
  }
}

// Asks the host which pages of [begin, end) in a tracked allocation were
// written since they were last asked about, and marks the textures over them
// (g_lock held).
void scan_range(uint64_t begin, uint64_t end) {
  static void* pages[4096];
  begin &= ~0xFFFull;
  for (;;) {
    ULONG_PTR count = sizeof(pages) / sizeof(pages[0]);
    DWORD granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(begin), static_cast<SIZE_T>(end - begin), pages,
                      &count, &granularity) != 0) {
      return;  // released since
    }
    for (ULONG_PTR i = 0; i < count; ++i) {
      const auto page = reinterpret_cast<uint64_t>(pages[i]);
      page_cache_written(page, granularity);  // the reset hides it from the page cache's own scan
      // The records over the page, from the address-ordered index (a walk
      // of the allocation's records per page was quadratic: tens of
      // thousands of texture copies in one pool, measured at most of the
      // submission thread's time during loading).
      mark_range(page, page + granularity);
    }
    if (count < sizeof(pages) / sizeof(pages[0])) return;
  }
}

void drop(Record* record, bool later = false);

// Drops the records filled from memory over [begin, end) (g_lock held): the
// title released the memory, so the textures there are gone; a texture
// viewed there later is a new one, filled from the memory then.
void drop_range(uint64_t begin, uint64_t end) {
  auto it = textures().lower_bound(Key{begin > g_largest ? begin - g_largest : 0, 0, 0, 0, 0, 0, 0});
  while (it != textures().end() && std::get<0>(it->first) < end) {
    Record* record = it->second;
    if (record->from_memory && record->address + record->size > begin) {
      drop(record, true);
      it = textures().erase(it);
    } else {
      ++it;
    }
  }
}

// Lets go of the retired host textures the GPU is done with (g_lock held).
// Epochs are taken in order: once one is not done, later ones are not either.
void release_retired() {
  size_t done = 0;
  while (done < g_retired.size() && gpu_epoch_done(g_retired[done].epoch)) {
    release_filled(g_retired[done].texture, g_retired[done].bytes);
    ++done;
  }
  g_retired.erase(g_retired.begin(), g_retired.begin() + static_cast<std::ptrdiff_t>(done));
}

// Applies the queued changes (g_lock held).
void apply_changes() {
  if (!g_any_change.load(std::memory_order_acquire)) return;
  std::vector<Change> changes;
  {
    std::lock_guard<std::mutex> guard(g_changes_lock);
    changes.swap(g_changes);
    g_any_change.store(false, std::memory_order_relaxed);
  }
  for (const Change& change : changes) {
    if (change.released) {
      drop_range(change.begin, change.begin + change.size);
    } else {
      mark_range(change.begin, change.begin + change.size);
      // Storage that changed under one address changed under its aliases
      // too (another view of the same physical pages was backed, remapped
      // or written).
      for_each_alias(change.begin, change.size, [](uint64_t alias, uint64_t granule, bool) {
        (void)granule;
        mark_range(alias, alias + 0x10000);
        forget_memory(alias, 0x10000);
      });
    }
    forget_memory(change.begin, change.size);  // the pages lose their watches (refreshed records watch again)
  }
}

// Applies queued changes and, at most every 16 ms (about once a frame), asks
// the host which tracked pages were written: per tracked allocation, one
// query per stretch its textures cover (g_lock held). Measured (development
// host): a query costs about 0.55 us per MiB it covers, about 0.2 us for a
// small one, so textures scattered over a pool of gigabytes are asked about
// where they are, not over the span between them (about 1 ms a scan).
void scan() {
  apply_changes();
  release_retired();
  // Asking the host which pages were written costs time in proportion to the
  // memory asked about (measured: 6.8 s of every 10 s on the submitting
  // thread during a title's loading, with copies over a large texture pool
  // scanned every 16 ms): the next scan waits ten times as long as this one
  // took, at least 16 ms, so scanning takes at most a tenth of the time.
  static uint64_t interval_ms = 16;
  const ULONGLONG now = GetTickCount64();
  if (now - g_last_scan < interval_ms) return;
  g_last_scan = now;
  struct Interval {
    ULONGLONG started;
    ~Interval() { interval_ms = std::max<uint64_t>(16, (GetTickCount64() - started) * 10); }
  } adapt{now};
  std::vector<std::pair<uint64_t, uint64_t>> stretches;
  for (auto& [base, allocation] : tracked()) {
    if (allocation.records.empty()) continue;
    stretches.clear();
    for (const Record* record : allocation.records) stretches.emplace_back(record->address, record->address + record->size);
    std::sort(stretches.begin(), stretches.end());
    constexpr uint64_t kJoin = 256ull << 10;  // gaps below this cost less than another query
    uint64_t first = stretches.front().first, last = stretches.front().second;
    for (size_t i = 1; i < stretches.size(); ++i) {
      if (stretches[i].first <= last + kJoin) {
        last = std::max(last, stretches[i].second);
        continue;
      }
      scan_range(first, last);
      first = stretches[i].first;
      last = stretches[i].second;
    }
    scan_range(first, last);
  }
}

// Starts keeping a record filled from memory current (g_lock held): the host
// tracks writes to its allocation, or its physical pages are watched through
// the title's view. Memory neither applies to yet (physical pages not mapped
// yet) is tried again when the record is refreshed.
void track(Record* record) {
  if (!tracking() || record->tracked || record->protected_watch) return;
  uint64_t base = 0, allocation_size = 0;
  bool watched = false, mapped = false;
  if (!allocation_of(record->address, &base, &allocation_size, &watched, &mapped)) return;
  if (watched) {
    Tracked& allocation = tracked()[base];
    allocation.size = allocation_size;
    // Writes to its pages so far go to the textures already there.
    scan_range(record->address, record->address + record->size);
    allocation.records.push_back(record);
    record->tracked = true;
  } else if (mapped && watch_memory(record->address, record->size, reinterpret_cast<WatchToken>(record))) {
    record->protected_watch = true;
  }
}

// The registry lets go of a record (g_lock held); `later`: its host texture
// goes once the GPU finished the work submitted until now.
void drop(Record* record, bool later) {
  // Copies into it may wait in the open upload list: submitted first, and
  // the texture let go of once that work is done.
  if (record->texture && record->from_memory && upload_open()) {
    submit_upload(g_uploader);
    later = true;
  }
  if (record->superseded_at) {
    g_superseded.erase(std::remove(g_superseded.begin(), g_superseded.end(), record), g_superseded.end());
  }
  if (record->protected_watch) unwatch(reinterpret_cast<WatchToken>(record));
  g_dirty.erase(std::remove(g_dirty.begin(), g_dirty.end(), record), g_dirty.end());
  for (auto& [base, allocation] : tracked()) {
    auto& list = allocation.records;
    list.erase(std::remove(list.begin(), list.end(), record), list.end());
  }
  if (record->texture && record->from_memory) {
    if (later) {
      g_retired.push_back({gpu_epoch_now(), record->texture, record->size});
    } else {
      release_filled(record->texture, record->size);
    }
  } else if (record->texture) {
    record->texture->Release();
  }
  delete record;
}

lib::TextureShape shape_of(const D3D12_RESOURCE_DESC& desc);
Key key_of(uint64_t address, const lib::TextureShape& shape);

// Marks the copies over [address, address + size) superseded by a new copy
// there (g_lock held); copies of exactly the same memory (another format of
// it) are not.
void supersede_over(uint64_t address, uint64_t size) {
  auto it = textures().lower_bound(Key{address > g_largest ? address - g_largest : 0, 0, 0, 0, 0, 0, 0});
  for (; it != textures().end() && std::get<0>(it->first) < address + size; ++it) {
    Record* other = it->second;
    if (!other->from_memory || other->superseded_at || other->address + other->size <= address) continue;
    if (other->address == address && other->size == size) continue;
    other->superseded_at = GetTickCount64();
    g_superseded.push_back(other);
  }
}

// Under video memory pressure, lets go of copies superseded a while ago
// (g_lock held): the title reused their memory, and a descriptor still naming
// one would read memory the console would show the new texture in. Released
// once the GPU is done with work recorded so far.
void retire_superseded() {
  constexpr uint64_t kGraceMs = 3000;
  if (!g_memory_pressure.load(std::memory_order_relaxed) || g_superseded.empty()) return;
  static const lib::Counter retired("d3d12.host_textures_superseded_released");
  const uint64_t now = GetTickCount64();
  size_t done = 0;
  while (done < g_superseded.size() && done < 512 && now - g_superseded[done]->superseded_at > kGraceMs) ++done;
  if (!done) return;
  std::vector<Record*> going(g_superseded.begin(), g_superseded.begin() + static_cast<std::ptrdiff_t>(done));
  g_superseded.erase(g_superseded.begin(), g_superseded.begin() + static_cast<std::ptrdiff_t>(done));
  for (Record* record : going) {
    record->superseded_at = 0;  // off the list already
    auto found = textures().find(key_of(record->address, shape_of(record->desc)));
    if (found == textures().end() || found->second != record) continue;
    textures().erase(found);
    drop(record, true);
    retired.add(1);
  }
}

bool make_uploader(Device* device, Uploader* out) {
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  const bool made = SUCCEEDED(device->host->CreateCommandQueue(&queue, IID_PPV_ARGS(&out->queue))) &&
         SUCCEEDED(device->host->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&out->allocator))) &&
         SUCCEEDED(device->host->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, out->allocator, nullptr,
                                                   IID_PPV_ARGS(&out->list))) &&
         SUCCEEDED(out->list->Close()) &&
         SUCCEEDED(device->host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&out->fence))) &&
         (out->event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr;
  if (made) {
    out->queue->SetName(L"holy texture upload queue");
    out->list->SetName(L"holy texture upload");
    register_gpu_timeline(out->fence, &out->value);  // its copies read title memory
  }
  return made;
}

Uploader* uploader(Device* device) {
  static Uploader* instance = [&] {
    auto* made = new Uploader();
    if (!make_uploader(device, made)) {
      LIB_LOG(lib::kError, "d3d12", "cannot create the texture upload queue");
      return static_cast<Uploader*>(nullptr);
    }
    g_uploader = made;
    return made;
  }();
  return instance;
}

// Waits for the uploader's submitted work and resets its list (g_lock held).
void begin_upload(Uploader* up) {
  if (up->fence->GetCompletedValue() < up->value) {
    up->fence->SetEventOnCompletion(up->value, up->event);
    WaitForSingleObject(up->event, INFINITE);
  }
  for (ID3D12Resource* buffer : up->staging) buffer->Release();
  up->staging.clear();
  up->chunk = 0;
  up->used = 0;
  up->allocator->Reset();
  up->list->Reset(up->allocator, nullptr);
}

bool upload_open() {
  return g_uploader && g_uploader->open;
}

// Opens the uploader's list for recording if it is not open (g_lock held).
void open_upload(Uploader* up) {
  if (up->open) return;
  begin_upload(up);
  up->open = true;
}

// Submits the uploader's list; returns the fence value it signals.
uint64_t submit_upload(Uploader* up) {
  up->open = false;
  up->list->Close();
  ID3D12CommandList* lists[] = {up->list};
  up->queue->ExecuteCommandLists(1, lists);
  up->queue->Signal(up->fence, ++up->value);
  check_point("texture upload");
  return up->value;
}

lib::TextureShape shape_of(const D3D12_RESOURCE_DESC& desc);

// Bytes the texture takes in title memory.
uint64_t memory_size(const D3D12_RESOURCE_DESC& desc) {
  gpu::Texture t;
  return console_layout(desc, &t) ? t.size : lib::texture_size(shape_of(desc));
}

lib::TextureShape shape_of(const D3D12_RESOURCE_DESC& desc) {
  lib::TextureShape shape;
  shape.dimension = static_cast<uint32_t>(desc.Dimension);
  shape.width = desc.Width;
  shape.height = desc.Height;
  shape.depth_or_array = desc.DepthOrArraySize ? desc.DepthOrArraySize : 1;
  shape.mips = desc.MipLevels ? desc.MipLevels : 1;
  shape.format = static_cast<uint32_t>(desc.Format);
  return shape;
}

Key key_of(uint64_t address, const lib::TextureShape& shape) {
  return Key{address, shape.dimension, shape.width, shape.height, shape.depth_or_array, shape.mips, shape.format};
}

// Whether `size` bytes from `address` are committed, readable title memory.
// Whether `size` bytes from `address` lie in private memory committed whole
// (the host's record of the allocation, memory_query.h): such memory needs no
// commit before it is read. Committed is not readable: titles keep no-access
// pages inside committed allocations (observed), so reads go through
// copy_title_memory.
bool committed_whole(uint64_t address, uint64_t size) {
  AllocationExtent extent;
  return query_allocation(reinterpret_cast<const void*>(address), &extent) && !extent.mapped &&
         extent.committed == extent.size && address + size <= extent.base + extent.size;
}

// Copies title memory, false when part of it cannot be read (the access
// faults; memory committed on first touch is committed by the runtime's own
// handler first). No C++ objects here: structured exception handling. The
// copy is an opaque call: the compiler takes an inlined copy for one that
// cannot fault and drops the handler around it (observed: the fault went
// unhandled).
void* (*volatile g_copy)(void*, const void*, size_t) = &std::memcpy;
bool copy_title_memory(void* to, const void* from, size_t size) {
  __try {
    g_copy(to, from, size);
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

bool readable(uint64_t address, uint64_t size) {
  const uint64_t end = address + size;
  while (address < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
    const DWORD protect = info.Protect & 0xFF;
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || protect == PAGE_NOACCESS ||
        protect == PAGE_EXECUTE) {
      return false;
    }
    address = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

ID3D12Resource* upload_buffer(Device* device, uint64_t size) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* buffer = nullptr;
  if (FAILED(device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)))) {
    return nullptr;
  }
  buffer->SetName(L"holy detiled texture staging");
  return buffer;
}

// Upload memory for `size` bytes (512-byte aligned) in the uploader's
// chunks, or a buffer of its own when large; released with the list's work.
bool upload_space(Device* device, Uploader* up, uint64_t size, ID3D12Resource** buffer, uint64_t* offset,
                  uint8_t** cpu) {
  size = lib::align_up(size, 512);
  if (size > kChunkBytes / 4) {
    ID3D12Resource* own = upload_buffer(device, size);
    void* mapped = nullptr;
    if (!own || FAILED(own->Map(0, nullptr, &mapped))) {
      if (own) own->Release();
      return false;
    }
    own->SetName(L"holy large texture staging");
    up->staging.push_back(own);
    *buffer = own;
    *offset = 0;
    *cpu = static_cast<uint8_t*>(mapped);
    return true;
  }
  if (up->chunk < up->chunks.size() && up->used + size > kChunkBytes) {
    ++up->chunk;
    up->used = 0;
  }
  if (up->chunk == up->chunks.size()) {
    ID3D12Resource* made = upload_buffer(device, kChunkBytes);
    void* mapped = nullptr;
    if (!made || FAILED(made->Map(0, nullptr, &mapped))) {
      if (made) made->Release();
      return false;
    }
    made->SetName(L"holy texture upload memory");
    up->chunks.push_back({made, static_cast<uint8_t*>(mapped)});
  }
  *buffer = up->chunks[up->chunk].buffer;
  *offset = up->used;
  *cpu = up->chunks[up->chunk].cpu + up->used;
  up->used += size;
  return true;
}

// Rows [y0, y1) of one subresource of a texture in the console's layout at
// `address`, into `out` (gpu::detile_rows); false when the title memory
// cannot be read (the access faults; memory committed on first touch is
// committed by the runtime's handler first). No C++ objects here
// (structured exception handling); the call is opaque so the handler stays
// around it.
bool (*volatile g_detile_rows)(const gpu::Layout&, const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint8_t*, uint64_t, uint64_t) = &gpu::detile_rows;
bool detile_title_rows(const gpu::Layout& layout, uint64_t address, uint64_t size, uint32_t mip, uint32_t slice,
                       uint32_t y0, uint32_t y1, uint8_t* out, uint64_t row_pitch, uint64_t depth_pitch) {
  __try {
    return g_detile_rows(layout, reinterpret_cast<const uint8_t*>(address), size, mip, slice, y0, y1, out, row_pitch,
                         depth_pitch);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

// The element rows of level `mip`, array slice `slice` that the record's
// changed spans reach, as [*y0, *y1); false if none. Rows of whole blocks
// (a block is a run of rows in a level); levels in the mip tail, linear
// levels' partial rows and 3D textures are taken whole.
bool changed_rows(const Record& record, const gpu::Layout& l, uint32_t mip, uint32_t slice, uint32_t* y0,
                  uint32_t* y1) {
  const gpu::Level& level = l.levels[mip];
  if (record.whole || l.surface.dimension == gpu::Dimension::k3D) {
    *y0 = 0;
    *y1 = level.height;
    return record.whole || !record.spans.empty();
  }
  const uint64_t slice_base = static_cast<uint64_t>(slice) * l.slice_size;
  const uint64_t element = 1ull << l.surface.element_log2;
  const uint64_t start = slice_base + level.offset;
  const uint64_t end = level.in_tail ? slice_base + l.alignment
                                     : start + static_cast<uint64_t>(level.pitch) * level.rows * element;
  bool any = false;
  uint32_t first = level.height, last = 0;
  for (const auto& [from, to] : record.spans) {
    const uint64_t b = std::max(from, start), e = std::min(to, end);
    if (b >= e) continue;
    any = true;
    if (level.in_tail) {
      first = 0;
      last = level.height;
      break;
    }
    uint64_t row_b, row_e;  // element rows the bytes [b, e) cover
    if (l.block_log2 == 0) {
      const uint64_t row_bytes = static_cast<uint64_t>(level.pitch) * element;
      row_b = (b - start) / row_bytes;
      row_e = (e - 1 - start) / row_bytes + 1;
    } else {
      const uint64_t per_row = std::max<uint64_t>(1, level.pitch / l.block_w);
      row_b = ((b - start) >> l.block_log2) / per_row * l.block_h;
      row_e = (((e - 1 - start) >> l.block_log2) / per_row + 1) * l.block_h;
    }
    first = std::min<uint32_t>(first, static_cast<uint32_t>(row_b));
    last = std::max<uint32_t>(last, static_cast<uint32_t>(std::min<uint64_t>(row_e, level.height)));
  }
  if (!any || first >= last) return false;
  *y0 = first;
  *y1 = last;
  return true;
}

// Records the copies filling `texture` from a texture in the console's
// layout: the subresource rows that changed (all of them when `first` or
// unknown), detiled by the CPU into upload memory first; no command is
// recorded unless every part could be read. False when none could be.
bool record_tiled_fill(Device* device, Uploader* up, const Record& record, const gpu::Texture& console,
                       ID3D12Resource* texture, bool first) {
  const lib::TextureShape shape = shape_of(record.desc);
  const gpu::Layout& l = console.planes[0];
  const lib::FormatInfo f = lib::format_info(shape.format);
  if (!f.block_bytes) return false;
  struct Part {
    uint32_t index, mip, y0, y1, depth, row_pitch;
    ID3D12Resource* buffer;
    uint64_t offset;
  };
  thread_local std::vector<Part> parts;
  parts.clear();
  const uint32_t count = lib::subresource_count(shape);
  const bool is3d = shape.dimension == 4;
  uint64_t bytes = 0;
  for (uint32_t index = 0; index < count; ++index) {
    const uint32_t mip = index % shape.mips, slice = index / shape.mips;
    if (mip >= l.surface.mips) continue;
    uint32_t y0 = 0, y1 = 0;
    if (first) {
      y1 = l.levels[mip].height;
    } else if (!changed_rows(record, l, mip, is3d ? 0 : slice, &y0, &y1)) {
      continue;
    }
    const gpu::Level& level = l.levels[mip];
    Part part{index, mip, y0, y1, is3d ? level.depth : 1u,
              static_cast<uint32_t>(lib::align_up(static_cast<uint64_t>(level.width) * f.block_bytes, 256)), nullptr, 0};
    const uint64_t size = static_cast<uint64_t>(part.row_pitch) * (y1 - y0) * part.depth;
    uint8_t* cpu = nullptr;
    if (!upload_space(device, up, size, &part.buffer, &part.offset, &cpu) ||
        !detile_title_rows(l, record.address, console.size, mip, is3d ? 0 : slice, y0, y1, cpu, part.row_pitch,
                           static_cast<uint64_t>(part.row_pitch) * (y1 - y0))) {
      return false;
    }
    bytes += size;
    parts.push_back(part);
  }
  if (parts.empty()) return !first;
  c_detiled().add(1);
  c_detiled_bytes().add(static_cast<int64_t>(bytes));
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = record.rest;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  if (!first) up->list->ResourceBarrier(1, &barrier);
  for (const Part& part : parts) {
    const uint32_t texel_height = std::max(1u, shape.height >> part.mip);
    const uint32_t top = part.y0 * f.block_size;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = part.buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = part.offset;
    source.PlacedFootprint.Footprint.Format = record.desc.Format;
    source.PlacedFootprint.Footprint.Width = static_cast<UINT>(std::max<uint64_t>(1, shape.width >> part.mip));
    source.PlacedFootprint.Footprint.Height = std::min(part.y1 * f.block_size, texel_height) - top;
    source.PlacedFootprint.Footprint.Depth = part.depth;
    source.PlacedFootprint.Footprint.RowPitch = part.row_pitch;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = texture;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = part.index;
    up->list->CopyTextureRegion(&target, 0, top, 0, &source, nullptr);
  }
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  up->list->ResourceBarrier(1, &barrier);
  return true;
}

// An upload buffer holding a copy of `size` bytes of title memory.
ID3D12Resource* staged_copy(Device* device, uint64_t address, uint64_t size) {
  ID3D12Resource* buffer = upload_buffer(device, size);
  if (!buffer) return nullptr;
  buffer->SetName(L"holy texture staging");
  void* mapped = nullptr;
  if (FAILED(buffer->Map(0, nullptr, &mapped))) {
    buffer->Release();
    return nullptr;
  }
  const bool copied = copy_title_memory(mapped, reinterpret_cast<const void*>(address), size);
  buffer->Unmap(0, nullptr);
  if (!copied) {
    buffer->Release();
    return nullptr;
  }
  return buffer;
}

// Records into the uploader's list the copies filling `texture` from the
// title memory of `record` (g_lock held): from the heap over the memory when
// one backs it whole (the GPU reads the memory when the copies run), else
// from a copy the CPU makes now. `first`: the texture is new (in the
// copy-destination state); otherwise it is a shader resource. False (logged
// for new textures) when the memory cannot be read.
bool record_fill(Device* device, Uploader* up, const Record& record, ID3D12Resource* texture, bool first) {
  static const lib::Counter filling("d3d12.texture_fill_us");
  const TimeIn timing_fill(filling);
  const lib::TextureShape shape = shape_of(record.desc);
  Backing backing;
  ID3D12Resource* source_buffer = nullptr;
  uint64_t source_offset = 0;
  // Observed: titles stream textures into pages of a pool mapped one by one
  // into an address range (queue slot 31), so no host heap backs such a
  // texture as a whole; the CPU reads it through the title's addresses.
  // Textures in the console's layout are detiled by the CPU into the
  // desktop's footprints on the way.
  gpu::Texture console;
  const bool tiled = console_layout(record.desc, &console);
  if (tiled) {
    if (record_tiled_fill(device, up, record, console, texture, first)) return true;
    if (first) {
      LIB_LOG(lib::kError, "d3d12", "texture dim %u %llux%ux%u, %u mips, format %u at %p (0x%llx bytes): no copy (its "
              "memory cannot be read)", shape.dimension, static_cast<unsigned long long>(shape.width), shape.height,
              shape.depth_or_array, shape.mips, shape.format, reinterpret_cast<void*>(record.address),
              static_cast<unsigned long long>(record.size));
    }
    return false;
  }
  const bool located = locate(device, record.address, &backing, record.size);
  bool is_readable = false;
  if (located && backing.whole && backing.contiguous >= record.size) {
    source_buffer = backing.whole;
    source_offset = backing.heap_offset;
  } else {
    // The copy itself finds out whether the memory can be read (a check
    // before it costs host queries of milliseconds on large committed runs).
    source_buffer = staged_copy(device, record.address, record.size);
    is_readable = source_buffer != nullptr;
    if (source_buffer) up->staging.push_back(source_buffer);
  }
  if (!source_buffer) {
    if (first) {
      MEMORY_BASIC_INFORMATION info{};
      VirtualQuery(reinterpret_cast<const void*>(record.address), &info, sizeof(info));
      LIB_LOG(lib::kError, "d3d12",
              "texture dim %u %llux%ux%u, %u mips, format %u at %p (0x%llx bytes): no copy (%s; memory state 0x%lx, "
              "protection 0x%lx, type 0x%lx, allocation %p)",
              shape.dimension, static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array,
              shape.mips, shape.format, reinterpret_cast<void*>(record.address),
              static_cast<unsigned long long>(record.size),
              is_readable ? "no upload buffer"
              : located   ? "no GPU heap covers it whole, and it cannot be read"
                          : "it lies in no readable memory",
              info.State, info.Protect, info.Type, info.AllocationBase);
    }
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = record.rest;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  if (!first) up->list->ResourceBarrier(1, &barrier);
  const uint32_t count = lib::subresource_count(shape);
  for (uint32_t index = 0; index < count; ++index) {
    lib::SubresourceLayout layout;
    lib::subresource_layout(shape, index, &layout);
    const uint32_t mip = index % shape.mips;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = source_buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = source_offset + layout.offset;
    source.PlacedFootprint.Footprint.Format = record.desc.Format;
    source.PlacedFootprint.Footprint.Width = static_cast<UINT>((shape.width >> mip) ? (shape.width >> mip) : 1);
    source.PlacedFootprint.Footprint.Height = (shape.height >> mip) ? (shape.height >> mip) : 1;
    source.PlacedFootprint.Footprint.Depth = layout.depth;
    source.PlacedFootprint.Footprint.RowPitch = layout.row_pitch;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = texture;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = index;
    up->list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  }
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  up->list->ResourceBarrier(1, &barrier);
  return true;
}

void replace(const Key& key, Record* record) {
  auto found = textures().find(key);
  if (found != textures().end()) {
    drop(found->second);
    found->second = record;
  } else {
    textures().emplace(key, record);
  }
}

}  // namespace

bool placed_texture_follows_memory(const D3D12_RESOURCE_DESC& desc) {
  constexpr D3D12_RESOURCE_FLAGS kWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  return desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && (desc.Flags & kWritten) == 0 && tracking();
}

namespace {

// A placed texture the GPU does not write: filled from its title memory now
// and kept current like textures viewed by address (g_lock not held). The
// caller receives a reference.
ID3D12Resource* memory_texture(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address) {
  const lib::TextureShape shape = shape_of(desc);
  const Key key = key_of(address, shape);
  const uint64_t size = memory_size(desc);
  if (!size) return nullptr;
  std::lock_guard<std::mutex> guard(g_lock);
  apply_changes();
  release_retired();
  Uploader* up = uploader(device);
  if (!up) return nullptr;
  D3D12_RESOURCE_DESC host_desc = desktop_resource_desc(desc);
  host_desc.Alignment = 0;
  auto* record = new Record();
  record->address = address;
  record->desc = desc;
  record->size = size;
  record->from_memory = true;
  record->rest = D3D12_RESOURCE_STATE_COMMON;
  if (!committed_whole(address, size) && !readable(address, size)) {
    HolyEnsureCommitted(reinterpret_cast<void*>(address), static_cast<size_t>(size));
  }
  track(record);
  if (!record->tracked && !record->protected_watch) c_untracked().add(1);
  open_upload(up);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* texture = nullptr;
  const HRESULT hr = device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &host_desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&texture));
  if (SUCCEEDED(hr)) texture->SetName(L"holy placed texture following its memory");
  LIB_LOG(hr < 0 ? lib::kError : lib::kDebug, "d3d12",
          "placed texture dim %d %llux%ux%u, %u mips, format %u, flags 0x%x at %p, following its memory -> 0x%08lX",
          desc.Dimension, static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize,
          desc.MipLevels, desc.Format, desc.Flags, reinterpret_cast<void*>(address), static_cast<unsigned long>(hr));
  if (FAILED(hr)) {
    drop(record);
    return nullptr;
  }
  if (!record_fill(device, up, *record, texture, true)) {
    // Not readable yet (pages not mapped): zeros until the memory changes.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                          record->rest};
    up->list->ResourceBarrier(1, &barrier);
  }
  record->texture = texture;
  record->whole = false;  // changes from now on are tracked by span
  // Submitted with the title's next submission, which waits for it.
  if (size >= (1u << 20)) {
    LIB_LOG(lib::kDebug, "d3d12", "placed texture %llux%u format %u at %p follows its memory: tracked %d, watched %d",
            static_cast<unsigned long long>(desc.Width), desc.Height, desc.Format, reinterpret_cast<void*>(address),
            record->tracked, record->protected_watch);
  }
  replace(key, record);
  g_largest = std::max(g_largest, size);
  c_live_textures().add(1);
  c_live_texture_bytes().add(static_cast<int64_t>(size));
  texture->AddRef();
  return texture;
}

}  // namespace

void texture_written_by_gpu(const D3D12_RESOURCE_DESC& desc, uint64_t address) {
  if (!address || !placed_texture_follows_memory(desc)) return;
  std::lock_guard<std::mutex> guard(g_lock);
  auto found = textures().find(key_of(address, shape_of(desc)));
  if (found == textures().end()) return;
  Record* record = found->second;
  if (!record->from_memory || record->rest != D3D12_RESOURCE_STATE_COMMON) return;
  // Its contents are the GPU's from now on: memory no longer overwrites them.
  if (record->protected_watch) unwatch(reinterpret_cast<WatchToken>(record));
  record->protected_watch = false;
  record->tracked = false;
  for (auto& [base, allocation] : tracked()) {
    auto& list = allocation.records;
    list.erase(std::remove(list.begin(), list.end(), record), list.end());
  }
  g_dirty.erase(std::remove(g_dirty.begin(), g_dirty.end(), record), g_dirty.end());
  record->dirty = false;
  record->from_memory = false;
  c_live_textures().add(-1);
  c_live_texture_bytes().add(-static_cast<int64_t>(record->size));
}

ID3D12Resource* placed_texture(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address,
                               UINT initial_layout, const D3D12_CLEAR_VALUE* clear) {
  if (address && placed_texture_follows_memory(desc)) return memory_texture(device, desc, address);
  const lib::TextureShape shape = shape_of(desc);
  const Key key = key_of(address, shape);
  D3D12_RESOURCE_DESC host_desc = desktop_resource_desc(desc);
  host_desc.Alignment = 0;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* texture = nullptr;
  const HRESULT hr = create_host_committed(device, heap, D3D12_HEAP_FLAG_NONE, host_desc, initial_layout, clear,
                                           &texture);
  LIB_LOG(hr < 0 ? lib::kError : lib::kDebug, "d3d12",
          "placed texture dim %d %llux%ux%u, %u mips, format %u, flags 0x%x at %p -> 0x%08lX", desc.Dimension,
          static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize, desc.MipLevels,
          desc.Format, desc.Flags, reinterpret_cast<void*>(address), static_cast<unsigned long>(hr));
  if (FAILED(hr)) return nullptr;
  auto* record = new Record();
  record->texture = texture;
  record->whole = false;  // changes from now on are tracked by span
  record->address = address;
  record->desc = desc;
  std::lock_guard<std::mutex> guard(g_lock);
  replace(key, record);
  texture->AddRef();
  return texture;
}

void register_texture(const D3D12_RESOURCE_DESC& desc, uint64_t address, ID3D12Resource* texture) {
  if (!texture || !address) return;
  const Key key = key_of(address, shape_of(desc));
  texture->AddRef();
  auto* record = new Record();
  record->texture = texture;
  record->whole = false;  // changes from now on are tracked by span
  record->address = address;
  record->desc = desc;
  std::lock_guard<std::mutex> guard(g_lock);
  replace(key, record);
}

void unregister_texture(const D3D12_RESOURCE_DESC& desc, uint64_t address, ID3D12Resource* texture) {
  if (!texture || !address) return;
  const Key key = key_of(address, shape_of(desc));
  std::lock_guard<std::mutex> guard(g_lock);
  auto found = textures().find(key);
  if (found != textures().end() && found->second->texture == texture) {
    drop(found->second);
    textures().erase(found);
  }
}

ID3D12Resource* texture_at(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address) {
  static const lib::Counter spent("d3d12.texture_view_copy_us");
  LARGE_INTEGER started;
  QueryPerformanceCounter(&started);
  struct Done {
    LARGE_INTEGER started;
    ~Done() {
      LARGE_INTEGER now, f;
      QueryPerformanceCounter(&now);
      QueryPerformanceFrequency(&f);
      spent.add((now.QuadPart - started.QuadPart) * 1000000 / f.QuadPart);
    }
  } done{started};
  const lib::TextureShape shape = shape_of(desc);
  const Key key = key_of(address, shape);

  std::lock_guard<std::mutex> guard(g_lock);
  // Released memory's textures go first: a texture viewed in memory the title
  // released and reused is a new one.
  apply_changes();
  release_retired();
  retire_superseded();
  auto found = textures().find(key);
  if (found != textures().end()) {
    Record* existing = found->second;
    if (existing->superseded_at) {  // viewed again: in use after all
      existing->superseded_at = 0;
      g_superseded.erase(std::remove(g_superseded.begin(), g_superseded.end(), existing), g_superseded.end());
    }
    return existing->texture;
  }

  const uint64_t size = memory_size(desc);
  if (!size) return nullptr;
  Uploader* up = uploader(device);
  if (!up) return nullptr;

  D3D12_RESOURCE_DESC host_desc = desc;
  host_desc.Alignment = 0;
  host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  host_desc.Format = desktop_format(desc.Format);
  host_desc.MipLevels = static_cast<UINT16>(shape.mips);
  host_desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
                     D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  auto* record = new Record();
  record->address = address;
  record->desc = desc;
  record->size = size;
  record->from_memory = true;

  // Memory committed on first touch reads as zero until touched: committed
  // now, so it can be copied (and its writes tracked).
  if (!committed_whole(address, size) && !readable(address, size)) {
    HolyEnsureCommitted(reinterpret_cast<void*>(address), static_cast<size_t>(size));
  }
  // Tracked from before the copy reads the memory (a write from now on makes
  // it dirty; the copy may already hold part of it).
  track(record);
  if (!record->tracked && !record->protected_watch) {
    c_untracked().add(1);
    // Whose memory it is: the CPU's writes to it are not seen.
    static std::atomic<int> logged{0};
    MEMORY_BASIC_INFORMATION info{};
    if (address && logged.fetch_add(1) < 64 &&
        VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) {
      LIB_LOG(lib::kWarn, "d3d12",
              "host texture of format %u %llux%ux%u at %p: writes to its memory are not tracked (state 0x%lx, type "
              "0x%lx, protection 0x%lx, allocation %p)",
              shape.format, static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array,
              reinterpret_cast<void*>(address), info.State, info.Type, info.Protect, info.AllocationBase);
    }
  }

  open_upload(up);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* texture = nullptr;
  HRESULT hr;
  {
    static const lib::Counter creating("d3d12.texture_create_us");
    const TimeIn timing_create(creating);
    hr = device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &host_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&texture));
  }
  if (SUCCEEDED(hr)) texture->SetName(L"holy texture viewed in title memory");
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "host texture of format %u %llux%u refused (0x%08lX)", shape.format,
            static_cast<unsigned long long>(shape.width), shape.height, static_cast<unsigned long>(hr));
  }
  if (FAILED(hr)) {
    drop(record);
    return nullptr;
  }
  if (!record_fill(device, up, *record, texture, true)) {
    // Memory that cannot be read yet (physical pages not mapped yet): the
    // texture starts as zeros (new host textures are) and is filled when the
    // memory changes (the title maps or writes it).
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
    up->list->ResourceBarrier(1, &barrier);
  }
  record->texture = texture;
  record->whole = false;  // changes from now on are tracked by span
  // Submitted with the title's next submission, which waits for it.

  LIB_LOG(lib::kDebug, "d3d12", "host texture for format %u %llux%ux%u, %u mips at %p", shape.format,
          static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array, shape.mips,
          reinterpret_cast<void*>(address));
  {
    // Copies made over memory another copy (of another shape) already covers:
    // the title reusing its texture memory for other textures (measurement).
    static const lib::Counter overlaps("d3d12.host_textures_over_other_copies");
    static const lib::Counter overlap_bytes("d3d12.host_texture_bytes_over_other_copies");
    auto it = textures().lower_bound(Key{address > g_largest ? address - g_largest : 0, 0, 0, 0, 0, 0, 0});
    for (; it != textures().end() && std::get<0>(it->first) < address + size; ++it) {
      const Record* other = it->second;
      if (other->from_memory && other->address + other->size > address) {
        overlaps.add(1);
        overlap_bytes.add(static_cast<int64_t>(std::min(other->size, size)));
        break;
      }
    }
  }
  supersede_over(address, size);
  textures().emplace(key, record);
  g_largest = std::max(g_largest, size);
  c_live_textures().add(1);
  c_live_texture_bytes().add(static_cast<int64_t>(size));
  return texture;
}

void texture_memory_released(uint64_t address, uint64_t size) {
  if (!size) return;
  std::lock_guard<std::mutex> guard(g_changes_lock);
  g_changes.push_back({address, size, true});
  g_any_change.store(true, std::memory_order_release);
}

void texture_memory_changed(uint64_t address, uint64_t size) {
  if (!size) return;
  std::lock_guard<std::mutex> guard(g_changes_lock);
  g_changes.push_back({address, size, false});
  g_any_change.store(true, std::memory_order_release);
}

void set_video_memory_pressure(bool over_budget) {
  g_memory_pressure.store(over_budget, std::memory_order_relaxed);
}

void refresh_textures(Device* device, ID3D12CommandQueue* queue) {
  static const lib::Counter spent("d3d12.texture_refresh_us");
  const TimeIn timing(spent);
  std::lock_guard<std::mutex> guard(g_lock);
  retire_superseded();
  if (tracking()) {
    static const lib::Counter scanning("d3d12.texture_scan_us");
    const TimeIn timing_scan(scanning);
    scan();
  }
  Uploader* up = uploader(device);
  if (!up) return;
  if (g_dirty.empty() && !up->open) {
    // Uploads submitted outside a submission (a texture let go of): the
    // title's work waits for them too.
    if (queue && up->value) queue->Wait(up->fence, up->value);
    return;
  }
  std::vector<Record*> dirty;
  dirty.swap(g_dirty);
  open_upload(up);
  uint32_t refreshed = 0;
  for (Record* record : dirty) {
    record->dirty = false;
    if (!record->texture) continue;
    // Watched again before the copy reads the memory; memory that could not
    // be tracked before (not mapped yet) is tried again.
    if (record->protected_watch) watch_memory(record->address, record->size, reinterpret_cast<WatchToken>(record));
    track(record);
    if (record_fill(device, up, *record, record->texture, false)) ++refreshed;
    record->whole = false;
    record->spans.clear();
  }
  const uint64_t value = submit_upload(up);
  // Work submitted after this on the title's queue sees the new contents
  // (refreshed, and new copies recorded since the last submission).
  if (queue) queue->Wait(up->fence, value);
  if (refreshed) {
    c_refreshes().add(refreshed);
    LIB_LOG(lib::kDebug, "d3d12", "%u host textures copied again from title memory written since", refreshed);
  }
}

void host_texture_totals(uint64_t* count, uint64_t* bytes) {
  std::lock_guard<std::mutex> guard(g_lock);
  *count = static_cast<uint64_t>(c_live_textures().value());
  *bytes = static_cast<uint64_t>(c_live_texture_bytes().value());
}

}  // namespace holy::d3d12
