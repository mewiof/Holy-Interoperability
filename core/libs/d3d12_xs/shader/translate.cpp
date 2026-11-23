#include "core/libs/d3d12_xs/shader/translate.h"

#include <cstdarg>
#include <cstdio>
#include <set>
#include <tuple>
#include <utility>

#include "core/libs/d3d12_xs/shader/dxbc.h"
#include "core/libs/d3d12_xs/shader/rdna.h"

namespace holy::d3d12 {
namespace {

using dxbc::Operand;
using rdna::Encoding;
using rdna::Instruction;
namespace dx = dxbc;

std::string format(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  return buffer;
}

// Temp register file: one register value per temp (x component).
constexpr uint32_t kVccLoR = 106, kVccHiR = 107, kM0R = 108, kExecLoR = 109, kExecHiR = 110, kSccR = 111;
constexpr uint32_t kNextR = 112, kLaneR = 113, kBarrierR = 114;
constexpr uint32_t kScratch = 116, kScratchCount = 112;
constexpr uint32_t kVgprBase = kScratch + kScratchCount;
constexpr uint32_t kNoRegister = 0xFFFFFFFFu;
constexpr uint32_t kNoView = 0xFFFFFFFFu;

Operand R(uint32_t r) { return dxbc::temp(r); }
Operand I(uint32_t v) { return dxbc::imm(v); }
Operand D(uint32_t r) { return dxbc::temp_dst(r); }
bool is_temp(const Operand& o) { return ((o[0] >> 12) & 0xFF) == dxbc::kTemp; }

class Translator {
 public:
  Translator(const uint8_t* code, size_t size, const ComputeShaderInfo& info) : code_(code), size_(size), info_(info) {}

  bool run(std::vector<uint8_t>* out, std::string* error);

 private:
  bool fail(const std::string& why) {
    if (error_.empty()) error_ = why;
    return false;
  }

  // ---- building blocks ----
  uint32_t fresh() {
    if (scratch_ >= kScratchCount) {
      fail("instruction needs too many temporaries");
      return kScratch;
    }
    return kScratch + scratch_++;
  }
  void emit(uint32_t opcode, const std::vector<Operand>& operands, uint32_t controls = 0) {
    p_.emit(opcode, operands, controls);
  }
  Operand op1(uint32_t opcode, const Operand& a, uint32_t controls = 0) {
    const uint32_t r = fresh();
    emit(opcode, {D(r), a}, controls);
    return R(r);
  }
  Operand op2(uint32_t opcode, const Operand& a, const Operand& b, uint32_t controls = 0) {
    const uint32_t r = fresh();
    emit(opcode, {D(r), a, b}, controls);
    return R(r);
  }
  Operand op3(uint32_t opcode, const Operand& a, const Operand& b, const Operand& c, uint32_t controls = 0) {
    const uint32_t r = fresh();
    emit(opcode, {D(r), a, b, c}, controls);
    return R(r);
  }
  Operand op4(uint32_t opcode, const Operand& a, const Operand& b, const Operand& c, const Operand& d) {
    const uint32_t r = fresh();
    emit(opcode, {D(r), a, b, c, d});
    return R(r);
  }
  void put(uint32_t reg, const Operand& v) { emit(dx::kMov, {D(reg), v}); }
  Operand in_temp(const Operand& v) { return is_temp(v) ? v : op1(dx::kMov, v); }
  Operand copy(const Operand& v) { return op1(dx::kMov, v); }
  // Integer helpers. Masks (comparison results) are 0 or ~0.
  Operand add(const Operand& a, const Operand& b) { return op2(dx::kIadd, a, b); }
  Operand sub(const Operand& a, const Operand& b) { return op2(dx::kIadd, a, dxbc::neg(in_temp(b))); }
  Operand band(const Operand& a, const Operand& b) { return op2(dx::kAnd, a, b); }
  Operand bor(const Operand& a, const Operand& b) { return op2(dx::kOr, a, b); }
  Operand bxor(const Operand& a, const Operand& b) { return op2(dx::kXor, a, b); }
  Operand bnot(const Operand& a) { return op1(dx::kNot, a); }
  Operand shl(const Operand& a, const Operand& n) { return op2(dx::kIshl, a, n); }
  Operand shr(const Operand& a, const Operand& n) { return op2(dx::kUshr, a, n); }
  Operand sar(const Operand& a, const Operand& n) { return op2(dx::kIshr, a, n); }
  Operand select(const Operand& mask, const Operand& a, const Operand& b) { return op3(dx::kMovc, mask, a, b); }
  Operand eq(const Operand& a, const Operand& b) { return op2(dx::kIeq, a, b); }
  Operand ne(const Operand& a, const Operand& b) { return op2(dx::kIne, a, b); }
  Operand ult(const Operand& a, const Operand& b) { return op2(dx::kUlt, a, b); }
  Operand ilt(const Operand& a, const Operand& b) { return op2(dx::kIlt, a, b); }
  Operand bit01(const Operand& mask) { return band(mask, I(1)); }
  Operand nonzero(const Operand& a) { return ne(a, I(0)); }
  Operand ubfe(const Operand& width, const Operand& offset, const Operand& v) {
    return op3(dx::kUbfe, width, offset, v);
  }
  Operand ibfe(const Operand& width, const Operand& offset, const Operand& v) {
    return op3(dx::kIbfe, width, offset, v);
  }
  Operand umul_hi(const Operand& a, const Operand& b) {
    const uint32_t r = fresh();
    emit(dx::kUmul, {D(r), dxbc::null_dst(), a, b});
    return R(r);
  }
  Operand imul_hi(const Operand& a, const Operand& b) {
    const uint32_t r = fresh();
    emit(dx::kImul, {D(r), dxbc::null_dst(), a, b});
    return R(r);
  }
  Operand mul_lo(const Operand& a, const Operand& b) {
    const uint32_t r = fresh();
    emit(dx::kImul, {dxbc::null_dst(), D(r), a, b});
    return R(r);
  }
  // Sum and carry (0/1).
  std::pair<Operand, Operand> addc(const Operand& a, const Operand& b) {
    const uint32_t r = fresh(), c = fresh();
    emit(dx::kUaddc, {D(r), D(c), a, b});
    return {R(r), R(c)};
  }
  // Difference and borrow (0/1).
  std::pair<Operand, Operand> subb(const Operand& a, const Operand& b) {
    const uint32_t r = fresh(), c = fresh();
    emit(dx::kUsubb, {D(r), D(c), a, b});
    return {R(r), R(c)};
  }
  void if_nz(const Operand& c) { emit(dx::kIf, {c}, dxbc::kTestNonZero); }
  void end_if() { emit(dx::kEndif, {}); }

  // ---- register access ----
  void use_vgpr(uint16_t code, int count = 1) {
    if (code >= rdna::kVgpr0) {
      const uint32_t last = code - rdna::kVgpr0 + static_cast<uint32_t>(count);
      if (last > vgprs_) vgprs_ = last;
    }
  }
  uint32_t vreg(uint32_t index) {
    use_vgpr(static_cast<uint16_t>(rdna::kVgpr0 + index));
    return kVgprBase + index;
  }

  // 32-bit source.
  Operand src(const Instruction& inst, uint16_t code) {
    if (code <= 105) return R(code);
    if (code >= rdna::kVgpr0) return R(vreg(code - rdna::kVgpr0));
    switch (code) {
      case rdna::kVccLo: return R(kVccLoR);
      case rdna::kVccHi: return R(kVccHiR);
      case rdna::kNull: return I(0);
      case rdna::kM0: return R(kM0R);
      case rdna::kExecLo: return R(kExecLoR);
      case rdna::kExecHi: return R(kExecHiR);
      case rdna::kScc: return R(kSccR);
      case rdna::kLiteral: return I(inst.literal);
      case 240: return I(0x3f000000u);
      case 241: return I(0xbf000000u);
      case 242: return I(0x3f800000u);
      case 243: return I(0xbf800000u);
      case 244: return I(0x40000000u);
      case 245: return I(0xc0000000u);
      case 246: return I(0x40800000u);
      case 247: return I(0xc0800000u);
      case 248: return I(0x3e22f983u);
      case 251: return bit01(eq(bor(R(kVccLoR), R(kVccHiR)), I(0)));
      case 252: return bit01(eq(bor(R(kExecLoR), R(kExecHiR)), I(0)));
      default: break;
    }
    if (code >= 128 && code <= 192) return I(code - 128u);
    if (code >= 193 && code <= 208) return I(static_cast<uint32_t>(-static_cast<int>(code - 192)));
    fail(format("unsupported operand %u", code));
    return I(0);
  }

  // 64-bit source.
  void src64(const Instruction& inst, uint16_t code, Operand* lo, Operand* hi) {
    if (code <= 104) {
      *lo = R(code);
      *hi = R(code + 1u);
    } else if (code >= rdna::kVgpr0) {
      *lo = R(vreg(code - rdna::kVgpr0));
      *hi = R(vreg(code - rdna::kVgpr0 + 1u));
    } else if (code == rdna::kVccLo) {
      *lo = R(kVccLoR);
      *hi = R(kVccHiR);
    } else if (code == rdna::kExecLo) {
      *lo = R(kExecLoR);
      *hi = R(kExecHiR);
    } else if (code >= 128 && code <= 208) {
      *lo = src(inst, code);
      *hi = I(code >= 193 ? 0xFFFFFFFFu : 0u);
    } else if (code == rdna::kLiteral) {
      *lo = I(inst.literal);
      *hi = I(0);
    } else if (code == rdna::kNull) {
      *lo = I(0);
      *hi = I(0);
    } else {
      fail(format("unsupported 64-bit operand %u", code));
      *lo = *hi = I(0);
    }
  }

  // Scalar destination register; kNoRegister for null.
  uint32_t sreg(uint16_t code) {
    if (code <= 105) return code;
    switch (code) {
      case rdna::kVccLo: return kVccLoR;
      case rdna::kVccHi: return kVccHiR;
      case rdna::kM0: return kM0R;
      case rdna::kExecLo: return kExecLoR;
      case rdna::kExecHi: return kExecHiR;
      case rdna::kNull: return kNoRegister;
      default: fail(format("unsupported scalar destination %u", code)); return kNoRegister;
    }
  }
  void set_s(uint16_t code, const Operand& value) {
    const uint32_t r = sreg(code);
    if (r != kNoRegister) put(r, value);
  }
  void set_s64(uint16_t code, const Operand& lo, const Operand& hi) {
    if (code == rdna::kNull) return;
    uint32_t r0, r1;
    if (code == rdna::kVccLo) {
      r0 = kVccLoR, r1 = kVccHiR;
    } else if (code == rdna::kExecLo) {
      r0 = kExecLoR, r1 = kExecHiR;
    } else if (code <= 104) {
      r0 = code, r1 = code + 1u;
    } else {
      fail(format("unsupported 64-bit destination %u", code));
      return;
    }
    // Both halves may read the destination: copy first.
    const Operand a = copy(lo), b = copy(hi);
    put(r0, a);
    put(r1, b);
  }
  void set_scc(const Operand& mask) { put(kSccR, bit01(mask)); }
  void set_scc01(const Operand& value) { put(kSccR, value); }
  void scc_nonzero(const Operand& value) { set_scc(nonzero(value)); }
  // A lane mask result from a condition mask: this lane's bit.
  void set_mask(uint16_t code, const Operand& condition) {
    const Operand bit = bit01(condition);
    if (info_.wave_size == 32 && code != rdna::kVccLo && code != rdna::kExecLo) {
      set_s(code, bit);
      return;
    }
    set_s64(code, bit, I(0));
  }
  Operand mask_bit(const Instruction& inst, uint16_t code) { return bit01(src(inst, code)); }
  Operand active() { return bit01(R(kExecLoR)); }

  // 64-bit helpers.
  std::pair<Operand, Operand> add64(const Operand& lo, const Operand& hi, const Operand& value, bool is_signed) {
    auto [r0, carry] = addc(lo, value);
    Operand r1 = add(hi, carry);
    if (is_signed) r1 = add(r1, sar(in_temp(value), I(31)));
    return {r0, r1};
  }
  // kind 0: shift left, 1: logical right, 2: arithmetic right; n in 0..63.
  std::pair<Operand, Operand> shift64(int kind, const Operand& lo, const Operand& hi, const Operand& count) {
    const Operand n = band(count, I(63));
    const Operand big = op2(dx::kUge, n, I(32));
    const Operand zero = eq(n, I(0));
    const Operand back = sub(I(32), n);  // 32 - n (only used when 0 < n < 32)
    Operand r0, r1;
    if (kind == 0) {
      const Operand small_hi = select(zero, hi, bor(shl(hi, n), shr(lo, back)));
      r0 = select(big, I(0), shl(lo, n));
      r1 = select(big, shl(lo, n), small_hi);  // ishl masks the count to n - 32
    } else {
      const Operand small_lo = select(zero, lo, bor(shr(lo, n), shl(hi, back)));
      const Operand high = kind == 1 ? shr(hi, n) : sar(hi, n);
      const Operand fill = kind == 1 ? I(0) : sar(hi, I(31));
      r0 = select(big, high, small_lo);
      r1 = select(big, fill, high);
    }
    return {r0, r1};
  }

  // ---- memory ----
  // Resolves a title GPU address to (view, offset) through the region table.
  std::pair<uint32_t, uint32_t> find(const Operand& lo, const Operand& hi) {
    const uint32_t view = fresh(), offset = fresh(), i = fresh();
    put(view, I(kNoView));
    put(offset, I(0));
    put(i, I(0));
    const Operand alo = in_temp(lo), ahi = in_temp(hi);
    emit(dx::kLoop, {});
    const uint32_t t = fresh(), d = fresh(), dh = fresh(), u = fresh();
    emit(dx::kUge, {D(t), R(i), dxbc::cb(0, kSystemCountsVector, 0)});
    emit(dx::kBreakc, {R(t)}, dxbc::kTestNonZero);
    emit(dx::kIadd, {D(d), alo, dxbc::neg(dxbc::cb_indexed(0, i, kSystemRegionVector, 0))});
    emit(dx::kUlt, {D(t), alo, dxbc::cb_indexed(0, i, kSystemRegionVector, 0)});  // borrow mask
    emit(dx::kIadd, {D(dh), ahi, dxbc::neg(dxbc::cb_indexed(0, i, kSystemRegionVector, 1))});
    emit(dx::kIadd, {D(dh), R(dh), R(t)});
    emit(dx::kIeq, {D(dh), R(dh), I(0)});
    emit(dx::kUlt, {D(u), R(d), dxbc::cb_indexed(0, i, kSystemRegionVector, 2)});
    emit(dx::kAnd, {D(u), R(u), R(dh)});
    if_nz(R(u));
    put(offset, R(d));
    put(view, dxbc::cb_indexed(0, i, kSystemRegionVector, 3));
    emit(dx::kBreak, {});
    end_if();
    emit(dx::kIadd, {D(i), R(i), I(1)});
    emit(dx::kEndloop, {});
    return {view, offset};
  }
  // Reads the dword at view/offset (any alignment); 0 when unresolved.
  Operand read(uint32_t view, const Operand& offset) {
    const uint32_t value = fresh();
    put(value, I(0));
    if_nz(ne(R(view), I(kNoView)));
    const Operand o = in_temp(offset);
    const Operand aligned = band(o, I(~3u));
    const Operand shift = shl(band(o, I(3)), I(3));
    const uint32_t w = fresh();
    emit(dx::kLdRaw, {D(w), aligned, dxbc::uav_indexed(0, view, dxbc::Access::Load)});
    if_nz(shift);
    const uint32_t w2 = fresh();
    emit(dx::kLdRaw, {D(w2), add(aligned, I(4)), dxbc::uav_indexed(0, view, dxbc::Access::Load)});
    emit(dx::kUshr, {D(w), R(w), shift});
    emit(dx::kIshl, {D(w2), R(w2), sub(I(32), shift)});
    emit(dx::kOr, {D(w), R(w), R(w2)});
    end_if();
    put(value, R(w));
    end_if();
    return R(value);
  }
  // Reads into a register, releasing the temporaries used on the way.
  void read_to(uint32_t dest, uint32_t view, uint32_t at, uint32_t byte_offset) {
    const uint32_t mark = scratch_;
    put(dest, read(view, byte_offset == 0 ? R(at) : add(R(at), I(byte_offset))));
    scratch_ = mark;
  }
  void write(uint32_t view, const Operand& offset, const Operand& value) {
    if_nz(ne(R(view), I(kNoView)));
    emit(dx::kStoreRaw, {dxbc::uav_indexed(0, view, dxbc::Access::Store), band(offset, I(~3u)), value});
    end_if();
  }
  // Byte or halfword store inside the containing dword.
  void write_part(uint32_t view, const Operand& offset, const Operand& value, uint32_t bytes) {
    if_nz(ne(R(view), I(kNoView)));
    const Operand aligned = band(offset, I(~3u));
    const Operand shift = shl(band(offset, I(3)), I(3));
    const Operand mask = shl(I(bytes == 1 ? 0xFFu : 0xFFFFu), shift);
    const uint32_t ignored = fresh();
    emit(dx::kImmAtomicAnd, {D(ignored), dxbc::uav_indexed(0, view, dxbc::Access::Atomic), aligned, bnot(mask)});
    emit(dx::kImmAtomicOr,
         {D(ignored), dxbc::uav_indexed(0, view, dxbc::Access::Atomic), aligned, band(shl(value, shift), mask)});
    end_if();
  }
  // Atomic kinds: 0 swap, 1 cmpswap, 2 add, 3 sub, 4 smin, 5 umin, 6 smax,
  // 7 umax, 8 and, 9 or, 10 xor. Returns the previous value.
  Operand atomic(int kind, uint32_t view, const Operand& offset, const Operand& value, const Operand& compare) {
    const uint32_t old = fresh();
    put(old, I(0));
    if_nz(ne(R(view), I(kNoView)));
    const Operand uav = dxbc::uav_indexed(0, view, dxbc::Access::Atomic);
    const Operand address = band(offset, I(~3u));
    static const uint32_t kOps[] = {dx::kImmAtomicExch, dx::kImmAtomicCmpExch, dx::kImmAtomicIadd,
                                    dx::kImmAtomicIadd, dx::kImmAtomicImin,    dx::kImmAtomicUmin,
                                    dx::kImmAtomicImax, dx::kImmAtomicUmax,    dx::kImmAtomicAnd,
                                    dx::kImmAtomicOr,   dx::kImmAtomicXor};
    if (kind == 1) {
      emit(dx::kImmAtomicCmpExch, {D(old), uav, address, compare, value});
    } else if (kind == 3) {
      emit(dx::kImmAtomicIadd, {D(old), uav, address, op1(dx::kIneg, value)});
    } else {
      emit(kOps[kind], {D(old), uav, address, value});
    }
    end_if();
    return R(old);
  }

  // ---- float helpers (VOP3 modifiers) ----
  Operand fsrc(const Instruction& inst, int index, uint16_t code) {
    Operand value = src(inst, code);
    if (inst.encoding == Encoding::VOP3 && (inst.abs & (1 << index) || inst.neg & (1 << index))) {
      value = in_temp(value);
      if (inst.abs & (1 << index)) value = dxbc::abs(value);
      if (inst.neg & (1 << index)) value = dxbc::neg(value);
    }
    return value;
  }
  Operand fresult(const Instruction& inst, Operand value) {
    if (inst.encoding == Encoding::VOP3) {
      if (inst.omod == 1) value = op2(dx::kMul, value, dxbc::immf(2.0f));
      if (inst.omod == 2) value = op2(dx::kMul, value, dxbc::immf(4.0f));
      if (inst.omod == 3) value = op2(dx::kMul, value, dxbc::immf(0.5f));
      if (inst.clamp) value = op1(dx::kMov, value, dxbc::kSaturate);
    }
    return value;
  }

  bool emit(const Instruction& inst);
  bool emit_sop2(const Instruction& inst);
  bool emit_sopk(const Instruction& inst);
  bool emit_sop1(const Instruction& inst);
  bool emit_sopc(const Instruction& inst);
  bool emit_smem(const Instruction& inst);
  bool emit_vector(const Instruction& inst);
  bool emit_flat(const Instruction& inst);
  bool emit_mubuf(const Instruction& inst);
  bool emit_ds(const Instruction& inst);

  const uint8_t* code_;
  size_t size_;
  ComputeShaderInfo info_;
  std::vector<Instruction> program_;
  dxbc::Program p_;
  std::string error_;
  uint32_t scratch_ = 0;
  uint32_t vgprs_ = 3;
  bool barrier_ = false;
  bool lds_ = false;
};

bool Translator::emit_sop2(const Instruction& inst) {
  const Operand a = src(inst, inst.src[0]);
  const Operand b = src(inst, inst.src[1]);
  const uint16_t d = inst.dst;
  // kind: 0 and, 1 or, 2 xor, 3 andn2, 4 orn2, 5 nand, 6 nor, 7 xnor
  auto bitwise32 = [&](int kind, const Operand& x, const Operand& y) {
    switch (kind) {
      case 0: return band(x, y);
      case 1: return bor(x, y);
      case 2: return bxor(x, y);
      case 3: return band(x, bnot(y));
      case 4: return bor(x, bnot(y));
      case 5: return bnot(band(x, y));
      case 6: return bnot(bor(x, y));
      default: return bnot(bxor(x, y));
    }
  };
  auto bitwise = [&](int kind) {
    if (inst.op % 2 == 1) {
      Operand alo, ahi, blo, bhi;
      src64(inst, inst.src[0], &alo, &ahi);
      src64(inst, inst.src[1], &blo, &bhi);
      const Operand r0 = bitwise32(kind, alo, blo), r1 = bitwise32(kind, ahi, bhi);
      scc_nonzero(bor(r0, r1));
      set_s64(d, r0, r1);
    } else {
      const Operand r = bitwise32(kind, a, b);
      scc_nonzero(r);
      set_s(d, r);
    }
    return true;
  };
  switch (inst.op) {
    case 0: {
      auto [r, c] = addc(a, b);
      set_scc01(c);
      set_s(d, r);
      return true;
    }
    case 1: {
      auto [r, c] = subb(a, b);
      set_scc01(c);
      set_s(d, r);
      return true;
    }
    case 2: {
      const Operand r = add(a, b);
      set_scc01(shr(band(bxor(a, r), bxor(b, r)), I(31)));
      set_s(d, r);
      return true;
    }
    case 3: {
      const Operand r = sub(a, b);
      set_scc01(shr(band(bxor(a, b), bxor(a, r)), I(31)));
      set_s(d, r);
      return true;
    }
    case 4: {
      auto [t, c1] = addc(a, b);
      auto [r, c2] = addc(t, R(kSccR));
      set_scc01(bor(c1, c2));
      set_s(d, r);
      return true;
    }
    case 5: {
      auto [t, b1] = subb(a, b);
      auto [r, b2] = subb(t, R(kSccR));
      set_scc01(bor(b1, b2));
      set_s(d, r);
      return true;
    }
    case 6: set_scc(ilt(a, b)); set_s(d, op2(dx::kImin, a, b)); return true;
    case 7: set_scc(ult(a, b)); set_s(d, op2(dx::kUmin, a, b)); return true;
    case 8: set_scc(ilt(b, a)); set_s(d, op2(dx::kImax, a, b)); return true;
    case 9: set_scc(ult(b, a)); set_s(d, op2(dx::kUmax, a, b)); return true;
    case 10: set_s(d, select(R(kSccR), a, b)); return true;
    case 11: {
      Operand alo, ahi, blo, bhi;
      src64(inst, inst.src[0], &alo, &ahi);
      src64(inst, inst.src[1], &blo, &bhi);
      set_s64(d, select(R(kSccR), alo, blo), select(R(kSccR), ahi, bhi));
      return true;
    }
    case 14: case 15: return bitwise(0);
    case 16: case 17: return bitwise(1);
    case 18: case 19: return bitwise(2);
    case 20: case 21: return bitwise(3);
    case 22: case 23: return bitwise(4);
    case 24: case 25: return bitwise(5);
    case 26: case 27: return bitwise(6);
    case 28: case 29: return bitwise(7);
    case 30: { const Operand r = shl(a, b); scc_nonzero(r); set_s(d, r); return true; }
    case 32: { const Operand r = shr(a, b); scc_nonzero(r); set_s(d, r); return true; }
    case 34: { const Operand r = sar(a, b); scc_nonzero(r); set_s(d, r); return true; }
    case 31: case 33: case 35: {
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      auto [r0, r1] = shift64(inst.op == 31 ? 0 : inst.op == 33 ? 1 : 2, lo, hi, b);
      scc_nonzero(bor(r0, r1));
      set_s64(d, r0, r1);
      return true;
    }
    case 36: set_s(d, shl(add(shl(I(1), a), I(0xFFFFFFFFu)), b)); return true;
    case 38: set_s(d, mul_lo(a, b)); return true;
    case 39: { const Operand r = ubfe(band(shr(b, I(16)), I(127)), band(b, I(31)), a); scc_nonzero(r); set_s(d, r); return true; }
    case 40: { const Operand r = ibfe(band(shr(b, I(16)), I(127)), band(b, I(31)), a); scc_nonzero(r); set_s(d, r); return true; }
    case 44: {
      const Operand t = sub(a, b);
      const Operand r = op2(dx::kImax, t, op1(dx::kIneg, t));
      scc_nonzero(r);
      set_s(d, r);
      return true;
    }
    case 46: case 47: case 48: case 49: {
      auto [r, c] = addc(shl(a, I(inst.op - 45u)), b);
      set_scc01(c);
      set_s(d, r);
      return true;
    }
    case 50: set_s(d, op4(dx::kBfi, I(16), I(16), b, a)); return true;
    case 51: set_s(d, bor(band(a, I(0xFFFF)), band(b, I(0xFFFF0000u)))); return true;
    case 52: set_s(d, bor(shr(a, I(16)), band(b, I(0xFFFF0000u)))); return true;
    case 53: set_s(d, umul_hi(a, b)); return true;
    case 54: set_s(d, imul_hi(a, b)); return true;
    default: return fail(format("SOP2 op %u", inst.op));
  }
}

bool Translator::emit_sopk(const Instruction& inst) {
  const uint16_t d = inst.dst;
  const Operand simm = I(static_cast<uint32_t>(inst.imm));
  const Operand uimm = I(static_cast<uint32_t>(inst.imm) & 0xFFFF);
  const uint32_t reg_index = sreg(d);
  if (reg_index == kNoRegister && inst.op != 18) return fail("SOPK without a register");
  const Operand reg = R(reg_index == kNoRegister ? 0 : reg_index);
  switch (inst.op) {
    case 0: put(reg_index, simm); return true;
    case 2: put(reg_index, select(R(kSccR), simm, reg)); return true;
    case 3: set_scc(eq(reg, simm)); return true;
    case 4: set_scc(ne(reg, simm)); return true;
    case 5: set_scc(ilt(simm, reg)); return true;
    case 6: set_scc(op2(dx::kIge, reg, simm)); return true;
    case 7: set_scc(ilt(reg, simm)); return true;
    case 8: set_scc(op2(dx::kIge, simm, reg)); return true;
    case 9: set_scc(eq(reg, uimm)); return true;
    case 10: set_scc(ne(reg, uimm)); return true;
    case 11: set_scc(ult(uimm, reg)); return true;
    case 12: set_scc(op2(dx::kUge, reg, uimm)); return true;
    case 13: set_scc(ult(reg, uimm)); return true;
    case 14: set_scc(op2(dx::kUge, uimm, reg)); return true;
    case 15: {
      const Operand r = add(reg, simm);
      set_scc01(shr(band(bxor(reg, r), bxor(simm, r)), I(31)));
      put(reg_index, r);
      return true;
    }
    case 16: put(reg_index, mul_lo(reg, simm)); return true;
    case 18: set_s(d, I(0)); return true;              // s_getreg_b32: hardware state reads as zero
    case 19: case 21: return true;                     // s_setreg: rounding/denormal modes, not modelled
    case 23: case 24: case 25: case 26: return true;   // waits
    default: return fail(format("SOPK op %u", inst.op));
  }
}

bool Translator::emit_sop1(const Instruction& inst) {
  const uint16_t d = inst.dst;
  const Operand a = src(inst, inst.src[0]);
  auto saveexec = [&](int kind) {
    // dst = exec; exec = f(src, exec); scc = exec != 0
    Operand slo, shi;
    src64(inst, inst.src[0], &slo, &shi);
    const Operand e0 = copy(R(kExecLoR)), e1 = copy(R(kExecHiR));
    auto f = [&](const Operand& s, const Operand& e) {
      switch (kind) {
        case 0: return band(s, e);
        case 1: return bor(s, e);
        case 2: return bxor(s, e);
        case 3: return band(s, bnot(e));
        case 4: return bor(s, bnot(e));
        case 5: return band(bnot(s), e);
        default: return bor(bnot(s), e);
      }
    };
    const Operand lo = f(slo, e0);
    const Operand hi = info_.wave_size == 32 ? I(0) : f(shi, e1);
    put(kExecLoR, lo);
    put(kExecHiR, hi);
    scc_nonzero(bor(R(kExecLoR), R(kExecHiR)));
    set_s64(d, e0, e1);
    return true;
  };
  auto saveexec32 = [&](int kind) {
    const Operand e0 = copy(R(kExecLoR));
    const Operand s = copy(a);
    Operand r;
    switch (kind) {
      case 0: r = band(s, e0); break;
      case 1: r = bor(s, e0); break;
      case 2: r = bxor(s, e0); break;
      case 3: r = band(s, bnot(e0)); break;
      case 4: r = bor(s, bnot(e0)); break;
      default: r = band(bnot(s), e0); break;
    }
    put(kExecLoR, r);
    scc_nonzero(R(kExecLoR));
    set_s(d, e0);
    return true;
  };
  switch (inst.op) {
    case 3: set_s(d, a); return true;
    case 4: {
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      set_s64(d, lo, hi);
      return true;
    }
    case 5: {
      const uint32_t r = sreg(d);
      if (r != kNoRegister) put(r, select(R(kSccR), a, R(r)));
      return true;
    }
    case 6: {
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      if (d > 104 && d != rdna::kVccLo && d != rdna::kExecLo) return fail("s_cmov_b64 destination");
      const uint32_t r0 = sreg(d), r1 = r0 + 1;
      set_s64(d, select(R(kSccR), lo, R(r0)), select(R(kSccR), hi, R(d == rdna::kVccLo ? kVccHiR : d == rdna::kExecLo ? kExecHiR : r1)));
      return true;
    }
    case 7: { const Operand r = bnot(a); scc_nonzero(r); set_s(d, r); return true; }
    case 8: {
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      const Operand r0 = bnot(lo), r1 = bnot(hi);
      scc_nonzero(bor(r0, r1));
      set_s64(d, r0, r1);
      return true;
    }
    case 11: set_s(d, op1(dx::kBfrev, a)); return true;
    case 15: { const Operand r = op1(dx::kCountbits, a); scc_nonzero(r); set_s(d, r); return true; }
    case 19: set_s(d, op1(dx::kFirstbitLo, a)); return true;
    case 21: set_s(d, op1(dx::kFirstbitHi, a)); return true;
    case 25: set_s(d, ibfe(I(8), I(0), a)); return true;
    case 26: set_s(d, ibfe(I(16), I(0), a)); return true;
    case 27: case 29: {
      const uint32_t r = sreg(d);
      if (r == kNoRegister) return true;
      const Operand bit = shl(I(1), a);
      put(r, inst.op == 27 ? band(R(r), bnot(bit)) : bor(R(r), bit));
      return true;
    }
    case 36: return saveexec(0);
    case 37: return saveexec(1);
    case 38: return saveexec(2);
    case 39: return saveexec(3);
    case 40: return saveexec(4);
    case 55: return saveexec(5);
    case 56: return saveexec(6);
    case 60: return saveexec32(0);
    case 61: return saveexec32(1);
    case 62: return saveexec32(2);
    case 63: return saveexec32(3);
    case 64: return saveexec32(4);
    case 68: return saveexec32(5);
    case 52: {
      const Operand r = op2(dx::kImax, a, op1(dx::kIneg, a));
      scc_nonzero(r);
      set_s(d, r);
      return true;
    }
    default: return fail(format("SOP1 op %u", inst.op));
  }
}

bool Translator::emit_sopc(const Instruction& inst) {
  const Operand a = src(inst, inst.src[0]);
  const Operand b = src(inst, inst.src[1]);
  auto compare = [&](int index, bool is_signed) {
    // eq, ne, gt, ge, lt, le
    const uint32_t lt = is_signed ? dx::kIlt : dx::kUlt;
    const uint32_t ge = is_signed ? dx::kIge : dx::kUge;
    switch (index) {
      case 0: return eq(a, b);
      case 1: return ne(a, b);
      case 2: return op2(lt, b, a);
      case 3: return op2(ge, a, b);
      case 4: return op2(lt, a, b);
      default: return op2(ge, b, a);
    }
  };
  if (inst.op <= 5) return set_scc(compare(inst.op, true)), true;
  if (inst.op <= 11) return set_scc(compare(inst.op - 6, false)), true;
  switch (inst.op) {
    case 12: set_scc01(bxor(band(shr(a, b), I(1)), I(1))); return true;
    case 13: set_scc01(band(shr(a, b), I(1))); return true;
    case 18: case 19: {
      Operand alo, ahi, blo, bhi;
      src64(inst, inst.src[0], &alo, &ahi);
      src64(inst, inst.src[1], &blo, &bhi);
      const Operand same = band(eq(alo, blo), eq(ahi, bhi));
      set_scc(inst.op == 18 ? same : bnot(same));
      return true;
    }
    default: return fail(format("SOPC op %u", inst.op));
  }
}

bool Translator::emit_smem(const Instruction& inst) {
  static const int kCounts[] = {1, 2, 4, 8, 16};
  const uint16_t base = inst.src[0];
  // Offset: immediate plus an SGPR unless the field names null.
  Operand offset = I(static_cast<uint32_t>(inst.imm));
  if (inst.src[1] != rdna::kM0 && inst.src[1] != rdna::kNull) offset = add(offset, src(inst, inst.src[1]));
  if (inst.op <= 4) {
    const int count = kCounts[inst.op];
    auto [lo, hi] = add64(R(base), band(R(base + 1u), I(0xFFFF)), offset, true);
    auto [view, at] = find(lo, hi);
    for (int i = 0; i < count; ++i) read_to(inst.dst + static_cast<uint32_t>(i), view, at, 4u * i);
    return true;
  }
  if (inst.op >= 8 && inst.op <= 12) {
    // s_buffer_load: buffer descriptor in s[base..base+3] (address, stride, records).
    const int count = kCounts[inst.op - 8];
    const Operand o = copy(offset);
    auto [lo, hi] = add64(R(base), band(R(base + 1u), I(0xFFFF)), o, false);
    const Operand records = copy(R(base + 2u));
    auto [view, at] = find(lo, hi);
    for (int i = 0; i < count; ++i) {
      const uint32_t dest = inst.dst + static_cast<uint32_t>(i);
      read_to(dest, view, at, 4u * i);
      const uint32_t mark = scratch_;
      put(dest, select(op2(dx::kUge, records, add(o, I(4u * i + 4u))), R(dest), I(0)));
      scratch_ = mark;
    }
    return true;
  }
  if (inst.op == 31 || inst.op == 32) return true;  // cache invalidation
  if (inst.op == 36 || inst.op == 37) {             // s_memtime / s_memrealtime
    set_s64(inst.dst, I(0), I(0));
    return true;
  }
  return fail(format("SMEM op %u", inst.op));
}

bool Translator::emit_flat(const Instruction& inst) {
  // Address: 64-bit VGPR pair, or SGPR pair base plus 32-bit VGPR offset.
  const uint16_t vaddr = inst.src[0];
  const bool saddr = inst.src[2] != 0x7D && inst.src[2] != rdna::kNull;
  use_vgpr(vaddr, saddr ? 1 : 2);
  const uint32_t data = inst.src[1] - rdna::kVgpr0;
  const uint32_t dst = inst.dst;
  if_nz(active());
  Operand lo, hi;
  if (saddr) {
    auto [l, h] = add64(R(inst.src[2]), R(inst.src[2] + 1u), R(vreg(vaddr - rdna::kVgpr0)), false);
    std::tie(lo, hi) = add64(l, h, I(static_cast<uint32_t>(inst.imm)), true);
  } else {
    std::tie(lo, hi) = add64(R(vreg(vaddr - rdna::kVgpr0)), R(vreg(vaddr - rdna::kVgpr0 + 1u)),
                             I(static_cast<uint32_t>(inst.imm)), true);
  }
  const auto location = find(lo, hi);
  const uint32_t view = location.first, at = location.second;
  auto load = [&](int count) {
    for (int i = 0; i < count; ++i) read_to(vreg(dst + static_cast<uint32_t>(i)), view, at, 4u * i);
  };
  auto store = [&](int count) {
    use_vgpr(inst.src[1], count);
    for (int i = 0; i < count; ++i) {
      const uint32_t mark = scratch_;
      write(view, add(R(at), I(4u * i)), R(vreg(data + static_cast<uint32_t>(i))));
      scratch_ = mark;
    }
  };
  bool ok = true;
  switch (inst.op) {
    case 8: put(vreg(dst), band(read(view, R(at)), I(0xFF))); break;
    case 9: put(vreg(dst), ibfe(I(8), I(0), read(view, R(at)))); break;
    case 10: put(vreg(dst), band(read(view, R(at)), I(0xFFFF))); break;
    case 11: put(vreg(dst), ibfe(I(16), I(0), read(view, R(at)))); break;
    case 12: load(1); break;
    case 13: load(2); break;
    case 14: load(4); break;
    case 15: load(3); break;
    case 24: write_part(view, R(at), R(vreg(data)), 1); break;
    case 26: write_part(view, R(at), R(vreg(data)), 2); break;
    case 28: store(1); break;
    case 29: store(2); break;
    case 30: store(4); break;
    case 31: store(3); break;
    default: {
      // Atomics: 48 swap, 49 cmpswap, 50 add, 51 sub, 53 smin, 54 umin,
      // 55 smax, 56 umax, 57 and, 58 or, 59 xor.
      static const int kAtomic[] = {0, 1, 2, 3, -1, 4, 5, 6, 7, 8, 9, 10};
      if (inst.op >= 48 && inst.op <= 59 && kAtomic[inst.op - 48] >= 0) {
        const int kind = kAtomic[inst.op - 48];
        use_vgpr(inst.src[1], kind == 1 ? 2 : 1);
        const Operand value = R(vreg(kind == 1 ? data + 1 : data));
        const Operand compare = kind == 1 ? R(vreg(data)) : I(0);
        const Operand old = atomic(kind, view, R(at), value, compare);
        if (inst.glc) put(vreg(dst), old);
      } else {
        ok = fail(format("FLAT op %u", inst.op));
      }
    }
  }
  end_if();
  return ok;
}

bool Translator::emit_mubuf(const Instruction& inst) {
  const uint32_t r = inst.src[2];
  const uint32_t vaddr = inst.src[0] - rdna::kVgpr0;
  const uint32_t data = inst.src[1] - rdna::kVgpr0;
  Operand offset = I(static_cast<uint32_t>(inst.imm));
  if (inst.src[3] != rdna::kNull && inst.src[3] != rdna::kM0 && inst.src[3] != 0x80) {
    offset = add(offset, src(inst, inst.src[3]));
  }
  Operand index = I(0);
  if (inst.idxen && inst.offen) {
    use_vgpr(inst.src[0], 2);
    index = R(vreg(vaddr));
    offset = add(offset, R(vreg(vaddr + 1)));
  } else if (inst.idxen) {
    index = R(vreg(vaddr));
  } else if (inst.offen) {
    offset = add(offset, R(vreg(vaddr)));
  }
  if_nz(active());
  // Descriptor: base (48 bits), stride (bits 16-29 of word 1), records (word 2).
  const Operand stride = ubfe(I(14), I(16), R(r + 1));
  const Operand inside = select(eq(stride, I(0)), ult(offset, R(r + 2)), ult(index, R(r + 2)));
  const Operand byte_offset = add(mul_lo(index, stride), offset);
  auto [lo, hi] = add64(R(r), band(R(r + 1), I(0xFFFF)), byte_offset, false);
  const auto location = find(lo, hi);
  const uint32_t at = location.second;
  const uint32_t view = fresh();
  put(view, select(inside, R(location.first), I(kNoView)));
  auto load = [&](int count) {
    for (int i = 0; i < count; ++i) read_to(vreg(data + static_cast<uint32_t>(i)), view, at, 4u * i);
  };
  auto store = [&](int count) {
    use_vgpr(inst.src[1], count);
    for (int i = 0; i < count; ++i) {
      const uint32_t mark = scratch_;
      write(view, add(R(at), I(4u * i)), R(vreg(data + static_cast<uint32_t>(i))));
      scratch_ = mark;
    }
  };
  bool ok = true;
  if (inst.encoding == Encoding::MTBUF) {
    // Typed loads: element format from the instruction (RDNA2 buffer format
    // numbers: 1 8_UNORM, 5 8_UINT, 7 16_UNORM, 11 16_UINT, 13 16_FLOAT,
    // 20 32_UINT, 21 32_SINT, 22 32_FLOAT; component count from the opcode).
    const uint32_t components = (inst.op & 3) + 1;
    const uint32_t fmt = inst.ext;
    if (inst.op >= 4) {
      ok = fail("typed buffer store");
    } else if (fmt == 20 || fmt == 21 || fmt == 22) {
      load(static_cast<int>(components));
    } else if (components != 1) {
      ok = fail(format("typed buffer format %u with %u components", fmt, components));
    } else {
      const Operand word = read(view, R(at));
      switch (fmt) {
        case 5: put(vreg(data), band(word, I(0xFF))); break;
        case 1: put(vreg(data), op2(dx::kMul, op1(dx::kUtof, band(word, I(0xFF))), dxbc::immf(1.0f / 255.0f))); break;
        case 11: put(vreg(data), band(word, I(0xFFFF))); break;
        case 7: put(vreg(data), op2(dx::kMul, op1(dx::kUtof, band(word, I(0xFFFF))), dxbc::immf(1.0f / 65535.0f))); break;
        case 13: put(vreg(data), op1(dx::kF16tof32, word)); break;
        default: ok = fail(format("typed buffer format %u", fmt)); break;
      }
    }
    end_if();
    return ok;
  }
  switch (inst.op) {
    case 8: put(vreg(data), band(read(view, R(at)), I(0xFF))); break;
    case 10: put(vreg(data), band(read(view, R(at)), I(0xFFFF))); break;
    case 12: load(1); break;
    case 13: load(2); break;
    case 14: load(4); break;
    case 15: load(3); break;
    case 28: store(1); break;
    case 29: store(2); break;
    case 30: store(4); break;
    case 31: store(3); break;
    case 113: case 114: break;  // cache invalidation
    default: {
      static const int kAtomic[] = {0, 1, 2, 3, -1, 4, 5, 6, 7, 8, 9, 10};
      if (inst.op >= 48 && inst.op <= 59 && kAtomic[inst.op - 48] >= 0) {
        const int kind = kAtomic[inst.op - 48];
        use_vgpr(inst.src[1], kind == 1 ? 2 : 1);
        const Operand value = R(vreg(kind == 1 ? data + 1 : data));
        const Operand compare = kind == 1 ? R(vreg(data)) : I(0);
        const Operand old = atomic(kind, view, R(at), value, compare);
        if (inst.glc) put(vreg(data), old);
      } else {
        ok = fail(format("MUBUF op %u", inst.op));
      }
    }
  }
  end_if();
  return ok;
}

bool Translator::emit_ds(const Instruction& inst) {
  lds_ = true;
  const uint32_t addr = vreg(inst.src[0] - rdna::kVgpr0);
  const uint32_t d0 = inst.src[1] - rdna::kVgpr0;
  const uint32_t d1 = inst.src[2] - rdna::kVgpr0;
  const uint32_t offset0 = inst.imm & 0xFF, offset1 = (inst.imm >> 8) & 0xFF, offset = inst.imm & 0xFFFF;
  if_nz(active());
  auto address = [&](uint32_t byte_offset) { return band(add(R(addr), I(byte_offset)), I(~3u)); };
  auto load = [&](uint32_t at) {
    const uint32_t r = fresh();
    emit(dx::kLdRaw, {D(r), address(at), dxbc::group_shared(0, dxbc::Access::Load)});
    return R(r);
  };
  auto store = [&](uint32_t at, const Operand& value) {
    emit(dx::kStoreRaw, {dxbc::group_shared(0, dxbc::Access::Store), address(at), value});
  };
  auto rmw = [&](uint32_t opcode, bool returns) {
    use_vgpr(inst.src[1]);
    const uint32_t old = fresh();
    emit(opcode, {D(old), dxbc::group_shared(0, dxbc::Access::Atomic), address(offset), R(vreg(d0))});
    if (returns) put(vreg(inst.dst), R(old));
  };
  bool ok = true;
  switch (inst.op) {
    case 13: use_vgpr(inst.src[1]); store(offset, R(vreg(d0))); break;
    case 14: case 15: {
      use_vgpr(inst.src[1]);
      use_vgpr(inst.src[2]);
      const uint32_t scale = inst.op == 14 ? 4 : 256;
      store(offset0 * scale, R(vreg(d0)));
      store(offset1 * scale, R(vreg(d1)));
      break;
    }
    case 54: put(vreg(inst.dst), load(offset)); break;
    case 55: case 56: {
      const uint32_t scale = inst.op == 55 ? 4 : 256;
      const Operand x = load(offset0 * scale), y = load(offset1 * scale);
      put(vreg(inst.dst), x);
      put(vreg(inst.dst + 1u), y);
      break;
    }
    case 77: use_vgpr(inst.src[1], 2); store(offset, R(vreg(d0))); store(offset + 4, R(vreg(d0 + 1))); break;
    case 223:
      use_vgpr(inst.src[1], 4);
      for (uint32_t i = 0; i < 4; ++i) store(offset + 4 * i, R(vreg(d0 + i)));
      break;
    case 118: case 255: {
      const uint32_t count = inst.op == 118 ? 2 : 4;
      std::vector<Operand> values;
      for (uint32_t i = 0; i < count; ++i) values.push_back(load(offset + 4 * i));
      for (uint32_t i = 0; i < count; ++i) put(vreg(inst.dst + i), values[i]);
      break;
    }
    case 0: rmw(dx::kImmAtomicIadd, false); break;
    case 32: rmw(dx::kImmAtomicIadd, true); break;
    case 7: rmw(dx::kImmAtomicUmin, false); break;
    case 8: rmw(dx::kImmAtomicUmax, false); break;
    case 9: rmw(dx::kImmAtomicAnd, false); break;
    case 10: rmw(dx::kImmAtomicOr, false); break;
    default: ok = fail(format("DS op %u", inst.op)); break;
  }
  end_if();
  return ok;
}

bool Translator::emit_vector(const Instruction& inst) {
  if (inst.src[0] == rdna::kDpp16 || inst.src[0] == rdna::kDpp8 || inst.src[0] == rdna::kDpp8Fi) {
    return fail("cross-lane operand (DPP)");
  }
  const bool sdwa = inst.src[0] == rdna::kSdwa;
  Instruction work = inst;
  Operand sdwa_src0, sdwa_src1;
  if (sdwa) {
    // SDWA: real src0 and operand selects live in the extension dword.
    const uint32_t e = inst.ext;
    const uint16_t real = static_cast<uint16_t>((e & 0xFF) + ((e >> 23) & 1 ? 0 : rdna::kVgpr0));
    work.src[0] = real;
    auto pick = [&](const Operand& value, uint32_t sel, bool sext) {
      if (sel >= 6) return value;
      const uint32_t width = sel < 4 ? 8 : 16;
      const uint32_t shift = sel < 4 ? 8 * sel : 16 * (sel - 4);
      return sext ? ibfe(I(width), I(shift), value) : ubfe(I(width), I(shift), value);
    };
    if (((e >> 8) & 7) != 6 && inst.encoding != Encoding::VOPC) return fail("SDWA destination select");
    if ((e >> 20) & 3 || (e >> 28) & 3) return fail("SDWA source modifiers");
    sdwa_src0 = pick(src(work, real), (e >> 16) & 7, (e >> 19) & 1);
    if (inst.encoding != Encoding::VOP1) sdwa_src1 = pick(src(work, work.src[1]), (e >> 24) & 7, (e >> 27) & 1);
  }
  auto s = [&](int i) {
    if (sdwa && i == 0) return sdwa_src0;
    if (sdwa && i == 1) return sdwa_src1;
    return src(work, work.src[i]);
  };
  auto f = [&](int i) {
    if (sdwa && i < 2) return s(i);
    return fsrc(work, i, work.src[i]);
  };

  // Normalise to (kind, op): VOP2/VOP1/VOPC and their VOP3 forms.
  enum Kind { kVop2, kVop1, kVopc, kVop3 } kind;
  uint16_t op = inst.op;
  if (inst.encoding == Encoding::VOP2) kind = kVop2;
  else if (inst.encoding == Encoding::VOP1) kind = kVop1;
  else if (inst.encoding == Encoding::VOPC) kind = kVopc;
  else if (op < 256) kind = kVopc;
  else if (op < 320) { kind = kVop2; op -= 256; }
  else if (op >= 0x180 && op < 0x200) { kind = kVop1; op -= 0x180; }
  else kind = kVop3;
  const bool e64 = inst.encoding == Encoding::VOP3;
  const Operand on = active();
  const bool sgpr_result = kind == kVopc || (kind == kVop1 && op == 2);
  const uint32_t dreg = sgpr_result ? kVgprBase : vreg(inst.dst);
  const Operand dval = R(dreg);
  // Results are written only by active lanes.
  auto set = [&](const Operand& value) {
    put(dreg, select(on, value[0] & 0x80000000u ? copy(value) : value, dval));
    return true;
  };
  auto setf = [&](const Operand& value) { return set(fresult(work, value)); };
  const Operand literal = I(inst.literal);
  auto fadd = [&](const Operand& x, const Operand& y) { return op2(dx::kAdd, x, y); };
  auto fmul = [&](const Operand& x, const Operand& y) { return op2(dx::kMul, x, y); };
  auto fmad = [&](const Operand& x, const Operand& y, const Operand& z) { return op3(dx::kMad, x, y, z); };
  auto fneg = [&](const Operand& x) { return dxbc::neg(in_temp(x)); };

  if (kind == kVopc) {
    // Compare: the mask bit goes to VCC (e32) or the named SGPRs (e64);
    // v_cmpx writes EXEC instead.
    Operand cond;
    const Operand a = s(0), b = s(1);
    const uint16_t base = op & 0xF0;
    const int index = op & 0xF;
    bool exec_write = false;
    auto icmp = [&](bool is_signed) {
      const uint32_t lt = is_signed ? dx::kIlt : dx::kUlt;
      const uint32_t ge = is_signed ? dx::kIge : dx::kUge;
      switch (index) {
        case 0: return I(0);
        case 1: return op2(lt, a, b);
        case 2: return eq(a, b);
        case 3: return op2(ge, b, a);
        case 4: return op2(lt, b, a);
        case 5: return ne(a, b);
        case 6: return op2(ge, a, b);
        default: return I(0xFFFFFFFFu);
      }
    };
    if (op < 0x20) {
      exec_write = op >= 0x10;
      const Operand fa = f(0), fb = f(1);
      auto lt = [&](const Operand& x, const Operand& y) { return op2(dx::kLt, x, y); };
      auto ge = [&](const Operand& x, const Operand& y) { return op2(dx::kGe, x, y); };
      auto ordered = [&]() { return band(op2(dx::kEq, fa, fa), op2(dx::kEq, fb, fb)); };
      auto lg = [&]() { return bor(lt(fa, fb), lt(fb, fa)); };
      switch (index) {
        case 0: cond = I(0); break;
        case 1: cond = lt(fa, fb); break;
        case 2: cond = op2(dx::kEq, fa, fb); break;
        case 3: cond = ge(fb, fa); break;
        case 4: cond = lt(fb, fa); break;
        case 5: cond = lg(); break;
        case 6: cond = ge(fa, fb); break;
        case 7: cond = ordered(); break;
        case 8: cond = bnot(ordered()); break;
        case 9: cond = bnot(ge(fa, fb)); break;
        case 10: cond = bnot(lg()); break;
        case 11: cond = bnot(lt(fb, fa)); break;
        case 12: cond = bnot(ge(fb, fa)); break;
        case 13: cond = op2(dx::kNe, fa, fb); break;
        case 14: cond = bnot(lt(fa, fb)); break;
        default: cond = I(0xFFFFFFFFu); break;
      }
    } else if (base == 0x80 || base == 0x90) {
      exec_write = base == 0x90;
      if (index < 8) {
        cond = icmp(true);
      } else if (index == 8) {
        // v_cmp_class_f32: class bit of src0 selected in the src1 mask.
        const Operand u = in_temp(f(0));
        const Operand sign = nonzero(shr(u, I(31)));
        const Operand e = ubfe(I(8), I(23), u);
        const Operand m = band(u, I(0x7FFFFF));
        const Operand m_zero = eq(m, I(0));
        const Operand e0 = select(m_zero, select(sign, I(5), I(6)), select(sign, I(4), I(7)));
        const Operand e255 = select(m_zero, select(sign, I(2), I(9)), select(band(m, I(0x400000)), I(1), I(0)));
        const Operand bit = select(eq(e, I(0)), e0, select(eq(e, I(255)), e255, select(sign, I(3), I(8))));
        cond = nonzero(band(shr(b, bit), I(1)));
      } else {
        return fail(format("VOPC op 0x%x", op));
      }
    } else if (base == 0xC0 || base == 0xD0) {
      exec_write = base == 0xD0;
      if (index >= 8) return fail(format("VOPC op 0x%x", op));
      cond = icmp(false);
    } else if (base == 0xA0 || base == 0xB0 || base == 0xE0 || base == 0xF0) {
      exec_write = base == 0xB0 || base == 0xF0;
      const bool is_signed = base == 0xA0 || base == 0xB0;
      if (index >= 8) return fail(format("VOPC op 0x%x", op));
      Operand alo, ahi, blo, bhi;
      src64(work, work.src[0], &alo, &ahi);
      src64(work, work.src[1], &blo, &bhi);
      const Operand hi_lt = op2(is_signed ? dx::kIlt : dx::kUlt, ahi, bhi);
      const Operand hi_eq = eq(ahi, bhi);
      const Operand lt = bor(hi_lt, band(hi_eq, ult(alo, blo)));
      const Operand same = band(eq(alo, blo), hi_eq);
      switch (index) {
        case 0: cond = I(0); break;
        case 1: cond = lt; break;
        case 2: cond = same; break;
        case 3: cond = bor(lt, same); break;
        case 4: cond = bnot(bor(lt, same)); break;
        case 5: cond = bnot(same); break;
        case 6: cond = bnot(lt); break;
        default: cond = I(0xFFFFFFFFu); break;
      }
    } else {
      return fail(format("VOPC op 0x%x", op));
    }
    const Operand c = band(on, cond);
    if (exec_write) {
      put(kExecLoR, bit01(c));
      put(kExecHiR, I(0));
      return true;
    }
    set_mask(e64 ? inst.dst : rdna::kVccLo, c);
    return true;
  }

  if (kind == kVop2) {
    const Operand a = s(0), b = s(1);
    switch (op) {
      case 1: return set(select(e64 ? mask_bit(work, work.src[2]) : bit01(R(kVccLoR)), b, a));
      case 3: return setf(fadd(f(0), f(1)));
      case 4: return setf(fadd(f(0), fneg(f(1))));
      case 5: return setf(fadd(f(1), fneg(f(0))));
      case 6: case 7: {
        const Operand fa = f(0), fb = f(1);
        const Operand zero = bor(op2(dx::kEq, fa, dxbc::immf(0.0f)), op2(dx::kEq, fb, dxbc::immf(0.0f)));
        if (op == 6) return setf(select(zero, dval, fmad(fa, fb, dval)));
        return setf(select(zero, dxbc::immf(0.0f), fmul(fa, fb)));
      }
      case 8: return setf(fmul(f(0), f(1)));
      case 9: return set(mul_lo(ibfe(I(24), I(0), a), ibfe(I(24), I(0), b)));
      case 11: return set(mul_lo(band(a, I(0xFFFFFF)), band(b, I(0xFFFFFF))));
      case 12: return set(umul_hi(band(a, I(0xFFFFFF)), band(b, I(0xFFFFFF))));
      case 15: return setf(op2(dx::kMin, f(0), f(1)));
      case 16: return setf(op2(dx::kMax, f(0), f(1)));
      case 17: return set(op2(dx::kImin, a, b));
      case 18: return set(op2(dx::kImax, a, b));
      case 19: return set(op2(dx::kUmin, a, b));
      case 20: return set(op2(dx::kUmax, a, b));
      case 22: return set(shr(b, a));
      case 24: return set(sar(b, a));
      case 26: return set(shl(b, a));
      case 27: return set(band(a, b));
      case 28: return set(bor(a, b));
      case 29: return set(bxor(a, b));
      case 30: return set(bnot(bxor(a, b)));
      case 31: case 43: return setf(fmad(f(0), f(1), dval));
      case 32: case 44: return setf(fmad(f(0), literal, f(1)));
      case 33: case 45: return setf(fmad(f(0), f(1), literal));
      case 37: return set(add(a, b));
      case 38: return set(sub(a, b));
      case 39: return set(sub(b, a));
      case 40: case 41: case 42: {
        const Operand cin = e64 ? mask_bit(work, work.src[2]) : bit01(R(kVccLoR));
        const uint16_t cout = e64 ? inst.sdst : rdna::kVccLo;
        Operand r, c;
        if (op == 40) {
          auto [t, c1] = addc(a, b);
          auto [u, c2] = addc(t, cin);
          r = u, c = bor(c1, c2);
        } else {
          const Operand x = op == 41 ? a : b, y = op == 41 ? b : a;
          auto [t, b1] = subb(x, y);
          auto [u, b2] = subb(t, cin);
          r = u, c = bor(b1, b2);
        }
        set(r);
        set_mask(cout, band(on, nonzero(c)));
        return true;
      }
      case 47: return set(op4(dx::kBfi, I(16), I(16), op1(dx::kF32tof16, f(1)), op1(dx::kF32tof16, f(0))));
      default: return fail(format("VOP2 op %u", op));
    }
  }

  if (kind == kVop1) {
    const Operand a = s(0);
    switch (op) {
      case 0: return true;
      case 1: return set(a);
      case 2:
        // Assumed uniform: the lane's own value (exact when all lanes agree).
        set_s(inst.dst, a);
        return true;
      case 5: return setf(op1(dx::kItof, a));
      case 6: return setf(op1(dx::kUtof, a));
      case 7: return set(op1(dx::kFtou, f(0)));
      case 8: return set(op1(dx::kFtoi, f(0)));
      case 10: return set(op1(dx::kF32tof16, f(0)));
      case 11: return setf(op1(dx::kF16tof32, a));
      case 12: return set(op1(dx::kFtoi, op1(dx::kRoundNi, fadd(f(0), dxbc::immf(0.5f)))));
      case 13: return set(op1(dx::kFtoi, op1(dx::kRoundNi, f(0))));
      case 17: case 18: case 19: case 20: return setf(op1(dx::kUtof, ubfe(I(8), I(8u * (op - 17u)), a)));
      case 32: return setf(op1(dx::kFrc, f(0)));
      case 33: return setf(op1(dx::kRoundZ, f(0)));
      case 34: return setf(op1(dx::kRoundPi, f(0)));
      case 35: return setf(op1(dx::kRoundNe, f(0)));
      case 36: return setf(op1(dx::kRoundNi, f(0)));
      case 37: return setf(op1(dx::kExp, f(0)));
      case 39: return setf(op1(dx::kLog, f(0)));
      case 42: case 43: return setf(op1(dx::kRcp, f(0)));
      case 46: return setf(op1(dx::kRsq, f(0)));
      case 51: return setf(op1(dx::kSqrt, f(0)));
      case 53: case 54: {
        const uint32_t r = fresh();
        const Operand angle = fmul(f(0), dxbc::immf(6.2831853f));
        if (op == 53) emit(dx::kSincos, {D(r), dxbc::null_dst(), angle});
        else emit(dx::kSincos, {dxbc::null_dst(), D(r), angle});
        return setf(R(r));
      }
      case 55: return set(bnot(a));
      case 56: return set(op1(dx::kBfrev, a));
      case 57: return set(op1(dx::kFirstbitHi, a));
      case 58: return set(op1(dx::kFirstbitLo, a));
      case 59: return set(op1(dx::kFirstbitShi, a));
      case 63: {
        const Operand u = in_temp(f(0));
        const Operand e = sub(ubfe(I(8), I(23), u), I(126));
        return set(select(op2(dx::kEq, u, dxbc::immf(0.0f)), I(0), e));
      }
      case 64: {
        const Operand u = in_temp(f(0));
        const Operand m = bor(band(u, I(0x807FFFFFu)), I(0x3F000000u));
        return setf(select(op2(dx::kEq, u, dxbc::immf(0.0f)), dxbc::immf(0.0f), m));
      }
      default: return fail(format("VOP1 op %u", op));
    }
  }

  // VOP3-only.
  const Operand a = s(0), b = s(1), c = s(2);
  auto med3 = [&](uint32_t mn, uint32_t mx, const Operand& x, const Operand& y, const Operand& z) {
    return op2(mx, op2(mn, x, y), op2(mn, op2(mx, x, y), z));
  };
  switch (op) {
    case 0x141: case 0x14b: case 0x16f: return setf(fmad(f(0), f(1), f(2)));
    case 0x142: return set(add(mul_lo(ibfe(I(24), I(0), a), ibfe(I(24), I(0), b)), c));
    case 0x143: return set(add(mul_lo(band(a, I(0xFFFFFF)), band(b, I(0xFFFFFF))), c));
    case 0x148: return set(ubfe(c, b, a));
    case 0x149: return set(ibfe(c, b, a));
    case 0x14a: return set(bor(band(a, b), band(bnot(a), c)));
    case 0x14e: case 0x14f: {
      const Operand n = op == 0x14e ? band(c, I(31)) : shl(band(c, I(3)), I(3));
      return set(select(eq(n, I(0)), b, bor(shr(b, n), shl(a, sub(I(32), n)))));
    }
    case 0x151: return setf(op2(dx::kMin, op2(dx::kMin, f(0), f(1)), f(2)));
    case 0x152: return set(op2(dx::kImin, op2(dx::kImin, a, b), c));
    case 0x153: return set(op2(dx::kUmin, op2(dx::kUmin, a, b), c));
    case 0x154: return setf(op2(dx::kMax, op2(dx::kMax, f(0), f(1)), f(2)));
    case 0x155: return set(op2(dx::kImax, op2(dx::kImax, a, b), c));
    case 0x156: return set(op2(dx::kUmax, op2(dx::kUmax, a, b), c));
    case 0x157: return setf(med3(dx::kMin, dx::kMax, f(0), f(1), f(2)));
    case 0x158: return set(med3(dx::kImin, dx::kImax, a, b, c));
    case 0x159: return set(med3(dx::kUmin, dx::kUmax, a, b, c));
    case 0x15f: return setf(f(0));  // v_div_fixup: special-case fixups not modelled
    case 0x16d:                      // v_div_scale: no scaling needed without denormal ranges
      set_mask(inst.sdst, I(0));
      return setf(f(0));
    case 0x169: return set(mul_lo(a, b));
    case 0x16a: return set(umul_hi(a, b));
    case 0x16c: return set(imul_hi(a, b));
    case 0x176: case 0x177: {
      Operand clo, chi;
      src64(work, work.src[2], &clo, &chi);
      const uint32_t hi = fresh(), lo = fresh();
      emit(op == 0x176 ? dx::kUmul : dx::kImul, {D(hi), D(lo), a, b});
      auto [r0, carry] = addc(R(lo), clo);
      const Operand r1 = add(add(R(hi), chi), carry);
      const uint32_t d1 = vreg(inst.dst + 1u);
      put(d1, select(on, r1, R(d1)));
      set(r0);
      set_mask(inst.sdst, I(0));
      return true;
    }
    case 0x178: return set(bxor(bxor(a, b), c));
    case 0x2ff: case 0x300: case 0x301: {
      Operand lo, hi;
      src64(work, work.src[1], &lo, &hi);
      auto [r0, r1] = shift64(op == 0x2ff ? 0 : op == 0x300 ? 1 : 2, lo, hi, a);
      const uint32_t d1 = vreg(inst.dst + 1u);
      const Operand keep0 = copy(r0);
      put(d1, select(on, r1, R(d1)));
      return set(keep0);
    }
    case 0x30f: case 0x310: case 0x319: {
      Operand r, carry;
      if (op == 0x30f) std::tie(r, carry) = addc(a, b);
      else if (op == 0x310) std::tie(r, carry) = subb(a, b);
      else std::tie(r, carry) = subb(b, a);
      set(r);
      set_mask(inst.sdst, band(on, nonzero(carry)));
      return true;
    }
    case 0x344: {
      // v_perm_b32: byte selects from {a, b}; selectors >= 8 give constants.
      Operand result = I(0);
      for (uint32_t i = 0; i < 4; ++i) {
        const Operand sel = ubfe(I(8), I(8 * i), c);
        const Operand from_b = ubfe(I(8), shl(sel, I(3)), b);
        const Operand from_a = ubfe(I(8), shl(add(sel, I(0xFFFFFFFCu)), I(3)), a);
        const Operand constant = select(eq(sel, I(13)), I(0xFF), I(0));
        const Operand byte = select(ult(sel, I(4)), from_b, select(ult(sel, I(8)), from_a, constant));
        result = bor(result, shl(byte, I(8 * i)));
      }
      return set(result);
    }
    case 0x345: return set(add(bxor(a, b), c));
    case 0x346: return set(add(shl(a, b), c));
    case 0x347: return set(shl(add(a, b), c));
    case 0x362: return setf(fmul(f(0), op1(dx::kExp, op1(dx::kItof, b))));
    case 0x363: return set(shl(add(shl(I(1), a), I(0xFFFFFFFFu)), b));
    case 0x364: return set(add(op1(dx::kCountbits, a), b));
    case 0x365: case 0x366:
      // Only the lane-index idiom (full mask) is expressible per lane.
      if (work.src[0] != 193) return fail("v_mbcnt with a lane mask");
      if (op == 0x365) return set(add(op2(dx::kUmin, R(kLaneR), I(32)), b));
      return set(add(select(ult(I(32), R(kLaneR)), add(R(kLaneR), I(0xFFFFFFE0u)), I(0)), b));
    case 0x36d: return set(add(add(a, b), c));
    case 0x36f: return set(bor(shl(a, b), c));
    case 0x371: return set(bor(band(a, b), c));
    case 0x372: return set(bor(bor(a, b), c));
    case 0x376: return set(sub(a, b));
    case 0x37f: return set(add(a, b));
    default: return fail(format("VOP3 op 0x%x", op));
  }
}

bool Translator::emit(const Instruction& inst) {
  scratch_ = 0;
  switch (inst.encoding) {
    case Encoding::SOP2: return emit_sop2(inst);
    case Encoding::SOPK: return emit_sopk(inst);
    case Encoding::SOP1: return emit_sop1(inst);
    case Encoding::SOPC: return emit_sopc(inst);
    case Encoding::SMEM: return emit_smem(inst);
    case Encoding::VOP1:
    case Encoding::VOP2:
    case Encoding::VOPC:
    case Encoding::VOP3: return emit_vector(inst);
    case Encoding::FLAT: return emit_flat(inst);
    case Encoding::MUBUF:
    case Encoding::MTBUF: return emit_mubuf(inst);
    case Encoding::DS: return emit_ds(inst);
    default: return fail(rdna::mnemonic(inst));
  }
}

bool Translator::run(std::vector<uint8_t>* out, std::string* error) {
  // Decode everything up to s_code_end.
  for (uint32_t pc = 0; pc + 4 <= size_;) {
    Instruction inst;
    if (!rdna::decode(code_, size_, pc, &inst)) break;
    if (inst.encoding == Encoding::SOPP && inst.op == 31) break;  // s_code_end
    program_.push_back(inst);
    pc += 4 * inst.words;
  }
  if (program_.empty()) {
    *error = "empty program";
    return false;
  }
  if (info_.wave_size == 0) {
    info_.wave_size = 32;
    for (const Instruction& inst : program_) {
      const bool saveexec64 = inst.encoding == Encoding::SOP1 &&
                              ((inst.op >= 36 && inst.op <= 40) || inst.op == 55 || inst.op == 56);
      const bool exec64 = inst.dst == rdna::kExecLo &&
                          ((inst.encoding == Encoding::SOP1 && (inst.op == 4 || inst.op == 8)) ||
                           (inst.encoding == Encoding::SOP2 && inst.op >= 14 && inst.op <= 29 && inst.op % 2 == 1));
      if (saveexec64 || exec64) {
        info_.wave_size = 64;
        break;
      }
    }
  }

  // Block leaders: entry, branch targets, instructions after branches.
  std::set<uint32_t> leaders = {0};
  auto is_branch = [](const Instruction& i) {
    return i.encoding == Encoding::SOPP && (i.op == 2 || (i.op >= 4 && i.op <= 9) || i.op == 1 || i.op == 10);
  };
  for (size_t i = 0; i < program_.size(); ++i) {
    const Instruction& inst = program_[i];
    if (!is_branch(inst)) continue;
    if (inst.op != 1 && inst.op != 10) leaders.insert(static_cast<uint32_t>(inst.pc + 4 + 4 * inst.imm));
    if (i + 1 < program_.size()) leaders.insert(program_[i + 1].pc);
  }

  // Body: switch over block addresses inside the dispatcher loop.
  dxbc::Program& body = p_;
  emit(dx::kSwitch, {R(kNextR)});
  for (size_t i = 0; i < program_.size(); ++i) {
    const Instruction& inst = program_[i];
    scratch_ = 0;
    if (leaders.count(inst.pc)) {
      if (i != 0) {
        if (!is_branch(program_[i - 1])) put(kNextR, I(inst.pc));
        emit(dx::kBreak, {});
      }
      emit(dx::kCase, {I(inst.pc)});
    }
    if (inst.encoding == Encoding::SOPP) {
      const uint32_t target = static_cast<uint32_t>(inst.pc + 4 + 4 * inst.imm);
      const uint32_t next = i + 1 < program_.size() ? program_[i + 1].pc : 0xFFFFFFFFu;
      auto branch = [&](const Operand& taken) { put(kNextR, select(taken, I(target), I(next))); };
      switch (inst.op) {
        case 1: emit(dx::kRet, {}); continue;
        case 2: put(kNextR, I(target)); continue;
        case 4: branch(eq(R(kSccR), I(0))); continue;
        case 5: branch(R(kSccR)); continue;
        case 6: branch(eq(bor(R(kVccLoR), R(kVccHiR)), I(0))); continue;
        case 7: branch(bor(R(kVccLoR), R(kVccHiR))); continue;
        case 8: branch(eq(bor(R(kExecLoR), R(kExecHiR)), I(0))); continue;
        case 9: branch(bor(R(kExecLoR), R(kExecHiR))); continue;
        case 10:
          // Workgroup barrier: leave the dispatcher, synchronise at the top
          // level, come back at the next block.
          barrier_ = true;
          put(kNextR, I(next));
          put(kBarrierR, I(1));
          continue;
        default: continue;  // waits, priorities, messages, prefetch hints
      }
    }
    if (!emit(inst)) {
      *error = format("at 0x%x: %s", inst.pc, error_.c_str());
      return false;
    }
    if (!error_.empty()) {
      *error = format("at 0x%x: %s", inst.pc, error_.c_str());
      return false;
    }
  }
  emit(dx::kRet, {});
  emit(dx::kDefault, {});
  emit(dx::kRet, {});
  emit(dx::kEndswitch, {});

  // Program: declarations, register initialisation, dispatcher loop.
  dxbc::Program program;
  program.dcl_global_flags(1);  // refactoring allowed
  program.dcl_constant_buffer(0, kTranslatedSpace, kSystemVectors, true);
  program.dcl_uav_raw_range(0, 0, kRegionTableSize - 1, kTranslatedSpace);
  program.dcl_input(dxbc::kThreadGroupId, 0x7);
  program.dcl_input(dxbc::kThreadIdInGroup, 0x7);
  program.dcl_input(dxbc::kThreadIdInGroupFlattened, 0);
  program.dcl_temps(kVgprBase + vgprs_);
  if (lds_) program.dcl_tgsm_raw(0, info_.lds_bytes > 32768 ? 32768 : info_.lds_bytes);
  program.dcl_thread_group(info_.threads[0], info_.threads[1], info_.threads[2]);
  auto init = [&](uint32_t reg, const Operand& value) { program.emit(dx::kMov, {D(reg), value}); };
  for (uint32_t i = 0; i < 106; ++i) init(i, I(0));
  for (uint32_t i = 0; i < vgprs_; ++i) init(kVgprBase + i, I(0));
  init(kVccLoR, I(0));
  init(kVccHiR, I(0));
  init(kM0R, I(0));
  init(kExecLoR, I(1));
  init(kExecHiR, I(0));
  init(kSccR, I(0));
  init(kNextR, I(0));
  for (uint32_t i = 0; i < info_.user_sgprs && i < 16; ++i) {
    init(i, dxbc::cb(0, kSystemUserVector + i / 4, i % 4));
  }
  uint32_t next_sgpr = info_.user_sgprs;
  for (uint32_t axis = 0; axis < 3; ++axis) {
    if (info_.tgid_enabled[axis]) init(next_sgpr++, dxbc::input(dxbc::kThreadGroupId, axis));
  }
  for (uint32_t axis = 0; axis < 3; ++axis) init(kVgprBase + axis, dxbc::input(dxbc::kThreadIdInGroup, axis));
  program.emit(dx::kAnd, {D(kLaneR), dxbc::input(dxbc::kThreadIdInGroupFlattened, 0), I(info_.wave_size - 1)});
  if (barrier_) {
    // The dispatcher stops at barriers; every thread reaches the same number
    // of them in programs that are correct on the console.
    program.emit(dx::kLoop, {});
    program.emit(dx::kMov, {D(kBarrierR), I(0)});
  }
  program.emit(dx::kLoop, {});
  program.append(body);
  if (barrier_) program.emit(dx::kBreakc, {R(kBarrierR)}, dxbc::kTestNonZero);
  program.emit(dx::kEndloop, {});
  if (barrier_) {
    program.emit(dx::kSync, {}, dxbc::kSyncThreads | dxbc::kSyncGroupMemory | dxbc::kSyncUavGroup);
    program.emit(dx::kEndloop, {});
  }
  program.emit(dx::kRet, {});
  *out = program.container();
  return true;
}

}  // namespace

bool translate_compute(const uint8_t* code, size_t size, const ComputeShaderInfo& info, std::vector<uint8_t>* dxbc,
                       std::string* error) {
  Translator translator(code, size, info);
  return translator.run(dxbc, error);
}

}  // namespace holy::d3d12
