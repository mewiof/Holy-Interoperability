#include <cstdint>
#include <vector>

#include "core/libs/lz.h"
#include "tests/unit/test.h"

using holy::lib::lz_compress;
using holy::lib::lz_decompress;

namespace {

bool round_trip(const std::vector<uint8_t>& data, size_t* packed_size = nullptr) {
  const std::vector<uint8_t> packed = lz_compress(data.data(), data.size());
  if (packed_size) *packed_size = packed.size();
  std::vector<uint8_t> back(data.size());
  return lz_decompress(packed.data(), packed.size(), back.data(), back.size()) && back == data;
}

}  // namespace

// Round trips: empty and tiny inputs (no matches), random bytes (literal
// runs past 15 + 255), repetition (long matches, distances of one byte that
// overlap the output), structured 32-bit tokens like translated programs.
TEST(lz_round_trips_and_compresses_repetition) {
  EXPECT(round_trip({}));
  EXPECT(round_trip({1, 2, 3}));
  EXPECT(round_trip(std::vector<uint8_t>(12, 7)));
  std::vector<uint8_t> random(5000);
  uint32_t state = 12345;
  for (uint8_t& b : random) {
    state = state * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(state >> 24);
  }
  EXPECT(round_trip(random));
  size_t packed = 0;
  EXPECT(round_trip(std::vector<uint8_t>(100000, 0xAB), &packed));
  EXPECT(packed < 600);
  std::vector<uint8_t> tokens;
  for (uint32_t i = 0; i < 20000; ++i) {
    const uint32_t words[3] = {0x0A000036u, 0x00100012u + (i % 7), (i % 50) * 4};
    const auto* p = reinterpret_cast<const uint8_t*>(words);
    tokens.insert(tokens.end(), p, p + sizeof(words));
  }
  EXPECT(round_trip(tokens, &packed));
  EXPECT(packed * 4 < tokens.size());  // a period of 350 tokens
  // Distances past 64 KiB are never used: a block repeated after 70 KiB.
  std::vector<uint8_t> far(random.begin(), random.end());
  far.resize(75000, 0);
  far.insert(far.end(), random.begin(), random.end());
  EXPECT(round_trip(far));
}

// Malformed input is refused without writing past the output.
TEST(lz_refuses_malformed_input) {
  std::vector<uint8_t> data(1000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 % 13);
  const std::vector<uint8_t> packed = lz_compress(data.data(), data.size());
  std::vector<uint8_t> out(data.size() + 16, 0xEE);
  // Too small and too large an output.
  EXPECT(!lz_decompress(packed.data(), packed.size(), out.data(), data.size() - 1));
  EXPECT(!lz_decompress(packed.data(), packed.size(), out.data(), data.size() + 1));
  // Truncated input.
  EXPECT(!lz_decompress(packed.data(), packed.size() - 1, out.data(), data.size()));
  // A distance before the start of the output.
  const uint8_t bad_distance[] = {0x10, 0x41, 0x05, 0x00, 0x00};
  EXPECT(!lz_decompress(bad_distance, sizeof(bad_distance), out.data(), 8));
  // A literal count past the input.
  const uint8_t bad_count[] = {0xF0, 0x40, 0x01};
  EXPECT(!lz_decompress(bad_count, sizeof(bad_count), out.data(), 100));
  EXPECT(out[data.size()] == 0xEE);  // nothing written past what was asked
}
