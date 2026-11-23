#include "core/libs/xgameruntime/provider.h"

#include <intrin.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>

#include "core/libs/support.h"

namespace holy::xgr {
namespace {

constexpr char kLibrary[] = "xgameruntime.dll";

SRWLOCK g_lock = SRWLOCK_INIT;

// {00000000-0000-0000-C000-000000000046}
constexpr GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

std::deque<Interface>& interfaces() {
  static auto* storage = new std::deque<Interface>();
  return *storage;
}

// IUnknown on a process-wide singleton.
HRESULT STDMETHODCALLTYPE query_interface(Object* self, REFIID iid, void** out) {
  if (!out) return E_POINTER;
  if (IsEqualGUID(iid, self->iface->iid) || IsEqualGUID(iid, kIidUnknown)) {
    *out = self;
    return S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE add_ref(Object*) {
  return 2;
}
ULONG STDMETHODCALLTYPE release(Object*) {
  return 1;
}

template <int Slot>
uint64_t unimplemented_slot(Object* self, uint64_t a1, uint64_t a2, uint64_t a3) {
  char symbol[160];
  std::snprintf(symbol, sizeof(symbol), "%s#%d", self->iface->name, Slot);
  HolyLog(holy::lib::kError, "import", "%s(%llx, %llx, %llx)", symbol, a1, a2, a3);
  HolyUnimplementedFrom(kLibrary, symbol, _ReturnAddress());
  return 0;
}

template <int... Slots>
constexpr auto make_stub_table(std::integer_sequence<int, Slots...>) {
  using Stub = uint64_t (*)(Object*, uint64_t, uint64_t, uint64_t);
  return std::array<Stub, sizeof...(Slots)>{&unimplemented_slot<Slots>...};
}

const auto kStubs = make_stub_table(std::make_integer_sequence<int, kMaxSlots>{});

Interface& create_locked(const GUID& iid, const char* name) {
  Interface& iface = interfaces().emplace_back();
  iface.iid = iid;
  if (name) {
    iface.name = name;
  } else {
    // Unknown services are named after their id in logs.
    char text[40];
    format_guid(iid, text);
    char* owned = new char[sizeof(text)];
    std::memcpy(owned, text, sizeof(text));
    iface.name = owned;
  }
  for (int slot = 0; slot < kMaxSlots; ++slot) iface.vtable[slot] = reinterpret_cast<void*>(kStubs[slot]);
  iface.vtable[0] = reinterpret_cast<void*>(&query_interface);
  iface.vtable[1] = reinterpret_cast<void*>(&add_ref);
  iface.vtable[2] = reinterpret_cast<void*>(&release);
  iface.object.vtable = iface.vtable;
  iface.object.iface = &iface;
  return iface;
}

Interface* lookup_locked(const GUID& iid) {
  for (Interface& iface : interfaces()) {
    if (IsEqualGUID(iface.iid, iid)) return &iface;
  }
  return nullptr;
}

}  // namespace

void format_guid(const GUID& g, char (&out)[40]) {
  std::snprintf(out, sizeof(out), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

Interface& define_interface(const GUID& iid, const char* name) {
  AcquireSRWLockExclusive(&g_lock);
  Interface* existing = lookup_locked(iid);
  Interface& iface = existing ? *existing : create_locked(iid, name);
  ReleaseSRWLockExclusive(&g_lock);
  return iface;
}

Interface& find_interface(const GUID& iid) {
  AcquireSRWLockShared(&g_lock);
  Interface* existing = lookup_locked(iid);
  ReleaseSRWLockShared(&g_lock);
  if (existing) return *existing;
  return define_interface(iid, nullptr);
}

}  // namespace holy::xgr
