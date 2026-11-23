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

namespace holy::d3d12 {
struct Device;
Device* create_device(const void* parameters);  // device.cpp
}  // namespace holy::d3d12

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

// Observed: D3D12XboxCreateDevice(adapter, parameters, REFIID, void** device),
// called once with a null adapter after D3D12ConfigureVirtualMemoryX.
HOLY_EXPORT HRESULT WINAPI D3D12XboxCreateDevice(void* adapter, const void* parameters, REFIID riid,
                                                 void** device) {
  (void)adapter;
  (void)riid;
  if (!device) return E_POINTER;
  *device = holy::d3d12::create_device(parameters);
  return *device ? S_OK : static_cast<HRESULT>(0x887A0004);  // DXGI_ERROR_UNSUPPORTED
}

HOLY_EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void* root_signature, void** blob,
                                                                void** error_blob) {
  using Fn = HRESULT(WINAPI*)(const void*, void**, void**);
  const auto fn = reinterpret_cast<Fn>(host_export("D3D12SerializeVersionedRootSignature"));
  if (!fn) return E_NOTIMPL;
  // Observed: root signature flags may carry bits above the desktop's
  // highest (0x800), e.g. 0x3000000. They describe console binding behaviour
  // and are dropped for the host. The flags word sits at the same offset in
  // the 1.0, 1.1 and 1.2 descriptions (after the version, count and pointer
  // fields and the static sampler count and pointer).
  alignas(8) uint8_t copy[64];
  const void* host_signature = root_signature;
  if (root_signature) {
    std::memcpy(copy, root_signature, sizeof(copy));
    auto* flags = reinterpret_cast<uint32_t*>(copy + 40);
    if (*flags & ~0xFFFu) {
      LIB_LOG(holy::lib::kDebug, "d3d12", "root signature flags 0x%x: console bits 0x%x dropped", *flags,
              *flags & ~0xFFFu);
      *flags &= 0xFFFu;
    }
    host_signature = copy;
  }
  const HRESULT result = fn(host_signature, blob, error_blob);
  LIB_LOG(holy::lib::kDebug, "d3d12", "D3D12SerializeVersionedRootSignature -> 0x%08lX", result);
  if (FAILED(result) && root_signature) {
    const auto* words = static_cast<const uint32_t*>(root_signature);
    LIB_LOG(holy::lib::kError, "d3d12", "root signature: version 0x%x params %u at %p samplers %u flags 0x%x",
            words[0], words[2], *reinterpret_cast<void* const*>(words + 4), words[6], words[10]);
    const auto* params = *reinterpret_cast<const uint32_t* const*>(words + 4);
    for (uint32_t i = 0; params && i < words[2] && i < 16; ++i) {
      const uint32_t* p = params + i * 8;  // 32-byte D3D12_ROOT_PARAMETER1
      LIB_LOG(holy::lib::kError, "d3d12", "  param %u: type %u %08x %08x %08x %08x vis %u", i, p[0], p[2], p[3],
              p[4], p[5], p[6]);
    }
    if (error_blob && *error_blob) {
      void* text = *error_blob;
      using GetPointerFn = void*(STDMETHODCALLTYPE*)(void*);
      const char* message = static_cast<const char*>(
          reinterpret_cast<GetPointerFn>((*static_cast<void***>(text))[3])(text));
      LIB_LOG(holy::lib::kError, "d3d12", "  host says: %s", message ? message : "?");
    }
  }
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
