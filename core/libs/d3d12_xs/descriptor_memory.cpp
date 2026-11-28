#include "core/libs/d3d12_xs/descriptor_memory.h"

#include <cstring>

#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/shader/descriptor_layout.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

void store(const DescriptorLocation& where, const uint32_t (&words)[8]) {
  HolyEnsureCommitted(reinterpret_cast<void*>(where.title), where.heap->stride);
  std::memcpy(reinterpret_cast<void*>(where.title), words, where.heap->stride);
}

}  // namespace

void write_buffer_descriptor(uint64_t title_handle, uint64_t address, uint32_t stride, uint32_t records,
                             uint32_t word3) {
  DescriptorLocation where;
  if (!find_descriptor(title_handle, &where)) return;
  const uint32_t words[8] = {static_cast<uint32_t>(address),
                             static_cast<uint32_t>((address >> 32) & 0xFFFF) | ((stride & 0x3FFF) << 16), records,
                             word3, 0, 0, 0, 0};
  store(where, words);
}

uint32_t typed_buffer_word3(uint32_t dxgi_format) {
  // Destination selects.
  constexpr uint32_t k0 = 0, k1 = 1, kX = 4, kY = 5, kZ = 6, kW = 7;
  struct Entry {
    uint32_t dxgi, format, x, y, z, w;
  };
  // DXGI format -> RDNA 2 unified buffer format (public instruction set
  // reference: 8_* 1-6, 16_* 7-13, 8_8_* 14-19, 32_* 20-22, 16_16_* 23-29,
  // 10_11_11_* 30-36, 2_10_10_10_* 50-55, 8_8_8_8_* 56-61, 32_32_* 62-64,
  // 16_16_16_16_* 65-71, 32_32_32_* 72-74, 32_32_32_32_* 75-77; within a
  // group unorm, snorm, uscaled, sscaled, uint, sint, float). Components a
  // format lacks read as 0, and alpha as 1, as desktop typed loads do.
  static const Entry kEntries[] = {
      {2, 77, kX, kY, kZ, kW},  {3, 75, kX, kY, kZ, kW},  {4, 76, kX, kY, kZ, kW},   // R32G32B32A32 F/U/S
      {6, 74, kX, kY, kZ, k1},  {7, 72, kX, kY, kZ, k1},  {8, 73, kX, kY, kZ, k1},   // R32G32B32
      {10, 71, kX, kY, kZ, kW}, {11, 65, kX, kY, kZ, kW}, {12, 69, kX, kY, kZ, kW},  // R16G16B16A16 F/UN/U
      {13, 66, kX, kY, kZ, kW}, {14, 70, kX, kY, kZ, kW},                            // SN/S
      {16, 64, kX, kY, k0, k1}, {17, 62, kX, kY, k0, k1}, {18, 63, kX, kY, k0, k1},  // R32G32
      {24, 50, kX, kY, kZ, kW}, {25, 54, kX, kY, kZ, kW},                            // R10G10B10A2 UN/U
      {26, 36, kX, kY, kZ, k1},                                                      // R11G11B10 F
      {28, 56, kX, kY, kZ, kW}, {29, 56, kX, kY, kZ, kW}, {30, 60, kX, kY, kZ, kW},  // R8G8B8A8 UN/sRGB/U
      {31, 57, kX, kY, kZ, kW}, {32, 61, kX, kY, kZ, kW},                            // SN/S
      {34, 29, kX, kY, k0, k1}, {35, 23, kX, kY, k0, k1}, {36, 27, kX, kY, k0, k1},  // R16G16 F/UN/U
      {37, 24, kX, kY, k0, k1}, {38, 28, kX, kY, k0, k1},                            // SN/S
      {41, 22, kX, k0, k0, k1}, {42, 20, kX, k0, k0, k1}, {43, 21, kX, k0, k0, k1},  // R32 F/U/S
      {49, 14, kX, kY, k0, k1}, {50, 18, kX, kY, k0, k1}, {51, 15, kX, kY, k0, k1},  // R8G8 UN/U/SN
      {52, 19, kX, kY, k0, k1},                                                      // S
      {54, 13, kX, k0, k0, k1}, {56, 7, kX, k0, k0, k1},  {57, 11, kX, k0, k0, k1},  // R16 F/UN/U
      {58, 8, kX, k0, k0, k1},  {59, 12, kX, k0, k0, k1},                            // SN/S
      {61, 1, kX, k0, k0, k1},  {62, 5, kX, k0, k0, k1},  {63, 2, kX, k0, k0, k1},   // R8 UN/U/SN
      {64, 6, kX, k0, k0, k1},                                                       // S
      {87, 56, kZ, kY, kX, kW}, {91, 56, kZ, kY, kX, kW},                            // B8G8R8A8 UN/sRGB
      {88, 56, kZ, kY, kX, k1},                                                      // B8G8R8X8
  };
  for (const Entry& e : kEntries) {
    if (e.dxgi == dxgi_format) return e.x | (e.y << 3) | (e.z << 6) | (e.w << 9) | (e.format << 12);
  }
  return 0;
}

void write_indexed_descriptor(uint64_t title_handle, uint32_t kind) {
  DescriptorLocation where;
  if (!find_descriptor(title_handle, &where)) return;
  const uint32_t words[8] = {where.index, kDescriptorMarker, kind, 0, 0, 0, 0, 0};
  store(where, words);
}

void copy_descriptor_memory(uint64_t destination, uint64_t source, uint32_t count) {
  DescriptorLocation first;
  const UINT stride = find_descriptor(destination, &first) ? first.heap->stride : kTitleDescriptorBytes;
  for (uint32_t i = 0; i < count; ++i) {
    DescriptorLocation to, from;
    if (!find_descriptor(destination + static_cast<uint64_t>(i) * stride, &to) ||
        !find_descriptor(source + static_cast<uint64_t>(i) * stride, &from)) {
      continue;
    }
    uint32_t words[8] = {};
    HolyEnsureCommitted(reinterpret_cast<void*>(from.title), from.heap->stride);
    std::memcpy(words, reinterpret_cast<const void*>(from.title), from.heap->stride);
    if (words[1] == kDescriptorMarker) words[0] = to.index;
    store(to, words);
  }
}

}  // namespace holy::d3d12
