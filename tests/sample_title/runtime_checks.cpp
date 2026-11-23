// Sample title checks of the game runtime, reached the way a title's
// statically linked client library reaches it: load the runtime by name, look
// up its entry points, query provider interfaces and call through their
// vtables. Interface ids and slots are the ones recorded in
// docs/PLATFORM_NOTES.md.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "tests/sample_title/checks.h"

namespace {

// --- runtime plumbing ---------------------------------------------------------

using QueryApiImplFn = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
using InitializeApiImplEx2Fn = HRESULT(WINAPI*)(uint64_t, uint64_t, uint64_t, const void*);

QueryApiImplFn g_query = nullptr;

template <typename R, typename... Args>
R call(void* object, int slot, Args... args) {
  using Fn = R(STDMETHODCALLTYPE*)(void*, Args...);
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot])(object, args...);
}

void* query(const GUID& provider, const GUID& iid) {
  void* object = nullptr;
  if (!g_query || FAILED(g_query(&provider, &iid, &object))) return nullptr;
  return object;
}

void release(void* object) {
  if (object) call<ULONG>(object, 2);
}

constexpr GUID kCore = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
constexpr GUID kNetworkingProvider = {0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
constexpr GUID kNetworking = {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};
constexpr GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
constexpr GUID kAnalytics = {0xb884675d, 0xb738, 0x4a9c, {0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b}};
constexpr GUID kIdentityProvider = {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
constexpr GUID kIdentity = {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
constexpr GUID kGameProvider = {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
constexpr GUID kGame = {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};

// Core interface slots.
enum CoreSlot {
  kAsyncGetStatus = 3,
  kAsyncGetResultSize = 4,
  kAsyncRun = 6,
  kAsyncBegin = 7,
  kAsyncSchedule = 9,
  kAsyncComplete = 10,
  kAsyncGetResult = 11,
  kQueueCreate = 12,
  kQueueDispatch = 16,
  kQueueCloseHandle = 17,
  kQueueSubmitDelayedCallback = 19,
  kQueueTerminate = 22,
  kQueueGetCurrentProcessTaskQueue = 25,
  kAssertNotTimeSensitive = 29,
};

// Documented public types.
struct AsyncBlock {
  void* queue;
  void* context;
  void(CALLBACK* callback)(AsyncBlock*);
  unsigned char internal[sizeof(void*) * 4];
};
struct AsyncProviderData {
  AsyncBlock* async;
  size_t buffer_size;
  void* buffer;
  void* context;
};
enum : uint32_t { kOpBegin = 0, kOpDoWork = 1, kOpGetResult = 2, kOpCancel = 3, kOpCleanup = 4 };
enum : uint32_t { kManual = 0, kThreadPool = 1 };
enum : uint32_t { kWorkPort = 0, kCompletionPort = 1 };

void* g_core = nullptr;

// --- task queues ------------------------------------------------------------------

std::atomic<int> g_callbacks{0};
std::atomic<int> g_canceled{0};
std::atomic<int> g_terminated{0};

void CALLBACK count_callback(void*, bool canceled) {
  ++g_callbacks;
  if (canceled) ++g_canceled;
}
void CALLBACK terminated_callback(void*) {
  ++g_terminated;
}

void test_task_queue() {
  void* queue = nullptr;
  HRESULT hr = call<HRESULT>(g_core, kQueueCreate, kManual, kManual, &queue);
  check(SUCCEEDED(hr) && queue, "XTaskQueueCreate creates a manual queue");
  if (!queue) return;
  hr = call<HRESULT>(g_core, kQueueSubmitDelayedCallback, queue, kWorkPort, 0u, nullptr, &count_callback);
  check(SUCCEEDED(hr) && g_callbacks == 0, "callbacks on a manual port wait for dispatch");
  check(call<bool>(g_core, kQueueDispatch, queue, kWorkPort, 0u) && g_callbacks == 1,
        "XTaskQueueDispatch runs a queued callback");
  check(!call<bool>(g_core, kQueueDispatch, queue, kWorkPort, 0u), "XTaskQueueDispatch reports an empty port");

  call<HRESULT>(g_core, kQueueSubmitDelayedCallback, queue, kWorkPort, 20u, nullptr, &count_callback);
  check(call<bool>(g_core, kQueueDispatch, queue, kWorkPort, 2000u) && g_callbacks == 2,
        "a delayed callback becomes dispatchable after its delay");

  call<HRESULT>(g_core, kQueueSubmitDelayedCallback, queue, kCompletionPort, 0u, nullptr, &count_callback);
  hr = call<HRESULT>(g_core, kQueueTerminate, queue, true, nullptr, &terminated_callback);
  check(SUCCEEDED(hr) && g_canceled == 1 && g_terminated == 1,
        "termination cancels pending callbacks and reports completion");
  hr = call<HRESULT>(g_core, kQueueSubmitDelayedCallback, queue, kWorkPort, 0u, nullptr, &count_callback);
  check(FAILED(hr), "a terminated queue refuses new callbacks");
  call<void>(g_core, kQueueCloseHandle, queue);

  void* process_queue = nullptr;
  check(call<bool>(g_core, kQueueGetCurrentProcessTaskQueue, &process_queue) && process_queue,
        "a process task queue exists by default");
  call<void>(g_core, kQueueCloseHandle, process_queue);
  call<void>(g_core, kAssertNotTimeSensitive);
}

// --- asynchronous calls ---------------------------------------------------------

std::atomic<int> g_work_runs{0};
std::atomic<int> g_completions{0};
std::atomic<int> g_cleanups{0};
const char kResultIdentity = 0;

HRESULT CALLBACK run_work(AsyncBlock*) {
  ++g_work_runs;
  return S_OK;
}
void CALLBACK completion(AsyncBlock*) {
  ++g_completions;
}

HRESULT CALLBACK provider(uint32_t op, const AsyncProviderData* data) {
  switch (op) {
    case kOpBegin:
      return call<HRESULT>(g_core, kAsyncSchedule, data->async, 0u);
    case kOpDoWork:
      call<void>(g_core, kAsyncComplete, data->async, S_OK, size_t{4});
      return S_OK;
    case kOpGetResult:
      if (data->buffer_size < 4) return E_INVALIDARG;
      std::memcpy(data->buffer, "data", 4);
      return S_OK;
    case kOpCleanup:
      ++g_cleanups;
      return S_OK;
    default:
      return S_OK;
  }
}

void test_async() {
  AsyncBlock block{};
  block.callback = &completion;
  HRESULT hr = call<HRESULT>(g_core, kAsyncRun, &block, &run_work);
  check(SUCCEEDED(hr), "XAsyncRun starts work on the process queue");
  hr = call<HRESULT>(g_core, kAsyncGetStatus, &block, true);
  check(hr == S_OK && g_work_runs == 1 && g_completions == 1,
        "waiting for an async call returns after its work and completion routine");

  AsyncBlock with_result{};
  hr = call<HRESULT>(g_core, kAsyncBegin, &with_result, nullptr, &kResultIdentity, "sample", &provider);
  check(SUCCEEDED(hr), "XAsyncBegin starts a provider call");
  hr = call<HRESULT>(g_core, kAsyncGetStatus, &with_result, true);
  size_t size = 0;
  call<HRESULT>(g_core, kAsyncGetResultSize, &with_result, &size);
  check(hr == S_OK && size == 4, "a completed call reports its result size");
  char buffer[8] = {};
  size_t used = 0;
  hr = call<HRESULT>(g_core, kAsyncGetResult, &with_result, &kResultIdentity, sizeof(buffer), buffer, &used);
  check(hr == S_OK && used == 4 && std::memcmp(buffer, "data", 4) == 0 && g_cleanups == 1,
        "XAsyncGetResult copies the result and cleans the provider up");
}

// --- services ------------------------------------------------------------------------

void test_networking() {
  void* networking = query(kNetworkingProvider, kNetworking);
  check(networking != nullptr, "the networking service is available");
  if (!networking) return;
  struct {
    uint32_t level, cost, iana;
    bool initialized, approaching, over, roaming;
  } hint{};
  const HRESULT hr = call<HRESULT>(networking, 15, &hint);
  if (g_expect_online) {
    check(hr == S_OK && hint.initialized, "with the host network, the connectivity hint is the host's");
  } else {
    check(hr == S_OK && hint.initialized && hint.level == 1, "offline, the connectivity hint reports no connectivity");
  }
  release(networking);
}

void test_user() {
  void* user = query(kUser, kUser);
  check(user != nullptr, "the user service is available");
  if (!user) return;
  AsyncBlock block{};
  HRESULT hr = call<HRESULT>(user, 7, 1u, &block);  // AddDefaultUserSilently
  call<HRESULT>(g_core, kAsyncGetStatus, &block, true);
  void* handle = reinterpret_cast<void*>(1);
  hr = call<HRESULT>(user, 8, &block, &handle);
  check(hr == static_cast<HRESULT>(0x89245106) && handle == nullptr,
        "adding the default user silently reports that nobody is signed in");
  AsyncBlock picker{};
  call<HRESULT>(user, 7, 0u, &picker);
  call<HRESULT>(g_core, kAsyncGetStatus, &picker, true);
  hr = call<HRESULT>(user, 8, &picker, &handle);
  check(hr == E_ABORT, "an account picker request ends as dismissed");
  release(user);
}

void test_system() {
  void* analytics = query(kAnalytics, kAnalytics);
  struct {
    uint16_t version[4];
    uint16_t hosting[4];
    char family[64];
    char form[64];
  } info{};
  if (analytics) call<void*>(analytics, 3, &info);
  check(analytics && info.version[0] == 10 && std::strcmp(info.family, "Windows.Xbox") == 0 &&
            std::strcmp(info.form, "Xbox Series X") == 0,
        "analytics info reports the host version and the configured console");
  release(analytics);

  void* game = query(kGameProvider, kGame);
  uint32_t title_id = 0;
  check(game && call<HRESULT>(game, 3, &title_id) == S_OK && title_id == 0x0A0B0C0D,
        "the title id comes from the package manifest");
  release(game);

  void* identity = query(kIdentityProvider, kIdentity);
  char text[64] = {};
  size_t used = 0;
  check(identity && call<HRESULT>(identity, 4, sizeof(text), text, &used) == S_OK &&
            std::strcmp(text, "RETAIL") == 0 && used == 7,
        "the sandbox is RETAIL");
  check(identity && call<HRESULT>(identity, 3, size_t{39}, text, &used) == S_OK && used == 39,
        "the console id fills the documented 39-byte buffer");
  char again[64] = {};
  check(identity && call<HRESULT>(identity, 3, size_t{39}, again, &used) == S_OK && std::strcmp(text, again) == 0,
        "the console id is stable");
  check(identity && call<HRESULT>(identity, 5, size_t{45}, text, &used) == S_OK && used == 45,
        "the app-specific device id fills the documented 45-byte buffer");
  release(identity);
}

}  // namespace

void test_game_runtime() {
  HMODULE runtime = LoadLibraryW(L"xgameruntime.dll");
  check(runtime != nullptr, "the game runtime loads by name");
  if (!runtime) return;
  const char* entry_points[] = {"QueryApiImpl",        "InitializeApiImpl", "InitializeApiImplEx",
                                "InitializeApiImplEx2", "XErrorReport",      "UninitializeApiImpl"};
  bool all = true;
  for (const char* name : entry_points) all = all && GetProcAddress(runtime, name) != nullptr;
  check(all, "the game runtime exports the client library's entry points");
  if (!all) return;
  g_query = reinterpret_cast<QueryApiImplFn>(GetProcAddress(runtime, "QueryApiImpl"));
  const auto initialize = reinterpret_cast<InitializeApiImplEx2Fn>(GetProcAddress(runtime, "InitializeApiImplEx2"));
  check(initialize(0x0000633600002712ull, 0x000065f400001eddull, 2, nullptr) == S_OK,
        "InitializeApiImplEx2 accepts the client library's call");
  g_core = query(kCore, kCore);
  check(g_core != nullptr, "the core interface is available");
  if (!g_core) return;
  test_task_queue();
  test_async();
  test_networking();
  test_user();
  test_system();
}
