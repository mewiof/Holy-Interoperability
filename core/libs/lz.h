// A fast byte-oriented LZ77 codec for data the runtime keeps on disk
// (translated programs: about 8.7 times smaller, compressed at about
// 440 MB/s and decompressed at about 3.7 GB/s on the development host,
// measured; deflate at level 6 gave 12.9 times at 81 MB/s and 754 MB/s).
//
// Format: a sequence of runs, each a token byte (literal count in the high
// four bits, match length minus 4 in the low four; 15 in either means more
// count follows as bytes of 255 ended by one below 255), the literals, then
// a match: a 16-bit little-endian distance back into the output (1-65535)
// and the length's extra bytes. The last run has literals only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace holy::lib {

// Compresses `size` bytes into a new buffer.
std::vector<uint8_t> lz_compress(const uint8_t* data, size_t size);

// Decompresses `size` bytes into exactly `capacity` bytes at `out`. False on
// malformed input or a size that does not match.
bool lz_decompress(const uint8_t* data, size_t size, uint8_t* out, size_t capacity);

}  // namespace holy::lib
