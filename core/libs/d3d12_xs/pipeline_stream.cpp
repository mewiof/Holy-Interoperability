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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/shader/translate.h"
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
        shaders.push_back(std::move(rebuilt));
        const D3D12_SHADER_BYTECODE host{shaders.back().data(), shaders.back().size()};
        std::memcpy(copy.data() + inner, &host, sizeof(host));
      }
    }
    at = align_up(inner + layout.size, sizeof(void*));
  }
  bool translated = false;
  if (!native.empty()) {
    // Vertex and pixel shaders with native code only are translated.
    std::vector<uint8_t> vs, ps;
    ShaderInterface vs_link, ps_link;
    bool have_vs = false, have_ps = false;
    for (const auto& [type, inner] : native) {
      D3D12_SHADER_BYTECODE bytecode;
      std::memcpy(&bytecode, copy.data() + inner, sizeof(bytecode));
      if (type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
        dump_shader(bytecode.pShaderBytecode, bytecode.BytecodeLength, "pipeline");
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
      std::vector<uint8_t>& out = vertex ? vs : ps;
      if (!native_program(bytecode.pShaderBytecode, bytecode.BytecodeLength, &program) ||
          !translate_graphics(program.block.data() + program.code_offset, program.block.size() - program.code_offset,
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
      std::vector<uint8_t>& out = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ? vs : ps;
      shaders.push_back(std::move(out));
      const D3D12_SHADER_BYTECODE host{shaders.back().data(), shaders.back().size()};
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
  if (FAILED(hr)) discard_host_caches_after_run();
  return hr;
}

// --- recipes: how graphics pipelines were made, to derive others from them ---

namespace {

struct Recipe {
  std::vector<uint8_t> stream;  // the host stream (host root signature, host bytecode)
  // What the stream points at, shared by recipes derived from this one.
  std::shared_ptr<std::vector<std::vector<uint8_t>>> blobs;
  std::shared_ptr<std::vector<D3D12_INPUT_ELEMENT_DESC>> elements;
  std::shared_ptr<std::vector<std::string>> names;
  bool translated = false;
};

std::mutex g_recipe_lock;
std::map<const void*, std::shared_ptr<Recipe>>& recipes() {
  static auto* map = new std::map<const void*, std::shared_ptr<Recipe>>();
  return *map;
}

// Calls `visit(type, inner offset, size)` for each subobject; false if the
// stream holds a type that is not a desktop one.
template <typename Visit>
bool walk(const std::vector<uint8_t>& stream, Visit visit) {
  for (size_t at = 0; at + 4 <= stream.size();) {
    uint32_t type;
    std::memcpy(&type, stream.data() + at, 4);
    SubobjectLayout layout;
    if (!layout_of(type, &layout)) return false;
    const size_t inner = align_up(at + 4, layout.align);
    if (inner + layout.size > stream.size()) return false;
    visit(type, inner, layout.size);
    at = align_up(inner + layout.size, sizeof(void*));
  }
  return true;
}

// Takes copies of what the stream points at in title memory (the input
// layout); false if it points at title memory the recipe cannot copy
// (stream output, view instancing).
bool own_pointers(Recipe* recipe) {
  bool ok = true;
  recipe->elements = std::make_shared<std::vector<D3D12_INPUT_ELEMENT_DESC>>();
  recipe->names = std::make_shared<std::vector<std::string>>();
  walk(recipe->stream, [&](uint32_t type, size_t inner, size_t) {
    if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
      D3D12_INPUT_LAYOUT_DESC layout;
      std::memcpy(&layout, recipe->stream.data() + inner, sizeof(layout));
      if (!layout.NumElements || !layout.pInputElementDescs) return;
      recipe->names->reserve(layout.NumElements);
      for (UINT i = 0; i < layout.NumElements; ++i) {
        recipe->names->push_back(layout.pInputElementDescs[i].SemanticName ? layout.pInputElementDescs[i].SemanticName
                                                                           : "");
      }
      for (UINT i = 0; i < layout.NumElements; ++i) {
        D3D12_INPUT_ELEMENT_DESC e = layout.pInputElementDescs[i];
        e.SemanticName = (*recipe->names)[i].c_str();
        recipe->elements->push_back(e);
      }
      layout.pInputElementDescs = recipe->elements->data();
      std::memcpy(recipe->stream.data() + inner, &layout, sizeof(layout));
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) {
      D3D12_STREAM_OUTPUT_DESC so;
      std::memcpy(&so, recipe->stream.data() + inner, sizeof(so));
      if (so.NumEntries) ok = false;
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
      D3D12_VIEW_INSTANCING_DESC vi;
      std::memcpy(&vi, recipe->stream.data() + inner, sizeof(vi));
      if (vi.ViewInstanceCount) ok = false;
    }
  });
  return ok;
}

// --- host compilation in the background ---------------------------------------------
//
// The host object behind a pipeline is set by its compile job; uses of the
// pipeline go through pipeline_host, which waits for the job. Jobs run when
// the pipeline is created. A compile the host refuses is reported; the
// pipeline then has no host object and its draws are dropped and reported.

struct Compile {
  Device* device;
  std::shared_ptr<Recipe> recipe;
  Wrapper* wrapper;
  HANDLE done;
};

std::mutex g_compile_lock;
std::map<const Wrapper*, HANDLE>& compiles() {
  static auto* map = new std::map<const Wrapper*, HANDLE>();
  return *map;
}

void CALLBACK compile_pipeline(PTP_CALLBACK_INSTANCE, void* context) {
  auto* job = static_cast<Compile*>(context);
  ID3D12Device2* device2 = nullptr;
  ID3D12PipelineState* state = nullptr;
  HRESULT hr = job->device->host->QueryInterface(IID_PPV_ARGS(&device2));
  if (SUCCEEDED(hr)) {
    const D3D12_PIPELINE_STATE_STREAM_DESC host_desc{job->recipe->stream.size(), job->recipe->stream.data()};
    hr = device2->CreatePipelineState(&host_desc, IID_PPV_ARGS(&state));
    device2->Release();
  }
  if (FAILED(hr) || !state) {
    discard_host_caches_after_run();
    LIB_LOG(lib::kError, "d3d12", "the host refused pipeline %p (0x%08lX)", static_cast<void*>(job->wrapper),
            static_cast<unsigned long>(hr));
  }
  InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&job->wrapper->host), state);
  SetEvent(job->done);
  // The job's reference.
  reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(Wrapper*)>(job->wrapper->vtable[2])(job->wrapper);
  delete job;
}

HRESULT create_from_recipe(Device* device, std::shared_ptr<Recipe> recipe, const lib::VtableInfo* info,
                           void* const* vtable, void** pipeline) {
  Wrapper* wrapper = wrap(nullptr, info, vtable);
  if (recipe->translated) wrapper->flags = kTranslatedPipeline;
  wrapper->refs.fetch_add(1);  // held by the compile job
  const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  {
    std::lock_guard<std::mutex> guard(g_compile_lock);
    compiles()[wrapper] = done;
  }
  auto* job = new Compile{device, recipe, wrapper, done};
  {
    std::lock_guard<std::mutex> guard(g_recipe_lock);
    recipes()[wrapper] = std::move(recipe);
  }
  // Compiled here: in the background the compiles competed with the title's
  // own start-up work for the processor and made start-up slower.
  compile_pipeline(nullptr, job);
  *pipeline = wrapper;
  return S_OK;
}

}  // namespace

void stream_append(std::vector<uint8_t>* stream, uint32_t type, const void* data, size_t size) {
  SubobjectLayout layout{size, 4};
  layout_of(type, &layout);
  stream->resize(align_up(stream->size(), sizeof(void*)), 0);
  const size_t at = stream->size();
  const size_t inner = align_up(at + 4, layout.align);
  stream->resize(align_up(inner + layout.size, sizeof(void*)), 0);
  std::memcpy(stream->data() + at, &type, 4);
  std::memcpy(stream->data() + inner, data, size < layout.size ? size : layout.size);
}

HRESULT create_recorded_pipeline(Device* device, std::vector<uint8_t> stream,
                                 std::vector<std::vector<uint8_t>> blobs, bool translated,
                                 const lib::VtableInfo* info, void* const* vtable, void** pipeline) {
  auto recipe = std::make_shared<Recipe>();
  recipe->stream = std::move(stream);
  recipe->blobs = std::make_shared<std::vector<std::vector<uint8_t>>>(std::move(blobs));
  recipe->translated = translated;
  if (!own_pointers(recipe.get())) {
    // Not derivable; created all the same.
    ID3D12Device2* device2 = nullptr;
    if (FAILED(device->host->QueryInterface(IID_PPV_ARGS(&device2)))) return E_NOINTERFACE;
    const D3D12_PIPELINE_STATE_STREAM_DESC host_desc{recipe->stream.size(), recipe->stream.data()};
    ID3D12PipelineState* state = nullptr;
    const HRESULT hr = device2->CreatePipelineState(&host_desc, IID_PPV_ARGS(&state));
    device2->Release();
    if (FAILED(hr)) return hr;
    Wrapper* wrapper = wrap(state, info, vtable);
    if (translated) wrapper->flags = kTranslatedPipeline;
    *pipeline = wrapper;
    return S_OK;
  }
  return create_from_recipe(device, std::move(recipe), info, vtable, pipeline);
}

// Observed: device slot 55 takes (base pipeline, count, entries, interface
// id, out) and returns an HRESULT. Entries are 0x14C bytes: a type dword and
// a desktop description: 0 D3D12_DEPTH_STENCIL_DESC, 1 D3D12_RASTERIZER_DESC,
// 2 D3D12_BLEND_DESC (field for field from how titles fill them). Type 5 is
// written next to a depth-stencil entry with zero depth fields and the same
// stencil masks; its meaning is not established and it is not applied.
// The result is the base pipeline with those parts replaced.
HRESULT create_derived_pipeline(Device* device, const void* base, uint32_t count, const uint8_t* entries,
                                const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                const void* caller) {
  if (!pipeline) return E_POINTER;
  *pipeline = nullptr;
  std::shared_ptr<Recipe> from;
  {
    std::lock_guard<std::mutex> guard(g_recipe_lock);
    auto found = recipes().find(base);
    if (found != recipes().end()) from = found->second;
  }
  if (!from) {
    LIB_LOG(lib::kError, "d3d12", "derived pipeline from %p, whose making was not recorded", base);
    HolyUnimplementedFrom(kLibrary, "pipeline derived from an unrecorded pipeline", caller);
    return E_NOTIMPL;
  }
  auto recipe = std::make_shared<Recipe>(*from);
  constexpr size_t kEntryBytes = 0x14C;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = entries + i * kEntryBytes;
    uint32_t kind;
    std::memcpy(&kind, entry, 4);
    uint32_t types[2] = {0, 0};
    size_t size = 0;
    switch (kind) {
      case 0:
        types[0] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
        types[1] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
        size = sizeof(D3D12_DEPTH_STENCIL_DESC);
        break;
      case 1:
        types[0] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        size = sizeof(D3D12_RASTERIZER_DESC);
        break;
      case 2:
        types[0] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
        size = sizeof(D3D12_BLEND_DESC);
        break;
      default:
        LIB_LOG(lib::kDebug, "d3d12", "derived pipeline entry of type %u not applied", kind);
        continue;
    }
    bool replaced = false;
    walk(recipe->stream, [&](uint32_t type, size_t inner, size_t) {
      if (type == types[0] || (types[1] && type == types[1])) {
        std::memcpy(recipe->stream.data() + inner, entry + 4, size);
        replaced = true;
      }
    });
    if (!replaced) stream_append(&recipe->stream, types[0], entry + 4, size);
  }
  const HRESULT hr = create_from_recipe(device, recipe, info, vtable, pipeline);
  LIB_LOG(FAILED(hr) ? lib::kError : lib::kDebug, "d3d12", "derived pipeline from %p with %u entries -> 0x%08lX",
          base, count, static_cast<unsigned long>(hr));
  return hr;
}

ID3D12PipelineState* pipeline_host(const void* pipeline) {
  if (!pipeline) return nullptr;
  auto* wrapper = static_cast<const Wrapper*>(pipeline);
  void* host = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(const_cast<IUnknown**>(&wrapper->host)),
                                                 nullptr, nullptr);
  if (host) return static_cast<ID3D12PipelineState*>(host);
  HANDLE done = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_compile_lock);
    auto found = compiles().find(wrapper);
    if (found != compiles().end()) done = found->second;
  }
  if (!done) return nullptr;
  WaitForSingleObject(done, INFINITE);
  return static_cast<ID3D12PipelineState*>(
      InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(const_cast<IUnknown**>(&wrapper->host)),
                                        nullptr, nullptr));
}

}  // namespace holy::d3d12
