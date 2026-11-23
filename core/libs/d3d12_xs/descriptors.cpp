// Descriptor heaps.
//
// Observed: device slot 14 is CreateDescriptorHeap(desc, riid, out), as on the
// desktop, and heaps keep the desktop order (8 GetDesc, 9 CPU start, 10 GPU
// start). Before creating a heap a title may register an allocation hook that
// answers an XMemAlloc of object type 4 and the heap's byte size (descriptor
// count times the increment size) with an address of its choosing, and it
// then requires the heap's CPU start to be that address. So the console
// runtime allocates a heap's CPU-side descriptor memory with XMemAlloc, object
// type 4. Observed: the title's hook selects callbacks by allocator id (bits
// 16-23) and the preset is registered for allocator id 0x80.
//
// Observed: native code reads descriptors from memory in 32-byte slots
// (descriptor tables at consecutive 0x20 offsets).
//
// Host model: the heap's CPU memory comes from XMemAlloc in the same way and
// CPU handles are addresses in it, 32 bytes per descriptor whatever the
// host's increment; each heap is backed by a host heap of the same type and
// size, and every API call taking a handle translates it to the host heap by
// index. A shader-visible heap's GPU start, as titles see it, is the same
// address (console GPU addresses are CPU addresses): GPU handles translate
// back to the host heap, and translated native code reads the descriptors
// the view functions write into that memory (descriptor_memory.h).
#include "core/libs/d3d12_xs/descriptors.h"

#include <map>
#include <mutex>

#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/support.h"

extern "C" __declspec(dllimport) void* XMemAlloc(SIZE_T size, uint64_t attributes);
extern "C" __declspec(dllimport) void XMemFree(void* address, uint64_t attributes);

namespace holy::d3d12 {
namespace {

// Object type 4 (bits 0-13), allocator id 0x80 (bits 16-23): titles register
// their preset-address hook for that allocator id. 64 KiB aligned.
constexpr uint64_t kDescriptorHeapAttributes = 4 | (0x80ull << 16) | (16ull << 24);
// Observed: titles ask for shader-visible CBV/SRV/UAV heaps larger than the
// host allows (1,245,184 descriptors against the desktop limit of 1,000,000).
constexpr UINT kHostShaderVisibleLimit = 1000000;

const lib::VtableInfo kHeapInfo{kLibrary, "ID3D12DescriptorHeapX"};

std::mutex g_lock;
std::map<uint64_t, DescriptorHeap*>& heaps_by_end() {
  static auto* storage = new std::map<uint64_t, DescriptorHeap*>();
  return *storage;
}

HRESULT STDMETHODCALLTYPE heap_query_interface(DescriptorHeap* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE heap_add_ref(DescriptorHeap* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE heap_release(DescriptorHeap* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    {
      std::lock_guard<std::mutex> lock(g_lock);
      heaps_by_end().erase(self->title_cpu + self->title_bytes);
    }
    self->host->Release();
    XMemFree(reinterpret_cast<void*>(self->title_cpu), kDescriptorHeapAttributes);
    delete self;
  }
  return static_cast<ULONG>(left);
}

// GetDesc returns a 16-byte structure: result pointer first.
D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE heap_get_desc(D3D12_DESCRIPTOR_HEAP_DESC* result,
                                                            DescriptorHeap* self) {
  *result = self->desc;
  return result;
}
// Eight-byte structures come back in rax.
uint64_t STDMETHODCALLTYPE heap_get_cpu_start(DescriptorHeap* self) {
  return self->title_cpu;
}
uint64_t STDMETHODCALLTYPE heap_get_gpu_start(DescriptorHeap* self) {
  return self->host_gpu ? self->title_cpu : 0;
}

const auto kHeapVtable = [] {
  auto table = lib::make_reporting_vtable<32>();
  table[0] = reinterpret_cast<void*>(&heap_query_interface);
  table[1] = reinterpret_cast<void*>(&heap_add_ref);
  table[2] = reinterpret_cast<void*>(&heap_release);
  table[8] = reinterpret_cast<void*>(&heap_get_desc);
  table[9] = reinterpret_cast<void*>(&heap_get_cpu_start);
  table[10] = reinterpret_cast<void*>(&heap_get_gpu_start);
  return table;
}();

}  // namespace

HRESULT create_descriptor_heap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC host_desc = *desc;
  host_desc.NodeMask = 0;
  if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
      (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && desc->NumDescriptors > kHostShaderVisibleLimit) {
    // Partial: created at the host's limit; descriptors past it are refused.
    host_desc.NumDescriptors = kHostShaderVisibleLimit;
    LIB_LOG(lib::kWarn, "d3d12", "shader-visible heap of %u descriptors created with the host limit of %u",
            desc->NumDescriptors, kHostShaderVisibleLimit);
  }
  ID3D12DescriptorHeap* host = nullptr;
  const HRESULT hr = device->host->CreateDescriptorHeap(&host_desc, IID_PPV_ARGS(&host));
  LIB_LOG(lib::kDebug, "d3d12", "descriptor heap type %d count %u flags 0x%x -> 0x%08lX", desc->Type,
          desc->NumDescriptors, desc->Flags, static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;

  const UINT increment = device->host->GetDescriptorHandleIncrementSize(desc->Type);
  const uint64_t bytes = static_cast<uint64_t>(desc->NumDescriptors) * kTitleDescriptorBytes;
  void* memory = XMemAlloc(static_cast<SIZE_T>(bytes), kDescriptorHeapAttributes);
  if (!memory) {
    host->Release();
    return E_OUTOFMEMORY;
  }
  auto* heap = new DescriptorHeap();
  heap->vtable = kHeapVtable.data();
  heap->info = &kHeapInfo;
  heap->host = host;
  heap->desc = *desc;
  heap->increment = increment;
  heap->host_count = host_desc.NumDescriptors;
  heap->title_cpu = reinterpret_cast<uint64_t>(memory);
  heap->title_bytes = bytes;
  heap->host_cpu = host->GetCPUDescriptorHandleForHeapStart().ptr;
  if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
    heap->host_gpu = host->GetGPUDescriptorHandleForHeapStart().ptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_lock);
    heaps_by_end()[heap->title_cpu + bytes] = heap;
  }
  *out = heap;
  return S_OK;
}

bool find_descriptor(uint64_t title_handle, DescriptorLocation* out) {
  std::lock_guard<std::mutex> lock(g_lock);
  auto it = heaps_by_end().upper_bound(title_handle);
  if (it == heaps_by_end().end() || title_handle < it->second->title_cpu) return false;
  DescriptorHeap* heap = it->second;
  const uint64_t index = (title_handle - heap->title_cpu) / kTitleDescriptorBytes;
  if (index >= heap->host_count) {
    LIB_LOG(lib::kError, "d3d12", "descriptor %llu is past the host heap's %u descriptors",
            static_cast<unsigned long long>(index), heap->host_count);
    return false;
  }
  out->heap = heap;
  out->index = static_cast<uint32_t>(index);
  out->title = heap->title_cpu + index * kTitleDescriptorBytes;
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE host_cpu_handle(uint64_t title_handle) {
  DescriptorLocation where;
  if (!find_descriptor(title_handle, &where)) return {0};
  return {static_cast<SIZE_T>(where.heap->host_cpu + static_cast<uint64_t>(where.index) * where.heap->increment)};
}

D3D12_GPU_DESCRIPTOR_HANDLE host_gpu_handle(uint64_t title_handle) {
  DescriptorLocation where;
  if (!find_descriptor(title_handle, &where) || !where.heap->host_gpu) return {0};
  return {where.heap->host_gpu + static_cast<uint64_t>(where.index) * where.heap->increment};
}

ID3D12DescriptorHeap* host_heap(const void* heap) {
  return heap ? static_cast<const DescriptorHeap*>(heap)->host : nullptr;
}

}  // namespace holy::d3d12
