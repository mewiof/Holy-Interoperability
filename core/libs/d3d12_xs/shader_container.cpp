// Console shader containers.
//
// Observed: console shaders are DXBC-style containers (the documented desktop
// container layout: "DXBC", a 16-byte digest, version, total size, part count
// and part offsets) whose DXIL part does not hold a desktop DXIL program
// directly but a console block ("XDAT") with the console GPU's native code and,
// inside it, a DXIL program header followed by LLVM bitcode. Some shaders keep
// their bitcode; others carry a 4-byte "NOBC" marker instead. Embedded root
// signatures (RTS0) may set flag bits above the desktop's.
//
// For shaders that keep their bitcode, a desktop container is rebuilt: the
// desktop parts are kept, the DXIL part becomes the plain program, RTS0 flags
// are masked to the desktop's, and the container digest is recomputed. The
// digest is the documented DXBC checksum: MD5 over everything after the digest
// field with a modified final padding; this implementation reproduces the
// digests of console containers byte for byte.
#include "core/libs/d3d12_xs/shader_container.h"

#include <cstdio>

#include <algorithm>
#include <cstring>

#include "core/libs/inflate.h"

namespace holy::d3d12 {
namespace {

uint32_t read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

void write32(std::vector<uint8_t>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

uint32_t rotl(uint32_t x, int c) {
  return (x << c) | (x >> (32 - c));
}

void md5_transform(uint32_t state[4], const uint8_t block[64]) {
  static const int kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  static const uint32_t kTable[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  uint32_t m[16];
  std::memcpy(m, block, 64);
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    f = f + a + kTable[i] + m[g];
    a = d;
    d = c;
    c = b;
    b = b + rotl(f, kShift[i]);
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

}  // namespace

void dxbc_checksum(const uint8_t* container, size_t size, uint8_t digest[16]) {
  const uint8_t* data = container + 20;
  const size_t length = size - 20;
  const uint32_t bits = static_cast<uint32_t>(length * 8);
  uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  const size_t full = length - length % 64;
  for (size_t i = 0; i < full; i += 64) md5_transform(state, data + i);
  const size_t left = length - full;
  uint8_t block[64] = {};
  const uint32_t tail = (bits >> 2) | 1;
  if (left >= 56) {
    std::memcpy(block, data + full, left);
    block[left] = 0x80;
    md5_transform(state, block);
    std::memset(block, 0, sizeof(block));
    std::memcpy(block, &bits, 4);
    std::memcpy(block + 60, &tail, 4);
    md5_transform(state, block);
  } else {
    std::memcpy(block, &bits, 4);
    std::memcpy(block + 4, data + full, left);
    block[4 + left] = 0x80;
    std::memcpy(block + 60, &tail, 4);
    md5_transform(state, block);
  }
  std::memcpy(digest, state, 16);
}

ShaderFormat rebuild_desktop_container(const void* blob, size_t size, std::vector<uint8_t>& out) {
  const auto* bytes = static_cast<const uint8_t*>(blob);
  if (!bytes || size < 32 || std::memcmp(bytes, "DXBC", 4) != 0) return ShaderFormat::Unknown;
  const uint32_t count = read32(bytes + 28);
  if (count > 64 || 32 + 4ull * count > size) return ShaderFormat::Unknown;

  struct Part {
    char tag[4];
    const uint8_t* data;
    uint32_t size;
  };
  std::vector<Part> keep;
  const uint8_t* dxil = nullptr;
  uint32_t dxil_size = 0;
  bool bytecode = false;  // a desktop SM5 program part
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t offset = read32(bytes + 32 + 4 * i);
    if (offset + 8ull > size) return ShaderFormat::Unknown;
    Part part;
    std::memcpy(part.tag, bytes + offset, 4);
    part.size = read32(bytes + offset + 4);
    part.data = bytes + offset + 8;
    if (offset + 8ull + part.size > size) return ShaderFormat::Unknown;
    if (!std::memcmp(part.tag, "SHEX", 4) || !std::memcmp(part.tag, "SHDR", 4)) bytecode = true;
    if (std::memcmp(part.tag, "DXIL", 4) == 0) {
      dxil = part.data;
      dxil_size = part.size;
    } else if (!std::memcmp(part.tag, "SFI0", 4) || !std::memcmp(part.tag, "ISG1", 4) ||
               !std::memcmp(part.tag, "OSG1", 4) || !std::memcmp(part.tag, "PSG1", 4) ||
               !std::memcmp(part.tag, "PSV0", 4) || !std::memcmp(part.tag, "RTS0", 4) ||
               !std::memcmp(part.tag, "ILDN", 4) || !std::memcmp(part.tag, "HASH", 4)) {
      keep.push_back(part);
    }
  }
  if (!dxil && bytecode) {
    out.assign(bytes, bytes + size);
    return ShaderFormat::Desktop;
  }
  if (!dxil || dxil_size < 24) return ShaderFormat::Unknown;
  if (std::memcmp(dxil, "XDAT", 4) != 0) {
    // A desktop program header. Console containers keep their block between
    // the header and the program, which the header's bitcode offset skips;
    // the program can also be just the "NOBC" marker.
    if (std::memcmp(dxil + 8, "DXIL", 4) == 0) {
      const uint64_t at = 8ull + read32(dxil + 16), length = read32(dxil + 20);
      if (at + length > dxil_size || length < 4 || std::memcmp(dxil + at, "BC\xC0\xDE", 4) != 0) {
        return ShaderFormat::NativeOnly;
      }
    }
    out.assign(bytes, bytes + size);
    return ShaderFormat::Desktop;
  }
  // Find the program header inside the console block: version, size, "DXIL".
  const uint8_t* header = nullptr;
  for (uint32_t at = 8; at + 24 <= dxil_size; at += 4) {
    if (std::memcmp(dxil + at, "DXIL", 4) == 0) {
      header = dxil + at - 8;
      break;
    }
  }
  if (!header) return ShaderFormat::NativeOnly;
  const uint32_t program_version = read32(header);
  const uint32_t dxil_version = read32(header + 12);
  const uint32_t bitcode_offset = read32(header + 16);
  const uint32_t bitcode_size = read32(header + 20);
  const uint8_t* bitcode = header + 8 + bitcode_offset;
  if (bitcode_size < 8 || bitcode + bitcode_size > dxil + dxil_size || std::memcmp(bitcode, "BC\xC0\xDE", 4) != 0) {
    return ShaderFormat::NativeOnly;
  }

  std::vector<uint8_t> program;
  write32(program, program_version);
  write32(program, static_cast<uint32_t>((24 + bitcode_size + 3) / 4));
  program.insert(program.end(), {'D', 'X', 'I', 'L'});
  write32(program, dxil_version);
  write32(program, 16);
  write32(program, bitcode_size);
  program.insert(program.end(), bitcode, bitcode + bitcode_size);
  while (program.size() % 4) program.push_back(0);

  const uint32_t parts = static_cast<uint32_t>(keep.size() + 1);
  const uint32_t header_size = 32 + 4 * parts;
  out.clear();
  out.insert(out.end(), {'D', 'X', 'B', 'C'});
  out.resize(20, 0);
  out.insert(out.end(), {1, 0, 0, 0});  // version 1.0
  write32(out, 0);                      // total size, patched below
  write32(out, parts);
  uint32_t offset = header_size;
  for (const Part& part : keep) {
    write32(out, offset);
    offset += 8 + part.size;
  }
  write32(out, offset);
  for (const Part& part : keep) {
    out.insert(out.end(), part.tag, part.tag + 4);
    write32(out, part.size);
    const size_t start = out.size();
    out.insert(out.end(), part.data, part.data + part.size);
    if (std::memcmp(part.tag, "RTS0", 4) == 0 && part.size >= 24) {
      uint32_t flags = read32(out.data() + start + 20);
      flags &= 0xFFF;
      std::memcpy(out.data() + start + 20, &flags, 4);
    }
  }
  out.insert(out.end(), {'D', 'X', 'I', 'L'});
  write32(out, static_cast<uint32_t>(program.size()));
  out.insert(out.end(), program.begin(), program.end());
  const auto total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  dxbc_checksum(out.data(), out.size(), out.data() + 4);
  return ShaderFormat::Rebuilt;
}

// Observed layout of the native program (all offsets little-endian dwords):
// the console block in the DXIL part starts with "XDAT" and a version; from
// byte 16 follow (offset, size, kind) entries, offsets relative to "XDAT",
// up to the first entry's data. One entry is a zlib stream holding the native
// program block ("FSSM"): kind 16 in version 5 blocks, kind 7 in version 3
// blocks; its recorded size has been seen to exceed the part by a few bytes.
// In the program block, the dword at 0xC0 is the offset of the code record,
// whose machine code starts 0x28 bytes in; the dword at 0xF8 equals the
// number of root signature parameters. A compute shader's thread group size
// is the desktop one, in the PSV0 part's runtime information (bytes 40-51).
namespace {

struct Parts {
  const uint8_t* dxil = nullptr;
  uint32_t dxil_size = 0;
  const uint8_t* psv = nullptr;
  uint32_t psv_size = 0;
};

bool find_parts(const void* blob, size_t size, Parts* out) {
  const auto* bytes = static_cast<const uint8_t*>(blob);
  if (!bytes || size < 32 || std::memcmp(bytes, "DXBC", 4) != 0) return false;
  const uint32_t count = read32(bytes + 28);
  if (count > 64 || 32 + 4ull * count > size) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t offset = read32(bytes + 32 + 4 * i);
    if (offset + 8ull > size) return false;
    const uint32_t part_size = read32(bytes + offset + 4);
    if (offset + 8ull + part_size > size) return false;
    if (std::memcmp(bytes + offset, "DXIL", 4) == 0) {
      out->dxil = bytes + offset + 8;
      out->dxil_size = part_size;
    } else if (std::memcmp(bytes + offset, "PSV0", 4) == 0) {
      out->psv = bytes + offset + 8;
      out->psv_size = part_size;
    }
  }
  return out->dxil != nullptr;
}

}  // namespace

bool native_program(const void* blob, size_t size, NativeProgram* out) {
  Parts parts;
  if (!find_parts(blob, size, &parts)) return false;
  const uint8_t* xdat = nullptr;
  for (uint32_t at = 0; at + 16 <= parts.dxil_size && at < 256; at += 4) {
    if (std::memcmp(parts.dxil + at, "XDAT", 4) == 0) {
      xdat = parts.dxil + at;
      break;
    }
  }
  if (!xdat) return false;
  const size_t available = static_cast<size_t>(parts.dxil + parts.dxil_size - xdat);
  const uint32_t first = read32(xdat + 16);
  for (uint32_t entry = 16; entry + 12 <= first && entry + 12 <= available; entry += 12) {
    const uint32_t offset = read32(xdat + entry), length = read32(xdat + entry + 4);
    if (offset >= available || xdat[offset] != 0x78) continue;  // not a zlib stream
    const size_t stream = std::min<size_t>(length, available - offset);
    if (!lib::zlib_inflate(xdat + offset, stream, &out->block)) continue;
    if (out->block.size() < 0x100 || std::memcmp(out->block.data(), "FSSM", 4) != 0) continue;
    const uint32_t record = read32(out->block.data() + 0xC0);
    if (record + 0x28ull >= out->block.size()) return false;
    out->code_offset = record + 0x28;
    out->root_parameters = read32(out->block.data() + 0xF8);
    return true;
  }
  return false;
}

bool native_compute_program(const void* blob, size_t size, NativeComputeProgram* out) {
  Parts parts;
  if (!find_parts(blob, size, &parts) || !parts.psv || parts.psv_size < 52 || read32(parts.psv) < 48) return false;
  for (int axis = 0; axis < 3; ++axis) out->threads[axis] = read32(parts.psv + 40 + 4 * axis);
  return native_program(blob, size, out);
}

bool native_compute_registers(const uint32_t* registers, size_t size, ComputeShaderInfo* info, std::string* error) {
  if (size < kNativeComputeRegisterBytes) {
    *error = "register block too short";
    return false;
  }
  const uint32_t rsrc2 = registers[6];
  // COMPUTE_PGM_RSRC2 (public RDNA register layout).
  if (rsrc2 & 1) {
    *error = "scratch memory";
    return false;
  }
  info->threads[0] = registers[3] & 0xFFFF;
  info->threads[1] = registers[3] >> 16;
  info->threads[2] = registers[4];
  if (!info->threads[0] || !info->threads[1] || !info->threads[2] ||
      info->threads[0] * info->threads[1] * info->threads[2] > 1024) {
    *error = "thread group shape";
    return false;
  }
  info->user_sgprs = (rsrc2 >> 1) & 31;
  for (uint32_t axis = 0; axis < 3; ++axis) info->tgid_enabled[axis] = ((rsrc2 >> (7 + axis)) & 1) != 0;
  info->tg_size_enabled = ((rsrc2 >> 10) & 1) != 0;
  const uint32_t lds = ((rsrc2 >> 15) & 0x1FF) * 512;  // granularity 128 dwords
  if (lds) info->lds_bytes = lds;
  return true;
}

bool native_graphics_description(const uint64_t* desc, NativeGraphicsDescription* out, std::string* error) {
  *out = {};
  const auto* state = reinterpret_cast<const uint32_t*>(desc[1]);
  const auto* pipeline = reinterpret_cast<const uint32_t*>(desc[3]);
  if (!state || desc[2] < kNativeRenderStateBytes || !pipeline || desc[4] < kNativePipelineBlockBytes) {
    *error = "unexpected description blocks";
    return false;
  }
  for (size_t i = 5; i + 3 < 5 + 2 * 6 && desc[i]; i += 4) {
    const auto* code = reinterpret_cast<const uint8_t*>(desc[i]);
    const auto* meta = reinterpret_cast<const uint32_t*>(desc[i + 2]);
    if (!desc[i + 1] || !meta || desc[i + 3] < 16) {
      *error = "unexpected stage blocks";
      return false;
    }
    GraphicsStage stage;
    if (!native_graphics_stage(code, desc[i + 1], &stage)) {
      *error = "a stage that is neither a vertex nor a pixel program";
      return false;
    }
    if (stage == GraphicsStage::Vertex && !out->vertex_code) {
      out->vertex_code = code;
      out->vertex_size = desc[i + 1];
    } else if (stage == GraphicsStage::Pixel && !out->pixel_code) {
      out->pixel_code = code;
      out->pixel_size = desc[i + 1];
    } else {
      *error = "two programs for one stage";
      return false;
    }
  }
  if (!out->vertex_code) {
    *error = "no vertex stage";
    return false;
  }
  out->render_targets = pipeline[3];
  if (out->render_targets > 8) {
    *error = "more than eight render targets";
    return false;
  }
  // Bytes 16-23: one DXGI format per render target (titles leave values in
  // the bytes past the target count).
  const auto* format_bytes = reinterpret_cast<const uint8_t*>(pipeline + 4);
  for (uint32_t i = 0; i < out->render_targets; ++i) out->formats[i] = format_bytes[i];
  if (pipeline[6]) {
    char text[64];
    std::snprintf(text, sizeof(text), "a depth format (assumed in dword 6: 0x%x)", pipeline[6]);
    *error = text;
    return false;
  }
  out->sample_mask = pipeline[7];
  out->topology_type = (pipeline[9] >> 16) & 0xFF;
  out->sample_count = pipeline[10] ? pipeline[10] : 1;
  out->target_mask = state[0x1E];
  out->mode_control = state[0x28];
  for (int i = 0; i < 8; ++i) out->blend[i] = state[0x34 + i];
  return true;
}

}  // namespace holy::d3d12
