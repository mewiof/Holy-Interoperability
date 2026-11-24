// Sample title checks of the game runtime, reached the way a title's
// statically linked client library reaches it: load the runtime by name, look
// up its entry points, query provider interfaces and call through their
// vtables. Interface ids and slots are the ones recorded in
// docs/PLATFORM_NOTES.md.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
constexpr GUID kPackageProvider = {0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
constexpr GUID kPackage = {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
constexpr GUID kStorageProvider = {0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3e, 0x84, 0x3e}};
constexpr GUID kStorage = {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};
constexpr GUID kDisplayProvider = {0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
constexpr GUID kDisplay = {0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}};
constexpr GUID kSpeechProvider = {0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}};
constexpr GUID kSpeech = {0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}};

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

void CALLBACK package_found(void*, const void*) {}

void test_package() {
  void* package = query(kPackageProvider, kPackage);
  check(package && call<bool>(package, 4), "the title runs as a packaged process");
  if (!package) return;
  char id[33] = {};
  check(call<HRESULT>(package, 3, sizeof(id), id) == S_OK && id[0] != 0, "the package has an identifier");
  void* monitor = nullptr;
  HRESULT hr = call<HRESULT>(package, 5, static_cast<const char*>(id), 0u, nullptr, 1000u, nullptr, &monitor);
  struct {
    uint64_t total, installed, launch;
    bool launchable, completed;
  } progress{};
  if (SUCCEEDED(hr)) call<void>(package, 7, monitor, &progress);
  check(SUCCEEDED(hr) && progress.completed && progress.launchable && progress.total > 0 &&
            progress.installed == progress.total,
        "the installation monitor reports a complete installation");
  if (monitor) call<void>(package, 6, monitor);
  check(call<HRESULT>(package, 39, 1u, 0u, nullptr, reinterpret_cast<void*>(&package_found)) == S_OK,
        "enumerating content packages finds none and succeeds");
  release(package);
}

void test_local_storage() {
  void* storage = query(kStorageProvider, kStorage);
  check(storage != nullptr, "persistent local storage is available");
  if (!storage) return;
  char path[512] = {};
  size_t used = 0;
  const HRESULT hr = call<HRESULT>(storage, 4, sizeof(path), path, &used);
  check(hr == S_OK && used == std::strlen(path) + 1, "persistent local storage has a path");
  std::string file = std::string(path) + "\\sample.bin";
  HANDLE handle = CreateFileA(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD written = 0;
  const bool wrote = handle != INVALID_HANDLE_VALUE && WriteFile(handle, "1234", 4, &written, nullptr);
  if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  check(wrote, "persistent local storage is writable");
  struct {
    uint64_t available, total_free, used, total;
  } info{};
  check(call<HRESULT>(storage, 5, &info) == S_OK && info.total == 64ull << 20 && info.used >= 4 &&
            info.available <= info.total - info.used,
        "space info reports the manifest's quota and the bytes in use");
  DeleteFileA(file.c_str());
  release(storage);
}

void test_display() {
  void* display = query(kDisplayProvider, kDisplay);
  float info[3] = {1, 1, 1};
  check(display && call<uint32_t>(display, 4, 0u, info) == 2 && info[1] == 0.0f,
        "HDR output is reported disabled");
  release(display);
}

// Documented public layout.
struct VoiceInformation {
  const char* description;
  const char* display_name;
  uint32_t gender;
  const char* voice_id;
  const char* language;
};

bool record_voice(const VoiceInformation* information, void* context) {
  auto* first = static_cast<std::string*>(context);
  if (first->empty() && information && information->voice_id) *first = information->voice_id;
  return true;
}

void test_speech() {
  void* speech = query(kSpeechProvider, kSpeech);
  std::string voice;
  check(speech && call<HRESULT>(speech, 8, static_cast<void*>(&voice), &record_voice) == S_OK,
        "the speech synthesizer enumerates the host's voices");
  void* synthesizer = nullptr;
  check(speech && call<HRESULT>(speech, 9, &synthesizer) == S_OK && synthesizer, "a speech synthesizer is created");
  if (!speech || !synthesizer) {
    release(speech);
    return;
  }
  check(call<HRESULT>(speech, 12, synthesizer, "no such voice") == HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
        "an unknown voice id is refused as not found");
  void* stream = nullptr;
  check(call<HRESULT>(speech, 15, stream, static_cast<size_t*>(nullptr)) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
        "an invalid stream handle is refused as not found");
  if (voice.empty()) {
    std::printf("      (the host has no voices: synthesis not checked)\n");
  } else {
    check(call<HRESULT>(speech, 12, synthesizer, voice.c_str()) == S_OK, "an enumerated voice can be chosen");
    const HRESULT made = call<HRESULT>(speech, 13, synthesizer, "Hello.", &stream);
    if (FAILED(made)) {
      // The host's engine refuses synthesis at low integrity (the sandbox):
      // its error comes back, with no stream.
      std::printf("      (the host engine refused synthesis: 0x%08lx)\n", static_cast<unsigned long>(made));
      check(stream == nullptr, "a refused synthesis returns the host's error and no stream");
    } else {
      size_t size = 0;
      check(stream && call<HRESULT>(speech, 15, stream, &size) == S_OK && size > 44,
            "plain text is synthesized into a stream");
      std::vector<uint8_t> wave(size);
      size_t used = 0;
      check(stream && call<HRESULT>(speech, 16, stream, size, static_cast<void*>(wave.data()), &used) == S_OK &&
                used == size && size > 44 && std::memcmp(wave.data(), "RIFF", 4) == 0 &&
                std::memcmp(wave.data() + 8, "WAVE", 4) == 0,
            "the stream holds a WAVE container");
      if (stream) call<void>(speech, 14, stream);
    }
  }
  call<void>(speech, 10, synthesizer);
  release(speech);
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
  test_package();
  test_local_storage();
  test_display();
  test_speech();
}
