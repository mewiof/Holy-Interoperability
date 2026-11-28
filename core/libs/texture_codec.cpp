// The console's texture codec (texture_codec.h).
#include "core/libs/texture_codec.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace holy::lib::codec {

namespace {

constexpr int kModels = 128;
constexpr int kBuckets = 9;
constexpr int kHistory = 256;
constexpr int kDictionary = 255;
constexpr uint32_t kLow = 1u << 24;  // the lane state reads a byte below this, decodes above it
constexpr int kRoundBytes = 8;       // input bytes the lanes take between them per round

// Values a block reconstructs to, before packing.
struct Plain {  // BC1, BC3, BC4, BC5: endpoint fields and indices
  uint8_t ep[8];
  uint8_t ix[32];
};
struct Bc6Ends {  // endpoint 0 R G B, endpoints 1-3; left-aligned in 16 bits, biased
  uint16_t e[4][3];
};
struct Bc6Index {
  uint8_t mode;
  int16_t ix[16];
};
struct Bc7Ends {
  uint16_t ctrl;
  uint16_t rows[3][8];  // per subset: R0 R1 G0 G1 B0 B1 A0 A1, left-aligned
};
struct Bc7Index {
  uint8_t bits[2];
  int16_t ix[2][16];
};

}  // namespace

// The table file, little-endian without padding: 128 models of 9 16-bit
// bucket frequencies (model 16g + n has g + 2 buckets; bucket 0 holds symbol
// 0, bucket b > 0 the 2^(b-1) symbols from 2^(b-1), each of the bucket's
// frequency; a model's frequencies cover 65536 slots), the index rank
// tables (BC1 256 bytes, BC3 alpha 64 and colour 256, BC4 64, BC5 64), 512
// 16-bit BC7 control words, then 255 dictionary entries per format in format
// order: BC1 6 + 16 bytes, BC3 8 + 32, BC4 2 + 16, BC5 4 + 32, BC6H (a mode
// byte not used, 12 16-bit endpoint values, a mode byte, 16 16-bit indices),
// BC7 (a 16-bit control word, 24 16-bit endpoint values, 2 index widths, 2
// bytes not used, 31 16-bit indices).
struct Tables {
  uint32_t freq[kModels][kBuckets];
  uint32_t start[kModels][kBuckets + 1];  // each bucket's first slot; 65536 past the model's last
  uint8_t window[kModels][256];           // the bucket of each 256-slot window's first slot
  uint8_t rank1[256], rank3a[64], rank3c[256], rank4[64], rank5[64];
  uint16_t ctrl7[512];
  Plain bc1[kDictionary], bc3[kDictionary], bc4[kDictionary], bc5[kDictionary];
  Bc6Ends bc6_ends[kDictionary];
  Bc6Index bc6_index[kDictionary];
  Bc7Ends bc7_ends[kDictionary];
  Bc7Index bc7_index[kDictionary];
};

void TablesDeleter::operator()(Tables* tables) const {
  delete tables;
}

namespace {

struct Reader {
  const uint8_t* at;
  const uint8_t* end;
  uint8_t u8() { return at < end ? *at++ : 0; }
  uint16_t u16() {
    const uint16_t low = u8();
    return static_cast<uint16_t>(low | (u8() << 8));
  }
  void bytes(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = u8();
  }
};

}  // namespace

TablesPtr parse_tables(const uint8_t* data, size_t size, std::string* why) {
  if (!data || size != kTablesSize) {
    if (why) *why = "the table file has the wrong size";
    return nullptr;
  }
  TablesPtr t(new Tables());
  Reader r{data, data + size};
  for (int m = 0; m < kModels; ++m) {
    for (int b = 0; b < kBuckets; ++b) t->freq[m][b] = r.u16();
    const int used = m / 16 + 2;
    uint32_t at = 0;
    for (int b = 0; b < used; ++b) {
      if (t->freq[m][b] == 0) {
        if (why) *why = "a model has an empty bucket";
        return nullptr;
      }
      t->start[m][b] = at;
      at += t->freq[m][b] << (b > 0 ? b - 1 : 0);
    }
    if (at != 65536) {
      if (why) *why = "a model does not cover 65536 slots";
      return nullptr;
    }
    for (int b = used; b <= kBuckets; ++b) t->start[m][b] = 65536;
    for (int w = 0; w < 256; ++w) {
      const uint32_t first = static_cast<uint32_t>(w) << 8;
      uint8_t bucket = 0;
      for (int b = 1; b <= kBuckets; ++b) bucket += t->start[m][b] <= first ? 1 : 0;
      t->window[m][w] = bucket;
    }
  }
  r.bytes(t->rank1, 256);
  r.bytes(t->rank3a, 64);
  r.bytes(t->rank3c, 256);
  r.bytes(t->rank4, 64);
  r.bytes(t->rank5, 64);
  for (uint16_t& c : t->ctrl7) c = r.u16();
  auto plain = [&r](Plain* dictionary, int ep, int ix) {
    for (int i = 0; i < kDictionary; ++i) {
      dictionary[i] = Plain{};
      r.bytes(dictionary[i].ep, ep);
      r.bytes(dictionary[i].ix, ix);
    }
  };
  plain(t->bc1, 6, 16);
  plain(t->bc3, 8, 32);
  plain(t->bc4, 2, 16);
  plain(t->bc5, 4, 32);
  for (int i = 0; i < kDictionary; ++i) {
    r.u8();
    for (auto& row : t->bc6_ends[i].e) {
      for (uint16_t& v : row) v = r.u16();
    }
    t->bc6_index[i].mode = r.u8();
    for (int16_t& v : t->bc6_index[i].ix) v = static_cast<int16_t>(r.u16());
  }
  for (int i = 0; i < kDictionary; ++i) {
    t->bc7_ends[i].ctrl = r.u16();
    for (auto& row : t->bc7_ends[i].rows) {
      for (uint16_t& v : row) v = r.u16();
    }
    Bc7Index& x = t->bc7_index[i];
    x = Bc7Index{};
    x.bits[0] = r.u8();
    x.bits[1] = r.u8();
    r.u16();
    for (int k = 0; k < 31; ++k) x.ix[k / 16][k % 16] = static_cast<int16_t>(r.u16());
  }
  return t;
}

namespace {

// --- lanes ---------------------------------------------------------------

struct Lane {
  uint8_t width = 0;  // symbol bits - 1 at the lane's full alphabet
  uint8_t phase = 0;  // 0 header, 1 length, 2 length's high byte, 3 payload
  bool long_length = false;
  bool reset = false;
  uint8_t model = 0;
  bool need = true;  // wants an input byte
  bool has_latch = false;
  uint8_t latch = 0;
  bool has_out = false;
  uint8_t out = 0;
  bool stalled = false;
  uint32_t x = 0;     // decoder state
  uint32_t left = 0;  // payload bytes left in the fragment

  // One step: takes the latched byte (header, length or payload) and decodes
  // at most one symbol.
  void step(const Tables& t) {
    if (phase != 3) {
      if (!has_latch) return;
      const uint8_t b = latch;
      has_latch = false;
      need = true;
      switch (phase) {
        case 0:
          model = static_cast<uint8_t>((width - (b >> 6)) * 16 + (b & 15));  // the top bits shrink the alphabet
          reset = (b & 0x10) != 0;
          long_length = (b & 0x20) != 0;
          left = 0;
          phase = 1;
          break;
        case 1:
          if (long_length) {
            left = b;
            phase = 2;
          } else {
            left = static_cast<uint32_t>(b) + 1;
            phase = 3;
          }
          break;
        default:
          left = ((static_cast<uint32_t>(b) << 8) | left) + 257;
          phase = 3;
          break;
      }
      return;
    }
    uint32_t state = x;
    if (state < kLow && has_latch) {
      has_latch = false;
      --left;
      state = (state << 8) | latch;
    }
    need = left != 0 && !has_latch;
    if (state > kLow) {
      const uint32_t slot = state & 0xFFFF;
      const uint32_t* start = t.start[model];
      uint32_t b = t.window[model][slot >> 8];
      while (start[b + 1] <= slot) ++b;
      uint32_t symbol, f, rest;
      if (b == 0) {
        // Symbol 0 owns slots 1 .. f[0] - 1; slot 0 is never used.
        symbol = 0;
        f = t.freq[model][0] - 1;
        rest = slot - 1;
      } else {
        const uint32_t offset = slot - start[b];
        f = t.freq[model][b];
        const uint32_t q = offset / f;
        symbol = (1u << (b - 1)) + q;
        rest = offset - q * f;
      }
      x = (state >> 16) * f + rest;
      has_out = true;
      out = static_cast<uint8_t>(symbol);
    } else if (left != 0) {
      x = state;  // at exactly 2^24 a lane neither reads nor decodes; only a finished one ends there
    } else {
      // The fragment ends; a byte read in this step does not reach the state.
      phase = 0;
      need = !has_latch;
      if (reset) x = 0;
    }
  }
};

// Decodes `per` symbols per block on every lane into `records` (a record of
// lanes * per symbols per block: lane j's symbols at j * per), taking input
// in the schedule's order.
// `fetch(j)` gives lane j the next input byte (false when there is none).
template <typename Fetch>
bool run_lanes(const Tables& t, const uint8_t* widths, int lane_count, int per, size_t blocks, Fetch&& fetch,
               uint8_t* records) {
  Lane lanes[16];
  for (int j = 0; j < lane_count; ++j) lanes[j].width = widths[j];
  const size_t stride = static_cast<size_t>(lane_count) * per;
  const uint32_t full = (1u << lane_count) - 1;
  size_t symbols = 0, at = 0;
  uint32_t done = 0;
  int idle = 0;
  while (symbols < blocks * per) {
    int taken = 0;
    bool moved = false;
    for (int j = 0; j < lane_count; ++j) {
      Lane& lane = lanes[j];
      const uint32_t bit = 1u << j;
      const int before = taken;
      if (!lane.stalled) lane.step(t);
      const bool waiting = lane.has_out && (done & bit) != 0;
      if (lane.need && !waiting && taken < kRoundBytes && fetch(j, &lane.latch)) {
        lane.has_latch = true;
        ++taken;
      }
      lane.stalled = (lane.need && before >= kRoundBytes) || waiting;
      if (!lane.stalled && lane.has_out) {
        records[at + static_cast<size_t>(j) * per] = lane.out;
        lane.has_out = false;
        done |= bit;
        moved = true;
      }
    }
    if (done == full) {
      ++symbols;
      at += symbols % per == 0 ? stride - per + 1 : 1;
      done = 0;
      moved = true;
    }
    idle = taken > 0 || moved ? 0 : idle + 1;
    if (idle > 3) return false;  // no lane can go on: the stream ends early or is corrupt
  }
  return true;
}

// --- shared record parts ----------------------------------------------------

// Two 4-bit symbols as one byte, the first on the even bits.
uint8_t interleave(uint8_t a, uint8_t b) {
  uint8_t v = 0;
  for (int k = 0; k < 4; ++k) v |= static_cast<uint8_t>((((a >> k) & 1) << (2 * k)) | (((b >> k) & 1) << (2 * k + 1)));
  return v;
}

// The endpoint and index references of a record.
void references(const uint8_t* s, bool paired, int* ep_ref, int* ix_ref) {
  const uint8_t d = paired ? interleave(s[0], s[1]) : s[0];
  const uint8_t ix = paired ? interleave(s[2], s[3]) : s[1];
  const uint8_t delta = static_cast<uint8_t>((d >> 1) ^ static_cast<uint8_t>(-(d & 1)));
  *ep_ref = static_cast<uint8_t>(ix + delta);
  *ix_ref = ix;
}

// Block `i`'s reference `r`: none (0), `r` blocks back, or before the first
// block into the dictionary.
template <typename T>
const T& pick(size_t i, int r, const T* history, const T* dictionary, const T& zero) {
  if (r == 0) return zero;
  if (static_cast<size_t>(r) <= i) return history[(i - r) % kHistory];
  return dictionary[r - i - 1];
}

// A zigzag residual (0, -1, 1, -2, ...) modulo 2^bits.
uint8_t zigzag(uint8_t v, int bits) {
  const uint8_t mask = static_cast<uint8_t>((1u << bits) - 1);
  return static_cast<uint8_t>(((v >> 1) ^ ((v & 1) ? mask : 0)) & mask);
}

// A byte's even and odd bits as two zigzag-coded 4-bit residuals.
void nibbles(uint8_t b, bool odd_first, int16_t out[2]) {
  auto half = [](uint8_t v) {
    uint8_t h = 0;
    for (int k = 0; k < 4; ++k) h |= static_cast<uint8_t>(((v >> (2 * k)) & 1) << k);
    return h;
  };
  auto decode = [](uint8_t u) { return static_cast<int16_t>((u >> 1) ^ -(u & 1)); };
  const int16_t even = decode(half(b)), odd = decode(half(static_cast<uint8_t>(b >> 1)));
  out[0] = odd_first ? odd : even;
  out[1] = odd_first ? even : odd;
}

// An index residual plus its reference, each left-aligned at its own width,
// back to `bits` bits (signed).
int16_t add_index(int16_t residual, int bits, int16_t reference, int reference_bits) {
  auto up = [](int16_t v, int b) -> uint16_t {
    return b == 0 || b > 16 ? 0 : static_cast<uint16_t>(static_cast<uint16_t>(v) << (16 - b));
  };
  const auto sum = static_cast<int16_t>(static_cast<uint16_t>(up(residual, bits) + up(reference, reference_bits)));
  return bits == 0 ? 0 : static_cast<int16_t>(sum >> (16 - bits));
}

// A little-endian bit writer for one 128-bit block.
struct Bits {
  uint64_t word[2] = {};
  uint32_t at = 0;
  void put(uint64_t value, uint32_t n) {
    for (uint32_t i = 0; i < n && at < 128; ++i, ++at) {
      if ((value >> i) & 1) word[at / 64] |= 1ull << (at % 64);
    }
  }
  void store(uint8_t* out) const { std::memcpy(out, word, 16); }
};

// --- BC1, BC3, BC4, BC5 -----------------------------------------------------

struct Widths {
  uint8_t ep[8];
  uint8_t ix[32];
  bool paired;
};

Widths make_widths(std::initializer_list<uint8_t> ep, int ix3, int ix2, bool paired) {
  Widths w{};
  int k = 0;
  for (uint8_t v : ep) w.ep[k++] = v;
  for (int i = 0; i < ix3 + ix2; ++i) w.ix[i] = i < ix3 ? 3 : 2;
  w.paired = paired;
  return w;
}

// 2-bit index residuals: one symbol per 2x2 quad of the block.
void quads(const uint8_t* rank, const uint8_t* s, int n, uint8_t* ix) {
  static const int kQuad[4] = {0, 1, 4, 5};
  for (int q = 0; q < n; ++q) {
    const uint8_t v = rank[s[q]];
    const int base = (q >> 1) * 8 + (q & 1) * 2;
    for (int k = 0; k < 4; ++k) ix[base + kQuad[k]] = (v >> (2 * k)) & 3;
  }
}

// 3-bit index residuals: one symbol per two pixels in raster order.
void pairs(const uint8_t* rank, const uint8_t* s, int n, uint8_t* ix) {
  for (int k = 0; k < n; ++k) {
    const uint8_t v = rank[s[k] & 63];
    ix[2 * k] = v & 7;
    ix[2 * k + 1] = (v >> 4) & 7;
  }
}

// Positions along the endpoint line to BC1 and BC4 codes.
constexpr uint32_t kBc1Code[4] = {0, 2, 3, 1};
constexpr uint64_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

void bc1_colour(const uint8_t* ep, const uint8_t* ix, uint8_t* out) {
  const uint16_t c0 = static_cast<uint16_t>((ep[0] << 11) | (ep[1] << 5) | ep[2]);
  const uint16_t c1 = static_cast<uint16_t>((ep[3] << 11) | (ep[4] << 5) | ep[5]);
  uint32_t bits = 0;
  for (int p = 0; p < 16; ++p) bits |= kBc1Code[ix[p] & 3] << (2 * p);
  std::memcpy(out, &c0, 2);
  std::memcpy(out + 2, &c1, 2);
  std::memcpy(out + 4, &bits, 4);
}

// A BC4 half; endpoints are kept 128 below their stored value.
void bc4_half(uint8_t e0, uint8_t e1, const uint8_t* ix, uint8_t* out) {
  uint64_t v = static_cast<uint64_t>(e0 ^ 0x80) | (static_cast<uint64_t>(e1 ^ 0x80) << 8);
  for (int p = 0; p < 16; ++p) v |= kBc4Code[ix[p] & 7] << (16 + 3 * p);
  std::memcpy(out, &v, 8);
}

Plain residual_plain(const Tables& t, uint32_t bc, const uint8_t* s) {
  Plain v{};
  switch (bc) {
    case 1: {
      static const int kBits[6] = {5, 6, 5, 5, 6, 5};
      for (int k = 0; k < 6; ++k) v.ep[k] = zigzag(s[2 + k], kBits[k]);
      quads(t.rank1, s + 8, 4, v.ix);
      break;
    }
    case 3:
      // Colour lanes carry each channel's two endpoints together.
      v.ep[0] = zigzag(s[14], 5);
      v.ep[1] = zigzag(s[16], 6);
      v.ep[2] = zigzag(s[18], 5);
      v.ep[3] = zigzag(s[15], 5);
      v.ep[4] = zigzag(s[17], 6);
      v.ep[5] = zigzag(s[19], 5);
      v.ep[6] = zigzag(s[4], 8);
      v.ep[7] = zigzag(s[5], 8);
      pairs(t.rank3a, s + 6, 8, v.ix);
      quads(t.rank3c, s + 20, 4, v.ix + 16);
      break;
    case 4:
      v.ep[0] = zigzag(s[2], 8);
      v.ep[1] = zigzag(s[3], 8);
      pairs(t.rank4, s + 4, 8, v.ix);
      break;
    default:  // 5
      for (int k = 0; k < 4; ++k) v.ep[k] = zigzag(s[4 + k], 8);
      pairs(t.rank5, s + 8, 16, v.ix);
      break;
  }
  return v;
}

void pack_plain(uint32_t bc, const Plain& v, uint8_t* out) {
  switch (bc) {
    case 1: bc1_colour(v.ep, v.ix, out); break;
    case 3:
      bc4_half(v.ep[6], v.ep[7], v.ix, out);
      bc1_colour(v.ep, v.ix + 16, out + 8);
      break;
    case 4: bc4_half(v.ep[0], v.ep[1], v.ix, out); break;
    default:
      bc4_half(v.ep[0], v.ep[1], v.ix, out);
      bc4_half(v.ep[2], v.ep[3], v.ix + 16, out + 8);
      break;
  }
}

void decode_plain(const Tables& t, uint32_t bc, const uint8_t* records, size_t stride, size_t blocks, uint8_t* out,
                  size_t block_bytes) {
  static const Widths kBc1 = make_widths({5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false);
  static const Widths kBc3 = make_widths({5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true);
  static const Widths kBc4 = make_widths({8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false);
  static const Widths kBc5 = make_widths({8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true);
  const Widths& w = bc == 1 ? kBc1 : bc == 3 ? kBc3 : bc == 4 ? kBc4 : kBc5;
  const Plain* dictionary = bc == 1 ? t.bc1 : bc == 3 ? t.bc3 : bc == 4 ? t.bc4 : t.bc5;
  std::vector<Plain> history(kHistory, Plain{});
  const Plain zero{};
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t* s = records + i * stride;
    int ep_ref, ix_ref;
    references(s, w.paired, &ep_ref, &ix_ref);
    const Plain& e = pick(i, ep_ref, history.data(), dictionary, zero);
    const Plain& x = pick(i, ix_ref, history.data(), dictionary, zero);
    Plain v = residual_plain(t, bc, s);
    for (int k = 0; k < 8; ++k) v.ep[k] = static_cast<uint8_t>((v.ep[k] + e.ep[k]) & ((1u << w.ep[k]) - 1));
    for (int k = 0; k < 32; ++k) v.ix[k] = static_cast<uint8_t>((v.ix[k] + x.ix[k]) & ((1u << w.ix[k]) - 1));
    history[i % kHistory] = v;
    pack_plain(bc, v, out + i * block_bytes);
  }
}

// --- BC7 (public mode properties and anchor tables) ------------------------

constexpr uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                  15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                                   3, 3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                   8, 15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                   3, 15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
constexpr uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                   15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                   15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                   15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
// Endpoint bits of R G B A per mode, before p-bits and rotation; index bits
// of the primary and secondary sets.
constexpr uint8_t kBc7Bits[8][4] = {{4, 4, 4, 0}, {6, 6, 6, 0}, {5, 5, 5, 0}, {7, 7, 7, 0},
                                    {5, 5, 5, 6}, {7, 7, 7, 8}, {7, 7, 7, 7}, {5, 5, 5, 5}};
constexpr uint8_t kBc7Index[8][2] = {{3, 0}, {3, 0}, {2, 0}, {2, 0}, {3, 2}, {2, 2}, {4, 0}, {2, 0}};

bool has_pbits(uint32_t mode) {
  return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7;
}
uint32_t rotation_of(uint16_t ctrl) {
  const uint32_t mode = ctrl & 7;
  return mode == 4 || mode == 5 ? (ctrl >> 3) & 3 : 0;
}
uint32_t subsets_of(uint16_t ctrl) {
  return (ctrl & 0xFF) >> 6;
}
// Channel precisions: the p-bit as red's lowest bit; a rotation swaps alpha
// with the rotated channel.
void bc7_precision(uint16_t ctrl, uint8_t p[4]) {
  const uint32_t mode = ctrl & 7, rot = rotation_of(ctrl);
  for (int c = 0; c < 4; ++c) p[c] = kBc7Bits[mode][c];
  if (rot) {
    std::swap(p[3], p[rot - 1]);
  } else if (has_pbits(mode)) {
    ++p[0];
  }
}

void bc7_pack(const Bc7Ends& ep, const Bc7Index& ix, const uint8_t prec[4], uint8_t* out) {
  const uint16_t ctrl = ep.ctrl;
  const uint32_t mode = ctrl & 7, subsets = subsets_of(ctrl), part = (ctrl >> 8) & 63, rot = rotation_of(ctrl);
  Bits w;
  w.put(1ull << mode, mode + 1);
  switch (mode) {
    case 0: w.put(part, 4); break;
    case 1: case 2: case 3: case 7: w.put(part, 6); break;
    case 4:
      w.put(rot, 2);
      w.put((ctrl >> 5) & 1, 1);
      break;
    case 5: w.put(rot, 2); break;
    default: break;
  }
  const uint32_t ends = 2 * subsets;
  auto field = [&ep](uint32_t row, uint32_t col, uint32_t p) -> uint64_t { return ep.rows[row][col] >> (16 - p); };
  uint32_t order[4] = {0, 1, 2, 3};
  if (rot) std::swap(order[3], order[rot - 1]);
  for (uint32_t ch : order) {
    const uint32_t p = prec[ch];
    if (!p) continue;
    const uint32_t drop = ch == 0 && has_pbits(mode) ? 1 : 0;
    for (uint32_t e = 0; e < ends; ++e) w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, p - drop);
  }
  if (mode == 1) {
    for (uint32_t row = 0; row < subsets; ++row) w.put(field(row, 0, prec[0]) & 1, 1);
  } else if (has_pbits(mode)) {
    for (uint32_t e = 0; e < ends; ++e) w.put(field(e / 2, e % 2, prec[0]) & 1, 1);
  }
  uint8_t anchors[3] = {0, 0xFF, 0xFF};
  if (subsets == 2) {
    anchors[1] = kAnchor2[part];
  } else if (subsets == 3) {
    anchors[1] = kAnchor3a[part];
    anchors[2] = kAnchor3b[part];
  }
  auto indices = [&](int set) {
    const uint32_t bits = ix.bits[set];
    for (int p = 0; p < 16; ++p) {
      const bool anchor = p == anchors[0] || p == anchors[1] || p == anchors[2];
      w.put(static_cast<uint16_t>(ix.ix[set][p]), bits - (anchor ? 1 : 0));
    }
  };
  if (mode == 4) {
    indices(1);
    indices(0);
  } else if (mode == 5) {
    indices(0);
    indices(1);
  } else {
    indices(0);
  }
  w.store(out);
}

bool decode_bc7(const Tables& t, const uint8_t* records, size_t stride, size_t blocks, uint8_t* out,
                const char** why) {
  struct Value {
    Bc7Ends ends;
    Bc7Index index;
  };
  std::vector<Value> history(kHistory, Value{});
  const Value zero{};
  static const int kOffsets[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t* s = records + i * stride;
    int ep_ref, ix_ref;
    references(s, true, &ep_ref, &ix_ref);
    const uint16_t ctrl = t.ctrl7[((s[31] & 15) << 5) | (s[30] & 31)];
    if (ctrl == 0) {
      *why = "a BC7 control code no block uses";
      return false;
    }
    const uint32_t mode = ctrl & 7, subsets = subsets_of(ctrl);
    uint8_t prec[4];
    bc7_precision(ctrl, prec);
    const bool three = subsets == 3;
    auto or0 = [three](uint8_t v) -> uint8_t { return three ? 0 : v; };
    auto if3 = [three](uint8_t v) -> uint8_t { return three ? v : 0; };
    const uint8_t stored[3][8] = {
        {s[4], s[5], s[6], s[7], s[8], s[9], or0(s[10]), or0(s[11])},
        {s[12], s[13], s[14], s[15], s[16], s[17], or0(s[18]), or0(s[19])},
        {s[20], s[21], if3(s[10]), if3(s[11]), if3(s[18]), if3(s[19]), 0, 0},
    };
    const Bc7Ends& r = ep_ref == 0                       ? zero.ends
                       : static_cast<size_t>(ep_ref) <= i ? history[(i - ep_ref) % kHistory].ends
                                                          : t.bc7_ends[ep_ref - i - 1];
    Value v{};
    v.ends.ctrl = ctrl;
    // Rows the reference lacks build on its first row.
    const uint32_t common = std::min(subsets, subsets_of(r.ctrl));
    const int columns = prec[3] > 0 ? 8 : 6;
    for (int c = 0; c < columns; ++c) {
      const uint32_t p = prec[c / 2];
      if (!p) continue;
      const uint16_t mask = static_cast<uint16_t>(0xFFFFu << (16 - p));
      for (uint32_t row = 0; row < subsets; ++row) {
        const uint8_t sv = stored[row][c];
        const uint16_t residual =
            static_cast<uint16_t>(((sv >> 1) ^ ((sv & 1) ? 0xFFFFu : 0u)) << (16 - p));
        const uint16_t base = r.rows[row < common ? row : 0][c] & mask;
        v.ends.rows[row][c] = static_cast<uint16_t>(base + residual);
      }
    }
    const Bc7Index& rx = ix_ref == 0                       ? zero.index
                         : static_cast<size_t>(ix_ref) <= i ? history[(i - ix_ref) % kHistory].index
                                                            : t.bc7_index[ix_ref - i - 1];
    v.index.bits[0] = kBc7Index[mode][0];
    v.index.bits[1] = kBc7Index[mode][1];
    for (int set = 0; set < 2; ++set) {
      if (!v.index.bits[set]) continue;
      for (int k = 0; k < 4; ++k) {
        for (int b = 0; b < 2; ++b) {
          int16_t pair[2];
          nibbles(s[kOffsets[set][k] + b], k < 3, pair);
          for (int h = 0; h < 2; ++h) {
            const int p = 4 * k + 2 * b + h;
            v.index.ix[set][p] = add_index(pair[h], v.index.bits[set], rx.ix[set][p], rx.bits[set]);
          }
        }
      }
    }
    history[i % kHistory] = v;
    bc7_pack(v.ends, v.index, prec, out + i * 16);
  }
  return true;
}

// --- BC6H (public mode layouts) ----------------------------------------------

// Bits of endpoint 0, then of the other endpoints' R, G, B, per mode 1-14;
// each mode's code and its width.
constexpr uint8_t kBc6Bits[15][4] = {{0, 0, 0, 0},    {10, 5, 5, 5}, {7, 6, 6, 6},  {11, 5, 4, 4}, {11, 4, 5, 4},
                                     {11, 4, 4, 5},   {9, 5, 5, 5},  {8, 6, 5, 5},  {8, 5, 6, 5},  {8, 5, 5, 6},
                                     {6, 6, 6, 6},    {10, 10, 10, 10}, {11, 9, 9, 9}, {12, 8, 8, 8}, {16, 4, 4, 4}};
constexpr uint8_t kBc6Code[15][2] = {{0, 0},  {0, 2},  {1, 2},  {2, 5},  {6, 5},  {10, 5}, {14, 5}, {18, 5},
                                     {22, 5}, {26, 5}, {30, 5}, {3, 5},  {7, 5},  {11, 5}, {15, 5}};

// A run of a mode's header: `len` bits of channel `ch` (0 R, 1 G, 2 B; 3 the
// partition, 5 bits) of endpoint `ep`, from bit `first` up.
struct Run {
  uint8_t ch, ep, first, len;
};
constexpr Run kPartition = {3, 0, 0, 5};
// The header after the mode bits, per mode (the public BC6H layouts; modes
// 13 and 14 with their high base bits in ascending order, as the console's
// packer writes them).
constexpr Run kMode1[] = {{1, 2, 4, 1}, {2, 2, 4, 1}, {2, 3, 4, 1}, {0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10},
                          {0, 1, 0, 5}, {1, 3, 4, 1}, {1, 2, 0, 4}, {1, 1, 0, 5},  {2, 3, 0, 1},  {1, 3, 0, 4},
                          {2, 1, 0, 5}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 5},  {2, 3, 2, 1},  {0, 3, 0, 5},
                          {2, 3, 3, 1}, kPartition};
constexpr Run kMode2[] = {{1, 2, 5, 1}, {1, 3, 4, 1}, {1, 3, 5, 1}, {0, 0, 0, 7}, {2, 3, 0, 1}, {2, 3, 1, 1},
                          {2, 2, 4, 1}, {1, 0, 0, 7}, {2, 2, 5, 1}, {2, 3, 2, 1}, {1, 2, 4, 1}, {2, 0, 0, 7},
                          {2, 3, 3, 1}, {2, 3, 5, 1}, {2, 3, 4, 1}, {0, 1, 0, 6}, {1, 2, 0, 4}, {1, 1, 0, 6},
                          {1, 3, 0, 4}, {2, 1, 0, 6}, {2, 2, 0, 4}, {0, 2, 0, 6}, {0, 3, 0, 6}, kPartition};
constexpr Run kMode3[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 5}, {0, 0, 10, 1},
                          {1, 2, 0, 4},  {1, 1, 0, 4},  {1, 0, 10, 1}, {2, 3, 0, 1}, {1, 3, 0, 4},
                          {2, 1, 0, 4},  {2, 0, 10, 1}, {2, 3, 1, 1},  {2, 2, 0, 4}, {0, 2, 0, 5},
                          {2, 3, 2, 1},  {0, 3, 0, 5},  {2, 3, 3, 1},  kPartition};
constexpr Run kMode4[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 4}, {0, 0, 10, 1}, {1, 3, 4, 1},
                          {1, 2, 0, 4},  {1, 1, 0, 5},  {1, 0, 10, 1}, {1, 3, 0, 4}, {2, 1, 0, 4},  {2, 0, 10, 1},
                          {2, 3, 1, 1},  {2, 2, 0, 4},  {0, 2, 0, 4},  {2, 3, 0, 1}, {2, 3, 2, 1},  {0, 3, 0, 4},
                          {1, 2, 4, 1},  {2, 3, 3, 1},  kPartition};
constexpr Run kMode5[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 4}, {0, 0, 10, 1}, {2, 2, 4, 1},
                          {1, 2, 0, 4},  {1, 1, 0, 4},  {1, 0, 10, 1}, {2, 3, 0, 1}, {1, 3, 0, 4},  {2, 1, 0, 5},
                          {2, 0, 10, 1}, {2, 2, 0, 4},  {0, 2, 0, 4},  {2, 3, 1, 1}, {2, 3, 2, 1},  {0, 3, 0, 4},
                          {2, 3, 4, 1},  {2, 3, 3, 1},  kPartition};
constexpr Run kMode6[] = {{0, 0, 0, 9}, {2, 2, 4, 1}, {1, 0, 0, 9}, {1, 2, 4, 1}, {2, 0, 0, 9}, {2, 3, 4, 1},
                          {0, 1, 0, 5}, {1, 3, 4, 1}, {1, 2, 0, 4}, {1, 1, 0, 5}, {2, 3, 0, 1}, {1, 3, 0, 4},
                          {2, 1, 0, 5}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 5}, {2, 3, 2, 1}, {0, 3, 0, 5},
                          {2, 3, 3, 1}, kPartition};
constexpr Run kMode7[] = {{0, 0, 0, 8}, {1, 3, 4, 1}, {2, 2, 4, 1}, {1, 0, 0, 8}, {2, 3, 2, 1}, {1, 2, 4, 1},
                          {2, 0, 0, 8}, {2, 3, 3, 1}, {2, 3, 4, 1}, {0, 1, 0, 6}, {1, 2, 0, 4}, {1, 1, 0, 5},
                          {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 6},
                          {0, 3, 0, 6}, kPartition};
constexpr Run kMode8[] = {{0, 0, 0, 8}, {2, 3, 0, 1}, {2, 2, 4, 1}, {1, 0, 0, 8}, {1, 2, 5, 1}, {1, 2, 4, 1},
                          {2, 0, 0, 8}, {1, 3, 5, 1}, {2, 3, 4, 1}, {0, 1, 0, 5}, {1, 3, 4, 1}, {1, 2, 0, 4},
                          {1, 1, 0, 6}, {1, 3, 0, 4}, {2, 1, 0, 5}, {2, 3, 1, 1}, {2, 2, 0, 4}, {0, 2, 0, 5},
                          {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, kPartition};
constexpr Run kMode9[] = {{0, 0, 0, 8}, {2, 3, 1, 1}, {2, 2, 4, 1}, {1, 0, 0, 8}, {2, 2, 5, 1}, {1, 2, 4, 1},
                          {2, 0, 0, 8}, {2, 3, 5, 1}, {2, 3, 4, 1}, {0, 1, 0, 5}, {1, 3, 4, 1}, {1, 2, 0, 4},
                          {1, 1, 0, 5}, {2, 3, 0, 1}, {1, 3, 0, 4}, {2, 1, 0, 6}, {2, 2, 0, 4}, {0, 2, 0, 5},
                          {2, 3, 2, 1}, {0, 3, 0, 5}, {2, 3, 3, 1}, kPartition};
constexpr Run kMode10[] = {{0, 0, 0, 6}, {1, 3, 4, 1}, {2, 3, 0, 1}, {2, 3, 1, 1}, {2, 2, 4, 1}, {1, 0, 0, 6},
                           {1, 2, 5, 1}, {2, 2, 5, 1}, {2, 3, 2, 1}, {1, 2, 4, 1}, {2, 0, 0, 6}, {1, 3, 5, 1},
                           {2, 3, 3, 1}, {2, 3, 5, 1}, {2, 3, 4, 1}, {0, 1, 0, 6}, {1, 2, 0, 4}, {1, 1, 0, 6},
                           {1, 3, 0, 4}, {2, 1, 0, 6}, {2, 2, 0, 4}, {0, 2, 0, 6}, {0, 3, 0, 6}, kPartition};
constexpr Run kMode11[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 10}, {1, 1, 0, 10}, {2, 1, 0, 10}};
constexpr Run kMode12[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 9}, {0, 0, 10, 1},
                           {1, 1, 0, 9},  {1, 0, 10, 1}, {2, 1, 0, 9},  {2, 0, 10, 1}};
constexpr Run kMode13[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 8}, {0, 0, 10, 2},
                           {1, 1, 0, 8},  {1, 0, 10, 2}, {2, 1, 0, 8},  {2, 0, 10, 2}};
constexpr Run kMode14[] = {{0, 0, 0, 10}, {1, 0, 0, 10}, {2, 0, 0, 10}, {0, 1, 0, 4}, {0, 0, 10, 6},
                           {1, 1, 0, 4},  {1, 0, 10, 6}, {2, 1, 0, 4},  {2, 0, 10, 6}};
struct Layout {
  const Run* runs;
  size_t count;
};
template <size_t N>
constexpr Layout layout_of(const Run (&runs)[N]) {
  return {runs, N};
}
constexpr Layout kBc6Layouts[15] = {
    {nullptr, 0},         layout_of(kMode1),  layout_of(kMode2),  layout_of(kMode3),  layout_of(kMode4),
    layout_of(kMode5),    layout_of(kMode6),  layout_of(kMode7),  layout_of(kMode8),  layout_of(kMode9),
    layout_of(kMode10),   layout_of(kMode11), layout_of(kMode12), layout_of(kMode13), layout_of(kMode14),
};

int bc6_index_bits(uint32_t mode) {
  return mode >= 1 && mode <= 10 ? 3 : mode >= 11 && mode <= 14 ? 4 : 0;
}

bool decode_bc6(const Tables& t, const uint8_t* records, size_t stride, size_t blocks, uint8_t* out,
                const char** why) {
  struct Value {
    Bc6Ends ends;
    Bc6Index index;
  };
  std::vector<Value> history(kHistory, Value{});
  Value zero{};
  zero.index.mode = 11;
  auto z = [](uint16_t v) { return static_cast<uint16_t>((v >> 1) ^ static_cast<uint16_t>(-(v & 1))); };
  auto wide = [](uint8_t a, uint8_t b) {
    uint16_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<uint16_t>((((a >> k) & 1) << (2 * k)) | (((b >> k) & 1) << (2 * k + 1)));
    return v;
  };
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t* s = records + i * stride;
    int ep_ref, ix_ref;
    references(s, true, &ep_ref, &ix_ref);
    const uint32_t mode = static_cast<uint8_t>(s[30] + 1);
    if (mode < 1 || mode > 14) {
      *why = "a BC6H mode out of range";
      return false;
    }
    const bool two = mode < 11;
    uint32_t shift[4];
    for (int k = 0; k < 4; ++k) shift[k] = 16 - kBc6Bits[mode][k];
    const uint16_t e0[3] = {z(wide(s[4], s[5])), z(wide(s[6], s[7])), z(wide(s[8], s[9]))};
    const uint16_t e1[3] = {z(wide(s[10], s[11])), z(wide(s[12], s[13])), z(wide(s[14], s[15]))};
    const uint16_t e2[3] = {z(s[16]), z(s[18]), z(s[20])};
    const uint16_t e3[3] = {z(s[17]), z(s[19]), z(s[21])};
    const Bc6Ends& r = ep_ref == 0                       ? zero.ends
                       : static_cast<size_t>(ep_ref) <= i ? history[(i - ep_ref) % kHistory].ends
                                                          : t.bc6_ends[ep_ref - i - 1];
    Value v{};
    // Endpoint 0 at the base precision, the others per channel; the sum is
    // cut to the precision.
    auto mask = [](uint32_t sh) { return static_cast<uint16_t>(0xFFFFu << sh); };
    for (int c = 0; c < 3; ++c) {
      v.ends.e[0][c] = static_cast<uint16_t>((r.e[0][c] + (e0[c] << shift[0])) & mask(shift[0]));
    }
    const uint16_t* residuals[3] = {e1, e2, e3};
    for (int k = 0; k < (two ? 3 : 1); ++k) {
      for (int c = 0; c < 3; ++c) {
        v.ends.e[k + 1][c] =
            static_cast<uint16_t>((r.e[k + 1][c] + (residuals[k][c] << shift[1 + c])) & mask(shift[1 + c]));
      }
    }
    if (!two) {
      for (int c = 0; c < 3; ++c) v.ends.e[2][c] = static_cast<uint16_t>((v.ends.e[0][c] >> shift[0]) << shift[1]);
      for (int c = 0; c < 3; ++c) v.ends.e[3][c] = v.ends.e[1][c];
    }
    const Bc6Index& rx = ix_ref == 0                       ? zero.index
                         : static_cast<size_t>(ix_ref) <= i ? history[(i - ix_ref) % kHistory].index
                                                            : t.bc6_index[ix_ref - i - 1];
    const int bits = bc6_index_bits(mode);
    v.index.mode = static_cast<uint8_t>(mode);
    for (int k = 0; k < 8; ++k) {
      int16_t pair[2];
      nibbles(s[22 + k], k < 6, pair);
      for (int h = 0; h < 2; ++h) {
        const int p = 2 * k + h;
        v.index.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
      }
    }
    history[i % kHistory] = v;
    // Fields: the working value with its top bit flipped.
    auto field = [&](uint32_t k, uint32_t c) -> uint64_t {
      const uint32_t sh = k == 0 ? shift[0] : shift[1 + c];
      return static_cast<uint16_t>(v.ends.e[k][c] ^ 0x8000) >> sh;
    };
    Bits w;
    w.put(kBc6Code[mode][0], kBc6Code[mode][1]);
    const uint32_t part = s[31] & 31;
    const Layout& layout = kBc6Layouts[mode];
    for (size_t n = 0; n < layout.count; ++n) {
      const Run& run = layout.runs[n];
      if (run.ch == 3) {
        w.put(part, 5);
      } else {
        w.put(field(run.ep, run.ch) >> run.first, run.len);
      }
    }
    const uint32_t anchor = two ? kAnchor2[part] : 0;
    for (int p = 0; p < 16; ++p) {
      const bool short_index = p == 0 || static_cast<uint32_t>(p) == anchor;
      const auto value = static_cast<uint16_t>(static_cast<uint16_t>(v.index.ix[p]) + (1u << (bits - 1)));
      w.put(value, bits - (short_index ? 1 : 0));
    }
    w.store(out + i * 16);
  }
  return true;
}

// The lanes of a block format: symbol widths (bits - 1), symbols per lane
// per block, bytes per block. False for formats the codec does not have.
bool lanes_of(uint32_t bc, const uint8_t** widths, int* lanes, int* per, size_t* block) {
  static const uint8_t kBc1Lanes[12] = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
  static const uint8_t kBc3Lanes[12] = {3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
  static const uint8_t kBc4Lanes[12] = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
  static const uint8_t kBc5Lanes[12] = {3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
  static const uint8_t kBc6Lanes[16] = {3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
  static const uint8_t kBc7Lanes[16] = {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};
  switch (bc) {
    case 1: *widths = kBc1Lanes; break;
    case 3: *widths = kBc3Lanes; break;
    case 4: *widths = kBc4Lanes; break;
    case 5: *widths = kBc5Lanes; break;
    case 6: *widths = kBc6Lanes; break;
    case 7: *widths = kBc7Lanes; break;
    default: return false;
  }
  *lanes = bc >= 6 ? 16 : 12;
  *per = bc == 1 || bc == 4 ? 1 : 2;
  *block = bc == 1 || bc == 4 ? 8 : 16;
  return true;
}

}  // namespace

bool schedule(const Tables& t, uint32_t bc, size_t blocks, const std::vector<std::vector<uint8_t>>& lane_bytes,
              std::vector<uint8_t>* stream) {
  const uint8_t* widths;
  int lanes, per;
  size_t block;
  if (!lanes_of(bc, &widths, &lanes, &per, &block) || lane_bytes.size() != static_cast<size_t>(lanes)) return false;
  std::vector<size_t> next(lane_bytes.size(), 0);
  std::vector<uint8_t> records(blocks * lanes * per);
  stream->clear();
  return run_lanes(
      t, widths, lanes, per, blocks,
      [&](int j, uint8_t* byte) {
        if (next[j] >= lane_bytes[j].size()) return false;
        *byte = lane_bytes[j][next[j]++];
        stream->push_back(*byte);
        return true;
      },
      records.data());
}

bool decode(const Tables& t, uint32_t bc, const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size,
            const char** why) {
  const char* reason = nullptr;
  if (!why) why = &reason;
  const uint8_t* widths;
  int lanes, per;
  size_t block;
  if (!lanes_of(bc, &widths, &lanes, &per, &block)) {
    *why = "a block format the codec does not have";
    return false;
  }
  if (!out || out_size == 0 || out_size % block != 0) {
    *why = "a destination that is not whole blocks";
    return false;
  }
  const size_t blocks = out_size / block, stride = static_cast<size_t>(lanes) * per;
  thread_local std::vector<uint8_t> records;
  records.assign(blocks * stride, 0);
  size_t next = 0;
  const auto fetch = [&](int, uint8_t* byte) {
    if (next >= in_size) return false;
    *byte = in[next++];
    return true;
  };
  if (!run_lanes(t, widths, lanes, per, blocks, fetch, records.data())) {
    *why = "the stream ends early or is corrupt";
    return false;
  }
  if (bc <= 5) {
    decode_plain(t, bc, records.data(), stride, blocks, out, block);
    return true;
  }
  return bc == 6 ? decode_bc6(t, records.data(), stride, blocks, out, why)
                 : decode_bc7(t, records.data(), stride, blocks, out, why);
}

}  // namespace holy::lib::codec
