// Command lists.
//
// Observed: device slot 73 creates a command list from a 16-byte descriptor
// whose first dword is the list type (titles pass {0, 2, 1, 0} for a direct
// list), an allocator, an initial pipeline state and the console command list
// interface id {c3827890-4024-474c-98cf-645b51dc4874}. The console command
// list has far more slots than the desktop one (titles call slots beyond 130);
// slot 9 is Close, as on the desktop. Every slot is established from the
// title's calls; unbound slots report themselves.
#include <d3d12.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/bindings.h"
#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/shader/descriptor_layout.h"
#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/ngg.h"
#include "core/libs/d3d12_xs/page_cache.h"
#include "core/libs/d3d12_xs/texture_dump.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/d3d12_xs/views.h"
#include "core/libs/d3d12_xs/pipeline_cache.h"
#include "core/libs/d3d12_xs/pipeline_stream.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

// Recorded work, for recorded_work_summary.
struct WorkCounts {
  std::atomic<uint64_t> draws{0}, indexed{0}, ngg{0}, dispatches{0}, no_pipeline{0}, unbound{0}, mesh{0};
  std::atomic<uint64_t> clears{0}, depth_clears{0}, nonblack_clears{0};
};
WorkCounts g_work;


const lib::VtableInfo kListInfo{kLibrary, "ID3D12GraphicsCommandListX"};

HRESULT STDMETHODCALLTYPE list_query_interface(CommandList* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE list_add_ref(CommandList* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE list_release(CommandList* self) {
  return static_cast<ULONG>(self->refs.fetch_sub(1) - 1);
}

// --- host command lists ------------------------------------------------------------
//
// Observed: titles reset a command list and submit it again while its
// previous submission is still executing (the console allows it). The
// desktop does not: a command list must not be submitted again before the
// GPU has finished its previous submission (the debug layer refuses such a
// submission; without it the driver re-records what the GPU is executing,
// and the host device is removed as hung). Host model: a title list records
// into a host list from a pool; resetting a title list whose host list may
// still be executing takes another host list, one the GPU is done with (or a
// new one), and leaves the busy one in the pool.

struct HostListPool {
  std::mutex lock;
  std::vector<HostList*> idle[4];  // by list type: direct, bundle, compute, copy
};

HostListPool& pool() {
  static auto* instance = new HostListPool();
  return *instance;
}

bool in_flight(const HostList* h) {
  return h->fence && h->fence->GetCompletedValue() < h->value;
}

HostList* new_host_list(Device* device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator) {
  ID3D12GraphicsCommandList* list = nullptr;
  const HRESULT hr = device->host->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&list));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "host command list of type %d refused (0x%08lX)", static_cast<int>(type),
            static_cast<unsigned long>(hr));
    return nullptr;
  }
  auto* h = new HostList();
  h->list = list;
  h->recording = true;
  list->QueryInterface(IID_PPV_ARGS(&h->list2));
  list->QueryInterface(IID_PPV_ARGS(&h->list7));
  return h;
}

void use_host_list(CommandList* self, HostList* h) {
  self->current = h;
  self->host = h->list;
  self->host2 = h->list2;
  self->host7 = h->list7;
  if (HolyLogEnabled(lib::kDebug)) {
    wchar_t name[32];
    std::swprintf(name, 32, L"title list %u", self->number);
    h->list->SetName(name);  // device-removed data reports lists by name
  }
}

HRESULT STDMETHODCALLTYPE list_close(CommandList* self) {
  const HRESULT hr = self->host->Close();
  if (SUCCEEDED(hr)) self->current->recording = false;
  LIB_LOG(FAILED(hr) ? lib::kError : lib::kTrace, "d3d12", "list %p closed -> 0x%08lX", static_cast<void*>(self),
          static_cast<unsigned long>(hr));
  return hr;
}

// Observed: slot 139 takes (allocator, pipeline state, 0) and returns an
// HRESULT titles require to succeed; they call it on lists taken from their
// pool with an allocator taken from theirs: a reset (desktop Reset takes the
// first two arguments).
HRESULT STDMETHODCALLTYPE list_reset_x(CommandList* self, void* allocator, void* state, UINT64 flags) {
  auto* host_allocator = host_of<ID3D12CommandAllocator>(allocator);
  if (!host_allocator) return E_INVALIDARG;
  if (state || flags) {
    LIB_LOG(lib::kError, "d3d12", "command list reset with a pipeline state %p / flags 0x%llx", state,
            static_cast<unsigned long long>(flags));
    HolyUnimplementedFrom(kLibrary, "command list reset with an initial pipeline state", _ReturnAddress());
    return E_NOTIMPL;
  }
  self->heaps[0] = self->heaps[1] = nullptr;
  self->memory_heap_bound = false;
  self->depth_format = DXGI_FORMAT_UNKNOWN;
  self->graphics = RootBinding{};
  self->compute = RootBinding{};
  self->pipeline = nullptr;
  self->bound = nullptr;
  self->rectangles = false;
  self->translated = false;
  std::memset(self->vertex_buffers, 0, sizeof(self->vertex_buffers));
  HostList* h = self->current;
  if (h->recording) {  // reset without a close: the desktop wants the list closed first
    h->list->Close();
    h->recording = false;
  }
  if (in_flight(h)) {
    HostListPool& p = pool();
    HostList* next = nullptr;
    {
      std::lock_guard<std::mutex> guard(p.lock);
      std::vector<HostList*>& idle = p.idle[self->type & 3];
      for (size_t i = 0; i < idle.size(); ++i) {
        if (!in_flight(idle[i])) {
          next = idle[i];
          idle[i] = idle.back();
          idle.pop_back();
          break;
        }
      }
      idle.push_back(h);
    }
    if (!next) {
      next = new_host_list(self->device, self->type, host_allocator);
      if (!next) return E_OUTOFMEMORY;
      use_host_list(self, next);
      LIB_LOG(lib::kTrace, "d3d12", "list %p reset onto a new host list", static_cast<void*>(self));
      return S_OK;
    }
    h = next;
    use_host_list(self, h);
  }
  release_ngg_scratch(h);
  release_ring_chunks(h);
  const HRESULT hr = h->list->Reset(host_allocator, nullptr);
  if (SUCCEEDED(hr)) {
    h->recording = true;
    h->executed = false;
  }
  LIB_LOG(FAILED(hr) ? lib::kError : lib::kTrace, "d3d12", "list %p reset with allocator %p -> 0x%08lX",
          static_cast<void*>(self), allocator, static_cast<unsigned long>(hr));
  return hr;
}

// Observed: slot 32 takes (count, heaps): SetDescriptorHeaps (desktop slot
// 28). Command list slots from here to at least desktop slot 48 are the
// desktop's plus four.
void STDMETHODCALLTYPE list_set_descriptor_heaps(CommandList* self, UINT count, void* const* heaps) {
  ID3D12DescriptorHeap* host[2] = {};
  UINT used = 0;
  // The call replaces every bound heap.
  self->heaps[0] = self->heaps[1] = nullptr;
  self->memory_heap_bound = false;
  for (UINT i = 0; i < count && i < 2; ++i) {
    auto* heap = static_cast<DescriptorHeap*>(heaps[i]);
    if (!heap) continue;
    host[used++] = heap->host;
    self->heaps[heap->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0] = heap;
  }
  self->host->SetDescriptorHeaps(used, host);
}

// Observed: slot 52 is ClearRenderTargetView(handle, colour, rect count,
// rects) (desktop slot 48).
void STDMETHODCALLTYPE list_clear_render_target_view(CommandList* self, SIZE_T handle, const FLOAT* colour,
                                                    UINT rect_count, const D3D12_RECT* rects) {
  const D3D12_CPU_DESCRIPTOR_HANDLE host = host_cpu_handle(handle);
  if (!host.ptr) {
    LIB_LOG(lib::kError, "d3d12", "render target clear of an unknown descriptor %p", reinterpret_cast<void*>(handle));
    return;
  }
  g_work.clears.fetch_add(1, std::memory_order_relaxed);
  HOLY_TRACE("list %u clears %s to %g %g %g %g (%u rects)", self->number, describe_target(handle).c_str(),
             colour ? colour[0] : 0.0f, colour ? colour[1] : 0.0f, colour ? colour[2] : 0.0f,
             colour ? colour[3] : 0.0f, rect_count);
  if (colour && (colour[0] != 0 || colour[1] != 0 || colour[2] != 0)) {
    g_work.nonblack_clears.fetch_add(1, std::memory_order_relaxed);
  }
  self->host->ClearRenderTargetView(host, colour, rect_count, rects);
}

// GPU writes of values into title memory, carried out by the host's
// WriteBufferImmediate (32-bit values) at the translated address.
void write_immediate(CommandList* self, uint64_t address, const uint32_t* values, UINT count,
                     D3D12_WRITEBUFFERIMMEDIATE_MODE mode) {
  page_cache_gpu_writes(address, 4ull * count);
  const uint64_t host = translate_gpu_va(self->device, address);
  if (!host || !self->host2 || count > 2) {
    LIB_LOG(lib::kError, "d3d12", "immediate write to %p: no GPU memory there", reinterpret_cast<void*>(address));
    return;
  }
  LIB_LOG(lib::kTrace, "d3d12", "immediate write of %u words to %p (host %p)", count, reinterpret_cast<void*>(address),
          reinterpret_cast<void*>(host));
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameters[2];
  D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2];
  for (UINT i = 0; i < count; ++i) {
    parameters[i] = {host + 4ull * i, values[i]};
    modes[i] = mode;
  }
  self->host2->WriteBufferImmediate(count, parameters, modes);
}

// Observed: slot 71 takes (address, 32-bit value, 0, 0); titles use it for
// per-frame progress markers they keep in their memory. Assumed: the GPU
// writes the value when it reaches the command.
void STDMETHODCALLTYPE list_write_value_x(CommandList* self, uint64_t address, uint32_t value, uint64_t a,
                                          uint32_t b) {
  if (a || b) {
    LIB_LOG(lib::kDebug, "d3d12", "value write with options 0x%llx 0x%x", static_cast<unsigned long long>(a), b);
  }
  write_immediate(self, address, &value, 1, D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT);
}

// Observed: slot 74 takes (address, 7, value, 1); titles write a frame counter
// with it into memory the CPU polls. Assumed: a 64-bit value written once the
// work before it is done (the host's "marker out" mode).
void STDMETHODCALLTYPE list_write_fence_value_x(CommandList* self, uint64_t address, uint32_t kind, uint64_t value,
                                                uint32_t flags) {
  if (kind != 7 || flags != 1) LIB_LOG(lib::kError, "d3d12", "fence value write of kind %u flags 0x%x", kind, flags);
  const uint32_t values[2] = {static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32)};
  write_immediate(self, address, values, 2, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT);
}

// Observed: slot 72 takes (address, 0, 0, 1); titles call it for 8-byte
// entries of their GPU timing records, which they turn into time with the
// queue's timestamp frequency. Assumed: it writes a 64-bit GPU timestamp.
// Host: a timestamp query resolved into the title's memory.
struct TimestampHeap {
  ID3D12QueryHeap* heap = nullptr;
  std::atomic<uint32_t> next{0};
};
constexpr UINT kTimestamps = 8192;

TimestampHeap* timestamps(Device* device) {
  static TimestampHeap* instance = [&] {
    auto* made = new TimestampHeap();
    D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimestamps, 0};
    if (FAILED(device->host->CreateQueryHeap(&desc, IID_PPV_ARGS(&made->heap)))) made->heap = nullptr;
    return made;
  }();
  return instance;
}

void STDMETHODCALLTYPE list_write_timestamp_x(CommandList* self, uint64_t address, uint64_t a, uint64_t b,
                                              uint32_t flags) {
  if (a || b || flags != 1) {
    LIB_LOG(lib::kDebug, "d3d12", "timestamp write with options 0x%llx 0x%llx 0x%x",
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), flags);
  }
  page_cache_gpu_writes(address, 8);
  TimestampHeap* heap = timestamps(self->device);
  Backing backing;
  if (!heap->heap || self->type == D3D12_COMMAND_LIST_TYPE_COPY || !locate(self->device, address, &backing, 8) ||
      !backing.whole || (backing.heap_offset & 7)) {
    LIB_LOG(lib::kError, "d3d12", "timestamp write to %p: not possible there", reinterpret_cast<void*>(address));
    return;
  }
  const UINT index = heap->next.fetch_add(1) % kTimestamps;
  self->host->EndQuery(heap->heap, D3D12_QUERY_TYPE_TIMESTAMP, index);
  self->host->ResolveQueryData(heap->heap, D3D12_QUERY_TYPE_TIMESTAMP, index, 1, backing.whole, backing.heap_offset);
}

// Observed: slot 146 takes (group count, groups) in the layout of the desktop
// D3D12_BARRIER_GROUP (type, barrier count, barriers): enhanced barriers
// (desktop ID3D12GraphicsCommandList7::Barrier). Barriers are forwarded with
// the host resources behind the title's. Sync, access and layout values are
// the desktop's, except layout 31 (one past the desktop's last), which titles
// move display buffers to right before presenting them (observed): it is
// taken as the present (common) layout.
D3D12_BARRIER_LAYOUT host_layout(D3D12_BARRIER_LAYOUT layout) {
  if (layout == D3D12_BARRIER_LAYOUT_UNDEFINED || layout <= D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON) return layout;
  static std::atomic<int> logged{0};
  if (layout != 31 && logged.fetch_add(1) < 8) {
    LIB_LOG(lib::kError, "d3d12", "texture barrier with layout %u (not a desktop layout)", static_cast<unsigned>(layout));
  }
  return D3D12_BARRIER_LAYOUT_PRESENT;
}

// Copies a depth texture (in `layout`) into its twin (`back`: the twin into
// it), leaving both as they were: the twin in the unordered-access layout.
void copy_with_twin(CommandList* self, const Resource* title, D3D12_BARRIER_LAYOUT layout, bool back) {
  ID3D12Resource* from = back ? title->twin : title->host;
  ID3D12Resource* to = back ? title->host : title->twin;
  const D3D12_BARRIER_LAYOUT from_layout = back ? D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS : layout;
  const D3D12_BARRIER_LAYOUT to_layout = back ? layout : D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
  D3D12_TEXTURE_BARRIER barriers[2]{};
  barriers[0].SyncBefore = D3D12_BARRIER_SYNC_ALL;
  barriers[0].SyncAfter = D3D12_BARRIER_SYNC_COPY;
  barriers[0].AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
  barriers[0].AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
  barriers[0].LayoutBefore = from_layout;
  barriers[0].LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
  barriers[0].pResource = from;
  barriers[0].Subresources.IndexOrFirstMipLevel = 0xFFFFFFFF;  // all subresources
  barriers[1] = barriers[0];
  barriers[1].AccessAfter = D3D12_BARRIER_ACCESS_COPY_DEST;
  barriers[1].LayoutBefore = to_layout;
  barriers[1].LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_DEST;
  barriers[1].pResource = to;
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_TEXTURE;
  group.NumBarriers = 2;
  group.pTextureBarriers = barriers;
  self->host7->Barrier(1, &group);
  const D3D12_RESOURCE_DESC depth = title->host->GetDesc();
  if (planar_depth(depth.Format)) {
    // The twin holds the depth plane: plane 0's subresources, one by one.
    const UINT slices = depth.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : depth.DepthOrArraySize;
    for (UINT index = 0; index < static_cast<UINT>(depth.MipLevels) * slices; ++index) {
      D3D12_TEXTURE_COPY_LOCATION target{}, source{};
      target.pResource = to;
      source.pResource = from;
      target.Type = source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      target.SubresourceIndex = index;
      source.SubresourceIndex = index;
      self->host->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    }
  } else {
    self->host->CopyResource(to, from);
  }
  for (auto& b : barriers) {
    std::swap(b.SyncBefore, b.SyncAfter);
    std::swap(b.AccessBefore, b.AccessAfter);
    std::swap(b.LayoutBefore, b.LayoutAfter);
    b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    b.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
  }
  self->host7->Barrier(1, &group);
}

void STDMETHODCALLTYPE list_barrier(CommandList* self, UINT32 count, const D3D12_BARRIER_GROUP* groups) {
  if (!self->host7) {
    LIB_LOG(lib::kError, "d3d12", "the host has no enhanced barriers");
    HolyUnimplementedFrom(kLibrary, "enhanced barriers on a host without them", _ReturnAddress());
    return;
  }
  std::vector<D3D12_BARRIER_GROUP> host(groups, groups + count);
  std::vector<D3D12_TEXTURE_BARRIER> textures;
  std::vector<D3D12_BUFFER_BARRIER> buffers;
  size_t texture_count = 0, buffer_count = 0;
  for (const D3D12_BARRIER_GROUP& g : host) {
    if (g.Type == D3D12_BARRIER_TYPE_TEXTURE) texture_count += g.NumBarriers;
    if (g.Type == D3D12_BARRIER_TYPE_BUFFER) buffer_count += g.NumBarriers;
  }
  textures.reserve(texture_count);
  buffers.reserve(buffer_count);
  static std::atomic<int> logged{0};
  for (D3D12_BARRIER_GROUP& g : host) {
    if (g.Type == D3D12_BARRIER_TYPE_TEXTURE) {
      const size_t first = textures.size();
      for (UINT32 i = 0; i < g.NumBarriers; ++i) {
        D3D12_TEXTURE_BARRIER b = g.pTextureBarriers[i];
        if (logged.fetch_add(1) < 16) {
          LIB_LOG(lib::kDebug, "d3d12", "texture barrier sync 0x%x->0x%x access 0x%x->0x%x layout %u->%u flags 0x%x",
                  b.SyncBefore, b.SyncAfter, b.AccessBefore, b.AccessAfter, b.LayoutBefore, b.LayoutAfter, b.Flags);
        }
        const auto* title = static_cast<const Resource*>(static_cast<const void*>(b.pResource));
        HOLY_TRACE("list %u texture barrier %llx (%llux%u format %u) layout %u->%u sync 0x%x->0x%x access 0x%x->0x%x",
                   self->number, title ? static_cast<unsigned long long>(title->title_va) : 0ull,
                   title ? static_cast<unsigned long long>(title->desc.Width) : 0ull, title ? title->desc.Height : 0u,
                   title ? static_cast<unsigned>(title->desc.Format) : 0u, b.LayoutBefore, b.LayoutAfter, b.SyncBefore,
                   b.SyncAfter, b.AccessBefore, b.AccessAfter);
        b.pResource = title ? title->host : nullptr;
        b.LayoutBefore = host_texture_layout(title, host_layout(b.LayoutBefore));
        b.LayoutAfter = host_texture_layout(title, host_layout(b.LayoutAfter));
        if (title && title->title_va && placed_texture_follows_memory(title->desc)) {
          b.Flags &= ~D3D12_TEXTURE_BARRIER_FLAG_DISCARD;  // its contents are its memory's
        }
        note_texture_layout(static_cast<Resource*>(static_cast<void*>(g.pTextureBarriers[i].pResource)), b.LayoutAfter);
        textures.push_back(b);
      }
      g.pTextureBarriers = textures.data() + first;
    } else if (g.Type == D3D12_BARRIER_TYPE_BUFFER) {
      const size_t first = buffers.size();
      for (UINT32 i = 0; i < g.NumBarriers; ++i) {
        D3D12_BUFFER_BARRIER b = g.pBufferBarriers[i];
        b.pResource = b.pResource ? static_cast<Resource*>(static_cast<void*>(b.pResource))->host : nullptr;
        buffers.push_back(b);
      }
      g.pBufferBarriers = buffers.data() + first;
    }
  }
  self->host7->Barrier(count, host.data());
  // Depth textures with an unordered-access twin (Resource::twin): their
  // contents move to the twin when the title moves them into the
  // unordered-access layout, and back when it moves them out.
  for (UINT32 g = 0; g < count; ++g) {
    if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
    for (UINT32 i = 0; i < groups[g].NumBarriers; ++i) {
      const D3D12_TEXTURE_BARRIER& b = groups[g].pTextureBarriers[i];
      const auto* title = static_cast<const Resource*>(static_cast<const void*>(b.pResource));
      if (!title || !title->twin) continue;
      const bool into = b.LayoutAfter == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS &&
                        b.LayoutBefore != D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
      const bool out = b.LayoutBefore == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS &&
                       b.LayoutAfter != D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
      if (into || out) copy_with_twin(self, title, host_texture_layout(title, host_layout(b.LayoutAfter)), out);
    }
  }
}

// Observed: slot 20 is IASetPrimitiveTopology, at its desktop position.
// Titles also set topology 17 (0x11), which the desktop does not define: the
// console GPU's rectangle list (documented in public RDNA register references
// as DI_PT_RECTLIST, 0x11). Each three vertices are three corners of a
// rectangle whose fourth corner the GPU adds. Passed to the host as is, an
// undefined topology removes the host device (as hung) at the next work the
// list submits. Host, partial: rectangle lists are drawn as triangle lists,
// which draws one half of each rectangle; other topologies the desktop does
// not define report themselves.
constexpr D3D12_PRIMITIVE_TOPOLOGY kRectangleList = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(17);

bool desktop_topology(D3D12_PRIMITIVE_TOPOLOGY t) {
  return (t >= D3D_PRIMITIVE_TOPOLOGY_UNDEFINED && t <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) ||
         (t >= D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && t <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) ||
         (t >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
}

void STDMETHODCALLTYPE list_set_primitive_topology(CommandList* self, D3D12_PRIMITIVE_TOPOLOGY topology) {
  self->rectangles = topology == kRectangleList;
  if (topology == kRectangleList) {
    // Drawn as triangle lists with a pipeline whose geometry program adds
    // each rectangle's fourth corner (pipeline_host_rectangles).
    topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  } else if (!desktop_topology(topology)) {
    LIB_LOG(lib::kError, "d3d12", "primitive topology %d is not a desktop topology", static_cast<int>(topology));
    HolyUnimplementedFrom(kLibrary, "IASetPrimitiveTopology(console topology)", _ReturnAddress());
    return;
  }
  self->topology = topology;
  HOLY_TRACE("list %u topology %d%s", self->number, static_cast<int>(topology), self->rectangles ? " (rectangles)" : "");
  self->host->IASetPrimitiveTopology(topology);
}

// Observed: slot 126 takes (rate, combiners[2]) as the desktop
// RSSetShadingRate: titles pass the bytes of one packed state word (rate,
// then the two combiners). Carried out when the host has variable-rate
// shading (combiners only from tier 2); without it, pixels are shaded at
// full rate, which the rate only permits coarsening.
void STDMETHODCALLTYPE list_set_shading_rate(CommandList* self, D3D12_SHADING_RATE rate,
                                             const D3D12_SHADING_RATE_COMBINER* combiners) {
  static const int tier = [&] {
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
    if (FAILED(self->device->host->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options)))) {
      return 0;
    }
    LIB_LOG(lib::kInfo, "d3d12", "host variable-rate shading tier %d", static_cast<int>(options.VariableShadingRateTier));
    return static_cast<int>(options.VariableShadingRateTier);
  }();
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    LIB_LOG(lib::kDebug, "d3d12", "shading rate 0x%x, combiners %u %u", static_cast<unsigned>(rate),
            combiners ? static_cast<unsigned>(combiners[0]) : 0u, combiners ? static_cast<unsigned>(combiners[1]) : 0u);
  }
  auto valid_rate = [](uint32_t r) { return r == 0 || r == 1 || r == 4 || r == 5 || r == 6 || r == 9 || r == 10; };
  if (!valid_rate(static_cast<uint32_t>(rate)) || (combiners && (combiners[0] > 4 || combiners[1] > 4))) {
    LIB_LOG(lib::kError, "d3d12", "shading rate 0x%x / combiners outside the desktop values", static_cast<unsigned>(rate));
    return;
  }
  if (tier == 0 || self->type == D3D12_COMMAND_LIST_TYPE_COMPUTE || self->type == D3D12_COMMAND_LIST_TYPE_COPY) return;
  ID3D12GraphicsCommandList5* list5 = nullptr;
  if (FAILED(self->host->QueryInterface(IID_PPV_ARGS(&list5)))) return;
  list5->RSSetShadingRate(rate, tier >= 2 ? combiners : nullptr);
  list5->Release();
}

// Slots 131 and 132 (observed once per list at start-up: (0xc3c0,
// 0x80020000, 0, 0xffffffffff00) and (0x700000, 0, 0, 0xffffffffff00), no
// result checked). Assumed: they program GPU state through register offset,
// value and mask, as the console GPU's driver interface allows. The host
// driver owns its GPU's register state, so the values are recorded in the
// log and not applied.
void STDMETHODCALLTYPE list_hardware_state(CommandList*, uint32_t a, uint32_t b, uint32_t c, uint64_t mask) {
  LIB_LOG(lib::kDebug, "d3d12", "hardware state 0x%x 0x%x 0x%x mask 0x%llx (not applied on the host)", a, b, c,
          static_cast<unsigned long long>(mask));
}

// --- pipeline state, root arguments, draws and dispatches ----------------------------
//
// Observed: slots 0-25 keep the desktop order (12 DrawInstanced, 13
// DrawIndexedInstanced, 14 Dispatch, 20-25 topology, viewports, scissors,
// blend factor, stencil reference, pipeline state); from desktop slot 28
// (SetDescriptorHeaps) to 48 (ClearRenderTargetView) the slots are the
// desktop's plus four (33-46 root signatures and arguments, 47 index buffer,
// 48 vertex buffers, 50 render targets, 51 depth-stencil clears), and
// OMSetDepthBounds (desktop 62) is slot 70.

bool is_translated(const void* pipeline) {
  return pipeline && (static_cast<const Wrapper*>(pipeline)->flags & kTranslatedPipeline) != 0;
}

void STDMETHODCALLTYPE list_set_pipeline_state(CommandList* self, void* pipeline) {
  self->pipeline = pipeline;
  self->translated = is_translated(pipeline);
  if (frame_traced()) {
    HOLY_TRACE("list %u pipeline %p%s: %ls", self->number, pipeline, self->translated ? " (translated)" : "",
               pipeline_label(pipeline).c_str());
  }
  if (auto* host = pipeline_host(pipeline)) {
    self->host->SetPipelineState(host);
    self->bound = host;
  } else if (pipeline) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) LIB_LOG(lib::kError, "d3d12", "pipeline %p has no host pipeline; its draws are dropped", pipeline);
    self->pipeline = nullptr;
    self->translated = false;
  }
}

void set_root_signature(CommandList* self, bool compute, void* signature) {
  RootBinding& binding = compute ? self->compute : self->graphics;
  if (binding.signature != signature) {
    std::memset(binding.packed, 0, sizeof(binding.packed));
    std::memset(binding.values, 0, sizeof(binding.values));
  }
  binding.signature = signature;
  binding.host_is_title = false;
  bind_title_root(self, compute);
}
void STDMETHODCALLTYPE list_set_compute_root_signature(CommandList* self, void* signature) {
  set_root_signature(self, true, signature);
}
void STDMETHODCALLTYPE list_set_graphics_root_signature(CommandList* self, void* signature) {
  set_root_signature(self, false, signature);
}

// Records a root argument and, while the title's root signature is bound on
// the host, sets it there too.
const RootParameterLayout* parameter(CommandList* self, bool compute, UINT index) {
  const RootBinding& binding = compute ? self->compute : self->graphics;
  const RootLayout* layout = root_layout(binding.signature);
  if (!layout || index >= layout->parameters.size() || index >= 64) {
    LIB_LOG(lib::kError, "d3d12", "root argument %u for a root signature without it", index);
    return nullptr;
  }
  return &layout->parameters[index];
}

void set_table(CommandList* self, bool compute, UINT index, uint64_t handle) {
  if (!parameter(self, compute, index)) return;
  RootBinding& binding = compute ? self->compute : self->graphics;
  binding.values[index] = handle;
  binding.packed[root_layout(binding.signature)->parameters[index].offset] = static_cast<uint32_t>(handle);
  if (!binding.host_is_title) return;
  const D3D12_GPU_DESCRIPTOR_HANDLE host = host_gpu_handle(handle);
  if (!host.ptr) return;
  compute ? self->host->SetComputeRootDescriptorTable(index, host) : self->host->SetGraphicsRootDescriptorTable(index, host);
}
void STDMETHODCALLTYPE list_set_compute_root_table(CommandList* self, UINT index, uint64_t handle) {
  set_table(self, true, index, handle);
}
void STDMETHODCALLTYPE list_set_graphics_root_table(CommandList* self, UINT index, uint64_t handle) {
  set_table(self, false, index, handle);
}

void set_constants(CommandList* self, bool compute, UINT index, UINT count, const void* values, UINT first) {
  const RootParameterLayout* p = parameter(self, compute, index);
  if (!p || first + count > p->dwords) return;
  RootBinding& binding = compute ? self->compute : self->graphics;
  std::memcpy(binding.packed + p->offset + first, values, 4ull * count);
  if (!binding.host_is_title) return;
  compute ? self->host->SetComputeRoot32BitConstants(index, count, values, first)
          : self->host->SetGraphicsRoot32BitConstants(index, count, values, first);
}
void STDMETHODCALLTYPE list_set_compute_root_constant(CommandList* self, UINT index, UINT value, UINT first) {
  set_constants(self, true, index, 1, &value, first);
}
void STDMETHODCALLTYPE list_set_graphics_root_constant(CommandList* self, UINT index, UINT value, UINT first) {
  set_constants(self, false, index, 1, &value, first);
}
void STDMETHODCALLTYPE list_set_compute_root_constants(CommandList* self, UINT index, UINT count, const void* values,
                                                       UINT first) {
  set_constants(self, true, index, count, values, first);
}
void STDMETHODCALLTYPE list_set_graphics_root_constants(CommandList* self, UINT index, UINT count, const void* values,
                                                        UINT first) {
  set_constants(self, false, index, count, values, first);
}

void set_view(CommandList* self, bool compute, UINT index, uint64_t address, D3D12_ROOT_PARAMETER_TYPE type) {
  const RootParameterLayout* p = parameter(self, compute, index);
  if (!p) return;
  RootBinding& binding = compute ? self->compute : self->graphics;
  binding.values[index] = address;
  binding.packed[p->offset] = static_cast<uint32_t>(address);
  binding.packed[p->offset + 1] = static_cast<uint32_t>(address >> 32);
  if (!binding.host_is_title) return;
  const uint64_t host = translate_gpu_va(self->device, address);
  if (!host && address) {
    LIB_LOG(lib::kError, "d3d12", "root view at %p lies in no GPU memory", reinterpret_cast<void*>(address));
    return;
  }
  ID3D12GraphicsCommandList* l = self->host;
  if (type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
    compute ? l->SetComputeRootConstantBufferView(index, host) : l->SetGraphicsRootConstantBufferView(index, host);
  } else if (type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
    compute ? l->SetComputeRootShaderResourceView(index, host) : l->SetGraphicsRootShaderResourceView(index, host);
  } else {
    compute ? l->SetComputeRootUnorderedAccessView(index, host) : l->SetGraphicsRootUnorderedAccessView(index, host);
  }
}
void STDMETHODCALLTYPE list_set_compute_root_cbv(CommandList* self, UINT index, uint64_t address) {
  set_view(self, true, index, address, D3D12_ROOT_PARAMETER_TYPE_CBV);
}
void STDMETHODCALLTYPE list_set_graphics_root_cbv(CommandList* self, UINT index, uint64_t address) {
  set_view(self, false, index, address, D3D12_ROOT_PARAMETER_TYPE_CBV);
}
void STDMETHODCALLTYPE list_set_compute_root_srv(CommandList* self, UINT index, uint64_t address) {
  set_view(self, true, index, address, D3D12_ROOT_PARAMETER_TYPE_SRV);
}
void STDMETHODCALLTYPE list_set_graphics_root_srv(CommandList* self, UINT index, uint64_t address) {
  set_view(self, false, index, address, D3D12_ROOT_PARAMETER_TYPE_SRV);
}
void STDMETHODCALLTYPE list_set_compute_root_uav(CommandList* self, UINT index, uint64_t address) {
  set_view(self, true, index, address, D3D12_ROOT_PARAMETER_TYPE_UAV);
}
void STDMETHODCALLTYPE list_set_graphics_root_uav(CommandList* self, UINT index, uint64_t address) {
  set_view(self, false, index, address, D3D12_ROOT_PARAMETER_TYPE_UAV);
}

void STDMETHODCALLTYPE list_set_index_buffer(CommandList* self, const D3D12_INDEX_BUFFER_VIEW* view) {
  if (!view) {
    self->index_buffer_set = false;
    self->host->IASetIndexBuffer(nullptr);
    return;
  }
  D3D12_INDEX_BUFFER_VIEW host = *view;
  host.BufferLocation = translate_gpu_va(self->device, view->BufferLocation);
  if (!host.BufferLocation && view->BufferLocation) {
    LIB_LOG(lib::kError, "d3d12", "index buffer at %p lies in no GPU memory", reinterpret_cast<void*>(view->BufferLocation));
    return;
  }
  self->index_buffer = host;
  self->index_buffer_set = true;
  self->host->IASetIndexBuffer(&host);
}

void STDMETHODCALLTYPE list_set_vertex_buffers(CommandList* self, UINT first, UINT count,
                                               const D3D12_VERTEX_BUFFER_VIEW* views) {
  D3D12_VERTEX_BUFFER_VIEW host[16] = {};
  UINT n = 0;
  for (UINT i = 0; i < count && first + i < 16; ++i, ++n) {
    self->vertex_buffers[first + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
    host[i] = self->vertex_buffers[first + i];
    host[i].BufferLocation = translate_gpu_va(self->device, host[i].BufferLocation);
  }
  self->host->IASetVertexBuffers(first, n, views ? host : nullptr);
}

void STDMETHODCALLTYPE list_set_render_targets(CommandList* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
                                               BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
  D3D12_CPU_DESCRIPTOR_HANDLE host[8] = {};
  const UINT n = count > 8 ? 8 : count;
  for (UINT i = 0; i < n && targets; ++i) {
    host[i] = host_cpu_handle(targets[single ? 0 : i].ptr);
    if (single) break;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE host_depth{};
  if (depth) host_depth = host_cpu_handle(depth->ptr);
  self->depth_format = depth ? depth_view_format(depth->ptr) : DXGI_FORMAT_UNKNOWN;
  if (frame_traced()) {
    for (UINT i = 0; i < n && targets; ++i) {
      HOLY_TRACE("list %u render target %u: %s", self->number, i, describe_target(targets[single ? 0 : i].ptr).c_str());
    }
    if (depth) HOLY_TRACE("list %u depth target: %s", self->number, describe_target(depth->ptr).c_str());
  }
  self->host->OMSetRenderTargets(n, targets ? host : nullptr, single, depth ? &host_depth : nullptr);
}

void STDMETHODCALLTYPE list_clear_depth_stencil_view(CommandList* self, SIZE_T handle, D3D12_CLEAR_FLAGS flags,
                                                     FLOAT depth, UINT8 stencil, UINT rect_count,
                                                     const D3D12_RECT* rects) {
  const D3D12_CPU_DESCRIPTOR_HANDLE host = host_cpu_handle(handle);
  if (!host.ptr) return;
  g_work.depth_clears.fetch_add(1, std::memory_order_relaxed);
  HOLY_TRACE("list %u clears depth %s to %g / %u (flags 0x%x)", self->number, describe_target(handle).c_str(), depth,
             stencil, flags);
  self->host->ClearDepthStencilView(host, flags, depth, stencil, rect_count, rects);
}

void STDMETHODCALLTYPE list_set_viewports(CommandList* self, UINT count, const D3D12_VIEWPORT* viewports) {
  if (count && viewports) {
    HOLY_TRACE("list %u viewport %g %g %g x %g depth %g-%g (%u viewports)", self->number, viewports[0].TopLeftX,
               viewports[0].TopLeftY, viewports[0].Width, viewports[0].Height, viewports[0].MinDepth,
               viewports[0].MaxDepth, count);
  }
  self->host->RSSetViewports(count, viewports);
}
void STDMETHODCALLTYPE list_set_scissors(CommandList* self, UINT count, const D3D12_RECT* rects) {
  if (count && rects) {
    HOLY_TRACE("list %u scissor %ld %ld %ld %ld", self->number, rects[0].left, rects[0].top, rects[0].right,
               rects[0].bottom);
  }
  self->host->RSSetScissorRects(count, rects);
}
void STDMETHODCALLTYPE list_set_blend_factor(CommandList* self, const FLOAT* factor) {
  if (factor) HOLY_TRACE("list %u blend factor %g %g %g %g", self->number, factor[0], factor[1], factor[2], factor[3]);
  self->host->OMSetBlendFactor(factor);
}
// Observed (the title's own code): the stencil reference is the one argument
// (titles write it into the list's command memory themselves when there is
// room, calling this slot when there is none: always, with this runtime's
// lists).
void STDMETHODCALLTYPE list_set_stencil_ref(CommandList* self, UINT reference) {
  HOLY_TRACE("list %u stencil reference %u", self->number, reference);
  self->host->OMSetStencilRef(reference);
}
void STDMETHODCALLTYPE list_set_depth_bounds(CommandList* self, FLOAT min, FLOAT max) {
  ID3D12GraphicsCommandList1* list1 = nullptr;
  if (SUCCEEDED(self->host->QueryInterface(IID_PPV_ARGS(&list1)))) {
    list1->OMSetDepthBounds(min, max);
    list1->Release();
  }
}

// Observed: slot 142 is called with all ones before draws, a 64-bit mask.
// Assumed: the sample mask. The host's is part of the pipeline; masks other
// than all samples report themselves.
void STDMETHODCALLTYPE list_set_sample_mask(CommandList*, uint64_t mask) {
  if (mask == UINT64_MAX || (mask & 0xFFFFFFFFu) == 0xFFFFFFFFu) return;
  LIB_LOG(lib::kError, "d3d12", "sample mask 0x%llx", static_cast<unsigned long long>(mask));
  HolyUnimplementedFrom(kLibrary, "a dynamic sample mask (command list slot 142)", _ReturnAddress());
}

// Observed: slot 75 takes one byte, which titles derive from bit 6 of a
// pipeline property byte and set only when it changes. Its meaning is not
// established; the call is recorded and not applied.
void STDMETHODCALLTYPE list_set_unknown_75(CommandList*, uint32_t value) {
  static std::atomic<int> logged{0};
  if ((value & 0xFF) && logged.fetch_add(1) < 4) {
    LIB_LOG(lib::kWarn, "d3d12", "command list slot 75 with %u (not applied: meaning not established)", value & 0xFF);
  }
}

void report_unbound(const char* what) {
  g_work.unbound.fetch_add(1, std::memory_order_relaxed);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) LIB_LOG(lib::kError, "d3d12", "%s with a translated pipeline could not be bound", what);
}

// With debug logging: a host marker naming the pipeline's programs, which
// device-removed data reports for work that did not finish.
void mark_work(CommandList* self) {
  if (!HolyLogEnabled(lib::kDebug)) return;
  const std::wstring label = pipeline_label(self->pipeline);
  if (label.empty()) {
    LIB_LOG(lib::kDebug, "d3d12", "work on title list %u: pipeline %p (not translated)", self->number, self->pipeline);
    return;
  }
  self->host->SetMarker(0, label.c_str(), static_cast<UINT>((label.size() + 1) * sizeof(wchar_t)));
  // Drivers need not keep marker strings; the log ties work to its list.
  LIB_LOG(lib::kDebug, "d3d12", "work on title list %u: %ls", self->number, label.c_str());
}

// For traces: what a draw reads, as far as the CPU can see it now: each bound
// vertex buffer with its first vertex (as floats), and the packed root
// arguments (the title may still write the memory before the work runs).
void trace_draw_inputs(const CommandList* self) {
  for (int i = 0; i < 16; ++i) {
    const D3D12_VERTEX_BUFFER_VIEW& v = self->vertex_buffers[i];
    if (!v.BufferLocation) continue;
    float first[8] = {};
    const uint32_t bytes = std::min<uint32_t>(v.SizeInBytes, sizeof(first));
    MEMORY_BASIC_INFORMATION info{};
    const bool readable = VirtualQuery(reinterpret_cast<const void*>(v.BufferLocation), &info, sizeof(info)) &&
                          info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (readable) std::memcpy(first, reinterpret_cast<const void*>(v.BufferLocation), bytes);
    HOLY_TRACE("list %u   vertex buffer %d: %llx, %u bytes, stride %u; first vertex %g %g %g %g | %g %g %g %g%s",
               self->number, i, static_cast<unsigned long long>(v.BufferLocation), v.SizeInBytes, v.StrideInBytes,
               first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7],
               readable ? "" : " (not readable)");
  }
  std::string root;
  for (int i = 0; i < 16; ++i) {
    char word[12];
    std::snprintf(word, sizeof(word), " %08x", self->graphics.packed[i]);
    root += word;
  }
  HOLY_TRACE("list %u   graphics root arguments:%s", self->number, root.c_str());
  // The image views behind each packed root argument that is a 32-bit
  // address in a shader-visible heap (titles pass descriptor tables so, and
  // native code reads descriptors there): the heap slot a translated program
  // samples (dword 0 of the descriptor, in the bound heap) and what the view
  // there was made as; a descriptor whose dword 0 is not its own slot is
  // marked (descriptor memory copied without the copy calls).
  const DescriptorHeap* bound = self->heaps[0];
  for (int a = 0; bound && a < 16; ++a) {
    DescriptorLocation where;
    if (!self->graphics.packed[a] || !find_gpu_descriptor(self->graphics.packed[a], &where)) continue;
    const uint32_t count = std::min<uint32_t>(8, where.heap->host_count - where.index);
    for (uint32_t i = 0; i < count; ++i) {
      const auto* words = reinterpret_cast<const uint32_t*>(where.title + static_cast<uint64_t>(i) * where.heap->stride);
      if (words[1] != kDescriptorMarker) continue;
      const std::string view = describe_view(bound->title_cpu + static_cast<uint64_t>(words[0]) * bound->stride);
      HOLY_TRACE("list %u   argument %d descriptor %u: slot %u%s, kind %u: %s", self->number, a, i, words[0],
                 words[0] == where.index + i ? "" : " (not its own slot)", words[2],
                 view.empty() ? "(view not known)" : view.c_str());
    }
  }
}

// The host pipeline a draw needs: the title pipeline's, or its rectangle
// list variant while the topology is a rectangle list.
void bind_draw_pipeline(CommandList* self) {
  ID3D12PipelineState* wanted = nullptr;
  if (self->rectangles) {
    wanted = pipeline_host_rectangles(self->pipeline);
    static std::atomic<bool> logged{false};
    if (!wanted && !logged.exchange(true)) {
      LIB_LOG(lib::kWarn, "d3d12", "rectangle lists drawn as triangle lists (half of each rectangle)");
    }
  }
  if (!wanted) wanted = pipeline_host_depth(self->pipeline, self->depth_format);
  if (!wanted) wanted = pipeline_host(self->pipeline);
  if (wanted && wanted != self->bound) {
    self->host->SetPipelineState(wanted);
    self->bound = wanted;
  }
}

void STDMETHODCALLTYPE list_draw_instanced(CommandList* self, UINT vertices, UINT instances, UINT first_vertex,
                                           UINT first_instance) {
  if (!self->pipeline) {  // no host pipeline (reported when it was set)
    g_work.no_pipeline.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_work.draws.fetch_add(1, std::memory_order_relaxed);
  HOLY_TRACE("list %u draws %u vertices x %u from %u / %u", self->number, vertices, instances, first_vertex,
             first_instance);
  if (frame_traced()) trace_draw_inputs(self);
  mark_work(self);
  NggPipeline ngg;
  if (self->translated && ngg_pipeline(self->pipeline, &ngg)) {
    g_work.ngg.fetch_add(1, std::memory_order_relaxed);
    if (!draw_ngg(self, ngg, vertices, instances, first_vertex, first_instance)) report_unbound("an NGG draw");
    self->bound = nullptr;  // the NGG passes set their own pipelines
    return;
  }
  bind_draw_pipeline(self);
  if (self->translated) {
    // Measured: the host's vertex index (SV_VertexID) does not include the
    // first vertex (a point drawn from vertex 5 has index 0). Native code adds
    // s2 to its vertex index, as the console's draws without indices pass the
    // first vertex there: so the first vertex goes in s2.
    if (!bind_translated_graphics(self, first_vertex, first_instance)) return report_unbound("a draw");
  } else {
    bind_title_root(self, false);
  }
  self->host->DrawInstanced(vertices, instances, first_vertex, first_instance);
}

void STDMETHODCALLTYPE list_draw_indexed_instanced(CommandList* self, UINT indices, UINT instances, UINT first_index,
                                                   INT base_vertex, UINT first_instance) {
  if (!self->pipeline) {  // no host pipeline (reported when it was set)
    g_work.no_pipeline.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_work.indexed.fetch_add(1, std::memory_order_relaxed);
  HOLY_TRACE("list %u draws %u indices x %u from %u, base %d / %u", self->number, indices, instances, first_index,
             base_vertex, first_instance);
  if (frame_traced()) trace_draw_inputs(self);
  mark_work(self);
  NggPipeline ngg;
  if (self->translated && ngg_pipeline(self->pipeline, &ngg)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) LIB_LOG(lib::kError, "d3d12", "indexed draw with an NGG pipeline: not drawn");
    return;
  }
  bind_draw_pipeline(self);
  if (self->translated) {
    // The host's vertex index is the index; native code adds the base vertex.
    if (!bind_translated_graphics(self, static_cast<uint32_t>(base_vertex), first_instance)) {
      return report_unbound("an indexed draw");
    }
    self->host->DrawIndexedInstanced(indices, instances, first_index, 0, first_instance);
    return;
  }
  bind_title_root(self, false);
  self->host->DrawIndexedInstanced(indices, instances, first_index, base_vertex, first_instance);
}

// --- copies ----------------------------------------------------------------------------
//
// Assumed (slots 0-25 keep the desktop order, observed for the others): slots
// 15, 16 and 17 are CopyBufferRegion, CopyTextureRegion and CopyResource. The
// host resources behind the title's take the copies; a buffer placed in title
// memory starts `slack` bytes into its host buffer (resource.cpp). Partial:
// copies into title memory under a texture the title views there do not
// reach the texture's host copy (GPU writes under textures, as elsewhere).

uint64_t buffer_slack(const Resource* resource) {
  if (resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !resource->host) return 0;
  return resource->host_va - resource->host->GetGPUVirtualAddress();
}

const Resource* copied(const void* resource, const char* what) {
  const auto* title = static_cast<const Resource*>(resource);
  if (title && title->host) return title;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) LIB_LOG(lib::kError, "d3d12", "%s with a resource %p that has no host resource: dropped", what, resource);
  return nullptr;
}

// A buffer copy of `bytes` between host buffers at host offsets, kept inside
// both. Copies cannot report errors: one past a host buffer's end removes
// the host device ("invalid call"). Host buffers may be shorter than the
// title's (place_buffer: memory not in order), and titles' ranges are not
// checked on the console.
void copy_inside(CommandList* self, ID3D12Resource* to, UINT64 to_offset, ID3D12Resource* from, UINT64 from_offset,
                 UINT64 bytes) {
  const UINT64 to_width = to->GetDesc().Width, from_width = from->GetDesc().Width;
  const UINT64 room = std::min(to_offset < to_width ? to_width - to_offset : 0,
                               from_offset < from_width ? from_width - from_offset : 0);
  if (bytes > room) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      LIB_LOG(lib::kWarn, "d3d12", "buffer copy of 0x%llx bytes (0x%llx -> 0x%llx) reaches past a host buffer "
              "(0x%llx, 0x%llx bytes): 0x%llx copied", static_cast<unsigned long long>(bytes),
              static_cast<unsigned long long>(from_offset), static_cast<unsigned long long>(to_offset),
              static_cast<unsigned long long>(from_width), static_cast<unsigned long long>(to_width),
              static_cast<unsigned long long>(room));
    }
    bytes = room;
  }
  if (bytes) self->host->CopyBufferRegion(to, to_offset, from, from_offset, bytes);
}

void STDMETHODCALLTYPE list_copy_buffer_region(CommandList* self, void* destination, UINT64 destination_offset,
                                               void* source, UINT64 source_offset, UINT64 bytes) {
  const Resource* to = copied(destination, "a buffer copy");
  const Resource* from = copied(source, "a buffer copy");
  if (!to || !from) return;
  HOLY_TRACE("list %u copies 0x%llx bytes between buffers", self->number, static_cast<unsigned long long>(bytes));
  page_cache_gpu_writes(to->title_va + destination_offset, bytes);
  copy_inside(self, to->host, destination_offset + buffer_slack(to), from->host, source_offset + buffer_slack(from),
              bytes);
}

// Observed: slot 62 takes (address, address, byte count, 0). A title locks
// a texture for the CPU with it: it allocates a staging buffer, passes the
// staging buffer first and the texture memory's GPU address second, waits for
// the list's work, then reads the staging buffer. Taken as a copy of memory
// on the GPU timeline from the second address to the first (the staging
// buffer is fresh when it is called: a copy the other way would only spread
// what it held). Host: copies between the buffers spanning the heaps over the
// two ranges, a piece per heap. Memory the GPU wrote into host textures of
// its own (render targets) is not in title memory, so it is not copied.
void STDMETHODCALLTYPE list_copy_memory_x(CommandList* self, uint64_t destination, uint64_t source, uint64_t bytes,
                                          uint32_t flags) {
  if (flags) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) LIB_LOG(lib::kWarn, "d3d12", "memory copy with flags 0x%x (taken as none)", flags);
  }
  HOLY_TRACE("list %u copies 0x%llx bytes of memory %p -> %p", self->number, static_cast<unsigned long long>(bytes),
             reinterpret_cast<void*>(source), reinterpret_cast<void*>(destination));
  page_cache_gpu_writes(destination, bytes);
  for (uint64_t done = 0; done < bytes;) {
    Backing to, from;
    if (!locate_range(self->device, destination + done, bytes - done, &to) ||
        !locate_range(self->device, source + done, bytes - done, &from) || !to.whole || !from.whole) {
      LIB_LOG(lib::kError, "d3d12", "memory copy of 0x%llx bytes %p -> %p: no GPU memory at 0x%llx of it",
              static_cast<unsigned long long>(bytes), reinterpret_cast<void*>(source),
              reinterpret_cast<void*>(destination), static_cast<unsigned long long>(done));
      return;
    }
    const uint64_t piece = std::min({bytes - done, to.contiguous, from.contiguous});
    if (!piece) return;
    self->host->CopyBufferRegion(to.whole, to.heap_offset, from.whole, from.heap_offset, piece);
    done += piece;
  }
}

bool host_copy_location(const D3D12_TEXTURE_COPY_LOCATION& title, D3D12_TEXTURE_COPY_LOCATION* host) {
  const Resource* resource = copied(title.pResource, "a texture copy");
  if (!resource) return false;
  *host = title;
  host->pResource = resource->host;
  if (title.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    host->PlacedFootprint.Offset += buffer_slack(resource);
    host->PlacedFootprint.Footprint.Format = desktop_format(title.PlacedFootprint.Footprint.Format);
  }
  return true;
}

void STDMETHODCALLTYPE list_copy_texture_region(CommandList* self, const D3D12_TEXTURE_COPY_LOCATION* destination,
                                                UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                                const D3D12_BOX* box) {
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  if (!destination || !source || !host_copy_location(*destination, &to) || !host_copy_location(*source, &from)) return;
  if (destination->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    const auto* target = static_cast<const Resource*>(static_cast<const void*>(destination->pResource));
    texture_written_by_gpu(target->desc, target->title_va);
  }
  HOLY_TRACE("list %u copies a texture region", self->number);
  if (destination->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
    const D3D12_SUBRESOURCE_FOOTPRINT& f = destination->PlacedFootprint.Footprint;
    page_cache_gpu_writes(static_cast<const Resource*>(static_cast<const void*>(destination->pResource))->title_va +
                              destination->PlacedFootprint.Offset,
                          static_cast<uint64_t>(f.RowPitch) * f.Height * f.Depth);
  }
  self->host->CopyTextureRegion(&to, x, y, z, &from, box);
}

void STDMETHODCALLTYPE list_copy_resource(CommandList* self, void* destination, void* source) {
  const Resource* to = copied(destination, "a resource copy");
  const Resource* from = copied(source, "a resource copy");
  if (!to || !from) return;
  HOLY_TRACE("list %u copies a resource", self->number);
  texture_written_by_gpu(to->desc, to->title_va);
  if (to->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) page_cache_gpu_writes(to->title_va, to->desc.Width);
  if (to->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && from->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    // Host buffers of placed buffers are wider than the title's (or shorter).
    copy_inside(self, to->host, buffer_slack(to), from->host, buffer_slack(from),
                std::min(to->desc.Width, from->desc.Width));
    return;
  }
  self->host->CopyResource(to->host, from->host);
}

void STDMETHODCALLTYPE list_dispatch(CommandList* self, UINT x, UINT y, UINT z) {
  if (!self->pipeline) {  // no host pipeline (reported when it was set)
    g_work.no_pipeline.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_work.dispatches.fetch_add(1, std::memory_order_relaxed);
  HOLY_TRACE("list %u dispatches %u x %u x %u", self->number, x, y, z);
  mark_work(self);
  if (self->translated) {
    if (!bind_translated_compute(self)) return report_unbound("a dispatch");
  } else {
    bind_title_root(self, true);
  }
  self->host->Dispatch(x, y, z);
}

// Observed (call shapes; slot numbers as an established map of the console
// list gives them): slot 80 DispatchX(x, y, z, flags), a dispatch with
// console flags; flag 0x20 makes the counts threads rather than groups
// (*assumed* from that map), converted with the program's group size. Other
// flags are logged once and ignored.
void STDMETHODCALLTYPE list_dispatch_x(CommandList* self, UINT x, UINT y, UINT z, UINT flags) {
  constexpr UINT kThreadCounts = 0x20;
  if (flags & ~kThreadCounts) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) LIB_LOG(lib::kWarn, "d3d12", "console dispatch flags 0x%x not followed", flags);
  }
  if (flags & kThreadCounts) {
    const Pipeline* pipeline = as_pipeline(self->pipeline);
    if (!pipeline || !pipeline->threads[0]) {
      HolyUnimplementedFrom(kLibrary, "ID3D12GraphicsCommandListX#80(thread counts, group size not known)",
                            _ReturnAddress());
      return;
    }
    x = (x + pipeline->threads[0] - 1) / pipeline->threads[0];
    y = (y + pipeline->threads[1] - 1) / pipeline->threads[1];
    z = (z + pipeline->threads[2] - 1) / pipeline->threads[2];
  }
  list_dispatch(self, x, y, z);
}

// Slot 101 DriverHintX(feature, value) (call shape (2, 1, 4) seen): a hint to
// the console's driver; the host's has no use for it.
uint64_t STDMETHODCALLTYPE list_driver_hint(CommandList*, uint64_t feature, uint64_t value) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) {
    LIB_LOG(lib::kDebug, "d3d12", "driver hint %llu = %llu: not used", static_cast<unsigned long long>(feature),
            static_cast<unsigned long long>(value));
  }
  return 0;
}

// Observed: slot 129 DispatchMesh(x, y, z), the desktop's mesh-program
// dispatch (34 x 1 x 1 seen during a title's loading). Not implemented yet:
// the dispatch is dropped and counted (d3d12.mesh_dispatches_dropped, and in
// each frame's work summary), the first ones logged with their pipeline.
void STDMETHODCALLTYPE list_dispatch_mesh(CommandList* self, UINT x, UINT y, UINT z) {
  static const lib::Counter dropped("d3d12.mesh_dispatches_dropped");
  dropped.add(1);
  g_work.mesh.fetch_add(1, std::memory_order_relaxed);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    const Pipeline* pipeline = as_pipeline(self->pipeline);
    LIB_LOG(lib::kError, "d3d12",
            "mesh dispatch %u x %u x %u with pipeline %p (content key %016llx, NGG group %u, record stride %u): not "
            "implemented, dropped",
            x, y, z, self->pipeline,
            static_cast<unsigned long long>(pipeline && pipeline->recipe ? pipeline->recipe->key : 0),
            pipeline ? pipeline->ngg.group : 0, pipeline ? pipeline->ngg.stride : 0);
  }
}

// Observed (call shapes): slot 85 CopyTextureRegionX and slot 86
// CopyResourceX take the desktop CopyTextureRegion and CopyResource
// arguments and a trailing flags word (0 seen); the copies are the same.
void STDMETHODCALLTYPE list_copy_texture_region_x(CommandList* self, const D3D12_TEXTURE_COPY_LOCATION* destination,
                                                  UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                                  const D3D12_BOX* box, UINT) {
  list_copy_texture_region(self, destination, x, y, z, source, box);
}
void STDMETHODCALLTYPE list_copy_resource_x(CommandList* self, void* destination, void* source, UINT) {
  list_copy_resource(self, destination, source);
}

const auto kListVtable = [] {
  auto table = lib::make_reporting_vtable<kCommandListSlots>();
  HOLY_D3D_SLOT(table, kCommandListObject, 0, list_query_interface);
  HOLY_D3D_SLOT(table, kCommandListObject, 1, list_add_ref);
  HOLY_D3D_SLOT(table, kCommandListObject, 2, list_release);
  HOLY_D3D_SLOT(table, kCommandListObject, 9, list_close);
  HOLY_D3D_SLOT(table, kCommandListObject, 12, list_draw_instanced);
  HOLY_D3D_SLOT(table, kCommandListObject, 13, list_draw_indexed_instanced);
  HOLY_D3D_SLOT(table, kCommandListObject, 14, list_dispatch);
  HOLY_D3D_SLOT(table, kCommandListObject, 15, list_copy_buffer_region);
  HOLY_D3D_SLOT(table, kCommandListObject, 16, list_copy_texture_region);
  HOLY_D3D_SLOT(table, kCommandListObject, 17, list_copy_resource);
  HOLY_D3D_SLOT(table, kCommandListObject, 20, list_set_primitive_topology);
  HOLY_D3D_SLOT(table, kCommandListObject, 21, list_set_viewports);
  HOLY_D3D_SLOT(table, kCommandListObject, 22, list_set_scissors);
  HOLY_D3D_SLOT(table, kCommandListObject, 23, list_set_blend_factor);
  HOLY_D3D_SLOT(table, kCommandListObject, 24, list_set_stencil_ref);
  HOLY_D3D_SLOT(table, kCommandListObject, 25, list_set_pipeline_state);
  HOLY_D3D_SLOT(table, kCommandListObject, 33, list_set_compute_root_signature);
  HOLY_D3D_SLOT(table, kCommandListObject, 34, list_set_graphics_root_signature);
  HOLY_D3D_SLOT(table, kCommandListObject, 35, list_set_compute_root_table);
  HOLY_D3D_SLOT(table, kCommandListObject, 36, list_set_graphics_root_table);
  HOLY_D3D_SLOT(table, kCommandListObject, 37, list_set_compute_root_constant);
  HOLY_D3D_SLOT(table, kCommandListObject, 38, list_set_graphics_root_constant);
  HOLY_D3D_SLOT(table, kCommandListObject, 39, list_set_compute_root_constants);
  HOLY_D3D_SLOT(table, kCommandListObject, 40, list_set_graphics_root_constants);
  HOLY_D3D_SLOT(table, kCommandListObject, 41, list_set_compute_root_cbv);
  HOLY_D3D_SLOT(table, kCommandListObject, 42, list_set_graphics_root_cbv);
  HOLY_D3D_SLOT(table, kCommandListObject, 43, list_set_compute_root_srv);
  HOLY_D3D_SLOT(table, kCommandListObject, 44, list_set_graphics_root_srv);
  HOLY_D3D_SLOT(table, kCommandListObject, 45, list_set_compute_root_uav);
  HOLY_D3D_SLOT(table, kCommandListObject, 46, list_set_graphics_root_uav);
  HOLY_D3D_SLOT(table, kCommandListObject, 47, list_set_index_buffer);
  HOLY_D3D_SLOT(table, kCommandListObject, 48, list_set_vertex_buffers);
  HOLY_D3D_SLOT(table, kCommandListObject, 50, list_set_render_targets);
  HOLY_D3D_SLOT(table, kCommandListObject, 51, list_clear_depth_stencil_view);
  HOLY_D3D_SLOT(table, kCommandListObject, 70, list_set_depth_bounds);
  HOLY_D3D_SLOT(table, kCommandListObject, 75, list_set_unknown_75);
  HOLY_D3D_SLOT(table, kCommandListObject, 142, list_set_sample_mask);
  HOLY_D3D_SLOT(table, kCommandListObject, 32, list_set_descriptor_heaps);
  HOLY_D3D_SLOT(table, kCommandListObject, 52, list_clear_render_target_view);
  HOLY_D3D_SLOT(table, kCommandListObject, 62, list_copy_memory_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 85, list_copy_texture_region_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 86, list_copy_resource_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 129, list_dispatch_mesh);
  HOLY_D3D_SLOT(table, kCommandListObject, 80, list_dispatch_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 101, list_driver_hint);
  HOLY_D3D_SLOT(table, kCommandListObject, 71, list_write_value_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 72, list_write_timestamp_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 74, list_write_fence_value_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 126, list_set_shading_rate);
  HOLY_D3D_SLOT(table, kCommandListObject, 139, list_reset_x);
  HOLY_D3D_SLOT(table, kCommandListObject, 146, list_barrier);
  HOLY_D3D_SLOT(table, kCommandListObject, 131, list_hardware_state);
  HOLY_D3D_SLOT(table, kCommandListObject, 132, list_hardware_state);
  return table;
}();

}  // namespace

std::string recorded_work_summary() {
  auto take = [](std::atomic<uint64_t>& counter) {
    return static_cast<unsigned long long>(counter.exchange(0, std::memory_order_relaxed));
  };
  char text[320];
  std::snprintf(text, sizeof(text),
                "%llu draws (%llu NGG), %llu indexed draws, %llu dispatches, %llu colour clears (%llu not black), "
                "%llu depth clears; dropped: %llu without a host pipeline, %llu unbound, %llu mesh dispatches",
                take(g_work.draws), take(g_work.ngg), take(g_work.indexed), take(g_work.dispatches),
                take(g_work.clears), take(g_work.nonblack_clears), take(g_work.depth_clears),
                take(g_work.no_pipeline), take(g_work.unbound), take(g_work.mesh));
  return text;
}

HRESULT create_command_list(Device* device, const uint32_t* desc, void* allocator, void* initial_state,
                            void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  auto* host_allocator = host_of<ID3D12CommandAllocator>(allocator);
  if (!host_allocator) return E_INVALIDARG;
  if (initial_state) {
    LIB_LOG(lib::kError, "d3d12", "command list with an initial pipeline state: not supported yet");
    return E_NOTIMPL;
  }
  const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]);
  HostList* host = new_host_list(device, type, host_allocator);
  LIB_LOG(lib::kDebug, "d3d12", "command list {%u, %u, %u, %u} -> %s", desc[0], desc[1], desc[2], desc[3],
          host ? "made" : "refused");
  if (!host) return E_OUTOFMEMORY;
  static std::atomic<uint32_t> lists{0};
  const uint32_t number = lists.fetch_add(1) + 1;
  auto* list = new CommandList();
  list->number = number;
  static const bool named = [] {
    lib::name_vtable(const_cast<void* const*>(kListVtable.data()), &kListInfo);  // its info is not after the vtable
    return true;
  }();
  (void)named;
  list->vtable = kListVtable.data();
  list->info = &kListInfo;
  list->device = device;
  list->type = type;
  use_host_list(list, host);
  *out = list;
  return S_OK;
}

void submit_command_lists(CommandQueue* queue, UINT count, void* const* lists) {
  ID3D12CommandList* stack[64];
  std::vector<ID3D12CommandList*> spill;
  ID3D12CommandList** host = stack;
  if (count > 64) {
    spill.resize(count);
    host = spill.data();
  }
  UINT used = 0;
  std::lock_guard<std::mutex> guard(queue->lock);
  for (UINT i = 0; i < count; ++i) {
    auto* list = static_cast<CommandList*>(lists[i]);
    if (!list || !list->current) continue;
    HostList* h = list->current;
    if (h->executed && in_flight(h)) {
      // The same recording submitted again before the GPU finished it: wait.
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 4) {
        LIB_LOG(lib::kWarn, "d3d12", "title list %u submitted again while executing; waiting for it", list->number);
      }
      const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (event && SUCCEEDED(h->fence->SetEventOnCompletion(h->value, event))) WaitForSingleObject(event, INFINITE);
      if (event) CloseHandle(event);
    }
    host[used++] = h->list;
  }
  if (!used) return;
  queue->host->ExecuteCommandLists(used, host);
  const uint64_t value = ++queue->submitted;
  queue->host->Signal(queue->fence, value);
  for (UINT i = 0; i < count; ++i) {
    auto* list = static_cast<CommandList*>(lists[i]);
    if (!list || !list->current) continue;
    list->current->fence = queue->fence;
    list->current->value = value;
    list->current->executed = true;
  }
}

}  // namespace holy::d3d12
