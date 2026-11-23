// Pipeline state streams (see pipeline_stream.cpp).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

#include "core/libs/d3d12_xs/device.h"

namespace holy::d3d12 {

HRESULT create_pipeline_from_stream(Device* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                    const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                    const void* caller);

// Appends a subobject in the desktop stream layout.
void stream_append(std::vector<uint8_t>* stream, uint32_t type, const void* data, size_t size);

// Creates a pipeline from a host stream and remembers how, so pipelines can be
// derived from it. `blobs` holds what the stream's shader subobjects point at.
HRESULT create_recorded_pipeline(Device* device, std::vector<uint8_t> stream,
                                 std::vector<std::vector<uint8_t>> blobs, bool translated,
                                 const lib::VtableInfo* info, void* const* vtable, void** pipeline);

// Device slot 55: `base` with some of its state replaced (pipeline_stream.cpp).
HRESULT create_derived_pipeline(Device* device, const void* base, uint32_t count, const uint8_t* entries,
                                const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                const void* caller);

}  // namespace holy::d3d12
