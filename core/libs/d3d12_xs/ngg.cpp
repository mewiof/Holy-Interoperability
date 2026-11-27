#include "core/libs/d3d12_xs/ngg.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "core/libs/d3d12_xs/bindings.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/pipeline_stream.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

std::mutex g_lock;
std::map<const void*, NggPipeline>& pipelines() {
  static auto* map = new std::map<const void*, NggPipeline>();
  return *map;
}

// Scratch memory: GPU buffers handed to host lists in blocks; a list's blocks
// return to the pool when the list is reset (its submissions done).
constexpr uint64_t kBlockBytes = 64ull << 20;

struct NggBlock {
  ID3D12Resource* buffer = nullptr;
  uint64_t va = 0;
  uint64_t size = 0;
  uint64_t used = 0;
};

std::mutex g_pool_lock;
std::vector<NggBlock*>& pool() {
  static auto* blocks = new std::vector<NggBlock*>();
  return *blocks;
}

NggBlock* new_block(Device* device, uint64_t bytes) {
  const uint64_t size = bytes > kBlockBytes ? (bytes + 0xFFFF) & ~0xFFFFull : kBlockBytes;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* buffer = nullptr;
  const HRESULT hr = device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "NGG scratch of %llu MiB refused (0x%08lX)",
            static_cast<unsigned long long>(size >> 20), static_cast<unsigned long>(hr));
    return nullptr;
  }
  auto* block = new NggBlock();
  block->buffer = buffer;
  block->va = buffer->GetGPUVirtualAddress();
  block->size = size;
  return block;
}

// `bytes` of scratch for a draw recorded in `host`: the GPU address.
uint64_t scratch(Device* device, HostList* host, uint64_t bytes) {
  bytes = (bytes + 255) & ~255ull;
  if (!host->ngg_blocks.empty()) {
    auto* last = static_cast<NggBlock*>(host->ngg_blocks.back());
    if (last->used + bytes <= last->size) {
      const uint64_t at = last->va + last->used;
      last->used += bytes;
      return at;
    }
  }
  NggBlock* block = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_pool_lock);
    std::vector<NggBlock*>& blocks = pool();
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i]->size >= bytes) {
        block = blocks[i];
        blocks[i] = blocks.back();
        blocks.pop_back();
        break;
      }
    }
  }
  if (!block) block = new_block(device, bytes);
  if (!block) return 0;
  block->used = bytes;
  host->ngg_blocks.push_back(block);
  return block->va;
}

}  // namespace

void register_ngg_pipeline(const void* graphics, void* compute, const NggInfo& info) {
  std::lock_guard<std::mutex> guard(g_lock);
  pipelines()[graphics] = NggPipeline{compute, info};
}

const NggPipeline* ngg_pipeline(const void* graphics) {
  if (!graphics) return nullptr;
  std::lock_guard<std::mutex> guard(g_lock);
  auto found = pipelines().find(graphics);
  return found == pipelines().end() ? nullptr : &found->second;
}

void release_ngg_scratch(HostList* host) {
  if (host->ngg_blocks.empty()) return;
  std::lock_guard<std::mutex> guard(g_pool_lock);
  for (void* block : host->ngg_blocks) pool().push_back(static_cast<NggBlock*>(block));
  host->ngg_blocks.clear();
}

bool draw_ngg(CommandList* list, const NggPipeline& ngg, uint32_t vertices, uint32_t instances,
              uint32_t first_vertex, uint32_t first_instance) {
  const NggInfo& info = ngg.info;
  if (!info.group || !info.stride || !vertices || !instances) return true;
  ID3D12PipelineState* compute = pipeline_host(ngg.compute);
  ID3D12PipelineState* graphics = pipeline_host(list->pipeline);
  if (!compute || !graphics || !list->host7) return false;
  const uint64_t groups = (static_cast<uint64_t>(vertices) + info.group - 1) / info.group;
  const uint64_t slots = groups * instances * info.group;
  const uint64_t vertex_bytes = slots * info.stride;
  const uint64_t index_bytes = slots * 12;
  if (vertex_bytes + index_bytes > 0xFFFFFFFFull || groups > 65535 || instances > 65535) {
    LIB_LOG(lib::kError, "d3d12", "NGG draw of %u vertices x %u instances is too large", vertices, instances);
    return false;
  }
  const uint64_t records = scratch(list->device, list->current, vertex_bytes + index_bytes);
  if (!records) return false;
  ID3D12GraphicsCommandList* host = list->host;
  auto* block = static_cast<NggBlock*>(list->current->ngg_blocks.back());
  D3D12_BUFFER_BARRIER barrier{};
  barrier.pResource = block->buffer;
  barrier.Offset = 0;
  barrier.Size = UINT64_MAX;
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_BUFFER;
  group.NumBarriers = 1;
  group.pBufferBarriers = &barrier;
  if (block->used > ((vertex_bytes + index_bytes + 255) & ~255ull)) {
    // Earlier draws of this list read the block: their reads come first.
    barrier.SyncBefore = D3D12_BARRIER_SYNC_INDEX_INPUT | D3D12_BARRIER_SYNC_VERTEX_SHADING;
    barrier.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    barrier.AccessBefore = D3D12_BARRIER_ACCESS_INDEX_BUFFER | D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    barrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    list->host7->Barrier(1, &group);
  }

  // Compute pass: the records.
  NggPass pass;
  pass.index_offset = static_cast<uint32_t>(vertex_bytes);
  pass.groups = static_cast<uint32_t>(groups);
  pass.first_vertex = first_vertex;
  pass.first_instance = first_instance;
  pass.records = records;
  if (!bind_translated_graphics(list, 0, first_instance, &pass)) return false;
  host->SetPipelineState(compute);
  host->Dispatch(static_cast<UINT>(groups), instances, 1);

  // The records become indices and vertex data.
  barrier.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
  barrier.SyncAfter = D3D12_BARRIER_SYNC_INDEX_INPUT | D3D12_BARRIER_SYNC_VERTEX_SHADING;
  barrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
  barrier.AccessAfter = D3D12_BARRIER_ACCESS_INDEX_BUFFER | D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
  list->host7->Barrier(1, &group);

  // Draw pass.
  if (!bind_translated_graphics(list, 0, first_instance)) return false;
  host->SetGraphicsRootShaderResourceView(kRootNggInput, records);
  host->SetPipelineState(graphics);
  const D3D12_INDEX_BUFFER_VIEW indices{records + vertex_bytes, static_cast<UINT>(index_bytes), DXGI_FORMAT_R32_UINT};
  host->IASetIndexBuffer(&indices);
  host->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  host->DrawIndexedInstanced(static_cast<UINT>(slots * 3), 1, 0, 0, 0);

  // The title's input assembly state.
  host->IASetIndexBuffer(list->index_buffer_set ? &list->index_buffer : nullptr);
  host->IASetPrimitiveTopology(list->topology);
  return true;
}

}  // namespace holy::d3d12
