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
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/libs/inflate.h"
#include "core/libs/support.h"
#include "core/libs/texture_codec.h"
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

// What a queue carries out in order: a request, a status (the first failure
// since the previous one), or a fence signal.
struct Operation {
  enum Kind { kRequest, kStatus, kSignal } kind = kRequest;
  uint64_t options = 0, destination = 0, page_offset = 0, source = 0, offset = 0;
  uint32_t destination_size = 0, source_size = 0, intermediate_size = 0;
  struct StatusArray* array = nullptr;
  uint32_t index = 0;
  IUnknown* fence = nullptr;
  uint64_t value = 0;
};

struct Queue {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  uint32_t source_type = 0;
  uint16_t capacity = 0;
  // Enqueued since the last submission, and submitted (the worker's).
  std::mutex lock;
  std::vector<Operation> pending;
  std::deque<Operation> submitted;
  std::condition_variable ready;
  HRESULT error = S_OK;  // first failure since the last status (the worker's)
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
// (from how titles fill it): options at 0, destination at 8, destination
// size at 0x18, source at 0x20 (a file object, or a memory address), file
// offset at 0x28, source size at 0x30.
// Options: bit 22, the source is memory; bit 0, the source is a zlib
// (RFC 1950) stream; bit 21, the destination is a list of physical page
// numbers (one 64-bit number per 64 KiB, as XMemAllocatePhysicalPages hands
// them out) with the offset into the first page in the low 16 bits of the
// field at 0x10, where the destination is otherwise an address. Observed: the
// title's texture streaming (bit 21 with zlib streams of 4 KiB, and with the
// format below) fills the lists after enqueueing, before submitting.
// Requests with any of bits 1-4 set (options 0x200002, 0x20000a, 0x20000c,
// 0x20000e seen) have sources smaller than their destinations (64 KiB and
// 1 MiB) and are not zlib streams: the console's texture codec
// (core/libs/texture_codec.h), bits 1-4 the block format number (1, 5, 6 and
// 7 seen). With bit 0 as well the source is a zlib stream inflating to the
// size at 0x34, then decoded (assumed: zlib first). The codec needs the
// format's constant tables, which this project does not ship: without them
// such requests fail (their status says so) and are counted, rather than
// leaving compressed bytes where the title expects texels.
constexpr uint64_t kSourceMemory = 1ull << 22;
constexpr uint64_t kPageDestination = 1ull << 21;
constexpr uint64_t kCompressed = 1ull;
constexpr uint64_t kOtherFormat = 0x1Eull;
constexpr size_t kPhysicalPage = 0x10000;
constexpr size_t kMaxListPages = 4096;  // 256 MiB of destination

HRESULT read_into(File* file, uint64_t offset, void* destination, uint32_t size) {
  // One event per thread, reused (requests come by the thousand).
  thread_local HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  overlapped.hEvent = event;
  ResetEvent(event);
  HolyEnsureCommitted(destination, size);
  HolyBeforeWrite(destination, size);
  DWORD got = 0;
  BOOL ok = ReadFile(file->handle, destination, size, nullptr, &overlapped);
  if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
  if (ok) ok = GetOverlappedResult(file->handle, &overlapped, &got, TRUE);
  if (!ok) return HRESULT_FROM_WIN32(GetLastError());
  return got == size ? S_OK : HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
}

HRESULT carry_out(const Operation& op);

// Queues carry out what was enqueued when it is submitted (the documented
// model), in order, on a worker thread of their own: titles enqueue reads into
// memory they map before submitting (observed: carried out at enqueue, reads
// landed in memory the title then mapped other pages over, and later reads
// of what they loaded found zeroes). A status entry turns pending when
// enqueued and gets the first failure since the previous status when the
// worker reaches it; fences are signalled after the operations before them.
// A queue whose enqueued operations reach its capacity submits them itself
// (the desktop's behaviour).
void worker(Queue* queue) {
  for (;;) {
    Operation op;
    {
      std::unique_lock<std::mutex> hold(queue->lock);
      queue->ready.wait(hold, [queue] { return !queue->submitted.empty(); });
      op = queue->submitted.front();
      queue->submitted.pop_front();
    }
    switch (op.kind) {
      case Operation::kRequest: {
        const HRESULT hr = carry_out(op);
        if (FAILED(hr) && SUCCEEDED(queue->error)) queue->error = hr;
        break;
      }
      case Operation::kStatus:
        op.array->entries[op.index].store(queue->error);
        queue->error = S_OK;
        reinterpret_cast<IUnknown*>(op.array)->Release();
        break;
      case Operation::kSignal:
        // The title's fence keeps the desktop ID3D12Fence order: Signal at slot 10.
        reinterpret_cast<ID3D12Fence*>(op.fence)->Signal(op.value);
        op.fence->Release();
        break;
    }
  }
}

void submit_locked(Queue* self) {
  if (self->pending.empty()) return;
  for (Operation& op : self->pending) self->submitted.push_back(op);
  self->pending.clear();
  self->ready.notify_one();
}

void enqueue(Queue* self, const Operation& op) {
  std::lock_guard<std::mutex> hold(self->lock);
  self->pending.push_back(op);
  if (self->capacity && self->pending.size() >= self->capacity) submit_locked(self);
}

HRESULT STDMETHODCALLTYPE enqueue_request(Queue* self, const uint8_t* request) {
  if (!request) return E_POINTER;
  Operation op;
  op.kind = Operation::kRequest;
  std::memcpy(&op.options, request, 8);
  std::memcpy(&op.destination, request + 8, 8);
  std::memcpy(&op.page_offset, request + 0x10, 8);
  std::memcpy(&op.destination_size, request + 0x18, 4);
  std::memcpy(&op.source, request + 0x20, 8);
  std::memcpy(&op.offset, request + 0x28, 8);
  std::memcpy(&op.source_size, request + 0x30, 4);
  std::memcpy(&op.intermediate_size, request + 0x34, 4);
  if (!op.destination || !op.source) return E_INVALIDARG;
  enqueue(self, op);
  return S_OK;
}

// Observed: slot 4 EnqueueStatus(status array, index), the desktop's order.
// Assumed in the desktop order: 5 EnqueueSignal(fence, value), 6 Submit().
void STDMETHODCALLTYPE enqueue_status(Queue* self, StatusArray* array, uint32_t index) {
  if (!array || index >= array->capacity) return;
  array->entries[index].store(kPending);
  reinterpret_cast<IUnknown*>(array)->AddRef();
  Operation op;
  op.kind = Operation::kStatus;
  op.array = array;
  op.index = index;
  enqueue(self, op);
}

void STDMETHODCALLTYPE enqueue_signal(Queue* self, IUnknown* fence, uint64_t value) {
  if (!fence) return;
  fence->AddRef();
  Operation op;
  op.kind = Operation::kSignal;
  op.fence = fence;
  op.value = value;
  enqueue(self, op);
}

void STDMETHODCALLTYPE submit(Queue* self) {
  std::lock_guard<std::mutex> hold(self->lock);
  submit_locked(self);
}

// HolyXMemWritePages (xmem.dll): bytes into physical pages by number.
using WritePagesFn = size_t (*)(const ULONG_PTR* frames, size_t count, size_t offset, const void* data, size_t size,
                                void (*shown)(void* context, uintptr_t address, size_t size), void* context,
                                size_t* skipped);

const holy::lib::Counter& c_page_requests() {
  static const holy::lib::Counter counter("dstorage.page_destination_requests");
  return counter;
}
const holy::lib::Counter& c_lost_bytes() {
  static const holy::lib::Counter counter("dstorage.page_bytes_lost");
  return counter;
}

// Writes `size` bytes to the request's destination: title memory at an
// address, or the physical pages its list names (read now: the title fills
// the lists after enqueueing). Copies of the memory (the graphics library's
// textures) hear of the write after the bytes are in place, at every address
// that shows the pages.
HRESULT deliver(const Operation& op, const void* data, size_t size) {
  if (!(op.options & kPageDestination)) {
    HolyEnsureCommitted(reinterpret_cast<void*>(op.destination), size);
    HolyBeforeWrite(reinterpret_cast<void*>(op.destination), size);
    std::memcpy(reinterpret_cast<void*>(op.destination), data, size);
    return S_OK;
  }
  static const auto write_pages =
      reinterpret_cast<WritePagesFn>(GetProcAddress(GetModuleHandleW(L"xmem.dll"), "HolyXMemWritePages"));
  if (!write_pages) return E_NOTIMPL;
  const size_t first = static_cast<size_t>(op.page_offset & 0xFFFF);
  const size_t count = (first + size + kPhysicalPage - 1) / kPhysicalPage;
  if (count > kMaxListPages) return E_INVALIDARG;
  ULONG_PTR frames[kMaxListPages];
  std::memcpy(frames, reinterpret_cast<const void*>(op.destination), count * sizeof(ULONG_PTR));
  c_page_requests().add(1);
  size_t lost = 0;
  const size_t written = write_pages(
      frames, count, first, data, size,
      [](void*, uintptr_t address, size_t bytes) { HolyBeforeWrite(reinterpret_cast<void*>(address), bytes); },
      nullptr, &lost);
  if (lost) {
    c_lost_bytes().add(static_cast<int64_t>(lost));
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      LIB_LOG(holy::lib::kError, "dstorage",
              "0x%zx of 0x%zx bytes for a page list (first page 0x%llx, offset 0x%zx) had no live page to go to",
              lost, size, static_cast<unsigned long long>(frames[0]), first);
    }
  }
  return written == size ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
}

// The codec's constant tables: the file HOLY_TEXTURE_CODEC_TABLES names, else
// texture-codec-tables.bin in the directory holding the titles' data
// directories. Read once; null when absent or invalid (said once).
const holy::lib::codec::Tables* codec_tables() {
  static const holy::lib::codec::TablesPtr tables = [] {
    std::wstring path;
    wchar_t named[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"HOLY_TEXTURE_CODEC_TABLES", named, MAX_PATH)) {
      path = named;
    } else if (const wchar_t* data = HolyDataDir(); data && *data) {
      // <root>/titles/<data key>: the root's file.
      path = data;
      for (int up = 0; up < 2 && !path.empty(); ++up) {
        const size_t slash = path.find_last_of(L"\\/");
        path.resize(slash == std::wstring::npos ? 0 : slash);
      }
      if (!path.empty()) path += L"\\texture-codec-tables.bin";
    }
    std::vector<uint8_t> bytes;
    if (!path.empty()) {
      const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        DWORD got = 0;
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
          bytes.resize(static_cast<size_t>(size.QuadPart));
          if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()) {
            bytes.clear();
          }
        }
        CloseHandle(file);
      }
    }
    std::string why;
    holy::lib::codec::TablesPtr parsed =
        bytes.empty() ? nullptr : holy::lib::codec::parse_tables(bytes.data(), bytes.size(), &why);
    if (parsed) {
      LIB_LOG(holy::lib::kInfo, "dstorage", "the texture codec's tables are loaded (%zu bytes)", bytes.size());
    } else {
      LIB_LOG(holy::lib::kWarn, "dstorage",
              "the texture codec has no tables (%s): its requests fail. The tables are not part of this project; "
              "name a file holding them (%zu bytes) with HOLY_TEXTURE_CODEC_TABLES",
              bytes.empty() ? "no file" : why.c_str(), holy::lib::codec::kTablesSize);
    }
    return parsed;
  }();
  return tables.get();
}

const holy::lib::Counter& c_codec_bytes() {
  static const holy::lib::Counter counter("dstorage.texture_codec_bytes");
  return counter;
}
const holy::lib::Counter& c_codec_us() {
  static const holy::lib::Counter counter("dstorage.texture_codec_us");
  return counter;
}

HRESULT carry_out(const Operation& op) {
  const uint64_t options = op.options, destination = op.destination, source = op.source, offset = op.offset;
  const uint32_t destination_size = op.destination_size, source_size = op.source_size;
  LIB_LOG(holy::lib::kDebug, "dstorage",
          "request options 0x%llx destination %p+0x%llx (0x%x) source %p offset 0x%llx size 0x%x",
          static_cast<unsigned long long>(options), reinterpret_cast<void*>(destination),
          static_cast<unsigned long long>(op.page_offset), destination_size, reinterpret_cast<void*>(source),
          static_cast<unsigned long long>(offset), source_size);
  if (!destination || !source) return E_INVALIDARG;
  {
    // Each kind of request once (options, and whether the sizes match):
    // what titles ask of the queue, for studying the options not established.
    static std::mutex seen_lock;
    static std::vector<uint64_t> seen;
    const uint64_t kind = options | (destination_size == source_size ? 0 : (1ull << 63));
    std::lock_guard<std::mutex> hold(seen_lock);
    if (std::find(seen.begin(), seen.end(), kind) == seen.end() && seen.size() < 64) {
      seen.push_back(kind);
      uint8_t head[16] = {};
      if (options & kSourceMemory) {
        std::memcpy(head, reinterpret_cast<const void*>(source + offset), std::min<uint32_t>(16, source_size));
      } else {
        read_into(reinterpret_cast<File*>(source), offset, head, std::min<uint32_t>(16, source_size));
      }
      LIB_LOG(holy::lib::kInfo, "dstorage",
              "first request of options 0x%llx: 0x%x bytes into 0x%x%s; source starts %02x %02x %02x %02x %02x %02x "
              "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
              static_cast<unsigned long long>(options), source_size, destination_size,
              options & kPageDestination ? " (pages)" : "", head[0], head[1], head[2], head[3], head[4], head[5],
              head[6], head[7], head[8], head[9], head[10], head[11], head[12], head[13], head[14], head[15]);
    }
  }
  if ((options & kOtherFormat) && !codec_tables()) {
    static const holy::lib::Counter unsupported("dstorage.unsupported_format_requests");
    unsupported.add(1);
    static std::mutex logged_lock;
    static std::vector<uint64_t> logged;
    std::lock_guard<std::mutex> hold(logged_lock);
    if (std::find(logged.begin(), logged.end(), options & 0xFF) == logged.end()) {
      logged.push_back(options & 0xFF);
      LIB_LOG(holy::lib::kError, "dstorage",
              "request in a compression format not implemented (options 0x%llx, 0x%x bytes into 0x%x): failed",
              static_cast<unsigned long long>(options), source_size, destination_size);
    }
    return E_NOTIMPL;
  }
  // The source's bytes: the title's memory, or read from the file into memory
  // of this thread's (straight into the destination for plain reads to an
  // address, the common case).
  thread_local std::vector<uint8_t> packed;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(source + offset);
  if (!(options & kSourceMemory)) {
    if (!(options & (kCompressed | kPageDestination))) {
      const HRESULT hr =
          read_into(reinterpret_cast<File*>(source), offset, reinterpret_cast<void*>(destination), source_size);
      if (FAILED(hr)) LIB_LOG(holy::lib::kError, "dstorage", "read failed (0x%08lX)", static_cast<unsigned long>(hr));
      return hr;
    }
    if (packed.size() < source_size) packed.resize(source_size);
    const HRESULT hr = read_into(reinterpret_cast<File*>(source), offset, packed.data(), source_size);
    if (FAILED(hr)) {
      LIB_LOG(holy::lib::kError, "dstorage", "read failed (0x%08lX)", static_cast<unsigned long>(hr));
      return hr;
    }
    bytes = packed.data();
  }
  if (options & kOtherFormat) {
    // The texture codec, after zlib when the request has both.
    const uint8_t* packed_blocks = bytes;
    size_t packed_size = source_size;
    thread_local std::vector<uint8_t> unzipped;
    if (options & kCompressed) {
      size_t produced = 0;
      unzipped.resize(op.intermediate_size);
      if (!op.intermediate_size ||
          !holy::lib::zlib_inflate_into(bytes, source_size, unzipped.data(), unzipped.size(), &produced)) {
        LIB_LOG(holy::lib::kError, "dstorage", "a zlib stream before the texture codec did not inflate (0x%x into 0x%x)",
                source_size, op.intermediate_size);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      }
      packed_blocks = unzipped.data();
      packed_size = produced;
    }
    thread_local std::vector<uint8_t> blocks;
    blocks.resize(destination_size);
    const char* why = nullptr;
    LARGE_INTEGER t0, t1, frequency;
    QueryPerformanceCounter(&t0);
    const bool decoded = holy::lib::codec::decode(*codec_tables(), static_cast<uint32_t>((options >> 1) & 15),
                                                  packed_blocks, packed_size, blocks.data(), blocks.size(), &why);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&frequency);
    c_codec_us().add((t1.QuadPart - t0.QuadPart) * 1000000 / frequency.QuadPart);
    if (!decoded) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 16) {
        LIB_LOG(holy::lib::kError, "dstorage", "texture codec request failed (%s): options 0x%llx, 0x%zx bytes into 0x%x",
                why ? why : "?", static_cast<unsigned long long>(options), packed_size, destination_size);
      }
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    c_codec_bytes().add(destination_size);
    return deliver(op, blocks.data(), blocks.size());
  }
  if (!(options & kCompressed)) return deliver(op, bytes, source_size);
  // Observed: the streams are zlib (RFC 1950) streams. Inflated into memory of
  // this thread's, then delivered: inflating reads its output back (matches,
  // the checksum), and the destination is title memory that may not read back
  // what was written (measured: requests whose streams inflate to exactly
  // their destination's size failed in place, invalid data, and succeeded into
  // private memory; the title then used the unwritten memory and crashed
  // during loading).
  thread_local std::vector<uint8_t> inflated;
  if (inflated.size() < destination_size) inflated.resize(destination_size);
  size_t produced = 0;
  if (!holy::lib::zlib_inflate_into(bytes, source_size, inflated.data(), destination_size, &produced)) {
    uint8_t head[8] = {};
    std::memcpy(head, bytes, std::min<size_t>(8, source_size));
    LIB_LOG(holy::lib::kError, "dstorage",
            "compressed request failed: options 0x%llx, 0x%x bytes at 0x%llx into 0x%x; stream starts "
            "%02x %02x %02x %02x %02x %02x %02x %02x",
            static_cast<unsigned long long>(options), source_size, static_cast<unsigned long long>(offset),
            destination_size, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  return deliver(op, inflated.data(), produced);
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
  // The worker lives as long as the process: titles keep their queues.
  queue->refs.fetch_add(1);
  std::thread(worker, queue).detach();
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
