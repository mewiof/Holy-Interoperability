#include "core/libs/d3d12_xs/forward.h"

#include <cstring>
#include <mutex>
#include <set>

namespace holy::d3d12 {
namespace {

// mov rcx, [rcx + 0x10] ; mov rax, [rcx] ; jmp qword ptr [rax + disp32]
constexpr uint8_t kThunk[] = {0x48, 0x8B, 0x49, 0x10, 0x48, 0x8B, 0x01, 0xFF, 0xA0, 0, 0, 0, 0};
constexpr size_t kThunkStride = 16;

const uint8_t* thunks() {
  static const uint8_t* code = [] {
    auto* memory = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kThunkStride * kWrapperSlots, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    for (int slot = 0; slot < kWrapperSlots; ++slot) {
      uint8_t* thunk = memory + slot * kThunkStride;
      std::memset(thunk, 0xCC, kThunkStride);
      std::memcpy(thunk, kThunk, sizeof(kThunk));
      const int32_t displacement = slot * 8;
      std::memcpy(thunk + 9, &displacement, sizeof(displacement));
    }
    DWORD old = 0;
    VirtualProtect(memory, kThunkStride * kWrapperSlots, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), memory, kThunkStride * kWrapperSlots);
    return memory;
  }();
  return code;
}

std::mutex g_lock;
std::set<const void*>& wrapper_vtables() {
  static auto* storage = new std::set<const void*>();
  return *storage;
}

HRESULT STDMETHODCALLTYPE wrapper_query_interface(Wrapper* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE wrapper_add_ref(Wrapper* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE wrapper_release(Wrapper* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    self->host->Release();
    delete self;
  }
  return static_cast<ULONG>(left);
}

}  // namespace

std::array<void*, kWrapperSlots> make_forwarding_vtable(int forwarded) {
  auto table = lib::make_reporting_vtable<kWrapperSlots>();
  for (int slot = 3; slot < forwarded && slot < kWrapperSlots; ++slot) {
    table[slot] = const_cast<uint8_t*>(thunks() + slot * kThunkStride);
  }
  table[0] = reinterpret_cast<void*>(&wrapper_query_interface);
  table[1] = reinterpret_cast<void*>(&wrapper_add_ref);
  table[2] = reinterpret_cast<void*>(&wrapper_release);
  return table;
}

Wrapper* wrap(IUnknown* host, const lib::VtableInfo* info, void* const* vtable) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    wrapper_vtables().insert(vtable);
  }
  auto* wrapper = new Wrapper();
  wrapper->vtable = vtable;
  wrapper->info = info;
  wrapper->host = host;
  return wrapper;
}

IUnknown* unwrap(const void* object) {
  if (!object) return nullptr;
  const auto* wrapper = static_cast<const Wrapper*>(object);
  std::lock_guard<std::mutex> lock(g_lock);
  return wrapper_vtables().count(wrapper->vtable) ? wrapper->host : nullptr;
}

}  // namespace holy::d3d12
