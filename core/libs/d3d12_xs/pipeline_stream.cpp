// Pipeline state streams (device slot 86).
//
// Observed: device slot 86 takes a D3D12_PIPELINE_STATE_STREAM_DESC (stream
// size, stream pointer), the interface id and the out pointer, as the
// desktop's CreatePipelineState. The stream is a sequence of subobjects, each
// a type followed by its description, aligned to pointer size (the desktop
// layout). Shader subobjects carry console containers: those with a desktop
// program are rebuilt (shader_container.h); graphics shaders with only native
// code cannot be translated yet and report themselves.
#include "core/libs/d3d12_xs/pipeline_stream.h"

#include <intrin.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/shader_container.h"
#include "core/libs/d3d12_xs/shader_dump.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

// Size and alignment of each desktop subobject's description.
struct SubobjectLayout {
  size_t size;
  size_t align;
};

bool layout_of(uint32_t type, SubobjectLayout* out) {
  switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: *out = {sizeof(void*), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: *out = {sizeof(D3D12_SHADER_BYTECODE), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: *out = {sizeof(D3D12_STREAM_OUTPUT_DESC), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: *out = {sizeof(D3D12_BLEND_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: *out = {sizeof(D3D12_RASTERIZER_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: *out = {sizeof(D3D12_DEPTH_STENCIL_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: *out = {sizeof(D3D12_INPUT_LAYOUT_DESC), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: *out = {sizeof(D3D12_RT_FORMAT_ARRAY), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: *out = {sizeof(DXGI_SAMPLE_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: *out = {sizeof(D3D12_CACHED_PIPELINE_STATE), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: *out = {sizeof(D3D12_DEPTH_STENCIL_DESC1), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: *out = {sizeof(D3D12_VIEW_INSTANCING_DESC), 8}; return true;
    default: return false;
  }
}

bool is_shader(uint32_t type) {
  return type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
}

size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

HRESULT create_pipeline_from_stream(Device* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                    const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                    const void* caller) {
  if (!desc || !pipeline || !desc->pPipelineStateSubobjectStream) return E_POINTER;
  *pipeline = nullptr;
  const auto* stream = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
  std::vector<uint8_t> copy(stream, stream + desc->SizeInBytes);
  std::vector<std::vector<uint8_t>> shaders;  // rebuilt containers kept alive until creation
  shaders.reserve(8);
  std::string summary;
  for (size_t at = 0; at + 4 <= copy.size();) {
    uint32_t type;
    std::memcpy(&type, copy.data() + at, 4);
    SubobjectLayout layout;
    if (!layout_of(type, &layout)) {
      LIB_LOG(lib::kError, "d3d12", "pipeline stream subobject type %u at offset %zu is not a desktop one", type, at);
      HolyUnimplementedFrom(kLibrary, "pipeline stream with a console subobject type", caller);
      return E_NOTIMPL;
    }
    const size_t inner = align_up(at + 4, layout.align);
    if (inner + layout.size > copy.size()) return E_INVALIDARG;
    summary += std::to_string(type) + " ";
    if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
      void* title = nullptr;
      std::memcpy(&title, copy.data() + inner, sizeof(title));
      void* host = title ? host_of<ID3D12RootSignature>(title) : nullptr;
      std::memcpy(copy.data() + inner, &host, sizeof(host));
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK) {
      const UINT zero = 0;
      std::memcpy(copy.data() + inner, &zero, sizeof(zero));
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) {
      const D3D12_CACHED_PIPELINE_STATE none{};
      std::memcpy(copy.data() + inner, &none, sizeof(none));
    } else if (is_shader(type)) {
      D3D12_SHADER_BYTECODE bytecode;
      std::memcpy(&bytecode, copy.data() + inner, sizeof(bytecode));
      if (bytecode.pShaderBytecode && bytecode.BytecodeLength) {
        std::vector<uint8_t> rebuilt;
        const ShaderFormat format = rebuild_desktop_container(bytecode.pShaderBytecode, bytecode.BytecodeLength, rebuilt);
        if (format == ShaderFormat::NativeOnly || format == ShaderFormat::Unknown) {
          dump_shader(bytecode.pShaderBytecode, bytecode.BytecodeLength, "pipeline");
          LIB_LOG(lib::kError, "d3d12", "pipeline stream shader (subobject type %u) has only native code", type);
          HolyUnimplementedFrom(kLibrary, "graphics pipeline with native-only shaders", caller);
          return E_NOTIMPL;
        }
        LIB_LOG(lib::kDebug, "d3d12", "pipeline stream shader type %u: %llu bytes, %s, %016llx", type,
                static_cast<unsigned long long>(bytecode.BytecodeLength),
                format == ShaderFormat::Rebuilt ? "rebuilt" : "desktop",
                static_cast<unsigned long long>(shader_hash(bytecode.pShaderBytecode, bytecode.BytecodeLength)));
        dump_shader(bytecode.pShaderBytecode, bytecode.BytecodeLength, "pipeline");
        shaders.push_back(std::move(rebuilt));
        const D3D12_SHADER_BYTECODE host{shaders.back().data(), shaders.back().size()};
        std::memcpy(copy.data() + inner, &host, sizeof(host));
      }
    }
    at = align_up(inner + layout.size, sizeof(void*));
  }
  ID3D12Device2* device2 = nullptr;
  if (FAILED(device->host->QueryInterface(IID_PPV_ARGS(&device2)))) return E_NOINTERFACE;
  const D3D12_PIPELINE_STATE_STREAM_DESC host_desc{copy.size(), copy.data()};
  ID3D12PipelineState* state = nullptr;
  const HRESULT hr = device2->CreatePipelineState(&host_desc, IID_PPV_ARGS(&state));
  device2->Release();
  LIB_LOG(lib::kDebug, "d3d12", "pipeline stream of %llu bytes (subobjects %s) -> 0x%08lX",
          static_cast<unsigned long long>(desc->SizeInBytes), summary.c_str(), static_cast<unsigned long>(hr));
  if (FAILED(hr)) {
    discard_host_caches_after_run();
    return hr;
  }
  *pipeline = wrap(state, info, vtable);
  return S_OK;
}

}  // namespace holy::d3d12
