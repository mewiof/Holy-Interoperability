// Store.
//
// Observed: interface {b09d803c-2414-4a05-82c6-66dfdc9e9a44}, provider
// {0dd112ac-7c24-448c-b92b-3960fb5bd30c}; its slots follow the documented
// XStore.h declarations in order. Slot 3's thunk passes two arguments (a user
// handle and a pointer to the caller's stack) after the core interface's
// blocking check, as the documented XStoreCreateContext(user, context*) does;
// slot 4 is XStoreCloseContextHandle(context).
//
// Policy: the store is an online service, which this runtime does not
// emulate, and no title is ever told that a user owns anything. A store
// context cannot be made: XStoreCreateContext fails as it does without a
// network (HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE), assumed the closest
// answer), so titles take their path for an unreachable store.
#include <windows.h>

#include <atomic>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"

namespace holy::xgr {
namespace {

HRESULT STDMETHODCALLTYPE create_context(Object*, void*, void** context) {
  if (context) *context = nullptr;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) {
    LIB_LOG(holy::lib::kInfo, "xstore", "store context requested: the store is an online service, not available");
  }
  return HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
}

void STDMETHODCALLTYPE close_context(Object*, void*) {}

}  // namespace

void register_store() {
  // {b09d803c-2414-4a05-82c6-66dfdc9e9a44} (provider {0dd112ac-7c24-448c-b92b-3960fb5bd30c})
  constexpr GUID kStore = {0xb09d803c, 0x2414, 0x4a05, {0x82, 0xc6, 0x66, 0xdf, 0xdc, 0x9e, 0x9a, 0x44}};
  Interface& store = define_interface(kStore, "store");
  set_method(store, 3, "XStoreCreateContext", &create_context);
  set_method(store, 4, "XStoreCloseContextHandle", &close_context);
}

}  // namespace holy::xgr
