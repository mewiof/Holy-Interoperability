#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/libs/texture_codec.h"
#include "tests/unit/test.h"

using namespace holy::lib::codec;

namespace {

// Tables made for the tests (the format's own are not part of this
// project): uniform models (model 16g + n: every symbol of its 2^(g+1)
// takes 2^(15-g) slots), identity rank tables, a BC7 control word for code
// 0 only (mode 6, one subset), empty dictionaries.
std::vector<uint8_t> test_tables() {
  std::vector<uint8_t> t;
  for (int m = 0; m < 128; ++m) {
    const int g = m / 16;
    for (int b = 0; b < 9; ++b) {
      const uint16_t f = b < g + 2 ? static_cast<uint16_t>(1u << (15 - g)) : 0;
      t.push_back(static_cast<uint8_t>(f));
      t.push_back(static_cast<uint8_t>(f >> 8));
    }
  }
  for (int v = 0; v < 256; ++v) t.push_back(static_cast<uint8_t>(v));  // BC1 ranks
  for (int v = 0; v < 64; ++v) t.push_back(static_cast<uint8_t>(v));   // BC3 alpha
  for (int v = 0; v < 256; ++v) t.push_back(static_cast<uint8_t>(v));  // BC3 colour
  for (int v = 0; v < 64; ++v) t.push_back(static_cast<uint8_t>((v & 7) | ((v >> 3) << 4)));  // BC4: low, next 3 bits
  for (int v = 0; v < 64; ++v) t.push_back(static_cast<uint8_t>(v));  // BC5
  for (int c = 0; c < 512; ++c) {
    const uint16_t ctrl = c == 0 ? 0x46 : 0;
    t.push_back(static_cast<uint8_t>(ctrl));
    t.push_back(static_cast<uint8_t>(ctrl >> 8));
  }
  t.resize(kTablesSize, 0);
  return t;
}

// One fragment of a lane of symbol width `width` (bits - 1) coding
// `symbols` with the uniform model of that width, from the state 2^24 back
// to it (no reset): header, length, payload.
std::vector<uint8_t> fragment(int width, const std::vector<uint8_t>& symbols) {
  const uint32_t f = 1u << (15 - width);
  uint32_t x = 1u << 24;
  std::vector<uint8_t> bytes;
  for (size_t i = symbols.size(); i-- > 0;) {
    const uint32_t s = symbols[i];
    const uint32_t freq = s == 0 ? f - 1 : f, start = s == 0 ? 1 : s * f;
    while (x >= (freq << 16)) {
      bytes.push_back(static_cast<uint8_t>(x));
      x >>= 8;
    }
    x = ((x / freq) << 16) + x % freq + start;
  }
  for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<uint8_t>(x >> (8 * k)));
  std::vector<uint8_t> payload(bytes.rbegin(), bytes.rend());  // the state's top byte first
  std::vector<uint8_t> out;
  if (payload.size() > 256) {
    const size_t n = payload.size() - 257;
    out = {0x20, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)};
  } else {
    out = {0x00, static_cast<uint8_t>(payload.size() - 1)};
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool encode(const Tables& t, uint32_t bc, const std::vector<int>& widths, int per,
            const std::vector<std::vector<uint8_t>>& records, std::vector<uint8_t>* stream) {
  std::vector<std::vector<uint8_t>> lanes(widths.size());
  for (size_t j = 0; j < widths.size(); ++j) {
    std::vector<uint8_t> symbols;
    for (const auto& r : records) {
      for (int k = 0; k < per; ++k) symbols.push_back(r[j * per + k]);
    }
    lanes[j] = fragment(widths[j], symbols);
    // A lane done with its fragment while others decode asks for its next
    // fragment's header; in a stream those bytes are its own. A long
    // fragment parks it: its state reaches 2^24 with payload left, and it
    // asks for nothing more.
    lanes[j].insert(lanes[j].end(), {0x20, 0xFF, 0xFF, 0, 0, 0});
  }
  return schedule(t, bc, records.size(), lanes, stream);
}

const std::vector<int> kBc1Widths = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
const std::vector<int> kBc4Widths = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};

}  // namespace

TEST(texture_codec_tables_are_checked) {
  std::vector<uint8_t> t = test_tables();
  std::string why;
  EXPECT(parse_tables(t.data(), t.size(), &why) != nullptr);
  EXPECT(parse_tables(t.data(), t.size() - 1, &why) == nullptr);
  t[0] = 0;  // model 0's first bucket empty
  t[1] = 0;
  EXPECT(parse_tables(t.data(), t.size(), &why) == nullptr && !why.empty());
}

// One BC1 block, worked by hand: no references (both 0), endpoint residuals
// zigzag-coded, indices one symbol per 2x2 quad (identity ranks).
TEST(texture_codec_decodes_a_bc1_block) {
  const std::vector<uint8_t> raw = test_tables();
  TablesPtr t = parse_tables(raw.data(), raw.size(), nullptr);
  EXPECT(t != nullptr);
  if (!t) return;
  const std::vector<uint8_t> record = {0, 0, 2, 4, 6, 8, 10, 12, 0x1B, 0x00, 0xFF, 0x00};
  std::vector<uint8_t> stream;
  EXPECT(encode(*t, 1, kBc1Widths, 1, {record}, &stream));
  uint8_t block[8] = {};
  const char* why = nullptr;
  EXPECT(decode(*t, 1, stream.data(), stream.size(), block, 8, &why));
  // Endpoints: zigzag 2, 4, 6, 8, 10, 12 = 1, 2, 3, 4, 5, 6 (5:6:5 bits).
  const uint16_t c0 = static_cast<uint16_t>((1 << 11) | (2 << 5) | 3), c1 = static_cast<uint16_t>((4 << 11) | (5 << 5) | 6);
  // Quad 0 (pixels 0, 1, 4, 5) takes 0x1B: positions 3, 2, 1, 0; quad 2
  // (pixels 8, 9, 12, 13) 0xFF: position 3. Positions to codes 0 2 3 1.
  const uint32_t bits = (1u << 0) | (3u << 2) | (2u << 8) | (1u << 16) | (1u << 18) | (1u << 24) | (1u << 26);
  uint8_t expected[8];
  std::memcpy(expected, &c0, 2);
  std::memcpy(expected + 2, &c1, 2);
  std::memcpy(expected + 4, &bits, 4);
  EXPECT(std::memcmp(block, expected, 8) == 0);
}

// A BC4 block, and a second that refers to it for its endpoints and indices
// with no residual: the same block again.
TEST(texture_codec_blocks_refer_to_earlier_blocks) {
  const std::vector<uint8_t> raw = test_tables();
  TablesPtr t = parse_tables(raw.data(), raw.size(), nullptr);
  if (!t) {
    EXPECT(false);
    return;
  }
  const std::vector<uint8_t> first = {0, 0, 7, 200, 1, 2, 3, 4, 5, 6, 7, 8};
  const std::vector<uint8_t> second = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // delta 0, index reference 1
  std::vector<uint8_t> stream;
  EXPECT(encode(*t, 4, kBc4Widths, 1, {first, second}, &stream));
  uint8_t blocks[16] = {};
  EXPECT(decode(*t, 4, stream.data(), stream.size(), blocks, 16, nullptr));
  EXPECT(std::memcmp(blocks, blocks + 8, 8) == 0);
  // Endpoints: zigzag 7 = -4 mod 256 = 252, zigzag 200 = 100; stored 128 apart.
  EXPECT(blocks[0] == (252 ^ 0x80) && blocks[1] == (100 ^ 0x80));
}

// Many blocks (long fragments, the schedule's byte budget per round), and a
// stream cut short fails rather than reading past its end.
TEST(texture_codec_long_streams_and_truncation) {
  const std::vector<uint8_t> raw = test_tables();
  TablesPtr t = parse_tables(raw.data(), raw.size(), nullptr);
  if (!t) {
    EXPECT(false);
    return;
  }
  std::vector<std::vector<uint8_t>> records;
  uint32_t seed = 12345;
  for (int i = 0; i < 300; ++i) {
    std::vector<uint8_t> r(12);
    for (size_t j = 0; j < r.size(); ++j) {
      seed = seed * 1103515245u + 12345u;
      r[j] = static_cast<uint8_t>((seed >> 16) & ((2u << kBc1Widths[j]) - 1));
    }
    r[0] = r[1] = 0;  // no references
    records.push_back(r);
  }
  std::vector<uint8_t> stream;
  EXPECT(encode(*t, 1, kBc1Widths, 1, records, &stream));
  std::vector<uint8_t> out(300 * 8);
  EXPECT(decode(*t, 1, stream.data(), stream.size(), out.data(), out.size(), nullptr));
  // Block 299's endpoints as its residuals give them.
  const auto& last = records.back();
  auto zigzag = [](uint8_t v, int bits) { return ((v >> 1) ^ ((v & 1) ? (1 << bits) - 1 : 0)) & ((1 << bits) - 1); };
  const uint16_t c0 = static_cast<uint16_t>((zigzag(last[2], 5) << 11) | (zigzag(last[3], 6) << 5) | zigzag(last[4], 5));
  uint16_t got = 0;
  std::memcpy(&got, out.data() + 299 * 8, 2);
  EXPECT(got == c0);
  const char* why = nullptr;
  EXPECT(!decode(*t, 1, stream.data(), stream.size() / 2, out.data(), out.size(), &why) && why != nullptr);
  EXPECT(!decode(*t, 2, stream.data(), stream.size(), out.data(), out.size(), &why));  // BC2: not a format of the codec
  EXPECT(!decode(*t, 1, stream.data(), stream.size(), out.data(), 12, &why));         // not whole blocks
}

// With the format's own tables (HOLY_TEXTURE_CODEC_TABLES names a file;
// skipped without one), they parse.
TEST(texture_codec_supplied_tables_parse) {
  const char* path = std::getenv("HOLY_TEXTURE_CODEC_TABLES");
  if (!path || !*path) return;
  FILE* file = std::fopen(path, "rb");
  if (!file) {
    EXPECT(false);
    return;
  }
  std::vector<uint8_t> bytes(kTablesSize + 1);
  const size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  std::string why;
  EXPECT(parse_tables(bytes.data(), got, &why) != nullptr);
}
