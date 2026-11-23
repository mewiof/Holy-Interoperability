// Objects of the console D3D12 implementation.
#pragma once

#include <windows.h>

#include <atomic>

#include "core/libs/vtable.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct D3D12_COMMAND_QUEUE_DESC;

namespace holy::d3d12 {

inline constexpr char kLibrary[] = "d3d12_xs.dll";
inline constexpr int kDeviceSlots = 160;
inline constexpr int kQueueSlots = 64;
inline constexpr int kCommandListSlots = 200;

struct Device {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12Device* host = nullptr;
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

Device* create_device(const void* parameters);
HRESULT create_command_list(Device* device, const uint32_t* desc, void* allocator, void* initial_state, void** out);

// `console_flags`: the descriptor's flags field has console meaning and is
// not passed to the host.
HRESULT create_command_queue(Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, bool console_flags,
                             void** out);

}  // namespace holy::d3d12
