// chad.dll: the console's audio DSP command interface (XDsp functions).
//
// Observed (call sites; how results are checked):
//
//   HRESULT XDspConnectWithMaximumStreamLimit(void* memory, uint32_t bytes, uint32_t streams, uint32_t, uint32_t,
//                                             Handle* out)
//           once at audio start-up, with a block of title memory and its size
//           (0x60000 seen), a stream count (30 seen), 0x80 and 1; titles
//           require exactly S_OK and stop otherwise.
//   HRESULT XDspActivate(Handle, ...)        four arguments; 0 counts as success
//   HRESULT XDspSubmitCommand(Handle, const void* command)   0 counts as success
//   HRESULT XDspDeactivate(Handle), XDspDisconnect(Handle)
//
// Host: a silent DSP. Connections, activations and commands are accepted and
// not carried out (nothing is decoded or mixed).
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "core/libs/support.h"

namespace {

constexpr uint32_t kConnection = 0x43484144;  // "CHAD"

struct Connection {
  uint32_t magic = kConnection;
  void* memory = nullptr;
  uint32_t bytes = 0;
  uint32_t streams = 0;
};

Connection* connection_of(void* handle) {
  auto* c = static_cast<Connection*>(handle);
  return c && c->magic == kConnection ? c : nullptr;
}

}  // namespace

HOLY_EXPORT HRESULT XDspConnectWithMaximumStreamLimit(void* memory, uint32_t bytes, uint32_t streams, uint32_t a,
                                                      uint32_t b, void** out) {
  if (!out) return E_POINTER;
  auto* connection = new Connection();
  connection->memory = memory;
  connection->bytes = bytes;
  connection->streams = streams;
  *out = connection;
  LIB_LOG(holy::lib::kInfo, "chad", "connected (memory %p, 0x%x bytes, %u streams, 0x%x, %u): a silent DSP", memory,
          bytes, streams, a, b);
  return S_OK;
}

HOLY_EXPORT HRESULT XDspActivate(void* handle, void*, void*, void*) {
  return connection_of(handle) ? S_OK : E_HANDLE;
}

HOLY_EXPORT HRESULT XDspSubmitCommand(void* handle, const void* command) {
  if (!connection_of(handle)) return E_HANDLE;
  if (!command) return E_POINTER;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    const auto* words = static_cast<const uint32_t*>(command);
    LIB_LOG(holy::lib::kDebug, "chad", "command %08x %08x %08x %08x: not carried out", words[0], words[1], words[2],
            words[3]);
  }
  return S_OK;
}

HOLY_EXPORT HRESULT XDspDeactivate(void* handle) {
  return connection_of(handle) ? S_OK : E_HANDLE;
}

HOLY_EXPORT HRESULT XDspDisconnect(void* handle) {
  Connection* connection = connection_of(handle);
  if (!connection) return E_HANDLE;
  connection->magic = 0;  // the object stays: titles may still hold the handle
  return S_OK;
}
