// xg_xs.dll: the console's texture layout library.
//
// Observed entry points:
//   HRESULT XGSuggestSwizzleMode(const XGResourceDesc*, uint32_t (0 seen), uint32_t* layout)
//   HRESULT XGCreateTextureComputer(const XGResourceDesc*, Computer** out)
// The 56-byte resource description has the desktop D3D12_RESOURCE_DESC layout
// (dimension, alignment, width, height, depth or array size, mip levels,
// format, sample description, layout, flags) with the console's flag bits;
// its layout field is 0 (the library picks), 1 (row-major) or 0x100 plus a
// swizzle mode.
//
// Textures live in title memory in the console GPU's own layout
// (core/libs/gpu_layout.h): titles stream them from their archives already
// in it, so this library reports that layout, copies titles' texel rows into
// it, and the graphics library reads textures out of it.
//
// Computer slots (observed from titles' calls; the computer is not a COM
// object in the desktop's slot order): 1 Release, 2 the layout, 6 an
// element's offset, 8 copy rows into a subresource, 9 copy a subresource
// out into rows, 10 the 64 KiB tiling. Unbound slots report themselves.
#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/libs/gpu_layout.h"
#include "core/libs/support.h"
#include "core/libs/texture_layout.h"
#include "core/libs/vtable.h"

namespace gpu = holy::lib::gpu;

namespace {

constexpr char kLibrary[] = "xg_xs.dll";
constexpr int kComputerSlots = 32;

struct Desc {
  uint32_t dimension = 0;
  uint64_t alignment = 0, width = 0;
  uint32_t height = 1;
  uint16_t depth_or_array = 1, mips = 1;
  uint32_t format = 0, samples = 1, quality = 0, layout = 0, flags = 0;
};

Desc read_desc(const uint8_t* d) {
  Desc out;
  std::memcpy(&out.dimension, d + 0, 4);
  std::memcpy(&out.alignment, d + 8, 8);
  std::memcpy(&out.width, d + 16, 8);
  std::memcpy(&out.height, d + 24, 4);
  std::memcpy(&out.depth_or_array, d + 28, 2);
  std::memcpy(&out.mips, d + 30, 2);
  std::memcpy(&out.format, d + 32, 4);
  std::memcpy(&out.samples, d + 36, 4);
  std::memcpy(&out.quality, d + 40, 4);
  std::memcpy(&out.layout, d + 44, 4);
  std::memcpy(&out.flags, d + 48, 4);
  return out;
}

struct Computer {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Desc desc;
  uint32_t mips = 1;
  // The console layout; when `console` is false (formats the GPU's layout
  // does not cover), the desktop's copyable footprints instead.
  bool console = false;
  gpu::Texture texture;
};

const holy::lib::VtableInfo kComputerInfo{kLibrary, "XGTextureAddressComputer"};

ULONG STDMETHODCALLTYPE computer_release(Computer* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}

holy::lib::TextureShape shape_of(const Desc& d) {
  holy::lib::TextureShape shape;
  shape.dimension = d.dimension;
  shape.width = d.width;
  shape.height = d.height;
  shape.depth_or_array = d.depth_or_array ? d.depth_or_array : 1;
  shape.mips = d.mips ? d.mips : 1;
  shape.format = d.format;
  return shape;
}

// The structure slot 2 fills, as titles read it (observed: they pass room
// for 0x1b38 bytes, copy one 0x6c8-byte plane record out of it by plane
// index, and read the level fields at 0x00, 0x08, 0x10 and 0x1c and the
// plane's size and array pitch). The other fields are filled as assumed
// from the shape of the record (not seen read).
struct LevelRecord {           // 0x70 bytes, one per mip level (15)
  uint64_t bytes;              // 0x00: the level in one array slice
  uint64_t offset;             // 0x08: from the start of the array slice (in the tail: within the tail block)
  uint64_t slice_bytes;        // 0x10: one 2D slice of the level (depth pitch)
  uint32_t pitch_texels;       // 0x18 (assumed)
  uint32_t row_pitch;          // 0x1c: bytes between rows of elements
  uint32_t alignment;          // 0x20 (assumed)
  uint32_t padded[3];          // 0x24: padded width, height (elements), depth or array (assumed)
  uint32_t elements[2];        // 0x30 (assumed)
  uint32_t texels[2];          // 0x38 (assumed)
  uint32_t depth_or_array;     // 0x40 (assumed)
  uint32_t samples;            // 0x44 (assumed)
  uint32_t swizzle;            // 0x48 (assumed)
  uint32_t unused_4c;
  uint64_t unused_50[2];
  uint32_t slice_depth;        // 0x60: depth slices a block spans (assumed)
  uint32_t in_tail;            // 0x64 (assumed)
  uint32_t tail_position;      // 0x68: x bits 0-10, y 11-21, in elements (assumed)
  uint32_t unused_6c;
};
static_assert(sizeof(LevelRecord) == 0x70);
struct PlaneRecord {           // 0x6c8 bytes
  uint32_t usage;              // 0x00: 1 data, 7 depth, 8 stencil (assumed)
  uint32_t unused_04;
  uint64_t bytes;              // 0x08: the plane, all array slices
  uint64_t array_pitch;        // 0x10: bytes between array slices
  uint64_t offset;             // 0x18: of the plane in the texture (assumed)
  uint64_t alignment;          // 0x20 (assumed)
  uint32_t element_bytes;      // 0x28 (assumed)
  uint32_t standard_mips;      // 0x2c (assumed)
  uint32_t tail_mips;          // 0x30 (assumed)
  uint32_t unused_34;
  LevelRecord levels[15];
};
static_assert(sizeof(PlaneRecord) == 0x6c8);
struct ResourceLayout {        // 0x1b38 bytes
  uint64_t bytes;              // 0x00
  uint64_t alignment;          // 0x08
  uint32_t mips;               // 0x10
  uint32_t planes;             // 0x14
  PlaneRecord planes_[4];
};
static_assert(sizeof(ResourceLayout) == 0x1b38);

// The mode a level in the tail is described with: the 256-byte mode of its
// kind (assumed).
uint32_t tail_swizzle(uint32_t swizzle) {
  switch (swizzle) {
    case 2: case 6: case 10: case 18: case 22: case 26: return 2;
    case 23: case 27: return 3;
    case 24: return 24;
    default: return 1;
  }
}

void describe_level(const Computer& c, const gpu::Layout& l, uint32_t i, LevelRecord* out) {
  const gpu::Level& m = l.levels[i];
  const bool is3d = c.desc.dimension == 4;
  const uint32_t block = l.surface.block;
  const uint64_t element = 1ull << l.surface.element_log2;
  const uint32_t array = c.desc.depth_or_array ? c.desc.depth_or_array : 1;
  out->pitch_texels = m.pitch * block;
  out->row_pitch = static_cast<uint32_t>(m.pitch * element);
  out->padded[0] = m.pitch;
  out->padded[1] = m.rows;
  out->padded[2] = is3d ? m.depth_pitch : array;
  out->elements[0] = m.width;
  out->elements[1] = m.height;
  out->texels[0] = std::max<uint32_t>(1, static_cast<uint32_t>(c.desc.width >> i));
  out->texels[1] = std::max<uint32_t>(1, c.desc.height >> i);
  out->depth_or_array = is3d ? m.depth : array;
  out->samples = l.surface.samples;
  out->slice_depth = l.block_d;
  const uint64_t slice = static_cast<uint64_t>(m.pitch) * m.rows * element;
  const uint64_t depth = is3d ? m.depth_pitch : 1;
  if (!m.in_tail) {
    out->offset = m.offset;
    out->slice_bytes = slice * l.surface.samples;
    out->bytes = out->slice_bytes * depth;
    out->alignment = static_cast<uint32_t>(l.alignment);
    out->swizzle = l.surface.swizzle;
  } else {
    out->in_tail = 1;
    out->alignment = 256;
    out->swizzle = tail_swizzle(l.surface.swizzle);
    if (l.thick) {
      out->slice_bytes = l.alignment / l.block_d;  // a 3D level in the tail is placed by position
      out->tail_position = m.tail_x | (m.tail_y << 11);
    } else {
      out->offset = m.tail_offset;
      out->slice_bytes = slice;
    }
    out->bytes = out->slice_bytes * depth;
  }
}

// Slot 2: the layout the other slots follow.
void STDMETHODCALLTYPE resource_layout(Computer* self, ResourceLayout* out) {
  if (!out) return;
  std::memset(out, 0, sizeof(*out));
  if (!self->console) {
    // The desktop's copyable footprints, one plane.
    const holy::lib::TextureShape shape = shape_of(self->desc);
    const uint32_t mips = std::min<uint32_t>(shape.mips, 15);
    const uint32_t slices = shape.dimension == 4 ? 1 : shape.depth_or_array;
    holy::lib::SubresourceLayout level;
    if (!holy::lib::subresource_layout(shape, 0, &level)) return;
    PlaneRecord& plane = out->planes_[0];
    uint64_t slice_end = 0;
    for (uint32_t mip = 0; mip < mips && holy::lib::subresource_layout(shape, mip, &level); ++mip) {
      LevelRecord& record = plane.levels[mip];
      record.bytes = level.size;
      record.offset = level.offset;
      record.slice_bytes = static_cast<uint64_t>(level.row_pitch) * level.rows;
      record.row_pitch = level.row_pitch;
      slice_end = level.offset + level.size;
    }
    holy::lib::SubresourceLayout next_slice;
    plane.array_pitch = slices > 1 && holy::lib::subresource_layout(shape, shape.mips, &next_slice)
                            ? next_slice.offset
                            : holy::lib::align_up(slice_end, 512);
    plane.bytes = holy::lib::texture_size(shape);
    out->bytes = plane.bytes;
    out->alignment = 512;
    out->mips = shape.mips;
    out->planes = 1;
    return;
  }
  const gpu::Texture& t = self->texture;
  out->bytes = t.size;
  out->alignment = t.alignment;
  out->mips = self->mips;
  out->planes = t.plane_count;
  const bool depth = (self->desc.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  for (uint32_t p = 0; p < t.plane_count; ++p) {
    const gpu::Layout& l = t.planes[p];
    PlaneRecord& plane = out->planes_[p];
    plane.usage = p == 1 ? 8 : depth && (t.plane_count > 1 || self->mips == 1) ? 7 : 1;
    plane.bytes = l.size;
    plane.array_pitch = l.slice_size;
    plane.offset = t.plane_offset[p];
    plane.alignment = l.alignment;
    plane.element_bytes = 1u << l.surface.element_log2;
    plane.standard_mips = l.first_tail_mip;
    plane.tail_mips = l.surface.mips - l.first_tail_mip;
    for (uint32_t i = 0; i < std::min<uint32_t>(l.surface.mips, 15); ++i) describe_level(*self, l, i, &plane.levels[i]);
  }
}

// The plane layout, level and slice a copy names; false when out of range.
bool copy_target(const Computer* self, uint32_t plane, uint32_t subresource, const gpu::Layout** layout,
                 uint32_t* mip, uint32_t* slice, uint64_t* plane_offset) {
  if (plane >= self->texture.plane_count) return false;
  *layout = &self->texture.planes[plane];
  *mip = subresource % self->mips;
  *slice = subresource / self->mips;
  *plane_offset = self->texture.plane_offset[plane];
  return *slice < ((*layout)->surface.dimension == gpu::Dimension::k3D ? 1 : (*layout)->surface.depth);
}

// Slot 8 CopyIntoSubresource(texture memory, plane, subresource, rows,
// row pitch, depth pitch): the title's texel rows into the layout.
HRESULT STDMETHODCALLTYPE copy_into_subresource(Computer* self, void* destination, uint32_t plane,
                                                uint32_t subresource, const void* source, uint32_t row_pitch,
                                                uint32_t slice_pitch) {
  if (!destination || !source) return E_INVALIDARG;
  if (!self->console) {
    const holy::lib::TextureShape shape = shape_of(self->desc);
    holy::lib::SubresourceLayout layout;
    if (plane != 0 || !holy::lib::subresource_layout(shape, subresource, &layout)) return E_INVALIDARG;
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
  const gpu::Layout* l;
  uint32_t mip, slice;
  uint64_t plane_offset;
  if (!copy_target(self, plane, subresource, &l, &mip, &slice, &plane_offset) ||
      !gpu::tile(*l, static_cast<uint8_t*>(destination) + plane_offset, l->size, mip, slice,
                 static_cast<const uint8_t*>(source), row_pitch, slice_pitch)) {
    LIB_LOG(holy::lib::kError, "xg", "copy into subresource %u plane %u of format %u (swizzle %u): refused",
            subresource, plane, self->desc.format, self->texture.planes[0].surface.swizzle);
    return E_INVALIDARG;
  }
  return S_OK;
}

// Slot 9 CopyFromSubresource(rows, plane, subresource, sample, texture
// memory, row pitch, depth pitch): a subresource out of the layout.
HRESULT STDMETHODCALLTYPE copy_from_subresource(Computer* self, void* destination, uint32_t plane,
                                                uint32_t subresource, uint32_t, const void* source,
                                                uint32_t row_pitch, uint32_t slice_pitch) {
  const gpu::Layout* l;
  uint32_t mip, slice;
  uint64_t plane_offset;
  if (!self->console || !destination || !source ||
      !copy_target(self, plane, subresource, &l, &mip, &slice, &plane_offset) ||
      !gpu::detile(*l, static_cast<const uint8_t*>(source) + plane_offset, l->size, mip, slice,
                   static_cast<uint8_t*>(destination), row_pitch, slice_pitch)) {
    LIB_LOG(holy::lib::kError, "xg", "copy from subresource %u plane %u of format %u: refused", subresource, plane,
            self->desc.format);
    return E_INVALIDARG;
  }
  return S_OK;
}

// Observed: slot 6 takes (plane, mip level, x, y, z or array slice, sample,
// bit offset out) and returns a 64-bit byte offset: titles add it to a
// texture's memory address to reach a texel (the element holding it, for
// block-compressed formats) they read or write on the CPU. The bit offset
// (assumed) is written zero when given; 0 for a texel outside the texture.
uint64_t STDMETHODCALLTYPE get_texel_element_offset(Computer* self, uint32_t plane, uint32_t mip, uint64_t x,
                                                     uint32_t y, uint32_t z, uint32_t sample, uint32_t* bit_offset) {
  if (bit_offset) *bit_offset = 0;
  if (!self->console) {
    const holy::lib::TextureShape shape = shape_of(self->desc);
    const holy::lib::FormatInfo f = holy::lib::format_info(shape.format);
    const uint32_t slice = shape.dimension == 4 ? 0 : z;
    holy::lib::SubresourceLayout layout;
    if (plane != 0 || sample != 0 || mip >= shape.mips || !f.block_bytes ||
        !holy::lib::subresource_layout(shape, mip + slice * shape.mips, &layout)) {
      return 0;
    }
    const uint64_t depth = shape.dimension == 4 ? z : 0;
    return layout.offset + (depth * layout.rows + y / f.block_size) * layout.row_pitch +
           x / f.block_size * f.block_bytes;
  }
  if (plane < self->texture.plane_count && mip < self->mips) {
    const gpu::Layout& l = self->texture.planes[plane];
    const gpu::Level& level = l.levels[mip];
    const uint32_t ex = static_cast<uint32_t>(std::min<uint64_t>(x / l.surface.block, UINT32_MAX));
    const uint32_t ey = y / l.surface.block;
    const uint32_t depth = l.surface.dimension == gpu::Dimension::k3D ? level.depth : l.surface.depth;
    if (l.addressable && sample == 0 && ex < level.width && ey < level.height && z < depth) {
      return self->texture.plane_offset[plane] + gpu::element_offset(l, mip, ex, ey, z);
    }
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    LIB_LOG(holy::lib::kError, "xg", "texel offset of plane %u mip %u (%llu, %u, %u) sample %u, format %u: refused",
            plane, mip, static_cast<unsigned long long>(x), y, z, sample, self->desc.format);
  }
  return 0;
}

// Observed: slot 10 takes (tile count*, packed mip info*, tile shape*,
// subresource tiling count* (in/out, 16 seen), first subresource tiling,
// subresource tilings*): the arguments of the desktop
// ID3D12Device::GetResourceTiling after its resource, in 64 KiB tiles: the
// standard levels of each slice from the smallest up after the mip tail
// (packed), as the layout places them. Only layouts of 64 KiB blocks have
// a tiling; others fail (*assumed* as on the console: 4 KiB layouts are not
// tiled resources).
HRESULT STDMETHODCALLTYPE get_resource_tiling(Computer* self, UINT* tiles, D3D12_PACKED_MIP_INFO* packed,
                                              D3D12_TILE_SHAPE* shape, UINT* count, UINT first,
                                              D3D12_SUBRESOURCE_TILING* tilings) {
  constexpr uint64_t kTile = 65536;
  static const bool trace = GetEnvironmentVariableW(L"HOLY_XG_TRACE", nullptr, 0) > 0;
  if (!self->console || self->texture.planes[0].alignment != kTile) {
    if (trace) {
      LIB_LOG(holy::lib::kInfo, "xg", "tiling of %llux%u, format %u, layout 0x%x: not a 64 KiB layout",
              static_cast<unsigned long long>(self->desc.width), self->desc.height, self->desc.format,
              self->desc.layout);
    }
    return E_FAIL;
  }
  const gpu::Layout& l = self->texture.planes[0];
  const uint32_t standard = l.first_tail_mip, mips = self->mips;
  const bool tail = standard < mips;
  if (tiles) *tiles = static_cast<UINT>(l.size / kTile);
  if (packed) {
    packed->NumStandardMips = static_cast<UINT8>(standard);
    packed->NumPackedMips = static_cast<UINT8>(mips - standard);
    packed->NumTilesForPackedMips = tail ? 1 : 0;
    packed->StartTileIndexInOverallResource = 0;  // the first slice's first tile
  }
  if (shape) *shape = {l.block_w * l.surface.block, l.block_h * l.surface.block, l.block_d};
  if (count) {
    const uint32_t slices = l.surface.dimension == gpu::Dimension::k3D ? 1 : l.surface.depth;
    const uint32_t total = mips * slices;
    const uint32_t wanted = first < total ? std::min<uint32_t>(*count, total - first) : 0;
    for (uint32_t i = 0; tilings && i < wanted; ++i) {
      const uint32_t mip = (first + i) % mips, slice = (first + i) / mips;
      if (mip >= standard) {
        tilings[i] = {0, 0, 0, D3D12_PACKED_TILE};
        continue;
      }
      const gpu::Level& m = l.levels[mip];
      tilings[i] = {m.pitch / l.block_w, static_cast<UINT16>(m.rows / l.block_h),
                    static_cast<UINT16>(std::max<uint32_t>(1, m.depth_pitch / l.block_d)),
                    static_cast<UINT>((slice * l.slice_size + m.offset) / kTile)};
    }
    *count = wanted;
  }
  if (trace) {
    LIB_LOG(holy::lib::kInfo, "xg", "tiling of %llux%u x%u, %u mips, format %u: %llu tiles; %u standard, %u packed",
            static_cast<unsigned long long>(self->desc.width), self->desc.height, self->desc.depth_or_array, mips,
            self->desc.format, static_cast<unsigned long long>(l.size / kTile), standard, mips - standard);
  }
  return S_OK;
}

const auto kComputerVtable = [] {
  auto table = holy::lib::make_reporting_vtable<kComputerSlots>();
  table[1] = reinterpret_cast<void*>(&computer_release);
  table[2] = reinterpret_cast<void*>(&resource_layout);
  table[6] = reinterpret_cast<void*>(&get_texel_element_offset);
  table[8] = reinterpret_cast<void*>(&copy_into_subresource);
  table[9] = reinterpret_cast<void*>(&copy_from_subresource);
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

// The layout field the library would pick (0x100 plus the swizzle mode; 1,
// row-major, for linear).
HOLY_EXPORT HRESULT WINAPI XGSuggestSwizzleMode(const void* desc, uint32_t unknown, uint32_t* mode) {
  if (!desc || !mode) return E_POINTER;
  log_desc(unknown ? "swizzle mode (second argument set)" : "swizzle mode", desc);
  const Desc d = read_desc(static_cast<const uint8_t*>(desc));
  *mode = 0x100 | gpu::suggested_swizzle(d.dimension, d.flags);
  return S_OK;
}

HOLY_EXPORT HRESULT WINAPI XGCreateTextureComputer(const void* desc, void** out) {
  if (!desc || !out) return E_POINTER;
  log_desc("texture computer", desc);
  auto* computer = new Computer();
  computer->vtable = kComputerVtable.data();
  computer->info = &kComputerInfo;
  computer->desc = read_desc(static_cast<const uint8_t*>(desc));
  const Desc& d = computer->desc;
  computer->console = gpu::texture_from_desc(d.dimension, d.width, d.height, d.depth_or_array, d.mips, d.format,
                                             d.samples, d.layout, d.flags, &computer->texture);
  computer->mips = computer->console ? computer->texture.planes[0].surface.mips : (d.mips ? d.mips : 1);
  if (!computer->console) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      LIB_LOG(holy::lib::kWarn, "xg",
              "texture computer for format %u (dimension %u, layout 0x%x): no console layout, the desktop's footprints",
              d.format, d.dimension, d.layout);
    }
  }
  *out = computer;
  return S_OK;
}
