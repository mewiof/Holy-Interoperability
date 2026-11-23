// Sample title checks of the console graphics device, called the way titles
// call it (slot numbers and conventions as in docs/PLATFORM_NOTES.md).
#include <d3d12.h>
#include <windows.h>

#include <cstdint>

#include "tests/sample_title/checks.h"

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void* adapter, const void* parameters,
                                                                      REFIID riid, void** device);

namespace {

template <typename R, typename... Args>
R call(void* object, int slot, Args... args) {
  using Fn = R(STDMETHODCALLTYPE*)(void*, Args...);
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot])(object, args...);
}

constexpr GUID kDeviceIid = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
constexpr GUID kListIid = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

}  // namespace

void test_graphics() {
  uint32_t parameters[16] = {0x20004, 0, 0x100000};
  void* device = nullptr;
  const HRESULT created = D3D12XboxCreateDevice(nullptr, parameters, kDeviceIid, &device);
  check(created == S_OK && device, "D3D12XboxCreateDevice creates a device on the host's Direct3D 12");
  if (!device) return;

  // Slot 26 returns a structure C-style: result pointer first, device second.
  D3D12_HEAP_PROPERTIES properties{};
  using HeapPropertiesFn = D3D12_HEAP_PROPERTIES*(STDMETHODCALLTYPE*)(D3D12_HEAP_PROPERTIES*, void*, UINT,
                                                                       D3D12_HEAP_TYPE);
  reinterpret_cast<HeapPropertiesFn>((*static_cast<void***>(device))[26])(&properties, device, 1,
                                                                          D3D12_HEAP_TYPE_UPLOAD);
  check(properties.Type == D3D12_HEAP_TYPE_CUSTOM && properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        "custom heap properties come back through the result pointer");

  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};
  void* queue = nullptr;
  check(call<HRESULT>(device, 8, &queue_desc, __uuidof(ID3D12CommandQueue), &queue) == S_OK && queue,
        "the device creates a direct command queue");
  const D3D12_COMMAND_QUEUE_DESC compute_desc{D3D12_COMMAND_LIST_TYPE_COMPUTE, 0,
                                              static_cast<D3D12_COMMAND_QUEUE_FLAGS>(1), 0};
  void* compute = nullptr;
  check(call<HRESULT>(device, 48, &compute_desc, __uuidof(ID3D12CommandQueue), &compute) == S_OK && compute,
        "the device creates a compute queue through its console entry");

  void* fence = nullptr;
  check(call<HRESULT>(device, 36, UINT64{5}, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), &fence) == S_OK && fence,
        "the device creates a fence");
  if (fence) {
    check(call<UINT64>(fence, 8) == 5, "a fence reports its initial value");
    check(call<HRESULT>(fence, 10, UINT64{9}) == S_OK && call<UINT64>(fence, 8) == 9,
          "a fence signalled from the CPU reports the new value");
    void* owner = nullptr;
    check(call<HRESULT>(fence, 7, kDeviceIid, &owner) == S_OK && owner == device,
          "a fence's device is the console device");
  }

  void* allocator = nullptr;
  check(call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator) ==
                S_OK &&
            allocator,
        "the device creates a command allocator");
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  void* list = nullptr;
  check(allocator && call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list) ==
                         S_OK &&
            list,
        "the device creates a command list");
  check(list && call<HRESULT>(list, 9) == S_OK, "a new command list closes");
}
