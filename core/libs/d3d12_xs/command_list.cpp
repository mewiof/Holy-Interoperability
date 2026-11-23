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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

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

HRESULT STDMETHODCALLTYPE list_close(CommandList* self) {
  const HRESULT hr = self->host->Close();
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
  const HRESULT hr = self->host->Reset(host_allocator, nullptr);
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
  self->host->ClearRenderTargetView(host, colour, rect_count, rects);
}

// GPU writes of values into title memory, carried out by the host's
// WriteBufferImmediate (32-bit values) at the translated address.
void write_immediate(CommandList* self, uint64_t address, const uint32_t* values, UINT count,
                     D3D12_WRITEBUFFERIMMEDIATE_MODE mode) {
  const uint64_t host = translate_gpu_va(self->device, address);
  if (!host || !self->host2 || count > 2) {
    LIB_LOG(lib::kError, "d3d12", "immediate write to %p: no GPU memory there", reinterpret_cast<void*>(address));
    return;
  }
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
  TimestampHeap* heap = timestamps(self->device);
  Backing backing;
  if (!heap->heap || self->type == D3D12_COMMAND_LIST_TYPE_COPY || !locate(self->device, address, &backing) ||
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
        b.pResource = b.pResource ? static_cast<Resource*>(static_cast<void*>(b.pResource))->host : nullptr;
        b.LayoutBefore = host_layout(b.LayoutBefore);
        b.LayoutAfter = host_layout(b.LayoutAfter);
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
}

// Observed: slot 20 is IASetPrimitiveTopology, at its desktop position.
void STDMETHODCALLTYPE list_set_primitive_topology(CommandList* self, D3D12_PRIMITIVE_TOPOLOGY topology) {
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

const auto kListVtable = [] {
  auto table = lib::make_reporting_vtable<kCommandListSlots>();
  table[0] = reinterpret_cast<void*>(&list_query_interface);
  table[1] = reinterpret_cast<void*>(&list_add_ref);
  table[2] = reinterpret_cast<void*>(&list_release);
  table[9] = reinterpret_cast<void*>(&list_close);
  table[20] = reinterpret_cast<void*>(&list_set_primitive_topology);
  table[32] = reinterpret_cast<void*>(&list_set_descriptor_heaps);
  table[52] = reinterpret_cast<void*>(&list_clear_render_target_view);
  table[71] = reinterpret_cast<void*>(&list_write_value_x);
  table[72] = reinterpret_cast<void*>(&list_write_timestamp_x);
  table[74] = reinterpret_cast<void*>(&list_write_fence_value_x);
  table[126] = reinterpret_cast<void*>(&list_set_shading_rate);
  table[139] = reinterpret_cast<void*>(&list_reset_x);
  table[146] = reinterpret_cast<void*>(&list_barrier);
  table[131] = reinterpret_cast<void*>(&list_hardware_state);
  table[132] = reinterpret_cast<void*>(&list_hardware_state);
  return table;
}();

}  // namespace

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
  ID3D12GraphicsCommandList* host = nullptr;
  const HRESULT hr = device->host->CreateCommandList(0, type, host_allocator, nullptr, IID_PPV_ARGS(&host));
  LIB_LOG(lib::kDebug, "d3d12", "command list {%u, %u, %u, %u} -> 0x%08lX", desc[0], desc[1], desc[2], desc[3],
          static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;
  auto* list = new CommandList();
  list->vtable = kListVtable.data();
  list->info = &kListInfo;
  list->device = device;
  list->host = host;
  list->type = type;
  host->QueryInterface(IID_PPV_ARGS(&list->host2));
  host->QueryInterface(IID_PPV_ARGS(&list->host7));
  *out = list;
  return S_OK;
}

}  // namespace holy::d3d12
