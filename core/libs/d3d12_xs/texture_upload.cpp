#include "core/libs/d3d12_xs/texture_upload.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/support.h"
#include "core/libs/texture_layout.h"

namespace holy::d3d12 {
namespace {

using Key = std::tuple<uint64_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>;

struct Uploader {
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE event = nullptr;
  uint64_t value = 0;
};

std::mutex g_lock;
std::map<Key, ID3D12Resource*>& textures() {
  static auto* map = new std::map<Key, ID3D12Resource*>();
  return *map;
}

bool make_uploader(Device* device, Uploader* out) {
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  const bool made = SUCCEEDED(device->host->CreateCommandQueue(&queue, IID_PPV_ARGS(&out->queue))) &&
         SUCCEEDED(device->host->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&out->allocator))) &&
         SUCCEEDED(device->host->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, out->allocator, nullptr,
                                                   IID_PPV_ARGS(&out->list))) &&
         SUCCEEDED(out->list->Close()) &&
         SUCCEEDED(device->host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&out->fence))) &&
         (out->event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr;
  if (made) {
    out->queue->SetName(L"holy texture upload queue");
    out->list->SetName(L"holy texture upload");
  }
  return made;
}

Uploader* uploader(Device* device) {
  static Uploader* instance = [&] {
    auto* made = new Uploader();
    if (!make_uploader(device, made)) {
      LIB_LOG(lib::kError, "d3d12", "cannot create the texture upload queue");
      return static_cast<Uploader*>(nullptr);
    }
    return made;
  }();
  return instance;
}

lib::TextureShape shape_of(const D3D12_RESOURCE_DESC& desc) {
  lib::TextureShape shape;
  shape.dimension = static_cast<uint32_t>(desc.Dimension);
  shape.width = desc.Width;
  shape.height = desc.Height;
  shape.depth_or_array = desc.DepthOrArraySize ? desc.DepthOrArraySize : 1;
  shape.mips = desc.MipLevels ? desc.MipLevels : 1;
  shape.format = static_cast<uint32_t>(desc.Format);
  return shape;
}

Key key_of(uint64_t address, const lib::TextureShape& shape) {
  return Key{address, shape.dimension, shape.width, shape.height, shape.depth_or_array, shape.mips, shape.format};
}

}  // namespace

ID3D12Resource* placed_texture(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address,
                               UINT initial_layout, const D3D12_CLEAR_VALUE* clear) {
  const lib::TextureShape shape = shape_of(desc);
  const Key key = key_of(address, shape);
  D3D12_RESOURCE_DESC host_desc = desktop_resource_desc(desc);
  host_desc.Alignment = 0;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* texture = nullptr;
  const HRESULT hr = create_host_committed(device, heap, D3D12_HEAP_FLAG_NONE, host_desc, initial_layout, clear,
                                           &texture);
  LIB_LOG(hr < 0 ? lib::kError : lib::kDebug, "d3d12",
          "placed texture dim %d %llux%ux%u, %u mips, format %u, flags 0x%x at %p -> 0x%08lX", desc.Dimension,
          static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize, desc.MipLevels,
          desc.Format, desc.Flags, reinterpret_cast<void*>(address), static_cast<unsigned long>(hr));
  if (FAILED(hr)) return nullptr;
  std::lock_guard<std::mutex> guard(g_lock);
  auto found = textures().find(key);
  if (found != textures().end()) {
    found->second->Release();
    found->second = texture;
  } else {
    textures().emplace(key, texture);
  }
  texture->AddRef();
  return texture;
}

ID3D12Resource* texture_at(Device* device, const D3D12_RESOURCE_DESC& desc, uint64_t address) {
  const lib::TextureShape shape = shape_of(desc);
  const Key key = key_of(address, shape);

  std::lock_guard<std::mutex> guard(g_lock);
  auto found = textures().find(key);
  if (found != textures().end()) return found->second;

  const uint64_t size = lib::texture_size(shape);
  Backing backing;
  if (!size || !locate(device, address, &backing) || !backing.whole || backing.contiguous < size) {
    LIB_LOG(lib::kError, "d3d12",
            "texture dim %u %llux%ux%u, %u mips, format %u at %p (0x%llx bytes) lies in no contiguous GPU memory",
            shape.dimension, static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array,
            shape.mips, shape.format, reinterpret_cast<void*>(address), static_cast<unsigned long long>(size));
    return nullptr;
  }
  Uploader* up = uploader(device);
  if (!up) return nullptr;

  D3D12_RESOURCE_DESC host_desc = desc;
  host_desc.Alignment = 0;
  host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  host_desc.MipLevels = static_cast<UINT16>(shape.mips);
  host_desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
                     D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ID3D12Resource* texture = nullptr;
  HRESULT hr = device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &host_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "host texture of format %u %llux%u refused (0x%08lX)", shape.format,
            static_cast<unsigned long long>(shape.width), shape.height, static_cast<unsigned long>(hr));
    return nullptr;
  }

  up->allocator->Reset();
  up->list->Reset(up->allocator, nullptr);
  const uint32_t count = lib::subresource_count(shape);
  for (uint32_t index = 0; index < count; ++index) {
    lib::SubresourceLayout layout;
    lib::subresource_layout(shape, index, &layout);
    const uint32_t mip = index % shape.mips;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backing.whole;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = backing.heap_offset + layout.offset;
    source.PlacedFootprint.Footprint.Format = desc.Format;
    source.PlacedFootprint.Footprint.Width = static_cast<UINT>((shape.width >> mip) ? (shape.width >> mip) : 1);
    source.PlacedFootprint.Footprint.Height = (shape.height >> mip) ? (shape.height >> mip) : 1;
    source.PlacedFootprint.Footprint.Depth = layout.depth;
    source.PlacedFootprint.Footprint.RowPitch = layout.row_pitch;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = texture;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = index;
    up->list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = texture;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  up->list->ResourceBarrier(1, &barrier);
  up->list->Close();
  ID3D12CommandList* lists[] = {up->list};
  up->queue->ExecuteCommandLists(1, lists);
  up->queue->Signal(up->fence, ++up->value);
  up->fence->SetEventOnCompletion(up->value, up->event);
  WaitForSingleObject(up->event, INFINITE);

  LIB_LOG(lib::kDebug, "d3d12", "host texture for format %u %llux%ux%u, %u mips at %p", shape.format,
          static_cast<unsigned long long>(shape.width), shape.height, shape.depth_or_array, shape.mips,
          reinterpret_cast<void*>(address));
  textures().emplace(key, texture);
  return texture;
}

}  // namespace holy::d3d12
