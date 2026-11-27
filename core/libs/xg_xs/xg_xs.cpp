// xg_xs.dll: the console's texture layout library.
//
// Observed entry points:
//   HRESULT XGSuggestSwizzleMode(const XGResourceDesc*, uint32_t (0 seen), uint32_t* mode)
//   HRESULT XGCreateTextureComputer(const XGResourceDesc*, Computer** out)
// The 56-byte resource description has the desktop D3D12_RESOURCE_DESC layout
// (dimension, alignment, width, height, depth or array size, mip levels,
// format, sample description, layout, flags) with the console's flag bits.
// The computer copies linear texel data into a texture's memory layout for
// titles: slot 8 CopyIntoSubresource(destination base, plane, subresource,
// source, source row pitch, source slice pitch), called per subresource; the
// result is not checked.
//
// The layout a title follows is the one this library reports, so it can be
// chosen: textures are laid out as the desktop's copyable footprints
// (core/libs/texture_layout.h); the swizzle mode reported is 0 (assumed to
// mean linear).
//
// Present state: work in progress; unbound computer slots report themselves.
#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/libs/support.h"
#include "core/libs/texture_layout.h"
#include "core/libs/vtable.h"

namespace {

constexpr char kLibrary[] = "xg_xs.dll";
constexpr int kComputerSlots = 32;

struct Computer {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  uint8_t desc[56];
};

const holy::lib::VtableInfo kComputerInfo{kLibrary, "XGTextureAddressComputer"};

HRESULT STDMETHODCALLTYPE computer_query_interface(Computer* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE computer_add_ref(Computer* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE computer_release(Computer* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}

holy::lib::TextureShape shape_of(const uint8_t* desc) {
  holy::lib::TextureShape shape;
  uint16_t depth = 0, mips = 0;
  std::memcpy(&shape.dimension, desc + 0, 4);
  std::memcpy(&shape.width, desc + 16, 8);
  std::memcpy(&shape.height, desc + 24, 4);
  std::memcpy(&depth, desc + 28, 2);
  std::memcpy(&mips, desc + 30, 2);
  std::memcpy(&shape.format, desc + 32, 4);
  shape.depth_or_array = depth ? depth : 1;
  shape.mips = mips ? mips : 1;
  return shape;
}

HRESULT STDMETHODCALLTYPE copy_into_subresource(Computer* self, void* destination, uint32_t plane,
                                                uint32_t subresource, const void* source, uint32_t row_pitch,
                                                uint32_t slice_pitch) {
  const holy::lib::TextureShape shape = shape_of(self->desc);
  holy::lib::SubresourceLayout layout;
  if (plane != 0 || !destination || !source || !holy::lib::subresource_layout(shape, subresource, &layout)) {
    LIB_LOG(holy::lib::kError, "xg", "copy into subresource %u plane %u of format %u not supported", subresource,
            plane, shape.format);
    return E_INVALIDARG;
  }
  auto* out = static_cast<uint8_t*>(destination) + layout.offset;
  const auto* in = static_cast<const uint8_t*>(source);
  for (uint32_t z = 0; z < layout.depth; ++z) {
    for (uint32_t row = 0; row < layout.rows; ++row) {
      std::memcpy(out + (static_cast<uint64_t>(z) * layout.rows + row) * layout.row_pitch,
                  in + static_cast<uint64_t>(z) * slice_pitch + static_cast<uint64_t>(row) * row_pitch,
                  layout.row_bytes);
    }
  }
  return S_OK;
}

// Observed: slot 10 takes (tile count*, packed mip info*, tile shape*,
// subresource tiling count* (in/out, 16 seen), first subresource tiling,
// subresource tilings*): the arguments of the desktop
// ID3D12Device::GetResourceTiling after its resource (titles stream tiled
// textures in 64 KiB pages). Host: the answer the host gives for a reserved
// resource of the same description (console flag bits dropped).
ID3D12Device* tiling_device() {
  static ID3D12Device* device = [] {
    ID3D12Device* made = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&made)))) made = nullptr;
    return made;
  }();
  return device;
}

HRESULT STDMETHODCALLTYPE get_resource_tiling(Computer* self, UINT* tiles, D3D12_PACKED_MIP_INFO* packed,
                                              D3D12_TILE_SHAPE* shape, UINT* count, UINT first,
                                              D3D12_SUBRESOURCE_TILING* tilings) {
  ID3D12Device* device = tiling_device();
  if (!device) return E_FAIL;
  D3D12_RESOURCE_DESC desc;
  std::memcpy(&desc, self->desc, sizeof(desc));
  desc.Alignment = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
  desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  ID3D12Resource* reserved = nullptr;
  const HRESULT hr = device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved));
  if (FAILED(hr)) {
    LIB_LOG(holy::lib::kError, "xg", "resource tiling of format %u %llux%u: the host refuses a reserved resource (0x%08lX)",
            desc.Format, static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned long>(hr));
    return hr;
  }
  device->GetResourceTiling(reserved, tiles, packed, shape, count, first, tilings);
  reserved->Release();
  return S_OK;
}

const auto kComputerVtable = [] {
  auto table = holy::lib::make_reporting_vtable<kComputerSlots>();
  table[0] = reinterpret_cast<void*>(&computer_query_interface);
  table[1] = reinterpret_cast<void*>(&computer_add_ref);
  table[2] = reinterpret_cast<void*>(&computer_release);
  table[8] = reinterpret_cast<void*>(&copy_into_subresource);
  table[10] = reinterpret_cast<void*>(&get_resource_tiling);
  return table;
}();

void log_desc(const char* what, const void* desc) {
  uint32_t d[14] = {};
  std::memcpy(d, desc, sizeof(d));
  LIB_LOG(holy::lib::kDebug, "xg", "%s: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", what,
          d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13]);
}

}  // namespace

HOLY_EXPORT HRESULT WINAPI XGSuggestSwizzleMode(const void* desc, uint32_t unknown, uint32_t* mode) {
  if (!desc || !mode) return E_POINTER;
  log_desc(unknown ? "swizzle mode (second argument set)" : "swizzle mode", desc);
  *mode = 0;
  return S_OK;
}

HOLY_EXPORT HRESULT WINAPI XGCreateTextureComputer(const void* desc, void** out) {
  if (!desc || !out) return E_POINTER;
  log_desc("texture computer", desc);
  auto* computer = new Computer();
  computer->vtable = kComputerVtable.data();
  computer->info = &kComputerInfo;
  std::memcpy(computer->desc, desc, sizeof(computer->desc));
  *out = computer;
  return S_OK;
}
