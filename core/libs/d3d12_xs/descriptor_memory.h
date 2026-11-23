// Descriptors in title memory (layout in shader/descriptor_layout.h).
#pragma once

#include <cstdint>

namespace holy::d3d12 {

// `title_handle` is a title CPU handle; nothing is written outside heaps.
void write_buffer_descriptor(uint64_t title_handle, uint64_t address, uint32_t stride, uint32_t records);
void write_indexed_descriptor(uint64_t title_handle, uint32_t kind);
// Copies `count` descriptors' memory; indexed descriptors get the index of
// the slot they land in.
void copy_descriptor_memory(uint64_t destination, uint64_t source, uint32_t count);

}  // namespace holy::d3d12
