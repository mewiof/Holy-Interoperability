#include "core/libs/d3d12_xs/shader/dxbc.h"

#include <cstring>

#include "core/libs/d3d12_xs/shader_container.h"

namespace holy::d3d12::dxbc {
namespace {

// Operand token fields.
constexpr uint32_t kOneComponent = 1;
constexpr uint32_t kFourComponents = 2;
constexpr uint32_t kMaskMode = 0u << 2;
constexpr uint32_t kSwizzleMode = 1u << 2;
constexpr uint32_t kSelectMode = 2u << 2;
constexpr uint32_t kExtended = 1u << 31;

constexpr uint32_t type_bits(uint32_t type) { return type << 12; }
constexpr uint32_t dimension(uint32_t count) { return count << 20; }
// Representation of index n: 0 immediate, 2 relative.
constexpr uint32_t relative_index(uint32_t n) { return 2u << (22 + 3 * n); }

uint32_t select(uint32_t component) { return kFourComponents | kSelectMode | (component << 4); }
uint32_t mask_x() { return kFourComponents | kMaskMode | (1u << 4); }
uint32_t swizzle_xxxx() { return kFourComponents | kSwizzleMode; }

void add_modifier(Operand& operand, uint32_t modifier) {
  if (operand[0] & kExtended) {
    operand[1] |= modifier;
    return;
  }
  operand[0] |= kExtended;
  operand.insert(operand.begin() + 1, 1u | modifier);  // extended type 1: modifier
}

}  // namespace

Operand temp(uint32_t index, uint32_t component) {
  return {select(component) | type_bits(kTemp) | dimension(1), index};
}
Operand temp_dst(uint32_t index) { return {mask_x() | type_bits(kTemp) | dimension(1), index}; }
Operand null_dst() { return {type_bits(kNull)}; }
Operand imm(uint32_t value) { return {kOneComponent | type_bits(kImmediate32), value}; }
Operand immf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return imm(bits);
}
Operand neg(Operand operand) {
  add_modifier(operand, 1u << 6);
  return operand;
}
Operand abs(Operand operand) {
  add_modifier(operand, 2u << 6);
  return operand;
}
Operand input(OperandType type, uint32_t component) { return {select(component) | type_bits(type)}; }
Operand cb(uint32_t id, uint32_t reg, uint32_t component) {
  return {select(component) | type_bits(kConstantBuffer) | dimension(3), id, 0, reg};
}
Operand cb_indexed(uint32_t id, uint32_t temp_index, uint32_t reg, uint32_t component) {
  // Third index: immediate plus relative (representation 3).
  Operand out{select(component) | type_bits(kConstantBuffer) | dimension(3) | (3u << (22 + 6)), id, 0, reg};
  const Operand rel = temp(temp_index);
  out.insert(out.end(), rel.begin(), rel.end());
  return out;
}
Operand uav_indexed(uint32_t id, uint32_t temp_index, Access access) {
  uint32_t components = access == Access::Load ? swizzle_xxxx() : access == Access::Store ? mask_x() : 0u;
  Operand out{kExtended | components | type_bits(kUav) | dimension(2) | relative_index(1),
              1u | (1u << 17),  // modifier token: non-uniform index
              id};
  const Operand rel = temp(temp_index);
  out.insert(out.end(), rel.begin(), rel.end());
  return out;
}
Operand group_shared(uint32_t id, Access access) {
  uint32_t components = access == Access::Load ? swizzle_xxxx() : access == Access::Store ? mask_x() : 0u;
  return {components | type_bits(kGroupShared) | dimension(1), id};
}

Program::Program() { tokens_ = {0x00050051u, 0u}; }

void Program::emit(uint32_t opcode, const std::vector<Operand>& operands, uint32_t controls) {
  size_t length = 1;
  for (const Operand& o : operands) length += o.size();
  tokens_.push_back(opcode | controls | (static_cast<uint32_t>(length) << 24));
  for (const Operand& o : operands) tokens_.insert(tokens_.end(), o.begin(), o.end());
}

void Program::dcl_global_flags(uint32_t flags) { tokens_.push_back(kDclGlobalFlags | (flags << 11) | (1u << 24)); }

void Program::dcl_constant_buffer(uint32_t id, uint32_t space, uint32_t vectors, bool dynamic) {
  tokens_.push_back(kDclConstantBuffer | (dynamic ? 1u << 11 : 0u) | (7u << 24));
  tokens_.insert(tokens_.end(), {swizzle_xxxx() | (0xE4u << 4) | type_bits(kConstantBuffer) | dimension(3), id, 0u, 0u,
                                 vectors, space});
}

void Program::dcl_uav_raw_range(uint32_t id, uint32_t first, uint32_t last, uint32_t space) {
  tokens_.push_back(kDclUavRaw | (6u << 24));
  tokens_.insert(tokens_.end(),
                 {swizzle_xxxx() | (0xE4u << 4) | type_bits(kUav) | dimension(3), id, first, last, space});
}

void Program::dcl_input(OperandType type, uint32_t mask) {
  tokens_.push_back(kDclInput | (2u << 24));
  tokens_.push_back(mask == 0 ? type_bits(type) : (kFourComponents | kMaskMode | (mask << 4) | type_bits(type)));
}

void Program::dcl_temps(uint32_t count) { tokens_.insert(tokens_.end(), {kDclTemps | (2u << 24), count}); }

void Program::dcl_tgsm_raw(uint32_t id, uint32_t bytes) {
  tokens_.insert(tokens_.end(), {kDclTgsmRaw | (4u << 24), type_bits(kGroupShared) | dimension(1), id, bytes});
}

void Program::dcl_thread_group(uint32_t x, uint32_t y, uint32_t z) {
  tokens_.insert(tokens_.end(), {kDclThreadGroup | (4u << 24), x, y, z});
}

void Program::append(const Program& other) {
  tokens_.insert(tokens_.end(), other.tokens_.begin() + 2, other.tokens_.end());
}

std::vector<uint8_t> Program::container() const {
  std::vector<uint32_t> shex = tokens_;
  shex[1] = static_cast<uint32_t>(shex.size());
  // Chunks: ISGN and OSGN (no elements), SHEX.
  const uint32_t signature[2] = {0, 8};
  const uint32_t header = 32 + 3 * 4;
  const uint32_t isgn = header, osgn = isgn + 8 + 8, code = osgn + 8 + 8;
  const uint32_t total = code + 8 + static_cast<uint32_t>(shex.size() * 4);
  std::vector<uint8_t> out(total);
  auto put = [&](uint32_t at, const void* data, size_t size) { std::memcpy(out.data() + at, data, size); };
  auto put32 = [&](uint32_t at, uint32_t value) { put(at, &value, 4); };
  put(0, "DXBC", 4);
  put32(20, 1);
  put32(24, total);
  put32(28, 3);
  put32(32, isgn);
  put32(36, osgn);
  put32(40, code);
  put(isgn, "ISGN", 4);
  put32(isgn + 4, 8);
  put(isgn + 8, signature, 8);
  put(osgn, "OSGN", 4);
  put32(osgn + 4, 8);
  put(osgn + 8, signature, 8);
  put(code, "SHEX", 4);
  put32(code + 4, static_cast<uint32_t>(shex.size() * 4));
  put(code + 8, shex.data(), shex.size() * 4);
  dxbc_checksum(out.data(), out.size(), out.data() + 4);
  return out;
}

}  // namespace holy::d3d12::dxbc
