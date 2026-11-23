// The sample title exports an allocation-hook table, as titles do. The hooks
// count calls and can hand out one preset block for descriptor heap memory
// (object type 4, allocator id 0x80), the way titles place their heaps.
#include <windows.h>

#include <atomic>
#include <cstdint>

extern "C" {
__declspec(dllimport) void* XMemAllocDefault(SIZE_T size, unsigned long long attributes);
__declspec(dllimport) void XMemFreeDefault(void* address, unsigned long long attributes);
}

std::atomic<int> g_hook_allocations{0};
void* g_preset_address = nullptr;
SIZE_T g_preset_size = 0;

namespace {

void* hook_alloc(SIZE_T size, unsigned long long attributes) {
  ++g_hook_allocations;
  const bool descriptor_heap = (attributes & 0x3FFF) == 4 && ((attributes >> 16) & 0xFF) == 0x80;
  if (descriptor_heap && g_preset_address && size == g_preset_size) {
    void* preset = g_preset_address;
    g_preset_address = nullptr;
    return preset;
  }
  return XMemAllocDefault(size, attributes);
}

void hook_free(void* address, unsigned long long attributes) {
  XMemFreeDefault(address, attributes);
}

}  // namespace

struct AllocationHooks {
  void* (*alloc)(SIZE_T, unsigned long long);
  void (*free)(void*, unsigned long long);
};

extern "C" __declspec(dllexport) const AllocationHooks __XMemAllocationHooks__ = {&hook_alloc, &hook_free};
