// Command queues.
//
// Observed: device slot 8 creates a queue from a D3D12_COMMAND_QUEUE_DESC
// (16 bytes: type, priority, flags, node mask; titles pass {0, 0, 0, 1} for
// the direct queue) with the desktop ID3D12CommandQueue interface id. Device
// slot 48 takes the same arguments and is used for the compute queues
// ({2, 0, 1, 0}); its flags field does not carry the desktop meaning and is
// not passed on. Queue methods are established from the title's calls.
#include <d3d12.h>

#include <vector>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/present.h"
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
  std::vector<ID3D12CommandList*> host;
  host.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    const auto* list = static_cast<const CommandList*>(lists[i]);
    if (list && list->host) host.push_back(list->host);
  }
  LIB_LOG(lib::kTrace, "d3d12", "execute %u lists (token %llu, flags 0x%x)", count,
          static_cast<unsigned long long>(token), flags);
  if (!host.empty()) self->host->ExecuteCommandLists(static_cast<UINT>(host.size()), host.data());
}

// Observed: slot 32 is PresentX(plane_count, planes, parameters) (present.h).
HRESULT STDMETHODCALLTYPE queue_present_x(CommandQueue* self, UINT count, const PresentPlane* planes,
                                          const void* parameters) {
  return present_x(self, count, planes, parameters);
}

const auto kQueueVtable = [] {
  auto table = lib::make_reporting_vtable<kQueueSlots>();
  table[0] = reinterpret_cast<void*>(&queue_query_interface);
  table[1] = reinterpret_cast<void*>(&queue_add_ref);
  table[2] = reinterpret_cast<void*>(&queue_release);
  table[16] = reinterpret_cast<void*>(&queue_signal);
  table[18] = reinterpret_cast<void*>(&queue_get_timestamp_frequency);
  table[32] = reinterpret_cast<void*>(&queue_present_x);
  table[35] = reinterpret_cast<void*>(&queue_execute_command_lists_x);
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
  auto* queue = new CommandQueue();
  queue->vtable = kQueueVtable.data();
  queue->info = &kQueueInfo;
  queue->device = device;
  queue->host = host;
  queue->type = desc->Type;
  *out = queue;
  return S_OK;
}

}  // namespace holy::d3d12
