#include "core/libs/texture_layout.h"
#include "tests/unit/test.h"

using holy::lib::console_texture_size;
using holy::lib::console_tiling;
using holy::lib::ConsoleTiling;
using holy::lib::TextureShape;

namespace {

TextureShape shape(uint32_t dimension, uint64_t width, uint32_t height, uint32_t depth_or_array, uint32_t mips,
                   uint32_t format) {
  TextureShape s;
  s.dimension = dimension;
  s.width = width;
  s.height = height;
  s.depth_or_array = depth_or_array;
  s.mips = mips;
  s.format = format;
  return s;
}

}  // namespace

// Tile shapes: the documented standard 64 KiB shapes (2D and 3D).
TEST(console_tiling_tile_shapes) {
  ConsoleTiling t;
  EXPECT(console_tiling(shape(3, 1024, 1024, 1, 1, 28), &t));  // 4-byte texels
  EXPECT(t.tile_width == 128 && t.tile_height == 128 && t.tile_depth == 1);
  EXPECT(console_tiling(shape(3, 1024, 1024, 1, 1, 61), &t));  // 1 byte
  EXPECT(t.tile_width == 256 && t.tile_height == 256);
  EXPECT(console_tiling(shape(3, 1024, 1024, 1, 1, 10), &t));  // 8 bytes
  EXPECT(t.tile_width == 128 && t.tile_height == 64);
  EXPECT(console_tiling(shape(3, 1024, 1024, 1, 1, 71), &t));  // BC1: 8-byte 4x4 blocks
  EXPECT(t.tile_width == 512 && t.tile_height == 256);
  EXPECT(console_tiling(shape(4, 256, 256, 256, 1, 61), &t));  // 3D, 1 byte
  EXPECT(t.tile_width == 64 && t.tile_height == 32 && t.tile_depth == 32);
  EXPECT(console_tiling(shape(4, 256, 256, 256, 1, 2), &t));  // 3D, 16 bytes
  EXPECT(t.tile_width == 16 && t.tile_height == 16 && t.tile_depth == 16);
  EXPECT(!console_tiling(shape(3, 64, 64, 1, 1, 6), &t));  // 12-byte texels: not modelled
}

// Levels in each slice: the mip tail first, then from the smallest level to
// level 0; levels that fit in half a tile's width share the tail.
TEST(console_tiling_orders_levels_smallest_first) {
  ConsoleTiling t;
  EXPECT(console_tiling(shape(3, 512, 512, 32, 10, 98), &t));  // BC7
  EXPECT(t.standard_mips == 2 && t.packed_mips == 8 && t.packed_tiles == 1);
  EXPECT(t.mips[1].start == 1 && t.mips[1].width_tiles == 1 && t.mips[1].height_tiles == 1);
  EXPECT(t.mips[0].start == 2 && t.mips[0].width_tiles == 2 && t.mips[0].height_tiles == 2);
  EXPECT(t.slice_tiles == 6 && t.slices == 32);
  // One level: no tail, whatever the size.
  EXPECT(console_tiling(shape(3, 64, 64, 1, 1, 98), &t));
  EXPECT(t.standard_mips == 1 && t.packed_mips == 0 && t.slice_tiles == 1);
  // All levels in the tail.
  EXPECT(console_tiling(shape(3, 64, 64, 1, 7, 98), &t));
  EXPECT(t.standard_mips == 0 && t.packed_mips == 7 && t.slice_tiles == 1);
  // A level wider than the tail but within a tile stays a level of its own.
  EXPECT(console_tiling(shape(3, 1024, 128, 1, 2, 28), &t));  // 4 bytes: tile 128x128, tail 64x128
  EXPECT(t.standard_mips == 2 && t.mips[1].start == 0 && t.mips[0].start == 4 && t.slice_tiles == 12);
}

// The 2D sizes agree with the console size estimate for 64 KiB blocks.
TEST(console_tiling_agrees_with_console_size) {
  const TextureShape shapes[] = {shape(3, 512, 512, 32, 10, 98), shape(3, 1920, 1080, 1, 1, 61),
                                 shape(3, 1024, 1024, 1, 11, 71), shape(3, 300, 200, 6, 9, 28)};
  for (const TextureShape& s : shapes) {
    ConsoleTiling t;
    EXPECT(console_tiling(s, &t));
    EXPECT(static_cast<uint64_t>(t.slice_tiles) * t.slices * 65536 == console_texture_size(s, false));
  }
}
