#include <cstring>
#include <vector>

#include "core/libs/gpu_layout.h"
#include "tests/unit/test.h"

using namespace holy::lib::gpu;

namespace {

constexpr uint32_t kLinear = 0, k256S = 1, k256D = 2, k4KS = 5, k4KD = 6, k64KS = 9, k64KD = 10;

Surface surface(Dimension dimension, uint32_t swizzle, uint32_t element_bytes, uint32_t block, uint32_t width,
                uint32_t height, uint32_t depth, uint32_t mips) {
  Surface s;
  s.dimension = dimension;
  s.swizzle = swizzle;
  while ((1u << s.element_log2) < element_bytes) ++s.element_log2;
  s.block = block;
  s.width = width;
  s.height = height;
  s.depth = depth;
  s.mips = mips;
  return s;
}

// FNV-1a over the 8 little-endian bytes of every element's address: levels in
// order, then slices (a 3D level's depth, else the array), rows, columns.
uint64_t address_hash(const Layout& l) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint32_t mip = 0; mip < l.surface.mips; ++mip) {
    const Level& level = l.levels[mip];
    const uint32_t slices = l.surface.dimension == Dimension::k3D ? level.depth : l.surface.depth;
    for (uint32_t z = 0; z < slices; ++z) {
      for (uint32_t y = 0; y < level.height; ++y) {
        for (uint32_t x = 0; x < level.width; ++x) {
          const uint64_t at = element_offset(l, mip, x, y, z);
          for (int byte = 0; byte < 8; ++byte) h = (h ^ ((at >> (8 * byte)) & 0xFF)) * 0x100000001b3ull;
        }
      }
    }
  }
  return h;
}

}  // namespace

// Layouts as AMD's public address library computes them for this GPU
// generation (Navi 21 settings, no pipe-bank XOR): the surface size, the first
// level in the mip tail (the whole chain's length when there is none) and the
// address hash of every element. Element sizes 1-16 bytes, block compression,
// arrays, mip tails, deep volumes, every mode element_offset addresses.
TEST(gpu_layout_matches_the_address_library) {
  struct Case {
    uint32_t swizzle;
    Dimension dimension;
    uint32_t element_bytes, block, width, height, depth, mips;
    uint64_t size;
    uint32_t first_tail;
    uint64_t hash;
  };
  const Dimension k1 = Dimension::k1D, k2 = Dimension::k2D, k3 = Dimension::k3D;
  const Case cases[] = {
      {k256S, k2, 1, 1, 83, 31, 1, 6, 5120, 6, 0x435e9cad96ffdd81ull},
      {k256D, k2, 16, 4, 109, 71, 1, 6, 13824, 6, 0xbc2bb84fc5287cb3ull},
      {k4KS, k2, 8, 1, 250, 175, 1, 8, 495616, 4, 0x1e8772404dcdc56eull},
      {k4KS, k2, 16, 1, 70, 70, 3, 4, 479232, 4, 0xf9486d1d511cd75cull},
      {k4KD, k2, 1, 1, 33, 17, 1, 6, 8192, 1, 0xaae10e3f15a1892eull},
      {k64KS, k2, 2, 1, 40, 9, 1, 3, 65536, 0, 0xf7a37fe641c2d565ull},
      {k64KS, k2, 4, 1, 100, 60, 6, 7, 786432, 1, 0xe6b750a0b65e09a3ull},
      {k64KS, k2, 8, 1, 65, 33, 1, 7, 131072, 1, 0xdf0a7ddb9b9341eeull},
      {k64KD, k2, 16, 4, 256, 256, 1, 9, 131072, 1, 0xe569763003d83e28ull},
      {k64KD, k2, 4, 1, 100, 60, 1, 7, 131072, 1, 0xb1db1641adaf77a6ull},
      {k64KD, k2, 1, 1, 255, 129, 2, 8, 262144, 1, 0xd70642f59a015115ull},
      {kLinear, k2, 4, 1, 30, 20, 1, 5, 10240, 5, 0x819a9c1281e640f2ull},
      {kLinear, k2, 2, 1, 17, 9, 3, 5, 15360, 5, 0xbf1a74f17803df62ull},
      {kLinear, k1, 4, 1, 1000, 1, 1, 10, 9216, 10, 0xd37ef9aca5decab4ull},
      {k4KS, k3, 4, 1, 64, 32, 16, 7, 188416, 3, 0xf06bd42a718390e6ull},
      {k4KS, k3, 4, 1, 4, 4, 16, 5, 8192, 0, 0xbf87001bad72d375ull},
      {k4KS, k3, 1, 1, 1, 1, 17, 2, 8192, 0, 0xcdf44ee333e5e03dull},
      {k64KS, k3, 4, 1, 16, 16, 64, 4, 262144, 0, 0x5d1aa7afc371e8e5ull},
      {k64KS, k3, 1, 1, 8, 8, 128, 4, 262144, 0, 0x8fecf01065426225ull},
      {k64KS, k3, 16, 1, 33, 17, 40, 6, 1966080, 3, 0xec9cfd44b89c2fd5ull},
      {kLinear, k3, 4, 1, 9, 7, 5, 3, 16640, 3, 0x6d5b351d4a3d7b5aull},
  };
  for (const Case& c : cases) {
    Layout l;
    EXPECT(make_layout(surface(c.dimension, c.swizzle, c.element_bytes, c.block, c.width, c.height, c.depth, c.mips),
                       &l));
    EXPECT(l.addressable);
    EXPECT(l.size == c.size);
    EXPECT(l.first_tail_mip == c.first_tail);
    EXPECT(address_hash(l) == c.hash);
  }
}

// Addresses checked by hand against the micro block patterns.
TEST(gpu_layout_addresses_by_hand) {
  Layout l;
  EXPECT(make_layout(surface(Dimension::k2D, k64KD, 4, 1, 128, 128, 1, 1), &l));
  EXPECT(l.block_w == 128 && l.block_h == 128);
  EXPECT(element_offset(l, 0, 1, 0, 0) == (1u << 2));
  EXPECT(element_offset(l, 0, 4, 0, 0) == (1u << 6));
  EXPECT(element_offset(l, 0, 0, 4, 0) == (1u << 7));
  EXPECT(element_offset(l, 0, 0, 8, 0) == (1u << 8));
  EXPECT(element_offset(l, 0, 64, 64, 0) == ((1u << 15) | (1u << 14)));
  EXPECT(make_layout(surface(Dimension::k2D, k4KS, 16, 1, 32, 32, 1, 1), &l));
  EXPECT(l.block_w == 16 && l.block_h == 16);
  EXPECT(element_offset(l, 0, 0, 1, 0) == (1u << 4));
  EXPECT(element_offset(l, 0, 1, 0, 0) == (1u << 6));
  EXPECT(element_offset(l, 0, 16, 0, 0) == 4096);  // blocks row by row
  EXPECT(element_offset(l, 0, 0, 16, 0) == 8192);
}

// The mip tail comes first: a 256x256 BC7 chain in 64 KiB display blocks is
// one tail block (levels 1-8), then level 0; 4 KiB blocks hold fewer levels
// in their tail (8 at most).
TEST(gpu_layout_mip_tail_first) {
  Layout l;
  EXPECT(make_layout(surface(Dimension::k2D, k64KD, 16, 4, 256, 256, 1, 9), &l));
  EXPECT(l.first_tail_mip == 1 && l.size == 2 * 65536 && l.levels[0].offset == 65536);
  EXPECT(l.levels[1].in_tail && l.levels[1].tail_offset == 32768 && l.levels[1].tail_x == 32 &&
         l.levels[1].tail_y == 0);
  EXPECT(element_offset(l, 1, 0, 0, 0) == 32768);
  EXPECT(make_layout(surface(Dimension::k2D, k4KS, 16, 4, 256, 256, 1, 9), &l));
  EXPECT(l.first_tail_mip == 3);
  EXPECT(l.levels[2].offset == 4096 && l.levels[1].offset == 8192 && l.levels[0].offset == 8192 + 16384);
}

// A volume deeper than its block keeps its chain in each slab's tail.
TEST(gpu_layout_deep_volume) {
  Layout l;
  EXPECT(make_layout(surface(Dimension::k3D, k64KS, 4, 1, 16, 16, 64, 4), &l));
  EXPECT(l.thick && l.block_w == 32 && l.block_h == 32 && l.block_d == 16);
  EXPECT(l.first_tail_mip == 0 && l.size == 4 * 65536);
  EXPECT(element_offset(l, 0, 0, 0, 0) == 32768);
  EXPECT(element_offset(l, 0, 0, 0, 15) == 42128);
  EXPECT(element_offset(l, 0, 0, 0, 16) == 65536 + 32768);
  EXPECT(element_offset(l, 0, 15, 15, 63) == 3 * 65536 + 49148);
  EXPECT(element_offset(l, 1, 0, 0, 31) == 65536 + 25744);
  EXPECT(element_offset(l, 3, 1, 1, 7) == 3740);
}

// Detiling then tiling gives the bytes back, and every element of every
// level has its own address.
TEST(gpu_layout_copies_round_trip) {
  const Surface shapes[] = {
      surface(Dimension::k2D, k4KS, 8, 4, 300, 200, 2, 7),
      surface(Dimension::k2D, k64KD, 4, 1, 129, 65, 1, 8),
      surface(Dimension::k3D, k4KS, 2, 1, 40, 24, 20, 4),
      surface(Dimension::k2D, kLinear, 1, 1, 77, 33, 1, 5),
  };
  for (const Surface& s : shapes) {
    Layout l;
    EXPECT(make_layout(s, &l));
    std::vector<uint8_t> tiled(l.size), back(l.size, 0);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = static_cast<uint8_t>(i * 131 + (i >> 9));
    std::vector<uint8_t> marks(l.size, 0);
    bool distinct = true;
    for (uint32_t mip = 0; mip < s.mips; ++mip) {
      const Level& level = l.levels[mip];
      const bool is3d = s.dimension == Dimension::k3D;
      const uint64_t row_pitch = (static_cast<uint64_t>(level.width) << s.element_log2) + 3;
      const uint64_t depth_pitch = row_pitch * level.height;
      std::vector<uint8_t> rows(depth_pitch * (is3d ? level.depth : 1));
      for (uint32_t slice = 0; slice < (is3d ? 1 : s.depth); ++slice) {
        EXPECT(detile(l, tiled.data(), tiled.size(), mip, slice, rows.data(), row_pitch, depth_pitch));
        EXPECT(tile(l, back.data(), back.size(), mip, slice, rows.data(), row_pitch, depth_pitch));
        for (uint32_t z = 0; z < (is3d ? level.depth : 1); ++z) {
          for (uint32_t y = 0; y < level.height; ++y) {
            for (uint32_t x = 0; x < level.width; ++x) {
              const uint64_t at = element_offset(l, mip, x, y, is3d ? z : slice);
              distinct = distinct && at < l.size && marks[at] == 0;
              if (at < l.size) marks[at] = 1;
            }
          }
        }
      }
    }
    EXPECT(distinct);
    bool same = true;
    for (size_t i = 0; i < l.size; ++i) same = same && (marks[i] == 0 || back[i] == tiled[i]);
    EXPECT(same);
  }
}

// The smaller levels lie at the start: they need only the memory before them.
TEST(gpu_layout_smaller_levels_first) {
  Layout l;
  EXPECT(make_layout(surface(Dimension::k2D, k4KS, 8, 4, 1024, 1024, 1, 9), &l));
  uint64_t previous = bytes_from(l, 0);
  EXPECT(previous == l.size);
  for (uint32_t mip = 1; mip < 9; ++mip) {
    const uint64_t need = bytes_from(l, mip);
    EXPECT(need <= previous);
    previous = need;
    const Level& level = l.levels[mip];
    std::vector<uint8_t> rows(static_cast<size_t>(level.width) * 8 * level.height);
    std::vector<uint8_t> tiled(need);
    EXPECT(detile(l, tiled.data(), need, mip, 0, rows.data(), level.width * 8, rows.size()));
    EXPECT(!detile(l, tiled.data(), need - 1, mip, 0, rows.data(), level.width * 8, rows.size()));
  }
  EXPECT(bytes_from(l, 3) * 4 < l.size);
}

// Depth with stencil is two planes; the suggested modes' sizes.
TEST(gpu_layout_textures) {
  Texture t;
  EXPECT(make_texture(Dimension::k2D, 1920, 1080, 1, 1, 20 /* D32_FLOAT_S8X24_UINT */, 1, 24, &t));
  EXPECT(t.plane_count == 2);
  EXPECT(t.planes[0].size == 1920ull * 1152 * 4);  // 4-byte depth in 128x128 blocks
  EXPECT(t.plane_offset[1] == t.planes[0].size);
  EXPECT(make_texture(Dimension::k2D, 512, 512, 1, 0, 71 /* BC1 */, 1, k4KS, &t));
  EXPECT(t.plane_count == 1 && t.planes[0].surface.mips == 10 && t.alignment == 4096);
  EXPECT(!make_texture(Dimension::k3D, 16, 16, 16, 1, 28, 1, k256D, &t));  // 3D cannot use 256-byte blocks
}
