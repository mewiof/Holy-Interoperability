// The console D3D12 device, backed by a host Direct3D 12 device.
//
// Observed: D3D12XboxCreateDevice(adapter, const parameters*, REFIID, void**)
// is called with a null adapter, a 0x40-byte parameter block whose first
// dword is 0x20004 (a version), and the console device's interface id
// {5405c344-d457-444e-b4dd-2366e45aee39}. The device's vtable only partly
// follows the desktop ID3D12Device order (slot 8 creates a command queue as on
// the desktop; slot 48 also creates one, unlike the desktop), so every slot is
// established from the title's calls; unbound slots report themselves.
//
// Observed calling convention: the C one for interface methods. Structures of
// up to eight bytes come back in rax; larger ones through a result pointer
// passed first, with the object second. All other methods take the object
// first.
#include "core/libs/d3d12_xs/device.h"

#include <d3d12.h>

#include <intrin.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/frame_pacing.h"
#include "core/libs/d3d12_xs/present.h"
#include "core/libs/d3d12_xs/ngg.h"
#include "core/libs/d3d12_xs/page_cache.h"
#include "core/libs/d3d12_xs/pipeline_stream.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/d3d12_xs/shader_container.h"
#include "core/libs/d3d12_xs/shader_dump.h"
#include "core/libs/d3d12_xs/translation_cache.h"
#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/views.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

// The title's call into a device method wrapped for exploration (`explored`
// below) or checking (call_check.h), so reports name the title's code rather
// than the wrapper.
#define CALLER() (t_api_caller ? t_api_caller : _ReturnAddress())

const lib::VtableInfo kDeviceInfo{kLibrary, "ID3D12DeviceX"};

// {5405c344-d457-444e-b4dd-2366e45aee39}, observed as the device's id.
constexpr GUID kDeviceIid = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

// Only the device's own interface is provided. Observed: titles also ask the
// device for IDXGIDevice and walk adapter and outputs through it; the console
// DXGI objects do not keep the desktop method order (an output's display
// mode list is at slot 21, with 32-byte entries), so they are not provided
// yet and the query reports itself.
HRESULT STDMETHODCALLTYPE device_query_interface(Device* self, REFIID iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  // {54ec77fa-1377-44e6-8c32-88fd5f44c84c}
  constexpr GUID kDxgiDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
  if (IsEqualGUID(iid, kDxgiDevice)) {
    *out = dxgi_device();
    return S_OK;
  }
  if (IsEqualGUID(iid, kDeviceIid) || IsEqualGUID(iid, __uuidof(IUnknown))) {
    self->refs.fetch_add(1);
    *out = self;
    return S_OK;
  }
  char symbol[96];
  std::snprintf(symbol, sizeof(symbol), "ID3D12DeviceX::QueryInterface(%08lx-%04x-%04x)", iid.Data1, iid.Data2,
                iid.Data3);
  HolyUnimplementedFrom(kLibrary, symbol, CALLER());
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE device_add_ref(Device* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE device_release(Device* self) {
  // The device lives as long as the process: titles keep raw pointers to it
  // in many places, and the console never destroys it during a run.
  return static_cast<ULONG>(self->refs.fetch_sub(1) - 1);
}

// Slot 26 (observed: called with node mask 1 and heap types 1, 2 and 3):
// GetCustomHeapProperties, as on the desktop. Structure returns take the
// object and the result pointer in either order (lib::struct_return_arguments).
D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE get_custom_heap_properties(void* first, void* second, UINT node_mask,
                                                                   D3D12_HEAP_TYPE type) {
  Device* self;
  D3D12_HEAP_PROPERTIES* result;
  lib::struct_return_arguments(first, second, &kDeviceInfo, &self, &result);
  *result = self->host->GetCustomHeapProperties(node_mask, type);
  return result;
}

HRESULT STDMETHODCALLTYPE create_queue(Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, void** queue) {
  return create_command_queue(self, desc, false, queue);
}

HRESULT STDMETHODCALLTYPE create_queue_x(Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, void** queue) {
  return create_command_queue(self, desc, true, queue);
}

// --- fences --------------------------------------------------------------------
//
// Observed: device slot 36 is called as CreateFence(initial value, flags,
// riid, out) with the desktop ID3D12Fence interface id; fences keep the
// desktop ID3D12Fence1 method order and forward to host fences. Flags are not
// passed on (titles ask for shared fences, which a single process does not
// need).
const lib::VtableInfo kFenceInfo{kLibrary, "ID3D12FenceX"};
Device* g_device = nullptr;

}  // namespace

ID3D12Device* host_device() { return g_device ? g_device->host : nullptr; }

namespace {

HRESULT STDMETHODCALLTYPE get_device_of_child(Wrapper*, REFIID, void** device) {
  if (!device) return E_POINTER;
  g_device->refs.fetch_add(1);
  *device = g_device;
  return S_OK;
}

// Observed: fence slot 12 (past the desktop ID3D12Fence1 methods) returns a
// pointer through which titles read the fence's current value directly,
// treating all ones as a removed device; titles read memory the GPU wrote once
// the value shows the work done. Host model: a cell per fence, owned by the
// fence and gone with its last reference (a fence made later at the same
// address has a cell of its own), kept at the host fence's completed value by
// one thread for all cells.
struct FenceCell {
  ID3D12Fence* fence = nullptr;  // the fence's host fence (the cell goes first)
  volatile uint64_t value = 0;
  uint64_t armed = 0;  // the value the watcher's event is set to come at (0: none)
};

struct Fence : Wrapper {
  std::atomic<FenceCell*> cell{nullptr};
};

// The thread keeping cells current: woken by the next completion of any
// watched fence (one event, set by each fence at its next value), it reads
// every cell's fence and sets each event again where the value moved.
class FenceWatcher {
 public:
  static FenceWatcher& get() {
    static auto* watcher = new FenceWatcher();
    return *watcher;
  }

  void add(FenceCell* cell) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!wake_) {
      wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      const HANDLE thread = wake_ ? CreateThread(nullptr, 64 * 1024, &run, this, 0, nullptr) : nullptr;
      if (!thread) {
        LIB_LOG(lib::kError, "d3d12", "no thread keeps fence values current: values read through fences stay as made");
        return;
      }
      CloseHandle(thread);
    }
    cells_.push_back(cell);
    SetEvent(wake_);
  }

  // Once it returns, the watcher no longer reads the cell or its fence.
  void remove(FenceCell* cell) {
    std::lock_guard<std::mutex> guard(lock_);
    cells_.erase(std::remove(cells_.begin(), cells_.end(), cell), cells_.end());
  }

 private:
  static DWORD WINAPI run(void* self) { static_cast<FenceWatcher*>(self)->loop(); }

  [[noreturn]] void loop() {
    for (;;) {
      {
        std::lock_guard<std::mutex> guard(lock_);
        for (FenceCell* cell : cells_) {
          const uint64_t seen = cell->fence->GetCompletedValue();
          cell->value = seen;
          if (seen == UINT64_MAX) {  // device removed
            static std::atomic<bool> reported{false};
            if (g_device && !reported.exchange(true)) report_device_removal(g_device);
            continue;
          }
          // Set once per value: a fence whose value stays put keeps one
          // pending event, however often others wake the thread.
          if (cell->armed != seen + 1 && SUCCEEDED(cell->fence->SetEventOnCompletion(seen + 1, wake_))) {
            cell->armed = seen + 1;
          }
        }
      }
      WaitForSingleObject(wake_, INFINITE);
    }
  }

  std::mutex lock_;
  std::vector<FenceCell*> cells_;
  HANDLE wake_ = nullptr;  // auto-reset
};

const volatile uint64_t* STDMETHODCALLTYPE fence_value_address(Fence* self) {
  if (FenceCell* cell = self->cell.load(std::memory_order_acquire)) return &cell->value;
  static std::mutex lock;
  std::lock_guard<std::mutex> guard(lock);
  FenceCell* cell = self->cell.load(std::memory_order_acquire);
  if (!cell) {
    cell = new FenceCell();
    cell->fence = static_cast<ID3D12Fence*>(self->host);
    cell->value = cell->fence->GetCompletedValue();
    FenceWatcher::get().add(cell);
    self->cell.store(cell, std::memory_order_release);
  }
  return &cell->value;
}

ULONG STDMETHODCALLTYPE fence_release(Fence* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    if (FenceCell* cell = self->cell.load(std::memory_order_acquire)) {
      FenceWatcher::get().remove(cell);
      delete cell;
    }
    if (self->host) self->host->Release();
    delete self;
  }
  return static_cast<ULONG>(left);
}

const auto kFenceVtable = [] {
  auto table = make_forwarding_vtable(12);
  table[2] = reinterpret_cast<void*>(&fence_release);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  table[12] = reinterpret_cast<void*>(&fence_value_address);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_fence(Device* self, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID, void** fence) {
  if (!fence) return E_POINTER;
  *fence = nullptr;
  ID3D12Fence* host = nullptr;
  const HRESULT hr = self->host->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&host));
  (void)flags;
  if (FAILED(hr)) return hr;
  auto* made = new Fence();
  init_wrapper(made, host, &kFenceInfo, kFenceVtable.data());
  *fence = made;
  return S_OK;
}

// --- command allocators ----------------------------------------------------------
//
// Observed: device slot 9 is CreateCommandAllocator(type, riid, out), as on
// the desktop; allocators keep the desktop order (slot 8 Reset).
//
// Observed: allocator slot 9 takes a title function, a context pointer and
// zero; the result is not checked. The function allocates title memory: it
// takes the size as its fourth argument, the log2 alignment as its fifth and
// the context as its seventh, and returns the memory. Assumed: the console
// allocator obtains its command-buffer memory through it. Host allocators
// manage their own memory, so the function is recorded and never called.
const lib::VtableInfo kAllocatorInfo{kLibrary, "ID3D12CommandAllocatorX"};

HRESULT STDMETHODCALLTYPE allocator_set_memory_callback(Wrapper*, void* function, void* context, UINT64 flags) {
  LIB_LOG(lib::kDebug, "d3d12", "command allocator memory callback %p (context %p, 0x%llx): not needed on the host",
          function, context, static_cast<unsigned long long>(flags));
  return S_OK;
}

const auto kAllocatorVtable = [] {
  auto table = make_forwarding_vtable(9);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  table[9] = reinterpret_cast<void*>(&allocator_set_memory_callback);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_command_allocator(Device* self, D3D12_COMMAND_LIST_TYPE type, REFIID,
                                                   void** allocator) {
  if (!allocator) return E_POINTER;
  *allocator = nullptr;
  ID3D12CommandAllocator* host = nullptr;
  const HRESULT hr = self->host->CreateCommandAllocator(type, IID_PPV_ARGS(&host));
  if (FAILED(hr)) return hr;
  *allocator = wrap(host, &kAllocatorInfo, kAllocatorVtable.data());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE create_command_list_x(Device* self, const uint32_t* desc, void* allocator,
                                                void* initial_state, REFIID, void** list) {
  return create_command_list(self, desc, allocator, initial_state, list);
}

HRESULT STDMETHODCALLTYPE set_frame_interval_x(Device*, void*, UINT length_us, UINT period, UINT flags) {
  return set_frame_interval(length_us, period, flags);
}

HRESULT STDMETHODCALLTYPE schedule_frame_event_x(Device*, UINT type, UINT64 offset_us, void*, UINT flags) {
  return schedule_frame_event(type, offset_us, flags);
}

// Observed: slot 80 has the shape of WaitFrameEventX(type, timeout_ms, target,
// flags, token*) of the public sample code. Titles call it with type 0 (the
// interval origin), a zero timeout and flags 2, require success and keep the
// token it writes; and with type 2, an infinite timeout and a pointer to the
// token of an earlier frame, to wait for that frame. Assumed: flag bit 1 asks
// for the current frame's token without waiting for the next origin; event 2
// of a frame is reached when its present has been carried out.
HRESULT STDMETHODCALLTYPE wait_frame_event_x(Device*, UINT type, UINT timeout_ms, const void* target, UINT flags,
                                             UINT64* token) {
  if (!token) return E_POINTER;
  if (target) {
    HolyUnimplementedFrom(kLibrary, "WaitFrameEventX(with a target output)", CALLER());
    return E_NOTIMPL;
  }
  if (type == 0) {
    if (flags & 2) {
      *token = next_frame_token();
      return S_OK;
    }
    uint64_t ignored = 0;
    const HRESULT hr = wait_frame_event(0, timeout_ms, &ignored);
    if (SUCCEEDED(hr)) *token = next_frame_token();
    return hr;
  }
  if (type == 2) return wait_for_presented(*token, timeout_ms);
  LIB_LOG(lib::kError, "d3d12", "WaitFrameEventX type %u flags 0x%x", type, flags);
  HolyUnimplementedFrom(kLibrary, "WaitFrameEventX(event type)", CALLER());
  return E_NOTIMPL;
}

// Observed: slot 81 takes (frame token, type mask, count*, records*) and
// returns an HRESULT: titles pass the token of a frame a few frames back, the
// mask 0x11100, a count of 6 and room for six 56-byte records, and use the
// records only when the call succeeds with a nonzero count: each record's
// dword 0 is one bit of the mask (0x1000 and 0x10000 are looked at), byte 8 a
// flag and bytes 0x18-0x1F a 64-bit value. Assumed: statistics of a frame's
// events (timing). Host: no such statistics are kept; the call succeeds with
// no records.
HRESULT STDMETHODCALLTYPE get_frame_statistics_x(Device*, UINT64 token, UINT mask, UINT* count, void* records) {
  if (!count) return E_POINTER;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    LIB_LOG(lib::kInfo, "d3d12", "frame statistics asked for (token %llu, mask 0x%x, %u records at %p): none kept",
            static_cast<unsigned long long>(token), mask, *count, records);
  }
  *count = 0;
  return S_OK;
}

// Slot 15: GetDescriptorHandleIncrementSize(type), as on the desktop.
UINT STDMETHODCALLTYPE get_descriptor_handle_increment_size(Device*, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  if (static_cast<uint32_t>(type) >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) {
    LIB_LOG(lib::kDebug, "d3d12", "descriptor increment for heap type %u", static_cast<unsigned>(type));
  }
  return title_descriptor_bytes(type);
}

HRESULT STDMETHODCALLTYPE create_placed_resource(Device* self, D3D12_GPU_VIRTUAL_ADDRESS address,
                                                 const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                 const D3D12_CLEAR_VALUE* clear, REFIID, void** resource) {
  return create_placed_resource_x(self, address, desc, state, clear, resource, CALLER());
}

HRESULT STDMETHODCALLTYPE create_descriptor_heap_x(Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID,
                                                   void** heap) {
  return create_descriptor_heap(self, desc, heap);
}

// Slot 25: GetResourceAllocationInfo(visible_mask, count, descs), as on the
// desktop; the 16-byte result comes back through the result pointer, for the
// description the host takes (desktop_resource_desc).
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE get_resource_allocation_info(
    void* first, void* second, UINT, UINT count, const D3D12_RESOURCE_DESC* descs) {
  Device* self;
  D3D12_RESOURCE_ALLOCATION_INFO* result;
  lib::struct_return_arguments(first, second, &kDeviceInfo, &self, &result);
  std::vector<D3D12_RESOURCE_DESC> host;
  for (UINT i = 0; i < count; ++i) host.push_back(desktop_resource_desc(descs[i]));
  *result = self->host->GetResourceAllocationInfo(0, count, host.data());
  if (result->SizeInBytes == UINT64_MAX) {
    for (UINT i = 0; i < count; ++i) {
      const D3D12_RESOURCE_DESC& d = descs[i];
      LIB_LOG(lib::kError, "d3d12",
              "allocation info refused: dim %d align 0x%llx %llux%ux%u mips %u fmt %d samples %u/%u layout %d flags 0x%x",
              d.Dimension, static_cast<unsigned long long>(d.Alignment), static_cast<unsigned long long>(d.Width),
              d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, d.SampleDesc.Count, d.SampleDesc.Quality, d.Layout,
              d.Flags);
    }
  }
  if (count && HolyLogEnabled(lib::kDebug)) {
    LIB_LOG(lib::kDebug, "d3d12", "allocation info dim %d %llux%u fmt %d mips %u flags 0x%x -> 0x%llx align 0x%llx",
            descs[0].Dimension, static_cast<unsigned long long>(descs[0].Width), descs[0].Height, descs[0].Format,
            descs[0].MipLevels, descs[0].Flags, static_cast<unsigned long long>(result->SizeInBytes),
            static_cast<unsigned long long>(result->Alignment));
  }
  return result;
}

// Observed: slot 108 is the desktop GetResourceAllocationInfo1(visible_mask,
// count, descs, infos): the result through the result pointer, and an
// optional array of per-resource offsets, alignments and sizes. Titles call
// it with a small-resource alignment and retry without when the alignment
// that comes back differs; a size of 4 GiB or more is fatal to them.
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE get_resource_allocation_info1(
    void* first, void* second, UINT, UINT count, const D3D12_RESOURCE_DESC* descs,
    D3D12_RESOURCE_ALLOCATION_INFO1* infos) {
  Device* self;
  D3D12_RESOURCE_ALLOCATION_INFO* result;
  lib::struct_return_arguments(first, second, &kDeviceInfo, &self, &result);
  std::vector<D3D12_RESOURCE_DESC> host;
  for (UINT i = 0; i < count; ++i) host.push_back(desktop_resource_desc(descs[i]));
  ID3D12Device4* device4 = nullptr;
  if (FAILED(self->host->QueryInterface(IID_PPV_ARGS(&device4)))) {
    *result = {UINT64_MAX, 0};
    LIB_LOG(lib::kError, "d3d12", "the host device has no GetResourceAllocationInfo1");
    return result;
  }
  *result = device4->GetResourceAllocationInfo1(0, count, host.data(), infos);
  device4->Release();
  if (result->SizeInBytes == UINT64_MAX) {
    LIB_LOG(lib::kError, "d3d12", "allocation info (slot 108) refused for %u descriptions, first dim %d fmt %d",
            count, count ? descs[0].Dimension : 0, count ? descs[0].Format : 0);
  }
  return result;
}

HRESULT STDMETHODCALLTYPE create_committed_resource_x(Device* self, const D3D12_HEAP_PROPERTIES* properties,
                                                      D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
                                                      UINT state, const D3D12_CLEAR_VALUE* clear, void*, UINT,
                                                      const void*, REFIID, void** resource) {
  // The shape of CreateCommittedResource3: `state` is an initial layout.
  return create_committed_resource(self, properties, flags, desc, state, true, clear, resource);
}

// Observed: device slot 27 takes the desktop CreateCommittedResource
// arguments (heap properties, heap flags, resource description, initial
// state, optimised clear value, interface id, out pointer).
HRESULT STDMETHODCALLTYPE create_committed_resource_desktop(Device* self, const D3D12_HEAP_PROPERTIES* properties,
                                                            D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
                                                            UINT state, const D3D12_CLEAR_VALUE* clear, REFIID,
                                                            void** resource) {
  return create_committed_resource(self, properties, flags, desc, state, false, clear, resource);
}

// --- root signatures -----------------------------------------------------------------
//
// Observed: device slot 16 is CreateRootSignature(node mask, blob, size, riid,
// out), as on the desktop, with blobs from D3D12SerializeVersionedRootSignature.
const lib::VtableInfo kRootSignatureInfo{kLibrary, "ID3D12RootSignatureX"};

const auto kRootSignatureVtable = [] {
  auto table = make_forwarding_vtable(7);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

}  // namespace

void report_device_removal(Device* device) {
  const HRESULT reason = device->host->GetDeviceRemovedReason();
  if (reason == S_OK) return;
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) return;  // once a process
  LIB_LOG(lib::kError, "d3d12", "the host device was removed (reason 0x%08lX)", static_cast<unsigned long>(reason));
  ID3D12InfoQueue* queue = nullptr;
  if (SUCCEEDED(device->host->QueryInterface(IID_PPV_ARGS(&queue)))) {
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = count > 20 ? count - 20 : 0; i < count; ++i) {
      SIZE_T length = 0;
      queue->GetMessage(i, nullptr, &length);
      std::vector<char> storage(length);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      if (SUCCEEDED(queue->GetMessage(i, message, &length))) {
        LIB_LOG(lib::kError, "d3d12", "  debug layer: %s", message->pDescription);
      }
    }
    queue->Release();
  }
  ID3D12DeviceRemovedExtendedData* dred = nullptr;
  if (FAILED(device->host->QueryInterface(IID_PPV_ARGS(&dred)))) return;
  D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
    LIB_LOG(lib::kError, "d3d12", "  page fault at GPU address 0x%llx",
            static_cast<unsigned long long>(fault.PageFaultVA));
  }
  ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
  if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&dred1))) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
      const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      LIB_LOG(lib::kError, "d3d12", "  list %p (%ls) on %ls: %u of %u", node->pCommandList,
              node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"title list",
              node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?", done, node->BreadcrumbCount);
      if (done >= node->BreadcrumbCount) continue;  // finished lists say nothing more
      const int op = node->pCommandHistory ? static_cast<int>(node->pCommandHistory[done]) : -1;
      // The marker before the operation that did not finish names its programs.
      const wchar_t* context = nullptr;
      for (UINT i = 0; i < node->BreadcrumbContextsCount; ++i) {
        if (node->pBreadcrumbContexts[i].BreadcrumbIndex <= done) context = node->pBreadcrumbContexts[i].pContextString;
      }
      LIB_LOG(lib::kError, "d3d12", "  command list %p: %u of %u operations completed; next operation %d, after %ls",
              node->pCommandList, done, node->BreadcrumbCount, op, context ? context : L"(no marker)");
      std::string history;
      for (UINT i = 0; node->pCommandHistory && i < node->BreadcrumbCount && i < 64; ++i) {
        history += (i == done ? " [" : " ") + std::to_string(static_cast<int>(node->pCommandHistory[i])) +
                   (i == done ? "]" : "");
      }
      LIB_LOG(lib::kError, "d3d12", "    operations (D3D12_AUTO_BREADCRUMB_OP):%s", history.c_str());
      for (UINT i = 0; i < node->BreadcrumbContextsCount && i < 16; ++i) {
        LIB_LOG(lib::kError, "d3d12", "    marker at operation %u: %ls", node->pBreadcrumbContexts[i].BreadcrumbIndex,
                node->pBreadcrumbContexts[i].pContextString);
      }
    }
  }
  if (dred1) dred1->Release();
  dred->Release();
}

namespace {

HRESULT STDMETHODCALLTYPE create_root_signature(Device* self, UINT, const void* blob, SIZE_T size, REFIID,
                                                void** signature) {
  if (!signature) return E_POINTER;
  *signature = nullptr;
  ID3D12RootSignature* host = nullptr;
  const HRESULT hr = self->host->CreateRootSignature(0, blob, size, IID_PPV_ARGS(&host));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "root signature of %llu bytes refused by the host (0x%08lX)",
            static_cast<unsigned long long>(size), static_cast<unsigned long>(hr));
    report_device_removal(self);
    return hr;
  }
  Wrapper* wrapper = wrap(host, &kRootSignatureInfo, kRootSignatureVtable.data());
  register_root_signature_hash(host, shader_hash(blob, size));
  ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
  if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&reader)))) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (SUCCEEDED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)) && desc) {
      register_root_layout(wrapper, desc->Desc_1_1);
      std::string text;
      for (UINT i = 0; i < desc->Desc_1_1.NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER1& p = desc->Desc_1_1.pParameters[i];
        char item[96];
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
          std::snprintf(item, sizeof(item), "[%u: %u constants b%u vis %u] ", i, p.Constants.Num32BitValues,
                        p.Constants.ShaderRegister, static_cast<unsigned>(p.ShaderVisibility));
        } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
          std::snprintf(item, sizeof(item), "[%u: table of %u ranges vis %u] ", i,
                        p.DescriptorTable.NumDescriptorRanges, static_cast<unsigned>(p.ShaderVisibility));
        } else {
          std::snprintf(item, sizeof(item), "[%u: root %s r%u vis %u] ", i,
                        p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV   ? "CBV"
                        : p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? "SRV"
                                                                           : "UAV",
                        p.Descriptor.ShaderRegister, static_cast<unsigned>(p.ShaderVisibility));
        }
        text += item;
      }
      LIB_LOG(lib::kDebug, "d3d12", "root signature %p: %u parameters %s", static_cast<void*>(host),
              desc->Desc_1_1.NumParameters, text.c_str());
    }
    reader->Release();
  }
  *signature = wrapper;
  return S_OK;
}

// --- pipeline states ------------------------------------------------------------------
// A heap over title memory. Observed: device slot 74 takes the address of a
// title reservation with physical pages mapped, the page count (64 KiB pages)
// and the out pointer, and returns an HRESULT; titles create it for their
// texture pools. Host heaps over that memory are opened per piece of storage
// when something is placed or mapped there (gpu_memory.h), not here: pools
// are large and mostly untouched.
struct TitleHeap {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Device* device = nullptr;
  uint64_t address = 0;
  uint64_t size = 0;
};

const lib::VtableInfo kTitleHeapInfo{kLibrary, "ID3D12HeapX"};

HRESULT STDMETHODCALLTYPE title_heap_query_interface(TitleHeap* self, REFIID iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!IsEqualGUID(iid, __uuidof(ID3D12Heap)) && !IsEqualGUID(iid, __uuidof(IUnknown))) return E_NOINTERFACE;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE title_heap_add_ref(TitleHeap* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE title_heap_release(TitleHeap* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}
HRESULT STDMETHODCALLTYPE title_heap_get_device(TitleHeap* self, REFIID, void** device) {
  if (!device) return E_POINTER;
  self->device->refs.fetch_add(1);
  *device = self->device;
  return S_OK;
}
// GetDesc: a 48-byte structure (lib::struct_return_arguments).
D3D12_HEAP_DESC* STDMETHODCALLTYPE title_heap_get_desc(void* first, void* second) {
  TitleHeap* self;
  D3D12_HEAP_DESC* result;
  lib::struct_return_arguments(first, second, &kTitleHeapInfo, &self, &result);
  *result = {};
  result->SizeInBytes = self->size;
  result->Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  result->Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  return result;
}

const auto kTitleHeapVtable = [] {
  auto table = lib::make_reporting_vtable<16>();
  table[0] = reinterpret_cast<void*>(&title_heap_query_interface);
  table[1] = reinterpret_cast<void*>(&title_heap_add_ref);
  table[2] = reinterpret_cast<void*>(&title_heap_release);
  table[7] = reinterpret_cast<void*>(&title_heap_get_device);
  table[8] = reinterpret_cast<void*>(&title_heap_get_desc);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_heap_over_memory(Device* self, void* address, uint32_t pages, void** heap) {
  if (!heap) return E_POINTER;
  *heap = nullptr;
  if (!address || pages == 0) return E_INVALIDARG;
  auto* object = new TitleHeap();
  object->vtable = kTitleHeapVtable.data();
  object->info = &kTitleHeapInfo;
  object->device = self;
  object->address = reinterpret_cast<uint64_t>(address);
  object->size = static_cast<uint64_t>(pages) << 16;
  LIB_LOG(lib::kDebug, "d3d12", "heap over title memory %p + 0x%llx", address,
          static_cast<unsigned long long>(object->size));
  *heap = object;
  return S_OK;
}

const lib::VtableInfo kQueryHeapInfo{kLibrary, "ID3D12QueryHeapX"};

const auto kQueryHeapVtable = [] {
  auto table = make_forwarding_vtable(8);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

// Observed: device slot 39 takes a D3D12_QUERY_HEAP_DESC (type, count, node
// mask), the interface id and the out pointer, as the desktop's
// CreateQueryHeap at the same slot.
HRESULT STDMETHODCALLTYPE create_query_heap(Device* self, const D3D12_QUERY_HEAP_DESC* desc, REFIID, void** heap) {
  if (!heap || !desc) return E_POINTER;
  *heap = nullptr;
  D3D12_QUERY_HEAP_DESC host_desc = *desc;
  host_desc.NodeMask = 0;
  ID3D12QueryHeap* host = nullptr;
  const HRESULT hr = self->host->CreateQueryHeap(&host_desc, IID_PPV_ARGS(&host));
  LIB_LOG(lib::kDebug, "d3d12", "query heap type %u count %u node mask 0x%x -> 0x%08lX",
          static_cast<unsigned>(desc->Type), desc->Count, desc->NodeMask, static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;
  *heap = wrap(host, &kQueryHeapInfo, kQueryHeapVtable.data());
  return S_OK;
}

const lib::VtableInfo kCommandSignatureInfo{kLibrary, "ID3D12CommandSignatureX"};

const auto kCommandSignatureVtable = [] {
  auto table = make_forwarding_vtable(8);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

// Observed: device slot 41 takes a D3D12_COMMAND_SIGNATURE_DESC, a root
// signature (null when no argument touches root state), the interface id
// and the out pointer, as the desktop's CreateCommandSignature. Argument
// types are passed on when the desktop knows them. Assumed: console type
// 105, seen alone with a 12-byte stride, is a dispatch.
constexpr uint32_t kConsoleDispatchArgument = 105;

HRESULT STDMETHODCALLTYPE create_command_signature(Device* self, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                   const void* root_signature, REFIID, void** signature) {
  if (!signature || !desc) return E_POINTER;
  *signature = nullptr;
  std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments(desc->pArgumentDescs,
                                                      desc->pArgumentDescs + desc->NumArgumentDescs);
  for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
    const auto* raw = reinterpret_cast<const uint32_t*>(&desc->pArgumentDescs[i]);
    LIB_LOG(lib::kDebug, "d3d12", "command signature stride %u argument %u: %08x %08x %08x %08x", desc->ByteStride, i,
            raw[0], raw[1], raw[2], raw[3]);
    if (raw[0] == kConsoleDispatchArgument) {
      arguments[i] = {};
      arguments[i].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    } else if (desc->pArgumentDescs[i].Type > D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) {
      HolyUnimplementedFrom(kLibrary, "command signature with a console argument type", CALLER());
      return E_NOTIMPL;
    }
  }
  D3D12_COMMAND_SIGNATURE_DESC host_desc = *desc;
  host_desc.pArgumentDescs = arguments.data();
  host_desc.NodeMask = 0;
  ID3D12CommandSignature* host = nullptr;
  const HRESULT hr = self->host->CreateCommandSignature(
      &host_desc, root_signature ? host_of<ID3D12RootSignature>(root_signature) : nullptr, IID_PPV_ARGS(&host));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "command signature refused by the host (0x%08lX)", static_cast<unsigned long>(hr));
    return hr;
  }
  *signature = wrap(host, &kCommandSignatureInfo, kCommandSignatureVtable.data());
  return S_OK;
}

const lib::VtableInfo kPipelineInfo{kLibrary, "ID3D12PipelineStateX"};

// Pipelines keep the desktop ID3D12PipelineState order; their methods wait
// for the host object, which is compiled in the background (pipeline_stream.h).
HRESULT STDMETHODCALLTYPE pipeline_get_private_data(Wrapper* self, REFGUID guid, UINT* size, void* data) {
  ID3D12PipelineState* host = pipeline_host(self);
  return host ? host->GetPrivateData(guid, size, data) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE pipeline_set_private_data(Wrapper* self, REFGUID guid, UINT size, const void* data) {
  ID3D12PipelineState* host = pipeline_host(self);
  return host ? host->SetPrivateData(guid, size, data) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE pipeline_set_private_data_interface(Wrapper* self, REFGUID guid, const IUnknown* data) {
  ID3D12PipelineState* host = pipeline_host(self);
  return host ? host->SetPrivateDataInterface(guid, data) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE pipeline_set_name_x(Wrapper* self, LPCWSTR name) {
  return pipeline_set_name(self, name);
}
HRESULT STDMETHODCALLTYPE pipeline_get_cached_blob(Wrapper* self, ID3DBlob** blob) {
  ID3D12PipelineState* host = pipeline_host(self);
  return host ? host->GetCachedBlob(blob) : E_FAIL;
}

const auto kPipelineVtable = [] {
  auto table = make_forwarding_vtable(3);
  table[2] = reinterpret_cast<void*>(&pipeline_release);
  table[3] = reinterpret_cast<void*>(&pipeline_get_private_data);
  table[4] = reinterpret_cast<void*>(&pipeline_set_private_data);
  table[5] = reinterpret_cast<void*>(&pipeline_set_private_data_interface);
  table[6] = reinterpret_cast<void*>(&pipeline_set_name_x);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  table[8] = reinterpret_cast<void*>(&pipeline_get_cached_blob);
  return table;
}();

// The root signature of translated pipelines: the memory slot views
// (u0-u65535, space 8) as a descriptor table, the page table (t0, space 8) as
// a root view, the system constants (b0, space 8) as a root constant buffer
// view, and the bound shader-visible heaps whole (from
// index 0) as texture and typed UAV arrays of each image dimension (spaces
// kTextureSpace + slot) and as the sampler array (kTextureSpace).
}  // namespace

ID3D12RootSignature* translated_root_signature(Device* device) {
  static std::mutex lock;
  std::lock_guard<std::mutex> guard(lock);
  if (device->translated_root) return device->translated_root;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kMemoryViews, 0, kTranslatedSpace, 0};
  constexpr uint32_t kSlots = kImageSlots;
  D3D12_DESCRIPTOR_RANGE images[3 * kSlots]{};
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    images[slot] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, kTextureSpace + slot, 0};
    images[kSlots + slot] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, kTextureSpace + slot, 0};
    images[2 * kSlots + slot] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, kIntegerImageSpace + slot, 0};
  }
  D3D12_DESCRIPTOR_RANGE samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, kTextureSpace, 0};
  D3D12_ROOT_PARAMETER parameters[kTranslatedRootParameters]{};
  parameters[kRootMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[kRootMemory].DescriptorTable = {1, &range};
  parameters[kRootPageTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[kRootPageTable].Descriptor = {0, kTranslatedSpace};
  parameters[kRootSystem].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[kRootSystem].Descriptor = {0, kTranslatedSpace};
  parameters[kRootImages].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[kRootImages].DescriptorTable = {3 * kSlots, images};
  parameters[kRootSamplers].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[kRootSamplers].DescriptorTable = {1, &samplers};
  parameters[kRootNggOutput].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[kRootNggOutput].Descriptor = {0, kNggSpace};
  parameters[kRootNggInput].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[kRootNggInput].Descriptor = {0, kNggSpace};
  const D3D12_ROOT_SIGNATURE_DESC desc{kTranslatedRootParameters, parameters, 0, nullptr,
                                       D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* blob = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
  if (SUCCEEDED(hr)) {
    hr = device->host->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&device->translated_root));
  }
  if (SUCCEEDED(hr)) {
    register_root_signature_hash(device->translated_root,
                                 shader_hash(blob->GetBufferPointer(), blob->GetBufferSize()));
  }
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "root signature for translated pipelines: 0x%08lX", static_cast<unsigned long>(hr));
  }
  if (blob) blob->Release();
  if (errors) errors->Release();
  return device->translated_root;
}

namespace {

// A compute pipeline from native code, translated to host bytecode (see
// shader/translate.h). `dump` is what is saved when that fails.
HRESULT create_compute_pipeline_from_code(Device* self, const uint8_t* code, size_t size,
                                          const ComputeShaderInfo& info, const void* dump, size_t dump_size,
                                          void** pipeline, const void* caller) {
  std::vector<uint8_t> bytecode;
  std::string error;
  const ULONGLONG start = GetTickCount64();
  const uint64_t key = compute_translation_key(code, size, info).value();
  uint32_t stored_size = 0;
  const bool cached = translation_header(key, &stored_size, nullptr);  // the bytes are read when compiling
  const bool translated = cached || translate_compute(code, size, info, &bytecode, &error);
  const bool on_disk = cached || (translated && store_translation(key, bytecode, nullptr));
  if (!translated) {
    dump_shader(dump, dump_size, "compute");
    LIB_LOG(lib::kError, "d3d12", "native compute shader not translatable: %s", error.c_str());
    HolyUnimplementedFrom(kLibrary, ("native shader code: " + error).c_str(), caller);
    return E_NOTIMPL;
  }
  ID3D12RootSignature* root = translated_root_signature(self);
  if (!root) return E_FAIL;
  const size_t program_size = cached ? stored_size : bytecode.size();
  LIB_LOG(cached ? lib::kDebug : lib::kInfo, "d3d12", "%s compute shader (%u threads, %zu bytes of host bytecode) in %llu ms",
          cached ? "cached" : "translated", info.threads[0] * info.threads[1] * info.threads[2], program_size,
          static_cast<unsigned long long>(GetTickCount64() - start));
  if (HolyLogEnabled(lib::kDebug) && !cached) {
    LIB_LOG(lib::kDebug, "d3d12", "native compute program %016llx -> host %016llx",
            static_cast<unsigned long long>(shader_hash(dump, dump_size)),
            static_cast<unsigned long long>(shader_hash(bytecode.data(), bytecode.size())));
  }
  // The host compile runs in the background, as for graphics pipelines
  // (pipeline_stream.cpp): titles create pipelines by the thousand.
  std::vector<uint8_t> stream;
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &root, sizeof(root));
  const D3D12_SHADER_BYTECODE cs{nullptr, program_size};  // patched in when compiling
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cs, sizeof(cs));
  std::vector<ProgramRef> programs{cached ? stored_program(key, stored_size)
                                          : intern_program(std::move(bytecode), on_disk ? key : 0)};
  return create_recorded_pipeline(self, std::move(stream), std::move(programs), true, &kPipelineInfo,
                                  kPipelineVtable.data(), pipeline);
}

// A compute pipeline from the native code inside a shader container.
HRESULT create_translated_compute_pipeline(Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                           void** pipeline, const void* caller) {
  LIB_LOG(lib::kDebug, "d3d12", "translating a compute shader of %llu bytes",
          static_cast<unsigned long long>(desc->CS.BytecodeLength));
  NativeComputeProgram program;
  if (!native_compute_program(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, &program)) {
    dump_shader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, "compute");
    HolyUnimplementedFrom(kLibrary, "compute pipeline: unreadable native program", caller);
    return E_NOTIMPL;
  }
  ComputeShaderInfo info;
  for (int axis = 0; axis < 3; ++axis) info.threads[axis] = program.threads[axis];
  return create_compute_pipeline_from_code(self, program.block.data() + program.code_offset,
                                           program.block.size() - program.code_offset, info,
                                           desc->CS.pShaderBytecode, desc->CS.BytecodeLength, pipeline, caller);
}

// Observed: device slot 62 takes a D3D12_COMPUTE_PIPELINE_STATE_DESC, two
// null pointers, the interface id and the out pointer. Shaders that keep
// their DXIL run from a rebuilt desktop container; shaders that carry only
// native GPU code, and those whose DXIL the host refuses, run translated.
HRESULT STDMETHODCALLTYPE create_compute_pipeline_state_x(Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                          void*, void*, REFIID, void** pipeline) {
  if (!pipeline || !desc) return E_POINTER;
  *pipeline = nullptr;
  std::vector<uint8_t> shader;
  const ShaderFormat format = rebuild_desktop_container(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, shader);
  if (format == ShaderFormat::NativeOnly) {
    return create_translated_compute_pipeline(self, desc, pipeline, CALLER());
  }
  if (format == ShaderFormat::Unknown) {
    dump_shader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, "compute");
    HolyUnimplementedFrom(kLibrary, "compute pipeline with an unknown shader format", CALLER());
    return E_NOTIMPL;
  }
  const uint64_t hash = shader_hash(desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
  if (host_refused_before(hash)) {
    NativeComputeProgram probe;
    if (native_compute_program(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, &probe)) {
      return create_translated_compute_pipeline(self, desc, pipeline, CALLER());
    }
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC host = *desc;
  host.pRootSignature = host_of<ID3D12RootSignature>(desc->pRootSignature);
  host.CS = {shader.data(), shader.size()};
  host.NodeMask = 0;
  host.CachedPSO = {};
  host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  ID3D12PipelineState* state = nullptr;
  const HRESULT hr = self->host->CreateComputePipelineState(&host, IID_PPV_ARGS(&state));
  if (FAILED(hr)) {
    discard_host_caches_after_run(hr);
    remember_host_refusal(hash);
    // Console containers keep a desktop-compatible DXIL program next to the
    // native code; when the host refuses the former, the latter is used.
    NativeComputeProgram probe;
    const bool native = native_compute_program(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, &probe);
    LIB_LOG(lib::kWarn, "d3d12", "the host refused a compute pipeline (0x%08lX)%s", static_cast<unsigned long>(hr),
            native ? "; translating its native code" : "");
    if (native) return create_translated_compute_pipeline(self, desc, pipeline, CALLER());
    dump_shader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, "compute");
    return hr;
  }
  *pipeline = wrap_host_pipeline(state, false, &kPipelineInfo, kPipelineVtable.data());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE create_pipeline_state(Device* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID,
                                                void** pipeline) {
  return create_pipeline_from_stream(self, desc, &kPipelineInfo, kPipelineVtable.data(), pipeline, CALLER());
}

// Observed: device slot 67 takes (description, pipeline interface id, out)
// and returns an HRESULT. The description is the root signature followed by
// (pointer, size) pairs: 28 bytes of compute registers
// (native_compute_registers), 20 bytes not understood, and the native code
// (it starts with no-operation instructions carrying literals). It creates a
// compute pipeline from native code given directly.
HRESULT STDMETHODCALLTYPE create_native_pipeline(Device* self, const uint64_t* desc, REFIID, void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  const auto* registers = reinterpret_cast<const uint32_t*>(desc[1]);
  const auto* code = reinterpret_cast<const uint8_t*>(desc[5]);
  const size_t code_size = desc[6];
  if (!registers || !code || !code_size) {
    HolyUnimplementedFrom(kLibrary, "compute pipeline from native code: unexpected description", CALLER());
    return E_NOTIMPL;
  }
  ComputeShaderInfo info;
  std::string error;
  if (!native_compute_registers(registers, desc[2], &info, &error)) {
    LIB_LOG(lib::kError, "d3d12", "native compute registers (%s): %08x %08x %08x %08x %08x %08x %08x", error.c_str(),
            registers[0], registers[1], registers[2], registers[3], registers[4], registers[5], registers[6]);
    dump_shader(code, code_size, "native compute");
    HolyUnimplementedFrom(kLibrary, ("compute pipeline from native code: " + error).c_str(), CALLER());
    return E_NOTIMPL;
  }
  LIB_LOG(lib::kDebug, "d3d12", "native compute pipeline: %u x %u x %u threads, %u user SGPRs, rsrc2 0x%08X",
          info.threads[0], info.threads[1], info.threads[2], info.user_sgprs, registers[6]);
  return create_compute_pipeline_from_code(self, code, code_size, info, code, code_size, out, CALLER());
}

// Blend factors and functions by their public RDNA encodings
// (CB_BLENDn_CONTROL); alpha factors use the alpha form of colour factors.
D3D12_BLEND blend_factor(uint32_t factor, bool alpha) {
  static const D3D12_BLEND kColour[] = {
      D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA,
      D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_DEST_COLOR,
      D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO,
      D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR,
      D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR};
  D3D12_BLEND out = factor < std::size(kColour) ? kColour[factor] : D3D12_BLEND_ZERO;
  if (alpha) {
    switch (out) {
      case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
      case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
      case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
      case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
      case D3D12_BLEND_SRC1_COLOR: return D3D12_BLEND_SRC1_ALPHA;
      case D3D12_BLEND_INV_SRC1_COLOR: return D3D12_BLEND_INV_SRC1_ALPHA;
      default: return out;
    }
  }
  return out;
}

D3D12_BLEND_OP blend_function(uint32_t function) {
  switch (function) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

// Observed: device slot 57 takes (description, pipeline interface id, out)
// and returns an HRESULT; it creates a graphics pipeline from native code
// (NativeGraphicsDescription in shader_container.h).
HRESULT STDMETHODCALLTYPE create_native_graphics_pipeline(Device* self, const uint64_t* desc, REFIID, void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  NativeGraphicsDescription native;
  std::string error;
  if (HolyLogEnabled(lib::kDebug)) {
    // The description and its blocks, for studying their layouts.
    auto words = [](const void* at, size_t bytes) {
      std::string text;
      const auto* w = static_cast<const uint32_t*>(at);
      for (size_t i = 0; w && i < bytes / 4; ++i) {
        char h[12];
        std::snprintf(h, sizeof(h), " %08x", w[i]);
        text += h;
      }
      return text;
    };
    LIB_LOG(lib::kDebug, "d3d12", "native pipeline description:%s", words(desc, 17 * 8).c_str());
    LIB_LOG(lib::kDebug, "d3d12", "native pipeline state block:%s",
            words(reinterpret_cast<const void*>(desc[1]), std::min<uint64_t>(desc[2], 0x400)).c_str());
    LIB_LOG(lib::kDebug, "d3d12", "native pipeline block:%s",
            words(reinterpret_cast<const void*>(desc[3]), std::min<uint64_t>(desc[4], 0x400)).c_str());
    // The root signature and each stage's metadata, for studying bindings.
    void* root_host = desc[0] ? static_cast<void*>(host_of<ID3D12RootSignature>(reinterpret_cast<void*>(desc[0]))) : nullptr;
    for (size_t i = 5; i + 3 < 5 + 2 * 6; i += 4) {
      if (!desc[i]) continue;
      std::string text;
      const auto* m = reinterpret_cast<const uint32_t*>(desc[i + 2]);
      for (size_t w = 0; m && w < desc[i + 3] / 4; ++w) {
        char h[12];
        std::snprintf(h, sizeof(h), "%s%08x", (w >= 4 && (w - 4) % 8 == 0) ? " | " : " ", m[w]);
        text += h;
      }
      GraphicsStage stage = GraphicsStage::Vertex;
      native_graphics_stage(reinterpret_cast<const uint8_t*>(desc[i]), desc[i + 1], &stage);
      LIB_LOG(lib::kDebug, "d3d12", "native pipeline: root signature %p, %s %016llx, metadata:%s", root_host,
              stage == GraphicsStage::Vertex ? "VS" : "PS",
              static_cast<unsigned long long>(shader_hash(reinterpret_cast<const void*>(desc[i]), desc[i + 1])),
              text.c_str());
      dump_shader(reinterpret_cast<const void*>(desc[i]), desc[i + 1], "native graphics");
    }
  }
  if (!native_graphics_description(desc, &native, &error)) {
    if (desc[5] && desc[6]) dump_shader(reinterpret_cast<const void*>(desc[5]), desc[6], "native graphics");
    HolyUnimplementedFrom(kLibrary, ("graphics pipeline from native code: " + error).c_str(), CALLER());
    return E_NOTIMPL;
  }
  ProgramRef vs, ps;
  ShaderInterface vs_link, ps_link;
  auto translate = [&](const uint8_t* code, size_t size, GraphicsStage stage, ProgramRef* bytecode,
                       ShaderInterface* link, const ShaderInterface* consumer = nullptr) {
    if (translate_graphics_cached(code, size, stage, bytecode, link, &error, consumer)) return true;
    dump_shader(code, size, "native graphics");
    LIB_LOG(lib::kError, "d3d12", "native %s shader %016llx not translatable: %s",
            stage == GraphicsStage::Vertex ? "vertex" : "pixel", static_cast<unsigned long long>(shader_hash(code, size)),
            error.c_str());
    HolyUnimplementedFrom(kLibrary, ("native shader code: " + error).c_str(), CALLER());
    return false;
  };
  // NGG vertex programs run as a compute pass before a draw of its records
  // (ngg.h).
  const bool ngg = native_ngg_program(native.vertex_code, native.vertex_size);
  NggInfo ngg_info;
  ProgramRef ngg_program;  // the compute pass's program
  if (ngg) {
    std::vector<uint8_t> cs;
    const uint64_t key = ngg_translation_key(native.vertex_code, native.vertex_size).value();
    uint32_t stored_size = 0;
    const bool stored = translation_header(key, &stored_size, &vs_link, &ngg_info);
    bool on_disk = stored;
    if (!on_disk) {
      if (!translate_ngg(native.vertex_code, native.vertex_size, &cs, &vs_link, &ngg_info, &error)) {
        dump_shader(native.vertex_code, native.vertex_size, "native graphics");
        LIB_LOG(lib::kError, "d3d12", "native NGG program %016llx not translatable: %s",
                static_cast<unsigned long long>(shader_hash(native.vertex_code, native.vertex_size)), error.c_str());
        HolyUnimplementedFrom(kLibrary, ("native shader code: " + error).c_str(), CALLER());
        return E_NOTIMPL;
      }
      on_disk = store_translation(key, cs, &vs_link, &ngg_info);
    }
    ngg_program = stored ? stored_program(key, stored_size) : intern_program(std::move(cs), on_disk ? key : 0);
    // The vertex program is written once the pixel program's attributes are
    // known.
  } else if (!translate(native.vertex_code, native.vertex_size, GraphicsStage::Vertex, &vs, &vs_link)) {
    return E_NOTIMPL;
  }
  if (native.pixel_code && !translate(native.pixel_code, native.pixel_size, GraphicsStage::Pixel, &ps, &ps_link)) {
    return E_NOTIMPL;
  }
  if (ngg) {
    // The records hold what the NGG program exports; the vertex program
    // passes on what the pixel program reads, half attributes unpacked.
    if (((ps_link.params | ps_link.half_params) & ~vs_link.params) != 0) {
      LIB_LOG(lib::kError, "d3d12", "pixel shader reads attributes 0x%x/0x%x the NGG program does not export (0x%x)",
              ps_link.params, ps_link.half_params, vs_link.params);
      return E_INVALIDARG;
    }
    vs = intern_program(ngg_vertex_program(ngg_info.stride, ps ? ps_link.params : 0, ps ? ps_link.half_params : 0));
  } else if (ps_link.half_params) {
    // The vertex program again, outputting what the pixel program reads (its
    // half attributes unpacked).
    if ((ps_link.half_params & ~vs_link.params) != 0) {
      LIB_LOG(lib::kError, "d3d12", "pixel shader reads half attributes 0x%x the vertex shader does not export (0x%x)",
              ps_link.half_params, vs_link.params);
      return E_INVALIDARG;
    }
    if (!translate(native.vertex_code, native.vertex_size, GraphicsStage::Vertex, &vs, &vs_link, &ps_link)) {
      return E_NOTIMPL;
    }
  }
  if (!ngg && (ps_link.params & ~vs_link.params) != 0) {
    LIB_LOG(lib::kError, "d3d12", "pixel shader reads attributes 0x%x the vertex shader does not export (0x%x)",
            ps_link.params, vs_link.params);
    return E_INVALIDARG;
  }
  ID3D12RootSignature* root = translated_root_signature(self);
  if (!root) return E_FAIL;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC host{};
  host.pRootSignature = root;
  host.VS = {nullptr, vs->size()};  // programs are patched in when compiling
  if (ps) host.PS = {nullptr, ps->size()};
  host.BlendState.IndependentBlendEnable = TRUE;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t b = native.blend[i];
    D3D12_RENDER_TARGET_BLEND_DESC& target = host.BlendState.RenderTarget[i];
    target.BlendEnable = (b >> 30) & 1;
    const bool separate = (b >> 29) & 1;
    target.SrcBlend = blend_factor(b & 0x1F, false);
    target.BlendOp = blend_function((b >> 5) & 7);
    target.DestBlend = blend_factor((b >> 8) & 0x1F, false);
    target.SrcBlendAlpha = blend_factor(separate ? (b >> 16) & 0x1F : b & 0x1F, true);
    target.BlendOpAlpha = blend_function(separate ? (b >> 21) & 7 : (b >> 5) & 7);
    target.DestBlendAlpha = blend_factor(separate ? (b >> 24) & 0x1F : (b >> 8) & 0x1F, true);
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = static_cast<UINT8>((native.target_mask >> (4 * i)) & 0xF);
  }
  host.SampleMask = native.sample_mask;
  // PA_SU_SC_MODE_CNTL: CULL_FRONT bit 0, CULL_BACK bit 1, FACE bit 2 (set:
  // clockwise is front). Depth clipping and the depth bias are assumed off
  // and zero (their registers are not established).
  const uint32_t mode = native.mode_control;
  host.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  host.RasterizerState.CullMode = (mode & 3) == 1   ? D3D12_CULL_MODE_FRONT
                                  : (mode & 3) == 2 ? D3D12_CULL_MODE_BACK
                                                    : D3D12_CULL_MODE_NONE;
  host.RasterizerState.FrontCounterClockwise = ((mode >> 2) & 1) == 0;
  host.RasterizerState.DepthClipEnable = TRUE;
  host.DepthStencilState.DepthEnable = FALSE;
  host.PrimitiveTopologyType = ngg ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                  : static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(native.topology_type);
  host.NumRenderTargets = native.render_targets;
  for (uint32_t i = 0; i < native.render_targets; ++i) host.RTVFormats[i] = static_cast<DXGI_FORMAT>(native.formats[i]);
  host.SampleDesc = {native.sample_count, 0};
  // As a desktop stream, recorded so pipelines can be derived from it.
  std::vector<uint8_t> stream;
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &host.pRootSignature, sizeof(void*));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &host.VS, sizeof(host.VS));
  if (ps) stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &host.PS, sizeof(host.PS));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &host.BlendState, sizeof(host.BlendState));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &host.SampleMask, sizeof(UINT));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &host.RasterizerState,
                sizeof(host.RasterizerState));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, &host.DepthStencilState,
                sizeof(host.DepthStencilState));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &host.PrimitiveTopologyType,
                sizeof(UINT));
  D3D12_RT_FORMAT_ARRAY formats{};
  formats.NumRenderTargets = host.NumRenderTargets;
  for (uint32_t i = 0; i < 8; ++i) formats.RTFormats[i] = host.RTVFormats[i];
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &formats, sizeof(formats));
  stream_append(&stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &host.SampleDesc, sizeof(host.SampleDesc));
  void* ngg_compute = nullptr;
  if (ngg) {
    std::vector<uint8_t> compute;
    stream_append(&compute, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &root, sizeof(root));
    const D3D12_SHADER_BYTECODE program{nullptr, ngg_program->size()};  // patched in when compiling
    stream_append(&compute, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &program, sizeof(program));
    const HRESULT made = create_recorded_pipeline(self, std::move(compute), {ngg_program}, true, &kPipelineInfo,
                                                  kPipelineVtable.data(), &ngg_compute);
    if (FAILED(made)) return made;
  }
  std::vector<ProgramRef> programs{vs};
  if (ps) programs.push_back(ps);
  const HRESULT hr = create_recorded_pipeline(self, std::move(stream), std::move(programs), true, &kPipelineInfo,
                                              kPipelineVtable.data(), out, native.depth_control,
                                              native.stencil_control, native.stencil_masks);
  if (SUCCEEDED(hr) && ngg) {
    // The graphics pipeline holds the compute pass's reference.
    Pipeline* graphics = as_pipeline(*out);
    graphics->ngg_compute = as_pipeline(ngg_compute);
    graphics->ngg = ngg_info;
  } else if (ngg_compute) {
    pipeline_release(static_cast<Wrapper*>(ngg_compute));
  }
  LIB_LOG(lib::kDebug, "d3d12",
          "native graphics pipeline: target format %u, blend 0x%08X, mode 0x%08X, vertex exports 0x%x, pixel reads "
          "0x%x -> 0x%08lX",
          native.formats[0], native.blend[0], mode, vs_link.params, ps_link.params, static_cast<unsigned long>(hr));
  if (FAILED(hr)) dump_shader(native.vertex_code, native.vertex_size, "native graphics");
  return hr;
}

// Device slot 55: a pipeline derived from another (pipeline_stream.cpp).
HRESULT STDMETHODCALLTYPE create_derived_pipeline_x(Device* self, const void* base, UINT count, const uint8_t* entries,
                                                    REFIID, void** out) {
  return create_derived_pipeline(self, base, count, entries, &kPipelineInfo, kPipelineVtable.data(), out,
                                 CALLER());
}

// Exploration (`--unimplemented continue`): a pipeline whose creation
// reported itself as unimplemented is handed out as a placeholder whose draws
// and dispatches are dropped (and counted), instead of the failure titles do
// not expect from pipeline creation on the console; the title's later calls
// then show. When unimplemented calls trap, the report has already ended the
// process.
template <auto Create>
struct explored;
template <typename... Args, HRESULT(STDMETHODCALLTYPE* Create)(Device*, Args...)>
struct explored<Create> {
  static HRESULT STDMETHODCALLTYPE call(Device* self, Args... args) {
    const void* outer = t_api_caller;
    if (!outer) t_api_caller = _ReturnAddress();
    const HRESULT hr = Create(self, args...);
    t_api_caller = outer;
    if (hr != E_NOTIMPL || HolyTrapsUnimplemented()) return hr;
    void** out = std::get<sizeof...(Args) - 1>(std::forward_as_tuple(args...));
    if (!out) return hr;
    *out = placeholder_pipeline(&kPipelineInfo, kPipelineVtable.data());
    return S_OK;
  }
};

const auto kDeviceVtable = [] {
  auto table = lib::make_reporting_vtable<kDeviceSlots>();
  HOLY_D3D_SLOT(table, kDeviceObject, 0, device_query_interface);
  HOLY_D3D_SLOT(table, kDeviceObject, 1, device_add_ref);
  HOLY_D3D_SLOT(table, kDeviceObject, 2, device_release);
  HOLY_D3D_SLOT(table, kDeviceObject, 8, create_queue);
  HOLY_D3D_SLOT(table, kDeviceObject, 9, create_command_allocator);
  HOLY_D3D_SLOT(table, kDeviceObject, 14, create_descriptor_heap_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 15, get_descriptor_handle_increment_size);
  HOLY_D3D_SLOT(table, kDeviceObject, 16, create_root_signature);
  HOLY_D3D_SLOT(table, kDeviceObject, 17, create_constant_buffer_view);
  HOLY_D3D_SLOT(table, kDeviceObject, 18, create_shader_resource_view);
  HOLY_D3D_SLOT(table, kDeviceObject, 19, create_unordered_access_view);
  HOLY_D3D_SLOT(table, kDeviceObject, 20, create_render_target_view);
  HOLY_D3D_SLOT(table, kDeviceObject, 21, create_depth_stencil_view);
  HOLY_D3D_SLOT(table, kDeviceObject, 22, create_sampler);
  HOLY_D3D_SLOT(table, kDeviceObject, 23, copy_descriptors);
  HOLY_D3D_SLOT(table, kDeviceObject, 24, copy_descriptors_simple);
  HOLY_D3D_SLOT(table, kDeviceObject, 25, get_resource_allocation_info);
  HOLY_D3D_SLOT(table, kDeviceObject, 26, get_custom_heap_properties);
  HOLY_D3D_SLOT(table, kDeviceObject, 27, create_committed_resource_desktop);
  HOLY_D3D_SLOT(table, kDeviceObject, 36, create_fence);
  HOLY_D3D_SLOT(table, kDeviceObject, 80, wait_frame_event_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 81, get_frame_statistics_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 39, create_query_heap);
  HOLY_D3D_SLOT(table, kDeviceObject, 41, create_command_signature);
  HOLY_D3D_SLOT(table, kDeviceObject, 48, create_queue_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 55, explored<&create_derived_pipeline_x>::call);
  HOLY_D3D_SLOT(table, kDeviceObject, 57, explored<&create_native_graphics_pipeline>::call);
  HOLY_D3D_SLOT(table, kDeviceObject, 61, create_sampler_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 62, explored<&create_compute_pipeline_state_x>::call);
  HOLY_D3D_SLOT(table, kDeviceObject, 67, explored<&create_native_pipeline>::call);
  HOLY_D3D_SLOT(table, kDeviceObject, 70, create_shader_resource_view_at);
  HOLY_D3D_SLOT(table, kDeviceObject, 73, create_command_list_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 74, create_heap_over_memory);
  HOLY_D3D_SLOT(table, kDeviceObject, 86, explored<&create_pipeline_state>::call);
  HOLY_D3D_SLOT(table, kDeviceObject, 78, set_frame_interval_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 79, schedule_frame_event_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 108, get_resource_allocation_info1);
  HOLY_D3D_SLOT(table, kDeviceObject, 127, create_committed_resource_x);
  HOLY_D3D_SLOT(table, kDeviceObject, 133, create_placed_resource);
  return table;
}();

}  // namespace

Device* create_device(const void* parameters) {
  uint32_t words[16] = {};
  if (parameters) std::memcpy(words, parameters, sizeof(words));
  LIB_LOG(lib::kInfo, "d3d12",
          "device parameters: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
          words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9],
          words[10], words[11], words[12], words[13], words[14], words[15]);

  // With debug logging (or HOLY_D3D_DEBUG=1 alone), the host's debug layer
  // checks every call and device removal records what the GPU was doing (the
  // documented device-removed extended data).
  const bool host_debug = HolyLogEnabled(lib::kDebug) || GetEnvironmentVariableW(L"HOLY_D3D_DEBUG", nullptr, 0) > 0;
  // HOLY_D3D_DRED=1: only the device-removed data (what the GPU was doing,
  // page faults), without the debug layer's checks and their cost.
  const bool dred_only = GetEnvironmentVariableW(L"HOLY_D3D_DRED", nullptr, 0) > 0;
  if (host_debug) {
    ID3D12Debug* layer = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) {
      layer->EnableDebugLayer();
      layer->Release();
    }
  }
  if (host_debug || dred_only) {
    ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
      dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      ID3D12DeviceRemovedExtendedDataSettings1* contexts = nullptr;
      if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&contexts)))) {
        contexts->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);  // marker strings
        contexts->Release();
      }
      dred->Release();
    }
  }
  // Before any translation: programs test for cached pages only when the
  // page cache runs.
  TranslatorOptions options;
  options.page_cache_reads = page_cache_enabled();
  set_translator_options(options);
  ID3D12Device* host = nullptr;
  const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&host));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "the host has no Direct3D 12 device (0x%08lX)", static_cast<unsigned long>(hr));
    return nullptr;
  }
  auto* device = new Device();
  device->vtable = kDeviceVtable.data();
  device->info = &kDeviceInfo;
  device->host = host;
  g_device = device;
  LIB_LOG(lib::kInfo, "d3d12", "host Direct3D 12 device created");
  log_object_vtables();
  if (host_debug) {
    // The debug layer's warnings and errors go to the log as they happen.
    ID3D12InfoQueue1* queue = nullptr;
    if (SUCCEEDED(host->QueryInterface(IID_PPV_ARGS(&queue)))) {
      DWORD cookie = 0;
      queue->RegisterMessageCallback(
          [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
            static std::atomic<int> budget{400};
            // 680: a pipeline with depth state and no depth format (expected
            // for translated pipelines whose depth format is not known).
            if (severity > D3D12_MESSAGE_SEVERITY_WARNING || id == 680 || budget.fetch_sub(1) <= 0) return;
            LIB_LOG(severity <= D3D12_MESSAGE_SEVERITY_ERROR ? lib::kError : lib::kWarn, "d3d12",
                    "debug layer (%d): %s", static_cast<int>(id), text);
          },
          D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
      queue->Release();
    }
  }
  return device;
}

}  // namespace holy::d3d12
