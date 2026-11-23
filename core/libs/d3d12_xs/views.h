// Descriptor creation and copies (device slots 17-24, see views.cpp).
#pragma once

#include <vector>

#include "core/libs/d3d12_xs/device.h"

namespace holy::d3d12 {

void STDMETHODCALLTYPE create_constant_buffer_view(Device* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                   SIZE_T dest);
void STDMETHODCALLTYPE create_shader_resource_view(Device* self, void* resource,
                                                   const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, SIZE_T dest);
void STDMETHODCALLTYPE create_unordered_access_view(Device* self, void* resource, void* counter,
                                                    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, SIZE_T dest);
void STDMETHODCALLTYPE create_render_target_view(Device* self, void* resource,
                                                 const D3D12_RENDER_TARGET_VIEW_DESC* desc, SIZE_T dest);
void STDMETHODCALLTYPE create_depth_stencil_view(Device* self, void* resource,
                                                 const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, SIZE_T dest);
struct ViewAtAddress;
void STDMETHODCALLTYPE create_shader_resource_view_at(Device* self, const D3D12_RESOURCE_DESC* resource,
                                                      const ViewAtAddress* view, SIZE_T dest);
void STDMETHODCALLTYPE create_sampler(Device* self, const D3D12_SAMPLER_DESC* desc, SIZE_T dest);
void STDMETHODCALLTYPE create_sampler_x(Device* self, const uint32_t* desc, SIZE_T dest);
void STDMETHODCALLTYPE copy_descriptors(Device* self, UINT dest_ranges, const SIZE_T* dest_starts,
                                        const UINT* dest_sizes, UINT source_ranges, const SIZE_T* source_starts,
                                        const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type);
void STDMETHODCALLTYPE copy_descriptors_simple(Device* self, UINT count, SIZE_T dest, SIZE_T source,
                                               D3D12_DESCRIPTOR_HEAP_TYPE type);

}  // namespace holy::d3d12
