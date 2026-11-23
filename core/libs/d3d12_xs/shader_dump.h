// Shader blob hashing and saving (see shader_dump.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace holy::d3d12 {

uint64_t shader_hash(const void* bytes, size_t size);
// Asks the launcher to discard the title's host caches before the next run
// (see kHostCacheDiscard in core/common/paths.h).
void discard_host_caches_after_run();

// Shaders whose desktop program the host refused in an earlier run (by
// shader_hash), kept in the data directory so later runs go straight to the
// native code instead of asking the host again.
bool host_refused_before(uint64_t hash);
void remember_host_refusal(uint64_t hash);

void dump_shader(const void* bytes, size_t size, const char* stage);

}  // namespace holy::d3d12
