// User service.
//
// Observed: interface {01acd177-91f9-4763-a38e-ccbb55ce32e0}. Its slots follow
// the documented XUser.h declarations in order; the ones bound here were
// confirmed from their thunks and call sites:
//
//    7 XUserAddAsync(XUserAddOptions, XAsyncBlock*)
//    8 XUserAddResult(XAsyncBlock*, XUserHandle*)
//   33 XUserRegisterForChangeEvent(queue, context, callback, token*)
//   34 XUserUnregisterForChangeEvent(token, bool wait)
//
// Policy: this runtime signs nobody in and emulates no account service. No
// user is ever present, so:
//   - XUserAddAsync with AddDefaultUserSilently completes with the documented
//     E_GAMEUSER_NO_DEFAULT_USER ("no user is signed in");
//   - XUserAddAsync that would show the account picker completes with E_ABORT,
//     the documented result of a dismissed picker: there is no picker to show
//     (assumed to be the closest platform answer);
//   - change-event registrations succeed and never fire, because the set of
//     users never changes.
#include <windows.h>

#include <atomic>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/libs/xgameruntime/task_queue.h"

namespace holy::xgr {
namespace {

constexpr HRESULT kNoDefaultUser = static_cast<HRESULT>(0x89245106);  // E_GAMEUSER_NO_DEFAULT_USER
constexpr uint32_t kAddDefaultUserSilently = 0x1;

const char kAddIdentity = 0;

HRESULT CALLBACK add_provider(AsyncOp op, const AsyncProviderData* data) {
  switch (op) {
    case AsyncOp::Begin:
      return async_schedule(data->async, 0);
    case AsyncOp::DoWork: {
      const auto options = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data->context));
      const HRESULT result = (options & kAddDefaultUserSilently) ? kNoDefaultUser : E_ABORT;
      LIB_LOG(holy::lib::kInfo, "xuser", "user add (options 0x%x): no user is signed in -> 0x%08lX", options,
              static_cast<unsigned long>(result));
      return result;
    }
    default:
      return S_OK;
  }
}

HRESULT STDMETHODCALLTYPE add_async(Object*, uint32_t options, AsyncBlock* block) {
  return async_begin(block, reinterpret_cast<void*>(static_cast<uintptr_t>(options)), &kAddIdentity,
                     "XUserAddAsync", &add_provider);
}

HRESULT STDMETHODCALLTYPE add_result(Object*, AsyncBlock* block, void** user) {
  if (!user) return E_INVALIDARG;
  *user = nullptr;
  return async_get_result(block, &kAddIdentity, sizeof(void*), user, nullptr);
}

std::atomic<uint64_t> g_next_token{1};

HRESULT STDMETHODCALLTYPE register_for_change_event(Object*, void*, void*, void* callback, RegistrationToken* token) {
  if (!callback || !token) return E_INVALIDARG;
  token->token = g_next_token.fetch_add(1);
  return S_OK;
}

void STDMETHODCALLTYPE unregister_for_change_event(Object*, RegistrationToken, bool) {}

}  // namespace

void register_user() {
  // {01acd177-91f9-4763-a38e-ccbb55ce32e0}
  constexpr GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
  Interface& user = define_interface(kUser, "user");
  set_method(user, 7, "XUserAddAsync", &add_async);
  set_method(user, 8, "XUserAddResult", &add_result);
  set_method(user, 33, "XUserRegisterForChangeEvent", &register_for_change_event);
  set_method(user, 34, "XUserUnregisterForChangeEvent", &unregister_for_change_event);
}

}  // namespace holy::xgr
