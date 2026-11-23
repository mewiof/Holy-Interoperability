// Objects of the console D3D12 implementation.
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <atomic>

#include "core/libs/vtable.h"


namespace holy::d3d12 {

inline constexpr char kLibrary[] = "d3d12_xs.dll";
inline constexpr int kDeviceSlots = 160;
inline constexpr int kQueueSlots = 64;
inline constexpr int kCommandListSlots = 200;
inline constexpr int kResourceSlots = 32;

struct Device {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12Device* host = nullptr;
  ID3D12RootSignature* translated_root = nullptr;  // shared by translated pipelines
};

struct CommandQueue {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Device* device = nullptr;
  ID3D12CommandQueue* host = nullptr;
  int type = 0;
};

struct CommandList {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Device* device = nullptr;
  ID3D12GraphicsCommandList* host = nullptr;
};

struct Resource {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12Resource* host = nullptr;
  uint64_t title_va = 0;  // the resource's GPU address as the title sees it
  uint64_t host_va = 0;   // the same place in the host's GPU address space
  D3D12_RESOURCE_DESC desc{};
  void* runtime_memory = nullptr;  // XMemAlloc'd as the console runtime does
};

Device* create_device(const void* parameters);

// The root signature of pipelines translated from native code: the region
// views (u0-u63, space 8) as a descriptor table and the system constants
// (b0, space 8) as a root constant buffer view; every stage sees both.
ID3D12RootSignature* translated_root_signature(Device* device);
HRESULT create_committed_resource(Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
                                  const D3D12_RESOURCE_DESC* desc, UINT state, const D3D12_CLEAR_VALUE* clear,
                                  void** out);
HRESULT create_placed_resource_x(Device* device, uint64_t address, const D3D12_RESOURCE_DESC* desc,
                                 D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void** out,
                                 const void* caller);

// The console IDXGIDevice a title obtains from the device.
void* dxgi_device();
HRESULT create_command_list(Device* device, const uint32_t* desc, void* allocator, void* initial_state, void** out);

// `console_flags`: the descriptor's flags field has console meaning and is
// not passed to the host.
HRESULT create_command_queue(Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, bool console_flags,
                             void** out);

}  // namespace holy::d3d12
