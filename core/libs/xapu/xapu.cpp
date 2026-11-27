// xapu.dll: the command queue of the console's audio processor.
//
// Observed (call sites; how results are checked):
//
//   HRESULT  XApuConnect(const XApuConfig* config, void** memory, Handle* out)
//            once at audio start-up; a failure is fatal to titles. The
//            configuration is six dwords ({1, 0x100, 2, 0x104000, 0, 1}
//            seen): dword 3 is the size of the memory block the call returns,
//            which titles lay out as 0x280-byte records followed, from
//            0x14000, by 0x1E00-byte buffers (128 of each for the size seen).
//   HRESULT  XApuEnqueueCommand(Handle, const void* command, uint32_t)
//            64-byte commands (dword 0 the kind); the result is not checked.
//   uint32_t XApuGetResultCount(Handle, uint32_t)
//   HRESULT  XApuDequeueResult(Handle, void* result)
//            titles poll the count and dequeue that many results (dword 0
//            the kind, 1 to 3; dword 1 an index into their records).
//   void     XApuDisconnect(Handle)
//
// Host: a silent audio device. The memory block is zeroed audio processor
// memory; commands are accepted and not carried out, so no result is ever
// produced (the count stays 0). HOLY_AUDIO_TRACE writes the configuration,
// every command and the 0x280-byte record its dword 1 names to
// <data>/logs/audio-xapu.txt (core/libs/trace_file.h), for studying them.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/libs/support.h"
#include "core/libs/trace_file.h"

extern "C" __declspec(dllimport) HRESULT ApuAlloc(void** address, uint32_t* apu_address, uint32_t bytes,
                                                  uint32_t alignment, uint32_t flags);
extern "C" __declspec(dllimport) HRESULT ApuFree(void* address);

namespace {

struct XApuConfig {
  uint32_t words[6];
};

constexpr uint32_t kConnection = 0x58415055;  // "XAPU"
constexpr uint32_t kRecordBytes = 0x280;
constexpr uint32_t kRecords = 0x100;  // records before the buffers (at 0x14000) for the size seen

holy::lib::TraceFile g_trace(L"HOLY_AUDIO_TRACE", L"audio-xapu.txt");

struct Connection {
  uint32_t magic = kConnection;
  void* memory = nullptr;
};

Connection* connection_of(void* handle) {
  auto* c = static_cast<Connection*>(handle);
  return c && c->magic == kConnection ? c : nullptr;
}

}  // namespace

HOLY_EXPORT HRESULT XApuConnect(const XApuConfig* config, void** memory, void** out) {
  if (!config || !memory || !out) return E_POINTER;
  *memory = nullptr;
  *out = nullptr;
  const uint32_t bytes = config->words[3];
  auto* connection = new Connection();
  if (bytes) {
    // Audio processor memory when the heap exists, else ordinary memory.
    if (FAILED(ApuAlloc(&connection->memory, nullptr, bytes, 0x1000, 1))) {
      connection->memory = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    } else {
      std::memset(connection->memory, 0, bytes);
    }
    if (!connection->memory) {
      delete connection;
      return E_OUTOFMEMORY;
    }
  }
  *memory = connection->memory;
  *out = connection;
  if (g_trace.on()) {
    uint32_t words[8];
    std::memcpy(words, config->words, sizeof(config->words));
    const auto base = reinterpret_cast<uintptr_t>(connection->memory);
    words[6] = static_cast<uint32_t>(base);
    words[7] = static_cast<uint32_t>(base >> 32);
    g_trace.event("connect", words, 8);
  }
  LIB_LOG(holy::lib::kInfo, "xapu", "connected (%u %u %u 0x%x %u %u): a silent audio device", config->words[0],
          config->words[1], config->words[2], config->words[3], config->words[4], config->words[5]);
  return S_OK;
}

HOLY_EXPORT HRESULT XApuEnqueueCommand(void* handle, const uint32_t* command, uint32_t flags) {
  Connection* connection = connection_of(handle);
  if (!connection) return E_HANDLE;
  if (!command) return E_POINTER;
  if (g_trace.on()) {
    g_trace.event("command", command, 16);
    if (connection->memory && command[1] < kRecords) {
      g_trace.event("record", static_cast<const uint8_t*>(connection->memory) + command[1] * kRecordBytes,
                    kRecordBytes / 4);
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    LIB_LOG(holy::lib::kDebug, "xapu", "command %u (%08x %08x %08x, flags 0x%x): not carried out", command[0],
            command[1], command[2], command[3], flags);
  }
  return S_OK;
}

HOLY_EXPORT uint32_t XApuGetResultCount(void* handle, uint32_t) {
  (void)handle;
  return 0;  // a silent device produces no results
}

HOLY_EXPORT HRESULT XApuDequeueResult(void* handle, void* result) {
  if (!connection_of(handle)) return E_HANDLE;
  if (!result) return E_POINTER;
  return HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);  // the queue is always empty
}

HOLY_EXPORT void XApuDisconnect(void* handle) {
  Connection* connection = connection_of(handle);
  if (!connection) return;
  connection->magic = 0;
  // The memory stays: titles may still hold pointers into it.
}
