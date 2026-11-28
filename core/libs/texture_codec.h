// The console's texture codec: the lossless packing of block-compressed
// texture data (BC1, BC3-BC7 block streams) that titles' archives carry and
// the console's fast-storage hardware unpacks (requests with a block format
// number in option bits 1-4, xcrdapi).
//
// Format, as implemented here (decode only):
//  - A stream has no header. It is 12 lanes (BC1, BC3, BC4, BC5: one symbol
//    per lane per block) or 16 lanes (BC6H, BC7: two) of entropy-coded
//    symbols; a fixed schedule decides which lane takes the next input byte
//    (each round the lanes step in order and take at most 8 bytes between
//    them).
//  - A lane is a run of fragments: a header byte (model and alphabet, a
//    state reset flag, a long-length flag), a length (one byte, or two for
//    long fragments), then payload bytes feeding a range-variant asymmetric
//    numeral system decoder (32-bit state, renormalised by bytes below 2^24,
//    16-bit probabilities) under one of 128 static models.
//  - Each block's symbols form a record: two references (to one of the 256
//    blocks before it, or past the first block into a per-format dictionary
//    of 255 entries) for its endpoints and its indices, and residuals added
//    to the referenced values modulo each field's width; the values are then
//    packed into the standard BC block layouts.
//  - The models, index rank tables, BC7 control codes and dictionaries are
//    constants of the format that cannot be derived. This project does not
//    ship them: they are read from a file the user supplies (kTablesSize
//    bytes, layout in texture_codec.cpp), and without it the codec's requests
//    fail as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace holy::lib::codec {

struct Tables;
struct TablesDeleter {
  void operator()(Tables* tables) const;
};
using TablesPtr = std::unique_ptr<Tables, TablesDeleter>;

constexpr size_t kTablesSize = 77982;

// Parses the constant tables; null (and the reason in `why`) when the bytes
// are not a valid table file.
TablesPtr parse_tables(const uint8_t* data, size_t size, std::string* why);

// Decodes a stream of block format `bc` (1, 3, 4, 5, 6 or 7) into `out`,
// whose size gives the number of blocks (8 bytes each for BC1 and BC4, 16
// for the others). Bytes after the last one the lanes need are ignored.
// False (and a reason) for an unsupported format, a destination that is not
// whole blocks, or a stream that ends early or is corrupt.
bool decode(const Tables& tables, uint32_t bc, const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size,
            const char** why);

// For tests and tools: the stream the lanes' bytes make in the schedule's
// order, for `blocks` blocks of format `bc` (one byte vector per lane, as a
// decoder takes them). False if the lanes run out or deliver too few symbols.
bool schedule(const Tables& tables, uint32_t bc, size_t blocks, const std::vector<std::vector<uint8_t>>& lane_bytes,
              std::vector<uint8_t>* stream);

}  // namespace holy::lib::codec
