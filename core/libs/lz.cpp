#include "core/libs/lz.h"

#include <cstring>

namespace holy::lib {
namespace {

constexpr size_t kMinMatch = 4;
constexpr size_t kMaxDistance = 0xFFFF;
constexpr int kHashBits = 16;
constexpr uint32_t kEmpty = 0xFFFFFFFFu;
// Earlier positions with the same hash tried for the longest match: 8 gave
// 8.7 times on translated programs at 444 MB/s (1: 5.0 times at 821 MB/s,
// 16: 9.3 times at 344 MB/s; measured).
constexpr int kChainDepth = 8;

void put_count(std::vector<uint8_t>& out, size_t n) {
  for (; n >= 255; n -= 255) out.push_back(255);
  out.push_back(static_cast<uint8_t>(n));
}

// Reads a count continued in bytes of 255; false past the end.
bool read_count(const uint8_t* data, size_t size, size_t* at, size_t* count) {
  uint8_t byte;
  do {
    if (*at >= size) return false;
    byte = data[(*at)++];
    *count += byte;
  } while (byte == 255);
  return true;
}

}  // namespace

std::vector<uint8_t> lz_compress(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size / 3 + 16);
  std::vector<uint32_t> table(size_t{1} << kHashBits, kEmpty);
  std::vector<uint32_t> previous(kMaxDistance + 1, kEmpty);  // the position before, same hash (by position % 64 Ki)
  auto hash = [&](size_t i) {
    uint32_t v;
    std::memcpy(&v, data + i, 4);
    return (v * 2654435761u) >> (32 - kHashBits);
  };
  auto insert = [&](size_t i) {
    const uint32_t h = hash(i);
    previous[i & kMaxDistance] = table[h];
    table[h] = static_cast<uint32_t>(i);
  };
  auto emit = [&](size_t anchor, size_t end, size_t distance, size_t length) {
    const size_t literals = end - anchor;
    const size_t extra = length ? length - kMinMatch : 0;
    out.push_back(static_cast<uint8_t>(((literals < 15 ? literals : 15) << 4) | (extra < 15 ? extra : 15)));
    if (literals >= 15) put_count(out, literals - 15);
    out.insert(out.end(), data + anchor, data + end);
    if (!length) return;
    out.push_back(static_cast<uint8_t>(distance));
    out.push_back(static_cast<uint8_t>(distance >> 8));
    if (extra >= 15) put_count(out, extra - 15);
  };
  size_t anchor = 0, i = 0;
  // Matches stop short of the end, so the last run always has literals.
  const size_t limit = size > 12 ? size - 12 : 0;
  while (i < limit) {
    size_t best = 0, from = 0;
    uint32_t candidate = table[hash(i)];
    for (int d = 0; d < kChainDepth && candidate != kEmpty && i - candidate <= kMaxDistance; ++d) {
      if (std::memcmp(data + candidate, data + i, 4) == 0) {
        size_t length = kMinMatch;
        while (i + length < size - 5 && data[candidate + length] == data[i + length]) ++length;
        if (length > best) best = length, from = candidate;
      }
      const uint32_t earlier = previous[candidate & kMaxDistance];
      if (earlier == kEmpty || earlier >= candidate) break;  // the slot was reused by a later position
      candidate = earlier;
    }
    insert(i);
    if (best) {
      emit(anchor, i, i - from, best);
      for (size_t k = i + 1; k < i + best && k < limit; k += 2) insert(k);
      i += best;
      anchor = i;
    } else {
      ++i;
    }
  }
  emit(anchor, size, 0, 0);
  return out;
}

bool lz_decompress(const uint8_t* data, size_t size, uint8_t* out, size_t capacity) {
  size_t i = 0, o = 0;
  while (i < size) {
    const uint8_t token = data[i++];
    size_t literals = token >> 4;
    if (literals == 15 && !read_count(data, size, &i, &literals)) return false;
    if (literals > size - i || literals > capacity - o) return false;
    std::memcpy(out + o, data + i, literals);
    i += literals;
    o += literals;
    if (i == size) return o == capacity;  // the last run: literals only
    if (size - i < 2) return false;
    const size_t distance = data[i] | (static_cast<size_t>(data[i + 1]) << 8);
    i += 2;
    size_t length = (token & 15) + kMinMatch;
    if ((token & 15) == 15 && !read_count(data, size, &i, &length)) return false;
    if (distance == 0 || distance > o || length > capacity - o) return false;
    const uint8_t* from = out + o - distance;
    if (distance >= length) {
      std::memcpy(out + o, from, length);
    } else {
      for (size_t k = 0; k < length; ++k) out[o + k] = from[k];  // overlapping: repeats the last bytes
    }
    o += length;
  }
  return o == capacity;
}

}  // namespace holy::lib
