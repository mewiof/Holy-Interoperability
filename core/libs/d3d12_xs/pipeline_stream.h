// Pipeline state streams (see pipeline_stream.cpp).
#pragma once

#include <d3d12.h>

#include "core/libs/d3d12_xs/device.h"

namespace holy::d3d12 {

HRESULT create_pipeline_from_stream(Device* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                    const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                    const void* caller);

}  // namespace holy::d3d12
