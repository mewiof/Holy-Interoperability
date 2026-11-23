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
#include "core/libs/d3d12_xs/views.h"

#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/support.h"

#include <intrin.h>

#include <cstddef>
#include <cstring>

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

}  // namespace

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
  if (resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || view->srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
    HolyUnimplementedFrom(kLibrary, "ID3D12DeviceX#70(texture view)", _ReturnAddress());
    return;
  }
  Backing backing;
  if (!locate(self, view->address, &backing) || !backing.whole) {
    LIB_LOG(lib::kError, "d3d12", "buffer view at %p lies in no GPU memory", reinterpret_cast<void*>(view->address));
    return;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC host = view->srv;
  const UINT stride = host.Buffer.StructureByteStride ? host.Buffer.StructureByteStride
                      : (host.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4
                                                                         : format_bytes(host.Format);
  host.Buffer.FirstElement += slack_elements(backing.heap_offset, stride);
  self->host->CreateShaderResourceView(backing.whole, &host, host_cpu_handle(dest));
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
  self->host->CreateConstantBufferView(desc ? &host : nullptr, host_cpu_handle(dest));
}

void STDMETHODCALLTYPE create_shader_resource_view(Device* self, void* resource,
                                                   const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, SIZE_T dest) {
  const auto* object = static_cast<const Resource*>(resource);
  D3D12_SHADER_RESOURCE_VIEW_DESC host = {};
  if (desc) {
    host = *desc;
    if (object && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
      const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride
                          : (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4
                                                                              : format_bytes(desc->Format);
      host.Buffer.FirstElement += slack_elements(slack_of(object), stride);
    }
  }
  self->host->CreateShaderResourceView(host_resource(resource), desc ? &host : nullptr, host_cpu_handle(dest));
}

void STDMETHODCALLTYPE create_unordered_access_view(Device* self, void* resource, void* counter,
                                                    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, SIZE_T dest) {
  const auto* object = static_cast<const Resource*>(resource);
  D3D12_UNORDERED_ACCESS_VIEW_DESC host = {};
  if (desc) {
    host = *desc;
    if (object && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
      const UINT stride = desc->Buffer.StructureByteStride ? desc->Buffer.StructureByteStride
                          : (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4
                                                                              : format_bytes(desc->Format);
      host.Buffer.FirstElement += slack_elements(slack_of(object), stride);
    }
  }
  self->host->CreateUnorderedAccessView(host_resource(resource), host_resource(counter), desc ? &host : nullptr,
                                        host_cpu_handle(dest));
}

void STDMETHODCALLTYPE create_render_target_view(Device* self, void* resource,
                                                 const D3D12_RENDER_TARGET_VIEW_DESC* desc, SIZE_T dest) {
  self->host->CreateRenderTargetView(host_resource(resource), desc, host_cpu_handle(dest));
}

void STDMETHODCALLTYPE create_depth_stencil_view(Device* self, void* resource,
                                                 const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, SIZE_T dest) {
  self->host->CreateDepthStencilView(host_resource(resource), desc, host_cpu_handle(dest));
}

void STDMETHODCALLTYPE create_sampler(Device* self, const D3D12_SAMPLER_DESC* desc, SIZE_T dest) {
  self->host->CreateSampler(desc, host_cpu_handle(dest));
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
}

void STDMETHODCALLTYPE copy_descriptors(Device* self, UINT dest_ranges, const SIZE_T* dest_starts,
                                        const UINT* dest_sizes, UINT source_ranges, const SIZE_T* source_starts,
                                        const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dests(dest_ranges), sources(source_ranges);
  for (UINT i = 0; i < dest_ranges; ++i) dests[i] = host_cpu_handle(dest_starts[i]);
  for (UINT i = 0; i < source_ranges; ++i) sources[i] = host_cpu_handle(source_starts[i]);
  self->host->CopyDescriptors(dest_ranges, dests.data(), dest_sizes, source_ranges, sources.data(), source_sizes,
                              type);
}

void STDMETHODCALLTYPE copy_descriptors_simple(Device* self, UINT count, SIZE_T dest, SIZE_T source,
                                               D3D12_DESCRIPTOR_HEAP_TYPE type) {
  self->host->CopyDescriptorsSimple(count, host_cpu_handle(dest), host_cpu_handle(source), type);
}

}  // namespace holy::d3d12
