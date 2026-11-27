// xgameruntime.dll: the console game runtime (users, storage, packages,
// lifecycle, task queues and the other platform services titles reach through
// the runtime's statically linked client library).
//
// Entry points (observed): the client library loads the runtime by name and
// looks up QueryApiImpl, InitializeApiImpl, InitializeApiImplEx,
// InitializeApiImplEx2, XErrorReport and UninitializeApiImpl. It initialises
// through the newest initialiser it finds:
//
//   HRESULT InitializeApiImplEx2(uint64_t version_a, uint64_t version_b,
//                                uint64_t flags, const void* options)
//   HRESULT InitializeApiImplEx(uint64_t version_a, uint64_t version_b,
//                               uint64_t flags)
//   HRESULT InitializeApiImpl(uint32_t, uint32_t)
//
// The two version words are constants compiled into the client library (each
// packs two 16-bit build numbers in its low and high halves); `flags` carries
// client-side bits; `options` is the caller's options pointer or null. Any
// runtime function called before explicit initialisation makes the client
// library initialise implicitly with the same call.
//
//   HRESULT QueryApiImpl(const GUID* provider, const GUID* iid, void** object)
//   void    XErrorReport(HRESULT, const char* message)
//   void    UninitializeApiImpl(void)
//
// QueryApiImpl hands out the provider interfaces (see provider.h). The client
// library maps REGDB_E_CLASSNOTREG from it to its own "missing dependencies"
// error; this runtime never returns that.
#include <windows.h>

#include <atomic>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"

namespace {

std::atomic<int> g_initialised{0};
INIT_ONCE g_services_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK register_once(PINIT_ONCE, PVOID, PVOID*) {
  holy::xgr::register_services();
  return TRUE;
}

void ensure_services() {
  InitOnceExecuteOnce(&g_services_once, &register_once, nullptr, nullptr);
}

HRESULT initialise(const char* entry, uint64_t version_a, uint64_t version_b, uint64_t flags,
                   const void* options) {
  ensure_services();
  const int count = g_initialised.fetch_add(1) + 1;
  LIB_LOG(holy::lib::kInfo, "xgameruntime", "%s(client %u.%u / %u.%u, flags 0x%llx, options %p) #%d", entry,
          static_cast<unsigned>(version_a & 0xFFFF), static_cast<unsigned>((version_a >> 32) & 0xFFFF),
          static_cast<unsigned>(version_b & 0xFFFF), static_cast<unsigned>((version_b >> 32) & 0xFFFF),
          static_cast<unsigned long long>(flags), options, count);
  return S_OK;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    LIB_LOG(holy::lib::kDebug, "xgameruntime", "game runtime loaded");
  }
  return TRUE;
}

HOLY_EXPORT HRESULT InitializeApiImpl(uint32_t version_a, uint32_t version_b) {
  return initialise("InitializeApiImpl", version_a, version_b, 0, nullptr);
}

HOLY_EXPORT HRESULT InitializeApiImplEx(uint64_t version_a, uint64_t version_b, uint64_t flags) {
  return initialise("InitializeApiImplEx", version_a, version_b, flags, nullptr);
}

HOLY_EXPORT HRESULT InitializeApiImplEx2(uint64_t version_a, uint64_t version_b, uint64_t flags,
                                         const void* options) {
  return initialise("InitializeApiImplEx2", version_a, version_b, flags, options);
}

HOLY_EXPORT void UninitializeApiImpl() {
  const int count = g_initialised.fetch_sub(1) - 1;
  LIB_LOG(holy::lib::kInfo, "xgameruntime", "UninitializeApiImpl (%d remaining)", count);
}

HOLY_EXPORT HRESULT QueryApiImpl(const GUID* provider, const GUID* iid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (!provider || !iid) return E_INVALIDARG;
  ensure_services();
  holy::xgr::Interface& iface = holy::xgr::find_interface(*iid);
  if (!IsEqualGUID(*provider, *iid) && HolyLogEnabled(holy::lib::kDebug)) {
    char text[40];
    holy::xgr::format_guid(*provider, text);
    LIB_LOG(holy::lib::kDebug, "xgameruntime", "query for %s through provider %s", iface.name, text);
  }
  *object = &iface.object;
  return S_OK;
}

// Observed: called by the client library with an HRESULT and a message when
// it detects a usage or installation error, before it returns that error.
HOLY_EXPORT void XErrorReport(HRESULT result, const char* message) {
  LIB_LOG(holy::lib::kError, "xgameruntime", "error report 0x%08lX: %s", static_cast<unsigned long>(result),
          message ? message : "(no message)");
}

namespace holy::xgr {

void register_services() {
  register_core_interface();
  register_display();
  register_local_storage();
  register_networking();
  register_package();
  register_speech();
  register_system();
  register_user();
  register_game_save();
}

}  // namespace holy::xgr
