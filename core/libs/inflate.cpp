// DEFLATE decoder following RFC 1951: stored, fixed-Huffman and
// dynamic-Huffman blocks. Codes of up to kFastBits bits are decoded with one
// table lookup (the next kFastBits input bits index a table of symbol and
// length); longer codes fall back to canonical decoding one bit at a time
// from per-length code counts. Input is read through a 64-bit bit buffer;
// output goes straight into the caller's buffer.
#include "core/libs/inflate.h"

#include <cstring>

namespace holy::lib {
namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10;
constexpr uint32_t kFastMask = (1u << kFastBits) - 1;

enum class Result { Ok, Malformed, Full };

struct Table {
  uint16_t fast[1 << kFastBits];      // (symbol << 4) | length; 0: a longer code
  uint16_t count[kMaxBits + 1] = {};  // codes of each length
  uint16_t symbol[320] = {};          // symbols ordered by code
};

// Builds a table from code lengths; false if over-subscribed.
bool build(Table* t, const uint8_t* lengths, int n) {
  std::memset(t->fast, 0, sizeof(t->fast));
  for (int i = 0; i <= kMaxBits; ++i) t->count[i] = 0;
  for (int i = 0; i < n; ++i) t->count[lengths[i]]++;
  if (t->count[0] == n) return true;  // no codes: decoding fails if used
  int left = 1;
  for (int i = 1; i <= kMaxBits; ++i) {
    left <<= 1;
    left -= t->count[i];
    if (left < 0) return false;
  }
  uint16_t offsets[kMaxBits + 1];
  offsets[1] = 0;
  for (int i = 1; i < kMaxBits; ++i) offsets[i + 1] = static_cast<uint16_t>(offsets[i] + t->count[i]);
  for (int i = 0; i < n; ++i) {
    if (lengths[i] != 0) t->symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
  }
  // Canonical codes (first code of each length), bit-reversed into the fast
  // table: DEFLATE sends Huffman codes most significant bit first.
  uint32_t next[kMaxBits + 1] = {};
  uint32_t code = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    next[len] = code;
    code = (code + t->count[len]) << 1;
  }
  for (int i = 0; i < n; ++i) {
    const int len = lengths[i];
    if (len == 0) continue;
    const uint32_t c = next[len]++;
    if (len > kFastBits) continue;
    uint32_t reversed = 0;
    for (int b = 0; b < len; ++b) reversed |= ((c >> b) & 1u) << (len - 1 - b);
    for (uint32_t at = reversed; at < (1u << kFastBits); at += 1u << len) {
      t->fast[at] = static_cast<uint16_t>((i << 4) | len);
    }
  }
  return true;
}

class Inflater {
 public:
  Inflater(const uint8_t* data, size_t size, uint8_t* out, size_t capacity)
      : in_(data), size_(size), out_(out), capacity_(capacity) {}

  Result run() {
    bool last = false;
    while (!last) {
      refill();
      last = take(1) != 0;
      const uint32_t type = take(2);
      Result r = Result::Malformed;
      if (type == 0) r = stored();
      else if (type == 1) r = fixed();
      else if (type == 2) r = dynamic();
      if (r != Result::Ok) return r;
    }
    return end() > size_ ? Result::Malformed : Result::Ok;
  }
  // The input position after the last block (whole bytes).
  size_t end() const { return position_ - static_cast<size_t>(count_ >> 3); }
  size_t produced() const { return written_; }

 private:
  // At least 56 bits in the buffer; input past the end reads as zero (end()
  // tells whether any of it was used).
  void refill() {
    if (position_ + 8 <= size_) {
      uint64_t word;
      std::memcpy(&word, in_ + position_, 8);
      buffer_ |= word << count_;
      position_ += static_cast<size_t>((63 - count_) >> 3);
      count_ |= 56;
      return;
    }
    while (count_ <= 56) {
      const uint64_t byte = position_ < size_ ? in_[position_] : 0;
      ++position_;
      buffer_ |= byte << count_;
      count_ += 8;
    }
  }
  uint32_t take(int n) {
    const uint32_t v = static_cast<uint32_t>(buffer_ & ((1ull << n) - 1));
    buffer_ >>= n;
    count_ -= n;
    return v;
  }

  // A symbol, or -1. Needs 15 bits in the buffer.
  int decode(const Table& t) {
    const uint16_t e = t.fast[buffer_ & kFastMask];
    if (e) {
      take(e & 15);
      return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int length = 1; length <= kMaxBits; ++length) {
      code |= static_cast<int>(take(1));
      const int count = t.count[length];
      if (code - count < first) return t.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }

  Result stored() {
    // Skip to the byte boundary, then read LEN and NLEN from the bit buffer.
    take(count_ & 7);
    refill();
    const uint32_t length = take(16), complement = take(16);
    if (length != (~complement & 0xFFFFu)) return Result::Malformed;
    // Bytes still in the bit buffer come first.
    uint32_t left = length;
    while (left && count_ >= 8) {
      if (written_ >= capacity_) return Result::Full;
      out_[written_++] = static_cast<uint8_t>(take(8));
      --left;
    }
    if (count_ != 0 && left) return Result::Malformed;
    // The buffer is empty here (count_ is 0 or the block is done).
    if (left) {
      const size_t at = end();
      if (at + left > size_) return Result::Malformed;
      if (written_ + left > capacity_) return Result::Full;
      std::memcpy(out_ + written_, in_ + at, left);
      written_ += left;
      position_ = at + left;
      buffer_ = 0;
      count_ = 0;
    }
    return Result::Ok;
  }

  Result codes(const Table& lengths, const Table& distances) {
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
      // 56 bits cover a length code with its extra bits and a distance code
      // with its extra bits (15 + 5 + 15 + 13).
      refill();
      int symbol = decode(lengths);
      if (symbol < 0) return Result::Malformed;
      if (symbol < 256) {
        if (written_ >= capacity_) return Result::Full;
        out_[written_++] = static_cast<uint8_t>(symbol);
        continue;
      }
      if (symbol == 256) return Result::Ok;
      symbol -= 257;
      if (symbol >= 29) return Result::Malformed;
      const size_t length = kLengthBase[symbol] + take(kLengthExtra[symbol]);
      const int dsym = decode(distances);
      if (dsym < 0 || dsym >= 30) return Result::Malformed;
      const size_t distance = kDistanceBase[dsym] + take(kDistanceExtra[dsym]);
      if (distance > written_) return Result::Malformed;
      if (written_ + length > capacity_) return Result::Full;
      uint8_t* to = out_ + written_;
      const uint8_t* from = to - distance;
      if (distance >= length) {
        std::memcpy(to, from, length);
      } else if (distance == 1) {
        std::memset(to, *from, length);
      } else {
        for (size_t i = 0; i < length; ++i) to[i] = from[i];
      }
      written_ += length;
    }
  }

  Result fixed() {
    static Table literal, distance;
    static const bool built = [] {
      uint8_t lengths[288 + 30];
      int i = 0;
      for (; i < 144; ++i) lengths[i] = 8;
      for (; i < 256; ++i) lengths[i] = 9;
      for (; i < 280; ++i) lengths[i] = 7;
      for (; i < 288; ++i) lengths[i] = 8;
      for (; i < 288 + 30; ++i) lengths[i] = 5;
      build(&literal, lengths, 288);
      build(&distance, lengths + 288, 30);
      return true;
    }();
    (void)built;
    return codes(literal, distance);
  }

  Result dynamic() {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    refill();
    const int nlen = static_cast<int>(take(5)) + 257;
    const int ndist = static_cast<int>(take(5)) + 1;
    const int ncode = static_cast<int>(take(4)) + 4;
    if (nlen > 286 || ndist > 30) return Result::Malformed;
    uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) {
      refill();
      lengths[kOrder[i]] = static_cast<uint8_t>(take(3));
    }
    Table code_lengths;
    if (!build(&code_lengths, lengths, 19)) return Result::Malformed;
    std::memset(lengths, 0, sizeof(lengths));
    int index = 0;
    while (index < nlen + ndist) {
      refill();
      const int symbol = decode(code_lengths);
      if (symbol < 0) return Result::Malformed;
      if (symbol < 16) {
        lengths[index++] = static_cast<uint8_t>(symbol);
        continue;
      }
      uint8_t value = 0;
      int repeat;
      if (symbol == 16) {
        if (index == 0) return Result::Malformed;
        value = lengths[index - 1];
        repeat = 3 + static_cast<int>(take(2));
      } else if (symbol == 17) {
        repeat = 3 + static_cast<int>(take(3));
      } else {
        repeat = 11 + static_cast<int>(take(7));
      }
      if (index + repeat > nlen + ndist) return Result::Malformed;
      while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return Result::Malformed;  // no end-of-block code
    Table literal, distance;
    if (!build(&literal, lengths, nlen) || !build(&distance, lengths + nlen, ndist)) return Result::Malformed;
    return codes(literal, distance);
  }

  const uint8_t* in_;
  size_t size_;
  uint8_t* out_;
  size_t capacity_;
  size_t position_ = 0;
  uint64_t buffer_ = 0;
  int count_ = 0;
  size_t written_ = 0;
};

uint32_t adler32(const uint8_t* data, size_t size) {
  uint32_t a = 1, b = 0;
  while (size) {
    // 5552 bytes keep the sums below 2^32 before the reduction.
    const size_t chunk = size < 5552 ? size : 5552;
    for (size_t i = 0; i < chunk; ++i) {
      a += data[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
    data += chunk;
    size -= chunk;
  }
  return (b << 16) | a;
}

Result inflate_zlib(const uint8_t* data, size_t size, uint8_t* out, size_t capacity, size_t* produced) {
  *produced = 0;
  if (size < 6) return Result::Malformed;
  const unsigned cmf = data[0], flg = data[1];
  if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0) return Result::Malformed;
  Inflater inflater(data + 2, size - 2, out, capacity);
  const Result r = inflater.run();
  if (r != Result::Ok) return r;
  const size_t end = 2 + inflater.end();
  if (end + 4 > size) return Result::Malformed;
  const uint32_t expected = (static_cast<uint32_t>(data[end]) << 24) | (static_cast<uint32_t>(data[end + 1]) << 16) |
                            (static_cast<uint32_t>(data[end + 2]) << 8) | data[end + 3];
  if (adler32(out, inflater.produced()) != expected) return Result::Malformed;
  *produced = inflater.produced();
  return Result::Ok;
}

}  // namespace

bool zlib_inflate_into(const uint8_t* data, size_t size, uint8_t* out, size_t capacity, size_t* produced) {
  return inflate_zlib(data, size, out, capacity, produced) == Result::Ok;
}

bool zlib_inflate(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t limit) {
  size_t capacity = size * 4 < 65536 ? 65536 : size * 4;
  if (capacity > limit) capacity = limit;
  for (;;) {
    out->resize(capacity);
    size_t produced = 0;
    const Result r = inflate_zlib(data, size, out->data(), capacity, &produced);
    if (r == Result::Ok) {
      out->resize(produced);
      return true;
    }
    if (r == Result::Malformed || capacity >= limit) {
      out->clear();
      return false;
    }
    capacity = capacity * 2 > limit ? limit : capacity * 2;
  }
}

}  // namespace holy::lib
