// User service.
//
// Observed: interface {01acd177-91f9-4763-a38e-ccbb55ce32e0}. Its slots follow
// the documented XUser.h declarations in order (signatures from the public
// XUser reference); 7, 8, 20, 33 and 34 were confirmed from their thunks and
// call sites, the others from the order:
//
//    3 XUserDuplicateHandle(handle, handle*)
//    4 XUserCloseHandle(handle)
//    5 XUserCompare(handle, handle) -> int32_t
//    6 XUserGetMaxUsers(uint32_t*)
//    7 XUserAddAsync(XUserAddOptions, XAsyncBlock*)
//    8 XUserAddResult(XAsyncBlock*, XUserHandle*)
//    9 XUserGetLocalId(handle, XUserLocalId*)
//   10 XUserFindUserByLocalId(XUserLocalId, handle*)
//   11 XUserGetId(handle, uint64_t*)
//   12 XUserFindUserById(uint64_t, handle*)
//   13 XUserGetIsGuest(handle, bool*)
//   14 XUserGetState(handle, XUserState*)
//   19 XUserGetAgeGroup(handle, XUserAgeGroup*)
//   20 XUserCheckPrivilege(handle, options, privilege, bool*, reasons*)
//   33 XUserRegisterForChangeEvent(queue, context, callback, token*)
//   34 XUserUnregisterForChangeEvent(token, bool wait)
//
// Observed: the gamertag comes from a second interface of the same provider,
// {cef4fac0-7676-4a94-a119-4c43f9eb5b74}, slot 3 (its only thunk passes five
// arguments: the handle, a 32-bit component, a size of 15 or more, a buffer
// and the size used), the documented XUserGetGamertag with a component. Slot
// 15 of the first interface (the order's place for the gamertag) is left
// unbound: no title has been seen calling it, and its form is not known.
//
// Policy: this runtime signs nobody in and emulates no account service. By
// default nobody is present:
//   - XUserAddAsync with AddDefaultUserSilently completes with the documented
//     E_GAMEUSER_NO_DEFAULT_USER ("no user is signed in");
//   - XUserAddAsync that would show the account picker completes with E_ABORT,
//     the documented result of a dismissed picker: there is no picker to show
//     (assumed to be the closest platform answer).
// With a local user (holy run --user local; observed: titles wait at their
// account prompt for a user, as a console always has one), one user is
// present: a profile of this host, signed in to no service. It is the
// default user and the one the picker picks; it is not a guest, is an adult,
// is called "Player", and its id is stable per host and title and identifies
// neither (a hash, like device ids). It has no privileges of online
// services (multiplayer, communication, content from others: denied as
// restricted), and no tokens (no service would accept them).
//
// Change-event registrations succeed and never fire: the set of users never
// changes while a title runs.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/libs/xgameruntime/task_queue.h"

namespace holy::xgr {
namespace {

constexpr HRESULT kNoDefaultUser = static_cast<HRESULT>(0x89245106);  // E_GAMEUSER_NO_DEFAULT_USER
constexpr HRESULT kUserNotFound = static_cast<HRESULT>(0x89245104);   // E_GAMEUSER_USER_NOT_FOUND
constexpr uint32_t kAddDefaultUserSilently = 0x1;
constexpr uint64_t kLocalId = 1;  // XUserLocalId of the local user
constexpr uint32_t kSignedIn = 0;  // XUserState
constexpr uint32_t kAdult = 3;     // XUserAgeGroup
constexpr uint32_t kRestricted = 2;  // XUserPrivilegeDenyReasons
constexpr char kGamertag[] = "Player";

// The local user. Handles are pointers to it; duplicating and closing them
// counts references but it lives as long as the process (it is the host's).
struct LocalUser {
  uint32_t magic = 0x52455355;  // "USER": tells our handles from others
  std::atomic<long> handles{0};
};
LocalUser g_user;

bool present() {
  static const bool local = HolyHasLocalUser() != 0;
  return local;
}

LocalUser* user_of(void* handle) {
  auto* user = static_cast<LocalUser*>(handle);
  return user == &g_user ? user : nullptr;
}

uint64_t user_id() {
  static const uint64_t id = [] {
    uint8_t digest[32] = {};
    uint64_t value = 0;
    if (host_title_digest("holy-interop-local-user:", digest)) std::memcpy(&value, digest, sizeof(value));
    // Nonzero, and clear of the top bit (titles keep ids in signed fields).
    value &= 0x7FFFFFFFFFFFFFFFull;
    return value ? value : 1;
  }();
  return id;
}

void* new_handle() {
  g_user.handles.fetch_add(1);
  return &g_user;
}

const char kAddIdentity = 0;

HRESULT CALLBACK add_provider(AsyncOp op, const AsyncProviderData* data) {
  switch (op) {
    case AsyncOp::Begin:
      return async_schedule(data->async, 0);
    case AsyncOp::DoWork: {
      const auto options = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data->context));
      if (present()) {
        LIB_LOG(holy::lib::kInfo, "xuser", "user add (options 0x%x): the local user", options);
        async_complete(data->async, S_OK, sizeof(void*));
        return E_PENDING;
      }
      const HRESULT result = (options & kAddDefaultUserSilently) ? kNoDefaultUser : E_ABORT;
      LIB_LOG(holy::lib::kInfo, "xuser", "user add (options 0x%x): no user is signed in -> 0x%08lX", options,
              static_cast<unsigned long>(result));
      return result;
    }
    case AsyncOp::GetResult:
      if (data->buffer_size < sizeof(void*) || !data->buffer) return E_INVALIDARG;
      *static_cast<void**>(data->buffer) = new_handle();
      return S_OK;
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

HRESULT STDMETHODCALLTYPE duplicate_handle(Object*, void* handle, void** duplicate) {
  if (!duplicate) return E_INVALIDARG;
  *duplicate = nullptr;
  if (!user_of(handle)) return E_INVALIDARG;
  *duplicate = new_handle();
  return S_OK;
}

void STDMETHODCALLTYPE close_handle(Object*, void* handle) {
  if (user_of(handle)) g_user.handles.fetch_sub(1);
}

int32_t STDMETHODCALLTYPE compare(Object*, void* first, void* second) {
  return first == second ? 0 : (first < second ? -1 : 1);
}

HRESULT STDMETHODCALLTYPE get_max_users(Object*, uint32_t* count) {
  if (!count) return E_INVALIDARG;
  *count = 1;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_local_id(Object*, void* handle, uint64_t* local_id) {
  if (!local_id || !user_of(handle)) return E_INVALIDARG;
  *local_id = kLocalId;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE find_user_by_local_id(Object*, uint64_t local_id, void** handle) {
  if (!handle) return E_INVALIDARG;
  *handle = nullptr;
  if (!present() || local_id != kLocalId) return kUserNotFound;
  *handle = new_handle();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_id(Object*, void* handle, uint64_t* id) {
  if (!id || !user_of(handle)) return E_INVALIDARG;
  *id = user_id();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE find_user_by_id(Object*, uint64_t id, void** handle) {
  if (!handle) return E_INVALIDARG;
  *handle = nullptr;
  if (!present() || id != user_id()) return kUserNotFound;
  *handle = new_handle();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_is_guest(Object*, void* handle, bool* guest) {
  if (!guest || !user_of(handle)) return E_INVALIDARG;
  *guest = false;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_state(Object*, void* handle, uint32_t* state) {
  if (!state || !user_of(handle)) return E_INVALIDARG;
  *state = kSignedIn;
  return S_OK;
}

// XUserGamertagComponent: 0 classic, 1 modern, 2 the modern suffix, 3 the
// unique modern form. The local user's name has no suffix.
HRESULT STDMETHODCALLTYPE get_gamertag(Object*, void* handle, uint32_t component, size_t size, char* buffer,
                                       size_t* used) {
  if (!buffer || !user_of(handle) || component > 3) return E_INVALIDARG;
  const std::string text = component == 2 ? std::string() : std::string(kGamertag);
  if (size < text.size() + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  if (used) *used = text.size() + 1;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_age_group(Object*, void* handle, uint32_t* group) {
  if (!group || !user_of(handle)) return E_INVALIDARG;
  *group = kAdult;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE check_privilege(Object*, void* handle, uint32_t options, uint32_t privilege, bool* has,
                                          uint32_t* reasons) {
  if (!has || !user_of(handle)) return E_INVALIDARG;
  *has = false;
  if (reasons) *reasons = kRestricted;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    LIB_LOG(holy::lib::kInfo, "xuser", "privilege %u (options 0x%x) of the local user: denied (no online service)",
            privilege, options);
  }
  return S_OK;
}

std::atomic<uint64_t> g_next_token{1};

HRESULT STDMETHODCALLTYPE register_for_change_event(Object*, void*, void*, void* callback, RegistrationToken* token) {
  if (!callback || !token) return E_INVALIDARG;
  token->token = g_next_token.fetch_add(1);
  return S_OK;
}

void STDMETHODCALLTYPE unregister_for_change_event(Object*, RegistrationToken, bool) {}

}  // namespace

bool local_user_id(void* handle, uint64_t* id) {
  if (!user_of(handle) || !id) return false;
  *id = user_id();
  return true;
}

void register_user() {
  // {01acd177-91f9-4763-a38e-ccbb55ce32e0}
  constexpr GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
  Interface& user = define_interface(kUser, "user");
  set_method(user, 3, "XUserDuplicateHandle", &duplicate_handle);
  set_method(user, 4, "XUserCloseHandle", &close_handle);
  set_method(user, 5, "XUserCompare", &compare);
  set_method(user, 6, "XUserGetMaxUsers", &get_max_users);
  set_method(user, 7, "XUserAddAsync", &add_async);
  set_method(user, 8, "XUserAddResult", &add_result);
  set_method(user, 9, "XUserGetLocalId", &get_local_id);
  set_method(user, 10, "XUserFindUserByLocalId", &find_user_by_local_id);
  set_method(user, 11, "XUserGetId", &get_id);
  set_method(user, 12, "XUserFindUserById", &find_user_by_id);
  set_method(user, 13, "XUserGetIsGuest", &get_is_guest);
  set_method(user, 14, "XUserGetState", &get_state);
  set_method(user, 19, "XUserGetAgeGroup", &get_age_group);
  set_method(user, 20, "XUserCheckPrivilege", &check_privilege);
  set_method(user, 33, "XUserRegisterForChangeEvent", &register_for_change_event);
  set_method(user, 34, "XUserUnregisterForChangeEvent", &unregister_for_change_event);
  // {cef4fac0-7676-4a94-a119-4c43f9eb5b74} (provider: the user service)
  constexpr GUID kUserGamertag = {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
  Interface& gamertag = define_interface(kUserGamertag, "user-gamertag");
  set_method(gamertag, 3, "XUserGetGamertag", &get_gamertag);
}

}  // namespace holy::xgr
