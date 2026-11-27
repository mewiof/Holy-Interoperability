// Sample title checks of the console graphics device, called the way titles
// call it (slot numbers and conventions as in docs/PLATFORM_NOTES.md).
#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <vector>

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
  if (list) {
    // Titles write commands themselves while the cursor (0x18) is below the
    // limit (0x30): the list must keep them equal so every call is a method.
    const auto* fields = static_cast<const uint64_t*>(list);
    check(fields[3] >= fields[6], "a command list offers no command space titles would write into");
  }
  if (allocator) {
    check(call<HRESULT>(allocator, 9, static_cast<void*>(nullptr), static_cast<void*>(nullptr), UINT64{0}) == S_OK,
          "a command allocator accepts a memory callback");
  }
  if (list && allocator) {
    check(call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK &&
              call<HRESULT>(list, 9) == S_OK,
          "a closed command list resets with an allocator and closes again");
  }
  if (queue && list && fence) {
    void* lists[] = {list};
    call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
    UINT64 frequency = 0;
    check(call<HRESULT>(queue, 18, &frequency) == S_OK && frequency > 0, "a queue reports its timestamp frequency");
    const auto* value = call<const volatile uint64_t*>(fence, 12);
    check(value && *value == 9, "a fence's value can be read through the address it gives");
    check(call<HRESULT>(queue, 16, fence, UINT64{12}) == S_OK, "a queue signals a fence");
    for (int i = 0; value && *value != 12 && i < 2000; ++i) Sleep(1);
    check(value && *value == 12, "a fence's value address follows the GPU's signals");
  }
  {
    // A fence released and another made at the same address (the allocator
    // reuses the memory, though not every time): the new fence's value is
    // its own, not the released one's.
    void* first = nullptr;
    call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &first);
    const auto* first_value = first ? call<const volatile uint64_t*>(first, 12) : nullptr;
    if (first) call<HRESULT>(first, 10, UINT64{7});  // Signal from the CPU
    for (int i = 0; first_value && *first_value != 7 && i < 2000; ++i) Sleep(1);
    check(first_value && *first_value == 7, "a fence's value address follows signals from the CPU");
    const void* released = first;
    if (first) call<ULONG>(first, 2);
    std::vector<void*> made;
    bool reused = false, own_values = true;
    for (int attempt = 0; attempt < 2048 && !reused; ++attempt) {
      void* next = nullptr;
      call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &next);
      if (!next) break;
      made.push_back(next);
      reused = next == released;
      const auto* next_value = call<const volatile uint64_t*>(next, 12);
      Sleep(reused ? 20 : 0);
      own_values = own_values && next_value && *next_value == 0;
    }
    std::printf("    (%zu fences made, %s)\n", made.size(),
                reused ? "the last at the released fence's address" : "none at the released fence's address");
    check(released && own_values, "a fence made where a released one was reads its own value");
    for (void* fence_made : made) call<ULONG>(fence_made, 2);
  }
  if (queue && list && allocator && fence) {
    // Titles reset a list and submit it again while its previous submission may
    // still be executing, and set the console's rectangle-list topology (17).
    // Work after either must run (the host device is not removed).
    auto* marker = static_cast<volatile uint32_t*>(
        VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const auto address = reinterpret_cast<uint64_t>(marker);
    void* lists[] = {list};
    bool recorded = true;
    for (uint32_t round = 0; round < 2 && marker; ++round) {
      recorded = recorded && call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
      call<void>(list, 20, static_cast<D3D12_PRIMITIVE_TOPOLOGY>(17));
      call<void>(list, 71, address + 4 * round, 0x1234u + round, UINT64{0}, 0u);
      recorded = recorded && call<HRESULT>(list, 9) == S_OK;
      call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
    }
    const auto* value = call<const volatile uint64_t*>(fence, 12);
    check(call<HRESULT>(queue, 16, fence, UINT64{20}) == S_OK, "a queue signals a fence after resubmitted work");
    for (int i = 0; value && *value < 20 && i < 5000; ++i) Sleep(1);
    check(recorded && marker && value && *value == 20 && marker[0] == 0x1234 && marker[1] == 0x1235,
          "a list reset and submitted again, with a rectangle-list topology set, runs on the GPU");
  }
  // Frame tokens: the current frame's without waiting (flag 2), and a wait for
  // a frame no present has reached yet returns at once.
  UINT64 token = 0;
  check(call<HRESULT>(device, 80, 0u, 0u, static_cast<const void*>(nullptr), 2u, &token) == S_OK && token != 0,
        "WaitFrameEventX hands out a frame token");
  check(call<HRESULT>(device, 80, 2u, 0u, static_cast<const void*>(nullptr), 0u, &token) == S_OK,
        "waiting for a frame before anything was presented returns");
  // Slot 108: GetResourceAllocationInfo1, result through the result pointer.
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 100000;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_RESOURCE_ALLOCATION_INFO info{};
  D3D12_RESOURCE_ALLOCATION_INFO1 info1{};
  using InfoFn = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT,
                                                                      UINT, const D3D12_RESOURCE_DESC*,
                                                                      D3D12_RESOURCE_ALLOCATION_INFO1*);
  reinterpret_cast<InfoFn>((*static_cast<void***>(device))[108])(&info, device, 1, 1, &buffer, &info1);
  check(info.SizeInBytes >= 100000 && info1.SizeInBytes == info.SizeInBytes,
        "GetResourceAllocationInfo1 reports sizes for each description and in total");
  test_graphics_resources(device, queue);
}
