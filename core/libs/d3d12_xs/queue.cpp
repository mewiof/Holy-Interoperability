// Command queues.
//
// Observed: device slot 8 creates a queue from a D3D12_COMMAND_QUEUE_DESC
// (16 bytes: type, priority, flags, node mask; titles pass {0, 0, 0, 1} for
// the direct queue) with the desktop ID3D12CommandQueue interface id. Device
// slot 48 takes the same arguments and is used for the compute queues
// ({2, 0, 1, 0}); its flags field does not carry the desktop meaning and is
// not passed on. Queue methods are established from the title's calls.
#include <d3d12.h>
#include <intrin.h>

#include <mutex>

#include <string>
#include <vector>

#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/page_cache.h"
#include "core/libs/d3d12_xs/present.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

const lib::VtableInfo kQueueInfo{kLibrary, "ID3D12CommandQueueX"};

HRESULT STDMETHODCALLTYPE queue_query_interface(CommandQueue* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE queue_add_ref(CommandQueue* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE queue_release(CommandQueue* self) {
  return static_cast<ULONG>(self->refs.fetch_sub(1) - 1);
}

// Observed: slot 18 takes a UINT64 pointer and returns an HRESULT; titles
// turn the value into a float tick rate: GetTimestampFrequency, two slots
// later than in the desktop order.
HRESULT STDMETHODCALLTYPE queue_get_timestamp_frequency(CommandQueue* self, UINT64* frequency) {
  return self->host->GetTimestampFrequency(frequency);
}

// Observed: slot 16 takes a fence and a value, as Signal (desktop slot 14).
HRESULT STDMETHODCALLTYPE queue_signal(CommandQueue* self, void* fence, UINT64 value) {
  auto* host = host_of<ID3D12Fence>(fence);
  if (!host) return E_INVALIDARG;
  return self->host->Signal(host, value);
}

// Observed: slot 35 takes (count, command lists, frame token, flags) and is how
// titles submit work (up to 256 lists at a time; flags 0 or 3). Assumed: the
// token ties the work to a frame for the console's frame statistics; the
// host has no such accounting, so lists are executed in order and the token
// and flags are not used.
void STDMETHODCALLTYPE queue_execute_command_lists_x(CommandQueue* self, UINT count, void* const* lists,
                                                    UINT64 token, UINT flags) {
  LIB_LOG(lib::kTrace, "d3d12", "execute %u lists (token %llu, flags 0x%x)", count,
          static_cast<unsigned long long>(token), flags);
  if (HolyLogEnabled(lib::kDebug)) {
    std::string numbers;
    for (UINT i = 0; i < count; ++i) {
      const auto* list = static_cast<const CommandList*>(lists[i]);
      if (list) numbers += " " + std::to_string(list->number);
    }
    LIB_LOG(lib::kDebug, "d3d12", "queue type %d executes title lists%s", self->type, numbers.c_str());
  }
  refresh_textures(self->device, self->host);
  page_cache_before_submit(self->device, self->host);
  collect_released_heaps();
  submit_command_lists(self, count, lists);
}

// Observed: slot 32 is PresentX(plane_count, planes, parameters) (present.h).
HRESULT STDMETHODCALLTYPE queue_present_x(CommandQueue* self, UINT count, const PresentPlane* planes,
                                          const void* parameters) {
  return present_x(self, count, planes, parameters);
}

// Observed: slot 31 takes (count, mappings, pool address, pool page count,
// 0) and its result is not checked: an update of the title's page mappings
// in queue order. Each mapping is {64 KiB-aligned address, run count, runs},
// each run {kind, pool page, page count}: kind 1 maps `count` pages of the
// pool (pool address + page * 64 KiB: memory the title reserved for physical
// pages and mapped) at the next pages from the mapping's address on, kind 2
// unmaps `count` pages there. Titles use it to move texture pool pages into
// and out of their streaming address range.
//
// Host: the pages are remapped when the call is made, through the memory
// library. GPU addresses are resolved when commands are recorded, so work
// recorded before the call keeps the old pages (also work submitted after
// it, which on the console would see the new ones: not handled).
struct PageRun {
  uint32_t kind;
  uint32_t page;
  uint32_t count;
};
struct PageMapping {
  uint64_t address;
  uint32_t runs;
  uint32_t reserved;
  const PageRun* run;
};
static_assert(sizeof(PageRun) == 12 && sizeof(PageMapping) == 24);
constexpr uint64_t kPoolPage = 0x10000;

HRESULT STDMETHODCALLTYPE queue_update_page_mappings_x(CommandQueue*, uint32_t count, const PageMapping* mappings,
                                                       uint64_t pool, uint32_t pool_pages, uint32_t flags) {
  using PageAtFn = ULONG_PTR (*)(const void*);
  using MapFn = void* (*)(void*, ULONG_PTR, ULONG_PTR*);
  static const HMODULE xmem = GetModuleHandleW(L"xmem.dll");
  static const auto page_at = reinterpret_cast<PageAtFn>(GetProcAddress(xmem, "HolyXMemPhysicalPageAt"));
  static const auto map = reinterpret_cast<MapFn>(GetProcAddress(xmem, "XMemMapPhysicalPages"));
  if (!page_at || !map) return E_FAIL;
  if (count && !mappings) return E_INVALIDARG;
  if (flags) LIB_LOG(lib::kWarn, "d3d12", "page mapping update with flags 0x%x (not understood)", flags);
  HRESULT result = S_OK;
  for (uint32_t i = 0; i < count; ++i) {
    const PageMapping& mapping = mappings[i];
    uint64_t at = mapping.address;
    for (uint32_t r = 0; r < mapping.runs && mapping.run; ++r) {
      const PageRun& run = mapping.run[r];
      if (run.kind == 1) {
        if (static_cast<uint64_t>(run.page) + run.count > pool_pages) {
          LIB_LOG(lib::kError, "d3d12", "page mapping run past the pool (%u + %u of %u pages)", run.page, run.count,
                  pool_pages);
          result = E_INVALIDARG;
        } else {
          std::vector<ULONG_PTR> pages(run.count);
          bool all = true;
          for (uint32_t p = 0; p < run.count; ++p) {
            pages[p] = page_at(reinterpret_cast<const void*>(pool + (static_cast<uint64_t>(run.page) + p) * kPoolPage));
            all = all && pages[p] != 0;
          }
          if (!all || !map(reinterpret_cast<void*>(at), run.count, pages.data())) {
            LIB_LOG(lib::kError, "d3d12", "page mapping of %u pool pages from %u at %p failed", run.count, run.page,
                    reinterpret_cast<void*>(at));
            result = E_FAIL;
          }
        }
      } else if (run.kind == 2) {
        if (!map(reinterpret_cast<void*>(at), run.count, nullptr)) {
          LIB_LOG(lib::kError, "d3d12", "page unmapping of %u pages at %p failed", run.count,
                  reinterpret_cast<void*>(at));
          result = E_FAIL;
        }
      } else {
        HolyUnimplementedFrom(kLibrary, "ID3D12CommandQueueX#31(page run kind)", _ReturnAddress());
        return E_NOTIMPL;
      }
      at += static_cast<uint64_t>(run.count) * kPoolPage;
    }
  }
  LIB_LOG(lib::kTrace, "d3d12", "page mapping update of %u ranges -> 0x%08lx", count, static_cast<unsigned long>(result));
  return result;
}

const auto kQueueVtable = [] {
  auto table = lib::make_reporting_vtable<kQueueSlots>();
  HOLY_D3D_SLOT(table, kQueueObject, 0, queue_query_interface);
  HOLY_D3D_SLOT(table, kQueueObject, 1, queue_add_ref);
  HOLY_D3D_SLOT(table, kQueueObject, 2, queue_release);
  HOLY_D3D_SLOT(table, kQueueObject, 16, queue_signal);
  HOLY_D3D_SLOT(table, kQueueObject, 18, queue_get_timestamp_frequency);
  HOLY_D3D_SLOT(table, kQueueObject, 31, queue_update_page_mappings_x);
  HOLY_D3D_SLOT(table, kQueueObject, 32, queue_present_x);
  HOLY_D3D_SLOT(table, kQueueObject, 35, queue_execute_command_lists_x);
  return table;
}();

}  // namespace

HRESULT create_command_queue(Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, bool console_flags,
                             void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  D3D12_COMMAND_QUEUE_DESC host_desc = *desc;
  host_desc.NodeMask = 0;
  if (console_flags) host_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  host_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  ID3D12CommandQueue* host = nullptr;
  const HRESULT hr = device->host->CreateCommandQueue(&host_desc, IID_PPV_ARGS(&host));
  LIB_LOG(lib::kDebug, "d3d12", "command queue type %d flags 0x%x -> 0x%08lX", desc->Type, desc->Flags,
          static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;
  host->SetName(desc->Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? L"title compute queue"
                : desc->Type == D3D12_COMMAND_LIST_TYPE_COPY  ? L"title copy queue"
                                                              : L"title direct queue");
  ID3D12Fence* fence = nullptr;
  const HRESULT fence_hr = device->host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (FAILED(fence_hr)) {
    host->Release();
    return fence_hr;
  }
  auto* queue = new CommandQueue();
  queue->vtable = kQueueVtable.data();
  queue->info = &kQueueInfo;
  queue->device = device;
  queue->host = host;
  queue->type = desc->Type;
  queue->fence = fence;
  register_gpu_timeline(fence, &queue->submitted);
  *out = queue;
  return S_OK;
}

namespace {

struct Timeline {
  ID3D12Fence* fence;
  const std::atomic<uint64_t>* submitted;
};
std::mutex g_timelines_lock;
std::vector<Timeline>& timelines() {
  static auto* list = new std::vector<Timeline>();
  return *list;
}

}  // namespace

void register_gpu_timeline(ID3D12Fence* fence, const std::atomic<uint64_t>* submitted) {
  fence->AddRef();
  std::lock_guard<std::mutex> guard(g_timelines_lock);
  timelines().push_back({fence, submitted});
}

GpuEpoch gpu_epoch_now() {
  GpuEpoch epoch;
  std::lock_guard<std::mutex> guard(g_timelines_lock);
  for (const Timeline& t : timelines()) {
    const uint64_t value = t.submitted->load(std::memory_order_acquire);
    if (value) epoch.marks.emplace_back(t.fence, value);
  }
  return epoch;
}

bool gpu_epoch_done(const GpuEpoch& epoch) {
  for (const auto& [fence, value] : epoch.marks) {
    if (fence->GetCompletedValue() < value) return false;
  }
  return true;
}

}  // namespace holy::d3d12
