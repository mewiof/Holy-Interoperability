// Resources.
//
// Observed: device slot 133 is CreatePlacedResourceX(gpu_address, desc,
// initial_state, clear_value, riid, out) (the name public GDK sample code
// uses), with the desktop ID3D12Resource interface id. The address lies in
// memory the title allocated itself; the descriptor is the desktop
// D3D12_RESOURCE_DESC, whose flags may carry bits the desktop does not define
// (0x20000 observed on buffers), and the initial state has been 0xffffffff.
//
// Resources keep the desktop ID3D12Resource method order for the slots titles
// call. As on the console, a resource's GPU address is the title address it
// was placed at, and mapping it yields that same address: the memory is the
// title's own. Buffers are placed in the host heap over that memory (see
// gpu_memory.h), so CPU writes and GPU reads see the same bytes.
#include <d3d12.h>

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/texture_dump.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/support.h"

extern "C" __declspec(dllimport) void* XMemAlloc(SIZE_T size, uint64_t attributes);
extern "C" __declspec(dllimport) void XMemFree(void* address, uint64_t attributes);

namespace holy::d3d12 {
namespace {

const lib::VtableInfo kResourceInfo{kLibrary, "ID3D12ResourceX"};
constexpr D3D12_RESOURCE_FLAGS kDesktopFlags = static_cast<D3D12_RESOURCE_FLAGS>(0x1FF);
// Allocator id 0x80 (bits 16-23), 64 KiB pages (page size field 2, bits
// 14-15), 64 KiB alignment (bits 24-28).
constexpr uint64_t kResourceMemoryAttributes = (0x80ull << 16) | (2ull << 14) | (16ull << 24);

const lib::Counter& c_resources() {
  static const lib::Counter c("d3d12.resources");
  return c;
}
const lib::Counter& c_released_kept() {
  static const lib::Counter c("d3d12.resources_released_kept");
  return c;
}
const lib::Counter& c_used_after_release() {
  static const lib::Counter c("d3d12.resources_used_after_release");
  return c;
}

// --- released resources ---------------------------------------------------------------
//
// A resource whose last reference goes releases its host objects at once, but
// the object itself is kept for a while with a vtable that logs any further
// use (with the caller) and answers as the live object did: titles may hold
// a resource's pointer in their own records after releasing it, and freed
// object memory reused for something else turns such a stale call into a
// jump through garbage.
constexpr ULONGLONG kKeepReleasedMs = 10 * 60 * 1000;

std::mutex g_released_lock;
std::deque<std::pair<ULONGLONG, Resource*>>& released() {
  static auto* list = new std::deque<std::pair<ULONGLONG, Resource*>>();
  return *list;
}

void note_use_after_release(const Resource* self, int slot, const void* caller) {
  c_used_after_release().add(1);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 32) return;
  char where[64];
  HolyDescribeAddress(caller, where, sizeof(where));
  LIB_LOG(lib::kWarn, "d3d12",
          "released resource %p (%llux%u, format %u, address 0x%llx) used through slot %d from %s", static_cast<const void*>(self),
          static_cast<unsigned long long>(self->desc.Width), self->desc.Height, static_cast<unsigned>(self->desc.Format),
          static_cast<unsigned long long>(self->title_va), slot, where);
}

HRESULT STDMETHODCALLTYPE released_query_interface(Resource* self, REFIID, void** out) {
  note_use_after_release(self, 0, _ReturnAddress());
  if (out) *out = nullptr;
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE released_add_ref(Resource* self) {
  note_use_after_release(self, 1, _ReturnAddress());
  return 0;
}
ULONG STDMETHODCALLTYPE released_release(Resource* self) {
  note_use_after_release(self, 2, _ReturnAddress());
  return 0;
}
HRESULT STDMETHODCALLTYPE released_map(Resource* self, UINT, const D3D12_RANGE*, void** data) {
  note_use_after_release(self, 8, _ReturnAddress());
  if (data) *data = reinterpret_cast<void*>(self->title_va);
  return S_OK;
}
void STDMETHODCALLTYPE released_unmap(Resource* self, UINT, const D3D12_RANGE*) {
  note_use_after_release(self, 9, _ReturnAddress());
}
const lib::VtableInfo kReleasedResourceInfo{kLibrary, "ID3D12ResourceX (released)"};

D3D12_RESOURCE_DESC* STDMETHODCALLTYPE released_get_desc(void* first, void* second) {
  Resource* self;
  D3D12_RESOURCE_DESC* result;
  lib::struct_return_arguments(first, second, &kReleasedResourceInfo, &self, &result, &kResourceInfo);
  note_use_after_release(self, 10, _ReturnAddress());
  *result = self->desc;
  return result;
}
D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE released_get_gpu_virtual_address(Resource* self) {
  note_use_after_release(self, 11, _ReturnAddress());
  return self->title_va;
}

const auto kReleasedVtable = [] {
  auto table = lib::make_reporting_vtable<kResourceSlots>();
  table[0] = reinterpret_cast<void*>(&released_query_interface);
  table[1] = reinterpret_cast<void*>(&released_add_ref);
  table[2] = reinterpret_cast<void*>(&released_release);
  table[8] = reinterpret_cast<void*>(&released_map);
  table[9] = reinterpret_cast<void*>(&released_unmap);
  table[10] = reinterpret_cast<void*>(&released_get_desc);
  table[11] = reinterpret_cast<void*>(&released_get_gpu_virtual_address);
  return table;
}();

void keep_released(Resource* self) {
  self->vtable = kReleasedVtable.data();
  self->info = &kReleasedResourceInfo;
  std::vector<Resource*> expired;
  const ULONGLONG now = GetTickCount64();
  {
    std::lock_guard<std::mutex> guard(g_released_lock);
    auto& list = released();
    list.emplace_back(now, self);
    while (!list.empty() && now - list.front().first > kKeepReleasedMs) {
      expired.push_back(list.front().second);
      list.pop_front();
    }
  }
  c_released_kept().add(1 - static_cast<int64_t>(expired.size()));
  for (Resource* old : expired) delete old;
}

HRESULT STDMETHODCALLTYPE resource_query_interface(Resource* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE resource_add_ref(Resource* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE resource_release(Resource* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    if (self->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) unregister_texture(self->desc, self->title_va, self->host);
    if (self->host) self->host->Release();
    self->host = nullptr;
    if (self->twin) self->twin->Release();
    self->twin = nullptr;
    if (self->runtime_memory) XMemFree(self->runtime_memory, kResourceMemoryAttributes);
    self->runtime_memory = nullptr;
    c_resources().add(-1);
    unregister_live_resource(self);
    keep_released(self);
  }
  return static_cast<ULONG>(left);
}

HRESULT STDMETHODCALLTYPE resource_map(Resource* self, UINT, const D3D12_RANGE*, void** data) {
  if (data) *data = reinterpret_cast<void*>(self->title_va);
  return S_OK;
}
void STDMETHODCALLTYPE resource_unmap(Resource*, UINT, const D3D12_RANGE*) {}

// A structure return (lib::struct_return_arguments): observed, titles pass
// the resource first and the result pointer second.
D3D12_RESOURCE_DESC* STDMETHODCALLTYPE resource_get_desc(void* first, void* second) {
  Resource* self;
  D3D12_RESOURCE_DESC* result;
  lib::struct_return_arguments(first, second, &kResourceInfo, &self, &result, &kReleasedResourceInfo);
  *result = self->desc;
  return result;
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE resource_get_gpu_virtual_address(Resource* self) {
  return self->title_va;
}

const auto kResourceVtable = [] {
  auto table = lib::make_reporting_vtable<kResourceSlots>();
  table[0] = reinterpret_cast<void*>(&resource_query_interface);
  table[1] = reinterpret_cast<void*>(&resource_add_ref);
  table[2] = reinterpret_cast<void*>(&resource_release);
  table[8] = reinterpret_cast<void*>(&resource_map);
  table[9] = reinterpret_cast<void*>(&resource_unmap);
  table[10] = reinterpret_cast<void*>(&resource_get_desc);
  table[11] = reinterpret_cast<void*>(&resource_get_gpu_virtual_address);
  return table;
}();

}  // namespace

// Formats the console adds to DXGI (documented: their numbers are public in
// open-source texture tooling), with the nearest desktop format: depth 16 /
// stencil 8 (118) and its depth (119) and stencil (120) views become the
// 24/8 family; the 10:10:10:2 float formats (116 7e3, 117 6e4) 16-bit float
// RGBA; R4G4 (190) R8G8. Host textures never live in title memory, so only
// precision and size differ.
DXGI_FORMAT desktop_format(DXGI_FORMAT format) {
  switch (static_cast<uint32_t>(format)) {
    case 116: case 117: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 118: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 119: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case 120: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case 190: return DXGI_FORMAT_R8G8_UNORM;
    default: return format;
  }
}

D3D12_CLEAR_VALUE desktop_clear_value(const D3D12_CLEAR_VALUE& clear) {
  D3D12_CLEAR_VALUE host = clear;
  host.Format = desktop_format(clear.Format);
  return host;
}

// Whether the host GPU supports `format` for the usages `flags` ask for
// (render target, depth-stencil, typed unordered access), asked once per
// format and usage.
bool host_supports(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  constexpr D3D12_RESOURCE_FLAGS kUsages = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const auto usage = static_cast<uint32_t>(flags & kUsages);
  const auto index = static_cast<uint32_t>(format);
  if (!usage || index >= 256) return true;
  static std::atomic<int8_t> known[256][8] = {};  // 0 unknown, 1 supported, -1 not
  const uint32_t slot = (usage & 1) | ((usage >> 1) & 2) | ((usage >> 1) & 4);  // RT, DS, UAV bits
  if (const int8_t answer = known[index][slot].load(std::memory_order_relaxed)) return answer > 0;
  ID3D12Device* host = host_device();
  if (!host) return true;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  bool ok = SUCCEEDED(host->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
  if ((flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
    ok = false;
  }
  if ((flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && !(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
    ok = false;
  }
  if ((flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
      !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
    ok = false;
  }
  known[index][slot].store(ok ? 1 : -1, std::memory_order_relaxed);
  return ok;
}

// Observed: titles ask for packed 16-bit formats (B5G6R5, B5G5R5A1,
// B4G4R4A4) as render targets and with typed unordered access; hosts may
// support neither (asked of the host, host_supports). Such textures are made
// as 8-bit RGBA with matching views: host textures never live in title
// memory, so only precision and size differ.
DXGI_FORMAT substitute_format(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  format = desktop_format(format);
  switch (format) {
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
      return host_supports(format, static_cast<D3D12_RESOURCE_FLAGS>(flags & kDesktopFlags))
                 ? format
                 : DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
      return format;
  }
}

D3D12_RESOURCE_DESC desktop_resource_desc(const D3D12_RESOURCE_DESC& desc) {
  D3D12_RESOURCE_DESC host = desc;
  host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags & kDesktopFlags);
  if (static_cast<uint32_t>(desc.Layout) > D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
    host.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  }
  host.Format = substitute_format(desc.Format, host.Flags);
  // Observed: titles ask for depth textures with unordered access, which the
  // desktop does not allow; the host texture is made without it.
  if ((host.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && (host.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
    host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  }
  return host;
}

HRESULT create_host_committed(Device* device, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags,
                              const D3D12_RESOURCE_DESC& desc, UINT initial, const D3D12_CLEAR_VALUE* clear,
                              ID3D12Resource** out) {
  *out = nullptr;
  ID3D12Device10* device10 = nullptr;
  if (FAILED(device->host->QueryInterface(IID_PPV_ARGS(&device10)))) return E_NOINTERFACE;
  D3D12_RESOURCE_DESC1 desc1{};
  std::memcpy(&desc1, &desc, sizeof(desc));
  D3D12_BARRIER_LAYOUT layout = static_cast<D3D12_BARRIER_LAYOUT>(initial);
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
  } else if (initial > D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON) {
    layout = D3D12_BARRIER_LAYOUT_COMMON;
  } else {
    layout = layout_for_flags(layout, desc.Flags);
  }
  const bool target = (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
  D3D12_CLEAR_VALUE host_clear = clear ? desktop_clear_value(*clear) : D3D12_CLEAR_VALUE{};
  if (clear) host_clear.Format = substitute_format(clear->Format, desc.Flags);
  const HRESULT hr = device10->CreateCommittedResource3(&heap, flags, &desc1, layout, target && clear ? &host_clear : nullptr,
                                                        nullptr, 0, nullptr, IID_PPV_ARGS(out));
  device10->Release();
  return hr;
}

// Observed: titles create textures in, and move them to, layouts of usages
// the host texture does not have (unordered access of depth textures, which
// the desktop does not allow: desktop_resource_desc). Such layouts become
// the common layout, which allows every access the host texture has.
D3D12_BARRIER_LAYOUT layout_for_flags(D3D12_BARRIER_LAYOUT layout, D3D12_RESOURCE_FLAGS flags) {
  switch (layout) {
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
      return (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? layout : D3D12_BARRIER_LAYOUT_COMMON;
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
      return (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? layout : D3D12_BARRIER_LAYOUT_COMMON;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
      return (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? layout : D3D12_BARRIER_LAYOUT_COMMON;
    default:
      return layout;
  }
}

D3D12_BARRIER_LAYOUT host_texture_layout(const Resource* resource, D3D12_BARRIER_LAYOUT layout) {
  if (!resource || resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return layout;
  return layout_for_flags(layout, desktop_resource_desc(resource->desc).Flags);
}

void make_depth_twin(Device* device, Resource* resource) {
  const D3D12_RESOURCE_FLAGS flags = resource->desc.Flags;
  if (resource->twin || !resource->host || !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
      !(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
    return;
  }
  const D3D12_RESOURCE_DESC host = resource->host->GetDesc();
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = host.Dimension;
  desc.Width = host.Width;
  desc.Height = host.Height;
  desc.DepthOrArraySize = host.DepthOrArraySize;
  desc.MipLevels = host.MipLevels;
  desc.Format = host.Format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  // The same format family as the depth texture, so copies go both ways.
  switch (desc.Format) {
    case DXGI_FORMAT_D16_UNORM: desc.Format = DXGI_FORMAT_R16_TYPELESS; break;
    case DXGI_FORMAT_D32_FLOAT: desc.Format = DXGI_FORMAT_R32_TYPELESS; break;
    default: break;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  const HRESULT hr = create_host_committed(device, heap, D3D12_HEAP_FLAG_NONE, desc,
                                           D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, &resource->twin);
  LIB_LOG(FAILED(hr) ? lib::kError : lib::kDebug, "d3d12",
          "unordered-access twin of a depth texture %llux%ux%u format %u -> 0x%08lX",
          static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize,
          static_cast<unsigned>(desc.Format), static_cast<unsigned long>(hr));
}

DXGI_FORMAT host_view_format(const void* resource, DXGI_FORMAT format) {
  const auto* r = static_cast<const Resource*>(resource);
  if (!r || r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return desktop_format(format);
  return substitute_format(format, r->desc.Flags);
}

// Observed: device slot 127 is called with heap properties, heap flags
// (ALLOW_DISPLAY for the buffers titles present), a resource description, an
// initial state, a clear value, a null protected session, zero castable
// formats and a null format list, the interface id and the out pointer: the
// shape of the desktop CreateCommittedResource3.
//
// Observed: around creating its display buffers a title registers an
// allocation hook for allocator id 0x80 that serves allocations whose page
// size field (attribute bits 14-15) is 2 from an arena sized for exactly those
// buffers, and afterwards requires the arena to be used up to within 2 MiB.
// So the console runtime allocates a committed resource's memory with
// XMemAlloc, allocator id 0x80, 64 KiB pages. This runtime makes the same
// allocation (of the host's size for the resource) so the title's accounting
// holds; the resource itself is a host committed resource, because textures
// cannot be placed in a heap over plain title memory.
HRESULT create_committed_resource(Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
                                  const D3D12_RESOURCE_DESC* desc, UINT state, bool layout,
                                  const D3D12_CLEAR_VALUE* clear, void** out) {
  if (!out || !desc || !properties) return E_POINTER;
  *out = nullptr;
  D3D12_HEAP_PROPERTIES host_properties = *properties;
  host_properties.CreationNodeMask = 0;
  host_properties.VisibleNodeMask = 0;
  D3D12_RESOURCE_DESC host_desc = desktop_resource_desc(*desc);
  const D3D12_HEAP_FLAGS host_flags = static_cast<D3D12_HEAP_FLAGS>(flags & ~D3D12_HEAP_FLAG_ALLOW_DISPLAY);
  const bool render_target = (host_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
  ID3D12Resource* host = nullptr;
  D3D12_CLEAR_VALUE host_clear = clear ? desktop_clear_value(*clear) : D3D12_CLEAR_VALUE{};
  if (clear) host_clear.Format = substitute_format(clear->Format, host_desc.Flags);
  const HRESULT hr =
      layout ? create_host_committed(device, host_properties, host_flags, host_desc, state, clear, &host)
             : device->host->CreateCommittedResource(&host_properties, host_flags, &host_desc,
                                                     D3D12_RESOURCE_STATE_COMMON,
                                                     render_target && clear ? &host_clear : nullptr,
                                                     IID_PPV_ARGS(&host));
  LIB_LOG(lib::kDebug, "d3d12", "committed resource dim %d %llux%u fmt %d heap %d flags 0x%x state 0x%x -> 0x%08lX",
          desc->Dimension, static_cast<unsigned long long>(desc->Width), desc->Height, desc->Format,
          properties->Type, desc->Flags, state, static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;
  const D3D12_RESOURCE_ALLOCATION_INFO size = device->host->GetResourceAllocationInfo(0, 1, &host_desc);
  void* memory = XMemAlloc(static_cast<SIZE_T>(size.SizeInBytes), kResourceMemoryAttributes);
  auto* resource = new Resource();
  resource->vtable = kResourceVtable.data();
  c_resources().add(1);
  register_live_resource(resource);
  resource->info = &kResourceInfo;
  resource->host = host;
  resource->desc = *desc;
  resource->runtime_memory = memory;
  if (layout) resource->layout.store(static_cast<int>(state));
  if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) make_depth_twin(device, resource);
  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    resource->host_va = host->GetGPUVirtualAddress();
    resource->title_va = resource->host_va;
  } else if (memory) {
    // As on the console, the texture's GPU address is the memory taken for
    // it; views the title makes at that address find this host texture.
    resource->title_va = reinterpret_cast<uint64_t>(memory);
    register_texture(*desc, resource->title_va, host);
  }
  *out = resource;
  return S_OK;
}

void log_object_vtables() {
  char live[64], gone[64];
  HolyDescribeAddress(kResourceVtable.data(), live, sizeof(live));
  HolyDescribeAddress(kReleasedVtable.data(), gone, sizeof(gone));
  LIB_LOG(lib::kDebug, "d3d12", "vtables: resource %s, released resource %s", live, gone);
}

HRESULT create_placed_resource_x(Device* device, uint64_t address, const D3D12_RESOURCE_DESC* desc,
                                 D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void** out,
                                 const void* caller) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    (void)caller;
    // Observed: the initial state of a placed texture is a barrier layout
    // (titles pass 3 and 6, then name them as the layout before their first
    // barriers).
    ID3D12Resource* texture = placed_texture(device, *desc, address, static_cast<UINT>(state), clear);
    if (!texture) return E_OUTOFMEMORY;
    auto* resource = new Resource();
    resource->vtable = kResourceVtable.data();
    c_resources().add(1);
    register_live_resource(resource);
    resource->info = &kResourceInfo;
    resource->host = texture;
    resource->layout.store(static_cast<int>(state));
    resource->title_va = address;
    resource->desc = *desc;
    make_depth_twin(device, resource);
    *out = resource;
    return S_OK;
  }
  Backing backing;
  if (!locate(device, address, &backing, desc->Width) || backing.contiguous < desc->Width) {
    LIB_LOG(lib::kError, "d3d12", "no GPU heap backs a buffer of 0x%llx bytes at %p (heap %p, 0x%llx contiguous)",
            static_cast<unsigned long long>(desc->Width), reinterpret_cast<void*>(address),
            static_cast<void*>(backing.heap), static_cast<unsigned long long>(backing.contiguous));
    return E_INVALIDARG;
  }
  // Placed buffers need 64 KiB aligned heap offsets; a buffer that starts
  // inside a 64 KiB block is placed from the block's start and remembers the
  // difference.
  const uint64_t slack = backing.heap_offset & 0xFFFF;
  D3D12_RESOURCE_DESC host_desc = *desc;
  host_desc.Width = desc->Width + slack;
  host_desc.Alignment = 0;
  host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>((desc->Flags & kDesktopFlags) |
                                                      D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
  (void)state;
  (void)clear;
  ID3D12Resource* host = nullptr;
  const HRESULT hr = device->host->CreatePlacedResource(backing.heap, backing.heap_offset - slack, &host_desc,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&host));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "placed buffer of 0x%llx bytes at %p: 0x%08lX",
            static_cast<unsigned long long>(desc->Width), reinterpret_cast<void*>(address),
            static_cast<unsigned long>(hr));
    return hr;
  }
  auto* resource = new Resource();
  resource->vtable = kResourceVtable.data();
  c_resources().add(1);
  register_live_resource(resource);
  resource->info = &kResourceInfo;
  resource->host = host;
  resource->title_va = address;
  resource->host_va = host->GetGPUVirtualAddress() + slack;
  resource->desc = *desc;
  *out = resource;
  return S_OK;
}

}  // namespace holy::d3d12
