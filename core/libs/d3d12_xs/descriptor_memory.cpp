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

void write_buffer_descriptor(uint64_t title_handle, uint64_t address, uint32_t stride, uint32_t records) {
  DescriptorLocation where;
  if (!find_descriptor(title_handle, &where)) return;
  const uint32_t words[8] = {static_cast<uint32_t>(address),
                             static_cast<uint32_t>((address >> 32) & 0xFFFF) | ((stride & 0x3FFF) << 16), records, 0,
                             0, 0, 0, 0};
  store(where, words);
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
