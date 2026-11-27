// The memory layout of textures in title memory, shared by the texture
// layout library (xg_xs) and the graphics library (d3d12_xs).
//
// The console layout library tells titles where each subresource of a
// texture lies in memory and copies texel data there for them; titles follow
// whatever it reports. The layout chosen here is the desktop's copyable
// footprint layout (documented for ID3D12Device::GetCopyableFootprints):
// subresources in order (mip levels within array slices, then planes), each
// starting at a multiple of 512 bytes, rows of blocks at a pitch that is a
// multiple of 256 bytes. Texture memory in that layout can be copied into a
// host texture with a plain buffer-to-texture copy.
#pragma once

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>

namespace holy::lib {

struct FormatInfo {
  uint32_t block_bytes = 0;  // bytes per texel, or per 4x4 block for block-compressed formats
  uint32_t block_size = 1;   // 1, or 4 for block-compressed formats
};

// Documented DXGI format sizes. Unknown formats report zero bytes.
inline FormatInfo format_info(uint32_t format) {
  switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
      return {16, 1};
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
      return {12, 1};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      return {8, 1};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return {4, 1};
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
      return {2, 1};
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
      return {1, 1};
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
      return {8, 4};
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
      return {16, 4};
    default:
      return {};
  }
}

// The resource description fields the layout depends on (the desktop
// D3D12_RESOURCE_DESC layout: dimension at 0, width at 16, height at 24,
// depth or array size at 28, mip levels at 30, format at 32).
struct TextureShape {
  uint32_t dimension = 0;  // 2 1D, 3 2D, 4 3D
  uint64_t width = 0;
  uint32_t height = 1;
  uint32_t depth_or_array = 1;
  uint32_t mips = 1;
  uint32_t format = 0;
};

struct SubresourceLayout {
  uint64_t offset = 0;      // from the texture's start
  uint32_t row_pitch = 0;   // bytes between rows of blocks
  uint32_t rows = 0;        // rows of blocks per slice
  uint32_t row_bytes = 0;   // bytes of texels in a row
  uint32_t depth = 1;       // slices (3D textures)
  uint64_t size = 0;        // bytes the subresource spans
};

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

inline uint32_t subresource_count(const TextureShape& shape) {
  const uint32_t mips = shape.mips ? shape.mips : 1;
  return mips * (shape.dimension == 4 ? 1 : (shape.depth_or_array ? shape.depth_or_array : 1));
}

// Layout of subresource `index` (mip level fastest, then array slice).
// Returns false for formats whose size is not known.
inline bool subresource_layout(const TextureShape& shape, uint32_t index, SubresourceLayout* out) {
  const FormatInfo f = format_info(shape.format);
  if (!f.block_bytes) return false;
  const uint32_t mips = shape.mips ? shape.mips : 1;
  uint64_t offset = 0;
  for (uint32_t i = 0; i <= index; ++i) {
    const uint32_t mip = i % mips;
    const uint64_t width = (shape.width >> mip) ? (shape.width >> mip) : 1;
    const uint32_t height = (shape.height >> mip) ? (shape.height >> mip) : 1;
    const uint32_t depth =
        shape.dimension == 4 ? ((shape.depth_or_array >> mip) ? (shape.depth_or_array >> mip) : 1) : 1;
    SubresourceLayout layout;
    layout.offset = align_up(offset, 512);
    layout.row_bytes = static_cast<uint32_t>((width + f.block_size - 1) / f.block_size * f.block_bytes);
    layout.row_pitch = static_cast<uint32_t>(align_up(layout.row_bytes, 256));
    layout.rows = (height + f.block_size - 1) / f.block_size;
    layout.depth = depth;
    layout.size = static_cast<uint64_t>(layout.row_pitch) * layout.rows * depth;
    if (i == index) {
      *out = layout;
      return true;
    }
    offset = layout.offset + layout.size;
  }
  return false;
}

// Total bytes of the texture in this layout.
inline uint64_t texture_size(const TextureShape& shape) {
  const uint32_t count = subresource_count(shape);
  SubresourceLayout last;
  if (!count || !subresource_layout(shape, count - 1, &last)) return 0;
  return last.offset + last.size;
}

// The block size, as log2 of its bytes (0: linear), of the console layout a
// texture gets from its description. Observed: the console description's
// layout field is 0x100 plus a swizzle mode when the title names one (titles
// pass the mode the texture layout library suggested, or one of their own:
// 0x102 and 0x105 seen), 0 when the driver chooses, 1 for row-major buffers.
// Swizzle modes (the public numbering of the GPU's surface modes): 0 linear;
// 1-3 256-byte blocks; 4-7 and 20-23 4 KiB blocks; 8-11, 16-19 and 24-27
// 64 KiB blocks; 28-31 variable blocks (taken as 64 KiB). When the driver
// chooses: 4 KiB blocks for small-resource placement (alignment 4 KiB),
// 64 KiB otherwise.
inline uint32_t console_block_log2(uint32_t layout, uint64_t alignment) {
  if (layout == 1) return 0;
  if (layout >= 0x100 && layout < 0x120) {
    const uint32_t mode = layout - 0x100;
    if (mode == 0) return 0;
    if (mode <= 3) return 8;
    if ((mode >= 4 && mode <= 7) || (mode >= 20 && mode <= 23)) return 12;
    return 16;
  }
  return alignment == 4096 ? 12 : 16;
}

// The memory the console GPU's layout of a texture takes, for blocks of
// 2^block_log2 bytes (0: linear); an estimate, the console's own computation
// is not available. Observed: titles size the memory they stream textures
// into by the allocation sizes the graphics library reports, and stream them
// in the console's layout; a size below it made a title read part of a
// compressed chunk and stop at its own check (a 128x4 texture of 8-byte
// texels, the driver's choice of layout at 4 KiB alignment: 8 KiB on the
// host, 16 KiB of stream), and sizes above it overran a title's pools (a
// 256x1 texture of 16-byte texels the title asked to be linear, estimated in
// 4 KiB blocks: 64 KiB).
// Layout (documented in the public descriptions of the GPU's surfaces):
// linear levels are rows 256-byte aligned; tiled levels are whole blocks. A
// block of B bytes holds B / e elements of e bytes (texels, or 4x4 blocks of
// compressed formats) as 2^ceil(n/2) by 2^floor(n/2) of them, n = log2(B / e)
// (the documented standard 64 KiB tile shapes follow the same rule: 128x128
// at 4 bytes, 128x64 at 8, 64x64 compressed blocks at 16). With blocks of
// 4 KiB or more, the levels that fit in half a block (half its width, its
// height; at most 9 levels with 4 KiB blocks, 12 with 64 KiB) share one
// block, the mip tail. 3D textures count each depth slice as a 2D level; zero
// for formats of unknown or odd element size.
inline uint64_t console_texture_size(const TextureShape& shape, uint32_t block_log2, uint32_t samples = 1) {
  const FormatInfo f = format_info(shape.format);
  if (!f.block_bytes || (f.block_bytes & (f.block_bytes - 1)) != 0) return 0;  // 12-byte texels: not modelled
  uint32_t element_log2 = 0;
  while ((1u << element_log2) < f.block_bytes) ++element_log2;
  const bool is3d = shape.dimension == 4;
  const uint32_t slices = is3d ? 1 : shape.depth_or_array;
  const uint32_t mips = shape.mips ? shape.mips : 1;
  uint64_t bytes = 0;
  if (block_log2 == 0) {
    for (uint32_t mip = 0; mip < mips; ++mip) {
      const uint64_t w = std::max<uint64_t>(1, shape.width >> mip), h = std::max<uint64_t>(1, shape.height >> mip);
      const uint64_t d = is3d ? std::max<uint32_t>(1, shape.depth_or_array >> mip) : 1;
      const uint64_t ew = (w + f.block_size - 1) / f.block_size, eh = (h + f.block_size - 1) / f.block_size;
      bytes += align_up(ew << element_log2, 256) * eh * d;
    }
    return align_up(bytes, 256) * slices * (samples ? samples : 1);
  }
  if (block_log2 <= element_log2) return 0;
  const uint32_t n = block_log2 - element_log2;
  const uint64_t block_w = 1ull << ((n + 1) / 2), block_h = 1ull << (n / 2);
  const uint64_t tail_w = block_w / 2, tail_h = block_h;
  const uint32_t max_in_tail = block_log2 >= 16 ? 12 : 9;
  uint64_t blocks = 0;
  bool tail = false;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint64_t w = std::max<uint64_t>(1, shape.width >> mip), h = std::max<uint64_t>(1, shape.height >> mip);
    const uint64_t d = is3d ? std::max<uint32_t>(1, shape.depth_or_array >> mip) : 1;
    const uint64_t ew = (w + f.block_size - 1) / f.block_size, eh = (h + f.block_size - 1) / f.block_size;
    if (block_log2 >= 12 && mips > 1 && ew <= tail_w && eh <= tail_h && mips - mip <= max_in_tail) {
      tail = true;  // this level and the smaller ones share the tail block
      break;
    }
    blocks += ((ew + block_w - 1) / block_w) * ((eh + block_h - 1) / block_h) * d;
  }
  if (tail) blocks += 1;
  return (blocks << block_log2) * slices * (samples ? samples : 1);
}

// The console GPU's tiled layout of a texture in 64 KiB tiles (blocks), as
// the console layout library reports it to titles in a resource tiling
// query (the desktop ID3D12Device::GetResourceTiling outputs).
// Layout (documented in the public descriptions of the GPU's tiled
// surfaces), and how it differs from the desktop's answer:
//  - Tile shapes follow the rule of console_texture_size (2D: 2^ceil(n/2) by
//    2^floor(n/2) elements of e bytes, n = log2(65536 / e)); 3D tiles are
//    thick, 2^ceil(n/3) by 2^round(n/3) by 2^floor(n/3) (the documented
//    standard 3D tile shapes: 64x32x32 at 1 byte down to 16x16x16 at 16).
//  - The levels that fit in the mip tail (half a tile's width, its height;
//    for 3D, its depth too; at most 12 levels, 10 for thick tiles; only for
//    textures with more than one level) share one tile: the packed mips.
//  - Each array slice holds its levels smallest first: the mip tail at the
//    slice's first tile, then the other levels from the smallest to level 0
//    (the desktop answer starts with level 0). Observed: a title placed
//    levels in memory, and found them in its archives, at the tile offsets
//    this reports; archives hold each texture's levels smallest first, and
//    the desktop order made a title read level 1 from the middle of level 0.
struct ConsoleMipTiling {
  uint32_t width_tiles = 0, height_tiles = 0, depth_tiles = 0;
  uint32_t start = 0;  // first tile, within an array slice
};

struct ConsoleTiling {
  uint32_t tile_width = 0, tile_height = 0, tile_depth = 0;  // texels
  uint32_t standard_mips = 0, packed_mips = 0, packed_tiles = 0;
  uint32_t slice_tiles = 0;  // tiles per array slice
  uint32_t slices = 1;
  ConsoleMipTiling mips[16];
};

// False for shapes not modelled: unknown or 12-byte formats, more than 16
// levels, planar formats.
inline bool console_tiling(const TextureShape& shape, ConsoleTiling* out) {
  const FormatInfo f = format_info(shape.format);
  if (!f.block_bytes || (f.block_bytes & (f.block_bytes - 1)) != 0) return false;
  const uint32_t mips = shape.mips ? shape.mips : 1;
  if (mips > 16) return false;
  uint32_t element_log2 = 0;
  while ((1u << element_log2) < f.block_bytes) ++element_log2;
  const uint32_t n = 16 - element_log2;
  const bool is3d = shape.dimension == 4;
  uint64_t tw, th, td;
  if (is3d) {
    tw = 1ull << ((n + 2) / 3);
    th = 1ull << ((n + 1) / 3);
    td = 1ull << (n / 3);
  } else {
    tw = 1ull << ((n + 1) / 2);
    th = 1ull << (n / 2);
    td = 1;
  }
  const uint64_t tail_w = tw / 2, tail_h = th, tail_d = td;
  const uint32_t max_in_tail = is3d ? 10 : 12;
  ConsoleTiling t;
  t.tile_width = static_cast<uint32_t>(tw * f.block_size);
  t.tile_height = static_cast<uint32_t>(th * f.block_size);
  t.tile_depth = static_cast<uint32_t>(td);
  t.slices = is3d ? 1 : (shape.depth_or_array ? shape.depth_or_array : 1);
  t.standard_mips = mips;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint64_t w = std::max<uint64_t>(1, shape.width >> mip);
    const uint64_t h = std::max<uint64_t>(1, (shape.height ? shape.height : 1) >> mip);
    const uint64_t d = is3d ? std::max<uint64_t>(1, shape.depth_or_array >> mip) : 1;
    const uint64_t ew = (w + f.block_size - 1) / f.block_size, eh = (h + f.block_size - 1) / f.block_size;
    if (mips > 1 && ew <= tail_w && eh <= tail_h && d <= tail_d && mips - mip <= max_in_tail) {
      t.standard_mips = mip;
      break;
    }
    ConsoleMipTiling& m = t.mips[mip];
    m.width_tiles = static_cast<uint32_t>((ew + tw - 1) / tw);
    m.height_tiles = static_cast<uint32_t>((eh + th - 1) / th);
    m.depth_tiles = static_cast<uint32_t>((d + td - 1) / td);
  }
  t.packed_mips = mips - t.standard_mips;
  t.packed_tiles = t.packed_mips ? 1 : 0;
  uint32_t next = t.packed_tiles;
  for (uint32_t mip = t.standard_mips; mip-- > 0;) {
    ConsoleMipTiling& m = t.mips[mip];
    m.start = next;
    next += m.width_tiles * m.height_tiles * m.depth_tiles;
  }
  t.slice_tiles = next;
  *out = t;
  return true;
}

}  // namespace holy::lib
