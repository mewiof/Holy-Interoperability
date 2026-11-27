// Translated programs run on the host's Direct3D 12 device: results the
// shader text alone cannot show (exactness of quad exchanges).
#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/shader/geometry.h"
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
  ID3D12Resource* memory = nullptr;  // "title memory" at 2^32: memory slot 1
  ID3D12Resource* page_table = nullptr;  // maps the page at 2^32 to slot 1
  ID3D12Resource* cube = nullptr;    // make_cube
  ID3D12DescriptorHeap* samplers = nullptr;
  UINT64 fence_value = 0;

  ~Gpu() {
    release(samplers);
    release(cube);
    release(page_table);
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
  // (b0), the memory slot views (u0-u65535) and the page table (t0) in the
  // translated register space.
  bool make_root() {
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = kMemoryViews;
    range.RegisterSpace = kTranslatedSpace;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    // Cube images (make_cube) and samplers as translated programs index them.
    D3D12_DESCRIPTOR_RANGE1 images{};
    images.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    images.NumDescriptors = UINT_MAX;
    images.RegisterSpace = kTextureSpace + kImageCube;
    images.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    D3D12_DESCRIPTOR_RANGE1 sampler_range = images;
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.RegisterSpace = kTextureSpace;
    D3D12_ROOT_PARAMETER1 parameters[5] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.RegisterSpace = kTranslatedSpace;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {1, &images};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable = {1, &sampler_range};
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[4].Descriptor.RegisterSpace = kTranslatedSpace;
    parameters[4].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 5;
    desc.Desc_1_1.pParameters = parameters;
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr))) return false;
    const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root));
    blob->Release();
    return SUCCEEDED(hr);
  }

  bool make_resources() {
    // The memory slot views, then one image (make_cube).
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMemoryViews + 1,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&views)))) {
      return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC null_view{};
    null_view.Format = DXGI_FORMAT_R32_TYPELESS;
    null_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    null_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateUnorderedAccessView(nullptr, nullptr, &null_view, views->GetCPUDescriptorHandleForHeapStart());
    {
      // Slot 1: 256 bytes of title memory at GPU address 1 << 32.
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
      D3D12_CPU_DESCRIPTOR_HANDLE at = views->GetCPUDescriptorHandleForHeapStart();
      at.ptr += increment;
      device->CreateUnorderedAccessView(memory, nullptr, &raw, at);
    }
    {
      // The page table (shader/translate.h): window 4 (addresses from 2^32)
      // is level 2 table 1, whose first page is slot 1 at view offset 0; the
      // second page starts at view offset 0x40 of the same slot, the third
      // 0x40 bytes before the view (as the page holding a heap's start does).
      const D3D12_HEAP_PROPERTIES upload = heap_of(D3D12_HEAP_TYPE_UPLOAD);
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = kPageTableLevel2Base + kPageTableTableBytes;
      buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(&page_table)))) {
        return false;
      }
      uint32_t* table = nullptr;
      if (FAILED(page_table->Map(0, nullptr, reinterpret_cast<void**>(&table)))) return false;
      std::memset(table, 0, static_cast<size_t>(buffer.Width));
      table[4] = 1;
      table[kPageTableLevel2Base / 4] = 1;
      table[kPageTableLevel2Base / 4 + 1] = 0;
      table[kPageTableLevel2Base / 4 + 2] = 1;
      table[kPageTableLevel2Base / 4 + 3] = 0x40;
      table[kPageTableLevel2Base / 4 + 4] = 1;
      table[kPageTableLevel2Base / 4 + 5] = 0xFFFFFFC0u;
      page_table->Unmap(0, nullptr);
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

  // HolySystem: the user SGPRs. (`memory` is always mapped at 2^32 by the
  // page table; `region` is kept for the callers' readability.)
  bool set_constants(std::initializer_list<uint32_t> user, bool region) {
    (void)region;
    uint32_t* mapped = nullptr;
    if (FAILED(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
    std::memset(mapped, 0, 2048);
    uint32_t i = 0;
    for (uint32_t value : user) mapped[4 * kSystemUserVector + i++] = value;
    constants->Unmap(0, nullptr);
    return true;
  }

  // A 2 x 2 R32_FLOAT cube whose texel (u, v) of face f holds 4f + 2v + u,
  // as cube image 0, and a point sampler as sampler 0.
  bool make_cube() {
    const D3D12_HEAP_PROPERTIES local = heap_of(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES upload = heap_of(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = 2;
    texture.DepthOrArraySize = 6;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R32_FLOAT;
    texture.SampleDesc.Count = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 6 * 512;  // a face every 512 bytes, rows 256 bytes apart
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* staging = nullptr;
    if (FAILED(device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&cube))) ||
        FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                               reinterpret_cast<void**>(&staging)))) {
      return false;
    }
    uint8_t* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
      staging->Release();
      return false;
    }
    for (uint32_t f = 0; f < 6; ++f) {
      for (uint32_t v = 0; v < 2; ++v) {
        for (uint32_t u = 0; u < 2; ++u) {
          const float value = static_cast<float>(4 * f + 2 * v + u);
          std::memcpy(mapped + 512 * f + 256 * v + 4 * u, &value, 4);
        }
      }
    }
    staging->Unmap(0, nullptr);
    allocator->Reset();
    list->Reset(allocator, nullptr);
    for (UINT f = 0; f < 6; ++f) {
      D3D12_TEXTURE_COPY_LOCATION from{};
      from.pResource = staging;
      from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      from.PlacedFootprint.Offset = 512ull * f;
      from.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 2, 2, 1, 256};
      D3D12_TEXTURE_COPY_LOCATION to{};
      to.pResource = cube;
      to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      to.SubresourceIndex = f;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {cube, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++fence_value);
    fence->SetEventOnCompletion(fence_value, nullptr);
    staging->Release();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.TextureCube.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE at = views->GetCPUDescriptorHandleForHeapStart();
    at.ptr += static_cast<SIZE_T>(kMemoryViews) *
              device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(cube, &view, at);
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap),
                                            reinterpret_cast<void**>(&samplers)))) {
      return false;
    }
    D3D12_SAMPLER_DESC point{};
    point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    point.AddressU = point.AddressV = point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&point, samplers->GetCPUDescriptorHandleForHeapStart());
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
    ID3D12DescriptorHeap* heaps[] = {views, samplers};
    list->SetDescriptorHeaps(samplers ? 2 : 1, heaps);
    list->SetComputeRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootShaderResourceView(4, page_table->GetGPUVirtualAddress());
    if (samplers) {
      D3D12_GPU_DESCRIPTOR_HANDLE images = views->GetGPUDescriptorHandleForHeapStart();
      images.ptr += static_cast<UINT64>(kMemoryViews) *
                    device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      list->SetComputeRootDescriptorTable(2, images);
      list->SetComputeRootDescriptorTable(3, samplers->GetGPUDescriptorHandleForHeapStart());
    }
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
  std::vector<uint32_t> draw(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps,
                             const std::vector<uint8_t>* gs = nullptr, UINT vertices = 3) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    if (gs) desc.GS = {gs->data(), gs->size()};
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
    list->SetGraphicsRootShaderResourceView(4, page_table->GetGPUVirtualAddress());
    const D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    const D3D12_RECT scissor{0, 0, kSize, kSize};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = targets->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(vertices, 1, 0, 0);
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

// A desktop program compiled from HLSL (empty without the host compiler).
std::vector<uint8_t> compiled(const char* source, const char* profile) {
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
  const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  ID3DBlob* blob = nullptr;
  std::vector<uint8_t> out;
  if (compile && SUCCEEDED(compile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0,
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

TEST(page_table_entries_place_pages_at_their_view_offsets) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // buffer_load_dword v0, off, s[0:3], 0; v1 with s[4:7]; v2 with s[8:11];
  // buffer_store_dwordx3 v[0:2], off, s[12:15], 0 offset:128; s_endpgm.
  const std::vector<uint32_t> program = {0xE0300000u, 0x80000000u, 0xE0300000u, 0x80010100u, 0xE0300000u,
                                         0x80020200u, 0xE07C0080u, 0x80030000u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 16;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  // Buffers at the second page (view offset 0x40), 0x80 into the third page
  // (0x40 - 0x40 + 0x80 = view offset 0x40), the third page's start (before
  // the view: reads zero), and the first page.
  EXPECT(gpu.set_constants({0x10000, 1, 256, 0, 0x20080, 1, 256, 0, 0x20000, 1, 256, 0, 0, 1, 256, 0}, true));
  std::vector<uint32_t> contents(64);
  for (uint32_t i = 0; i < 64; ++i) contents[i] = 0x1000 + i;
  const std::vector<uint32_t> after = gpu.dispatch(cs, contents);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[32] == 0x1010);  // word 16, through the second page
  EXPECT(after[33] == 0x1010);  // word 16, through the third page
  EXPECT(after[34] == 0);       // before the view
}

// Scalar memory addresses (documented): the two low bits of each part (base,
// offset, instruction offset) are ignored, so scalar loads read whole,
// aligned dwords.
TEST(scalar_loads_ignore_the_low_address_bits) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // s_load_dword s4, s[0:1], 0x4; s_buffer_load_dword s5, s[12:15], s7;
  // v_mov_b32 v0, s4; v_mov_b32 v1, s5; buffer_store_dwordx2 v[0:1], off,
  // s[8:11], 0 offset:128; s_endpgm.
  const std::vector<uint32_t> program = {0xF4000100u, 0xFA000004u, 0xF4200146u, 0x0E000000u, 0x7E000204u,
                                         0x7E020205u, 0xE0740080u, 0x80020000u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 16;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  // The address 2^32 + 2 (plus 4: word 1) and a buffer at 2^32 read at
  // offset 10 (word 2); unaligned reads would combine two words.
  EXPECT(gpu.set_constants({2, 1, 0, 0, 0, 0, 0, 10, 0, 1, 256, 0, 0, 1, 256, 0}, true));
  std::vector<uint32_t> contents(64);
  for (uint32_t i = 0; i < 64; ++i) contents[i] = 0x1000 + i;
  const std::vector<uint32_t> after = gpu.dispatch(cs, contents);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[32] == 0x1001);
  EXPECT(after[33] == 0x1002);
}

// Buffer accesses past the descriptor's records (documented): loads read
// zero, stores are dropped.
TEST(buffer_accesses_past_the_records_read_zero_and_write_nothing) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // buffer_load_dword v0, off, s[0:3], 0 offset:32 (past 16 bytes);
  // buffer_load_dword v1, off, s[0:3], 0 offset:4; s_waitcnt vmcnt(0);
  // buffer_store_dword v1, off, s[0:3], 0 offset:20 (past);
  // buffer_store_dwordx2 v[0:1], off, s[4:7], 0 offset:192; s_endpgm.
  const std::vector<uint32_t> program = {0xE0300020u, 0x80000000u, 0xE0300004u, 0x80000100u, 0xBF8C3F70u,
                                         0xE0700014u, 0x80000100u, 0xE07400C0u, 0x80010000u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 8;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 1, 16, 0, 0, 1, 256, 0}, true));
  std::vector<uint32_t> contents(64);
  for (uint32_t i = 0; i < 64; ++i) contents[i] = 0x1000 + i;
  const std::vector<uint32_t> after = gpu.dispatch(cs, contents);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[48] == 0);
  EXPECT(after[49] == 0x1001);
  EXPECT(after[5] == 0x1005);
}

// EXEC (the lane model): after s_and_saveexec an inactive lane keeps what a
// vector write would have changed; a region s_cbranch_execz guards is
// skipped by it (and runs unmasked in active lanes); writes after EXEC is
// restored reach both lanes.
TEST(inactive_lanes_keep_registers_and_skip_execz_regions) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v_cmp_eq_u32 vcc, 0, v0; s_and_saveexec_b32 s0, vcc_lo; v1 = 7;
  // s_cbranch_execz skip; v2 = 9; skip: s_or_b32 exec_lo, exec_lo, s0;
  // v3 = 5; v4 = v0 * 16; buffer_store_dwordx3 v[1:3], v4, s[4:7], 0 offen;
  // s_endpgm.
  const std::vector<uint32_t> program = {0x7D840080u, 0xBE803C6Au, 0x7E020287u, 0xBF880001u, 0x7E040289u,
                                         0x887E007Eu, 0x7E060285u, 0x34080084u, 0xE07C1000u, 0x80010104u,
                                         0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 2;
  info.user_sgprs = 8;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 0, 0, 0, 0, 1, 256, 0}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64, 0));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[0] == 7 && after[1] == 9 && after[2] == 5);  // lane 0, active
  EXPECT(after[4] == 0 && after[5] == 0 && after[6] == 5);  // lane 1, inactive until the restore
}

// Saved masks: EXEC saved while on is on again when restored; a mask saved
// while a lane is inactive (an inner if without an execz branch) keeps it
// inactive when restored.
TEST(restored_exec_masks_reactivate_only_lanes_that_were_active) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v_cmp_eq_u32 vcc, 0, v0; s_and_saveexec_b32 s0, vcc_lo;
  // s_and_saveexec_b32 s1, vcc_lo; v1 = 7; s_or_b32 exec_lo, exec_lo, s1;
  // v2 = 9; s_or_b32 exec_lo, exec_lo, s0; v3 = 5; v4 = v0 * 16;
  // buffer_store_dwordx3 v[1:3], v4, s[4:7], 0 offen; s_endpgm.
  const std::vector<uint32_t> program = {0x7D840080u, 0xBE803C6Au, 0xBE813C6Au, 0x7E020287u, 0x887E017Eu,
                                         0x7E040289u, 0x887E007Eu, 0x7E060285u, 0x34080084u, 0xE07C1000u,
                                         0x80010104u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 2;
  info.user_sgprs = 8;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 0, 0, 0, 0, 1, 256, 0}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64, 0));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[0] == 7 && after[1] == 9 && after[2] == 5);  // lane 0
  EXPECT(after[4] == 0 && after[5] == 0 && after[6] == 5);  // lane 1: inactive until the outer restore
}

// Control flow (the linear layout): nested loops closed by backward
// branches, a branch forward out of both, uniform scalar counters.
TEST(nested_loops_and_a_branch_out_of_both_run_as_written) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // s4 = 0; s6 = 3;
  // outer: s5 = 4;
  //   inner: s4 += 1; s5 -= 1; if (s4 == 10) goto exit; if (s5 != 0) goto inner;
  //   s6 -= 1; if (s6 != 0) goto outer;
  // exit: v0 = s4; v1 = s6; buffer_store_dwordx2 v[0:1], off, s[0:3], 0 offset:128; s_endpgm.
  const std::vector<uint32_t> program = {0xBE840380u, 0xBE860383u, 0xBE850384u, 0x80048104u, 0x80858105u,
                                         0xBF068A04u, 0xBF850005u, 0xBF078005u, 0xBF85FFFAu, 0x80868106u,
                                         0xBF078006u, 0xBF85FFF6u, 0x7E000204u, 0x7E020206u, 0xE0740080u,
                                         0x80000000u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 4;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64, 0));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  // Two whole outer rounds (8), then the exit at 10 in the third, s6 at 1.
  EXPECT(after[32] == 10);
  EXPECT(after[33] == 1);
}

// Loops whose ranges overlap (one region with a loop inside): a branch back
// from B to A and one from C to B.
TEST(overlapping_loops_run_as_written) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // s4 = s5 = s6 = 0;
  // A: s5 += 1; B: s4 += s5; if (s5 < 3) goto A;
  // C: s6 += 1; if (s6 < 2) goto B;
  // v0..v2 = s4, s5, s6; buffer_store_dwordx3 v[0:2], off, s[0:3], 0 offset:128; s_endpgm.
  const std::vector<uint32_t> program = {0xBE840380u, 0xBE850380u, 0xBE860380u, 0x80058105u, 0x80040504u,
                                         0xBF0A8305u, 0xBF85FFFCu, 0x80068106u, 0xBF0A8206u, 0xBF85FFFAu,
                                         0x7E000204u, 0x7E020205u, 0x7E040206u, 0xE07C0080u, 0x80000000u,
                                         0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 4;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64, 0));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  // A-B three times (sum 1 + 2 + 3), C, B again (sum 9), C.
  EXPECT(after[32] == 9);
  EXPECT(after[33] == 3);
  EXPECT(after[34] == 2);
}

// Ray intersection (image_bvh64_intersect_ray): a box node of floats whose
// children are hit nearest first, a missed child and an invalid one; a
// triangle hit (the distance as numerator and denominator, barycentric
// numerators of the vertices the triangle id selects); the node's second
// triangle (slots 1, 3, 2) missed.
TEST(ray_intersection_sorts_box_hits_and_tests_triangles) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  auto bits = [](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  };
  // v0, v1 = the node pointer (base 0: address / 8, the type in bits 2-0);
  // v2 = extent 100; v3-v5 = origin 0; v6-v8 = direction (0, 0, 1);
  // v9-v11 = inverse direction (max, max, 1); image_bvh64_intersect_ray
  // v[12:15], v[0:11], s[0:3]; buffer_store_dwordx4 v[12:15], off, s[4:7],
  // 0 offset:192; again for the triangles (offsets 208, 224); s_endpgm.
  const uint32_t intersect[2] = {0xF19C9F01u, 0x00000C00u};
  std::vector<uint32_t> program = {0x7E0002FFu, 0x20000005u, 0x7E020280u, 0x7E0402FFu, bits(100.0f),
                                   0x7E060280u, 0x7E080280u, 0x7E0A0280u, 0x7E0C0280u, 0x7E0E0280u,
                                   0x7E1002F2u, 0x7E1202FFu, 0x7F7FFFFFu, 0x7E1402FFu, 0x7F7FFFFFu,
                                   0x7E1602F2u, intersect[0], intersect[1], 0xE07800C0u, 0x80010C00u,
                                   0x7E0002FFu, 0x20000010u, intersect[0], intersect[1], 0xE07800D0u,
                                   0x80010C00u, 0x7E0002FFu, 0x20000011u, intersect[0], intersect[1],
                                   0xE07800E0u, 0x80010C00u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 8;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  // The BVH descriptor: base 0, box sorting, barycentric triangle results;
  // then title memory at 2^32 for the results.
  EXPECT(gpu.set_constants({0, 0x80000000u, 0xFFFFFFFFu, 0x890003FFu, 0, 1, 256, 0}, true));
  std::vector<uint32_t> memory(64, 0);
  // The box node at 2^32: children 0x111 (z 5-6), 0x222 (z 2-3), 0x333 (to
  // the side), an invalid one (in the ray's way).
  const uint32_t children[4] = {0x111, 0x222, 0x333, 0xFFFFFFFFu};
  const float bounds[4][6] = {{-1, -1, 5, 1, 1, 6}, {-1, -1, 2, 1, 1, 3}, {5, 5, 0, 6, 6, 1}, {-1, -1, 1, 1, 1, 2}};
  for (int i = 0; i < 4; ++i) {
    memory[i] = children[i];
    for (int c = 0; c < 6; ++c) memory[4 + 6 * i + c] = bits(bounds[i][c]);
  }
  // The triangle node at 2^32 + 128: slots (-1,-1,10), (1,-1,10), (0,1,10),
  // (5,5,10); triangles 0 and 1 return vertex 1's and vertex 2's weights.
  const float slots[4][3] = {{-1, -1, 10}, {1, -1, 10}, {0, 1, 10}, {5, 5, 10}};
  for (int s = 0; s < 4; ++s) {
    for (int c = 0; c < 3; ++c) memory[32 + 3 * s + c] = bits(slots[s][c]);
  }
  memory[47] = 0x0909u;
  const std::vector<uint32_t> after = gpu.dispatch(cs, memory);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[48] == 0x222 && after[49] == 0x111 && after[50] == 0xFFFFFFFFu && after[51] == 0xFFFFFFFFu);
  // (0, 0) is a quarter of vertex 0, a quarter of vertex 1, half of vertex 2:
  // numerators -1, -1, -2 over -4; the distance -40 / -4.
  EXPECT(after[52] == bits(-40.0f) && after[53] == bits(-4.0f));
  EXPECT(after[54] == bits(-1.0f) && after[55] == bits(-2.0f));
  EXPECT(after[56] == 0x7F800000u && after[57] == bits(1.0f) && after[58] == 0 && after[59] == 0);
  std::printf("      (box %x %x %x %x; triangle %08x %08x %08x %08x; miss %08x %08x %08x %08x)\n", after[48],
              after[49], after[50], after[51], after[52], after[53], after[54], after[55], after[56], after[57],
              after[58], after[59]);
}

// A box node of half floats, directions as halves (A16), sorting off:
// children in the node's order, misses as 0xFFFFFFFF.
TEST(ray_intersection_reads_half_boxes_and_half_directions) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0, v1 = the node pointer (type 4); v2 = extent 100; v3-v5 = origin 0;
  // v6 = dir.x | dir.y (0, 0); v7 = dir.z | inv.x (1, 65504); v8 = inv.y |
  // inv.z (65504, 1); image_bvh64_intersect_ray v[12:15], v[0:8], s[0:3]
  // a16; buffer_store_dwordx4 v[12:15], off, s[4:7], 0 offset:192; s_endpgm.
  const std::vector<uint32_t> program = {0x7E0002FFu, 0x20000004u, 0x7E020280u, 0x7E0402FFu, 0x42C80000u,
                                         0x7E060280u, 0x7E080280u, 0x7E0A0280u, 0x7E0C0280u, 0x7E0E02FFu,
                                         0x7BFF3C00u, 0x7E1002FFu, 0x3C007BFFu, 0xF19C9F01u, 0x40000C00u,
                                         0xE07800C0u, 0x80010C00u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 8;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 0, 0xFFFFFFFFu, 0x890003FFu, 0, 1, 256, 0}, true));
  // Children 0xA (z 5-6), 0xB (to the side), 0xC (z 2-3), 0xD (to the
  // side); bounds as halves: min x | min y, min z | max x, max y | max z.
  const std::vector<uint32_t> node = {0xA,         0xB,         0xC,         0xD,         0xBC00BC00u, 0x3C004500u,
                                      0x46003C00u, 0x45004500u, 0x46000000u, 0x3C004600u, 0xBC00BC00u, 0x3C004000u,
                                      0x42003C00u, 0x45004500u, 0x46000000u, 0x3C004600u};
  std::vector<uint32_t> memory(64, 0);
  std::copy(node.begin(), node.end(), memory.begin());
  const std::vector<uint32_t> after = gpu.dispatch(cs, memory);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[48] == 0xA && after[49] == 0xFFFFFFFFu && after[50] == 0xC && after[51] == 0xFFFFFFFFu);
}

// Differential: random programs (fixed seed) of scalar and vector arithmetic,
// scalar ifs, counted loops, loops entered in the middle, divergent ifs with
// and without an execz branch and early exits; every loop runs a few times
// at most, so every program ends. Each runs on 8 lanes translated as the
// reference (TranslatorOptions::reference: the dispatcher, nothing known of
// EXEC, no folded moves) and as the runtime translates it; the results must
// be the same.
namespace {

struct Assembler {
  std::vector<uint32_t> words;
  std::vector<std::pair<size_t, int>> fixups;
  std::vector<size_t> labels;
  int label() {
    labels.push_back(SIZE_MAX);
    return static_cast<int>(labels.size() - 1);
  }
  void bind(int l) { labels[static_cast<size_t>(l)] = words.size(); }
  void branch(uint32_t op, int l) {
    fixups.emplace_back(words.size(), l);
    words.push_back(0xBF800000u | (op << 16));
  }
  void finish() {
    for (const auto& [at, l] : fixups) {
      const int64_t delta = static_cast<int64_t>(labels[static_cast<size_t>(l)]) - static_cast<int64_t>(at) - 1;
      words[at] |= static_cast<uint32_t>(delta) & 0xFFFF;
    }
  }
};

struct ProgramGenerator {
  uint32_t state = 0;
  Assembler a;
  int out = 0;
  int statements = 0;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  uint32_t s() { return 8 + next() % 8; }  // data SGPRs s8-s15
  uint32_t v() { return 1 + next() % 3; }  // data VGPRs v1-v3
  void literal(uint32_t sgpr, uint32_t value) {
    a.words.push_back(0xBE8003FFu | (sgpr << 16));
    a.words.push_back(value);
  }
  void block(int depth) {
    const int count = 1 + static_cast<int>(next() % 4);
    for (int k = 0; k < count && statements < 48; ++k, ++statements) {
      const uint32_t choice = next() % (depth < 3 ? 11 : 5);
      if (choice <= 1) {  // s_add_u32 / s_xor_b32 sD, sA, sB
        const uint32_t op = choice == 0 ? 0 : 18;
        a.words.push_back(0x80000000u | (op << 23) | (s() << 16) | (s() << 8) | s());
      } else if (choice <= 3) {  // v_add_nc_u32 / v_xor_b32 vD, sA, vB
        const uint32_t op = choice == 2 ? 37 : 29;
        a.words.push_back((op << 25) | (v() << 17) | (v() << 9) | s());
      } else if (choice == 4) {  // v_mov_b32 vD, sA
        a.words.push_back(0x7E000200u | (v() << 17) | s());
      } else if (choice == 5) {  // if (sA < sB) {...} else {...}
        const int otherwise = a.label(), end = a.label();
        a.words.push_back(0xBF0A0000u | (s() << 8) | s());  // s_cmp_lt_u32
        a.branch(4, otherwise);                              // s_cbranch_scc0
        block(depth + 1);
        a.branch(2, end);
        a.bind(otherwise);
        block(depth + 1);
        a.bind(end);
      } else if (choice == 6 || choice == 7) {  // a counted loop; 7: entered in the middle
        const uint32_t counter = 20 + static_cast<uint32_t>(depth);
        literal(counter, 1 + next() % 3);
        const int top = a.label(), body = a.label();
        if (choice == 7) a.branch(2, body);
        a.bind(top);
        if (choice == 7) a.words.push_back(0x80800000u | (counter << 16) | (129u << 8) | counter);  // s_sub_u32 1
        a.bind(body);
        block(depth + 1);
        if (choice == 6) a.words.push_back(0x80800000u | (counter << 16) | (129u << 8) | counter);
        a.words.push_back(0xBF070000u | (128u << 8) | counter);  // s_cmp_lg_u32 counter, 0
        a.branch(5, top);                                        // s_cbranch_scc1
      } else if (choice == 8 || choice == 9) {  // a divergent if; 8 with an execz branch
        const uint32_t saved = 16 + static_cast<uint32_t>(depth);
        const int end = a.label();
        a.words.push_back(0x7C000000u | (0xC4u << 17) | (0u << 9) | 24u);  // v_cmp_gt_u32 vcc, s24, v0
        a.words.push_back(0xBE803C6Au | (saved << 16));                    // s_and_saveexec_b32 saved, vcc_lo
        if (choice == 8) a.branch(8, end);                                // s_cbranch_execz
        block(depth + 1);
        a.bind(end);
        a.words.push_back(0x887E007Eu | (saved << 8));  // s_or_b32 exec_lo, exec_lo, saved
      } else {  // if (sA == sB) leave for the stores
        a.words.push_back(0xBF060000u | (s() << 8) | s());  // s_cmp_eq_u32
        a.branch(5, out);
      }
    }
  }
  std::vector<uint8_t> generate() {
    out = a.label();
    for (uint32_t r = 8; r < 16; ++r) literal(r, next() % 16);  // small values: comparisons go both ways
    literal(24, next() % 9);                                     // the divergent ifs' lane threshold
    for (uint32_t r = 1; r <= 3; ++r) a.words.push_back((37u << 25) | (r << 17) | (0u << 9) | (8 + r));
    block(0);
    a.bind(out);
    a.words.push_back(0x34140085u);  // v_lshlrev_b32 v10, 5, v0
    for (uint32_t r = 0; r < 4; ++r) a.words.push_back(0x7E000200u | ((4 + r) << 17) | (8 + r));  // v4-v7 = s8-s11
    for (uint32_t r = 1; r <= 7; ++r) {
      a.words.push_back(0xE0701000u | (4 * (r - 1)));          // buffer_store_dword vr, v10, s[4:7], 0 offen
      a.words.push_back(10u | (r << 8) | (1u << 16) | (0x80u << 24));
    }
    a.words.push_back(0xBF810000u);
    a.finish();
    std::vector<uint8_t> code;
    for (uint32_t w : a.words) {
      const auto* p = reinterpret_cast<const uint8_t*>(&w);
      code.insert(code.end(), p, p + 4);
    }
    return code;
  }
};

}  // namespace

TEST(random_control_flow_runs_the_same_as_the_reference_translation) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  EXPECT(gpu.set_constants({0, 0, 0, 0, 0, 1, 256, 0}, true));
  int compared = 0, different = 0, failed = 0;
  for (uint32_t seed = 1; seed <= 60; ++seed) {
    ProgramGenerator generator;
    generator.state = seed * 2654435761u;
    const std::vector<uint8_t> code = generator.generate();
    ComputeShaderInfo info;
    info.threads[0] = 8;
    info.user_sgprs = 8;
    std::vector<uint8_t> reference, optimised;
    std::string error;
    TranslatorOptions options;
    options.reference = true;
    set_translator_options(options);
    const bool made_reference = translate_compute(code.data(), code.size(), info, &reference, &error);
    set_translator_options(TranslatorOptions());
    const bool made = translate_compute(code.data(), code.size(), info, &optimised, &error);
    if (!made_reference || !made) {
      ++failed;
      std::printf("      (seed %u: %s)\n", seed, error.c_str());
      continue;
    }
    const std::vector<uint32_t> expected = gpu.dispatch(reference, std::vector<uint32_t>(64, 0));
    const std::vector<uint32_t> got = gpu.dispatch(optimised, std::vector<uint32_t>(64, 0));
    ++compared;
    if (expected.size() != 64 || got != expected) {
      ++different;
      if (different <= 3) std::printf("      (seed %u: results differ)\n", seed);
    }
  }
  std::printf("      (%d random programs compared, %d different, %d not translated)\n", compared, different, failed);
  EXPECT(failed == 0);
  EXPECT(different == 0);
  EXPECT(compared == 60);
}

// A group barrier between local data share writes and reads of other
// threads' values (two waves of 32: the barrier is the group's).
TEST(group_barriers_order_local_data_share_between_waves) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v1 = v0 * 4; ds_write_b32 v1, v0; s_barrier; v2 = 252 - v1;
  // ds_read_b32 v3, v2; buffer_store_dword v3, v1, s[0:3], 0 offen; s_endpgm.
  const std::vector<uint32_t> program = {0x34020082u, 0xD8340000u, 0x00000001u, 0xBF8A0000u, 0x4C0402FFu,
                                         0x000000FCu, 0xD8D80000u, 0x03000002u, 0xE0701000u, 0x80000301u,
                                         0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 64;
  info.user_sgprs = 4;
  info.lds_bytes = 256;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64, 0));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  bool reversed = true;
  for (uint32_t i = 0; i < 64; ++i) reversed = reversed && after[i] == 63 - i;
  EXPECT(reversed);
}

// Cached pages (shader/translate.h): the fourth page of the window is cache
// line 0, whose copy is slot 2 (a buffer holding 0x2000 + i) and whose page
// itself is slot 1 from view offset 0x80. Reads take the copy until a write;
// the write goes to the page and sets the line's flag; reads after it take
// the page.
TEST(cached_pages_read_their_copy_until_written) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  D3D12_HEAP_PROPERTIES write_back{};
  write_back.Type = D3D12_HEAP_TYPE_CUSTOM;
  write_back.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  write_back.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 256;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* copy = nullptr;
  ID3D12Resource* flags = nullptr;
  EXPECT(SUCCEEDED(gpu.device->CreateCommittedResource(&write_back, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                                                       reinterpret_cast<void**>(&copy))));
  EXPECT(SUCCEEDED(gpu.device->CreateCommittedResource(&write_back, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                                                       reinterpret_cast<void**>(&flags))));
  if (!copy || !flags) return;
  uint32_t* words = nullptr;
  copy->Map(0, nullptr, reinterpret_cast<void**>(&words));
  for (uint32_t i = 0; i < 64; ++i) words[i] = 0x2000 + i;
  copy->Unmap(0, nullptr);
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
  raw.Format = DXGI_FORMAT_R32_TYPELESS;
  raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  raw.Buffer.NumElements = 64;
  const UINT increment = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE at = gpu.views->GetCPUDescriptorHandleForHeapStart();
  at.ptr += 2ull * increment;
  gpu.device->CreateUnorderedAccessView(copy, nullptr, &raw, at);
  at = gpu.views->GetCPUDescriptorHandleForHeapStart();
  at.ptr += static_cast<SIZE_T>(kFlagsView) * increment;
  gpu.device->CreateUnorderedAccessView(flags, nullptr, &raw, at);
  uint32_t* table = nullptr;
  gpu.page_table->Map(0, nullptr, reinterpret_cast<void**>(&table));
  table[kPageTableLevel2Base / 4 + 6] = kCachedPageBit | 0;
  table[kPageTableLevel1Bytes / 4 + 0] = 2;     // the copy: slot 2, view offset 0
  table[kPageTableLevel1Bytes / 4 + 1] = 0;
  table[kPageTableLevel1Bytes / 4 + 2] = 1;     // the page: slot 1, view offset 0x80
  table[kPageTableLevel1Bytes / 4 + 3] = 0x80;
  gpu.page_table->Unmap(0, nullptr);
  // buffer_load_dword v0, off, s[0:3], 0; v1 = 0x5555; buffer_store_dword
  // v1, off, s[0:3], 0 offset:4; buffer_load_dword v2, off, s[0:3], 0
  // offset:8; s_waitcnt vmcnt(0); buffer_store_dword v0 and v2, off,
  // s[4:7], 0 offset:192 and 196; s_endpgm.
  const std::vector<uint32_t> program = {0xE0300000u, 0x80000000u, 0x7E0202FFu, 0x00005555u, 0xE0700004u,
                                         0x80000100u, 0xE0300008u, 0x80000200u, 0xBF8C3F70u, 0xE07000C0u,
                                         0x80010000u, 0xE07000C4u, 0x80010200u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 8;
  std::vector<uint8_t> cs;
  std::string error;
  TranslatorOptions options;
  options.page_cache_reads = true;
  set_translator_options(options);
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  set_translator_options(TranslatorOptions());
  EXPECT(gpu.set_constants({0x30000, 1, 256, 0, 0, 1, 256, 0}, true));
  std::vector<uint32_t> contents(64);
  for (uint32_t i = 0; i < 64; ++i) contents[i] = 0x1000 + i;
  const std::vector<uint32_t> after = gpu.dispatch(cs, contents);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  uint32_t* flag = nullptr;
  flags->Map(0, nullptr, reinterpret_cast<void**>(&flag));
  const uint32_t line_flag = flag ? flag[0] : 0;
  flags->Unmap(0, nullptr);
  copy->Map(0, nullptr, reinterpret_cast<void**>(&words));
  const uint32_t copy_word = words ? words[1] : 0;
  copy->Unmap(0, nullptr);
  EXPECT(after[48] == 0x2000);       // read before the write: the copy
  EXPECT(after[33] == 0x5555);       // the write: the page itself (0x80 + 4)
  EXPECT(after[49] == 0x1000 + 34);  // read after the write: the page (0x80 + 8)
  EXPECT(line_flag == 1);            // the line's flag, set by the write
  EXPECT(copy_word == 0x2001);       // the copy kept
  copy->Release();
  flags->Release();
}

// Relative moves (documented): v_movrels_b32 reads v[src + M0],
// v_movreld_b32 writes v[dst + M0].
TEST(relative_moves_index_registers_by_m0) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v1..v4 = 0x11, 0x22, 0x33, 0x44; s_mov_b32 m0, 2; v_movrels_b32 v5, v1
  // (v3); s_mov_b32 m0, 1; v_movreld_b32 v1, 0x99 (v2); buffer_store_dword
  // v5, v2, v3 at 0, 4, 8; s_endpgm.
  const std::vector<uint32_t> program = {0x7E0202FFu, 0x11u, 0x7E0402FFu, 0x22u, 0x7E0602FFu, 0x33u, 0x7E0802FFu,
                                         0x44u, 0xBEFC0382u, 0x7E0A8701u, 0xBEFC0381u, 0x7E0284FFu, 0x99u,
                                         0xE0700000u, 0x80000500u, 0xE0700004u, 0x80000200u, 0xE0700008u,
                                         0x80000300u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 4;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[0] == 0x33);  // v3, read through v1 + 2
  EXPECT(after[1] == 0x99);  // v2, written through v1 + 1
  EXPECT(after[2] == 0x33);  // v3 kept
}

// Doubles (documented: register pairs, low dword first; inline float
// constants as doubles): add, multiply and conversions on the host's doubles.
TEST(double_precision_adds_multiplies_and_converts) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v[1:2] = 1.5, v[3:4] = 2.25, v[12:13] = 1 + 2^-40; v_add_f64 v[5:6];
  // v_mul_f64 v[7:8]; v_cvt_f32_f64 v9, v[5:6]; v_cvt_f64_f32 v[10:11], v9;
  // v_add_f64 v[14:15], v[12:13], 1.0; stores of v[5:8], v[9:11], v[14:15].
  const std::vector<uint32_t> program = {
      0x7E020280u, 0x7E0402FFu, 0x3FF80000u, 0x7E060280u, 0x7E0802FFu, 0x40020000u, 0x7E1802FFu, 0x00001000u,
      0x7E1A02FFu, 0x3FF00000u, 0xD5640005u, 0x00020701u, 0xD5650007u, 0x00020701u, 0x7E121F05u, 0x7E142109u,
      0xD564000Eu, 0x0001E50Cu, 0xE0780000u, 0x80000500u, 0xE07C0010u, 0x80000900u, 0xE074001Cu, 0x80000E00u,
      0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 1;
  info.user_sgprs = 4;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));
  const std::vector<uint32_t> after = gpu.dispatch(cs, std::vector<uint32_t>(64));
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[0] == 0 && after[1] == 0x400E0000u);  // 3.75
  EXPECT(after[2] == 0 && after[3] == 0x400B0000u);  // 3.375
  EXPECT(after[4] == 0x40700000u);                   // 3.75f
  EXPECT(after[5] == 0 && after[6] == 0x400E0000u);  // 3.75 again
  EXPECT(after[7] == 0x00000800u && after[8] == 0x40000000u);  // 2 + 2^-40
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

TEST(scalar_bit_counts_and_searches_run_as_documented) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // s[4:5] = 0x100, s[12:13] = 0xFFFFFFFF_FFFF0000, s[16:17] = 0;
  // s6 = s_ff1_i32_b64 s[4:5]; s7 = s_flbit_i32 0x40000000;
  // s8 = s_flbit_i32_b64 s[4:5]; s9 = s_bcnt0_i32_b64 s[4:5];
  // s10 = s_flbit_i32_i64 s[12:13]; s[14:15] = s_brev_b64 s[4:5];
  // s_bitset1_b64 s[16:17], 40; v0-v7 = s6-s10, s14, s15, s17;
  // s_waitcnt_vscnt null, 0; s_setreg_imm32_b32 hwreg(1, 4, 4), 3 (neither
  // has a destination register);
  // buffer_store_dwordx4 v[0:3] offset:64; v[4:7] offset:80; s_endpgm.
  const std::vector<uint32_t> program = {
      0xBE8403FFu, 0x00000100u, 0xBE850380u, 0xBE8C03FFu, 0xFFFF0000u, 0xBE8D03C1u, 0xBE900380u, 0xBE910380u,
      0xBE861404u, 0xBE8717FFu, 0x40000000u, 0xBE881604u, 0xBE890E04u, 0xBE8A180Cu, 0xBE8E0C04u, 0xBE901EA8u,
      0x7E000206u, 0x7E020207u, 0x7E040208u, 0x7E060209u, 0x7E08020Au, 0x7E0A020Eu, 0x7E0C020Fu, 0x7E0E0211u,
      0xBBFD0000u, 0xBA801901u, 0x00000003u,
      0xE0780040u, 0x80000000u, 0xE0780050u, 0x80000400u, 0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 8);           // lowest set bit of 0x100
  EXPECT(after[17] == 1);           // one leading bit equal to the sign of 0x40000000
  EXPECT(after[18] == 55);          // leading zeros of the 64-bit 0x100
  EXPECT(after[19] == 63);          // zero bits of the 64-bit 0x100
  EXPECT(after[20] == 48);          // leading sign bits of 0xFFFFFFFF_FFFF0000
  EXPECT(after[21] == 0 && after[22] == 0x00800000u);  // 0x100 reversed over 64 bits
  EXPECT(after[23] == 0x100);       // bit 40 set: bit 8 of the high half
}

TEST(sub_dword_destination_selects_place_and_extend) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = 0x12345678; v1 = v2 = 0xAAAABBBB; v3 = 0xF0;
  // v_mov_b32_sdwa v1, v0 dst_sel:WORD_1 dst_unused:PRESERVE src0_sel:BYTE_0;
  // v_mov_b32_sdwa v2, v3 dst_sel:BYTE_1 dst_unused:SEXT src0_sel:BYTE_0;
  // buffer_store_dwordx4 v[0:3] offset:64; s_endpgm.
  const std::vector<uint32_t> program = {0x7E0002FFu, 0x12345678u, 0x7E0202FFu, 0xAAAABBBBu, 0x7E0402FFu, 0xAAAABBBBu,
                                         0x7E0602FFu, 0x000000F0u, 0x7E0202F9u, 0x00001500u, 0x7E0402F9u, 0x00000903u,
                                         0xE0780040u, 0x80000000u, 0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x12345678u);
  EXPECT(after[17] == 0x0078BBBBu);  // byte 0 of v0 into the high word, the low word kept
  EXPECT(after[18] == 0xFFFFF000u);  // 0xF0 sign-extended above byte 1, zero below
  EXPECT(after[19] == 0x000000F0u);
}

TEST(local_data_share_pairs_bytes_and_shorts) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = 0; v1 = 0x11223344; v2 = 0x55667788; v3 = 0xAABBCCDD; v4 = 0x99;
  // ds_write2st64_b64 v0, v[1:2], v[3:4] offset1:1; ds_read2st64_b64 v[8:11], v0 offset1:1;
  // ds_write_b8 v0, v4 offset:1; ds_read_i8 v12, v0 offset:1; ds_read_u16 v13, v0 offset:2;
  // buffer_store_dwordx4 v[8:11] offset:64; buffer_store_dwordx2 v[12:13] offset:80; s_endpgm.
  const std::vector<uint32_t> program = {
      0x7E000280u, 0x7E0202FFu, 0x11223344u, 0x7E0402FFu, 0x55667788u, 0x7E0602FFu, 0xAABBCCDDu, 0x7E0802FFu,
      0x00000099u, 0xD93C0100u, 0x00030100u, 0xD9E00100u, 0x08000000u, 0xD8780001u, 0x00000400u, 0xD8E40001u,
      0x0C000000u, 0xD8F00002u, 0x0D000000u, 0xE0780040u, 0x80000800u, 0xE0740050u, 0x80000C00u, 0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x11223344u && after[17] == 0x55667788u);  // first pair at 0
  EXPECT(after[18] == 0xAABBCCDDu && after[19] == 0x00000099u);  // second pair 64 elements on
  EXPECT(after[20] == 0xFFFFFF99u);  // the stored byte, sign-extended
  EXPECT(after[21] == 0x00001122u);  // the untouched high half of the word
}

TEST(program_data_is_read_through_the_program_counter) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // s_getpc_b64 s[4:5]; s_add_u32 s4, s4, 0x34 (the table after s_code_end);
  // s_movk_i32 s6, 0x100; s_mov_b32 s7, 0; v_mov_b32 v0, 0;
  // buffer_load_dword v1, off, s[4:7] offset:4; buffer_load_dword v2, off, s[4:7] offset:6;
  // buffer_store_dwordx2 v[1:2], off, s[0:3] offset:64; s_endpgm; s_code_end; table.
  const std::vector<uint32_t> program = {0xBE841F00u, 0x8004FF04u, 0x00000034u, 0xB0060100u, 0xBE870380u,
                                         0x7E000280u, 0xE0300004u, 0x80010100u, 0xE0300006u, 0x80010200u,
                                         0xE0740040u, 0x80000100u, 0xBF810000u, 0xBF9F0000u, 0x11223344u,
                                         0x55667788u, 0x99AABBCCu};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x55667788u);  // the table's second dword
  EXPECT(after[17] == 0xBBCC5566u);  // an unaligned dword across the second and third
}

TEST(packed_half_operations_select_halves) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = {1.5, -2.0} (halves), v1 = {2.0, 0.5}, v5 = 0.25;
  // v_pk_mul_f16 v2, v0, v1; v_pk_add_u16 v3, v0, v1 op_sel:[1,0] op_sel_hi:[0,1];
  // v_dot2_f32_f16 v4, v0, v1, v5; buffer_store_dwordx3 v[2:4] offset:64; s_endpgm.
  const std::vector<uint32_t> program = {0x7E0002FFu, 0xC0003E00u, 0x7E0202FFu, 0x38004000u, 0x7E0A02FFu,
                                         0x3E800000u, 0xCC104002u, 0x18020300u, 0xCC0A0803u, 0x10020300u,
                                         0xCC134004u, 0x1C160300u, 0xE07C0040u, 0x80000200u, 0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0xBC004200u);  // {3.0, -1.0}
  EXPECT(after[17] == 0x76000000u);  // {0xC000 + 0x4000 (wrapped), 0x3E00 + 0x3800}
  EXPECT(after[18] == 0x40100000u);  // 1.5 * 2 - 2 * 0.5 + 0.25 = 2.25
}

TEST(half_legacy_and_cube_operations_run_as_documented) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = 0.0, v1 = +inf, v2 = 2.0, v4 = {-2.0, 1.0}, v5 = {1.5, 2.0} (halves),
  // v11 = 0.5, v12 = -3.0, v13 = 1.0;
  // v_mad_legacy_f32 v3, v0, v1, v2; v_add_f16 v6, v4, v5;
  // v_pack_b32_f16 v7, v4, v5 op_sel:[1,1]; v_mad_u32_u16 v8, v4, v5, v2;
  // v_cmp_gt_f16 vcc, v5, v4; v_cndmask_b32 v9, 0, v2; v_cubeid_f32 v10, v11, v12, v13;
  // buffer_store_dwordx4 v[6:9] offset:64; buffer_store_dword v3 offset:80;
  // buffer_store_dword v10 offset:84; s_endpgm.
  const std::vector<uint32_t> program = {
      0x7E000280u, 0x7E0202FFu, 0x7F800000u, 0x7E0402FFu, 0x40000000u, 0x7E0802FFu, 0x3C00C000u, 0x7E0A02FFu,
      0x40003E00u, 0x7E1602FFu, 0x3F000000u, 0x7E1802FFu, 0xC0400000u, 0x7E1A02FFu, 0x3F800000u, 0xD5400003u,
      0x040A0300u, 0x640C0B04u, 0xD7111807u, 0x00020B04u, 0xD7730008u, 0x040A0B04u, 0x7D980905u, 0x02120480u,
      0xD544000Au, 0x0436190Bu, 0xE0780040u, 0x80000600u, 0xE0700050u, 0x80000300u, 0xE0700054u, 0x80000A00u,
      0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x0000B800u);  // -2.0 + 1.5 = -0.5, high half zero
  EXPECT(after[17] == 0x40003C00u);  // the high halves packed: {1.0, 2.0}
  EXPECT(after[18] == 0x6E800000u);  // 0xC000 * 0x3E00 + 0x40000000
  EXPECT(after[19] == 0x40000000u);  // 1.5 > -2.0
  EXPECT(after[20] == 0x40000000u);  // 0 * inf + 2 = 2 (legacy multiply)
  EXPECT(after[21] == 0x40400000u);  // major axis -y: face 3
}

TEST(format_stores_pack_by_the_descriptor_format) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = 1.0, v1 = 0.5, v2 = 0, v3 = 0.25;
  // buffer_store_format_xyzw v[0:3], off, s[0:3] offset:64 (8_8_8_8_UNORM);
  // s[4:7] = s[0:3] with format 13 (16_FLOAT);
  // buffer_store_format_x v1, off, s[4:7] offset:68; s_endpgm.
  const std::vector<uint32_t> program = {0x7E0002F2u, 0x7E0202F0u, 0x7E040280u, 0x7E0602FFu, 0x3E800000u,
                                         0xE01C0040u, 0x80000000u, 0xBE840400u, 0xBE860302u, 0xBE8703FFu,
                                         0x0000D000u, 0xE0100044u, 0x80010100u, 0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 56u << 12}, true));  // title memory at 2^32
  std::vector<uint32_t> contents(18, 0);
  contents[17] = 0x11112222u;
  const std::vector<uint32_t> after = gpu.dispatch(cs, contents);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x400080FFu);  // {255, 128, 0, 64}
  EXPECT(after[17] == 0x11113800u);  // a half 0.5 in the low half; the high half kept
}

TEST(lane_permutes_through_local_data_share_instructions) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = lane; v4 = lane * 4; v2 = v4 + 4;
  // ds_bpermute_b32 v1, v2, v0 (lane + 1); ds_swizzle_b32 v3, v0 offset:0x801b (quads reversed);
  // buffer_store_dword v1, v4 offen; buffer_store_dword v3, v4 offen offset:128; s_endpgm.
  const std::vector<uint32_t> program = {0x34080082u, 0x4A040884u, 0xDACC0000u, 0x01000002u, 0xD8D4801Bu,
                                         0x03000000u, 0xE0701000u, 0x80000104u, 0xE0701080u, 0x80000304u,
                                         0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 32;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  bool permuted = true, swizzled = true;
  for (uint32_t i = 0; i < 32; ++i) {
    permuted = permuted && after[i] == ((i + 1) & 31);
    swizzled = swizzled && after[32 + i] == ((i & ~3u) | (3 - (i & 3)));
  }
  EXPECT(permuted);
  EXPECT(swizzled);
}

void cube_sampling_check(bool gradients) {
  Gpu gpu;
  if (!gpu.open() || !gpu.make_cube()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // Face coordinates as open-source drivers form them from a direction:
  // v4 = lane * 16; buffer_load_dwordx3 v[5:7], v4, s[0:3] offen (x, y, z);
  // v_cubeid_f32 v10, v5, v6, v7; v_cubesc_f32 v11, ...; v_cubetc_f32 v12, ...;
  // v_cubema_f32 v13, ...; v_rcp_f32 v13, |v13|; v20 = 1.5;
  // v_fma_f32 v11, v11, v13, v20; v_fma_f32 v12, v12, v13, v20; v13 = v10;
  // image_sample_lz v14, v[11:13], s[4:11], s[12:15] dmask:0x1 dim:CUBE;
  // buffer_store_dword v14, v4, s[0:3] offen offset:128; s_endpgm.
  // With `gradients`, image_sample_d instead, with zero face-coordinate
  // gradients: v[15:17] = s, t, face; v[11:14] = 0;
  // image_sample_d v18, v[11:17], s[4:11], s[12:15] dmask:0x1 dim:CUBE;
  // buffer_store_dword v18, v4, s[0:3] offen offset:128.
  std::vector<uint32_t> program = {
      0x34080084u, 0xE03C1000u, 0x80000504u, 0xD544000Au, 0x041E0D05u, 0xD545000Bu, 0x041E0D05u, 0xD546000Cu,
      0x041E0D05u, 0xD547000Du, 0x041E0D05u, 0xD5AA010Du, 0x0000010Du, 0x7E2802FFu, 0x3FC00000u, 0xD54B000Bu,
      0x04521B0Bu, 0xD54B000Cu, 0x04521B0Cu, 0x7E1A030Au};
  if (gradients) {
    program.insert(program.end(), {0x7E1E030Bu, 0x7E20030Cu, 0x7E22030Du, 0x7E160280u, 0x7E180280u, 0x7E1A0280u,
                                   0x7E1C0280u, 0xF0880118u, 0x0061120Bu, 0xE0701080u, 0x80001204u, 0xBF810000u});
  } else {
    program.insert(program.end(), {0xF09C0118u, 0x00610E0Bu, 0xE0701080u, 0x80000E04u, 0xBF810000u});
  }
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 8;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(error.empty());
  // Image and sampler descriptors (s[4:11], s[12:15]) select index 0.
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  // A direction on each face, off its centre, and two more.
  const float directions[8][3] = {{1.0f, 0.5f, -0.5f},  {-1.0f, -0.5f, -0.5f}, {0.5f, 1.0f, -0.5f},
                                  {-0.5f, -1.0f, 0.5f}, {0.5f, -0.5f, 1.0f},   {-0.5f, 0.5f, -1.0f},
                                  {0.3f, -0.6f, 2.0f},  {-2.0f, 0.7f, 0.4f}};
  std::vector<uint32_t> contents(32, 0);
  for (uint32_t lane = 0; lane < 8; ++lane) std::memcpy(&contents[4 * lane], directions[lane], 12);
  const std::vector<uint32_t> after = gpu.dispatch(cs, contents);
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  // The host's own face and texel for each direction.
  for (uint32_t lane = 0; lane < 8; ++lane) {
    const float x = directions[lane][0], y = directions[lane][1], z = directions[lane][2];
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    uint32_t face;
    float u, v;
    if (ax >= ay && ax >= az) {
      face = x > 0 ? 0 : 1;
      u = (x > 0 ? -z : z) / ax;
      v = -y / ax;
    } else if (ay >= az) {
      face = y > 0 ? 2 : 3;
      u = x / ay;
      v = (y > 0 ? z : -z) / ay;
    } else {
      face = z > 0 ? 4 : 5;
      u = (z > 0 ? x : -x) / az;
      v = -y / az;
    }
    const uint32_t tu = u > 0 ? 1 : 0, tv = v > 0 ? 1 : 0;
    float sampled;
    std::memcpy(&sampled, &after[32 + 4 * lane], 4);
    EXPECT(sampled == static_cast<float>(4 * face + 2 * tv + tu));
    if (sampled != static_cast<float>(4 * face + 2 * tv + tu)) {
      std::printf("      lane %u: sampled %g, expected %u\n", lane, sampled, 4 * face + 2 * tv + tu);
    }
  }
}

TEST(cube_face_coordinates_sample_the_direction_they_came_from) { cube_sampling_check(false); }

TEST(cube_face_coordinate_gradients_sample_the_same_texels) { cube_sampling_check(true); }

TEST(pixel_wave_reductions_reduce_over_the_quad) {
  Gpu gpu;
  const std::vector<uint8_t> vs = covering_vertex_shader();
  if (vs.empty() || !gpu.open()) {
    std::printf("      (no Direct3D 12 device or shader compiler: not run)\n");
    return;
  }
  // The console compiler's whole-wave reduction of v1 (op: v_min_u32 or
  // v_add_nc_u32), as titles' pixel programs carry it:
  // s_or_saveexec_b32 s66, -1; v_cndmask_b32 v126, identity, v1, s66;
  // v_op_dpp v126, v126, v126 quad_perm:[1,0,3,2], then [2,3,0,1],
  // row_half_mirror, row_mirror (fetch inactive); v_permlanex16_b32 v127, v126, 0, 0;
  // v_op v126, v127, v126; s_mov_b32 exec_lo, s66; v_readlane_b32 s65, v126, 31;
  // then v2 = s65 (min) or v2 = float(s65) (sum of whole numbers).
  auto reduction = [](uint32_t op, uint32_t identity, std::initializer_list<uint32_t> before, uint32_t last) {
    const uint32_t dpp = 0x00FCFCFAu | (op << 25), plain = 0x00FCFD7Fu | (op << 25);
    std::vector<uint32_t> middle(before);
    const std::vector<uint32_t> body = {0xBEC23DC1u, 0xD501007Eu, 0x010A0200u | identity,
                                        dpp, 0xFF04B17Eu, dpp, 0xFF044E7Eu, dpp, 0xFF05417Eu, dpp, 0xFF05407Eu,
                                        0xD778007Fu, 0x0201017Eu, plain, 0xBEFE0342u, 0xD7600041u, 0x00013F7Eu,
                                        last};
    middle.insert(middle.end(), body.begin(), body.end());
    std::vector<uint32_t> code = {0xC8010000u, 0x100200FFu, 0x3F9E3779u};
    code.insert(code.end(), middle.begin(), middle.end());
    code.insert(code.end(), {0xF8000801u, 2u, 0xBF810000u});
    std::vector<uint8_t> out;
    for (uint32_t v : code) {
      const auto* p = reinterpret_cast<const uint8_t*>(&v);
      out.insert(out.end(), p, p + 4);
    }
    return out;
  };
  ShaderInterface link;
  std::string error;
  std::vector<uint8_t> own, minimum, own_whole, sum;
  EXPECT(translate_graphics(pixel_program({}, 1).data(), pixel_program({}, 1).size(), GraphicsStage::Pixel, &own,
                            &link, &error));
  // Minimum of v1's bits (positive floats: their order).
  const std::vector<uint8_t> min_code = reduction(0x13, 0xC1, {}, 0x7E040241u);
  EXPECT(translate_graphics(min_code.data(), min_code.size(), GraphicsStage::Pixel, &minimum, &link, &error));
  EXPECT(error.empty());
  // Sum of v1 = u32(v0): v_cvt_u32_f32 v1, v0 first; v_cvt_f32_u32 v2, s65 last.
  const std::vector<uint8_t> whole_code = pixel_program({0x7E020F00u, 0x7E040D01u}, 2);
  EXPECT(translate_graphics(whole_code.data(), whole_code.size(), GraphicsStage::Pixel, &own_whole, &link, &error));
  const std::vector<uint8_t> sum_code = reduction(0x25, 0x80, {0x7E020F00u}, 0x7E040C41u);
  EXPECT(translate_graphics(sum_code.data(), sum_code.size(), GraphicsStage::Pixel, &sum, &link, &error));
  EXPECT(error.empty());
  const std::vector<uint32_t> a = gpu.draw(vs, own), b = gpu.draw(vs, minimum);
  const std::vector<uint32_t> c = gpu.draw(vs, own_whole), d = gpu.draw(vs, sum);
  EXPECT(a.size() == kSize * kSize && b.size() == a.size() && c.size() == a.size() && d.size() == a.size());
  if (a.size() != kSize * kSize || b.size() != a.size() || c.size() != a.size() || d.size() != a.size()) return;
  auto as_float = [](uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  bool exact_min = true, exact_sum = true;
  for (UINT y = 0; y < kSize; ++y) {
    for (UINT x = 0; x < kSize; ++x) {
      uint32_t low = 0xFFFFFFFFu;
      float total = 0;
      for (UINT j = 0; j < 4; ++j) {
        const UINT at = ((y & ~1u) + (j >> 1)) * kSize + (x & ~1u) + (j & 1);
        if (a[at] < low) low = a[at];
        total += as_float(c[at]);
      }
      exact_min = exact_min && b[y * kSize + x] == low;
      exact_sum = exact_sum && as_float(d[y * kSize + x]) == total;
    }
  }
  EXPECT(exact_min);
  EXPECT(exact_sum);
}

TEST(lane_mask_counts_follow_the_lane_mask_model) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v4 = lane * 4; v_mbcnt_lo_u32_b32 v1, exec_lo, 0 (a lane mask: no lane below);
  // v_mbcnt_lo_u32_b32 v2, 56, 0 (a constant: by lane index);
  // buffer_store_dword v1, v4 offen; buffer_store_dword v2, v4 offen offset:128; s_endpgm.
  const std::vector<uint32_t> program = {0x34080082u, 0xD7650001u, 0x0001007Eu, 0xD7650002u, 0x000100B8u,
                                         0xE0701000u, 0x80000104u, 0xE0701080u, 0x80000204u, 0xBF810000u};
  std::vector<uint8_t> code;
  for (uint32_t v : program) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    code.insert(code.end(), p, p + 4);
  }
  ComputeShaderInfo info;
  info.threads[0] = 32;
  std::vector<uint8_t> cs;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &cs, &error));
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  bool lane_masks = true, constants = true;
  for (uint32_t i = 0; i < 32; ++i) {
    lane_masks = lane_masks && after[i] == 0;
    uint32_t below = 0;
    for (uint32_t bit = 0; bit < i; ++bit) below += (56u >> bit) & 1;
    constants = constants && after[32 + i] == below;
  }
  EXPECT(lane_masks);
  EXPECT(constants);
}

TEST(subroutine_calls_return_and_data_after_the_end_is_not_code) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // 0x00 s_getpc_b64 s[4:5]; 0x04 s_add_u32 s4, s4, 0x18 (the subroutine at 0x1c);
  // 0x0c s_swappc_b64 s[6:7], s[4:5]; 0x10 buffer_store_dword v1, off, s[0:3] offset:64;
  // 0x18 s_endpgm; 0x1c v_mov_b32 v1, 0x1234; 0x24 s_setpc_b64 s[6:7] (return);
  // 0x28 a data word (no instruction).
  const std::vector<uint32_t> program = {0xBE841F00u, 0x8004FF04u, 0x00000018u, 0xBE862104u, 0xE0700040u,
                                         0x80000100u, 0xBF810000u, 0x7E0202FFu, 0x00001234u, 0xBE802006u,
                                         0x00000004u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x1234u);
}

TEST(pack_conversions_and_packed_half_fma_run_as_documented) {
  Gpu gpu;
  if (!gpu.open()) {
    std::printf("      (no Direct3D 12 device: not run)\n");
    return;
  }
  // v0 = 0.5, v1 = -1.0, v5 = 0x12345, v6 = 7, v7 = {1.0, -1.0}, v8 = {1.5, 2.0}, v9 = {2.0, 0.5} (halves);
  // v_cvt_pknorm_u16_f32 v2, v0, v1; v_cvt_pknorm_i16_f32 v3, v0, v1; v_cvt_pk_u16_u32 v4, v5, v6;
  // v_pk_fmac_f16 v7, v8, v9; buffer_store_dwordx4 v[2:5] offset:64; buffer_store_dword v7 offset:80.
  const std::vector<uint32_t> program = {
      0x7E0002F0u, 0x7E0202F3u, 0x7E0A02FFu, 0x00012345u, 0x7E0C0287u, 0x7E0E02FFu, 0xBC003C00u, 0x7E1002FFu,
      0x40003E00u, 0x7E1202FFu, 0x38004000u, 0xD7690002u, 0x00020300u, 0xD7680003u, 0x00020300u, 0xD76A0004u,
      0x00020D05u, 0x780E1308u, 0xE0780040u, 0x80000200u, 0xE0700050u, 0x80000700u, 0xBF810000u};
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
  EXPECT(error.empty());
  EXPECT(gpu.set_constants({0, 1, 256, 0}, true));  // title memory at 2^32
  const std::vector<uint32_t> after = gpu.dispatch(cs, {0});
  EXPECT(after.size() == 64);
  if (after.size() != 64) return;
  EXPECT(after[16] == 0x00008000u);  // unorm16: 0.5 -> 32768 (to even), -1 -> 0
  EXPECT(after[17] == 0x80014000u);  // snorm16: 0.5 -> 16384, -1 -> -32767
  EXPECT(after[18] == 0x0007FFFFu);  // 0x12345 saturated, 7
  EXPECT(after[20] == 0x00004400u);  // {1.5 * 2 + 1, 2 * 0.5 - 1} = {4.0, 0}
}

// Rectangle lists: the generated geometry program draws the whole rectangle
// three corners describe, and attributes take their affine values in the
// corner it adds.
TEST(rectangle_lists_draw_the_whole_rectangle) {
  Gpu gpu;
  const std::vector<uint8_t> vs = compiled(
      "struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };"
      "O main(uint id : SV_VertexID) {"
      "  float2 uv = float2(id == 1, id == 2);"
      "  O o;"
      "  o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);"
      "  o.t = float4(10 + 3 * uv.x + 5 * uv.y, 0, 0, 0);"
      "  return o;"
      "}",
      "vs_5_1");
  const std::vector<uint8_t> ps = compiled(
      "float4 main(float4 pos : SV_Position, float4 t : TEXCOORD0) : SV_Target { return t.x; }", "ps_5_1");
  if (vs.empty() || ps.empty() || !gpu.open()) {
    std::printf("      (no Direct3D 12 device or shader compiler: not run)\n");
    return;
  }
  std::vector<SignatureEntry> outputs;
  EXPECT(read_output_signature(vs.data(), vs.size(), &outputs));
  EXPECT(outputs.size() == 2);
  const std::vector<uint8_t> gs = rectangle_list_program(outputs);
  const std::vector<uint32_t> half = gpu.draw(vs, ps);
  const std::vector<uint32_t> whole = gpu.draw(vs, ps, &gs);
  EXPECT(half.size() == kSize * kSize && whole.size() == kSize * kSize);
  if (half.size() != kSize * kSize || whole.size() != kSize * kSize) return;
  auto as_float = [](uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  bool covered = true, affine = true, half_uncovered = false;
  for (UINT y = 0; y < kSize; ++y) {
    for (UINT x = 0; x < kSize; ++x) {
      const float u = (x + 0.5f) / kSize, v = (y + 0.5f) / kSize;
      const float expected = 10 + 3 * u + 5 * v;
      const float got = as_float(whole[y * kSize + x]);
      covered = covered && got != 0.0f;
      affine = affine && std::fabs(got - expected) < 1e-4f;
      half_uncovered = half_uncovered || half[y * kSize + x] == 0;
    }
  }
  EXPECT(half_uncovered);  // the triangle alone leaves the far corner empty
  EXPECT(covered);
  EXPECT(affine);
}
