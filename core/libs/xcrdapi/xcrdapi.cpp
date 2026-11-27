// xcrdapi.dll: the console's fast-storage API (a DirectStorage variant).
//
// Entry point (observed): DStorageGetFactory(REFIID, void**), the signature of
// the public desktop dstorage.h function of the same name. The console factory
// has its own interface id; its methods are COM-style.
//
// Factory slots (observed from the title's calls):
//    4 OpenFile(path, riid, void** file)
//    5 CreateStatusArray(uint32_t capacity, const char* name, riid, void** array)
//    8 called once with 32 MiB before any queue exists: the staging buffer size
//   10 CreateQueue(const QueueDesc*, riid, void** queue); the descriptor has the
//      layout of the public DSTORAGE_QUEUE_DESC (source type, capacity,
//      priority, name) followed by a field the title sets to 1
//
// Present state: work in progress. Unbound slots report themselves.
#include <windows.h>
#include <d3d12.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "core/libs/inflate.h"
#include "core/libs/support.h"
#include "core/libs/vtable.h"

namespace {

constexpr char kLibrary[] = "xcrdapi.dll";
constexpr int kSlots = 48;

struct Object {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
};

HRESULT STDMETHODCALLTYPE query_interface(Object* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE add_ref(Object*) {
  return 2;
}
ULONG STDMETHODCALLTYPE release(Object*) {
  return 1;
}

template <int N>
std::array<void*, N> com_vtable() {
  auto table = holy::lib::make_reporting_vtable<N>();
  table[0] = reinterpret_cast<void*>(&query_interface);
  table[1] = reinterpret_cast<void*>(&add_ref);
  table[2] = reinterpret_cast<void*>(&release);
  return table;
}

// --- files ------------------------------------------------------------------------

const holy::lib::VtableInfo kFileInfo{kLibrary, "IDStorageFileX"};

struct File {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  HANDLE handle = INVALID_HANDLE_VALUE;
};

ULONG STDMETHODCALLTYPE file_add_ref(File* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE file_release(File* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    if (self->handle != INVALID_HANDLE_VALUE) CloseHandle(self->handle);
    delete self;
  }
  return static_cast<ULONG>(left);
}

const auto kFileVtable = [] {
  auto table = com_vtable<kSlots>();
  table[1] = reinterpret_cast<void*>(&file_add_ref);
  table[2] = reinterpret_cast<void*>(&file_release);
  return table;
}();

// Opens a package or scratch file for reading. Paths are the title's (mounted
// drives, package-relative) and are translated like every other file access.
HRESULT STDMETHODCALLTYPE open_file(Object*, const wchar_t* path, REFIID, void** file) {
  if (!file) return E_POINTER;
  *file = nullptr;
  if (!path) return E_INVALIDARG;
  wchar_t host[1024];
  if (!HolyTranslatePath(path, host, std::size(host))) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  const HANDLE handle = CreateFileW(host, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    LIB_LOG(holy::lib::kDebug, "dstorage", "open failed (error %lu)", error);
    return HRESULT_FROM_WIN32(error);
  }
  auto* object = new File();
  object->vtable = kFileVtable.data();
  object->info = &kFileInfo;
  object->handle = handle;
  *file = object;
  return S_OK;
}

// --- status arrays ----------------------------------------------------------------
//
// Observed: factory slot 5 CreateStatusArray(capacity, name, riid, void** out),
// the desktop's signature. Entries are completed by queues (EnqueueStatus);
// slots 3 IsComplete(index) and 4 GetHResult(index) keep the desktop order
// (assumed until titles are seen to call them).

const holy::lib::VtableInfo kStatusArrayInfo{kLibrary, "IDStorageStatusArrayX"};

struct StatusArray {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  uint32_t capacity = 0;
  std::unique_ptr<std::atomic<int64_t>[]> entries;  // HRESULT once complete, kPending before
};

constexpr int64_t kPending = INT64_MIN;

ULONG STDMETHODCALLTYPE status_add_ref(StatusArray* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE status_release(StatusArray* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}
bool STDMETHODCALLTYPE status_is_complete(StatusArray* self, uint32_t index) {
  return index < self->capacity && self->entries[index].load() != kPending;
}
HRESULT STDMETHODCALLTYPE status_get_hresult(StatusArray* self, uint32_t index) {
  if (index >= self->capacity) return E_INVALIDARG;
  const int64_t value = self->entries[index].load();
  return value == kPending ? E_PENDING : static_cast<HRESULT>(value);
}

const auto kStatusArrayVtable = [] {
  auto table = com_vtable<kSlots>();
  table[1] = reinterpret_cast<void*>(&status_add_ref);
  table[2] = reinterpret_cast<void*>(&status_release);
  table[3] = reinterpret_cast<void*>(&status_is_complete);
  table[4] = reinterpret_cast<void*>(&status_get_hresult);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_status_array(Object*, uint32_t capacity, const char* name, REFIID, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (capacity == 0) return E_INVALIDARG;
  auto* array = new StatusArray();
  array->vtable = kStatusArrayVtable.data();
  array->info = &kStatusArrayInfo;
  array->capacity = capacity;
  array->entries.reset(new std::atomic<int64_t>[capacity]);
  for (uint32_t i = 0; i < capacity; ++i) array->entries[i].store(kPending);
  LIB_LOG(holy::lib::kDebug, "dstorage", "status array of %u entries (%s)", capacity, name ? name : "unnamed");
  *out = array;
  return S_OK;
}

// --- queues -----------------------------------------------------------------------
//
// Observed: factory slot 10 CreateQueue(const QueueDesc*, riid, void** out).
// The description starts with the desktop DSTORAGE_QUEUE_DESC fields (source
// type, capacity, priority, name, device); a field the title sets to 1
// follows. Queue methods are established from the title's calls.

const holy::lib::VtableInfo kQueueInfo{kLibrary, "IDStorageQueueX"};

struct Queue {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  uint32_t source_type = 0;
  uint16_t capacity = 0;
  std::atomic<HRESULT> error{S_OK};  // first failure since the last status
};

ULONG STDMETHODCALLTYPE queue_add_ref(Queue* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE queue_release(Queue* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}

// Observed: queue slot 3 EnqueueRequest(const Request*). Request layout
// (from how titles fill it): options at 0 (bit 22: the source is memory;
// bit 0: set for the title's format 3, a zlib stream), destination
// address at 8, destination size at 0x18, source at 0x20 (a file object, or
// a memory address), file offset at 0x28, source size at 0x30.
// Requests are carried out when enqueued: completion is only earlier than
// the console's.
constexpr uint64_t kSourceMemory = 1ull << 22;
constexpr uint64_t kCompressed = 1ull;

HRESULT read_into(File* file, uint64_t offset, void* destination, uint32_t size) {
  // One event per thread, reused (requests come by the thousand).
  thread_local HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  overlapped.hEvent = event;
  ResetEvent(event);
  HolyEnsureCommitted(destination, size);
  DWORD got = 0;
  BOOL ok = ReadFile(file->handle, destination, size, nullptr, &overlapped);
  if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
  if (ok) ok = GetOverlappedResult(file->handle, &overlapped, &got, TRUE);
  if (!ok) return HRESULT_FROM_WIN32(GetLastError());
  return got == size ? S_OK : HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
}

HRESULT enqueue_request_body(const uint8_t* request);

HRESULT STDMETHODCALLTYPE enqueue_request(Queue* self, const uint8_t* request) {
  const HRESULT hr = enqueue_request_body(request);
  if (FAILED(hr)) {
    HRESULT expected = S_OK;
    self->error.compare_exchange_strong(expected, hr);
  }
  return hr;
}

// Observed: slot 4 EnqueueStatus(status array, index), the desktop's order;
// the entry gets the first failure since the previous status (requests are
// complete by then). Assumed in the desktop order: 5 EnqueueSignal(fence,
// value), 6 Submit().
void STDMETHODCALLTYPE enqueue_status(Queue* self, StatusArray* array, uint32_t index) {
  if (!array || index >= array->capacity) return;
  array->entries[index].store(self->error.exchange(S_OK));
}

void STDMETHODCALLTYPE enqueue_signal(Queue*, IUnknown* fence, uint64_t value) {
  // The title's fence keeps the desktop ID3D12Fence order: Signal at slot 10.
  if (fence) reinterpret_cast<ID3D12Fence*>(fence)->Signal(value);
}

void STDMETHODCALLTYPE submit(Queue*) {}

HRESULT enqueue_request_body(const uint8_t* request) {
  if (!request) return E_POINTER;
  uint64_t options = 0, destination = 0, source = 0, offset = 0;
  uint32_t destination_size = 0, source_size = 0;
  std::memcpy(&options, request, 8);
  std::memcpy(&destination, request + 8, 8);
  std::memcpy(&destination_size, request + 0x18, 4);
  std::memcpy(&source, request + 0x20, 8);
  std::memcpy(&offset, request + 0x28, 8);
  std::memcpy(&source_size, request + 0x30, 4);
  LIB_LOG(holy::lib::kDebug, "dstorage",
          "request options 0x%llx destination %p (0x%x) source %p offset 0x%llx size 0x%x",
          static_cast<unsigned long long>(options), reinterpret_cast<void*>(destination), destination_size,
          reinterpret_cast<void*>(source), static_cast<unsigned long long>(offset), source_size);
  if (!destination || !source) return E_INVALIDARG;
  if (options & kCompressed) {
    // Observed: the streams are zlib (RFC 1950) streams, inflated straight
    // into the destination.
    thread_local std::vector<uint8_t> packed;
    const uint8_t* stream = reinterpret_cast<const uint8_t*>(source + offset);
    HRESULT hr = S_OK;
    if (!(options & kSourceMemory)) {
      if (packed.size() < source_size) packed.resize(source_size);
      hr = read_into(reinterpret_cast<File*>(source), offset, packed.data(), source_size);
      stream = packed.data();
    }
    if (SUCCEEDED(hr)) {
      HolyEnsureCommitted(reinterpret_cast<void*>(destination), destination_size);
      size_t produced = 0;
      if (!holy::lib::zlib_inflate_into(stream, source_size, reinterpret_cast<uint8_t*>(destination), destination_size,
                                        &produced)) {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      }
    }
    if (FAILED(hr)) {
      LIB_LOG(holy::lib::kError, "dstorage", "compressed request failed (0x%08lX)", static_cast<unsigned long>(hr));
      return hr;
    }
    return S_OK;
  }
  if (options & kSourceMemory) {
    std::memcpy(reinterpret_cast<void*>(destination), reinterpret_cast<const void*>(source + offset), source_size);
    return S_OK;
  }
  auto* file = reinterpret_cast<File*>(source);
  const HRESULT hr = read_into(file, offset, reinterpret_cast<void*>(destination), source_size);
  if (FAILED(hr)) LIB_LOG(holy::lib::kError, "dstorage", "read failed (0x%08lX)", static_cast<unsigned long>(hr));
  return hr;
}

const auto kQueueVtable = [] {
  auto table = com_vtable<kSlots>();
  table[1] = reinterpret_cast<void*>(&queue_add_ref);
  table[2] = reinterpret_cast<void*>(&queue_release);
  table[3] = reinterpret_cast<void*>(&enqueue_request);
  table[4] = reinterpret_cast<void*>(&enqueue_status);
  table[5] = reinterpret_cast<void*>(&enqueue_signal);
  table[6] = reinterpret_cast<void*>(&submit);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_queue(Object*, const uint8_t* desc, REFIID, void** out) {
  if (!out || !desc) return E_POINTER;
  *out = nullptr;
  uint32_t words[10] = {};
  std::memcpy(words, desc, sizeof(words));
  auto* queue = new Queue();
  queue->vtable = kQueueVtable.data();
  queue->info = &kQueueInfo;
  std::memcpy(&queue->source_type, desc, 4);
  std::memcpy(&queue->capacity, desc + 4, 2);
  LIB_LOG(holy::lib::kDebug, "dstorage", "queue: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", words[0],
          words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9]);
  *out = queue;
  return S_OK;
}

// --- factory ------------------------------------------------------------------

const holy::lib::VtableInfo kFactoryInfo{kLibrary, "IDStorageFactoryX"};

HRESULT STDMETHODCALLTYPE set_staging_buffer_size(Object*, uint32_t size) {
  LIB_LOG(holy::lib::kDebug, "dstorage", "staging buffer size 0x%x", size);
  return S_OK;
}

const auto kFactoryVtable = [] {
  auto table = com_vtable<kSlots>();
  table[4] = reinterpret_cast<void*>(&open_file);
  table[5] = reinterpret_cast<void*>(&create_status_array);
  table[8] = reinterpret_cast<void*>(&set_staging_buffer_size);
  table[10] = reinterpret_cast<void*>(&create_queue);
  return table;
}();

Object g_factory{kFactoryVtable.data(), &kFactoryInfo};

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DStorageGetFactory(REFIID riid, void** factory) {
  if (!factory) return E_POINTER;
  (void)riid;
  *factory = &g_factory;
  return S_OK;
}
