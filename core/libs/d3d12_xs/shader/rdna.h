// Decoder for the console GPU's shader instruction set (RDNA2, gfx10.3).
//
// Encodings and opcode numbers follow AMD's public "RDNA 2 Instruction Set
// Architecture" reference guide. Only the decoding lives here; meaning is
// given to instructions by the translator (translate.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace holy::d3d12::rdna {

enum class Encoding : uint8_t {
  SOP2, SOPK, SOP1, SOPC, SOPP, SMEM, VOP2, VOP1, VOPC, VOP3, VOP3P, VINTRP, DS, MUBUF, MTBUF, MIMG, EXP, FLAT,
  Unknown,
};

// Operand codes (9 bits): 0-105 SGPRs, 106/107 VCC, 124 null, 125 M0,
// 126/127 EXEC, 128-208 integer constants, 240-248 float constants,
// 253 SCC, 255 literal, 256-511 VGPRs.
inline constexpr uint16_t kVccLo = 106;
inline constexpr uint16_t kVccHi = 107;
inline constexpr uint16_t kNull = 124;
inline constexpr uint16_t kM0 = 125;
inline constexpr uint16_t kExecLo = 126;
inline constexpr uint16_t kExecHi = 127;
inline constexpr uint16_t kScc = 253;
inline constexpr uint16_t kLiteral = 255;
inline constexpr uint16_t kVgpr0 = 256;
inline constexpr uint16_t kSdwa = 249;
inline constexpr uint16_t kDpp16 = 250;
inline constexpr uint16_t kDpp8 = 233;
inline constexpr uint16_t kDpp8Fi = 234;

struct Instruction {
  uint32_t pc = 0;  // byte offset in the program
  uint32_t words = 1;
  Encoding encoding = Encoding::Unknown;
  uint16_t op = 0;
  // Generic operand slots; their meaning depends on the encoding (see rdna.cpp).
  uint16_t dst = 0;
  uint16_t sdst = 0;  // VOP3b carry/mask destination
  uint16_t src[4] = {};
  uint32_t literal = 0;
  int32_t imm = 0;       // SOPK/SOPP immediate, SMEM/MUBUF/FLAT offsets
  uint32_t ext = 0;      // SDWA/DPP extension dword
  uint32_t raw[3] = {};  // the instruction words
  // VOP3 modifiers
  uint8_t abs = 0, neg = 0, omod = 0, opsel = 0;
  bool clamp = false;
  // memory instruction fields
  bool glc = false, offen = false, idxen = false;
  uint8_t seg = 0;      // FLAT segment: 0 flat, 1 scratch, 2 global
  uint8_t dmask = 0;    // MIMG
  uint8_t dim = 0;      // MIMG
  uint8_t target = 0;   // EXP target
  uint8_t enable = 0;   // EXP channel mask
  bool done = false, compressed = false;
};

// Decodes one instruction at `pc` (bytes). Returns false past the end.
bool decode(const uint8_t* code, size_t size, uint32_t pc, Instruction* out);

// Mnemonic for logs and generated comments.
std::string mnemonic(const Instruction& inst);

}  // namespace holy::d3d12::rdna
