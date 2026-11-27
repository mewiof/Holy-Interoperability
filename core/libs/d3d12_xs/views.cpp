// Descriptor creation and copies.
//
// Observed: device slots 17-24 have the desktop meanings and signatures
// (CreateConstantBufferView, CreateShaderResourceView, CreateUnorderedAccessView,
// CreateRenderTargetView, CreateDepthStencilView, CreateSampler,
// CopyDescriptors, CopyDescriptorsSimple); slot 18 is called with a null
// resource and a descriptor to make null descriptors, as on the desktop.
//
// Translation: destination handles are title CPU handles (see
// descriptors.cpp); constant buffer locations are title GPU addresses (see
// gpu_memory.h); resources are this library's objects. Buffer views of a
// buffer placed with leading slack (resource.cpp) are shifted by the slack.
// Every shader-visible view and sampler is also written into the title's
// descriptor memory for translated native code (descriptor_memory.h);
// copies copy that memory too.
#include "core/libs/d3d12_xs/views.h"

#include "core/libs/d3d12_xs/call_check.h"

#include "core/libs/d3d12_xs/descriptor_memory.h"
#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/shader/descriptor_layout.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/page_cache.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/support.h"

#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace holy::d3d12 {
namespace {

ID3D12Resource* host_resource(const void* resource) {
  return resource ? static_cast<const Resource*>(resource)->host : nullptr;
}

uint64_t slack_of(const Resource* resource) {
  return resource ? resource->host_va - resource->host->GetGPUVirtualAddress() : 0;
}

UINT format_bytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return 16;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
      return 8;
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
      return 2;
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    default:
      return 4;
  }
}

// Element offset for `slack` bytes in a buffer view; 0 if it does not divide.
UINT64 slack_elements(uint64_t slack, UINT stride) {
  if (!slack) return 0;
  if (!stride || slack % stride) {
    LIB_LOG(lib::kError, "d3d12", "buffer view slack of %llu bytes is not a multiple of its %u-byte elements",
            static_cast<unsigned long long>(slack), stride);
    return 0;
  }
  return slack / stride;
}

// Fits a buffer view of `stride`-byte elements into its host buffer; false
// when none of it lies in the buffer. Observed: titles make views that reach
// past the end of their buffers (a 64 KiB buffer viewed as 29,515 four-byte
// elements), which the console allows (a view is an address and a size).
// Measured (the host's debug layer): the host removes the device for such a
// view ("invalid call": view creation cannot report the error). Translated
// programs read views through the title's descriptor memory, which keeps the
// title's range; desktop programs see the part inside the buffer.
bool fit_buffer_view(ID3D12Resource* buffer, UINT64* first, UINT* count, UINT stride) {
  if (!buffer || !stride) return true;
  const UINT64 elements = buffer->GetDesc().Width / stride;
  if (*first >= elements || !*count) return false;
  if (*count > elements - *first) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) {
      LIB_LOG(lib::kWarn, "d3d12", "buffer view of %u %u-byte elements from %llu reaches past its buffer of %llu: "
              "the host view stops at the buffer's end", *count, stride, static_cast<unsigned long long>(*first),
              static_cast<unsigned long long>(elements));
    }
    *count = static_cast<UINT>(elements - *first);
  }
  return true;
}

// A shader resource view of `buffer` fitted into it (fit_buffer_view), or a
// null view when nothing of it lies in the buffer.
void create_buffer_srv(ID3D12Device* host, ID3D12Resource* buffer, D3D12_SHADER_RESOURCE_VIEW_DESC desc, UINT stride,
                       D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  if (!fit_buffer_view(buffer, &desc.Buffer.FirstElement, &desc.Buffer.NumElements, stride)) {
    buffer = nullptr;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 1;
  }
  host->CreateShaderResourceView(buffer, &desc, dest);
}

void create_buffer_uav(ID3D12Device* host, ID3D12Resource* buffer, ID3D12Resource* counter,
                       D3D12_UNORDERED_ACCESS_VIEW_DESC desc, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
  // A host buffer without unordered access (upload and readback buffers
  // cannot have it) takes no such view: the host removes the device for one.
  const bool allowed = buffer && (buffer->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (buffer && !allowed) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) {
      LIB_LOG(lib::kWarn, "d3d12", "unordered access view of a buffer without unordered access: a null view");
    }
  }
  if (!allowed || !fit_buffer_view(buffer, &desc.Buffer.FirstElement, &desc.Buffer.NumElements, stride)) {
    buffer = nullptr;
    counter = nullptr;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 1;
    desc.Buffer.CounterOffsetInBytes = 0;
  }
  host->CreateUnorderedAccessView(buffer, counter, &desc, dest);
}

// Render-target and depth-stencil descriptors and what they view, kept only
// while calls are wrapped (call_check.h), for traces.
struct TargetView {
  uint64_t va = 0;
  uint64_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t view_format = 0;
};
std::mutex g_targets_lock;
std::unordered_map<SIZE_T, TargetView> g_targets;

// Depth-stencil views' formats (pipelines' depth variants need them; views
// are made rarely).
std::mutex g_depth_lock;
std::unordered_map<SIZE_T, DXGI_FORMAT> g_depth_formats;

void note_target(SIZE_T dest, const void* resource, DXGI_FORMAT view_format) {
  if (!call_check_enabled()) return;
  TargetView view;
  if (const auto* r = static_cast<const Resource*>(resource)) {
    view = {r->title_va, r->desc.Width, r->desc.Height, static_cast<uint32_t>(r->desc.Format),
            static_cast<uint32_t>(view_format)};
  }
  std::lock_guard<std::mutex> guard(g_targets_lock);
  g_targets[dest] = view;
}

}  // namespace

DXGI_FORMAT depth_view_format(SIZE_T handle) {
  std::lock_guard<std::mutex> guard(g_depth_lock);
  const auto it = g_depth_formats.find(handle);
  return it == g_depth_formats.end() ? DXGI_FORMAT_UNKNOWN : it->second;
}

std::string describe_target(SIZE_T handle) {
  TargetView view;
  {
    std::lock_guard<std::mutex> guard(g_targets_lock);
    const auto it = g_targets.find(handle);
    if (it == g_targets.end()) return "(unknown view)";
    view = it->second;
  }
  char text[128];
  std::snprintf(text, sizeof(text), "%llx: %llux%u format %u (view %u)", static_cast<unsigned long long>(view.va),
                static_cast<unsigned long long>(view.width), view.height, view.format, view.view_format);
  return text;
}

// Observed (device slot 70): a shader resource view made from a resource
// description and an address rather than a resource object:
//   (const D3D12_RESOURCE_DESC* resource, const ViewAtAddress* view, void* dest)
// where the view is a D3D12_SHADER_RESOURCE_VIEW_DESC followed by the
// resource's GPU address, and dest a descriptor in a title descriptor heap.
// Titles flush the destination's cache lines afterwards. Buffer views are made
// over the buffer spanning the memory's host heap.
struct ViewAtAddress {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv;
  uint64_t address;
};
static_assert(offsetof(ViewAtAddress, address) == 40);

void STDMETHODCALLTYPE create_shader_resource_view_at(Device* self, const D3D12_RESOURCE_DESC* resource,
                                                      const ViewAtAddress* view, SIZE_T dest) {
  if (!resource || !view) return;
  // A resource description has a known dimension and plausible sizes (titles
  // pass other blocks too: not established).
  const bool plausible = static_cast<uint32_t>(resource->Dimension) <= D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                         (resource->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
                          (resource->Width <= 65536 && resource->Height <= 65536 && resource->MipLevels <= 17 &&
                           resource->SampleDesc.Count >= 1 && resource->SampleDesc.Count <= 16 &&
                           static_cast<uint32_t>(resource->Format) < 256));
  if (!plausible) {
    const auto* words = reinterpret_cast<const uint32_t*>(resource);
    const auto* v = reinterpret_cast<const uint32_t*>(view);
    LIB_LOG(lib::kWarn, "d3d12",
            "slot 70 with a description that is not a resource description (a null view is made): %08x %08x %08x %08x %08x %08x %08x %08x | "
            "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x (caller %p)",
            words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], v[0], v[1], v[2], v[3], v[4],
            v[5], v[6], v[7], v[8], v[9], v[10], v[11], _ReturnAddress());
    // Observed on one title path whose description buffer is left unwritten:
    // the view becomes a null view of the requested kind.
    if (view->srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
      self->host->CreateShaderResourceView(nullptr, &view->srv, host_cpu_handle(dest));
      publish_descriptors(dest);
    }
    write_indexed_descriptor(dest, kKindShaderResource);
    return;
  }
  if (resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    // A texture in title memory: viewed through a host copy (texture_upload.h).
    ID3D12Resource* texture = texture_at(self, *resource, view->address);
    if (texture) self->host->CreateShaderResourceView(texture, &view->srv, host_cpu_handle(dest));
    if (texture) publish_descriptors(dest);
    write_indexed_descriptor(dest, kKindShaderResource);
    return;
  }
  if (view->srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
    HolyUnimplementedFrom(kLibrary, "ID3D12DeviceX#70(texture view of a buffer)", _ReturnAddress());
    return;
  }
  Backing backing;
  if (!locate(self, view->address, &backing, 64ull << 20) || !backing.whole) {
    LIB_LOG(lib::kError, "d3d12", "buffer view at %p lies in no GPU memory", reinterpret_cast<void*>(view->address));
    return;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC host = view->srv;
  const UINT stride = host.Buffer.StructureByteStride ? host.Buffer.StructureByteStride
                      : (host.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4
                                                                         : format_bytes(host.Format);
  host.Buffer.FirstElement += slack_elements(backing.heap_offset, stride);
  create_buffer_srv(self->host, backing.whole, host, stride, host_cpu_handle(dest));
  publish_descriptors(dest);
  const bool raw = (view->srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
  const uint32_t element = view->srv.Buffer.StructureByteStride;
  write_buffer_descriptor(dest, view->address + view->srv.Buffer.FirstElement * stride, raw ? 0 : element,
                          raw || !element ? static_cast<uint32_t>(view->srv.Buffer.NumElements * stride)
                                          : view->srv.Buffer.NumElements);
}

// Observed: titles create constant buffer views whose size is not a multiple
// of 256 bytes (128), which the desktop requires; the view is widened to the
// next multiple (shaders read no further than the title's size).
void STDMETHODCALLTYPE create_constant_buffer_view(Device* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                   SIZE_T dest) {
  D3D12_CONSTANT_BUFFER_VIEW_DESC host = {};
  if (desc) {
    host = *desc;
    host.SizeInBytes = (desc->SizeInBytes + 255u) & ~255u;
    host.BufferLocation = translate_gpu_va(self, desc->BufferLocation);
    if (desc->BufferLocation && !host.BufferLocation) {
      LIB_LOG(lib::kError, "d3d12", "constant buffer at %p lies in no GPU memory",
              reinterpret_cast<void*>(desc->BufferLocation));
    }
  }
  // Observed: titles also place constant buffer views at addresses that are
  // not 256-byte aligned, which the desktop does not allow. The host
  // descriptor becomes a null view; translated native code reads the buffer
  // through the title's descriptor memory, which keeps the exact address.
  const bool host_viewable = !desc || (host.BufferLocation & 255) == 0;
  if (!host_viewable) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
      LIB_LOG(lib::kWarn, "d3d12",
              "constant buffer view at %p (host %p) is not 256-byte aligned: the host view is a null view",
              reinterpret_cast<void*>(desc->BufferLocation), reinterpret_cast<void*>(host.BufferLocation));
    }
  }
  self->host->CreateConstantBufferView(host_viewable && desc ? &host : nullptr, host_cpu_handle(dest));
  publish_descriptors(dest);
  if (desc) write_buffer_descriptor(dest, desc->BufferLocation, 0, desc->SizeInBytes);
}

void STDMETHODCALLTYPE create_shader_resource_view(Device* self, void* resource,
                                                   const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, SIZE_T dest) {
  const auto* object = static_cast<const Resource*>(resource);
  D3D12_SHADER_RESOURCE_VIEW_DESC host = {};
  if (desc) {
    host = *desc;
    host.Format = host_view_format(object, desc->Format);
    if (object && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
      const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride
                          : (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4
                                                                              : format_bytes(desc->Format);
      host.Buffer.FirstElement += slack_elements(slack_of(object), stride);
      create_buffer_srv(self->host, host_resource(resource), host, stride, host_cpu_handle(dest));
      publish_descriptors(dest);
      const bool raw = (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0 || !desc->Buffer.StructureByteStride;
      write_buffer_descriptor(dest, object->title_va + desc->Buffer.FirstElement * stride,
                              raw ? 0 : desc->Buffer.StructureByteStride,
                              raw ? static_cast<uint32_t>(desc->Buffer.NumElements * stride) : desc->Buffer.NumElements);
      return;
    }
  }
  self->host->CreateShaderResourceView(host_resource(resource), desc ? &host : nullptr, host_cpu_handle(dest));
  publish_descriptors(dest);
  write_indexed_descriptor(dest, kKindShaderResource);
}

void STDMETHODCALLTYPE create_unordered_access_view(Device* self, void* resource, void* counter,
                                                    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, SIZE_T dest) {
  const auto* object = static_cast<const Resource*>(resource);
  if (object && (object->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
    // The host texture has no unordered access (desktop_resource_desc): the
    // view is of its twin (Resource::twin).
    if (!object->twin) {
      LIB_LOG(lib::kError, "d3d12", "unordered access view of a depth texture without a twin (format %u)",
              object->desc.Format);
      HolyUnimplementedFrom(kLibrary, "unordered access view of a depth texture", _ReturnAddress());
      return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC host = desc ? *desc : D3D12_UNORDERED_ACCESS_VIEW_DESC{};
    if (desc) {
      // The twin's typed format: unsigned integer views stay so, others are
      // the twin's normalised or float format (a 16-bit depth texture of the
      // console's 118 family has a 32-bit float twin: desktop_format).
      const DXGI_FORMAT twin = object->twin->GetDesc().Format;
      const bool integer = desc->Format == DXGI_FORMAT_R16_UINT || desc->Format == DXGI_FORMAT_R32_UINT;
      if (twin == DXGI_FORMAT_R16_TYPELESS) {
        host.Format = integer ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R16_UNORM;
      } else if (twin == DXGI_FORMAT_R32_TYPELESS) {
        host.Format = desc->Format == DXGI_FORMAT_R32_UINT ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
      }
      // The twin has one plane (the depth plane of a texture with stencil).
      if (desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) host.Texture2D.PlaneSlice = 0;
      if (desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) host.Texture2DArray.PlaneSlice = 0;
    }
    self->host->CreateUnorderedAccessView(object->twin, nullptr, desc ? &host : nullptr, host_cpu_handle(dest));
    publish_descriptors(dest);
    write_indexed_descriptor(dest, kKindUnorderedAccess);
    return;
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC host = {};
  if (desc) {
    host = *desc;
    host.Format = host_view_format(object, desc->Format);
    if (object && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
      const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride
                          : (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4
                                                                              : format_bytes(desc->Format);
      host.Buffer.FirstElement += slack_elements(slack_of(object), stride);
    }
  }
  if (object && desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
    const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride
                        : (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4
                                                                            : format_bytes(desc->Format);
    create_buffer_uav(self->host, host_resource(resource), host_resource(counter), host, stride, host_cpu_handle(dest));
  } else {
    self->host->CreateUnorderedAccessView(host_resource(resource), host_resource(counter), desc ? &host : nullptr,
                                          host_cpu_handle(dest));
  }
  publish_descriptors(dest);
  if (object && desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
    const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride
                        : (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4
                                                                            : format_bytes(desc->Format);
    const bool raw = (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0 || !desc->Buffer.StructureByteStride;
    write_buffer_descriptor(dest, object->title_va + desc->Buffer.FirstElement * stride,
                            raw ? 0 : desc->Buffer.StructureByteStride,
                            raw ? static_cast<uint32_t>(desc->Buffer.NumElements * stride) : desc->Buffer.NumElements);
    // Desktop programs write through the view: its memory's copies are not
    // read again (page_cache.h).
    page_cache_gpu_writes(object->title_va + desc->Buffer.FirstElement * stride,
                          static_cast<uint64_t>(desc->Buffer.NumElements) * stride);
  } else {
    write_indexed_descriptor(dest, kKindUnorderedAccess);
  }
}

void STDMETHODCALLTYPE create_render_target_view(Device* self, void* resource,
                                                 const D3D12_RENDER_TARGET_VIEW_DESC* desc, SIZE_T dest) {
  D3D12_RENDER_TARGET_VIEW_DESC host{};
  if (desc) {
    host = *desc;
    host.Format = host_view_format(resource, desc->Format);
  }
  self->host->CreateRenderTargetView(host_resource(resource), desc ? &host : nullptr, host_cpu_handle(dest));
  publish_descriptors(dest);
  note_target(dest, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
}

void STDMETHODCALLTYPE create_depth_stencil_view(Device* self, void* resource,
                                                 const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, SIZE_T dest) {
  D3D12_DEPTH_STENCIL_VIEW_DESC host{};
  if (desc) {
    host = *desc;
    host.Format = desktop_format(desc->Format);
  }
  self->host->CreateDepthStencilView(host_resource(resource), desc ? &host : nullptr, host_cpu_handle(dest));
  publish_descriptors(dest);
  {
    const auto* r = static_cast<const Resource*>(resource);
    const DXGI_FORMAT format = desc ? host.Format : r ? desktop_format(r->desc.Format) : DXGI_FORMAT_UNKNOWN;
    std::lock_guard<std::mutex> guard(g_depth_lock);
    g_depth_formats[dest] = format;
  }
  note_target(dest, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
}

void STDMETHODCALLTYPE create_sampler(Device* self, const D3D12_SAMPLER_DESC* desc, SIZE_T dest) {
  self->host->CreateSampler(desc, host_cpu_handle(dest));
  publish_descriptors(dest);
  write_indexed_descriptor(dest, kKindSampler);
}

// Observed: device slot 61 takes an 84-byte console sampler description and
// the destination descriptor address. Field meanings (assumed, from how
// titles fill it and the values seen): dwords 1-3 min, mag and mip filter
// (1 point, 2 linear), 6-8 address modes U/V/W (desktop values), 9-10 min and
// max LOD, 12 LOD bias, 13 comparison function (desktop values, 0 none), 16
// maximum anisotropy, 17-20 border colour. Dwords 0, 4, 5, 11, 14 and 15 are
// not understood and ignored.
void STDMETHODCALLTYPE create_sampler_x(Device* self, const uint32_t* desc, SIZE_T dest) {
  auto as_float = [&](int index) {
    float value;
    std::memcpy(&value, desc + index, 4);
    return value;
  };
  LIB_LOG(lib::kDebug, "d3d12",
          "sampler x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", desc[0],
          desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], desc[7], desc[8], desc[9], desc[10], desc[11], desc[12],
          desc[13], desc[14], desc[15], desc[16]);
  D3D12_SAMPLER_DESC host{};
  const bool min_linear = desc[1] == 2, mag_linear = desc[2] == 2, mip_linear = desc[3] == 2;
  const bool compare = desc[13] >= D3D12_COMPARISON_FUNC_NEVER && desc[13] <= D3D12_COMPARISON_FUNC_ALWAYS;
  const UINT anisotropy = desc[16] < 1 ? 1 : desc[16] > 16 ? 16 : desc[16];
  if (anisotropy > 1) {
    host.Filter = compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
  } else {
    const int filter = (min_linear ? 0x10 : 0) | (mag_linear ? 0x4 : 0) | (mip_linear ? 0x1 : 0);
    host.Filter = static_cast<D3D12_FILTER>(filter | (compare ? 0x80 : 0));
  }
  auto address = [](uint32_t mode) {
    return mode >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && mode <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE
               ? static_cast<D3D12_TEXTURE_ADDRESS_MODE>(mode)
               : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  };
  host.AddressU = address(desc[6]);
  host.AddressV = address(desc[7]);
  host.AddressW = address(desc[8]);
  host.MinLOD = as_float(9);
  host.MaxLOD = as_float(10);
  host.MipLODBias = as_float(12);
  host.ComparisonFunc = compare ? static_cast<D3D12_COMPARISON_FUNC>(desc[13]) : D3D12_COMPARISON_FUNC_NEVER;
  host.MaxAnisotropy = anisotropy;
  for (int i = 0; i < 4; ++i) host.BorderColor[i] = as_float(17 + i);
  self->host->CreateSampler(&host, host_cpu_handle(dest));
  publish_descriptors(dest);
  write_indexed_descriptor(dest, kKindSampler);
}

void STDMETHODCALLTYPE copy_descriptors(Device* self, UINT dest_ranges, const SIZE_T* dest_starts,
                                        const UINT* dest_sizes, UINT source_ranges, const SIZE_T* source_starts,
                                        const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dests(dest_ranges), sources(source_ranges);
  for (UINT i = 0; i < dest_ranges; ++i) dests[i] = host_cpu_handle(dest_starts[i]);
  for (UINT i = 0; i < source_ranges; ++i) sources[i] = host_cpu_handle(source_starts[i]);
  self->host->CopyDescriptors(dest_ranges, dests.data(), dest_sizes, source_ranges, sources.data(), source_sizes,
                              type);
  for (UINT i = 0; i < dest_ranges; ++i) publish_descriptors(dest_starts[i], dest_sizes ? dest_sizes[i] : 1);
  // Descriptor memory, one descriptor at a time across both range lists
  // (a null size array means ranges of one).
  UINT d = 0, s = 0, d_at = 0, s_at = 0;
  while (d < dest_ranges && s < source_ranges) {
    const UINT d_size = dest_sizes ? dest_sizes[d] : 1, s_size = source_sizes ? source_sizes[s] : 1;
    if (d_at >= d_size) {
      ++d;
      d_at = 0;
      continue;
    }
    if (s_at >= s_size) {
      ++s;
      s_at = 0;
      continue;
    }
    const uint64_t stride = title_descriptor_bytes(type);
    copy_descriptor_memory(dest_starts[d] + d_at * stride, source_starts[s] + s_at * stride, 1);
    ++d_at;
    ++s_at;
  }
}

void STDMETHODCALLTYPE copy_descriptors_simple(Device* self, UINT count, SIZE_T dest, SIZE_T source,
                                               D3D12_DESCRIPTOR_HEAP_TYPE type) {
  self->host->CopyDescriptorsSimple(count, host_cpu_handle(dest), host_cpu_handle(source), type);
  publish_descriptors(dest, count);
  copy_descriptor_memory(dest, source, count);
}

}  // namespace holy::d3d12
