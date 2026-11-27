#include "core/libs/d3d12_xs/shader/rdna.h"

#include <cstdio>
#include <cstring>

namespace holy::d3d12::rdna {
namespace {

uint32_t word(const uint8_t* code, size_t size, uint32_t pc) {
  if (pc + 4 > size) return 0;
  uint32_t w;
  std::memcpy(&w, code + pc, 4);
  return w;
}

bool uses_literal(const Instruction& inst, int count) {
  for (int i = 0; i < count; ++i) {
    if (inst.src[i] == kLiteral) return true;
  }
  return false;
}

bool extension(uint16_t src0) {
  return src0 == kSdwa || src0 == kDpp16 || src0 == kDpp8 || src0 == kDpp8Fi;
}

}  // namespace

bool decode(const uint8_t* code, size_t size, uint32_t pc, Instruction* out) {
  if (pc + 4 > size) return false;
  Instruction inst;
  inst.pc = pc;
  const uint32_t w = word(code, size, pc);
  const uint32_t w1 = word(code, size, pc + 4);
  const uint32_t w2 = word(code, size, pc + 8);
  inst.raw[0] = w;
  const uint32_t top9 = w >> 23;
  const uint32_t top6 = w >> 26;

  if (top9 == 0x17D) {  // SOP1
    inst.encoding = Encoding::SOP1;
    inst.op = (w >> 8) & 0xFF;
    inst.dst = (w >> 16) & 0x7F;
    inst.src[0] = w & 0xFF;
    if (uses_literal(inst, 1)) inst.words = 2;
  } else if (top9 == 0x17E) {  // SOPC
    inst.encoding = Encoding::SOPC;
    inst.op = (w >> 16) & 0x7F;
    inst.src[0] = w & 0xFF;
    inst.src[1] = (w >> 8) & 0xFF;
    if (uses_literal(inst, 2)) inst.words = 2;
  } else if (top9 == 0x17F) {  // SOPP
    inst.encoding = Encoding::SOPP;
    inst.op = (w >> 16) & 0x7F;
    inst.imm = static_cast<int16_t>(w & 0xFFFF);
  } else if ((w >> 28) == 0xB) {  // SOPK
    inst.encoding = Encoding::SOPK;
    inst.op = (w >> 23) & 0x1F;
    inst.dst = (w >> 16) & 0x7F;
    inst.imm = static_cast<int16_t>(w & 0xFFFF);
    if (inst.op == 21) inst.words = 2;  // s_setreg_imm32_b32
  } else if ((w >> 30) == 0x2) {  // SOP2
    inst.encoding = Encoding::SOP2;
    inst.op = (w >> 23) & 0x7F;
    inst.dst = (w >> 16) & 0x7F;
    inst.src[0] = w & 0xFF;
    inst.src[1] = (w >> 8) & 0xFF;
    if (uses_literal(inst, 2)) inst.words = 2;
  } else if (top6 == 0x3D) {  // SMEM
    inst.encoding = Encoding::SMEM;
    inst.words = 2;
    inst.op = (w >> 18) & 0xFF;
    inst.glc = (w >> 16) & 1;
    inst.dst = (w >> 6) & 0x7F;
    inst.src[0] = static_cast<uint16_t>((w & 0x3F) * 2);  // sbase
    inst.imm = static_cast<int32_t>(w1 << 11) >> 11;     // 21-bit signed offset
    inst.src[1] = w1 >> 25;                              // soffset
  } else if (top6 == 0x35) {  // VOP3 / VOP3b
    inst.encoding = Encoding::VOP3;
    inst.words = 2;
    inst.op = (w >> 16) & 0x3FF;
    inst.dst = w & 0xFF;
    inst.sdst = (w >> 8) & 0x7F;
    inst.abs = (w >> 8) & 0x7;
    inst.opsel = (w >> 11) & 0xF;
    inst.clamp = (w >> 15) & 1;
    inst.src[0] = w1 & 0x1FF;
    inst.src[1] = (w1 >> 9) & 0x1FF;
    inst.src[2] = (w1 >> 18) & 0x1FF;
    inst.omod = (w1 >> 27) & 0x3;
    inst.neg = (w1 >> 29) & 0x7;
    if (uses_literal(inst, 3)) inst.words = 3;
  } else if ((w >> 24) == 0xCC) {  // VOP3P
    inst.encoding = Encoding::VOP3P;
    inst.words = 2;
    inst.op = (w >> 16) & 0x7F;
    inst.dst = w & 0xFF;
    inst.neg_hi = (w >> 8) & 0x7;
    inst.opsel = (w >> 11) & 0x7;
    inst.opsel_hi = static_cast<uint8_t>(((w1 >> 27) & 0x3) | (((w >> 14) & 1) << 2));
    inst.clamp = (w >> 15) & 1;
    inst.src[0] = w1 & 0x1FF;
    inst.src[1] = (w1 >> 9) & 0x1FF;
    inst.src[2] = (w1 >> 18) & 0x1FF;
    inst.neg = (w1 >> 29) & 0x7;
    if (uses_literal(inst, 3)) inst.words = 3;
  } else if (top6 == 0x32) {  // VINTRP
    inst.encoding = Encoding::VINTRP;
    inst.op = (w >> 16) & 0x3;
    inst.dst = (w >> 18) & 0xFF;
    inst.src[0] = static_cast<uint16_t>(kVgpr0 + (w & 0xFF));
    inst.imm = static_cast<int32_t>((w >> 10) & 0x3F);  // attribute
    inst.src[1] = (w >> 8) & 0x3;                       // channel
  } else if (top6 == 0x36) {  // DS
    inst.encoding = Encoding::DS;
    inst.words = 2;
    inst.op = (w >> 18) & 0xFF;
    inst.imm = static_cast<int32_t>(w & 0xFFFF);  // offset0 | offset1 << 8
    inst.src[0] = static_cast<uint16_t>(kVgpr0 + (w1 & 0xFF));          // addr
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w1 >> 8) & 0xFF));   // data0
    inst.src[2] = static_cast<uint16_t>(kVgpr0 + ((w1 >> 16) & 0xFF));  // data1
    inst.dst = (w1 >> 24) & 0xFF;
  } else if (top6 == 0x37) {  // FLAT / SCRATCH / GLOBAL
    inst.encoding = Encoding::FLAT;
    inst.words = 2;
    inst.op = (w >> 18) & 0x7F;
    inst.seg = (w >> 14) & 0x3;
    inst.glc = (w >> 16) & 1;
    inst.imm = static_cast<int32_t>(w << 20) >> 20;  // 12-bit signed offset
    inst.src[0] = static_cast<uint16_t>(kVgpr0 + (w1 & 0xFF));         // addr
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w1 >> 8) & 0xFF));  // data
    inst.src[2] = (w1 >> 16) & 0x7F;                                   // saddr (0x7d = off)
    inst.dst = (w1 >> 24) & 0xFF;
  } else if (top6 == 0x38) {  // MUBUF
    inst.encoding = Encoding::MUBUF;
    inst.words = 2;
    inst.op = (w >> 18) & 0x7F;
    inst.imm = static_cast<int32_t>(w & 0xFFF);
    inst.offen = (w >> 12) & 1;
    inst.idxen = (w >> 13) & 1;
    inst.glc = (w >> 14) & 1;
    inst.src[0] = static_cast<uint16_t>(kVgpr0 + (w1 & 0xFF));         // vaddr
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w1 >> 8) & 0xFF));  // vdata
    inst.src[2] = static_cast<uint16_t>(((w1 >> 16) & 0x1F) * 4);      // srsrc
    inst.src[3] = (w1 >> 24) & 0xFF;                                    // soffset
  } else if (top6 == 0x3A) {  // MTBUF
    inst.encoding = Encoding::MTBUF;
    inst.words = 2;
    inst.op = ((w >> 16) & 0x7) | (((w1 >> 21) & 1) << 3);
    inst.imm = static_cast<int32_t>(w & 0xFFF);
    inst.offen = (w >> 12) & 1;
    inst.idxen = (w >> 13) & 1;
    inst.src[0] = static_cast<uint16_t>(kVgpr0 + (w1 & 0xFF));
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w1 >> 8) & 0xFF));
    inst.src[2] = static_cast<uint16_t>(((w1 >> 16) & 0x1F) * 4);
    inst.src[3] = (w1 >> 24) & 0xFF;
    inst.ext = (w >> 19) & 0x7F;  // format
  } else if (top6 == 0x3C) {  // MIMG
    inst.encoding = Encoding::MIMG;
    const uint32_t nsa = (w >> 1) & 0x3;
    inst.words = 2 + nsa;
    inst.op = static_cast<uint16_t>(((w >> 18) & 0x7F) | ((w & 1) << 7));
    inst.dim = (w >> 3) & 0x7;
    inst.dmask = (w >> 8) & 0xF;
    inst.glc = (w >> 13) & 1;
    inst.src[0] = static_cast<uint16_t>(kVgpr0 + (w1 & 0xFF));         // vaddr
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w1 >> 8) & 0xFF));  // vdata
    inst.src[2] = static_cast<uint16_t>(((w1 >> 16) & 0x1F) * 4);      // srsrc
    inst.src[3] = static_cast<uint16_t>(((w1 >> 21) & 0x1F) * 4);      // ssamp
    inst.ext = nsa ? w2 : 0;
    for (uint32_t i = 0; i < 4 * nsa && i < 12; ++i) {
      inst.nsa[i] = static_cast<uint8_t>(word(code, size, pc + 8 + 4 * (i / 4)) >> (8 * (i % 4)));
    }
  } else if (top6 == 0x3E) {  // EXP
    inst.encoding = Encoding::EXP;
    inst.words = 2;
    inst.enable = w & 0xF;
    inst.target = (w >> 4) & 0x3F;
    inst.compressed = (w >> 10) & 1;
    inst.done = (w >> 11) & 1;
    for (int i = 0; i < 4; ++i) inst.src[i] = static_cast<uint16_t>(kVgpr0 + ((w1 >> (8 * i)) & 0xFF));
  } else if ((w >> 25) == 0x3F) {  // VOP1
    inst.encoding = Encoding::VOP1;
    inst.op = (w >> 9) & 0xFF;
    inst.dst = (w >> 17) & 0xFF;
    inst.src[0] = w & 0x1FF;
    if (inst.src[0] == kLiteral) inst.words = 2;
    if (extension(inst.src[0])) {
      inst.words = 2;
      inst.ext = w1;
    }
  } else if ((w >> 25) == 0x3E) {  // VOPC
    inst.encoding = Encoding::VOPC;
    inst.op = (w >> 17) & 0xFF;
    inst.src[0] = w & 0x1FF;
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w >> 9) & 0xFF));
    if (inst.src[0] == kLiteral) inst.words = 2;
    if (extension(inst.src[0])) {
      inst.words = 2;
      inst.ext = w1;
    }
  } else if ((w >> 31) == 0) {  // VOP2
    inst.encoding = Encoding::VOP2;
    inst.op = (w >> 25) & 0x3F;
    inst.dst = (w >> 17) & 0xFF;
    inst.src[0] = w & 0x1FF;
    inst.src[1] = static_cast<uint16_t>(kVgpr0 + ((w >> 9) & 0xFF));
    const bool has_constant = inst.op == 32 || inst.op == 33 || inst.op == 44 || inst.op == 45 || inst.op == 55 ||
                              inst.op == 56;  // madmk/madak/fmamk/fmaak (f32, f16)
    if (inst.src[0] == kLiteral || has_constant) inst.words = 2;
    if (extension(inst.src[0])) {
      inst.words = 2;
      inst.ext = w1;
    }
  } else {
    inst.encoding = Encoding::Unknown;
  }
  if (inst.words >= 2) inst.raw[1] = w1;
  if (inst.words >= 3) inst.raw[2] = w2;
  // The literal is the last word of instructions that carry one.
  if (inst.encoding == Encoding::VOP3 || inst.encoding == Encoding::VOP3P) {
    if (inst.words == 3) inst.literal = w2;
  } else if (inst.words == 2 && inst.encoding != Encoding::SMEM && !extension(inst.src[0])) {
    inst.literal = w1;
  }
  *out = inst;
  return pc + 4 * inst.words <= size;
}

std::string mnemonic(const Instruction& inst) {
  static const char* kNames[] = {"sop2", "sopk", "sop1", "sopc", "sopp", "smem", "vop2", "vop1", "vopc",
                                 "vop3", "vop3p", "vintrp", "ds", "mubuf", "mtbuf", "mimg", "exp", "flat", "?"};
  char text[48];
  std::snprintf(text, sizeof(text), "%s op %u", kNames[static_cast<int>(inst.encoding)], inst.op);
  return text;
}

}  // namespace holy::d3d12::rdna

namespace holy::d3d12 {
const char* rdna_build() { return __DATE__ " " __TIME__; }
}  // namespace holy::d3d12
