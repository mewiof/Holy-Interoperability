// Translated programs run on the host's Direct3D 12 device: results the
// shader text alone cannot show (exactness of quad exchanges).
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/shader/translate.h"
#include "tests/unit/test.h"

using namespace holy::d3d12;

namespace {

constexpr UINT kSize = 4;  // a 4 x 4 target: four quads

D3D12_HEAP_PROPERTIES heap_of(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  return properties;
}

template <typename T>
void release(T*& object) {
  if (object) object->Release();
  object = nullptr;
}

struct Gpu {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  ID3D12RootSignature* root = nullptr;
  ID3D12DescriptorHeap* views = nullptr;
  ID3D12DescriptorHeap* targets = nullptr;
  ID3D12Resource* target = nullptr;
  ID3D12Resource* readback = nullptr;
  ID3D12Resource* constants = nullptr;
  ID3D12Resource* memory = nullptr;  // "title memory": region 0, view u0
  UINT64 fence_value = 0;

  ~Gpu() {
    release(memory);
    release(constants);
    release(readback);
    release(target);
    release(targets);
    release(views);
    release(root);
    release(fence);
    release(list);
    release(allocator);
    release(queue);
    release(device);
  }

  bool open() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&device)))) {
      IDXGIAdapter* warp = nullptr;
      if (SUCCEEDED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&warp)))) {
        D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
        warp->Release();
      }
    }
    factory->Release();
    if (!device) return false;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                              reinterpret_cast<void**>(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                         __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)))) {
      return false;
    }
    list->Close();
    return make_root() && make_resources();
  }

  // The bindings every translated program declares: the system constants
  // (b0) and the region table (u0-u63) in the translated register space.
  bool make_root() {
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = kRegionTableSize;
    range.RegisterSpace = kTranslatedSpace;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_ROOT_PARAMETER1 parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.RegisterSpace = kTranslatedSpace;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 2;
    desc.Desc_1_1.pParameters = parameters;
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr))) return false;
    const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root));
    blob->Release();
    return SUCCEEDED(hr);
  }

  bool make_resources() {
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRegionTableSize,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&views)))) {
      return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC null_view{};
    null_view.Format = DXGI_FORMAT_R32_TYPELESS;
    null_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    null_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < kRegionTableSize; ++i) {
      D3D12_CPU_DESCRIPTOR_HANDLE at = views->GetCPUDescriptorHandleForHeapStart();
      at.ptr += static_cast<SIZE_T>(i) * increment;
      device->CreateUnorderedAccessView(nullptr, nullptr, &null_view, at);
    }
    {
      // Region 0: 256 bytes of title memory at GPU address 1 << 32.
      const D3D12_HEAP_PROPERTIES local = heap_of(D3D12_HEAP_TYPE_DEFAULT);
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = 256;
      buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      if (FAILED(device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(&memory)))) {
        return false;
      }
      D3D12_UNORDERED_ACCESS_VIEW_DESC raw = null_view;
      raw.Buffer.NumElements = 64;
      device->CreateUnorderedAccessView(memory, nullptr, &raw, views->GetCPUDescriptorHandleForHeapStart());
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&rtv_heap, __uuidof(ID3D12DescriptorHeap),
                                            reinterpret_cast<void**>(&targets)))) {
      return false;
    }
    const D3D12_HEAP_PROPERTIES local = heap_of(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = kSize;
    texture.DepthOrArraySize = texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R32_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &texture,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, __uuidof(ID3D12Resource),
                                               reinterpret_cast<void**>(&target)))) {
      return false;
    }
    device->CreateRenderTargetView(target, nullptr, targets->GetCPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256 * kSize;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES read = heap_of(D3D12_HEAP_TYPE_READBACK);
    const D3D12_HEAP_PROPERTIES upload = heap_of(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC constant_buffer = buffer;
    constant_buffer.Width = 4096;  // HolySystem (kSystemVectors vectors) and an upload area
    if (FAILED(device->CreateCommittedResource(&read, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, __uuidof(ID3D12Resource),
                                               reinterpret_cast<void**>(&readback))) ||
        FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &constant_buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                               reinterpret_cast<void**>(&constants)))) {
      return false;
    }
    return set_constants({}, false);
  }

  // HolySystem: the user SGPRs and, when `region`, region 0 over `memory`.
  bool set_constants(std::initializer_list<uint32_t> user, bool region) {
    uint32_t* mapped = nullptr;
    if (FAILED(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    std::memset(mapped, 0, 2048);
    if (region) {
      const uint32_t entry[4] = {0, 1, 256, 0};  // base lo, base hi, size, view
      std::memcpy(mapped + 4 * kSystemRegionVector, entry, sizeof(entry));
      mapped[4 * kSystemCountsVector] = 1;
    }
    uint32_t i = 0;
    for (uint32_t value : user) mapped[4 * kSystemUserVector + i++] = value;
    constants->Unmap(0, nullptr);
    return true;
  }

  // Fills title memory with `contents`, runs one group of `cs` and returns
  // title memory afterwards (64 words); empty when the host refuses.
  std::vector<uint32_t> dispatch(const std::vector<uint8_t>& cs, const std::vector<uint32_t>& contents) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {cs.data(), cs.size()};
    ID3D12PipelineState* pipeline = nullptr;
    if (FAILED(device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                  reinterpret_cast<void**>(&pipeline)))) {
      return {};
    }
    // Contents go through the upload area after the system constants.
    uint8_t* mapped = nullptr;
    if (FAILED(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return {};
    std::memset(mapped + 2048, 0, 256);
    std::memcpy(mapped + 2048, contents.data(), 4 * contents.size());
    constants->Unmap(0, nullptr);
    allocator->Reset();
    list->Reset(allocator, pipeline);
    list->CopyBufferRegion(memory, 0, constants, 2048, 256);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {memory, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1, &barrier);
    list->SetComputeRootSignature(root);
    list->SetDescriptorHeaps(1, &views);
    list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    barrier.Transition = {memory, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback, 0, memory, 0, 256);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++fence_value);
    fence->SetEventOnCompletion(fence_value, nullptr);
    pipeline->Release();
    std::vector<uint32_t> words(64);
    void* out = nullptr;
    if (FAILED(readback->Map(0, nullptr, &out))) return {};
    std::memcpy(words.data(), out, 256);
    readback->Unmap(0, nullptr);
    return words;
  }

  // Draws a triangle covering the target with the two programs and returns
  // the target's texels as bits (row-major); empty when the host refuses.
  std::vector<uint32_t> draw(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = 0xFFFFFFFFu;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    ID3D12PipelineState* pipeline = nullptr;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                   reinterpret_cast<void**>(&pipeline)))) {
      return {};
    }
    allocator->Reset();
    list->Reset(allocator, pipeline);
    list->SetGraphicsRootSignature(root);
    list->SetDescriptorHeaps(1, &views);
    list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = targets->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, kSize, kSize, 1, 256};
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = target;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++fence_value);
    fence->SetEventOnCompletion(fence_value, nullptr);
    pipeline->Release();
    std::vector<uint32_t> texels(kSize * kSize);
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) return {};
    for (UINT y = 0; y < kSize; ++y) std::memcpy(&texels[y * kSize], static_cast<uint8_t*>(mapped) + 256 * y, 4 * kSize);
    readback->Unmap(0, nullptr);
    return texels;
  }
};

// A desktop vertex shader for a triangle covering the target, with a
// per-pixel varying TEXCOORD0.x (in the register a translated pixel program
// reads attribute 0 from).
std::vector<uint8_t> covering_vertex_shader() {
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
  const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  const char source[] =
      "struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };"
      "O main(uint id : SV_VertexID) {"
      "  float2 uv = float2((id << 1) & 2, id & 2);"
      "  O o;"
      "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);"
      "  o.t = float4(uv.x * 8 + uv.y * 64 + 1, 0, 0, 0);"
      "  return o;"
      "}";
  ID3DBlob* blob = nullptr;
  std::vector<uint8_t> out;
  if (compile && SUCCEEDED(compile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "vs_5_1", 0, 0,
                                   &blob, nullptr))) {
    const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
    out.assign(bytes, bytes + blob->GetBufferSize());
    blob->Release();
  }
  return out;
}

// v_interp_p2_f32 v0, attr0.x; v_mul_f32 v1, 0x3f9e3779, v0 (every mantissa
// bit in use); `middle`; exp mrt0 `exported` done; s_endpgm.
std::vector<uint8_t> pixel_program(std::initializer_list<uint32_t> middle, uint32_t exported) {
  std::vector<uint32_t> code = {0xC8010000u, 0x100200FFu, 0x3F9E3779u};
  code.insert(code.end(), middle.begin(), middle.end());
  code.insert(code.end(), {0xF8000801u, exported, 0xBF810000u});
  std::vector<uint8_t> out;
  for (uint32_t v : code) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
  }
  return out;
}

}  // namespace

TEST(pixel_quad_permutes_are_exact_on_the_host) {
  Gpu gpu;
  const std::vector<uint8_t> vs = covering_vertex_shader();
  if (vs.empty() || !gpu.open()) {
    std::printf("      (no Direct3D 12 device or shader compiler: not run)\n");
    return;
  }
  std::vector<uint8_t> own, permuted, horizontal;
  ShaderInterface link;
  std::string error;
  EXPECT(translate_graphics(pixel_program({}, 1).data(), pixel_program({}, 1).size(), GraphicsStage::Pixel, &own,
                            &link, &error));
  // v_mov_b32_dpp v2, v1 quad_perm:[3,2,0,1] (diagonal and vertical reads).
  const std::vector<uint8_t> diagonal_code = pixel_program({0x7E0402FAu, 0xFF004B01u}, 2);
  EXPECT(translate_graphics(diagonal_code.data(), diagonal_code.size(), GraphicsStage::Pixel, &permuted, &link,
                            &error));
  // v_mov_b32_dpp v2, v1 quad_perm:[1,0,3,2] (horizontal reads).
  const std::vector<uint8_t> horizontal_code = pixel_program({0x7E0402FAu, 0xFF00B101u}, 2);
  EXPECT(translate_graphics(horizontal_code.data(), horizontal_code.size(), GraphicsStage::Pixel, &horizontal, &link,
                            &error));
  // v_add_f32_dpp v2, v1 quad_perm:[0,0,0,0], -|v1| (source 1 modifiers).
  std::vector<uint8_t> difference;
  const std::vector<uint8_t> difference_code = pixel_program({0x060402FAu, 0xFFC00001u}, 2);
  EXPECT(translate_graphics(difference_code.data(), difference_code.size(), GraphicsStage::Pixel, &difference, &link,
                            &error));
  const std::vector<uint32_t> a = gpu.draw(vs, own);
  const std::vector<uint32_t> b = gpu.draw(vs, permuted);
  const std::vector<uint32_t> c = gpu.draw(vs, horizontal);
  const std::vector<uint32_t> d = gpu.draw(vs, difference);
  EXPECT(a.size() == kSize * kSize && b.size() == a.size() && c.size() == a.size() && d.size() == a.size());
  if (a.size() != kSize * kSize || b.size() != a.size() || c.size() != a.size() || d.size() != a.size()) return;
  bool distinct = true;
  for (size_t i = 0; i < a.size(); ++i) {
    for (size_t j = 0; j < i; ++j) distinct = distinct && a[i] != a[j];
  }
  EXPECT(distinct);
  auto source = [&](const std::vector<uint32_t>& texels, UINT x, UINT y, uint32_t selects) {
    const uint32_t lane = (x & 1) | ((y & 1) << 1);
    const uint32_t from = (selects >> (2 * lane)) & 3;
    return texels[((y & ~1u) + (from >> 1)) * kSize + (x & ~1u) + (from & 1)];
  };
  auto as_float = [](uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  bool exact_diagonal = true, exact_horizontal = true, exact_difference = true;
  for (UINT y = 0; y < kSize; ++y) {
    for (UINT x = 0; x < kSize; ++x) {
      exact_diagonal = exact_diagonal && b[y * kSize + x] == source(a, x, y, 0x4B);
      exact_horizontal = exact_horizontal && c[y * kSize + x] == source(a, x, y, 0xB1);
      const float expected = as_float(source(a, x, y, 0x00)) + -as_float(a[y * kSize + x]);
      exact_difference = exact_difference && as_float(d[y * kSize + x]) == expected;
    }
  }
  EXPECT(exact_diagonal);
  EXPECT(exact_horizontal);
  EXPECT(exact_difference);
}

TEST(format_loads_take_format_and_selects_from_the_descriptor) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // buffer_load_format_xyzw v[4:7], off, s[0:3], 0;
  // buffer_store_dwordx4 v[4:7], off, s[0:3], 0 offset:64; s_endpgm.
  const std::vector<uint32_t> program = {0xE00C0000u, 0x80000400u, 0xE0780040u, 0x80000400u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  // Descriptor: title memory, 256 bytes, format 56 (8_8_8_8_UNORM) with
  // selects w, z, one, x.
  const uint32_t selects = 7u | (6u << 3) | (1u << 6) | (4u << 9);
  EXPECT(gpu.set_constants({0, 1, 256, selects | (56u << 12)}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0x40302010u});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  auto as_float = [](uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  auto close_to = [](float a, float b) { return a - b < 1e-6f && b - a < 1e-6f; };
  EXPECT(close_to(as_float(after[16]), 64.0f / 255.0f));
  EXPECT(close_to(as_float(after[17]), 48.0f / 255.0f));
  EXPECT(as_float(after[18]) == 1.0f);
  EXPECT(close_to(as_float(after[19]), 16.0f / 255.0f));
  // Format 0 (invalid) reads zero, the constant-one select too.
  EXPECT(gpu.set_constants({0, 1, 256, selects}, true));
  const std::vector<uint32_t> invalid = gpu.dispatch(cs, {0x40302010u});
  EXPECT(invalid.size() == 64);
  if (invalid.size() == 64) EXPECT(invalid[16] == 0 && invalid[17] == 0 && invalid[18] == 0 && invalid[19] == 0);
  // Format 29 (16_16_FLOAT) with selects x, y, zero, one: half floats.
  const uint32_t plain = 4u | (5u << 3) | (0u << 6) | (1u << 9);
  EXPECT(gpu.set_constants({0, 1, 256, plain | (29u << 12)}, true));
  const std::vector<uint32_t> halves = gpu.dispatch(cs, {0x3C00BC00u});
  EXPECT(halves.size() == 64);
  if (halves.size() == 64) {
    EXPECT(as_float(halves[16]) == -1.0f && as_float(halves[17]) == 1.0f);
    EXPECT(halves[18] == 0 && as_float(halves[19]) == 1.0f);
  }
}
