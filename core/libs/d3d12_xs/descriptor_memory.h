// Descriptors in title memory (layout in shader/descriptor_layout.h).
#pragma once

#include <cstdint>

namespace holy::d3d12 {

// `title_handle` is a title CPU handle; nothing is written outside heaps.
void write_buffer_descriptor(uint64_t title_handle, uint64_t address, uint32_t stride, uint32_t records,
                             uint32_t word3 = 0);
// Word 3 of a buffer descriptor for a typed view of `dxgi_format`: the RDNA 2
// buffer format (bits 12-18) and destination selects (bits 0-11, three bits a
// component: 0 zero, 1 one, 4-7 the loaded x-w), as native typed loads
// (buffer_load_format_*) read them; 0 (format invalid: loads read zero) for
// formats without a buffer format.
uint32_t typed_buffer_word3(uint32_t dxgi_format);
void write_indexed_descriptor(uint64_t title_handle, uint32_t kind);
// Copies `count` descriptors' memory; indexed descriptors get the index of
// the slot they land in.
void copy_descriptor_memory(uint64_t destination, uint64_t source, uint32_t count);

}  // namespace holy::d3d12
