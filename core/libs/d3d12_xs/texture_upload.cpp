#include "core/libs/d3d12_xs/texture_upload.h"

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
#include "core/libs/d3d12_xs/write_watch.h"
#include "core/libs/support.h"
#include "core/libs/texture_layout.h"

namespace holy::d3d12 {
namespace {

using Key = std::tuple<uint64_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;

struct Uploader {
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  uint64_t value = 0;
  std::vector<ID3D12Resource*> staging;  // released once the work submitted with them is done
};

// A host texture views by address find: filled from title memory
// (texture_at), or the host texture of a resource the title placed or
// created (placed_texture, register_texture), which the GPU writes.
struct Record {
  ID3D12Resource* texture = nullptr;  // the registry's reference
  uint64_t address = 0;
  D3D12_RESOURCE_DESC desc{};
  uint64_t size = 0;         // bytes in title memory
  bool from_memory = false;  // filled from title memory: kept current
  bool protected_watch = false;  // its memory is watched by page protection (write_watch.h)
  bool tracked = false;          // its allocation's writes are tracked by the host
  bool dirty = false;
};

// Allocations holding textures filled from their memory whose writes the host
// tracks (xmem reserves title memory with MEM_WRITE_WATCH), by base.
struct Tracked {
  uint64_t size = 0;
  std::vector<Record*> records;
};

// The registry, its tracking and the uploader.
std::mutex g_lock;
std::atomic<uint64_t> g_texture_bytes{0};
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
std::vector<std::pair<uint64_t, uint64_t>> g_changes;
std::atomic<bool> g_any_change{false};

const lib::Counter& c_refreshes() {
  static const lib::Counter c("d3d12.texture_refreshes");
  return c;
}
const lib::Counter& c_untracked() {
  static const lib::Counter c("d3d12.textures_untracked");
  return c;
}

// HOLY_TEXTURE_TRACKING=0 turns tracking off (textures are copied once).
bool tracking() {
  static const bool enabled = [] {
    wchar_t text[8] = {};
    const bool off = GetEnvironmentVariableW(L"HOLY_TEXTURE_TRACKING", text, 8) && text[0] == L'0';
    if (!off) {
      start_write_watch(&texture_memory_changed);
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
bool allocation_of(uint64_t address, uint64_t* base, uint64_t* size, bool* watched, bool* mapped) {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State == MEM_FREE) return false;
  const auto allocation = reinterpret_cast<uint64_t>(info.AllocationBase);
  uint64_t end = allocation;
  for (;;) {
    MEMORY_BASIC_INFORMATION part{};
    if (!VirtualQuery(reinterpret_cast<const void*>(end), &part, sizeof(part))) break;
    if (reinterpret_cast<uint64_t>(part.AllocationBase) != allocation || part.State == MEM_FREE) break;
    end = reinterpret_cast<uint64_t>(part.BaseAddress) + part.RegionSize;
  }
  *base = allocation;
  *size = end - allocation;
  *mapped = info.Type == MEM_MAPPED;
  void* page = nullptr;
  ULONG_PTR count = 1;
  DWORD granularity = 0;
  // Fails for memory reserved without write tracking (mapped views: physical pages).
  *watched = info.Type == MEM_PRIVATE &&
             GetWriteWatch(0, reinterpret_cast<void*>(allocation), static_cast<SIZE_T>(*size), &page, &count,
                           &granularity) == 0;
  return true;
}

void mark(Record* record) {
  if (record->dirty || !record->texture) return;
  record->dirty = true;
  g_dirty.push_back(record);
}

// Marks the textures filled from memory over [begin, end) (g_lock held).
void mark_range(uint64_t begin, uint64_t end) {
  auto it = textures().lower_bound(Key{begin > g_largest ? begin - g_largest : 0, 0, 0, 0, 0, 0, 0});
  for (; it != textures().end() && std::get<0>(it->first) < end; ++it) {
    Record* record = it->second;
    if (record->from_memory && record->address + record->size > begin) mark(record);
  }
}

// Asks the host which pages of [begin, end) in a tracked allocation were
// written since they were last asked about, and marks the textures over them
// (g_lock held).
void scan_range(Tracked& allocation, uint64_t begin, uint64_t end) {
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
      for (Record* record : allocation.records) {
        if (record->address < page + granularity && page < record->address + record->size) mark(record);
      }
    }
    if (count < sizeof(pages) / sizeof(pages[0])) return;
  }
}

// Applies queued changes and, at most every 16 ms (about once a frame), asks
// the host which tracked pages were written: one query per tracked
// allocation, over the span its textures cover (g_lock held).
void scan() {
  if (g_any_change.load(std::memory_order_acquire)) {
    std::vector<std::pair<uint64_t, uint64_t>> changes;
    {
      std::lock_guard<std::mutex> guard(g_changes_lock);
      changes.swap(g_changes);
      g_any_change.store(false, std::memory_order_relaxed);
    }
    for (const auto& [begin, size] : changes) {
      mark_range(begin, begin + size);
      forget_memory(begin, size);  // remapped or released pages lose their watches (refreshed records watch again)
    }
  }
  const ULONGLONG now = GetTickCount64();
  if (now - g_last_scan < 16) return;
  g_last_scan = now;
  for (auto& [base, allocation] : tracked()) {
    if (allocation.records.empty()) continue;
    uint64_t first = UINT64_MAX, last = 0;
    for (const Record* record : allocation.records) {
      first = std::min(first, record->address);
      last = std::max(last, record->address + record->size);
    }
    scan_range(allocation, first, last);
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
    scan_range(allocation, record->address, record->address + record->size);
    allocation.records.push_back(record);
    record->tracked = true;
  } else if (mapped && watch_memory(record->address, record->size, reinterpret_cast<WatchToken>(record))) {
    record->protected_watch = true;
  }
}

// The registry lets go of a record (g_lock held).
void drop(Record* record) {
  if (record->protected_watch) unwatch(reinterpret_cast<WatchToken>(record));
  g_dirty.erase(std::remove(g_dirty.begin(), g_dirty.end(), record), g_dirty.end());
  for (auto& [base, allocation] : tracked()) {
    auto& list = allocation.records;
    list.erase(std::remove(list.begin(), list.end(), record), list.end());
  }
  if (record->texture) record->texture->Release();
  delete record;
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
  up->allocator->Reset();
  up->list->Reset(up->allocator, nullptr);
}

// Submits the uploader's list; returns the fence value it signals.
uint64_t submit_upload(Uploader* up) {
  up->list->Close();
  ID3D12CommandList* lists[] = {up->list};
  up->queue->ExecuteCommandLists(1, lists);
  up->queue->Signal(up->fence, ++up->value);
  check_point("texture upload");
  return up->value;
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

// An upload buffer holding a copy of `size` bytes of title memory.
ID3D12Resource* staged_copy(Device* device, uint64_t address, uint64_t size) {
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
  void* mapped = nullptr;
  if (FAILED(buffer->Map(0, nullptr, &mapped))) {
    buffer->Release();
    return nullptr;
  }
  std::memcpy(mapped, reinterpret_cast<const void*>(address), size);
  buffer->Unmap(0, nullptr);
  return buffer;
}

// Records into the uploader's list the copies filling `texture` from the
// title memory of `record` (g_lock held): from the heap over the memory when
// one backs it whole (the GPU reads the memory when the copies run), else
// from a copy the CPU makes now. `first`: the texture is new (in the
// copy-destination state); otherwise it is a shader resource. False (logged
// for new textures) when the memory cannot be read.
bool record_fill(Device* device, Uploader* up, const Record& record, ID3D12Resource* texture, bool first) {
  const lib::TextureShape shape = shape_of(record.desc);
  Backing backing;
  ID3D12Resource* source_buffer = nullptr;
  uint64_t source_offset = 0;
  // Observed: titles stream textures into pages of a pool mapped one by one
  // into an address range (queue slot 31), so no host heap backs such a
  // texture as a whole; the CPU reads it through the title's addresses.
  if (locate(device, record.address, &backing, record.size) && backing.whole && backing.contiguous >= record.size) {
    source_buffer = backing.whole;
    source_offset = backing.heap_offset;
  } else if (readable(record.address, record.size)) {
    source_buffer = staged_copy(device, record.address, record.size);
    if (source_buffer) up->staging.push_back(source_buffer);
  }
  if (!source_buffer) {
    if (first) {
      LIB_LOG(lib::kError, "d3d12",
              "texture dim %u %llux%ux%u, %u mips, format %u at %p (0x%llx bytes) lies in no readable memory",
              shape.dimension, static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array,
              shape.mips, shape.format, reinterpret_cast<void*>(record.address),
              static_cast<unsigned long long>(record.size));
    }
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
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

ID3D12Resource* placed_texture(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address,
                               UINT initial_layout, const D3D12_CLEAR_VALUE* clear) {
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
  const lib::TextureShape shape = shape_of(desc);
  const Key key = key_of(address, shape);

  std::lock_guard<std::mutex> guard(g_lock);
  auto found = textures().find(key);
  if (found != textures().end()) return found->second->texture;

  const uint64_t size = lib::texture_size(shape);
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
  if (!readable(address, size)) HolyEnsureCommitted(reinterpret_cast<void*>(address), static_cast<size_t>(size));
  // Tracked from before the copy reads the memory (a write from now on makes
  // it dirty; the copy may already hold part of it).
  track(record);
  if (!record->tracked && !record->protected_watch) c_untracked().add(1);

  begin_upload(up);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* texture = nullptr;
  HRESULT hr = device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &host_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "host texture of format %u %llux%u refused (0x%08lX)", shape.format,
            static_cast<unsigned long long>(shape.width), shape.height, static_cast<unsigned long>(hr));
  }
  if (FAILED(hr)) {
    up->list->Close();
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
  const uint64_t value = submit_upload(up);
  up->fence->SetEventOnCompletion(value, up->event);
  WaitForSingleObject(up->event, INFINITE);

  LIB_LOG(lib::kDebug, "d3d12", "host texture for format %u %llux%ux%u, %u mips at %p", shape.format,
          static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array, shape.mips,
          reinterpret_cast<void*>(address));
  textures().emplace(key, record);
  g_largest = std::max(g_largest, size);
  g_texture_bytes.fetch_add(size, std::memory_order_relaxed);
  return texture;
}

void texture_memory_changed(uint64_t address, uint64_t size) {
  if (!size) return;
  std::lock_guard<std::mutex> guard(g_changes_lock);
  g_changes.emplace_back(address, size);
  g_any_change.store(true, std::memory_order_release);
}

void refresh_textures(Device* device, ID3D12CommandQueue* queue) {
  std::lock_guard<std::mutex> guard(g_lock);
  if (tracking()) scan();
  if (g_dirty.empty()) return;
  Uploader* up = uploader(device);
  if (!up) return;
  std::vector<Record*> dirty;
  dirty.swap(g_dirty);
  begin_upload(up);
  uint32_t refreshed = 0;
  for (Record* record : dirty) {
    record->dirty = false;
    if (!record->texture) continue;
    // Watched again before the copy reads the memory; memory that could not
    // be tracked before (not mapped yet) is tried again.
    if (record->protected_watch) watch_memory(record->address, record->size, reinterpret_cast<WatchToken>(record));
    track(record);
    if (record_fill(device, up, *record, record->texture, false)) ++refreshed;
  }
  const uint64_t value = submit_upload(up);
  // Work submitted after this on the title's queue sees the new contents.
  if (queue && refreshed) queue->Wait(up->fence, value);
  if (refreshed) {
    c_refreshes().add(refreshed);
    LIB_LOG(lib::kDebug, "d3d12", "%u host textures copied again from title memory written since", refreshed);
  }
}

void host_texture_totals(uint64_t* count, uint64_t* bytes) {
  std::lock_guard<std::mutex> guard(g_lock);
  *count = textures().size();
  *bytes = g_texture_bytes.load(std::memory_order_relaxed);
}

}  // namespace holy::d3d12
