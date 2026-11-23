// Networking service: connectivity information.
//
// Observed: interface {bf2346b2-39af-4658-b5ea-44713c7e83b3}, reached through
// provider {37e56907-2f10-41e8-b72f-36edb185331a}. Slots identified from their
// thunks against the documented XNetworking.h declarations:
//
//   15 XNetworkingGetConnectivityHint(XNetworkingConnectivityHint*)
//   16 XNetworkingRegisterConnectivityHintChanged(queue, context, callback, token*)
//   17 XNetworkingUnregisterConnectivityHintChanged(token, bool wait)
//
// Documented: XNetworkingConnectivityHint is { level, cost, ianaInterfaceType,
// networkInitialized, approachingDataLimit, overDataLimit, roaming }, with the
// level and cost enumerations numbered like the host's
// NL_NETWORK_CONNECTIVITY_LEVEL_HINT and NL_NETWORK_CONNECTIVITY_COST_HINT.
// Observed: titles treat level 3 (internet access) and 4 (constrained internet
// access) as online, and wait for networkInitialized before trusting the rest.
//
// Offline (the default): the network stack is initialised and reports no
// connectivity, which is what a console with its network disconnected
// reports. With the host network allowed, the host's own connectivity hint is
// passed through. Change notifications register successfully; offline the
// hint never changes, so they never fire.
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/libs/xgameruntime/task_queue.h"

namespace holy::xgr {
namespace {

struct ConnectivityHint {
  uint32_t level;
  uint32_t cost;
  uint32_t iana_interface_type;
  bool network_initialized;
  bool approaching_data_limit;
  bool over_data_limit;
  bool roaming;
};
static_assert(sizeof(ConnectivityHint) == 16);

constexpr uint32_t kLevelNone = 1;
constexpr uint32_t kCostUnknown = 0;

// The host's NL_NETWORK_CONNECTIVITY_HINT (iphlpapi, Windows 10 2004+).
struct HostHint {
  uint32_t level;
  uint32_t cost;
  BOOLEAN approaching_data_limit;
  BOOLEAN over_data_limit;
  BOOLEAN roaming;
};

bool host_hint(ConnectivityHint* hint) {
  using Fn = DWORD(WINAPI*)(HostHint*);
  static const auto fn = reinterpret_cast<Fn>(
      GetProcAddress(LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32),
                     "GetNetworkConnectivityHint"));
  HostHint host{};
  if (!fn || fn(&host) != NO_ERROR) return false;
  hint->level = host.level <= 4 ? host.level : 0;
  hint->cost = host.cost <= 3 ? host.cost : 0;
  hint->iana_interface_type = 0;
  hint->network_initialized = true;
  hint->approaching_data_limit = host.approaching_data_limit != 0;
  hint->over_data_limit = host.over_data_limit != 0;
  hint->roaming = host.roaming != 0;
  return true;
}

HRESULT STDMETHODCALLTYPE get_connectivity_hint(Object*, ConnectivityHint* hint) {
  if (!hint) return E_INVALIDARG;
  *hint = ConnectivityHint{kLevelNone, kCostUnknown, 0, true, false, false, false};
  if (!HolyIsOffline() && !host_hint(hint)) {
    *hint = ConnectivityHint{0, kCostUnknown, 0, true, false, false, false};
  }
  return S_OK;
}

std::atomic<uint64_t> g_next_token{1};

HRESULT STDMETHODCALLTYPE register_connectivity_hint_changed(Object*, void*, void*, void* callback,
                                                             RegistrationToken* token) {
  if (!callback || !token) return E_INVALIDARG;
  token->token = g_next_token.fetch_add(1);
  return S_OK;
}

void STDMETHODCALLTYPE unregister_connectivity_hint_changed(Object*, RegistrationToken, bool) {}

}  // namespace

void register_networking() {
  // {bf2346b2-39af-4658-b5ea-44713c7e83b3}
  constexpr GUID kNetworking = {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};
  Interface& networking = define_interface(kNetworking, "networking");
  set_method(networking, 15, "XNetworkingGetConnectivityHint", &get_connectivity_hint);
  set_method(networking, 16, "XNetworkingRegisterConnectivityHintChanged", &register_connectivity_hint_changed);
  set_method(networking, 17, "XNetworkingUnregisterConnectivityHintChanged", &unregister_connectivity_hint_changed);
}

}  // namespace holy::xgr
