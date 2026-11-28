// The console GPU's texture memory layout (gpu_layout.h).
#include "core/libs/gpu_layout.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace holy::lib::gpu {

namespace {

constexpr uint32_t kMaxExtent = 1u << 14;  // the GPU's image descriptors hold up to 16384 per side
constexpr uint32_t kNoBlock = 0xFF;

uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}
uint32_t ceil_div(uint32_t value, uint32_t divisor) {
  return (value + divisor - 1) / divisor;
}

// Elements along one side of level `mip` as its data has them: texels halved
// (rounding down, at least 1), then whole elements.
uint32_t level_extent(uint32_t texels, uint32_t mip, uint32_t block) {
  return ceil_div(std::max(1u, texels >> mip), block);
}
// The same for placement (padding, the tail test, offsets): the address
// library halves level 0's elements rounding up, which is one more than
// level_extent for some odd sizes.
uint32_t placed_extent(uint32_t texels, uint32_t mip, uint32_t block) {
  return ceil_div(ceil_div(std::max(1u, texels), block), 1u << mip);
}

bool standard_mode(uint32_t s) {
  return s == 1 || s == 5 || s == 9 || s == 17 || s == 21 || s == 25;
}
bool display_mode(uint32_t s) {
  return s == 2 || s == 6 || s == 10 || s == 18 || s == 22 || s == 26;
}

// 256-byte micro blocks in elements, by log2 of the element size.
constexpr uint32_t kMicro2D[5][2] = {{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}};
constexpr uint32_t kMicro3D[5][3] = {{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}};
// 1 KiB thick blocks, the base the larger thick blocks grow from.
constexpr uint32_t kThick1K[5][3] = {{16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}};

// The micro block patterns: for each address bit from the element's size up
// to bit 7, the coordinate bit it takes, as axis * 16 + bit (axis 0 x, 1 y,
// 2 z). Thin standard, thin display, thick (standard and display 3D).
constexpr uint8_t X(uint8_t b) {
  return b;
}
constexpr uint8_t Y(uint8_t b) {
  return 16 + b;
}
constexpr uint8_t Z(uint8_t b) {
  return 32 + b;
}
struct MicroPattern {
  uint8_t count;
  uint8_t bits[8];
};
constexpr MicroPattern kPatterns[3][5] = {
    {
        {8, {X(0), X(1), X(2), X(3), Y(0), Y(1), Y(2), Y(3)}},
        {7, {X(0), X(1), X(2), Y(0), Y(1), Y(2), X(3)}},
        {6, {X(0), X(1), Y(0), Y(1), Y(2), X(2)}},
        {5, {X(0), Y(0), Y(1), X(1), X(2)}},
        {4, {Y(0), Y(1), X(0), X(1)}},
    },
    {
        {8, {X(0), X(1), X(2), Y(1), Y(0), Y(2), X(3), Y(3)}},
        {7, {X(0), X(1), X(2), Y(0), Y(1), Y(2), X(3)}},
        {6, {X(0), X(1), Y(0), Y(1), X(2), Y(2)}},
        {5, {X(0), Y(0), X(1), X(2), Y(1)}},
        {4, {X(0), Y(0), X(1), Y(1)}},
    },
    {
        {8, {X(0), X(1), Z(0), Y(0), Z(1), Y(1), X(2), Z(2)}},
        {7, {X(0), Z(0), Y(0), Z(1), Y(1), X(1), Z(2)}},
        {6, {X(0), Y(0), Z(0), Y(1), X(1), Z(1)}},
        {5, {X(0), Z(0), Y(0), X(1), Z(1)}},
        {4, {Z(0), Y(0), X(0), Z(1)}},
    },
};

// Fills the layout's coordinate bits: the micro pattern, then the block's
// remaining address bits to y, x (thin) or y, x, z (thick) in turn.
void fill_pattern(Layout* l, uint32_t kind) {
  const MicroPattern& micro = kPatterns[kind][l->surface.element_log2];
  for (uint32_t i = 0; i < micro.count; ++i) {
    const uint32_t axis = micro.bits[i] >> 4, bit = micro.bits[i] & 15;
    l->coordinate_bits[axis][bit] = static_cast<uint8_t>(l->surface.element_log2 + i);
    l->coordinate_bit_count[axis] = static_cast<uint8_t>(std::max<uint32_t>(l->coordinate_bit_count[axis], bit + 1));
  }
  static const uint32_t kThinOrder[2] = {1, 0}, kThickOrder[3] = {1, 0, 2};
  const uint32_t* order = kind == 2 ? kThickOrder : kThinOrder;
  const uint32_t period = kind == 2 ? 3 : 2;
  for (uint32_t address_bit = 8, i = 0; address_bit < l->block_log2; ++address_bit, ++i) {
    const uint32_t axis = order[i % period];
    l->coordinate_bits[axis][l->coordinate_bit_count[axis]++] = static_cast<uint8_t>(address_bit);
  }
}

// Byte offset inside a block of coordinate `c` along `axis`.
uint64_t spread(const Layout& l, uint32_t axis, uint32_t c) {
  uint64_t out = 0;
  for (uint32_t i = 0; i < l.coordinate_bit_count[axis]; ++i) out |= static_cast<uint64_t>((c >> i) & 1) << l.coordinate_bits[axis][i];
  return out;
}

void make_linear(const Surface& s, Layout* l) {
  const uint32_t bytes = 1u << s.element_log2;
  const uint32_t align = 256 / bytes;
  const bool is3d = s.dimension == Dimension::k3D;
  uint64_t offset = 0;
  for (uint32_t i = s.mips; i-- > 0;) {
    Level& level = l->levels[i];
    level.width = level_extent(s.width, i, s.block);
    level.height = level_extent(s.height, i, s.block);
    level.depth = is3d ? level_extent(s.depth, i, 1) : 1;
    level.pitch = static_cast<uint32_t>(align_up(placed_extent(s.width, i, s.block), align));
    level.rows = placed_extent(s.height, i, s.block);
    level.depth_pitch = is3d ? s.depth : 1;
    level.offset = offset;
    offset += static_cast<uint64_t>(level.pitch) * level.rows * bytes;
  }
  l->block_w = align;
  l->slices = s.depth;
  l->slice_size = offset;
  l->size = offset * s.depth;
  l->alignment = 256;
  l->first_tail_mip = s.mips;
  l->addressable = true;
}

void make_tiled(const Surface& s, Layout* l) {
  const uint32_t e = s.element_log2;
  const uint32_t b = l->block_log2;
  const uint64_t bytes = 1ull << e;
  const uint64_t block_bytes = 1ull << b;
  const bool is3d = s.dimension == Dimension::k3D;
  l->thick = is3d && (standard_mode(s.swizzle) || display_mode(s.swizzle));
  if (l->thick) {
    const uint32_t grow = (b - 10) / 3, rest = (b - 10) % 3;
    l->block_w = kThick1K[e][0] << grow;
    l->block_h = kThick1K[e][1] << (grow + rest / 2);
    l->block_d = kThick1K[e][2] << (grow + (rest ? 1 : 0));
  } else {
    uint32_t samples_log2 = 0;
    while ((1u << samples_log2) < s.samples) ++samples_log2;
    const uint32_t n = b - e - samples_log2;
    const bool wide = (samples_log2 & 1) == 0 || (b & 1) != 0;
    const uint32_t w = (n + (wide ? 1 : 0)) / 2;
    l->block_w = 1u << w;
    l->block_h = 1u << (n - w);
    l->block_d = 1;
  }
  const uint32_t all_slices = static_cast<uint32_t>(align_up(s.depth, l->block_d));
  auto placed_depth = [&](uint32_t mip) { return l->thick ? placed_extent(s.depth, mip, 1) : 1u; };
  auto data_depth = [&](uint32_t mip) { return is3d ? level_extent(s.depth, mip, 1) : 1u; };
  uint64_t slice_size = 0;
  l->first_tail_mip = s.mips;
  if (s.mips == 1) {
    Level& level = l->levels[0];
    level.width = level_extent(s.width, 0, s.block);
    level.height = level_extent(s.height, 0, s.block);
    level.depth = data_depth(0);
    level.pitch = static_cast<uint32_t>(align_up(level.width, l->block_w));
    level.rows = static_cast<uint32_t>(align_up(level.height, l->block_h));
    level.depth_pitch = is3d ? all_slices : 1;
    slice_size = static_cast<uint64_t>(level.pitch) * level.rows * bytes * s.samples;
  } else {
    // The largest level the tail takes: the block halved along one side.
    uint32_t tail[3] = {l->block_w, l->block_h, l->block_d};
    if (l->thick) {
      tail[b % 3 == 0 ? 1 : b % 3 == 1 ? 0 : 2] >>= 1;
    } else {
      tail[0] >>= 1;
    }
    // How many levels the tail can hold; 256-byte blocks have no tail.
    const uint32_t effective = l->thick ? b - (b - 8) / 3 : b;
    const uint32_t max_in_tail = effective == 8 ? 0 : effective < 12 ? 1 + (1u << (effective - 9)) : effective - 4;
    // Depth with 8- and 16-bit elements in the Z mode enters the tail as if
    // it had 32-bit elements.
    uint32_t fits[2] = {tail[0], tail[1]};
    if (s.swizzle == 24 && e <= 1) {
      fits[0] /= kMicro2D[e][0] / kMicro2D[2][0];
      fits[1] /= kMicro2D[e][1] / kMicro2D[2][1];
    }
    uint64_t sizes[kMaxLevels] = {};
    for (uint32_t i = 0; i < s.mips; ++i) {
      const uint32_t w = placed_extent(s.width, i, s.block), h = placed_extent(s.height, i, s.block);
      if (w <= fits[0] && h <= fits[1] && s.mips - i <= max_in_tail) {
        l->first_tail_mip = i;
        slice_size += block_bytes / l->block_d;
        break;
      }
      Level& level = l->levels[i];
      level.width = level_extent(s.width, i, s.block);
      level.height = level_extent(s.height, i, s.block);
      level.depth = data_depth(i);
      level.pitch = static_cast<uint32_t>(align_up(w, l->block_w));
      level.rows = static_cast<uint32_t>(align_up(h, l->block_h));
      level.depth_pitch = is3d ? static_cast<uint32_t>(align_up(placed_depth(i), l->block_d)) : 1;
      const uint64_t one = static_cast<uint64_t>(level.pitch) * level.rows * bytes;
      slice_size += one;
      sizes[i] = one * l->block_d;
    }
    // Levels outside the tail from the smallest up, after the tail block.
    uint64_t offset = l->first_tail_mip < s.mips ? block_bytes : 0;
    for (uint32_t i = l->first_tail_mip; i-- > 0;) {
      l->levels[i].offset = offset;
      offset += sizes[i];
    }
    // Levels in the tail: each at a fixed offset in the tail block, from
    // which its element position follows (address bits 8 and up of the
    // offset alternate y and x).
    const uint32_t micro_w = l->thick ? kMicro3D[e][0] : kMicro2D[e][0];
    const uint32_t micro_h = l->thick ? kMicro3D[e][1] : kMicro2D[e][1];
    uint32_t pitch = tail[0], rows = tail[1];
    for (uint32_t i = l->first_tail_mip; i < s.mips; ++i) {
      const uint32_t m = max_in_tail - 1 - (i - l->first_tail_mip);
      const uint32_t offset_in_tail = m > 6 ? 16u << m : m << 8;
      uint32_t x = 0, y = 0;
      for (uint32_t k = 0; k < 6; ++k) {
        y |= ((offset_in_tail >> (8 + 2 * k)) & 1) << k;
        x |= ((offset_in_tail >> (9 + 2 * k)) & 1) << k;
      }
      if (b & 1) {
        std::swap(x, y);
        if (e & 1) {
          y = (y << 1) | (x & 1);
          x >>= 1;
        }
      }
      Level& level = l->levels[i];
      level.width = level_extent(s.width, i, s.block);
      level.height = level_extent(s.height, i, s.block);
      level.depth = data_depth(i);
      level.pitch = pitch;
      level.rows = rows;
      level.depth_pitch = l->thick ? static_cast<uint32_t>(align_up(placed_depth(i), kMicro3D[e][2])) : 1;
      level.in_tail = true;
      level.tail_offset = offset_in_tail;
      level.tail_x = x * micro_w;
      level.tail_y = y * micro_h;
      pitch = std::max(pitch >> 1, micro_w);
      rows = std::max(rows >> 1, micro_h);
    }
  }
  // A thick slab holds the chains of its block_d depth slices together.
  l->slice_size = slice_size * (l->thick ? l->block_d : 1);
  l->slices = all_slices / l->block_d;
  l->size = l->slice_size * l->slices;
  l->alignment = block_bytes;
  l->addressable = !swizzle_is_xor(s.swizzle) && s.samples == 1;
  if (l->addressable) fill_pattern(l, l->thick ? 2 : display_mode(s.swizzle) ? 1 : 0);
}

}  // namespace

uint32_t swizzle_block_log2(uint32_t swizzle) {
  switch (swizzle) {
    case 0: return 0;
    case 1: case 2: return 8;
    case 5: case 6: case 21: case 22: case 23: return 12;
    case 9: case 10: case 17: case 18: case 24: case 25: case 26: case 27: return 16;
    default: return kNoBlock;
  }
}

bool swizzle_is_xor(uint32_t swizzle) {
  return swizzle >= 16;
}

bool make_layout(const Surface& s, Layout* out) {
  *out = Layout{};
  out->surface = s;
  const uint32_t b = swizzle_block_log2(s.swizzle);
  const bool power_of_two_samples = s.samples && (s.samples & (s.samples - 1)) == 0;
  bool valid = b != kNoBlock && s.width >= 1 && s.width <= kMaxExtent && s.height >= 1 && s.height <= kMaxExtent &&
               s.depth >= 1 && s.depth <= kMaxExtent && s.mips >= 1 && s.mips <= kMaxLevels && s.element_log2 <= 4 &&
               (s.block == 1 || s.block == 4) && power_of_two_samples && s.samples <= 8;
  if (valid && s.samples > 1) {
    valid = s.block == 1 && s.dimension == Dimension::k2D && s.mips == 1 && s.swizzle != 0;
  }
  if (valid && s.dimension == Dimension::k1D) valid = s.height == 1;
  if (valid && s.dimension == Dimension::k3D) {
    const uint32_t m = s.swizzle;
    valid = m == 0 || m == 5 || m == 9 || m == 17 || m == 21 || (m >= 24 && m <= 27);
  }
  if (!valid) return false;
  out->block_log2 = b;
  if (b == 0) {
    make_linear(s, out);
  } else {
    make_tiled(s, out);
  }
  return true;
}

uint64_t element_offset(const Layout& l, uint32_t mip, uint32_t x, uint32_t y, uint32_t z) {
  const Level& level = l.levels[mip];
  const uint32_t slab = l.thick ? z / l.block_d : z, inner = l.thick ? z % l.block_d : 0;
  const uint64_t base = static_cast<uint64_t>(slab) * l.slice_size + level.offset;
  const uint64_t bytes = 1ull << l.surface.element_log2;
  if (l.block_log2 == 0) return base + (static_cast<uint64_t>(y) * level.pitch + x) * bytes;
  const uint32_t ty = y + level.tail_y, tx = x + level.tail_x;
  const uint64_t blocks_per_row = level.pitch / l.block_w;
  const uint64_t row = ((ty / l.block_h) * blocks_per_row << l.block_log2) | spread(l, 1, ty % l.block_h) |
                       spread(l, 2, inner);
  const uint64_t column = (static_cast<uint64_t>(tx / l.block_w) << l.block_log2) + spread(l, 0, tx % l.block_w);
  return base + row + column;
}

uint64_t bytes_from(const Layout& l, uint32_t first) {
  if (first == 0 || first >= l.surface.mips || l.slices != 1 || l.thick || l.surface.dimension == Dimension::k3D ||
      l.surface.samples != 1) {
    return l.size;
  }
  uint64_t end = 0;
  for (uint32_t i = first; i < l.surface.mips; ++i) {
    const Level& level = l.levels[i];
    const uint64_t level_bytes = (static_cast<uint64_t>(level.pitch) * level.rows) << l.surface.element_log2;
    end = std::max(end, level.in_tail ? l.alignment : level.offset + level_bytes);
  }
  return std::min(end, l.size);
}

namespace {

// Copies one level of one slice between the layout and rows, in the
// direction `to_rows` says.
bool walk(const Layout& l, uint8_t* tiled, uint64_t tiled_size, uint32_t mip, uint32_t slice, uint32_t y0,
          uint32_t y1, uint8_t* rows, uint64_t row_pitch, uint64_t depth_pitch, bool to_rows) {
  if (!l.addressable || mip >= l.surface.mips) return false;
  const Level& level = l.levels[mip];
  const bool is3d = l.surface.dimension == Dimension::k3D;
  if (is3d ? slice != 0 : slice >= l.slices) return false;
  const uint32_t element = 1u << l.surface.element_log2;
  const uint64_t row_bytes = static_cast<uint64_t>(level.width) * element;
  y1 = std::min(y1, level.height);
  if (y0 >= y1) return false;
  if (row_pitch < row_bytes || (level.depth > 1 && depth_pitch < row_pitch * (y1 - y0 - 1) + row_bytes)) {
    return false;
  }
  // Every address of the level lies below bytes_from (whole blocks).
  if (tiled_size < bytes_from(l, mip)) return false;
  // Columns: the part of an address that depends on x, as runs of elements
  // contiguous in both layouts.
  struct Run {
    uint64_t tiled;
    uint32_t linear, bytes;
  };
  thread_local std::vector<Run> runs;
  runs.clear();
  if (l.block_log2 == 0) {
    runs.push_back({0, 0, static_cast<uint32_t>(row_bytes)});
  } else {
    // An address is a row part (y, z) plus a column part (x): the column
    // offsets relative to x = 0 hold for every row.
    const uint64_t first = element_offset(l, mip, 0, 0, 0);
    for (uint32_t x = 0; x < level.width; ++x) {
      const uint64_t at = element_offset(l, mip, x, 0, 0) - first;
      if (!runs.empty() && runs.back().tiled + runs.back().bytes == at) {
        runs.back().bytes += element;
      } else {
        runs.push_back({at, x * element, element});
      }
    }
  }
  for (uint32_t z = 0; z < (is3d ? level.depth : 1); ++z) {
    const uint32_t zc = is3d ? z : slice;
    for (uint32_t y = y0; y < y1; ++y) {
      const uint64_t start = element_offset(l, mip, 0, y, zc);
      uint8_t* out = rows + z * depth_pitch + (y - y0) * row_pitch;
      if (to_rows) {
        for (const Run& run : runs) std::memcpy(out + run.linear, tiled + start + run.tiled, run.bytes);
      } else {
        for (const Run& run : runs) std::memcpy(tiled + start + run.tiled, out + run.linear, run.bytes);
      }
    }
  }
  return true;
}

}  // namespace

bool detile(const Layout& l, const uint8_t* tiled, uint64_t tiled_size, uint32_t mip, uint32_t slice, uint8_t* rows,
            uint64_t row_pitch, uint64_t depth_pitch) {
  return walk(l, const_cast<uint8_t*>(tiled), tiled_size, mip, slice, 0, ~0u, rows, row_pitch, depth_pitch, true);
}

bool detile_rows(const Layout& l, const uint8_t* tiled, uint64_t tiled_size, uint32_t mip, uint32_t slice, uint32_t y0,
                 uint32_t y1, uint8_t* rows, uint64_t row_pitch, uint64_t depth_pitch) {
  return walk(l, const_cast<uint8_t*>(tiled), tiled_size, mip, slice, y0, y1, rows, row_pitch, depth_pitch, true);
}

bool tile(const Layout& l, uint8_t* tiled, uint64_t tiled_size, uint32_t mip, uint32_t slice, const uint8_t* rows,
          uint64_t row_pitch, uint64_t depth_pitch) {
  return walk(l, tiled, tiled_size, mip, slice, 0, ~0u, const_cast<uint8_t*>(rows), row_pitch, depth_pitch, false);
}

bool format_planes(uint32_t f, FormatPlanes* out) {
  *out = FormatPlanes{};
  switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      out->element_log2 = 2;
      out->stencil = true;
      return true;
    default:
      break;
  }
  uint32_t bytes = 0, block = 1;
  switch (f) {
    case 1: case 2: case 3: case 4: bytes = 16; break;                 // R32G32B32A32
    case 9: case 10: case 11: case 12: case 13: case 14: case 15:     // R16G16B16A16
    case 16: case 17: case 18: bytes = 8; break;                      // R32G32
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
      bytes = 8, block = 4;
      break;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
      bytes = 16, block = 4;
      break;
    default:
      if ((f >= 23 && f <= 43) || f == 67 || (f >= 87 && f <= 93) || f == 116 || f == 117) {
        bytes = 4;  // 32-bit colour and depth formats, including the BGRA and shared-exponent ones
      } else if ((f >= 48 && f <= 59) || f == 85 || f == 86 || f == 115) {
        bytes = 2;
      } else if (f >= 60 && f <= 65) {
        bytes = 1;
      } else if (f == 189 || f == 191) {
        bytes = 4;  // console-only formats
      } else if (f == 190) {
        bytes = 1;
      } else if (f >= 118 && f <= 120) {
        out->stencil = true;  // console-only 16-bit depth with stencil
        bytes = 2;
      }
      break;
  }
  if (!bytes) return false;
  while ((1u << out->element_log2) < bytes) ++out->element_log2;
  out->block = block;
  return true;
}

bool make_texture(Dimension dimension, uint32_t width, uint32_t height, uint32_t depth_or_array, uint32_t mips,
                  uint32_t dxgi_format, uint32_t samples, uint32_t swizzle, Texture* out) {
  *out = Texture{};
  FormatPlanes f;
  if (!format_planes(dxgi_format, &f)) return false;
  if (mips == 0) {
    const uint32_t largest = std::max({width, height, dimension == Dimension::k3D ? depth_or_array : 1u, 1u});
    while ((largest >> mips) > 0) ++mips;
  }
  Surface s;
  s.dimension = dimension;
  s.width = width;
  s.height = height;
  s.depth = depth_or_array;
  s.mips = mips;
  s.element_log2 = f.element_log2;
  s.block = f.block;
  s.samples = samples ? samples : 1;
  s.swizzle = swizzle;
  if (!make_layout(s, &out->planes[0])) return false;
  out->plane_count = 1;
  if (f.stencil) {
    s.element_log2 = 0;
    if (!make_layout(s, &out->planes[1])) return false;
    out->plane_offset[1] = align_up(out->planes[0].size, out->planes[1].alignment);
    out->plane_count = 2;
  }
  const uint32_t last = out->plane_count - 1;
  out->size = out->plane_offset[last] + out->planes[last].size;
  out->alignment = std::max(out->planes[0].alignment, out->planes[last].alignment);
  return true;
}

uint32_t suggested_swizzle(uint32_t d3d_dimension, uint32_t flags) {
  if (d3d_dimension == 2) return 0;
  if (flags & 2) return 24;  // D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
  if (flags & 1) return 27;  // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  return 5;
}

bool texture_from_desc(uint32_t d3d_dimension, uint64_t width, uint32_t height, uint32_t depth_or_array,
                       uint32_t mips, uint32_t format, uint32_t samples, uint32_t layout, uint32_t flags,
                       Texture* out) {
  Dimension dimension;
  switch (d3d_dimension) {
    case 2: dimension = Dimension::k1D; break;
    case 3: dimension = Dimension::k2D; break;
    case 4: dimension = Dimension::k3D; break;
    default: return false;
  }
  uint32_t swizzle;
  if (layout == 0) {
    swizzle = suggested_swizzle(d3d_dimension, flags);
  } else if (layout == 1) {
    swizzle = 0;
  } else if ((layout & ~0xFFu) == 0x100) {
    swizzle = layout & 0xFF;
  } else {
    return false;
  }
  if (width == 0 || width > kMaxExtent) return false;
  return make_texture(dimension, static_cast<uint32_t>(width), height ? height : 1, depth_or_array ? depth_or_array : 1,
                      mips, format, samples, swizzle, out);
}

}  // namespace holy::lib::gpu
