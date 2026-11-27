// Root arguments and translated pipelines.
//
// Observed, from native code and the root signatures it was built for:
// - graphics stages read their root arguments from user SGPRs starting at s8,
//   packed as in RootLayout (a descriptor table as the low half of its GPU
//   address; native code sets the high half to zero), followed by the base of
//   the bound sampler heap (code indexes it with sampler indices times 16);
// - vertex shaders read 16-byte buffer descriptors of the bound vertex
//   buffers from a table whose address is in s[0:1] (40 bits; the high
//   dword's bits 8-31 hold other data), add s2 to the vertex index (the base
//   vertex of indexed draws) and have the start instance in s3 (assumed);
// - compute programs read their root arguments from memory at s[0:1], packed
//   the same way.
//
// Host model, per draw or dispatch of a translated pipeline: the system
// constants the translation reads (user SGPRs) and the vertex buffer table
// are written into an upload ring the GPU reads from. Title memory is reached
// through the memory map (gpu_memory.h): the slot views in the reserved block
// of the bound shader-visible heap and the page table, both the same for
// every call. Memory the call names (buffers of the bound descriptor tables
// and root views, vertex buffers) is located first if the page table does not
// have it yet; descriptor heaps' GPU ranges are entered when the heaps are
// made, the ring once.
//
// Partial: memory reached only through addresses held in memory (pointers
// in constant data, descriptors outside the bound tables) that nothing has
// located reads as zero. The ring is not fenced: it assumes the GPU is fewer
// than about a thousand draws behind.
#include "core/libs/d3d12_xs/bindings.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/shader/descriptor_layout.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

std::mutex g_layout_lock;
std::map<const void*, RootLayout>& layouts() {
  static auto* map = new std::map<const void*, RootLayout>();
  return *map;
}

// --- upload ring ---------------------------------------------------------------------

constexpr size_t kRingBytes = 64ull << 20;

struct Ring {
  uint8_t* base = nullptr;
  std::atomic<uint64_t> next{0};
};

Ring* ring() {
  static Ring* instance = [] {
    auto* made = new Ring();
    // Below 2^40: vertex shaders take the vertex buffer table's address with
    // only 8 bits of its high dword.
    for (uint64_t at = 0x0000008000000000ull; at >= 0x0000001000000000ull && !made->base; at >>= 1) {
      made->base = static_cast<uint8_t*>(
          VirtualAlloc(reinterpret_cast<void*>(at), kRingBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!made->base) LIB_LOG(lib::kError, "d3d12", "no upload ring below 2^40");
    return made;
  }();
  return instance;
}

// `bytes` of ring memory aligned to 256, or null.
uint8_t* ring_alloc(size_t bytes) {
  Ring* r = ring();
  if (!r->base) return nullptr;
  bytes = (bytes + 255) & ~size_t{255};
  for (;;) {
    uint64_t at = r->next.fetch_add(bytes);
    if (at + bytes <= kRingBytes) return r->base + at;
    uint64_t expected = at + bytes;
    r->next.compare_exchange_strong(expected, 0);  // wrap (one thread wins)
  }
}

// --- reach --------------------------------------------------------------------------------

// Buffers named by the descriptors of a table (from its title GPU address).
void reach_table(Device* device, uint64_t table, uint32_t descriptors) {
  DescriptorLocation where;
  if (!table || !find_gpu_descriptor(table, &where)) return;
  const uint32_t available = where.heap->host_count - where.index;
  const uint32_t count = descriptors < available ? descriptors : available;
  for (uint32_t i = 0; i < count; ++i) {
    const auto* words = reinterpret_cast<const uint32_t*>(where.title + static_cast<uint64_t>(i) * where.heap->stride);
    if (words[1] == kDescriptorMarker) continue;
    const uint64_t base = words[0] | (static_cast<uint64_t>(words[1] & 0xFFFF) << 32);
    if (base) reach(device, base);
  }
}

void reach_root(Device* device, const RootBinding& binding) {
  const RootLayout* layout = root_layout(binding.signature);
  if (!layout) return;
  for (size_t i = 0; i < layout->parameters.size() && i < 64; ++i) {
    const RootParameterLayout& p = layout->parameters[i];
    if (p.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      reach_table(device, binding.values[i], p.descriptors);
    } else if (p.type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      reach(device, binding.values[i]);
    }
  }
}

// The ring in the memory map (once).
void reach_ring(Device* device) {
  static std::once_flag once;
  std::call_once(once, [device] {
    Ring* r = ring();
    Backing backing;
    if (!r->base || !locate(device, reinterpret_cast<uint64_t>(r->base), &backing, kRingBytes) || !backing.slot) {
      LIB_LOG(lib::kError, "d3d12", "the upload ring is not reachable by translated programs");
    }
  });
}

constexpr UINT kReservedViews = kReservedHostViews;

// The system constants of shader/translate.h, written into the ring.
uint64_t write_system(Device* device, const uint32_t (&user)[16], const NggPass* ngg = nullptr) {
  uint8_t* block = ring_alloc(kSystemVectors * 16);
  if (!block) return 0;
  auto* v = reinterpret_cast<uint32_t*>(block);
  std::memset(v, 0, kSystemVectors * 16);
  std::memcpy(&v[4 * kSystemUserVector], user, sizeof(user));
  if (ngg) {
    v[4 * kSystemNggVector + 0] = ngg->index_offset;
    v[4 * kSystemNggVector + 1] = ngg->groups;
    v[4 * kSystemNggVector + 2] = ngg->first_vertex;
    v[4 * kSystemNggVector + 3] = ngg->first_instance;
  }
  return translate_gpu_va(device, reinterpret_cast<uint64_t>(block));
}

// User SGPRs from s8: the packed root arguments, then the sampler heap base.
void pack_root(const CommandList* list, const RootBinding& binding, uint32_t* user, uint32_t first, uint32_t limit) {
  const RootLayout* layout = root_layout(binding.signature);
  uint32_t at = first;
  if (layout) {
    for (uint32_t i = 0; i < layout->dwords && at < limit; ++i) user[at++] = binding.packed[i];
  }
  if (at < limit && list->heaps[1]) user[at] = static_cast<uint32_t>(list->heaps[1]->title_gpu);
}

// A shader-visible heap of the runtime's own holding only the memory slot
// views, for translated calls of lists the title bound no CBV/SRV/UAV heap to
// (observed in no title yet; drawing without descriptors needs none).
DescriptorHeap* memory_heap(Device* device) {
  static DescriptorHeap* heap = [device] {
    const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    void* made = nullptr;
    if (FAILED(create_descriptor_heap(device, &desc, &made))) {
      LIB_LOG(lib::kError, "d3d12", "no heap of memory views for translated calls without a bound heap");
    }
    return static_cast<DescriptorHeap*>(made);
  }();
  return heap;
}

// The heap whose memory slot views a translated call uses: the bound
// CBV/SRV/UAV heap, or the runtime's own (bound on the host for the call).
const DescriptorHeap* views_heap(CommandList* list) {
  if (list->heaps[0] && list->heaps[0]->host_count > kReservedViews) return list->heaps[0];
  DescriptorHeap* own = memory_heap(list->device);
  if (!own) return nullptr;
  if (!list->memory_heap_bound) {
    ID3D12DescriptorHeap* hosts[2] = {own->host, list->heaps[1] ? list->heaps[1]->host : nullptr};
    list->host->SetDescriptorHeaps(list->heaps[1] ? 2 : 1, hosts);
    list->memory_heap_bound = true;
  }
  return own;
}

void bind_common(CommandList* list, bool compute, uint64_t page_table, uint64_t system) {
  ID3D12GraphicsCommandList* host = list->host;
  ID3D12RootSignature* root = translated_root_signature(list->device);
  RootBinding& binding = compute ? list->compute : list->graphics;
  const D3D12_GPU_DESCRIPTOR_HANDLE views{memory_views(views_heap(list))};
  if (compute) {
    host->SetComputeRootSignature(root);
    host->SetComputeRootDescriptorTable(kRootMemory, views);
    host->SetComputeRootShaderResourceView(kRootPageTable, page_table);
    host->SetComputeRootConstantBufferView(kRootSystem, system);
    if (list->heaps[0]) host->SetComputeRootDescriptorTable(kRootImages, {list->heaps[0]->host_gpu});
    if (list->heaps[1]) host->SetComputeRootDescriptorTable(kRootSamplers, {list->heaps[1]->host_gpu});
  } else {
    host->SetGraphicsRootSignature(root);
    host->SetGraphicsRootDescriptorTable(kRootMemory, views);
    host->SetGraphicsRootShaderResourceView(kRootPageTable, page_table);
    host->SetGraphicsRootConstantBufferView(kRootSystem, system);
    if (list->heaps[0]) host->SetGraphicsRootDescriptorTable(kRootImages, {list->heaps[0]->host_gpu});
    if (list->heaps[1]) host->SetGraphicsRootDescriptorTable(kRootSamplers, {list->heaps[1]->host_gpu});
  }
  binding.host_is_title = false;
}

}  // namespace

void register_root_layout(const void* signature, const D3D12_ROOT_SIGNATURE_DESC1& desc) {
  RootLayout layout;
  for (UINT i = 0; i < desc.NumParameters; ++i) {
    const D3D12_ROOT_PARAMETER1& p = desc.pParameters[i];
    RootParameterLayout entry{p.ParameterType, layout.dwords, 0, 0};
    switch (p.ParameterType) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        entry.dwords = 1;
        for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
          const D3D12_DESCRIPTOR_RANGE1& range = p.DescriptorTable.pDescriptorRanges[r];
          const uint32_t n = range.NumDescriptors == UINT_MAX ? 64 : range.NumDescriptors;
          const uint32_t end = (range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                    ? entry.descriptors
                                    : range.OffsetInDescriptorsFromTableStart) + n;
          if (end > entry.descriptors) entry.descriptors = end;
        }
        if (entry.descriptors > 256) entry.descriptors = 256;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: entry.dwords = p.Constants.Num32BitValues; break;
      default: entry.dwords = 2; break;
    }
    layout.dwords += entry.dwords;
    layout.parameters.push_back(entry);
  }
  std::lock_guard<std::mutex> guard(g_layout_lock);
  layouts()[signature] = std::move(layout);
}

const RootLayout* root_layout(const void* signature) {
  std::lock_guard<std::mutex> guard(g_layout_lock);
  auto found = layouts().find(signature);
  return found == layouts().end() ? nullptr : &found->second;
}

bool bind_translated_graphics(CommandList* list, uint32_t base_vertex, uint32_t start_instance, const NggPass* ngg) {
  Device* device = list->device;
  if (!views_heap(list)) return false;
  const uint64_t page_table = page_table_address(device);
  if (!page_table) return false;
  reach_ring(device);
  reach_root(device, list->graphics);
  // Vertex buffer table: RDNA 2 buffer descriptors (base, stride, records,
  // identity component selection).
  uint8_t* table = ring_alloc(16 * 16);
  if (!table) return false;
  auto* vt = reinterpret_cast<uint32_t*>(table);
  std::memset(vt, 0, 16 * 16);
  for (int i = 0; i < 16; ++i) {
    const D3D12_VERTEX_BUFFER_VIEW& v = list->vertex_buffers[i];
    if (!v.BufferLocation) continue;
    vt[4 * i + 0] = static_cast<uint32_t>(v.BufferLocation);
    vt[4 * i + 1] = static_cast<uint32_t>((v.BufferLocation >> 32) & 0xFFFF) | ((v.StrideInBytes & 0x3FFF) << 16);
    vt[4 * i + 2] = v.StrideInBytes ? v.SizeInBytes / v.StrideInBytes : v.SizeInBytes;
    vt[4 * i + 3] = 4u | (5u << 3) | (6u << 6) | (7u << 9);
    reach(device, v.BufferLocation);
  }
  uint32_t user[16] = {};
  const uint64_t table_address = reinterpret_cast<uint64_t>(table);
  user[0] = static_cast<uint32_t>(table_address);
  user[1] = static_cast<uint32_t>(table_address >> 32);
  user[2] = base_vertex;
  user[3] = start_instance;
  pack_root(list, list->graphics, user, 8, 16);
  const uint64_t system = write_system(device, user, ngg);
  if (!system) return false;
  // An NGG draw's compute pass reads the graphics bindings (it is the
  // console's vertex stage) and writes the draw's records.
  bind_common(list, ngg != nullptr, page_table, system);
  if (ngg) list->host->SetComputeRootUnorderedAccessView(kRootNggOutput, ngg->records);
  return true;
}

bool bind_translated_compute(CommandList* list) {
  Device* device = list->device;
  if (!views_heap(list)) return false;
  const uint64_t page_table = page_table_address(device);
  if (!page_table) return false;
  reach_ring(device);
  reach_root(device, list->compute);
  // The root table: the packed arguments, then the sampler heap base.
  uint8_t* table = ring_alloc(4 * 66);
  if (!table) return false;
  auto* root = reinterpret_cast<uint32_t*>(table);
  std::memset(root, 0, 4 * 66);
  pack_root(list, list->compute, root, 0, 66);
  uint32_t user[16] = {};
  const uint64_t table_address = reinterpret_cast<uint64_t>(table);
  user[0] = static_cast<uint32_t>(table_address);
  user[1] = static_cast<uint32_t>(table_address >> 32);
  const uint64_t system = write_system(device, user);
  if (!system) return false;
  bind_common(list, true, page_table, system);
  return true;
}

void bind_title_root(CommandList* list, bool compute) {
  RootBinding& binding = compute ? list->compute : list->graphics;
  if (binding.host_is_title || !binding.signature) return;
  ID3D12GraphicsCommandList* host = list->host;
  auto* root = host_of<ID3D12RootSignature>(binding.signature);
  if (compute) {
    host->SetComputeRootSignature(root);
  } else {
    host->SetGraphicsRootSignature(root);
  }
  binding.host_is_title = true;
  const RootLayout* layout = root_layout(binding.signature);
  if (!layout) return;
  for (UINT i = 0; i < layout->parameters.size() && i < 64; ++i) {
    const RootParameterLayout& p = layout->parameters[i];
    const uint64_t value = binding.values[i];
    switch (p.type) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        const D3D12_GPU_DESCRIPTOR_HANDLE handle = host_gpu_handle(value);
        if (!handle.ptr) break;
        compute ? host->SetComputeRootDescriptorTable(i, handle) : host->SetGraphicsRootDescriptorTable(i, handle);
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        compute ? host->SetComputeRoot32BitConstants(i, p.dwords, binding.packed + p.offset, 0)
                : host->SetGraphicsRoot32BitConstants(i, p.dwords, binding.packed + p.offset, 0);
        break;
      default: {
        const uint64_t address = translate_gpu_va(list->device, value);
        if (!address) break;
        if (p.type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
          compute ? host->SetComputeRootConstantBufferView(i, address) : host->SetGraphicsRootConstantBufferView(i, address);
        } else if (p.type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
          compute ? host->SetComputeRootShaderResourceView(i, address) : host->SetGraphicsRootShaderResourceView(i, address);
        } else {
          compute ? host->SetComputeRootUnorderedAccessView(i, address)
                  : host->SetGraphicsRootUnorderedAccessView(i, address);
        }
        break;
      }
    }
  }
}

}  // namespace holy::d3d12
