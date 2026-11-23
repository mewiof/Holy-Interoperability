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
// Observed calling convention: methods returning a structure are called
// C-style, with the result pointer in the first argument and the device in
// the second; all other methods take the device first.
#include "core/libs/d3d12_xs/device.h"

#include <d3d12.h>

#include <intrin.h>

#include <cstdio>
#include <cstring>

#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

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
  if (IsEqualGUID(iid, kDeviceIid) || IsEqualGUID(iid, __uuidof(IUnknown))) {
    self->refs.fetch_add(1);
    *out = self;
    return S_OK;
  }
  char symbol[96];
  std::snprintf(symbol, sizeof(symbol), "ID3D12DeviceX::QueryInterface(%08lx-%04x-%04x)", iid.Data1, iid.Data2,
                iid.Data3);
  HolyUnimplementedFrom(kLibrary, symbol, _ReturnAddress());
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

// Slot 26 (observed: called with node mask 1 and heap types 1, 2 and 3, the
// result pointer first): GetCustomHeapProperties, as on the desktop.
D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE get_custom_heap_properties(D3D12_HEAP_PROPERTIES* result, Device* self,
                                                                   UINT node_mask, D3D12_HEAP_TYPE type) {
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

HRESULT STDMETHODCALLTYPE get_device_of_child(Wrapper*, REFIID, void** device) {
  if (!device) return E_POINTER;
  g_device->refs.fetch_add(1);
  *device = g_device;
  return S_OK;
}

const auto kFenceVtable = [] {
  auto table = make_forwarding_vtable(12);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_fence(Device* self, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID, void** fence) {
  if (!fence) return E_POINTER;
  *fence = nullptr;
  ID3D12Fence* host = nullptr;
  const HRESULT hr = self->host->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&host));
  (void)flags;
  if (FAILED(hr)) return hr;
  *fence = wrap(host, &kFenceInfo, kFenceVtable.data());
  return S_OK;
}

// --- command allocators ----------------------------------------------------------
//
// Observed: device slot 9 is CreateCommandAllocator(type, riid, out), as on
// the desktop; allocators keep the desktop order (slot 8 Reset).
const lib::VtableInfo kAllocatorInfo{kLibrary, "ID3D12CommandAllocatorX"};

const auto kAllocatorVtable = [] {
  auto table = make_forwarding_vtable(9);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
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

const auto kDeviceVtable = [] {
  auto table = lib::make_reporting_vtable<kDeviceSlots>();
  table[0] = reinterpret_cast<void*>(&device_query_interface);
  table[1] = reinterpret_cast<void*>(&device_add_ref);
  table[2] = reinterpret_cast<void*>(&device_release);
  table[8] = reinterpret_cast<void*>(&create_queue);
  table[9] = reinterpret_cast<void*>(&create_command_allocator);
  table[26] = reinterpret_cast<void*>(&get_custom_heap_properties);
  table[36] = reinterpret_cast<void*>(&create_fence);
  table[48] = reinterpret_cast<void*>(&create_queue_x);
  table[73] = reinterpret_cast<void*>(&create_command_list_x);
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
  return device;
}

}  // namespace holy::d3d12
