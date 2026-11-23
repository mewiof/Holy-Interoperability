// Command lists.
//
// Observed: device slot 73 creates a command list from a 16-byte descriptor
// whose first dword is the list type (titles pass {0, 2, 1, 0} for a direct
// list), an allocator, an initial pipeline state and the console command list
// interface id {c3827890-4024-474c-98cf-645b51dc4874}. The console command
// list has far more slots than the desktop one (titles call slots beyond 130);
// slot 9 is Close, as on the desktop. Every slot is established from the
// title's calls; unbound slots report themselves.
#include <d3d12.h>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

const lib::VtableInfo kListInfo{kLibrary, "ID3D12GraphicsCommandListX"};

HRESULT STDMETHODCALLTYPE list_query_interface(CommandList* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE list_add_ref(CommandList* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE list_release(CommandList* self) {
  return static_cast<ULONG>(self->refs.fetch_sub(1) - 1);
}

HRESULT STDMETHODCALLTYPE list_close(CommandList* self) {
  return self->host->Close();
}

const auto kListVtable = [] {
  auto table = lib::make_reporting_vtable<kCommandListSlots>();
  table[0] = reinterpret_cast<void*>(&list_query_interface);
  table[1] = reinterpret_cast<void*>(&list_add_ref);
  table[2] = reinterpret_cast<void*>(&list_release);
  table[9] = reinterpret_cast<void*>(&list_close);
  return table;
}();

}  // namespace

HRESULT create_command_list(Device* device, const uint32_t* desc, void* allocator, void* initial_state,
                            void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  auto* host_allocator = host_of<ID3D12CommandAllocator>(allocator);
  if (!host_allocator) return E_INVALIDARG;
  if (initial_state) {
    LIB_LOG(lib::kError, "d3d12", "command list with an initial pipeline state: not supported yet");
    return E_NOTIMPL;
  }
  const auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]);
  ID3D12GraphicsCommandList* host = nullptr;
  const HRESULT hr = device->host->CreateCommandList(0, type, host_allocator, nullptr, IID_PPV_ARGS(&host));
  LIB_LOG(lib::kDebug, "d3d12", "command list {%u, %u, %u, %u} -> 0x%08lX", desc[0], desc[1], desc[2], desc[3],
          static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;
  auto* list = new CommandList();
  list->vtable = kListVtable.data();
  list->info = &kListInfo;
  list->device = device;
  list->host = host;
  *out = list;
  return S_OK;
}

}  // namespace holy::d3d12
