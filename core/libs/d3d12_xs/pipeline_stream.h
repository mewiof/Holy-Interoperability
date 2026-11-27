// Pipeline state streams (see pipeline_stream.cpp).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/shader/translate.h"

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/pipeline_cache.h"

namespace holy::d3d12 {

HRESULT create_pipeline_from_stream(Device* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                    const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                    const void* caller);

// translate_graphics with results kept by code hash (and consumer): titles
// create many pipelines from the same programs.
bool translate_graphics_cached(const uint8_t* code, size_t size, GraphicsStage stage, ProgramRef* program,
                               ShaderInterface* link, std::string* error, const ShaderInterface* consumer = nullptr);

}  // namespace holy::d3d12
