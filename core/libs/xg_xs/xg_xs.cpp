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

#include <algorithm>
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

// Observed: slot 6 takes (plane, mip level, x, y, z or array slice, sample,
// 0) and returns a 64-bit byte offset: titles add it to a texture's memory
// address to reach a texel (the element holding it, for block-compressed
// formats) they read or write on the CPU. The offset follows the layout
// copy_into_subresource writes (lib::subresource_layout); the last argument
// (a bit offset out, assumed) is written zero when given.
uint64_t STDMETHODCALLTYPE get_texel_element_offset(Computer* self, uint32_t plane, uint32_t mip, uint64_t x,
                                                     uint32_t y, uint32_t z, uint32_t sample, uint32_t* bit_offset) {
  const holy::lib::TextureShape shape = shape_of(self->desc);
  const holy::lib::FormatInfo f = holy::lib::format_info(shape.format);
  const uint32_t slice = shape.dimension == 4 ? 0 : z;
  holy::lib::SubresourceLayout layout;
  if (bit_offset) *bit_offset = 0;
  if (plane != 0 || sample != 0 || mip >= shape.mips || !f.block_bytes ||
      !holy::lib::subresource_layout(shape, mip + slice * shape.mips, &layout)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      LIB_LOG(holy::lib::kError, "xg", "texel offset of plane %u mip %u sample %u, format %u: not supported", plane,
              mip, sample, shape.format);
    }
    return 0;
  }
  const uint64_t depth = shape.dimension == 4 ? z : 0;
  return layout.offset + (depth * layout.rows + y / f.block_size) * layout.row_pitch +
         x / f.block_size * f.block_bytes;
}

// Observed: slot 10 takes (tile count*, packed mip info*, tile shape*,
// subresource tiling count* (in/out, 16 seen), first subresource tiling,
// subresource tilings*): the arguments of the desktop
// ID3D12Device::GetResourceTiling after its resource (titles stream tiled
// textures in 64 KiB pages, and place each level at the tiles reported).
// The answer is the console's layout (lib::console_tiling); shapes it does
// not model get the host's answer for a reserved resource of the same
// description (console flag bits dropped), which orders levels differently.
ID3D12Device* tiling_device() {
  static ID3D12Device* device = [] {
    ID3D12Device* made = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&made)))) made = nullptr;
    return made;
  }();
  return device;
}

// The console layout's answer; false when the shape is not modelled.
bool console_resource_tiling(const uint8_t* desc, UINT* tiles, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape,
                             UINT* count, UINT first, D3D12_SUBRESOURCE_TILING* tilings) {
  uint32_t samples = 1;
  std::memcpy(&samples, desc + 36, 4);
  holy::lib::ConsoleTiling t;
  if (samples > 1 || !holy::lib::console_tiling(shape_of(desc), &t)) return false;
  const uint32_t mips = t.standard_mips + t.packed_mips;
  if (tiles) *tiles = t.slice_tiles * t.slices;
  if (packed) {
    packed->NumStandardMips = static_cast<UINT8>(t.standard_mips);
    packed->NumPackedMips = static_cast<UINT8>(t.packed_mips);
    packed->NumTilesForPackedMips = t.packed_tiles;
    packed->StartTileIndexInOverallResource = 0;  // the first slice's first tile
  }
  if (shape) *shape = {t.tile_width, t.tile_height, t.tile_depth};
  if (count) {
    const uint32_t total = mips * t.slices;
    const uint32_t wanted = first < total ? std::min<uint32_t>(*count, total - first) : 0;
    for (uint32_t i = 0; tilings && i < wanted; ++i) {
      const uint32_t mip = (first + i) % mips, slice = (first + i) / mips;
      if (mip >= t.standard_mips) {
        tilings[i] = {0, 0, 0, D3D12_PACKED_TILE};
        continue;
      }
      const holy::lib::ConsoleMipTiling& m = t.mips[mip];
      tilings[i] = {m.width_tiles, static_cast<UINT16>(m.height_tiles), static_cast<UINT16>(m.depth_tiles),
                    slice * t.slice_tiles + m.start};
    }
    *count = wanted;
  }
  return true;
}

HRESULT STDMETHODCALLTYPE get_resource_tiling(Computer* self, UINT* tiles, D3D12_PACKED_MIP_INFO* packed,
                                              D3D12_TILE_SHAPE* shape, UINT* count, UINT first,
                                              D3D12_SUBRESOURCE_TILING* tilings) {
  static const bool trace = GetEnvironmentVariableW(L"HOLY_XG_TRACE", nullptr, 0) > 0;
  if (console_resource_tiling(self->desc, tiles, packed, shape, count, first, tilings)) {
    if (trace) {
      const holy::lib::TextureShape s = shape_of(self->desc);
      LIB_LOG(holy::lib::kInfo, "xg",
              "tiling of %llux%u x%u, %u mips, format %u: %u tiles; %u standard mips, %u packed in %u tiles; tile "
              "%ux%ux%u",
              static_cast<unsigned long long>(s.width), s.height, s.depth_or_array, s.mips, s.format,
              tiles ? *tiles : 0, packed ? packed->NumStandardMips : 0, packed ? packed->NumPackedMips : 0,
              packed ? packed->NumTilesForPackedMips : 0, shape ? shape->WidthInTexels : 0,
              shape ? shape->HeightInTexels : 0, shape ? shape->DepthInTexels : 0);
    }
    return S_OK;
  }
  {
    static std::atomic<int> logged{0};
    const holy::lib::TextureShape s = shape_of(self->desc);
    if (logged.fetch_add(1) < 8) {
      LIB_LOG(holy::lib::kWarn, "xg", "resource tiling of format %u (%u mips): not modelled, the host's answer instead",
              s.format, s.mips);
    }
  }
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
  if (trace) {
    LIB_LOG(holy::lib::kInfo, "xg",
            "tiling of %ux%u x%u, %u mips, format %u: %u tiles; packed: %u standard mips, %u packed, %u tiles from %u; "
            "tile %ux%ux%u",
            static_cast<unsigned>(desc.Width), desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format,
            tiles ? *tiles : 0, packed ? packed->NumStandardMips : 0, packed ? packed->NumPackedMips : 0,
            packed ? packed->NumTilesForPackedMips : 0, packed ? packed->StartTileIndexInOverallResource : 0,
            shape ? shape->WidthInTexels : 0, shape ? shape->HeightInTexels : 0, shape ? shape->DepthInTexels : 0);
  }
  return S_OK;
}

const auto kComputerVtable = [] {
  auto table = holy::lib::make_reporting_vtable<kComputerSlots>();
  table[0] = reinterpret_cast<void*>(&computer_query_interface);
  table[1] = reinterpret_cast<void*>(&computer_add_ref);
  table[2] = reinterpret_cast<void*>(&computer_release);
  table[6] = reinterpret_cast<void*>(&get_texel_element_offset);
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
