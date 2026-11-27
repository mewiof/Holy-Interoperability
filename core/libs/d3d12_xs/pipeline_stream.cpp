// Pipeline state streams (device slot 86).
//
// Observed: device slot 86 takes a D3D12_PIPELINE_STATE_STREAM_DESC (stream
// size, stream pointer), the interface id and the out pointer, as the
// desktop's CreatePipelineState. The stream is a sequence of subobjects, each
// a type followed by its description, aligned to pointer size (the desktop
// layout). Shader subobjects carry console containers: those with a desktop
// program are rebuilt (shader_container.h). When the vertex and pixel shaders
// carry only native code, both are translated (shader/translate.h) and the
// pipeline uses the translated root signature; other stages with native code,
// and pipelines mixing native and desktop shaders, report themselves.
#include "core/libs/d3d12_xs/pipeline_stream.h"

#include <intrin.h>

#include <cstring>
#include <cwchar>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/pipeline_cache.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/d3d12_xs/shader_container.h"
#include "core/libs/d3d12_xs/shader_dump.h"
#include "core/libs/d3d12_xs/translation_cache.h"
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
  std::vector<ProgramRef> shaders;  // in subobject order
  std::string summary;
  // Native-only shaders found: subobject type, where its description sits.
  std::vector<std::pair<uint32_t, size_t>> native;
  bool desktop_shader = false;
  size_t root_at = 0;
  bool have_root = false;
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
      root_at = inner;
      have_root = true;
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
      D3D12_INPUT_LAYOUT_DESC layout_desc;
      std::memcpy(&layout_desc, copy.data() + inner, sizeof(layout_desc));
      for (UINT i = 0; i < layout_desc.NumElements && layout_desc.pInputElementDescs; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = layout_desc.pInputElementDescs[i];
        LIB_LOG(lib::kDebug, "d3d12", "input element %u: %s%u format %u slot %u offset %u class %u step %u", i,
                e.SemanticName ? e.SemanticName : "?", e.SemanticIndex, static_cast<unsigned>(e.Format), e.InputSlot,
                e.AlignedByteOffset, static_cast<unsigned>(e.InputSlotClass), e.InstanceDataStepRate);
      }
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
        LIB_LOG(lib::kDebug, "d3d12", "stream shader subobject %u at %p: %016llx (%llu bytes), %s", type,
                bytecode.pShaderBytecode,
                static_cast<unsigned long long>(shader_hash(bytecode.pShaderBytecode, bytecode.BytecodeLength)),
                static_cast<unsigned long long>(bytecode.BytecodeLength),
                format == ShaderFormat::NativeOnly ? "native only"
                : format == ShaderFormat::Unknown  ? "unknown"
                                                   : "desktop program");
        if (format == ShaderFormat::Unknown) {
          dump_shader(bytecode.pShaderBytecode, bytecode.BytecodeLength, "stream");
          const auto* b = static_cast<const uint8_t*>(bytecode.pShaderBytecode);
          LIB_LOG(lib::kDebug, "d3d12",
                  "unknown shader at %p (%llu bytes): %02x%02x%02x%02x %02x%02x%02x%02x; 16 bytes before: %02x%02x%02x%02x "
                  "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                  bytecode.pShaderBytecode, static_cast<unsigned long long>(bytecode.BytecodeLength), b[0], b[1], b[2],
                  b[3], b[4], b[5], b[6], b[7], b[-16], b[-15], b[-14], b[-13], b[-12], b[-11], b[-10], b[-9], b[-8],
                  b[-7], b[-6], b[-5], b[-4], b[-3], b[-2], b[-1]);
        }
        if (format == ShaderFormat::NativeOnly || format == ShaderFormat::Unknown) {
          native.emplace_back(type, inner);
          at = align_up(inner + layout.size, sizeof(void*));
          continue;
        }
        desktop_shader = true;
        const D3D12_SHADER_BYTECODE host{nullptr, rebuilt.size()};  // the program is patched in when compiling
        shaders.push_back(intern_program(std::move(rebuilt)));
        std::memcpy(copy.data() + inner, &host, sizeof(host));
      }
    }
    at = align_up(inner + layout.size, sizeof(void*));
  }
  bool translated = false;
  if (!native.empty()) {
    // Vertex and pixel shaders with native code only are translated.
    ProgramRef vs, ps;
    ShaderInterface vs_link, ps_link;
    bool have_vs = false, have_ps = false;
    for (const auto& [type, inner] : native) {
      D3D12_SHADER_BYTECODE bytecode;
      std::memcpy(&bytecode, copy.data() + inner, sizeof(bytecode));
      if (type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
        dump_shader(bytecode.pShaderBytecode, bytecode.BytecodeLength, "pipeline");
        LIB_LOG(lib::kError, "d3d12", "pipeline stream: native-only shader %016llx in subobject type %u",
                static_cast<unsigned long long>(shader_hash(bytecode.pShaderBytecode, bytecode.BytecodeLength)),
                static_cast<unsigned>(type));
        HolyUnimplementedFrom(kLibrary, "graphics pipeline with a native-only shader other than vertex and pixel",
                              caller);
        return E_NOTIMPL;
      }
      NativeProgram program;
      std::string error;
      const bool vertex = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
      // A shader whose code is not of its subobject's stage, or a shader
      // subobject that holds no shader container, is refused as the desktop
      // runtime refuses invalid shader bytecode. Observed: titles create such
      // pipelines and handle the failure.
      if (!native_program(bytecode.pShaderBytecode, bytecode.BytecodeLength, &program)) {
        LIB_LOG(lib::kError, "d3d12", "pipeline stream: subobject %u holds no shader (%016llx, %llu bytes)", type,
                static_cast<unsigned long long>(shader_hash(bytecode.pShaderBytecode, bytecode.BytecodeLength)),
                static_cast<unsigned long long>(bytecode.BytecodeLength));
        return E_INVALIDARG;
      }
      GraphicsStage by_code = GraphicsStage::Vertex;
      if (native_graphics_stage(program.block.data() + program.code_offset, program.block.size() - program.code_offset,
                                &by_code) &&
          (by_code == GraphicsStage::Vertex) != vertex) {
        LIB_LOG(lib::kError, "d3d12", "pipeline stream: the %s subobject holds a %s program (%016llx)",
                vertex ? "vertex" : "pixel", by_code == GraphicsStage::Vertex ? "vertex" : "pixel",
                static_cast<unsigned long long>(shader_hash(bytecode.pShaderBytecode, bytecode.BytecodeLength)));
        return E_INVALIDARG;
      }
      ProgramRef& out = vertex ? vs : ps;
      if (!native_program(bytecode.pShaderBytecode, bytecode.BytecodeLength, &program) ||
          !translate_graphics_cached(program.block.data() + program.code_offset, program.block.size() - program.code_offset,
                              vertex ? GraphicsStage::Vertex : GraphicsStage::Pixel, &out,
                              vertex ? &vs_link : &ps_link, &error)) {
        dump_shader(bytecode.pShaderBytecode, bytecode.BytecodeLength, "pipeline");
        LIB_LOG(lib::kError, "d3d12", "native %s shader %016llx not translatable: %s", vertex ? "vertex" : "pixel",
                static_cast<unsigned long long>(shader_hash(bytecode.pShaderBytecode, bytecode.BytecodeLength)),
                error.empty() ? "unreadable native program" : error.c_str());
        HolyUnimplementedFrom(kLibrary, ("native shader code: " + error).c_str(), caller);
        return E_NOTIMPL;
      }
      (vertex ? have_vs : have_ps) = true;
    }
    if (desktop_shader || !have_vs || !have_root) {
      HolyUnimplementedFrom(kLibrary, "graphics pipeline mixing native and desktop shaders", caller);
      return E_NOTIMPL;
    }
    if (have_ps && (ps_link.params & ~vs_link.params) != 0) {
      LIB_LOG(lib::kError, "d3d12", "pixel shader reads attributes 0x%x the vertex shader does not export (0x%x)",
              ps_link.params, vs_link.params);
      return E_INVALIDARG;
    }
    for (const auto& [type, inner] : native) {
      ProgramRef& out = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ? vs : ps;
      const D3D12_SHADER_BYTECODE host{nullptr, out->size()};  // patched in when compiling
      shaders.push_back(std::move(out));
      std::memcpy(copy.data() + inner, &host, sizeof(host));
    }
    ID3D12RootSignature* root = translated_root_signature(device);
    if (!root) return E_FAIL;
    std::memcpy(copy.data() + root_at, &root, sizeof(root));
    translated = true;
    LIB_LOG(lib::kDebug, "d3d12", "translated vertex shader exports 0x%x; pixel shader reads 0x%x, writes targets 0x%x",
            vs_link.params, ps_link.params, ps_link.targets);
  }
  const HRESULT hr = create_recorded_pipeline(device, std::move(copy), std::move(shaders), translated, info, vtable,
                                              pipeline);
  LIB_LOG(lib::kDebug, "d3d12", "pipeline stream of %llu bytes (subobjects %s) -> 0x%08lX",
          static_cast<unsigned long long>(desc->SizeInBytes), summary.c_str(), static_cast<unsigned long>(hr));
  if (FAILED(hr)) discard_host_caches_after_run(hr);
  return hr;
}

bool translate_graphics_cached(const uint8_t* code, size_t size, GraphicsStage stage, ProgramRef* program,
                               ShaderInterface* link, std::string* error, const ShaderInterface* consumer) {
  // Translations by native code hash, stage and consumer; the programs
  // themselves are held by the pipelines using them (pipeline_cache.h).
  struct Entry {
    std::weak_ptr<const Program> program;
    ShaderInterface link;
  };
  static std::mutex lock;
  static auto* cache = new std::map<std::tuple<uint64_t, int, uint64_t>, Entry>();
  const uint64_t reads = consumer ? (static_cast<uint64_t>(consumer->params) << 32 | consumer->half_params) : 0;
  const auto key = std::make_tuple(shader_hash(code, size), static_cast<int>(stage), reads);
  {
    std::lock_guard<std::mutex> guard(lock);
    auto found = cache->find(key);
    if (found != cache->end()) {
      if (ProgramRef held = found->second.program.lock()) {
        *program = std::move(held);
        *link = found->second.link;
        return true;
      }
    }
  }
  std::vector<uint8_t> dxbc;
  const uint64_t disk_key = graphics_translation_key(code, size, stage, consumer).value();
  uint32_t stored_size = 0;
  if (translation_header(disk_key, &stored_size, link)) {
    // Stored: the bytes are read when a pipeline using it is compiled.
    *program = stored_program(disk_key, stored_size);
    std::lock_guard<std::mutex> guard(lock);
    (*cache)[key] = Entry{*program, *link};
    return true;
  }
  if (!translate_graphics(code, size, stage, &dxbc, link, error, consumer)) return false;
  const bool on_disk = store_translation(disk_key, dxbc, link);
  if (HolyLogEnabled(lib::kDebug)) {
    dump_shader(code, size, stage == GraphicsStage::Vertex ? "vertex" : "pixel");
    LIB_LOG(lib::kDebug, "d3d12", "native %s program %016llx -> host %016llx",
            stage == GraphicsStage::Vertex ? "vertex" : "pixel", static_cast<unsigned long long>(std::get<0>(key)),
            static_cast<unsigned long long>(shader_hash(dxbc.data(), dxbc.size())));
  }
  *program = intern_program(std::move(dxbc), on_disk ? disk_key : 0);
  std::lock_guard<std::mutex> guard(lock);
  (*cache)[key] = Entry{*program, *link};
  return true;
}

}  // namespace holy::d3d12
