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
constexpr uint32_t dimension_bits(uint32_t count) { return count << 20; }
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
Operand input_register(uint32_t index, uint32_t component) {
  return {select(component) | type_bits(kInput) | dimension(1), index};
}
Operand output_register(uint32_t index, uint32_t mask) {
  return {kFourComponents | kMaskMode | (mask << 4) | type_bits(kOutput) | dimension(1), index};
}
Operand output_depth() { return {kOneComponent | type_bits(kOutputDepth)}; }
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
Operand icb_indexed(uint32_t temp_index, uint32_t component) {
  // One index: immediate plus relative (representation 3), immediate 0.
  Operand out{select(component) | type_bits(kImmediateConstantBuffer) | dimension(1) | (3u << 22), 0};
  const Operand rel = temp(temp_index);
  out.insert(out.end(), rel.begin(), rel.end());
  return out;
}
Operand uav_indexed(uint32_t id, uint32_t temp_index, Access access) {
  uint32_t components = access == Access::Load         ? swizzle_xxxx()
                        : access == Access::LoadVector ? swizzle_xxxx() | (0xE4u << 4)
                        : access == Access::Store      ? mask_x()
                                                       : 0u;
  Operand out{kExtended | components | type_bits(kUav) | dimension(2) | relative_index(1),
              1u | (1u << 17),  // modifier token: non-uniform index
              id};
  const Operand rel = temp(temp_index);
  out.insert(out.end(), rel.begin(), rel.end());
  return out;
}
Operand uav(uint32_t id, uint32_t index, Access access) {
  uint32_t components = access == Access::Load ? swizzle_xxxx() : access == Access::Store ? mask_x() : 0u;
  return {components | type_bits(kUav) | dimension(2), id, index};
}
Operand srv_xyzw(uint32_t id, uint32_t index) {
  return {swizzle_xxxx() | (0xE4u << 4) | type_bits(kResource) | dimension(2), id, index};
}
Operand temp_dst_mask(uint32_t index, uint32_t mask) {
  return {kFourComponents | kMaskMode | (mask << 4) | type_bits(kTemp) | dimension(1), index};
}
Operand temp_swizzle(uint32_t index, uint32_t swizzle) {
  return {kFourComponents | kSwizzleMode | ((swizzle & 0xFF) << 4) | type_bits(kTemp) | dimension(1), index};
}
Operand temp_xyzw(uint32_t index) {
  return {kFourComponents | kSwizzleMode | (0xE4u << 4) | type_bits(kTemp) | dimension(1), index};
}

namespace {
Operand indexed(uint32_t type, uint32_t components, uint32_t id, uint32_t temp_index) {
  Operand out{kExtended | components | type_bits(type) | dimension(2) | relative_index(1),
              1u | (1u << 17),  // modifier token: non-uniform index
              id};
  const Operand rel = temp(temp_index);
  out.insert(out.end(), rel.begin(), rel.end());
  return out;
}
}  // namespace

Operand texture_indexed(uint32_t id, uint32_t temp_index) {
  return indexed(kResource, swizzle_xxxx() | (0xE4u << 4), id, temp_index);
}
Operand typed_uav_indexed(uint32_t id, uint32_t temp_index, bool store) {
  return indexed(kUav, store ? (kFourComponents | kMaskMode | (0xFu << 4)) : (swizzle_xxxx() | (0xE4u << 4)), id,
                 temp_index);
}
Operand typed_uav_atomic(uint32_t id, uint32_t temp_index) { return indexed(kUav, 0, id, temp_index); }
Operand sampler_indexed(uint32_t id, uint32_t temp_index) { return indexed(kSampler, 0, id, temp_index); }
Operand sampler_indexed_select(uint32_t id, uint32_t temp_index, uint32_t component) {
  return indexed(kSampler, select(component), id, temp_index);
}

Operand gs_input_xyzw(uint32_t vertex, uint32_t reg) {
  return {kFourComponents | kSwizzleMode | (0xE4u << 4) | type_bits(kInput) | dimension(2), vertex, reg};
}
Operand output_xyzw(uint32_t reg) { return output_register(reg, 0xF); }
Operand stream0() {
  constexpr uint32_t kStream = 16;
  return {type_bits(kStream) | dimension(1), 0};
}

Operand group_shared(uint32_t id, Access access) {
  uint32_t components = access == Access::Load ? swizzle_xxxx() : access == Access::Store ? mask_x() : 0u;
  return {components | type_bits(kGroupShared) | dimension(1), id};
}

Program::Program(ProgramType type) { tokens_ = {(static_cast<uint32_t>(type) << 16) | 0x51u, 0u}; }

void Program::emit(uint32_t opcode, const Operand* operands, size_t count, uint32_t controls) {
  size_t length = 1;
  for (size_t i = 0; i < count; ++i) length += operands[i].size();
  tokens_.push_back(opcode | controls | (static_cast<uint32_t>(length) << 24));
  for (size_t i = 0; i < count; ++i) tokens_.insert(tokens_.end(), operands[i].begin(), operands[i].end());
}

namespace {

// An operand as fold_moves sees it.
struct ParsedOperand {
  uint32_t index_token;  // the first index's token (temps: the register number)
  uint8_t type;
  uint8_t mode;  // selection mode (four components): 0 mask, 1 swizzle, 2 select
  uint8_t components;
  uint8_t bits;  // the mask, swizzle or selected component (bits 11-4)
  uint8_t dims;
  bool simple;      // one immediate index, no extended token, no relative index
  bool many_reads;  // more relative reads than `reads` holds: treated as reading every temp
  uint8_t read_count;
  uint32_t reads[2];  // temps read through relative indices
  void add_read(uint32_t reg) {
    if (read_count < 2) reads[read_count++] = reg;
    else many_reads = true;
  }
};

// Parses the operand at tokens[at]; returns the position after it.
size_t parse_operand(const std::vector<uint32_t>& tokens, size_t at, size_t end, ParsedOperand* out) {
  *out = ParsedOperand{};
  if (at >= end) return end;
  const uint32_t token = tokens[at];
  out->components = static_cast<uint8_t>(token & 3);
  out->mode = static_cast<uint8_t>((token >> 2) & 3);
  out->bits = static_cast<uint8_t>((token >> 4) & 0xFF);
  out->type = static_cast<uint8_t>((token >> 12) & 0xFF);
  out->dims = static_cast<uint8_t>((token >> 20) & 3);
  bool extended = (token >> 31) & 1;
  out->simple = !extended;
  size_t pos = at + 1;
  while (extended && pos < end) extended = (tokens[pos++] >> 31) & 1;
  if (out->type == kImmediate32 || out->type == 5) {  // immediates: values follow
    const size_t count = out->components == 2 ? 4 : 1;
    return pos + (out->type == 5 ? 2 * count : count);
  }
  for (uint32_t d = 0; d < out->dims && pos <= end; ++d) {
    const uint32_t representation = (token >> (22 + 3 * d)) & 7;
    if (d == 0) out->index_token = static_cast<uint32_t>(pos);
    if (representation == 0 || representation == 3) pos += 1;
    if (representation == 1 || representation == 4) pos += 2;
    if (representation >= 2) {
      out->simple = false;
      ParsedOperand nested;
      pos = parse_operand(tokens, pos, end, &nested);
      if (nested.type == kTemp && nested.dims == 1) out->add_read(tokens[nested.index_token]);
      for (uint32_t r = 0; r < nested.read_count; ++r) out->add_read(nested.reads[r]);
      out->many_reads = out->many_reads || nested.many_reads;
    }
  }
  if (out->dims != 1) out->simple = false;
  return pos;
}

struct ParsedInstruction {
  uint32_t start, length;
  uint16_t opcode, controls;
  uint32_t count;  // operands parsed; more than 8 make the program left as it is
  ParsedOperand operands[8];
};

}  // namespace

size_t Program::fold_moves(uint32_t first_scratch) {
  std::vector<ParsedInstruction> code;
  code.reserve(tokens_.size() / 4);
  for (size_t at = 2; at < tokens_.size();) {  // after the version and length tokens
    ParsedInstruction& inst = code.emplace_back();
    inst.start = static_cast<uint32_t>(at);
    const uint32_t token = tokens_[at];
    inst.opcode = static_cast<uint16_t>(token & 0x7FF);
    inst.controls = static_cast<uint16_t>((token >> 11) & 0x1FFF);
    inst.length = (token >> 24) & 0x7F;
    inst.count = 0;
    if (inst.opcode == 53) return 0;  // declarations' data: not instructions only
    if (inst.length == 0 || at + inst.length > tokens_.size()) return 0;
    size_t pos = at + 1;
    for (bool extended = (token >> 31) & 1; extended && pos < at + inst.length;) {
      extended = (tokens_[pos++] >> 31) & 1;
    }
    while (pos < at + inst.length) {
      if (inst.count == 8) return 0;
      pos = parse_operand(tokens_, pos, at + inst.length, &inst.operands[inst.count++]);
    }
    at += inst.length;
  }
  auto is_temp_x_dst = [](const ParsedOperand& o) {
    return o.type == kTemp && o.components == 2 && o.mode == 0 && o.bits == 1 && o.simple;
  };
  auto is_temp_x_src = [](const ParsedOperand& o) {
    return o.type == kTemp && o.components == 2 && o.simple && (o.mode == 1 || o.mode == 2) && o.bits == 0;
  };
  auto destination = [](const ParsedOperand& o) { return o.type == kTemp && o.components == 2 && o.mode == 0; };
  // A move this pass may fold: "mov rX.x, rT.x" after an instruction whose
  // only destination is rT.x, rT a scratch temp.
  auto candidate = [&](size_t k) {
    const ParsedInstruction& move = code[k];
    if (k == 0 || move.opcode != kMov || move.controls != 0 || move.count != 2) return false;
    if (!is_temp_x_dst(move.operands[0]) || !is_temp_x_src(move.operands[1])) return false;
    const uint32_t target = tokens_[move.operands[0].index_token];
    const uint32_t temp = tokens_[move.operands[1].index_token];
    if (temp < first_scratch || temp == target) return false;
    const ParsedInstruction& def = code[k - 1];
    if (def.count == 0 || !is_temp_x_dst(def.operands[0]) || tokens_[def.operands[0].index_token] != temp) {
      return false;
    }
    for (uint32_t o = 1; o < def.count; ++o) {  // no second destination (uaddc, imul with null, ...)
      if (destination(def.operands[o]) || def.operands[o].type == kNull) return false;
    }
    return true;
  };

  // Liveness of scratch temps' components, from the end back (a block's
  // code has ifs, elses and endifs; anything else ends the pass): a move
  // folds when its temp's x is not live after it.
  constexpr uint32_t kTracked = 128;
  struct Live {
    uint64_t words[kTracked * 4 / 64] = {};
    void merge(const Live& other) {
      for (size_t i = 0; i < std::size(words); ++i) words[i] |= other.words[i];
    }
  };
  auto slot = [&](uint32_t reg, uint32_t component, size_t* word, uint64_t* bit) {
    if (reg < first_scratch || reg >= first_scratch + kTracked) return false;
    const uint32_t index = (reg - first_scratch) * 4 + component;
    *word = index / 64;
    *bit = uint64_t{1} << (index % 64);
    return true;
  };
  auto mark = [&](Live& live, uint32_t reg, uint32_t component, bool on) {
    size_t word;
    uint64_t bit;
    if (!slot(reg, component, &word, &bit)) return;
    if (on) live.words[word] |= bit;
    else live.words[word] &= ~bit;
  };
  auto is_live = [&](const Live& live, uint32_t reg, uint32_t component) {
    size_t word;
    uint64_t bit;
    return slot(reg, component, &word, &bit) && (live.words[word] & bit) != 0;
  };
  // An instruction's reads, added to `live`; false when they cannot be told.
  auto add_reads = [&](const ParsedInstruction& inst, Live& live) {
    for (uint32_t k = 0; k < inst.count; ++k) {
      const ParsedOperand& o = inst.operands[k];
      if (o.many_reads) return false;
      for (uint32_t r = 0; r < o.read_count; ++r) {
        for (uint32_t c = 0; c < 4; ++c) mark(live, o.reads[r], c, true);
      }
      if (o.type != kTemp || destination(o) || o.dims != 1) continue;
      const uint32_t reg = tokens_[o.index_token];
      if (o.components == 2 && o.mode == 2) {
        mark(live, reg, o.bits & 3, true);
      } else if (o.components == 2 && o.mode == 1) {
        for (uint32_t c = 0; c < 4; ++c) mark(live, reg, (o.bits >> (2 * c)) & 3, true);
      } else {
        for (uint32_t c = 0; c < 4; ++c) mark(live, reg, c, true);
      }
    }
    return true;
  };
  struct Frame {
    Live after;       // live after the endif
    Live else_entry;  // live at the else branch's start
    bool has_else = false;
  };
  std::vector<Frame> frames;
  std::vector<char> foldable(code.size(), 0);
  Live live;  // nothing is live past the block: scratch temps live within one instruction's code
  for (size_t k = code.size(); k-- > 0;) {
    const ParsedInstruction& inst = code[k];
    if (candidate(k)) foldable[k] = !is_live(live, tokens_[inst.operands[1].index_token], 0);
    const uint32_t op = inst.opcode;
    if (op == kEndif) {
      frames.push_back(Frame{live, Live{}, false});
      continue;
    }
    if (op == kElse) {
      if (frames.empty()) return 0;
      frames.back().else_entry = live;
      frames.back().has_else = true;
      live = frames.back().after;
      continue;
    }
    if (op == kIf) {
      if (frames.empty()) return 0;
      const Frame frame = frames.back();
      frames.pop_back();
      live.merge(frame.has_else ? frame.else_entry : frame.after);
      if (!add_reads(inst, live)) return 0;
      continue;
    }
    if (op == kLoop || op == kEndloop || op == kBreak || op == kBreakc || op == kSwitch || op == kCase ||
        op == kDefault || op == kEndswitch || op == kRet || op == 7 || op == 8 || op == 63) {
      return 0;
    }
    for (uint32_t o = 0; o < inst.count; ++o) {
      const ParsedOperand& d = inst.operands[o];
      if (!destination(d) || !d.simple) continue;
      for (uint32_t c = 0; c < 4; ++c) {
        if ((d.bits >> c) & 1) mark(live, tokens_[d.index_token], c, false);
      }
    }
    if (!add_reads(inst, live)) return 0;
  }
  if (!frames.empty()) return 0;

  std::vector<char> drop(code.size(), 0);
  size_t folded = 0;
  for (size_t k = 1; k < code.size(); ++k) {
    if (!foldable[k] || drop[k - 1]) continue;
    tokens_[code[k - 1].operands[0].index_token] = tokens_[code[k].operands[0].index_token];
    drop[k] = 1;
    ++folded;
  }
  if (!folded) return 0;
  std::vector<uint32_t> out(tokens_.begin(), tokens_.begin() + 2);
  out.reserve(tokens_.size());
  for (size_t k = 0; k < code.size(); ++k) {
    if (drop[k]) continue;
    out.insert(out.end(), tokens_.begin() + static_cast<std::ptrdiff_t>(code[k].start),
               tokens_.begin() + static_cast<std::ptrdiff_t>(code[k].start + code[k].length));
  }
  tokens_ = std::move(out);
  return folded;
}

void Program::dcl_global_flags(uint32_t flags) { tokens_.push_back(kDclGlobalFlags | (flags << 11) | (1u << 24)); }

void Program::dcl_immediate_constant_buffer(const std::vector<uint32_t>& data) {
  // Custom-data block (opcode 53, class 3): the length token counts the whole
  // block, both header tokens included.
  std::vector<uint32_t> padded = data;
  while (padded.size() % 4) padded.push_back(0);
  tokens_.push_back(53u | (3u << 11));
  tokens_.push_back(static_cast<uint32_t>(padded.size() + 2));
  tokens_.insert(tokens_.end(), padded.begin(), padded.end());
}

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

void Program::dcl_resource_raw_range(uint32_t id, uint32_t first, uint32_t last, uint32_t space) {
  tokens_.push_back(kDclResourceRaw | (6u << 24));
  tokens_.insert(tokens_.end(),
                 {swizzle_xxxx() | (0xE4u << 4) | type_bits(kResource) | dimension(3), id, first, last, space});
}

void Program::dcl_texture_range(uint32_t id, ResourceDimension dimension, uint32_t space) {
  tokens_.push_back(kDclResource | (static_cast<uint32_t>(dimension) << 11) | (7u << 24));
  tokens_.insert(tokens_.end(), {swizzle_xxxx() | (0xE4u << 4) | type_bits(kResource) | dimension_bits(3), id, 0u,
                                 0xFFFFFFFFu, 0x5555u, space});
}

void Program::dcl_typed_uav_range(uint32_t id, ResourceDimension dimension, uint32_t space, bool integer,
                                  bool ordered) {
  // Opcode flag 0x20000: rasterizer-ordered access.
  tokens_.push_back(kDclUavTyped | (static_cast<uint32_t>(dimension) << 11) | (ordered ? 0x20000u : 0u) |
                    (7u << 24));
  // Return type per component: 5 float, 4 unsigned integer.
  tokens_.insert(tokens_.end(), {swizzle_xxxx() | (0xE4u << 4) | type_bits(kUav) | dimension_bits(3), id, 0u,
                                 0xFFFFFFFFu, integer ? 0x4444u : 0x5555u, space});
}

void Program::dcl_sampler_range(uint32_t id, uint32_t space) {
  tokens_.push_back(kDclSampler | (6u << 24));
  tokens_.insert(tokens_.end(), {swizzle_xxxx() | (0xE4u << 4) | type_bits(kSampler) | dimension_bits(3), id, 0u,
                                 0xFFFFFFFFu, space});
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

void Program::dcl_input_sgv(uint32_t reg, uint32_t mask, SystemValue name) {
  tokens_.insert(tokens_.end(), {kDclInputSgv | (4u << 24),
                                 kFourComponents | kMaskMode | (mask << 4) | type_bits(kInput) | dimension(1), reg,
                                 static_cast<uint32_t>(name)});
}

void Program::dcl_input_ps(uint32_t reg, uint32_t mask, Interpolation mode) {
  tokens_.insert(tokens_.end(), {kDclInputPs | (static_cast<uint32_t>(mode) << 11) | (3u << 24),
                                 kFourComponents | kMaskMode | (mask << 4) | type_bits(kInput) | dimension(1), reg});
}

void Program::dcl_input_ps_siv(uint32_t reg, uint32_t mask, Interpolation mode, SystemValue name) {
  tokens_.insert(tokens_.end(), {kDclInputPsSiv | (static_cast<uint32_t>(mode) << 11) | (4u << 24),
                                 kFourComponents | kMaskMode | (mask << 4) | type_bits(kInput) | dimension(1), reg,
                                 static_cast<uint32_t>(name)});
}

void Program::dcl_output(uint32_t reg, uint32_t mask) {
  tokens_.insert(tokens_.end(), {kDclOutput | (3u << 24), output_register(reg, mask)[0], reg});
}

void Program::dcl_output_siv(uint32_t reg, uint32_t mask, SystemValue name) {
  tokens_.insert(tokens_.end(),
                 {kDclOutputSiv | (4u << 24), output_register(reg, mask)[0], reg, static_cast<uint32_t>(name)});
}

void Program::dcl_output_depth() { tokens_.insert(tokens_.end(), {kDclOutput | (2u << 24), output_depth()[0]}); }

void Program::dcl_gs_input(uint32_t vertices, uint32_t reg, uint32_t mask, bool position) {
  const uint32_t operand = kFourComponents | kMaskMode | (mask << 4) | type_bits(kInput) | dimension(2);
  if (position) {
    tokens_.insert(tokens_.end(), {kDclInputSiv | (5u << 24), operand, vertices, reg, static_cast<uint32_t>(kPosition)});
  } else {
    tokens_.insert(tokens_.end(), {kDclInput | (4u << 24), operand, vertices, reg});
  }
}

void Program::dcl_gs_triangles_to_strips(uint32_t max_vertices) {
  constexpr uint32_t kTriangle = 3, kTriangleStrip = 5;
  const Operand stream = stream0();
  tokens_.push_back(kDclGsInputPrimitive | (kTriangle << 11) | (1u << 24));
  tokens_.push_back(kDclStream | (static_cast<uint32_t>(1 + stream.size()) << 24));
  tokens_.insert(tokens_.end(), stream.begin(), stream.end());
  tokens_.push_back(kDclGsOutputTopology | (kTriangleStrip << 11) | (1u << 24));
  tokens_.insert(tokens_.end(), {kDclMaxOutputVertexCount | (2u << 24), max_vertices});
}

void Program::append(const Program& other) {
  tokens_.insert(tokens_.end(), other.tokens_.begin() + 2, other.tokens_.end());
}

namespace {

// A signature chunk's contents: count, 8, 24-byte elements, then the names.
// With streams (OSG5), each element starts with its stream (28 bytes).
std::vector<uint8_t> signature_chunk(const std::vector<SignatureElement>& elements, bool streams = false) {
  const size_t stride = streams ? 28 : 24, lead = streams ? 4 : 0;
  std::vector<uint8_t> out(8 + stride * elements.size());
  const uint32_t count = static_cast<uint32_t>(elements.size()), eight = 8;
  std::memcpy(out.data(), &count, 4);
  std::memcpy(out.data() + 4, &eight, 4);
  for (size_t i = 0; i < elements.size(); ++i) {
    const SignatureElement& e = elements[i];
    const uint32_t name = static_cast<uint32_t>(out.size());
    out.insert(out.end(), e.name, e.name + std::strlen(e.name) + 1);
    const uint32_t fields[5] = {name, e.index, e.system_value, e.component_type, e.reg};
    std::memcpy(out.data() + 8 + stride * i + lead, fields, sizeof(fields));
    out[8 + stride * i + lead + 20] = e.mask;
    out[8 + stride * i + lead + 21] = e.rw_mask;
  }
  while (out.size() % 4) out.push_back(0);
  return out;
}

}  // namespace

std::vector<uint8_t> Program::container(const std::vector<SignatureElement>& inputs,
                                        const std::vector<SignatureElement>& outputs, uint64_t features) const {
  std::vector<uint32_t> shex = tokens_;
  shex[1] = static_cast<uint32_t>(shex.size());
  // Chunks: ISGN, OSGN, SHEX, and SFI0 (8 bytes of feature bits) if any.
  const bool geometry = (tokens_[0] >> 16) == static_cast<uint32_t>(ProgramType::Geometry);
  const std::vector<uint8_t> in_chunk = signature_chunk(inputs), out_chunk = signature_chunk(outputs, geometry);
  const uint32_t chunks = features ? 4 : 3;
  const uint32_t header = 32 + chunks * 4;
  const uint32_t isgn = header, osgn = isgn + 8 + static_cast<uint32_t>(in_chunk.size());
  const uint32_t code = osgn + 8 + static_cast<uint32_t>(out_chunk.size());
  const uint32_t sfi0 = code + 8 + static_cast<uint32_t>(shex.size() * 4);
  const uint32_t total = sfi0 + (features ? 16 : 0);
  std::vector<uint8_t> out(total);
  auto put = [&](uint32_t at, const void* data, size_t size) { std::memcpy(out.data() + at, data, size); };
  auto put32 = [&](uint32_t at, uint32_t value) { put(at, &value, 4); };
  put(0, "DXBC", 4);
  put32(20, 1);
  put32(24, total);
  put32(28, chunks);
  put32(32, isgn);
  put32(36, osgn);
  put32(40, code);
  if (features) {
    put32(44, sfi0);
    put(sfi0, "SFI0", 4);
    put32(sfi0 + 4, 8);
    put(sfi0 + 8, &features, 8);
  }
  put(isgn, "ISGN", 4);
  put32(isgn + 4, static_cast<uint32_t>(in_chunk.size()));
  put(isgn + 8, in_chunk.data(), in_chunk.size());
  put(osgn, geometry ? "OSG5" : "OSGN", 4);
  put32(osgn + 4, static_cast<uint32_t>(out_chunk.size()));
  put(osgn + 8, out_chunk.data(), out_chunk.size());
  put(code, "SHEX", 4);
  put32(code + 4, static_cast<uint32_t>(shex.size() * 4));
  put(code + 8, shex.data(), shex.size() * 4);
  dxbc_checksum(out.data(), out.size(), out.data() + 4);
  return out;
}

}  // namespace holy::d3d12::dxbc

namespace holy::d3d12 {
const char* dxbc_build() { return __DATE__ " " __TIME__; }
}  // namespace holy::d3d12
