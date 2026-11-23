// DEFLATE decoder following RFC 1951: stored, fixed-Huffman and
// dynamic-Huffman blocks, canonical Huffman codes decoded one bit at a time
// from per-length code counts.
#include "core/libs/d3d12_xs/shader/inflate.h"

namespace holy::d3d12 {
namespace {

constexpr int kMaxBits = 15;

struct Huffman {
  uint16_t count[kMaxBits + 1] = {};  // codes of each length
  uint16_t symbol[320] = {};          // symbols ordered by code
};

class Inflater {
 public:
  Inflater(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t limit)
      : data_(data), size_(size), out_(out), limit_(limit) {}

  bool run() {
    bool last = false;
    while (!last) {
      int value;
      if (!bits(1, &value)) return false;
      last = value != 0;
      int type;
      if (!bits(2, &type)) return false;
      bool ok = false;
      if (type == 0) ok = stored();
      else if (type == 1) ok = fixed();
      else if (type == 2) ok = dynamic();
      if (!ok) return false;
    }
    return true;
  }
  size_t position() const { return position_; }

 private:
  bool bits(int need, int* value) {
    uint32_t v = buffer_;
    while (count_ < need) {
      if (position_ >= size_) return false;
      v |= static_cast<uint32_t>(data_[position_++]) << count_;
      count_ += 8;
    }
    buffer_ = v >> need;
    count_ -= need;
    *value = static_cast<int>(v & ((1u << need) - 1));
    return true;
  }

  bool emit(uint8_t byte) {
    if (out_->size() >= limit_) return false;
    out_->push_back(byte);
    return true;
  }

  bool stored() {
    buffer_ = 0;
    count_ = 0;
    if (position_ + 4 > size_) return false;
    const unsigned length = data_[position_] | (data_[position_ + 1] << 8);
    const unsigned complement = data_[position_ + 2] | (data_[position_ + 3] << 8);
    position_ += 4;
    if (length != (~complement & 0xFFFFu) || position_ + length > size_) return false;
    for (unsigned i = 0; i < length; ++i) {
      if (!emit(data_[position_++])) return false;
    }
    return true;
  }

  bool decode(const Huffman& h, int* symbol) {
    int code = 0, first = 0, index = 0;
    for (int length = 1; length <= kMaxBits; ++length) {
      int bit;
      if (!bits(1, &bit)) return false;
      code |= bit;
      const int count = h.count[length];
      if (code - count < first) {
        *symbol = h.symbol[index + (code - first)];
        return true;
      }
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return false;
  }

  // Builds a table from code lengths; false if over-subscribed.
  static bool build(Huffman* h, const uint8_t* lengths, int n) {
    for (int i = 0; i <= kMaxBits; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
    if (h->count[0] == n) return true;  // no codes: decoding will fail if used
    int left = 1;
    for (int i = 1; i <= kMaxBits; ++i) {
      left <<= 1;
      left -= h->count[i];
      if (left < 0) return false;
    }
    uint16_t offsets[kMaxBits + 1];
    offsets[1] = 0;
    for (int i = 1; i < kMaxBits; ++i) offsets[i + 1] = static_cast<uint16_t>(offsets[i] + h->count[i]);
    for (int i = 0; i < n; ++i) {
      if (lengths[i] != 0) h->symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    return true;
  }

  bool codes(const Huffman& lengths, const Huffman& distances) {
    static const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                               33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int symbol;
      if (!decode(lengths, &symbol)) return false;
      if (symbol < 256) {
        if (!emit(static_cast<uint8_t>(symbol))) return false;
        continue;
      }
      if (symbol == 256) return true;
      symbol -= 257;
      if (symbol >= 29) return false;
      int extra;
      if (!bits(kLengthExtra[symbol], &extra)) return false;
      const size_t length = kLengthBase[symbol] + static_cast<size_t>(extra);
      int dsym;
      if (!decode(distances, &dsym) || dsym >= 30) return false;
      if (!bits(kDistanceExtra[dsym], &extra)) return false;
      const size_t distance = kDistanceBase[dsym] + static_cast<size_t>(extra);
      if (distance > out_->size()) return false;
      for (size_t i = 0; i < length; ++i) {
        if (!emit((*out_)[out_->size() - distance])) return false;
      }
    }
  }

  bool fixed() {
    uint8_t lengths[288 + 30];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    for (; i < 288 + 30; ++i) lengths[i] = 5;
    Huffman literal, distance;
    build(&literal, lengths, 288);
    build(&distance, lengths + 288, 30);
    return codes(literal, distance);
  }

  bool dynamic() {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen, ndist, ncode;
    if (!bits(5, &nlen) || !bits(5, &ndist) || !bits(4, &ncode)) return false;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) {
      int v;
      if (!bits(3, &v)) return false;
      lengths[kOrder[i]] = static_cast<uint8_t>(v);
    }
    Huffman code_lengths;
    if (!build(&code_lengths, lengths, 19)) return false;
    int index = 0;
    while (index < nlen + ndist) {
      int symbol;
      if (!decode(code_lengths, &symbol)) return false;
      if (symbol < 16) {
        lengths[index++] = static_cast<uint8_t>(symbol);
        continue;
      }
      uint8_t value = 0;
      int repeat;
      if (symbol == 16) {
        if (index == 0) return false;
        value = lengths[index - 1];
        if (!bits(2, &repeat)) return false;
        repeat += 3;
      } else if (symbol == 17) {
        if (!bits(3, &repeat)) return false;
        repeat += 3;
      } else {
        if (!bits(7, &repeat)) return false;
        repeat += 11;
      }
      if (index + repeat > nlen + ndist) return false;
      while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;  // no end-of-block code
    Huffman literal, distance;
    if (!build(&literal, lengths, nlen) || !build(&distance, lengths + nlen, ndist)) return false;
    return codes(literal, distance);
  }

  const uint8_t* data_;
  size_t size_;
  std::vector<uint8_t>* out_;
  size_t limit_;
  size_t position_ = 0;
  uint32_t buffer_ = 0;
  int count_ = 0;
};

}  // namespace

bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t limit) {
  out->clear();
  if (size < 6) return false;
  const unsigned cmf = data[0], flg = data[1];
  if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0) return false;
  Inflater inflater(data + 2, size - 2, out, limit);
  if (!inflater.run()) return false;
  const size_t end = 2 + inflater.position();
  if (end + 4 > size) return false;
  uint32_t a = 1, b = 0;
  for (uint8_t byte : *out) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t expected = (static_cast<uint32_t>(data[end]) << 24) | (static_cast<uint32_t>(data[end + 1]) << 16) |
                            (static_cast<uint32_t>(data[end + 2]) << 8) | data[end + 3];
  return ((b << 16) | a) == expected;
}

}  // namespace holy::d3d12
