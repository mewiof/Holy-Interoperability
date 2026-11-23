// The console DXGI objects a title reaches from the graphics device.
//
// Observed: titles query the device for IDXGIDevice
// {54ec77fa-1377-44e6-8c32-88fd5f44c84c}, call slot 7 (GetAdapter, as on the
// desktop), then the adapter's slot 7 (EnumOutputs(0, &output), as on the
// desktop), then the output's slot 21 twice as (format 0, flags 0, &count,
// modes): a display mode list of 32-byte entries in the layout of the public
// DXGI_MODE_DESC1 (titles look for a refresh numerator of 120 at offset 8).
// The console output keeps no desktop order at that slot.
//
// Answers come from the host: the modes are those of the host's primary
// output for 8-bit BGRA, which a format of 0 is taken to mean.
#include <dxgi1_6.h>

#include <cstring>
#include <vector>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

struct DxgiObject {
  void* const* vtable;
  const lib::VtableInfo* info;
};

const lib::VtableInfo kDxgiDeviceInfo{kLibrary, "IDXGIDeviceX"};
const lib::VtableInfo kAdapterInfo{kLibrary, "IDXGIAdapterX"};
const lib::VtableInfo kOutputInfo{kLibrary, "IDXGIOutputX"};

HRESULT STDMETHODCALLTYPE object_query_interface(DxgiObject* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE object_add_ref(DxgiObject*) {
  return 2;
}
ULONG STDMETHODCALLTYPE object_release(DxgiObject*) {
  return 1;
}

template <int N>
std::array<void*, N> object_vtable() {
  auto table = lib::make_reporting_vtable<N>();
  table[0] = reinterpret_cast<void*>(&object_query_interface);
  table[1] = reinterpret_cast<void*>(&object_add_ref);
  table[2] = reinterpret_cast<void*>(&object_release);
  return table;
}

// --- output ----------------------------------------------------------------------

std::vector<DXGI_MODE_DESC1>& host_modes() {
  static auto* modes = [] {
    auto* list = new std::vector<DXGI_MODE_DESC1>();
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return list;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    // The first output of any adapter: the display the user is looking at.
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
      adapter->EnumOutputs(0, &output);
      adapter->Release();
    }
    IDXGIOutput1* output1 = nullptr;
    if (output && SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output1)))) {
      UINT count = 0;
      output1->GetDisplayModeList1(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, nullptr);
      list->resize(count);
      if (count) output1->GetDisplayModeList1(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, list->data());
      list->resize(count);
      output1->Release();
    }
    if (output) output->Release();
    factory->Release();
    LIB_LOG(lib::kInfo, "dxgi", "%zu host display modes", list->size());
    return list;
  }();
  return *modes;
}

HRESULT STDMETHODCALLTYPE output_get_display_mode_list(DxgiObject*, DXGI_FORMAT format, UINT, UINT* count,
                                                       DXGI_MODE_DESC1* modes) {
  if (!count) return E_INVALIDARG;
  const std::vector<DXGI_MODE_DESC1>& list = host_modes();
  std::vector<DXGI_MODE_DESC1> matching;
  for (const DXGI_MODE_DESC1& mode : list) {
    if (format == DXGI_FORMAT_UNKNOWN || format == mode.Format) matching.push_back(mode);
  }
  if (!modes) {
    *count = static_cast<UINT>(matching.size());
    return S_OK;
  }
  const UINT copied = std::min<UINT>(*count, static_cast<UINT>(matching.size()));
  std::memcpy(modes, matching.data(), copied * sizeof(DXGI_MODE_DESC1));
  *count = copied;
  return copied < matching.size() ? DXGI_ERROR_MORE_DATA : S_OK;
}

const auto kOutputVtable = [] {
  auto table = object_vtable<48>();
  table[21] = reinterpret_cast<void*>(&output_get_display_mode_list);
  return table;
}();
DxgiObject g_output{kOutputVtable.data(), &kOutputInfo};

// --- adapter ----------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE adapter_enum_outputs(DxgiObject*, UINT index, void** output) {
  if (!output) return E_INVALIDARG;
  *output = nullptr;
  if (index > 0) return DXGI_ERROR_NOT_FOUND;
  *output = &g_output;
  return S_OK;
}

const auto kAdapterVtable = [] {
  auto table = object_vtable<32>();
  table[7] = reinterpret_cast<void*>(&adapter_enum_outputs);
  return table;
}();
DxgiObject g_adapter{kAdapterVtable.data(), &kAdapterInfo};

// --- device -------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE device_get_adapter(DxgiObject*, void** adapter) {
  if (!adapter) return E_INVALIDARG;
  *adapter = &g_adapter;
  return S_OK;
}

const auto kDxgiDeviceVtable = [] {
  auto table = object_vtable<32>();
  table[7] = reinterpret_cast<void*>(&device_get_adapter);
  return table;
}();
DxgiObject g_dxgi_device{kDxgiDeviceVtable.data(), &kDxgiDeviceInfo};

}  // namespace

void* dxgi_device() {
  return &g_dxgi_device;
}

}  // namespace holy::d3d12
