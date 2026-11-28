// The console GPU's memory layout of textures: how big a texture is, where
// each mip level and slice of it begins, the address of every element, and
// copies out of that layout into rows.
//
// The rules are the GPU family's (RDNA2), as AMD's public address library
// (addrlib, MIT-licensed, shipped in Mesa under src/amd/addrlib) computes
// them for this generation: block shapes, level placement and mip tails
// (ComputeSurfaceInfo / ComputeThinBlockDimension / ComputeThickBlockDimension
// / GetMipTailDim / GetMaxNumMipsInTail, the linear rules) and the 256-byte
// micro block patterns of the standard and display swizzle modes. This is an
// implementation of those rules written for this project, checked against
// that library's results (tests/unit/test_gpu_layout.cpp).
//
// Units: an element is a texel, or a 4x4 block of a block-compressed format.
// Each array slice (or, for thick 3D layouts, each slab of block-depth
// slices) holds its whole mip chain, smallest levels first: the mip tail (the
// small levels sharing one block) at the slice's start, level 0 last.
//
// Observed: titles stream textures into memory in this layout (64 KiB tiles
// of their archives land at the tile positions of the levels), size the
// memory by it, and read level ranges out of compressed chunks by it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace holy::lib::gpu {

enum class Dimension : uint8_t { k1D, k2D, k3D };

// Swizzle modes by the GPU's numbering: 0 linear; 1 and 2 256-byte standard
// and display; 5 and 6 4 KiB; 9 and 10 64 KiB; 17 and 18 64 KiB with pipe
// and bank XOR ("_T"); 21-23 4 KiB and 24-27 64 KiB XOR modes (Z, S, D, R).
// The other numbers do not exist on this GPU generation.
uint32_t swizzle_block_log2(uint32_t swizzle);  // 0 linear; 0xFF for none
bool swizzle_is_xor(uint32_t swizzle);

struct Surface {
  Dimension dimension = Dimension::k2D;
  uint32_t width = 1, height = 1;
  uint32_t depth = 1;  // a 3D texture's depth, else the array size
  uint32_t mips = 1;
  uint32_t element_log2 = 0;  // log2 of the bytes of an element (0-4)
  uint32_t block = 1;         // texels per element side: 1, or 4 for block compression
  uint32_t samples = 1;
  uint32_t swizzle = 0;
};

struct Level {
  uint32_t width = 0, height = 0, depth = 1;  // elements (depth: of a 3D texture's level)
  uint32_t pitch = 0, rows = 0;               // padded elements
  uint32_t depth_pitch = 1;
  uint64_t offset = 0;  // of its first block within the slice (levels in the tail: 0)
  bool in_tail = false;
  uint32_t tail_offset = 0, tail_x = 0, tail_y = 0;  // where in the tail block it starts
};

constexpr uint32_t kMaxLevels = 16;

struct Layout {
  Surface surface;
  uint32_t block_log2 = 0;                   // 0: linear
  uint32_t block_w = 1, block_h = 1, block_d = 1;  // block in elements (linear: pitch alignment)
  bool thick = false;                        // 3D blocks spanning block_d depth slices
  uint32_t slices = 1;                       // array slices, thick slabs, or thin 3D depth slices
  uint64_t slice_size = 0;                   // bytes of one slice: its whole mip chain
  uint64_t size = 0;
  uint64_t alignment = 256;
  uint32_t first_tail_mip = 0;               // surface.mips when there is no tail
  Level levels[kMaxLevels];
  // Inside a block: the address bit each bit of an element coordinate lands
  // on, per axis (x, y, z). Empty for linear layouts and the XOR modes.
  uint8_t coordinate_bits[3][16] = {};
  uint8_t coordinate_bit_count[3] = {};
  bool addressable = false;  // element_offset and detile work (linear, or a non-XOR mode, one sample)
};

// False for descriptions that make no texture on this GPU (zero or
// oversized extents, more than 16 levels, an element size the GPU does not
// have, a swizzle mode it does not have or that the dimension cannot use,
// multisampled mip chains or block compression).
bool make_layout(const Surface& surface, Layout* out);

// Byte offset, from the texture's start, of element (x, y) of level `mip` in
// array slice (or 3D depth slice) `z`. The coordinates must lie inside the
// level and the layout must be addressable.
uint64_t element_offset(const Layout& layout, uint32_t mip, uint32_t x, uint32_t y, uint32_t z);

// The bytes from the texture's start that level `first` and the smaller ones
// take (the smaller a level, the nearer the start): titles streaming a
// texture keep memory for the levels they have and another texture right
// behind them. The whole size where slices follow each other.
uint64_t bytes_from(const Layout& layout, uint32_t first);

// Copies level `mip` of array slice `slice` (3D textures: every depth slice
// of the level; `slice` 0) out of `tiled` (at least bytes_from(mip) bytes)
// into rows `row_pitch` bytes apart, depth slices `depth_pitch` apart. False
// if the layout is not addressable or a buffer is too small.
bool detile(const Layout& layout, const uint8_t* tiled, uint64_t tiled_size, uint32_t mip, uint32_t slice,
            uint8_t* rows, uint64_t row_pitch, uint64_t depth_pitch);
// The reverse: rows into the layout (`tiled` at least the layout's size).
bool tile(const Layout& layout, uint8_t* tiled, uint64_t tiled_size, uint32_t mip, uint32_t slice, const uint8_t* rows,
          uint64_t row_pitch, uint64_t depth_pitch);

// The planes a format is stored in on this GPU: depth formats with stencil
// keep their stencil in a second plane of 1-byte elements (D24S8 and D32S8
// alike, the depth plane of 4-byte elements). Element sizes as log2 bytes;
// false for formats whose element size is not a power of two or unknown.
struct FormatPlanes {
  uint32_t element_log2 = 0;
  uint32_t block = 1;
  bool stencil = false;
};
bool format_planes(uint32_t dxgi_format, FormatPlanes* out);

// A whole texture: its planes one after the other (depth, then stencil, the
// second at its alignment). Compression metadata the console adds to render
// targets and depth buffers (not modelled) is not counted.
struct Texture {
  Layout planes[2];
  uint64_t plane_offset[2] = {};
  uint32_t plane_count = 0;
  uint64_t size = 0;
  uint64_t alignment = 0;
};
bool make_texture(Dimension dimension, uint32_t width, uint32_t height, uint32_t depth_or_array, uint32_t mips,
                  uint32_t dxgi_format, uint32_t samples, uint32_t swizzle, Texture* out);

// The swizzle mode the console's layout library picks when a description
// leaves it the choice (layout field 0), and suggests: linear for 1D
// textures, the 64 KiB Z mode (with XOR) for depth targets, the 64 KiB R
// mode (with XOR) for render targets, otherwise 4 KiB standard, at any
// size. `d3d_dimension` and `flags` as in D3D12_RESOURCE_DESC.
uint32_t suggested_swizzle(uint32_t d3d_dimension, uint32_t flags);

// The texture a resource description makes in title memory: dimension 2-4
// (1D, 2D, 3D as D3D12 numbers them), the layout field 0 (the library's
// pick), 1 (row-major: linear) or 0x100 plus a swizzle mode. False for
// buffers, other layout values and shapes make_texture refuses.
bool texture_from_desc(uint32_t d3d_dimension, uint64_t width, uint32_t height, uint32_t depth_or_array,
                       uint32_t mips, uint32_t format, uint32_t samples, uint32_t layout, uint32_t flags,
                       Texture* out);

}  // namespace holy::lib::gpu
