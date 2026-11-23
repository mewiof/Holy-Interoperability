// Decompression of zlib streams (RFC 1950 framing around RFC 1951 DEFLATE
// data), as used for the native program inside console shader containers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace holy::d3d12 {

// Decompresses a complete zlib stream. Returns false on malformed input or
// when the output would exceed `limit` bytes. The Adler-32 trailer is checked.
bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t limit = 64u << 20);

}  // namespace holy::d3d12
