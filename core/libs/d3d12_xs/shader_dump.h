// Shader blob hashing and saving (see shader_dump.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace holy::d3d12 {

uint64_t shader_hash(const void* bytes, size_t size);
void dump_shader(const void* bytes, size_t size, const char* stage);

}  // namespace holy::d3d12
