// d3d12_xs.dll: the console's Direct3D 12 entry points.
//
// Present state: the virtual-memory configuration call and the root-signature
// helpers, which have the same shape as their desktop counterparts and are
// forwarded to the host's Direct3D 12. Device creation and everything behind
// it (the device, queues, command lists and the console-specific extensions)
// is the graphics backend still to be written; until then that import stays
// unexported so the binder reports the exact call a title makes.
#include <windows.h>

#include <cstring>

#include "core/libs/support.h"

namespace {

FARPROC host_export(const char* name) {
  static const HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module ? GetProcAddress(module, name) : nullptr;
}

}  // namespace

// Observed contract: called once, before any device exists, with a pointer to
// a 32-byte configuration block; a negative HRESULT is treated as fatal. On
// the console it sizes the address-space ranges the graphics runtime manages
// on the title's behalf. The host's Direct3D owns its address space, so the
// configuration is recorded for the backend and accepted.
HOLY_EXPORT HRESULT WINAPI D3D12ConfigureVirtualMemoryX(const void* configuration) {
  if (!configuration) return E_INVALIDARG;
  uint32_t words[8];
  std::memcpy(words, configuration, sizeof(words));
  LIB_LOG(holy::lib::kInfo, "d3d12", "virtual memory configuration: %08x %08x %08x %08x %08x %08x %08x %08x",
          words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
  return S_OK;
}

HOLY_EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void* root_signature, void** blob,
                                                                void** error_blob) {
  using Fn = HRESULT(WINAPI*)(const void*, void**, void**);
  const auto fn = reinterpret_cast<Fn>(host_export("D3D12SerializeVersionedRootSignature"));
  if (!fn) return E_NOTIMPL;
  const HRESULT result = fn(root_signature, blob, error_blob);
  LIB_LOG(holy::lib::kDebug, "d3d12", "D3D12SerializeVersionedRootSignature -> 0x%08lX", result);
  return result;
}

HOLY_EXPORT HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size,
                                                                REFIID interface_id, void** deserializer) {
  using Fn = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);
  const auto fn = reinterpret_cast<Fn>(host_export("D3D12CreateRootSignatureDeserializer"));
  if (!fn) return E_NOTIMPL;
  const HRESULT result = fn(data, size, interface_id, deserializer);
  LIB_LOG(holy::lib::kDebug, "d3d12", "D3D12CreateRootSignatureDeserializer -> 0x%08lX", result);
  return result;
}
