#include "core/libs/d3d12_xs/shader/translate.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
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
constexpr uint32_t kNextR = 112, kLaneR = 113, kBarrierR = 114, kExportedR = 115;
constexpr uint32_t kScratch = 116, kScratchCount = 112;
constexpr uint32_t kVgprBase = kScratch + kScratchCount;
// Graphics outputs are kept in temps and written when the program ends:
// output register r component c lives in kOutputBase + 4 r + c (r 34 is depth).
constexpr uint32_t kOutputBase = kVgprBase + 256;
constexpr uint32_t kOutputRegisters = 35;
constexpr uint32_t kDepthRegister = 34;
constexpr uint32_t kNoRegister = 0xFFFFFFFFu;
constexpr uint32_t kEndBlock = 0xFFFFFFFFu;  // "next block" once the program has ended
// Operand code standing for the value another lane exchanged (cross-lane
// operations); outside the 9-bit operand range.
constexpr uint16_t kExchangeOperand = 0x200;
// Operand code standing for a DPP instruction's source 1 with its modifiers.
constexpr uint16_t kModifiedOperand = 0x201;
constexpr uint32_t kNoView = 0xFFFFFFFFu;
// Program data reached through s_getpc_b64 (tables after the code): the
// program's bytes appear at this address (above any title memory, inside the
// 48 bits a buffer descriptor holds), read through kCodeView.
constexpr uint32_t kCodeAddressHi = 0xFFF0u;
constexpr uint32_t kCodeView = 0xFFFFFFFEu;
// The immediate constant buffer (4096 vectors): the buffer format table
// (format_info), then program data (code_dword).
constexpr uint32_t kFormatTableVectors = 64;  // two words a format, 128 formats
constexpr uint32_t kInvalidFormat = 7;       // the numeric type of formats the table lacks
constexpr uint32_t kCodeDataLimit = 65536 - kFormatTableVectors * 16;

Operand R(uint32_t r) { return dxbc::temp(r); }
Operand I(uint32_t v) { return dxbc::imm(v); }
Operand D(uint32_t r) { return dxbc::temp_dst(r); }
bool is_temp(const Operand& o) { return ((o[0] >> 12) & 0xFF) == dxbc::kTemp; }

class Translator {
 public:
  enum class Stage { Compute, Vertex, Pixel, Ngg };

  Translator(const uint8_t* code, size_t size, const ComputeShaderInfo& info) : code_(code), size_(size), info_(info) {}
  Translator(const uint8_t* code, size_t size, GraphicsStage stage)
      : code_(code), size_(size), stage_(stage == GraphicsStage::Vertex ? Stage::Vertex : Stage::Pixel) {
    info_.user_sgprs = 16;
  }
  struct NggTag {};
  Translator(const uint8_t* code, size_t size, NggTag) : code_(code), size_(size), stage_(Stage::Ngg) {
    info_.user_sgprs = 16;
  }

  const NggInfo& ngg() const { return ngg_; }

  const ShaderInterface& link() const { return iface_; }
  void set_consumer(const ShaderInterface* consumer) { consumer_ = consumer; }

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
  void emit(uint32_t opcode, std::initializer_list<Operand> operands, uint32_t controls = 0) {
    p_.emit(opcode, operands, controls);
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
  // EXEC is kept as this lane's bit alone (the lane model: a lane mask holds
  // the lane's bit at bit 0), so it serves as the lane's condition as it is.
  void put(uint32_t reg, const Operand& v) {
    if (reg == kExecLoR) return emit(dx::kAnd, {D(reg), v, I(1)});
    emit(dx::kMov, {D(reg), v});
  }
  // put(reg, select(mask, a, b)) as one instruction (its sources are read
  // before the register is written).
  void put_select(uint32_t reg, const Operand& mask, const Operand& a, const Operand& b) {
    emit(dx::kMovc, {D(reg), mask, a, b});
  }
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
  Operand uge(const Operand& a, const Operand& b) { return op2(dx::kUge, a, b); }
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
  // One past the highest VGPR the program names (relative moves reach VGPRs
  // through M0, within the ones a program uses).
  uint32_t relative_vgpr_limit() {
    if (!relative_limit_) {
      uint32_t limit = 1;
      for (const Instruction& inst : program_) {
        for (uint16_t code : inst.src) {
          if (code >= rdna::kVgpr0 && code < rdna::kVgpr0 + 256) limit = std::max<uint32_t>(limit, code - rdna::kVgpr0 + 4);
        }
        const bool vector_dst = inst.encoding == Encoding::VOP1 || inst.encoding == Encoding::VOP2 ||
                                inst.encoding == Encoding::VOP3 || inst.encoding == Encoding::VOP3P ||
                                inst.encoding == Encoding::VINTRP || inst.encoding == Encoding::MUBUF ||
                                inst.encoding == Encoding::MTBUF || inst.encoding == Encoding::FLAT ||
                                inst.encoding == Encoding::MIMG || inst.encoding == Encoding::DS;
        if (vector_dst && inst.dst < 256) limit = std::max<uint32_t>(limit, inst.dst + 4u);
      }
      relative_limit_ = std::min<uint32_t>(limit, 256);
    }
    return relative_limit_;
  }

  // 32-bit source.
  Operand src(const Instruction& inst, uint16_t code) {
    if (code == kExchangeOperand) return R(exchange_reg_);
    if (code == kModifiedOperand) return R(modified_reg_);
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
  Operand active() { return R(kExecLoR); }
  // Stages that run as host compute programs (thread groups, group shared
  // memory, exchanges between lanes).
  bool computeish() const { return stage_ == Stage::Compute || stage_ == Stage::Ngg; }

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
  // Resolves a title GPU address to (view, offset) through the page table
  // (translate.h): view 0, the null view, when no page is mapped there.
  // `write`: for a store or an atomic (cached pages: the page itself, and its
  // flag set), else for a read (cached pages: the copy unless flagged).
  // Without branches: windows with no table (table 0) and addresses from
  // 2^47 read the zero table (translate.h), whose entries are slot 0. Every
  // memory access has one, and the host compiler's time grows faster than
  // the size of what it compiles (measured).
  std::pair<uint32_t, uint32_t> find(const Operand& lo, const Operand& hi, bool write = false) {
    const uint32_t view = fresh(), offset = fresh();
    const uint32_t mark = scratch_;  // what find uses besides its results goes when it returns
    const Operand alo = in_temp(lo), ahi = in_temp(hi);
    page_table_used_ = true;
    const Operand window = shl(bor(shl(ahi, I(2)), shr(alo, I(30))), I(2));
    const uint32_t table = fresh();
    emit(dx::kLdRaw, {D(table), select(ult(ahi, I(0x8000)), window, I(kPageTableZeroTable)),
                      dxbc::srv_xyzw(kPageTableId, 0)});
    const uint32_t entry = fresh();
    const Operand at = add(add(I(kPageTableZeroTable), shl(R(table), I(17))), shl(ubfe(I(14), I(16), alo), I(3)));
    emit(dx::kLdRaw, {dxbc::temp_dst_mask(entry, 0x3), at, dxbc::srv_xyzw(kPageTableId, 0)});
    put(view, R(entry));
    if (translator_options().page_cache_reads) {
      put(offset, dxbc::temp(entry, 1));
      if_nz(band(R(view), I(kCachedPageBit)));
      const uint32_t line = fresh(), line_at = fresh(), flags = fresh();
      put(line_at, shl(band(R(view), I(~kCachedPageBit)), I(2)));  // the flag's byte offset
      put(flags, I(kFlagsView));
      emit(dx::kLdRaw, {dxbc::temp_dst_mask(line, 0xF), add(I(kPageTableLevel1Bytes), shl(R(line_at), I(2))),
                        dxbc::srv_xyzw(kPageTableId, 0)});
      if (write) {
        emit(dx::kStoreRaw, {dxbc::uav_indexed(0, flags, dxbc::Access::Store), R(line_at), I(1)});
        put(view, dxbc::temp(line, 2));
        put(offset, dxbc::temp(line, 3));
      } else {
        const uint32_t flag = fresh();
        emit(dx::kLdRaw, {D(flag), R(line_at), dxbc::uav_indexed(0, flags, dxbc::Access::Load)});
        put_select(view, R(flag), dxbc::temp(line, 2), dxbc::temp(line, 0));
        put_select(offset, R(flag), dxbc::temp(line, 3), dxbc::temp(line, 1));
      }
      end_if();
      put(offset, add(R(offset), band(alo, I(0xFFFF))));
    } else {
      put(offset, add(dxbc::temp(entry, 1), band(alo, I(0xFFFF))));
    }
    if (code_data_) {
      const Operand in_code = band(eq(ahi, I(kCodeAddressHi)), ult(alo, I(static_cast<uint32_t>(size_))));
      if_nz(band(in_code, eq(R(view), I(0))));
      put(view, I(kCodeView));
      put(offset, alo);
      end_if();
    }
    scratch_ = mark;
    return {view, offset};
  }
  // A buffer format's component widths (a byte each, x first) and numeric
  // type (kInvalidFormat for formats the table lacks) from the format table
  // at the start of the immediate constant buffer (format_table): a lookup
  // where a switch over 128 formats made every typed access hundreds of
  // instructions long. `fmt` holds the format (below 128).
  void format_info(uint32_t fmt, uint32_t widths, uint32_t numeric) {
    format_table_ = true;
    const uint32_t vec = fresh();
    put(vec, shr(R(fmt), I(1)));
    const Operand odd = in_temp(band(R(fmt), I(1)));
    put_select(widths, odd, dxbc::icb_indexed(vec, 2), dxbc::icb_indexed(vec, 0));
    put_select(numeric, odd, dxbc::icb_indexed(vec, 3), dxbc::icb_indexed(vec, 1));
  }
  // The program dword at a byte offset (4-aligned), 0 outside the data the
  // immediate constant buffer holds (after the format table).
  Operand code_dword(const Operand& byte_offset) {
    const uint32_t result = fresh();
    put(result, I(0));
    const Operand rel = sub(byte_offset, I(code_first_));
    if_nz(band(uge(byte_offset, I(code_first_)), ult(rel, I(code_bytes_))));
    const uint32_t vec = fresh();
    put(vec, add(shr(rel, I(4)), I(kFormatTableVectors)));
    const Operand comp = ubfe(I(2), I(2), rel);
    Operand value = dxbc::icb_indexed(vec, 3);
    for (int c = 2; c >= 0; --c) value = select(eq(comp, I(static_cast<uint32_t>(c))), dxbc::icb_indexed(vec, c), value);
    put(result, value);
    end_if();
    return R(result);
  }
  // Reads the dword at view/offset (any alignment); 0 when unresolved (view 0,
  // the null view, reads zero; only programs reading their own code have
  // views that are not memory views).
  Operand read(uint32_t view, const Operand& offset) {
    const uint32_t value = fresh();
    if (code_data_) {
      put(value, I(0));
      if_nz(eq(R(view), I(kCodeView)));
      const Operand o = in_temp(offset);
      const Operand aligned = band(o, I(~3u));
      const Operand shift = shl(band(o, I(3)), I(3));
      const uint32_t w = fresh();
      put(w, code_dword(aligned));
      if_nz(shift);
      const Operand next = code_dword(add(aligned, I(4)));
      put(w, bor(shr(R(w), shift), shl(next, sub(I(32), shift))));
      end_if();
      put(value, R(w));
      end_if();
      if_nz(ult(R(view), I(kMemoryViews)));
    }
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
    if (!code_data_) return R(w);
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
  // Reads `count` consecutive dwords (1-4) from byte `byte_offset` past `at`
  // into registers `dest(i)`: one guard, and one load when the address is a
  // multiple of 4 (else dword by dword, any alignment); 0 when unresolved.
  // `aligned`: the address is known to be a multiple of 4 (one load, no
  // alignment test). Programs reading their own code as data read dword by
  // dword.
  template <typename Dest>
  void read_dwords(Dest dest, int count, uint32_t view, uint32_t at, uint32_t byte_offset, bool aligned = false) {
    if (code_data_ || (count == 1 && !aligned)) {
      for (int i = 0; i < count; ++i) read_to(dest(i), view, at, byte_offset + 4u * static_cast<uint32_t>(i));
      return;
    }
    const uint32_t mark = scratch_;
    const uint32_t o = fresh();
    put(o, byte_offset == 0 ? R(at) : add(R(at), I(byte_offset)));
    if (aligned) {
      const uint32_t v = fresh();
      const dxbc::Access access = count == 1 ? dxbc::Access::Load : dxbc::Access::LoadVector;
      emit(dx::kLdRaw, {dxbc::temp_dst_mask(v, (1u << count) - 1), R(o), dxbc::uav_indexed(0, view, access)});
      for (int i = 0; i < count; ++i) put(dest(i), dxbc::temp(v, static_cast<uint32_t>(i)));
      scratch_ = mark;
      return;
    }
    if_nz(band(R(o), I(3)));
    for (int i = 0; i < count; ++i) {
      const uint32_t inner = scratch_;
      const Operand word_at = band(add(R(o), I(4u * static_cast<uint32_t>(i))), I(~3u));
      const Operand shift = shl(band(R(o), I(3)), I(3));
      const uint32_t w = fresh(), w2 = fresh();
      emit(dx::kLdRaw, {D(w), word_at, dxbc::uav_indexed(0, view, dxbc::Access::Load)});
      emit(dx::kLdRaw, {D(w2), add(word_at, I(4)), dxbc::uav_indexed(0, view, dxbc::Access::Load)});
      put(dest(i), bor(shr(R(w), shift), shl(R(w2), sub(I(32), shift))));
      scratch_ = inner;
    }
    emit(dx::kElse, {});
    const uint32_t v = fresh();
    emit(dx::kLdRaw,
         {dxbc::temp_dst_mask(v, (1u << count) - 1), R(o), dxbc::uav_indexed(0, view, dxbc::Access::LoadVector)});
    for (int i = 0; i < count; ++i) put(dest(i), dxbc::temp(v, static_cast<uint32_t>(i)));
    end_if();
    scratch_ = mark;
  }
  // Writes go to memory views only (view 0 drops them); a guard where a view
  // can be the program's code.
  void if_memory_view(uint32_t view) {
    if (code_data_) if_nz(ult(R(view), I(kMemoryViews)));
  }
  void end_memory_view() {
    if (code_data_) end_if();
  }
  void write(uint32_t view, const Operand& offset, const Operand& value) {
    if_memory_view(view);
    emit(dx::kStoreRaw, {dxbc::uav_indexed(0, view, dxbc::Access::Store), band(offset, I(~3u)), value});
    end_memory_view();
  }
  // Byte or halfword store inside the containing dword.
  void write_part(uint32_t view, const Operand& offset, const Operand& value, uint32_t bytes) {
    if_memory_view(view);
    const Operand aligned = band(offset, I(~3u));
    const Operand shift = shl(band(offset, I(3)), I(3));
    const Operand mask = shl(I(bytes == 1 ? 0xFFu : 0xFFFFu), shift);
    const uint32_t ignored = fresh();
    emit(dx::kImmAtomicAnd, {D(ignored), dxbc::uav_indexed(0, view, dxbc::Access::Atomic), aligned, bnot(mask)});
    emit(dx::kImmAtomicOr,
         {D(ignored), dxbc::uav_indexed(0, view, dxbc::Access::Atomic), aligned, band(shl(value, shift), mask)});
    end_memory_view();
  }
  // Atomic kinds: 0 swap, 1 cmpswap, 2 add, 3 sub, 4 smin, 5 umin, 6 smax,
  // 7 umax, 8 and, 9 or, 10 xor. Returns the previous value.
  Operand atomic(int kind, uint32_t view, const Operand& offset, const Operand& value, const Operand& compare) {
    const uint32_t old = fresh();
    if (code_data_) put(old, I(0));
    if_memory_view(view);
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
    end_memory_view();
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
  // Output modifiers of a float result (VOP3, and SDWA once emit_vector has
  // copied them from the SDWA dword).
  Operand fresult(const Instruction& inst, Operand value) {
    if (inst.omod == 1) value = op2(dx::kMul, value, dxbc::immf(2.0f));
    if (inst.omod == 2) value = op2(dx::kMul, value, dxbc::immf(4.0f));
    if (inst.omod == 3) value = op2(dx::kMul, value, dxbc::immf(0.5f));
    if (inst.clamp) value = op1(dx::kMov, value, dxbc::kSaturate);
    return value;
  }

  bool linear_layout(const std::vector<std::pair<uint32_t, dxbc::Program>>& blocks,
                     const std::vector<char>& barrier_after, dxbc::Program& body);
  bool emit(const Instruction& inst);
  bool emit_sop2(const Instruction& inst);
  bool emit_sopk(const Instruction& inst);
  bool emit_sop1(const Instruction& inst);
  bool emit_sopc(const Instruction& inst);
  bool emit_smem(const Instruction& inst);
  bool emit_vector(const Instruction& inst);
  bool emit_flat(const Instruction& inst);
  bool emit_mubuf(const Instruction& inst);
  bool typed_load(uint32_t fmt, uint32_t components, uint32_t view, uint32_t at, uint32_t data);
  bool decode_format(uint32_t fmt, uint32_t components, uint32_t view, uint32_t at, const uint32_t* out);
  void format_load(uint32_t descriptor, uint32_t components, uint32_t view, uint32_t at, uint32_t data);
  void format_store(const Operand& format, uint32_t components, uint32_t view, uint32_t at, uint32_t data);
  bool emit_ds(const Instruction& inst);
  bool emit_export(const Instruction& inst);
  bool emit_image(const Instruction& inst);
  bool image_atomic64(const Instruction& inst, uint32_t slot, uint32_t coord_count, uint32_t index_reg);
  bool emit_vop3p(const Instruction& inst);
  // Cross-lane operations (compute programs): each lane stores the source
  // value and its EXEC bit in group shared memory; after a group barrier the
  // continuation reads the source lane's.
  static bool cross_lane(const Instruction& inst);
  void exchange_write(const Instruction& inst);
  bool exchange_read(const Instruction& inst);
  bool quad_read(const Instruction& inst);
  // The value lanes outside the quad contribute to `inst` as the source of
  // its operation (src0 = identity leaves the other operand), if it has one.
  bool source_identity(const Instruction& inst, uint32_t* identity) const;
  // The identity the next instruction reading `vgpr` (after program index
  // `from`) applies to it, if that instruction is such an operation.
  bool consumer_identity(size_t from, uint16_t vgpr, uint32_t* identity) const;
  bool finish_exchange(const Instruction& inst, uint32_t value, Operand valid, Operand write, bool fetch_inactive,
                       bool bound_zero, const Operand& source_active);
  bool emit_interp(const Instruction& inst);
  // Ends the program: the dispatcher leaves its loop (a single structured
  // exit; host drivers have refused programs that return from inside it).
  void end_program() { put(kNextR, I(kEndBlock)); }
  // After the dispatcher: graphics outputs are written from their temps;
  // pixel shaders that exported nothing (killed lanes) are discarded. A pixel
  // program without any export (depth-only passes, programs that only write
  // memory) keeps its pixels: the depth test and write happen without it.
  void epilogue(dxbc::Program& program) {
    if (stage_ == Stage::Pixel && pixel_exports_) program.emit(dx::kDiscard, {R(kExportedR)});
    for (uint32_t reg = 0; reg < kOutputRegisters; ++reg) {
      if (!outputs_register(reg)) continue;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(written_[reg] & (1u << c))) continue;
        const Operand value = R(kOutputBase + 4 * reg + c);
        if (reg == kDepthRegister) program.emit(dx::kMov, {dxbc::output_depth(), value});
        else program.emit(dx::kMov, {dxbc::output_register(reg, 1u << c), value});
      }
    }
    if (stage_ == Stage::Vertex && consumer_) {
      // Attributes the pixel program reads as halves: each channel's low and
      // high halves (kNextR, done with by now, holds the high one).
      for (uint32_t n = 0; n < 32; ++n) {
        if (!((consumer_->half_params & iface_.params) & (1u << n))) continue;
        const uint32_t out = half_param_register(consumer_->params, consumer_->half_params, n);
        for (uint32_t c = 0; c < 4; ++c) {
          const Operand value = R(kOutputBase + 4 * (n + 1) + c);
          const uint32_t component = (c & 1) * 2;
          program.emit(dx::kF16tof32, {dxbc::output_register(out + (c >> 1), 1u << component), value});
          program.emit(dx::kUshr, {D(kNextR), value, I(16)});
          program.emit(dx::kF16tof32, {dxbc::output_register(out + (c >> 1), 1u << (component + 1)), R(kNextR)});
        }
      }
    }
    program.emit(dx::kRet, {});
  }
  // Vertex programs with a consumer: whether output register `reg` (an
  // attribute's, n + 1) is output as is.
  bool outputs_register(uint32_t reg) const {
    if (stage_ != Stage::Vertex || !consumer_ || reg == 0 || reg > 32) return true;
    return (consumer_->params >> (reg - 1)) & 1;
  }
  void set_output(uint32_t reg, uint32_t component, const Operand& value) {
    written_[reg] |= 1u << component;
    put(kOutputBase + 4 * reg + component, value);
  }

  // NGG programs (see translate_ngg): the group size from the program's
  // v_lshrrev_b32 vN, k, v5 (the group of a vertex id), or the same as
  // v_readfirstlane_b32 sN, v5 then s_lshr_b32 sM, sN, k (observed: a third
  // of a title's NGG programs, k 7), and the vertex record from the
  // parameters it exports.
  bool ngg_scan() {
    uint32_t shift = 0;
    int max_param = -1;
    uint64_t first_lane_ids[2] = {};  // SGPRs a v_readfirstlane_b32 of v5 wrote
    for (const Instruction& inst : program_) {
      if (inst.encoding == Encoding::VOP2 && inst.op == 22 && inst.src[1] == rdna::kVgpr0 + 5 &&
          inst.src[0] >= 129 && inst.src[0] <= 136) {
        shift = inst.src[0] - 128;  // inline constants 1-8
      }
      if (inst.encoding == Encoding::VOP1 && inst.op == 2 && inst.src[0] == rdna::kVgpr0 + 5 && inst.dst < 106) {
        first_lane_ids[inst.dst / 64] |= 1ull << (inst.dst % 64);
      }
      if (inst.encoding == Encoding::SOP2 && inst.op == 32 && inst.src[0] < 106 &&
          (first_lane_ids[inst.src[0] / 64] >> (inst.src[0] % 64) & 1) && inst.src[1] >= 129 && inst.src[1] <= 136) {
        shift = inst.src[1] - 128;
      }
      if (inst.encoding == Encoding::EXP && inst.target >= 32 && inst.target < 64) {
        max_param = std::max(max_param, static_cast<int>(inst.target) - 32);
      }
    }
    if (!shift) return fail("an NGG program without a vertex group (v_lshrrev_b32 of v5)");
    ngg_.group = 1u << shift;
    ngg_.stride = 16u * static_cast<uint32_t>(2 + max_param);
    info_.threads[0] = ngg_.group;
    info_.threads[1] = info_.threads[2] = 1;
    return true;
  }
  // Group index (y groups of x) and this thread's slot (vertex and
  // primitive record) in the draw.
  Operand ngg_slot() {
    const Operand group = add(mul_lo(dxbc::input(dxbc::kThreadGroupId, 1), dxbc::cb(0, kSystemNggVector, 1)),
                              dxbc::input(dxbc::kThreadGroupId, 0));
    return add(mul_lo(group, I(ngg_.group)), dxbc::input(dxbc::kThreadIdInGroupFlattened, 0));
  }
  void ngg_prologue(dxbc::Program& program) {
    // Documented by open-source drivers for NGG programs: s2 group
    // information (vertices in bits 12-20, primitives in 22-30), s3 wave
    // information (wave index in bits 24-27, waves in 28-31), v5 the vertex
    // id, v8 the instance id. The draw's vertex groups are thread groups (x)
    // per instance (y).
    const uint32_t waves = (ngg_.group + info_.wave_size - 1) / info_.wave_size;
    const uint32_t shift = info_.wave_size == 64 ? 6 : 5;
    dxbc::Program saved = std::move(p_);
    p_ = dxbc::Program();
    put(2, I((ngg_.group << 22) | (ngg_.group << 12)));
    const Operand wave = shr(dxbc::input(dxbc::kThreadIdInGroupFlattened, 0), I(shift));
    put(3, bor(I((waves << 28) | (info_.wave_size << 8) | info_.wave_size), shl(wave, I(24))));
    const Operand first = add(dxbc::cb(0, kSystemNggVector, 2), mul_lo(dxbc::input(dxbc::kThreadGroupId, 0), I(ngg_.group)));
    if (vgprs_ > 5) put(kVgprBase + 5, add(first, dxbc::input(dxbc::kThreadIdInGroupFlattened, 0)));
    if (vgprs_ > 8) put(kVgprBase + 8, add(dxbc::cb(0, kSystemNggVector, 3), dxbc::input(dxbc::kThreadGroupId, 1)));
    // Every primitive slot starts as a degenerate triangle.
    const Operand slot = ngg_slot();
    const Operand base = sub(slot, dxbc::input(dxbc::kThreadIdInGroupFlattened, 0));
    const Operand at = add(dxbc::cb(0, kSystemNggVector, 0), mul_lo(slot, I(12)));
    for (uint32_t i = 0; i < 3; ++i) {
      emit(dx::kStoreRaw, {dxbc::uav(1, 0, dxbc::Access::Store), add(at, I(4 * i)), base});
    }
    program.append(p_);
    p_ = std::move(saved);
    scratch_ = 0;
  }
  // Exports of NGG programs go to the draw's vertex and primitive records.
  bool ngg_export(const Instruction& inst) {
    auto vgpr = [&](int i) { return R(vreg(inst.src[i] - rdna::kVgpr0)); };
    const uint32_t target = inst.target;
    if (target == 9 || (target >= 13 && target <= 15)) return true;  // null, point size, clip distances
    if_nz(active());
    const Operand slot = in_temp(ngg_slot());
    if (target == 12 || (target >= 32 && target < 64)) {
      if (target == 12) iface_.position = true;
      else iface_.params |= 1u << (target - 32);
      const uint32_t offset = target == 12 ? 0 : 16 * (1 + target - 32);
      const Operand at = add(mul_lo(slot, I(ngg_.stride)), I(offset));
      for (int c = 0; c < 4; ++c) {
        if (!(inst.enable & (1 << c))) continue;
        emit(dx::kStoreRaw, {dxbc::uav(1, 0, dxbc::Access::Store), add(at, I(4u * c)), vgpr(c)});
      }
    } else if (target == 20) {
      // Primitive: vertex indices in bits 0-8, 10-18, 20-28 of the group,
      // bit 31 a null primitive.
      const Operand packed = in_temp(vgpr(0));
      const Operand base = in_temp(sub(slot, dxbc::input(dxbc::kThreadIdInGroupFlattened, 0)));
      const Operand null = in_temp(nonzero(shr(packed, I(31))));
      const Operand at = add(dxbc::cb(0, kSystemNggVector, 0), mul_lo(slot, I(12)));
      for (uint32_t i = 0; i < 3; ++i) {
        const Operand index = select(null, base, add(base, ubfe(I(9), I(10 * i), packed)));
        emit(dx::kStoreRaw, {dxbc::uav(1, 0, dxbc::Access::Store), add(at, I(4 * i)), index});
      }
    } else {
      end_if();
      return fail(format("NGG export target %u", target));
    }
    end_if();
    return true;
  }

  // Cube coordinates as the console GPU takes them (documented by
  // open-source drivers, which form them from v_cube*: s and t are the face
  // coordinates divided by the major axis plus 1.5, then the face) turned
  // back into a direction for a host cube texture, in place in temp t.
  void cube_direction(uint32_t t) {
    const Operand s1 = in_temp(op2(dx::kAdd, dxbc::temp(t, 0), dxbc::immf(-1.5f)));
    const Operand t1 = in_temp(op2(dx::kAdd, dxbc::temp(t, 1), dxbc::immf(-1.5f)));
    // Cube arrays: the third coordinate is 8 x slice + face; slice 0 is read.
    const Operand face = in_temp(band(op1(dx::kFtou, dxbc::temp(t, 2)), I(7)));
    // The major axis is 0.5: s and t are the minor axes over twice the major.
    cube_vector(t, face, s1, t1, 0.5f);
  }
  // Writes into temp t the direction of face coordinates (s, t) relative to
  // the face centre with major axis component h: faces +x (h, -t, -s),
  // -x (-h, -t, s), +y (s, h, t), -y (s, -h, -t), +z (s, -t, h), -z (-s, -t, -h).
  // With h = 0 it maps face-coordinate gradients to direction gradients (the
  // direction is linear in s and t on a face).
  void cube_vector(uint32_t t, const Operand& face, const Operand& s1, const Operand& t1, float h) {
    const Operand ns = in_temp(dxbc::neg(s1)), nt = in_temp(dxbc::neg(t1));
    const Operand one = dxbc::immf(h), minus = dxbc::immf(-h);
    auto by_face = [&](const Operand v[6]) {
      Operand r = v[5];
      for (int f = 4; f >= 0; --f) r = select(eq(face, I(static_cast<uint32_t>(f))), v[f], r);
      return r;
    };
    const Operand xs[6] = {one, minus, s1, s1, s1, ns};
    const Operand ys[6] = {nt, nt, one, minus, nt, nt};
    const Operand zs[6] = {ns, s1, t1, nt, one, minus};
    const Operand x = in_temp(by_face(xs)), y = in_temp(by_face(ys)), z = in_temp(by_face(zs));
    emit(dx::kMov, {dxbc::temp_dst_mask(t, 1), x});
    emit(dx::kMov, {dxbc::temp_dst_mask(t, 2), y});
    emit(dx::kMov, {dxbc::temp_dst_mask(t, 4), z});
    emit(dx::kMov, {dxbc::temp_dst_mask(t, 8), I(0)});
  }

  const uint8_t* code_;
  size_t size_;
  ComputeShaderInfo info_;
  std::vector<Instruction> program_;
  size_t at_ = 0;  // index in program_ of the instruction being translated
  dxbc::Program p_;
  std::string error_;
  uint32_t scratch_ = 0;
  uint32_t vgprs_ = 3;
  uint32_t relative_limit_ = 0;  // relative_vgpr_limit()
  bool doubles_ = false;         // double-precision operations are used
  bool barrier_ = false;
  bool lds_ = false;
  // s_getpc_b64 present: program bytes [code_first_, + code_bytes_) are an
  // immediate constant buffer.
  bool code_data_ = false;
  uint32_t code_first_ = 0, code_bytes_ = 0;
  bool format_table_ = false;  // format_info is used
  std::set<uint32_t> indirect_targets_;  // addresses s_setpc_b64 / s_swappc_b64 reach
  const ShaderInterface* consumer_ = nullptr;  // vertex programs: the pixel program's interface
  uint32_t scan_params_ = 0;  // pixel: attributes read whole (VINTRP), from the scan
  Stage stage_ = Stage::Compute;
  NggInfo ngg_;
  ShaderInterface iface_;
  uint8_t written_[kOutputRegisters] = {};  // components exported per output register
  bool pixel_exports_ = false;              // a pixel program reaches an export instruction
  uint32_t exchange_reg_ = 0;    // temp holding the exchanged value while an instruction is emitted
  uint32_t modified_reg_ = 0;    // temp holding a DPP instruction's modified source 1
  bool exchange_ = false;        // the exchange area is used
  bool quad_ = false;            // a pixel program exchanges values within its quad (reads its position)
  uint32_t exchange_parity_ = 0; // which of the two exchange buffers the next exchange uses
  uint32_t textures_used_ = 0;  // ImageSlot bits
  uint32_t integer_uavs_used_ = 0;  // ImageSlot bits of image atomics
  bool ordered_uavs_ = false;        // pixel programs: integer image views are rasterizer-ordered
  uint64_t features_ = 0;            // SFI0 feature bits
  uint32_t uavs_used_ = 0;
  bool samplers_used_ = false;
  bool page_table_used_ = false;  // find() reads the page table
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
    case 37: {
      // s_bfm_b64: ((1 << S0[5:0]) - 1) << S1[5:0], 64 bits wide.
      const Operand width = band(a, I(63));
      const Operand big = op2(dx::kUge, width, I(32));
      const Operand low = add(shl(I(1), width), I(0xFFFFFFFFu));  // ishl masks the count to 5 bits
      auto [r0, r1] = shift64(0, select(big, I(0xFFFFFFFFu), low), select(big, low, I(0)), b);
      set_s64(d, r0, r1);
      return true;
    }
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
  // Without a destination: s_version (1), s_setreg (19: the register is its
  // source; 21: a literal), the waits (23-26; documented: a null register
  // waits for the immediate count alone). Not modelled: rounding and denormal
  // modes, and waits (the host orders memory accesses itself).
  switch (inst.op) {
    case 1: case 19: case 21: case 23: case 24: case 25: case 26: return true;
    default: break;
  }
  const uint32_t reg_index = sreg(d);
  if (reg_index == kNoRegister && inst.op != 18) return fail(format("SOPK op %u without a register", inst.op));
  const Operand reg = R(reg_index == kNoRegister ? 0 : reg_index);
  switch (inst.op) {
    case 0: put(reg_index, simm); return true;
    case 2: put_select(reg_index, R(kSccR), simm, reg); return true;
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
      if (r != kNoRegister) put_select(r, R(kSccR), a, R(r));
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
    case 9: case 10: {
      // s_wqm_b32/b64: every group of four bits with any bit set becomes 0xF.
      auto quads = [&](const Operand& v) {
        const Operand x = copy(v);
        const Operand any = band(bor(bor(x, shr(x, I(1))), bor(shr(x, I(2)), shr(x, I(3)))), I(0x11111111u));
        return mul_lo(any, I(15));
      };
      if (inst.op == 9) {
        const Operand r = quads(a);
        scc_nonzero(r);
        set_s(d, r);
        return true;
      }
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      const Operand r0 = copy(quads(lo)), r1 = copy(quads(hi));
      scc_nonzero(bor(r0, r1));
      set_s64(d, r0, r1);
      return true;
    }
    case 11: set_s(d, op1(dx::kBfrev, a)); return true;
    case 12: {  // s_brev_b64
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      set_s64(d, op1(dx::kBfrev, hi), op1(dx::kBfrev, lo));
      return true;
    }
    case 13: {  // s_bcnt0_i32_b32
      const Operand r = sub(I(32), op1(dx::kCountbits, a));
      scc_nonzero(r);
      set_s(d, r);
      return true;
    }
    case 14: case 16: {  // s_bcnt0_i32_b64, s_bcnt1_i32_b64
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      const Operand ones = add(op1(dx::kCountbits, lo), op1(dx::kCountbits, hi));
      const Operand r = inst.op == 14 ? sub(I(64), ones) : ones;
      scc_nonzero(r);
      set_s(d, r);
      return true;
    }
    case 15: { const Operand r = op1(dx::kCountbits, a); scc_nonzero(r); set_s(d, r); return true; }
    case 17: set_s(d, op1(dx::kFirstbitLo, bnot(a))); return true;  // s_ff0_i32_b32
    case 18: case 20: {  // s_ff0_i32_b64, s_ff1_i32_b64: the lowest clear (set) bit, -1 if none
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      if (inst.op == 18) {
        lo = bnot(lo);
        hi = bnot(hi);
      }
      const Operand in_hi = select(ne(hi, I(0)), add(op1(dx::kFirstbitLo, hi), I(32)), I(0xFFFFFFFFu));
      set_s(d, select(ne(lo, I(0)), op1(dx::kFirstbitLo, lo), in_hi));
      return true;
    }
    case 19: set_s(d, op1(dx::kFirstbitLo, a)); return true;
    case 21: set_s(d, op1(dx::kFirstbitHi, a)); return true;
    case 22: case 24: {
      // s_flbit_i32_b64: leading zeros; s_flbit_i32_i64: leading bits equal
      // to the sign bit. -1 when there is no such boundary.
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      if (inst.op == 24) {
        const Operand sign = sar(hi, I(31));
        lo = bxor(lo, sign);
        hi = bxor(hi, sign);
      }
      const Operand in_lo = select(ne(lo, I(0)), add(op1(dx::kFirstbitHi, lo), I(32)), I(0xFFFFFFFFu));
      set_s(d, select(ne(hi, I(0)), op1(dx::kFirstbitHi, hi), in_lo));
      return true;
    }
    case 23: set_s(d, op1(dx::kFirstbitShi, a)); return true;  // s_flbit_i32
    case 25: set_s(d, ibfe(I(8), I(0), a)); return true;
    case 26: set_s(d, ibfe(I(16), I(0), a)); return true;
    case 27: case 29: {
      const uint32_t r = sreg(d);
      if (r == kNoRegister) return true;
      const Operand bit = shl(I(1), a);
      put(r, inst.op == 27 ? band(R(r), bnot(bit)) : bor(R(r), bit));
      return true;
    }
    case 28: case 30: {  // s_bitset0_b64, s_bitset1_b64: bit a[5:0] of the 64-bit destination
      if (d > 104 && d != rdna::kVccLo && d != rdna::kExecLo) return fail("s_bitset_b64 destination");
      const uint32_t r0 = sreg(d);
      const uint32_t r1 = d == rdna::kVccLo ? kVccHiR : d == rdna::kExecLo ? kExecHiR : r0 + 1;
      const Operand index = band(a, I(63));
      const Operand bit = shl(I(1), index);  // the shift takes the low five bits
      const Operand low = ult(index, I(32));
      const Operand lo_bit = select(low, bit, I(0)), hi_bit = select(low, I(0), bit);
      if (inst.op == 28) {
        set_s64(d, band(R(r0), bnot(lo_bit)), band(R(r1), bnot(hi_bit)));
      } else {
        set_s64(d, bor(R(r0), lo_bit), bor(R(r1), hi_bit));
      }
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
    case 32: case 33: {
      // s_setpc_b64 / s_swappc_b64 (swappc: dst = the address of the next
      // instruction): a jump to the address in src, a block of this program
      // (decoding found the addresses the program forms).
      Operand lo, hi;
      src64(inst, inst.src[0], &lo, &hi);
      const Operand target = copy(lo);
      if (inst.op == 33) set_s64(d, I(inst.pc + 4), I(kCodeAddressHi));
      put(kNextR, target);
      return true;
    }
    case 31:  // s_getpc_b64: the address of the next instruction, in the program's data range
      set_s64(d, I(inst.pc + 4), I(kCodeAddressHi));
      return true;
    case 41: case 42: case 43: case 65: case 66: case 67: {
      // s_nand/nor/xnor_saveexec (b64, b32): dst = exec; exec = ~(src op exec).
      // Lane masks keep the lane's own bit only.
      Operand slo, shi;
      if (inst.op <= 43) {
        src64(inst, inst.src[0], &slo, &shi);
      } else {
        slo = a;
      }
      const Operand e0 = copy(R(kExecLoR)), e1 = copy(R(kExecHiR));
      const uint32_t kind = inst.op <= 43 ? inst.op - 41 : inst.op - 65;
      const Operand combined = kind == 0 ? band(slo, e0) : kind == 1 ? bor(slo, e0) : bxor(slo, e0);
      put(kExecLoR, band(bnot(combined), I(1)));
      put(kExecHiR, I(0));
      scc_nonzero(R(kExecLoR));
      if (inst.op <= 43) {
        set_s64(d, e0, e1);
      } else {
        set_s(d, e0);
      }
      return true;
    }
    case 57: case 58: case 70: case 71: {
      // s_andn1/andn2_wrexec (b64, b32): exec = ~src & exec (andn1) or
      // src & ~exec (andn2); dst = the new exec.
      const Operand src_lo = copy(a);
      const Operand e0 = copy(R(kExecLoR));
      const bool andn1 = inst.op == 57 || inst.op == 70;
      put(kExecLoR, band(andn1 ? band(bnot(src_lo), e0) : band(src_lo, bnot(e0)), I(1)));
      put(kExecHiR, I(0));
      scc_nonzero(R(kExecLoR));
      if (inst.op <= 58) {
        set_s64(d, R(kExecLoR), I(0));
      } else {
        set_s(d, R(kExecLoR));
      }
      return true;
    }
    case 59: {  // s_bitreplicate_b64_b32: bit i of the source becomes bits 2i and 2i + 1
      auto spread = [&](const Operand& half) {
        Operand x = band(half, I(0xFFFF));
        x = band(bor(x, shl(x, I(8))), I(0x00FF00FFu));
        x = band(bor(x, shl(x, I(4))), I(0x0F0F0F0Fu));
        x = band(bor(x, shl(x, I(2))), I(0x33333333u));
        x = band(bor(x, shl(x, I(1))), I(0x55555555u));
        return bor(x, shl(x, I(1)));
      };
      const Operand v = copy(a);
      set_s64(d, spread(v), spread(shr(v, I(16))));
      return true;
    }
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

// The linear layout: blocks in program order, each run when the next-block
// register names it (a test only where an earlier branch can pass over it);
// loops only around regions of blocks that backward branches go back into,
// continuing while the register names a block of the region; group
// barriers in line, where the block they follow ends (every thread passes
// them there; programs that are correct on the console reach the same
// barriers in every thread). Each backward branch needs a region holding
// its block and its target; regions that overlap are merged (a loop the
// program enters in its middle, past the block its branch back goes to, is
// one region), regions inside others stay loops of their own. Branches
// forward need no loop and no search, and the host compiler sees code
// outside loops as such (the dispatcher keeps every register live around
// one loop). Programs that jump through registers keep the dispatcher
// (false).
bool Translator::linear_layout(const std::vector<std::pair<uint32_t, dxbc::Program>>& blocks,
                               const std::vector<char>& barrier_after, dxbc::Program& body) {
  const size_t count = blocks.size();
  auto block_of = [&](uint32_t pc) {
    size_t lo = 0, hi = count;  // the last block starting at or before pc
    while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      (blocks[mid].first <= pc ? lo : hi) = mid;
    }
    return lo;
  };
  struct Skip {
    size_t from;      // the block the branch ends
    uint32_t target;  // kEndBlock: the program's end
    size_t to;        // backward: the target's block
    bool backward;
  };
  std::vector<Skip> skips;
  std::vector<std::pair<size_t, size_t>> back;  // target block, branching block
  for (const Instruction& inst : program_) {
    if (inst.encoding == Encoding::SOP1 && (inst.op == 32 || inst.op == 33)) return false;
    if (inst.encoding != Encoding::SOPP) continue;
    const size_t from = block_of(inst.pc);
    if (inst.op == 1) {
      skips.push_back({from, kEndBlock, 0, false});
      continue;
    }
    if (inst.op != 2 && (inst.op < 4 || inst.op > 9)) continue;
    const uint32_t target = static_cast<uint32_t>(inst.pc + 4 + 4 * inst.imm);
    if (target > inst.pc) {
      skips.push_back({from, target, 0, false});
      continue;
    }
    const size_t to = block_of(target);
    if (blocks[to].first != target) return false;
    back.emplace_back(to, from);
    skips.push_back({from, target, to, true});
  }
  // Regions [first, last] (block indices), nested or disjoint: in order of
  // start (the widest first), each backward branch's range goes inside the
  // innermost open region holding its start, whose ancestors grow to its end
  // where it reaches past them.
  struct Region {
    size_t first, last;
  };
  std::vector<Region> regions;
  std::sort(back.begin(), back.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second > b.second;
  });
  {
    std::vector<size_t> open;
    for (const auto& [first, last] : back) {
      while (!open.empty() && regions[open.back()].last < first) open.pop_back();
      for (size_t k = open.size(); k-- > 0 && regions[open[k]].last < last;) regions[open[k]].last = last;
      if (!open.empty() && regions[open.back()].first == first && regions[open.back()].last == last) continue;
      regions.push_back({first, last});
      open.push_back(regions.size() - 1);
    }
  }
  // Blocks a branch can pass over: forward, those up to its target (all, for
  // the program's end); backward, the rest of the innermost region holding
  // both ends (whose loop takes the branch back).
  std::vector<int> delta(count + 1, 0);
  for (const Skip& skip : skips) {
    const size_t first = skip.from + 1;
    size_t last = count;  // one past
    if (skip.backward) {
      size_t span = SIZE_MAX;
      for (const Region& r : regions) {
        if (r.first <= skip.to && r.last >= skip.from && r.last - r.first < span) {
          span = r.last - r.first;
          last = r.last + 1;
        }
      }
    } else if (skip.target != kEndBlock) {
      last = std::min(count, block_of(skip.target - 1) + 1);
    }
    if (first < last) {
      ++delta[first];
      --delta[last];
    }
  }
  // Regions by first block, the widest first.
  std::vector<size_t> order(regions.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return regions[a].first != regions[b].first ? regions[a].first < regions[b].first
                                                : regions[a].last > regions[b].last;
  });
  size_t next_region = 0;
  std::vector<size_t> loops;  // open regions, innermost last
  int passed = 0;
  for (size_t b = 0; b < count; ++b) {
    passed += delta[b];
    for (; next_region < order.size() && regions[order[next_region]].first == b; ++next_region) {
      body.emit(dx::kLoop, {});
      loops.push_back(order[next_region]);
    }
    if (passed > 0) {
      body.emit(dx::kIeq, {D(kScratch), R(kNextR), I(blocks[b].first)});
      body.emit(dx::kIf, {R(kScratch)}, dxbc::kTestNonZero);
      body.append(blocks[b].second);
      body.emit(dx::kEndif, {});
    } else {
      body.append(blocks[b].second);
    }
    if (barrier_after[b]) {
      body.emit(dx::kSync, {}, dxbc::kSyncThreads | dxbc::kSyncGroupMemory | dxbc::kSyncUavGlobal);
    }
    // Round again while the next block is one of the region's (a branch
    // back); forward branches out of it and the program's end leave.
    while (!loops.empty() && regions[loops.back()].last == b) {
      const Region& r = regions[loops.back()];
      const uint32_t start = blocks[r.first].first;
      body.emit(dx::kIadd, {D(kScratch), R(kNextR), I(0u - start)});
      body.emit(dx::kUge, {D(kScratch), R(kScratch), I(blocks[r.last].first - start + 1)});
      body.emit(dx::kBreakc, {R(kScratch)}, dxbc::kTestNonZero);
      body.emit(dx::kEndloop, {});
      loops.pop_back();
    }
  }
  return true;
}

bool Translator::emit_smem(const Instruction& inst) {
  static const int kCounts[] = {1, 2, 4, 8, 16};
  const uint16_t base = inst.src[0];
  // Offset: immediate plus an SGPR unless the field names null. Documented:
  // the two low bits of each part of a scalar memory address (base, offset,
  // instruction offset) are ignored, so scalar loads read whole, aligned
  // dwords.
  Operand offset = I(static_cast<uint32_t>(inst.imm) & ~3u);
  if (inst.src[1] != rdna::kNull) offset = add(offset, band(src(inst, inst.src[1]), I(~3u)));
  if (inst.op <= 4) {
    const int count = kCounts[inst.op];
    auto [lo, hi] = add64(band(R(base), I(~3u)), band(R(base + 1u), I(0xFFFF)), offset, true);
    auto [view, at] = find(lo, hi);
    for (int i = 0; i < count; i += 4) {
      read_dwords([&](int k) { return inst.dst + static_cast<uint32_t>(i + k); }, std::min(4, count - i), view, at,
                  4u * static_cast<uint32_t>(i), true);
    }
    return true;
  }
  if (inst.op >= 8 && inst.op <= 12) {
    // s_buffer_load: buffer descriptor in s[base..base+3] (address, stride, records).
    const int count = kCounts[inst.op - 8];
    const Operand o = copy(offset);
    auto [lo, hi] = add64(band(R(base), I(~3u)), band(R(base + 1u), I(0xFFFF)), o, false);
    const Operand records = copy(R(base + 2u));
    auto [view, at] = find(lo, hi);
    for (int i = 0; i < count; i += 4) {
      read_dwords([&](int k) { return inst.dst + static_cast<uint32_t>(i + k); }, std::min(4, count - i), view, at,
                  4u * static_cast<uint32_t>(i), true);
    }
    for (int i = 0; i < count; ++i) {
      const uint32_t dest = inst.dst + static_cast<uint32_t>(i);
      const uint32_t mark = scratch_;
      put_select(dest, op2(dx::kUge, records, add(o, I(4u * i + 4u))), R(dest), I(0));
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
  const bool saddr = inst.src[2] != rdna::kNull;
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
  const auto location = find(lo, hi, inst.op >= 24);  // stores and atomics from 24 on
  const uint32_t view = location.first, at = location.second;
  auto load = [&](int count) {
    read_dwords([&](int k) { return vreg(dst + static_cast<uint32_t>(k)); }, count, view, at, 0);
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

// RDNA 2 unified buffer formats (public instruction set reference): the
// component widths, low bits first, and the numeric type. Names in the
// reference list components high bits first ("10_11_11" is x 11, y 11, z 10).
namespace {
enum class Numeric : uint8_t { Unorm, Snorm, Uscaled, Sscaled, Uint, Sint, Float };
struct BufferFormat {
  uint8_t bits[4];  // 0: component absent
};
// Formats 1-77 come in groups sharing a layout; the numeric type follows
// the order unorm, snorm, uscaled, sscaled, uint, sint, float within a group.
bool buffer_format(uint32_t fmt, BufferFormat* layout, Numeric* numeric) {
  struct Group {
    uint8_t first, count;
    uint8_t bits[4];
    const Numeric* types;
  };
  static const Numeric kAll[] = {Numeric::Unorm, Numeric::Snorm, Numeric::Uscaled, Numeric::Sscaled,
                                 Numeric::Uint,  Numeric::Sint,  Numeric::Float};
  static const Numeric kInt[] = {Numeric::Unorm, Numeric::Snorm, Numeric::Uscaled,
                                 Numeric::Sscaled, Numeric::Uint, Numeric::Sint};
  static const Numeric kWide[] = {Numeric::Uint, Numeric::Sint, Numeric::Float};
  static const Group kGroups[] = {
      {1, 6, {8, 0, 0, 0}, kInt},           {7, 7, {16, 0, 0, 0}, kAll},
      {14, 6, {8, 8, 0, 0}, kInt},          {20, 3, {32, 0, 0, 0}, kWide},
      {23, 7, {16, 16, 0, 0}, kAll},        {30, 7, {11, 11, 10, 0}, kAll},
      {37, 7, {10, 11, 11, 0}, kAll},       {44, 6, {2, 10, 10, 10}, kInt},
      {50, 6, {10, 10, 10, 2}, kInt},       {56, 6, {8, 8, 8, 8}, kInt},
      {62, 3, {32, 32, 0, 0}, kWide},       {65, 7, {16, 16, 16, 16}, kAll},
      {72, 3, {32, 32, 32, 0}, kWide},      {75, 3, {32, 32, 32, 32}, kWide},
  };
  for (const Group& g : kGroups) {
    if (fmt >= g.first && fmt < static_cast<uint32_t>(g.first + g.count)) {
      for (int i = 0; i < 4; ++i) layout->bits[i] = g.bits[i];
      *numeric = g.types[fmt - g.first];
      return true;
    }
  }
  return false;
}
}  // namespace

bool Translator::typed_load(uint32_t fmt, uint32_t components, uint32_t view, uint32_t at, uint32_t data) {
  uint32_t out[4];
  for (uint32_t c = 0; c < components; ++c) out[c] = vreg(data + c);
  return decode_format(fmt, components, view, at, out);
}

// Loads with the format in the buffer descriptor (buffer_load_format_*):
// word 3 holds the destination selects (bits 0-11, three bits a component:
// 0 zero, 1 one, 4-7 the loaded x-w) and the format (bits 12-18), as in the
// public RDNA 2 buffer descriptor layout. The format is known only when the
// program runs: the format table (format_info) gives the component widths
// and numeric type, then one decoder handles every format (a decoder per
// format made programs too large for the host's shader compiler). Format 0
// (invalid) and formats the table lacks read as zero.
void Translator::format_load(uint32_t descriptor, uint32_t components, uint32_t view, uint32_t at, uint32_t data) {
  constexpr uint32_t kInvalid = kInvalidFormat;
  uint32_t loaded[4], words[4];
  for (uint32_t c = 0; c < 4; ++c) loaded[c] = fresh();
  for (uint32_t c = 0; c < 4; ++c) words[c] = fresh();
  const uint32_t fmt = fresh(), widths = fresh(), numeric = fresh(), offset = fresh(), one = fresh();
  put(fmt, ubfe(I(7), I(12), R(descriptor)));
  format_info(fmt, widths, numeric);
  for (uint32_t w = 0; w < 4; ++w) {
    const uint32_t mark = scratch_;
    put(words[w], read(view, add(R(at), I(4 * w))));
    scratch_ = mark;
  }
  {
    const uint32_t mark = scratch_;
    const Operand integer = bor(eq(R(numeric), I(static_cast<uint32_t>(Numeric::Uint))),
                                eq(R(numeric), I(static_cast<uint32_t>(Numeric::Sint))));
    put_select(one, eq(R(numeric), I(kInvalid)), I(0), select(integer, I(1), dxbc::immf(1.0f)));
    scratch_ = mark;
  }
  put(offset, I(0));
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t mark = scratch_;
    const Operand width = in_temp(ubfe(I(8), I(8 * c), R(widths)));
    const Operand index = in_temp(shr(R(offset), I(5)));
    const Operand shift = in_temp(band(R(offset), I(31)));
    const Operand word = in_temp(select(band(index, I(2)), select(band(index, I(1)), R(words[3]), R(words[2])),
                                        select(band(index, I(1)), R(words[1]), R(words[0]))));
    const Operand full = in_temp(eq(width, I(32)));
    const Operand u = in_temp(select(full, word, ubfe(width, shift, word)));
    const Operand sv = in_temp(select(full, word, ibfe(width, shift, word)));
    const Operand top = in_temp(select(full, I(0xFFFFFFFFu), add(shl(I(1), width), I(0xFFFFFFFFu))));
    const Operand unorm = op2(dx::kDiv, op1(dx::kUtof, u), op1(dx::kUtof, top));
    const Operand snorm =
        op2(dx::kMax, op2(dx::kDiv, op1(dx::kItof, sv), op1(dx::kUtof, shr(top, I(1)))), dxbc::immf(-1.0f));
    // Floats: 32-bit as they are; 16-bit halves; 11- and 10-bit unsigned
    // small floats (the half layout without sign, fewer mantissa bits).
    const Operand small = select(eq(width, I(16)), op1(dx::kF16tof32, u),
                                 select(eq(width, I(11)), op1(dx::kF16tof32, shl(u, I(4))),
                                        op1(dx::kF16tof32, shl(u, I(5)))));
    const Operand real = select(full, u, small);
    auto is = [&](Numeric type) { return eq(R(numeric), I(static_cast<uint32_t>(type))); };
    Operand value = select(is(Numeric::Float), real, I(0));
    value = select(is(Numeric::Sint), sv, value);
    value = select(is(Numeric::Uint), u, value);
    value = select(is(Numeric::Sscaled), op1(dx::kItof, sv), value);
    value = select(is(Numeric::Uscaled), op1(dx::kUtof, u), value);
    value = select(is(Numeric::Snorm), snorm, value);
    value = select(is(Numeric::Unorm), unorm, value);
    // Absent components read as 0, and w as one.
    put(loaded[c], select(eq(width, I(0)), c == 3 ? R(one) : I(0), value));
    put(offset, add(R(offset), width));
    scratch_ = mark;
  }
  for (uint32_t c = 0; c < components; ++c) {
    const uint32_t mark = scratch_;
    const Operand select_bits = ubfe(I(3), I(3 * c), R(descriptor));
    const Operand from_xy = select(band(select_bits, I(1)), R(loaded[1]), R(loaded[0]));
    const Operand from_zw = select(band(select_bits, I(1)), R(loaded[3]), R(loaded[2]));
    const Operand component = select(band(select_bits, I(2)), from_zw, from_xy);
    const Operand constant = select(eq(select_bits, I(1)), R(one), I(0));
    put(vreg(data + c), select(band(select_bits, I(4)), component, constant));
    scratch_ = mark;
  }
}

// Stores in a buffer format known when the program runs (the descriptor's,
// or a typed store's): components packed low bits first by width and numeric
// type (the inverse of format_load), components the instruction lacks as 0.
// Elements of 8 or 16 bits write only their own bytes; others whole dwords.
// Format 0 and formats outside the table store nothing.
void Translator::format_store(const Operand& format, uint32_t components, uint32_t view, uint32_t at,
                              uint32_t data) {
  constexpr uint32_t kInvalid = kInvalidFormat;
  uint32_t words[4];
  for (uint32_t w = 0; w < 4; ++w) words[w] = fresh();
  const uint32_t fmt = fresh(), widths = fresh(), numeric = fresh(), offset = fresh();
  put(fmt, band(format, I(127)));
  format_info(fmt, widths, numeric);
  for (uint32_t w = 0; w < 4; ++w) put(words[w], I(0));
  put(offset, I(0));
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t mark = scratch_;
    const Operand width = in_temp(ubfe(I(8), I(8 * c), R(widths)));
    const Operand full = in_temp(eq(width, I(32)));
    const Operand mask = in_temp(select(full, I(0xFFFFFFFFu), add(shl(I(1), width), I(0xFFFFFFFFu))));
    const Operand v = c < components ? R(vreg(data + c)) : I(0);
    const Operand top = in_temp(op1(dx::kUtof, mask));
    const Operand unorm = op1(dx::kFtou, op1(dx::kRoundNe, op2(dx::kMul, op1(dx::kMov, v, dxbc::kSaturate), top)));
    const Operand half_top = in_temp(op1(dx::kUtof, shr(mask, I(1))));
    const Operand clamped = op2(dx::kMin, op2(dx::kMax, v, dxbc::immf(-1.0f)), dxbc::immf(1.0f));
    const Operand snorm = op1(dx::kFtoi, op1(dx::kRoundNe, op2(dx::kMul, clamped, half_top)));
    const Operand half = op1(dx::kF32tof16, v);
    const Operand small = select(eq(width, I(16)), half, select(eq(width, I(11)), shr(half, I(4)), shr(half, I(5))));
    auto is = [&](Numeric type) { return eq(R(numeric), I(static_cast<uint32_t>(type))); };
    Operand bits = select(is(Numeric::Float), select(full, v, small), v);  // uint and sint as they are
    bits = select(is(Numeric::Sscaled), op1(dx::kFtoi, v), bits);
    bits = select(is(Numeric::Uscaled), op1(dx::kFtou, v), bits);
    bits = select(is(Numeric::Snorm), snorm, bits);
    bits = select(is(Numeric::Unorm), unorm, bits);
    const Operand field = in_temp(band(bits, mask));
    const Operand index = in_temp(shr(R(offset), I(5)));
    const Operand placed = in_temp(shl(field, band(R(offset), I(31))));
    const Operand present = in_temp(ne(width, I(0)));
    for (uint32_t w = 0; w < 4; ++w) {
      put(words[w], select(band(present, eq(index, I(w))), bor(R(words[w]), placed), R(words[w])));
    }
    put(offset, add(R(offset), width));
    scratch_ = mark;
  }
  // R(offset) is now the element's size in bits.
  if_nz(ne(R(numeric), I(kInvalid)));
  if_nz(ult(R(offset), I(32)));
  if_nz(eq(R(offset), I(8)));
  write_part(view, R(at), R(words[0]), 1);
  emit(dx::kElse, {});
  write_part(view, R(at), R(words[0]), 2);
  end_if();
  emit(dx::kElse, {});
  for (uint32_t w = 0; w < 4; ++w) {
    const uint32_t mark = scratch_;
    if_nz(ult(I(32 * w), R(offset)));
    write(view, add(R(at), I(4 * w)), R(words[w]));
    end_if();
    scratch_ = mark;
  }
  end_if();
  end_if();
}

// Decodes `components` components of format `fmt` at (view, at) into the
// temps `out`.
bool Translator::decode_format(uint32_t fmt, uint32_t components, uint32_t view, uint32_t at, const uint32_t* out) {
  BufferFormat layout;
  Numeric numeric;
  if (!buffer_format(fmt, &layout, &numeric)) return fail(format("typed buffer format %u", fmt));
  const bool integer = numeric == Numeric::Uint || numeric == Numeric::Sint;
  uint32_t bit = 0;
  for (uint32_t c = 0; c < components; ++c) {
    const uint32_t width = layout.bits[c];
    const uint32_t dest = out[c];
    const uint32_t mark = scratch_;
    if (width == 0) {
      // Absent components read as 0, and w as one.
      put(dest, c == 3 ? (integer ? I(1) : dxbc::immf(1.0f)) : I(0));
      continue;
    }
    const Operand word = read(view, add(R(at), I((bit / 32) * 4)));
    const uint32_t shift = bit % 32;
    Operand v = width == 32 ? word : ubfe(I(width), I(shift), word);
    const bool sign = numeric == Numeric::Snorm || numeric == Numeric::Sscaled || numeric == Numeric::Sint;
    if (sign && width < 32) v = ibfe(I(width), I(shift), word);
    switch (numeric) {
      case Numeric::Uint: case Numeric::Sint: break;
      case Numeric::Unorm:
        v = op2(dx::kMul, op1(dx::kUtof, v), dxbc::immf(1.0f / static_cast<float>((1ull << width) - 1)));
        break;
      case Numeric::Snorm:
        v = op2(dx::kMax, op2(dx::kMul, op1(dx::kItof, v), dxbc::immf(1.0f / static_cast<float>((1ull << (width - 1)) - 1))),
                dxbc::immf(-1.0f));
        break;
      case Numeric::Uscaled: v = op1(dx::kUtof, v); break;
      case Numeric::Sscaled: v = op1(dx::kItof, v); break;
      case Numeric::Float:
        if (width == 16) {
          v = op1(dx::kF16tof32, v);
        } else if (width == 11 || width == 10) {
          // Unsigned small floats: 5-bit exponent and 6 or 5 mantissa bits,
          // the half-float layout without sign and with fewer mantissa bits.
          v = op1(dx::kF16tof32, shl(v, I(width == 11 ? 4 : 5)));
        }
        break;
    }
    put(dest, v);
    scratch_ = mark;
    bit += width;
  }
  return true;
}

bool Translator::emit_mubuf(const Instruction& inst) {
  const uint32_t r = inst.src[2];
  const uint32_t vaddr = inst.src[0] - rdna::kVgpr0;
  const uint32_t data = inst.src[1] - rdna::kVgpr0;
  Operand offset = I(static_cast<uint32_t>(inst.imm));
  if (inst.src[3] != rdna::kNull && inst.src[3] != 0x80) {
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
  // Stores: typed 4-7 (both encodings), 24-31; atomics 48-59.
  const bool writes = inst.encoding == Encoding::MTBUF
                          ? inst.op >= 4 && inst.op <= 7
                          : (inst.op >= 4 && inst.op <= 7) || (inst.op >= 24 && inst.op <= 31) ||
                                (inst.op >= 48 && inst.op <= 59);
  const auto location = find(lo, hi, writes);
  const uint32_t at = location.second;
  const uint32_t view = fresh();
  put_select(view, inside, R(location.first), I(kNoView));
  auto load = [&](int count) {
    read_dwords([&](int k) { return vreg(data + static_cast<uint32_t>(k)); }, count, view, at, 0);
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
    // Typed loads: element format from the instruction, component count
    // from the opcode.
    const uint32_t components = (inst.op & 3) + 1;
    if (inst.op >= 4 && inst.op <= 7) {
      use_vgpr(inst.src[1], static_cast<int>(components));
      format_store(I(inst.ext), components, view, at, data);
    } else if (inst.op >= 4) {
      ok = fail(format("MTBUF op %u", inst.op));
    } else {
      ok = typed_load(inst.ext, components, view, at, data);
    }
    end_if();
    return ok;
  }
  switch (inst.op) {
    case 0: case 1: case 2: case 3:
      use_vgpr(inst.src[1], static_cast<int>(inst.op) + 1);
      format_load(r + 3, inst.op + 1, view, at, data);
      break;
    case 4: case 5: case 6: case 7:
      use_vgpr(inst.src[1], static_cast<int>(inst.op) - 3);
      format_store(ubfe(I(7), I(12), R(r + 3)), inst.op - 3, view, at, data);
      break;
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
  auto rmw = [&](uint32_t opcode, bool returns, bool negate = false) {
    use_vgpr(inst.src[1]);
    const uint32_t old = fresh();
    const Operand value = negate ? sub(I(0), R(vreg(d0))) : R(vreg(d0));
    emit(opcode, {D(old), dxbc::group_shared(0, dxbc::Access::Atomic), address(offset), value});
    if (returns) put(vreg(inst.dst), R(old));
  };
  // Compare and store (documented: DATA is the comparand, DATA2 the value
  // stored, the reverse of the buffer compare-swap).
  auto compare_store = [&](bool returns) {
    use_vgpr(inst.src[1]);
    use_vgpr(inst.src[2]);
    const uint32_t old = fresh();
    emit(dx::kImmAtomicCmpExch,
         {D(old), dxbc::group_shared(0, dxbc::Access::Atomic), address(offset), R(vreg(d0)), R(vreg(d1))});
    if (returns) put(vreg(inst.dst), R(old));
  };
  // Byte and short stores: the bytes of their word are cleared, then set, with
  // atomics, so lanes storing neighbouring bytes of one word do not race.
  // (_d16_hi: the data is the high half of the register.)
  auto store_part = [&](uint32_t bits, bool high = false) {
    use_vgpr(inst.src[1]);
    const Operand shift = shl(band(add(R(addr), I(offset)), I(3)), I(3));
    const Operand field = I(bits == 8 ? 0xFFu : 0xFFFFu);
    const Operand at = address(offset);
    const Operand data = high ? shr(R(vreg(d0)), I(16)) : R(vreg(d0));
    emit(dx::kAtomicAnd, {dxbc::group_shared(0, dxbc::Access::Atomic), at, bnot(shl(field, shift))});
    emit(dx::kAtomicOr, {dxbc::group_shared(0, dxbc::Access::Atomic), at, shl(band(data, field), shift)});
  };
  // Byte and short loads, sign- or zero-extended.
  auto load_part = [&](uint32_t bits, bool sign) {
    const Operand shift = shl(band(add(R(addr), I(offset)), I(3)), I(3));
    const Operand word = load(offset);
    put(vreg(inst.dst), sign ? ibfe(I(bits), shift, word) : ubfe(I(bits), shift, word));
  };
  // D16 loads (documented): the value, extended to 16 bits, into one half
  // of the destination; the other half is kept.
  auto load_half = [&](uint32_t bits, bool sign, bool high) {
    const Operand shift = shl(band(add(R(addr), I(offset)), I(3)), I(3));
    const Operand word = load(offset);
    const Operand value = sign ? ibfe(I(bits), shift, word) : ubfe(I(bits), shift, word);
    const uint32_t d = vreg(inst.dst);
    put(d, op4(dx::kBfi, I(16), I(high ? 16 : 0), value, R(d)));
  };
  bool ok = true;
  switch (inst.op) {
    case 13: use_vgpr(inst.src[1]); store(offset, R(vreg(d0))); break;
    case 160: store_part(8, true); break;   // ds_write_b8_d16_hi
    case 161: store_part(16, true); break;  // ds_write_b16_d16_hi
    case 162: load_half(8, false, false); break;   // ds_read_u8_d16
    case 163: load_half(8, false, true); break;    // ds_read_u8_d16_hi
    case 164: load_half(8, true, false); break;    // ds_read_i8_d16
    case 165: load_half(8, true, true); break;     // ds_read_i8_d16_hi
    case 166: load_half(16, false, false); break;  // ds_read_u16_d16
    case 167: load_half(16, false, true); break;   // ds_read_u16_d16_hi
    case 222:  // ds_write_b96
      use_vgpr(inst.src[1], 3);
      for (uint32_t i = 0; i < 3; ++i) store(offset + 4 * i, R(vreg(d0 + i)));
      break;
    case 61: case 62: {
      // ds_consume / ds_append (documented): the dword at M0's base plus the
      // offset is decremented / incremented by the active lanes of the wave
      // and its previous value returned; a lane mask's wave is the lane alone
      // (see v_mbcnt), so each lane counts one.
      if ((inst.raw[0] >> 17) & 1) {
        ok = fail("ds_append/ds_consume on the global data share");
        break;
      }
      const uint32_t old = fresh();
      const Operand at = band(add(band(R(kM0R), I(0xFFFF)), I(offset)), I(~3u));
      emit(dx::kImmAtomicIadd,
           {D(old), dxbc::group_shared(0, dxbc::Access::Atomic), at, I(inst.op == 62 ? 1u : 0xFFFFFFFFu)});
      put(vreg(inst.dst), R(old));
      break;
    }
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
    case 78: case 79: {  // ds_write2_b64, ds_write2st64_b64
      use_vgpr(inst.src[1], 2);
      use_vgpr(inst.src[2], 2);
      const uint32_t scale = inst.op == 78 ? 8 : 512;
      store(offset0 * scale, R(vreg(d0)));
      store(offset0 * scale + 4, R(vreg(d0 + 1)));
      store(offset1 * scale, R(vreg(d1)));
      store(offset1 * scale + 4, R(vreg(d1 + 1)));
      break;
    }
    case 119: case 120: {  // ds_read2_b64, ds_read2st64_b64
      const uint32_t scale = inst.op == 119 ? 8 : 512;
      const Operand x0 = load(offset0 * scale), x1 = load(offset0 * scale + 4);
      const Operand y0 = load(offset1 * scale), y1 = load(offset1 * scale + 4);
      put(vreg(inst.dst), x0);
      put(vreg(inst.dst + 1u), x1);
      put(vreg(inst.dst + 2u), y0);
      put(vreg(inst.dst + 3u), y1);
      break;
    }
    case 30: store_part(8); break;   // ds_write_b8
    case 31: store_part(16); break;  // ds_write_b16
    case 57: load_part(8, true); break;    // ds_read_i8
    case 58: load_part(8, false); break;   // ds_read_u8
    case 59: load_part(16, true); break;   // ds_read_i16
    case 60: load_part(16, false); break;  // ds_read_u16
    case 223:
      use_vgpr(inst.src[1], 4);
      for (uint32_t i = 0; i < 4; ++i) store(offset + 4 * i, R(vreg(d0 + i)));
      break;
    case 118: case 254: case 255: {
      const uint32_t count = inst.op == 118 ? 2 : inst.op == 254 ? 3 : 4;
      std::vector<Operand> values;
      for (uint32_t i = 0; i < count; ++i) values.push_back(load(offset + 4 * i));
      for (uint32_t i = 0; i < count; ++i) put(vreg(inst.dst + i), values[i]);
      break;
    }
    case 0: rmw(dx::kImmAtomicIadd, false); break;
    case 1: rmw(dx::kImmAtomicIadd, false, true); break;
    case 5: rmw(dx::kImmAtomicImin, false); break;
    case 6: rmw(dx::kImmAtomicImax, false); break;
    case 7: rmw(dx::kImmAtomicUmin, false); break;
    case 8: rmw(dx::kImmAtomicUmax, false); break;
    case 9: rmw(dx::kImmAtomicAnd, false); break;
    case 10: rmw(dx::kImmAtomicOr, false); break;
    case 11: rmw(dx::kImmAtomicXor, false); break;
    case 16: compare_store(false); break;
    case 32: rmw(dx::kImmAtomicIadd, true); break;
    case 33: rmw(dx::kImmAtomicIadd, true, true); break;
    case 37: rmw(dx::kImmAtomicImin, true); break;
    case 38: rmw(dx::kImmAtomicImax, true); break;
    case 39: rmw(dx::kImmAtomicUmin, true); break;
    case 40: rmw(dx::kImmAtomicUmax, true); break;
    case 41: rmw(dx::kImmAtomicAnd, true); break;
    case 42: rmw(dx::kImmAtomicOr, true); break;
    case 43: rmw(dx::kImmAtomicXor, true); break;
    case 45: rmw(dx::kImmAtomicExch, true); break;
    case 48: compare_store(true); break;
    default: ok = fail(format("DS op %u", inst.op)); break;
  }
  end_if();
  return ok;
}

namespace {
// SDWA instructions whose destination is a VGPR (VOP1 and VOP2; compares
// write lane masks and have no destination select).
bool kind_is_vector_result(const Instruction& inst) {
  return inst.encoding == Encoding::VOP1 || inst.encoding == Encoding::VOP2;
}
}  // namespace

bool Translator::emit_vector(const Instruction& inst) {
  if (inst.src[0] == rdna::kDpp16 || inst.src[0] == rdna::kDpp8 || inst.src[0] == rdna::kDpp8Fi) {
    return fail("cross-lane operand (DPP) outside a compute program");
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
    // Output modifiers (documented): clamp in bit 13, omod in bits 14-15.
    if (inst.encoding != Encoding::VOPC) {
      work.clamp = (e >> 13) & 1;
      work.omod = static_cast<uint8_t>((e >> 14) & 3);
    }
    if (inst.encoding != Encoding::VOPC && ((e >> 11) & 3) == 3) return fail("SDWA unused-bits mode 3");
    sdwa_src0 = pick(src(work, real), (e >> 16) & 7, (e >> 19) & 1);
    if (inst.encoding != Encoding::VOP1) sdwa_src1 = pick(src(work, work.src[1]), (e >> 24) & 7, (e >> 27) & 1);
  }
  auto s = [&](int i) {
    if (sdwa && i == 0) return sdwa_src0;
    if (sdwa && i == 1) return sdwa_src1;
    return src(work, work.src[i]);
  };
  // SDWA float modifiers of sources 0 and 1 (documented: negate bits 20/28,
  // absolute value bits 21/29 of the SDWA dword).
  auto sdwa_modify = [&](Operand v, int i) {
    if (!sdwa || i > 1) return v;
    const uint32_t bits = (inst.ext >> (i == 0 ? 20 : 28)) & 3;
    if (bits & 2) v = dxbc::abs(in_temp(v));
    if (bits & 1) v = dxbc::neg(in_temp(v));
    return v;
  };
  auto f = [&](int i) {
    if (sdwa && i < 2) return sdwa_modify(s(i), i);
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
  // SDWA destination select (documented): the result's low byte or word goes
  // to the selected field; the other bits are zero (unused mode 0), the
  // field's sign above it and zero below (1) or the register's own (2).
  const uint32_t dst_sel = sdwa && kind_is_vector_result(inst) ? (inst.ext >> 8) & 7 : 6;
  const uint32_t dst_unused = (inst.ext >> 11) & 3;
  auto place = [&](const Operand& value) -> Operand {
    if (dst_sel >= 6) return value;
    const uint32_t width = dst_sel < 4 ? 8 : 16;
    const uint32_t shift = dst_sel < 4 ? 8 * dst_sel : 16 * (dst_sel - 4);
    if (dst_unused == 2) return op4(dx::kBfi, I(width), I(shift), value, dval);
    const Operand field = dst_unused == 1 ? ibfe(I(width), I(0), value) : ubfe(I(width), I(0), value);
    return shift ? shl(field, I(shift)) : field;
  };
  // Results are written only by active lanes.
  auto set = [&](const Operand& value) {
    const Operand placed = place(value);
    put_select(dreg, on, placed[0] & 0x80000000u ? copy(placed) : placed, dval);
    return true;
  };
  auto setf = [&](const Operand& value) { return set(fresult(work, value)); };
  // Doubles (documented 64-bit float operands): VGPR or SGPR pairs low dword
  // first, inline float constants as doubles, a literal as the high dword;
  // VOP3 abs and neg act on bit 63. Packed into a temp's x and y.
  auto d64 = [&](int i) -> uint32_t {
    const uint16_t code = work.src[i];
    Operand lo = I(0), hi = I(0);
    if (code >= rdna::kVgpr0) {
      lo = R(vreg(code - rdna::kVgpr0));
      hi = R(vreg(code - rdna::kVgpr0 + 1u));
    } else if (code <= 104) {
      lo = R(code);
      hi = R(code + 1u);
    } else if (code >= 240 && code <= 248) {
      static const uint32_t kHigh[] = {0x3FE00000u, 0xBFE00000u, 0x3FF00000u, 0xBFF00000u, 0x40000000u,
                                       0xC0000000u, 0x40100000u, 0xC0100000u, 0x3FC45F30u};
      hi = I(kHigh[code - 240]);
      lo = I(code == 248 ? 0x6DC9C883u : 0u);  // 1/(2 pi)
    } else if (code == rdna::kLiteral) {
      hi = I(inst.literal);
    } else if (code != 128) {
      fail(format("a double operand of code %u", code));
    }
    if (e64 && ((inst.abs >> i) & 1)) hi = band(hi, I(0x7FFFFFFFu));
    if (e64 && ((inst.neg >> i) & 1)) hi = bxor(hi, I(0x80000000u));
    const uint32_t t = fresh();
    emit(dx::kMov, {dxbc::temp_dst_mask(t, 0x1), lo});
    emit(dx::kMov, {dxbc::temp_dst_mask(t, 0x2), hi});
    doubles_ = true;
    return t;
  };
  // A double result (a temp's x and y) into vdst and vdst + 1, active lanes.
  auto set64 = [&](uint32_t t) {
    if (inst.clamp || inst.omod) return fail("a double result with clamp or output modifier");
    const uint32_t lo = vreg(inst.dst), hi = vreg(inst.dst + 1u);
    put_select(lo, on, dxbc::temp(t, 0), R(lo));
    put_select(hi, on, dxbc::temp(t, 1), R(hi));
    return true;
  };
  auto dop = [&](uint32_t opcode, uint32_t a64, uint32_t b64) {
    const uint32_t r = fresh();
    emit(opcode, {dxbc::temp_dst_mask(r, 0x3), dxbc::temp_swizzle(a64, 0x44), dxbc::temp_swizzle(b64, 0x44)});
    return r;
  };
  const Operand literal = I(inst.literal);
  auto fadd = [&](const Operand& x, const Operand& y) { return op2(dx::kAdd, x, y); };
  auto fmul = [&](const Operand& x, const Operand& y) { return op2(dx::kMul, x, y); };
  auto fmad = [&](const Operand& x, const Operand& y, const Operand& z) { return op3(dx::kMad, x, y, z); };
  auto fneg = [&](const Operand& x) { return dxbc::neg(in_temp(x)); };
  // 16-bit operands: the low half, or (VOP3) the half op_sel names; halves as
  // floats take the VOP3 abs/neg modifiers. 16-bit results go to the low half
  // (the high half zero) or, with op_sel bit 3 (VOP3), to the high half,
  // keeping the low one.
  auto h16 = [&](int i) {
    Operand v = s(i);
    if (e64 && ((inst.opsel >> i) & 1)) v = shr(v, I(16));
    return band(v, I(0xFFFF));
  };
  auto f16 = [&](int i) {
    Operand v = op1(dx::kF16tof32, h16(i));
    if (e64 && ((inst.abs >> i) & 1)) v = dxbc::abs(in_temp(v));
    if (e64 && ((inst.neg >> i) & 1)) v = dxbc::neg(in_temp(v));
    return sdwa_modify(v, i);
  };
  auto sext16 = [&](const Operand& v) { return ibfe(I(16), I(0), v); };
  auto set16 = [&](const Operand& value) {
    if (e64 && ((inst.opsel >> 3) & 1)) return set(op4(dx::kBfi, I(16), I(16), value, dval));
    return set(band(value, I(0xFFFF)));
  };
  auto setf16 = [&](Operand value) {
    if (work.clamp) value = op1(dx::kMov, value, dxbc::kSaturate);
    return set16(op1(dx::kF32tof16, value));
  };

  if (kind == kVopc) {
    // Compare: the mask bit goes to VCC (e32) or the named SGPRs (e64);
    // v_cmpx writes EXEC instead.
    Operand cond;
    const Operand a = s(0), b = s(1);
    const uint16_t base = op & 0xF0;
    const int index = op & 0xF;
    bool exec_write = false;
    auto icmp_of = [&](bool is_signed, const Operand& a, const Operand& b, int index) {
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
    auto icmp = [&](bool is_signed) { return icmp_of(is_signed, a, b, index); };
    // Class test: the class bit of a float with `ebits` exponent and `mbits`
    // mantissa bits (in the low bits of u) selected in the mask b.
    auto class_test = [&](const Operand& u, uint32_t ebits, uint32_t mbits) {
      const uint32_t emax = (1u << ebits) - 1;
      const Operand sign = nonzero(ubfe(I(1), I(ebits + mbits), u));
      const Operand e = ubfe(I(ebits), I(mbits), u);
      const Operand m = band(u, I((1u << mbits) - 1));
      const Operand m_zero = eq(m, I(0));
      const Operand e0 = select(m_zero, select(sign, I(5), I(6)), select(sign, I(4), I(7)));
      const Operand emaxed =
          select(m_zero, select(sign, I(2), I(9)), select(band(m, I(1u << (mbits - 1))), I(1), I(0)));
      const Operand bit = select(eq(e, I(0)), e0, select(eq(e, I(emax)), emaxed, select(sign, I(3), I(8))));
      return nonzero(band(shr(b, bit), I(1)));
    };
    // Float compares by their 16-entry index (f, lt, eq, le, gt, lg, ge, o, u,
    // nge, nlg, ngt, nle, neq, nlt, tru).
    auto fcmp = [&](int which, const Operand& x0, const Operand& y0) {
      const Operand fa = in_temp(x0), fb = in_temp(y0);
      auto lt = [&](const Operand& x, const Operand& y) { return op2(dx::kLt, x, y); };
      auto ge = [&](const Operand& x, const Operand& y) { return op2(dx::kGe, x, y); };
      auto ordered = [&]() { return band(op2(dx::kEq, fa, fa), op2(dx::kEq, fb, fb)); };
      auto lg = [&]() { return bor(lt(fa, fb), lt(fb, fa)); };
      switch (which) {
        case 0: return I(0);
        case 1: return lt(fa, fb);
        case 2: return op2(dx::kEq, fa, fb);
        case 3: return ge(fb, fa);
        case 4: return lt(fb, fa);
        case 5: return lg();
        case 6: return ge(fa, fb);
        case 7: return ordered();
        case 8: return bnot(ordered());
        case 9: return bnot(ge(fa, fb));
        case 10: return bnot(lg());
        case 11: return bnot(lt(fb, fa));
        case 12: return bnot(ge(fb, fa));
        case 13: return op2(dx::kNe, fa, fb);
        case 14: return bnot(lt(fa, fb));
        default: return I(0xFFFFFFFFu);
      }
    };
    if ((base == 0xC0 || base == 0xD0 || base == 0xE0 || base == 0xF0) && index >= 8) {
      // f16 compares: the first eight at 0xC8 (v_cmpx 0xD8), the others at 0xE8 (0xF8).
      exec_write = base == 0xD0 || base == 0xF0;
      cond = fcmp(base >= 0xE0 ? index : index - 8, f16(0), f16(1));
    } else if ((base == 0xA0 || base == 0xB0) && index >= 9 && index <= 14) {
      // Unsigned 16-bit compares (lt, eq, le, gt, ne, ge) of the low halves.
      exec_write = base == 0xB0;
      cond = icmp_of(false, band(a, I(0xFFFF)), band(b, I(0xFFFF)), index - 8);
    } else if (op < 0x20) {
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
        cond = class_test(in_temp(f(0)), 8, 23);
      } else if (index == 15) {
        // v_cmp_class_f16, on the low half of src0.
        cond = class_test(band(a, I(0xFFFF)), 5, 10);
      } else {
        // 9-14: signed 16-bit compares (lt, eq, le, gt, ne, ge) of the low halves.
        cond = icmp_of(true, ibfe(I(16), I(0), a), ibfe(I(16), I(0), b), index - 8);
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
      case 50: return setf16(fadd(f16(0), f16(1)));
      case 51: return setf16(fadd(f16(0), fneg(f16(1))));
      case 52: return setf16(fadd(f16(1), fneg(f16(0))));
      case 53: return setf16(fmul(f16(0), f16(1)));
      case 54: return setf16(fmad(f16(0), f16(1), op1(dx::kF16tof32, band(dval, I(0xFFFF)))));
      case 55: return setf16(fmad(f16(0), op1(dx::kF16tof32, I(inst.literal & 0xFFFF)), f16(1)));
      case 56: return setf16(fmad(f16(0), f16(1), op1(dx::kF16tof32, I(inst.literal & 0xFFFF))));
      case 57: return setf16(op2(dx::kMax, f16(0), f16(1)));
      case 58: return setf16(op2(dx::kMin, f16(0), f16(1)));
      case 59: return setf16(fmul(f16(0), op1(dx::kExp, op1(dx::kItof, sext16(h16(1))))));
      case 60: {
        // v_pk_fmac_f16: each half of D += the halves of S0 times S1.
        const Operand x = in_temp(s(0)), y = in_temp(s(1)), z = in_temp(dval);
        auto half = [&](const Operand& v, bool high) {
          return op1(dx::kF16tof32, high ? shr(v, I(16)) : band(v, I(0xFFFF)));
        };
        const Operand lo = op1(dx::kF32tof16, fmad(half(x, false), half(y, false), half(z, false)));
        const Operand hi = op1(dx::kF32tof16, fmad(half(x, true), half(y, true), half(z, true)));
        return set(bor(band(lo, I(0xFFFF)), shl(hi, I(16))));
      }
      default: return fail(format("VOP2 op %u", op));
    }
  }

  if (kind == kVop1) {
    const Operand a = s(0);
    switch (op) {
      case 0: return true;
      case 1: return set(a);
      case 2:
        // v_readfirstlane_b32: the first lane EXEC enables. Lane masks hold
        // only this lane's bit (v_mbcnt above), so that lane is this one: the
        // lane's own value (exact when all lanes agree, and what scalarising
        // loops and wave-coalesced atomics need).
        set_s(inst.dst, a);
        return true;
      case 15: {  // v_cvt_f32_f64
        const uint32_t r = fresh();
        emit(dx::kDtof, {dxbc::temp_dst_mask(r, 0x1), dxbc::temp_swizzle(d64(0), 0x44)});
        return setf(R(r));
      }
      case 16: {  // v_cvt_f64_f32
        const uint32_t r = fresh();
        emit(dx::kFtod, {dxbc::temp_dst_mask(r, 0x3), f(0)});
        doubles_ = true;
        return set64(r);
      }
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
      case 66: case 67: {
        // Documented relative moves: v_movreld_b32 (66) v[vdst + M0] = src0,
        // v_movrels_b32 (67) vdst = v[src0 + M0]. Host temps cannot be
        // indexed: a selection on M0 over the program's VGPRs (beyond them
        // reads give zero and writes go nowhere).
        const uint32_t limit = relative_vgpr_limit();
        if (op == 67) {
          if (work.src[0] < rdna::kVgpr0) return fail("v_movrels_b32 with a scalar operand");
          const uint32_t base = work.src[0] - rdna::kVgpr0;
          const uint32_t value = fresh();
          put(value, I(0));
          for (uint32_t k = 0; base + k < limit; ++k) {
            const uint32_t mark = scratch_;
            put_select(value, eq(R(kM0R), I(k)), R(vreg(base + k)), R(value));
            scratch_ = mark;
          }
          return set(R(value));
        }
        const Operand v = copy(s(0));
        for (uint32_t k = 0; inst.dst + k < limit; ++k) {
          const uint32_t mark = scratch_;
          const uint32_t r = vreg(inst.dst + k);
          put_select(r, band(on, eq(R(kM0R), I(k))), v, R(r));
          scratch_ = mark;
        }
        return true;
      }
      case 101: {
        // v_swap_b32: vdst and the VGPR src0 exchange values.
        if (work.src[0] < rdna::kVgpr0) return fail("v_swap_b32 with a scalar operand");
        const uint32_t other = vreg(work.src[0]);
        const Operand old = copy(dval);
        put_select(other, on, old, R(other));
        return set(a);
      }
      case 80: return setf16(op1(dx::kUtof, h16(0)));
      case 81: return setf16(op1(dx::kItof, sext16(h16(0))));
      case 82: return set16(op1(dx::kFtou, op2(dx::kMin, op2(dx::kMax, f16(0), dxbc::immf(0.0f)), dxbc::immf(65535.0f))));
      case 83: return set16(op1(dx::kFtoi, op2(dx::kMin, op2(dx::kMax, f16(0), dxbc::immf(-32768.0f)), dxbc::immf(32767.0f))));
      case 84: return setf16(op1(dx::kRcp, f16(0)));
      case 85: return setf16(op1(dx::kSqrt, f16(0)));
      case 86: return setf16(op1(dx::kRsq, f16(0)));
      case 87: return setf16(op1(dx::kLog, f16(0)));
      case 88: return setf16(op1(dx::kExp, f16(0)));
      case 91: return setf16(op1(dx::kRoundNi, f16(0)));
      case 92: return setf16(op1(dx::kRoundPi, f16(0)));
      case 93: return setf16(op1(dx::kRoundZ, f16(0)));
      case 94: return setf16(op1(dx::kRoundNe, f16(0)));
      case 95: return setf16(op1(dx::kFrc, f16(0)));
      case 96: case 97: {
        const uint32_t r = fresh();
        const Operand angle = fmul(f16(0), dxbc::immf(6.2831853f));
        if (op == 96) emit(dx::kSincos, {D(r), dxbc::null_dst(), angle});
        else emit(dx::kSincos, {dxbc::null_dst(), D(r), angle});
        return setf16(R(r));
      }
      default: return fail(format("VOP1 op %u", op));
    }
  }

  // VOP3-only.
  const Operand a = s(0), b = s(1), c = s(2);
  auto med3 = [&](uint32_t mn, uint32_t mx, const Operand& x, const Operand& y, const Operand& z) {
    return op2(mx, op2(mn, x, y), op2(mn, op2(mx, x, y), z));
  };
  // Cube map coordinates (documented): the major axis is z when |z| is the
  // largest, else y when |y| >= |x|, else x.
  auto cube = [&](int which) {
    const Operand x = in_temp(f(0)), y = in_temp(f(1)), z = in_temp(f(2));
    const Operand ax = dxbc::abs(x), ay = dxbc::abs(y), az = dxbc::abs(z);
    const Operand major_z = band(op2(dx::kGe, az, ax), op2(dx::kGe, az, ay));
    const Operand major_y = band(bnot(major_z), op2(dx::kGe, ay, ax));
    const Operand zero = dxbc::immf(0.0f);
    const Operand xn = op2(dx::kLt, x, zero), yn = op2(dx::kLt, y, zero), zn = op2(dx::kLt, z, zero);
    switch (which) {
      case 0:  // face id: x+ 0, x- 1, y+ 2, y- 3, z+ 4, z- 5
        return select(major_z, select(zn, dxbc::immf(5.0f), dxbc::immf(4.0f)),
                      select(major_y, select(yn, dxbc::immf(3.0f), dxbc::immf(2.0f)),
                             select(xn, dxbc::immf(1.0f), dxbc::immf(0.0f))));
      case 1:  // s
        return select(major_z, select(zn, fneg(x), x), select(major_y, x, select(xn, z, fneg(z))));
      case 2:  // t
        return select(major_z, fneg(y), select(major_y, select(yn, fneg(z), z), fneg(y)));
      default:  // twice the major axis coordinate
        return fmul(select(major_z, z, select(major_y, y, x)), dxbc::immf(2.0f));
    }
  };
  switch (op) {
    case 0x342: case 0x343: {
      // v_interp_p1ll_f16 / p1lv_f16: attribute (bits 0-5 of the source-0
      // field), channel (6-7) and high half (8); the host interpolates, so the
      // result is the interpolated half as a float (v_interp_p2_f16 finishes).
      const uint32_t attribute = work.src[0] & 0x3F, channel = (work.src[0] >> 6) & 3, high = (work.src[0] >> 8) & 1;
      if (stage_ != Stage::Pixel || attribute >= 31) return fail("half interpolation outside a pixel shader");
      const uint32_t reg = half_param_register(scan_params_, iface_.half_params, attribute) + (channel >> 1);
      return set(dxbc::input_register(reg, (channel & 1) * 2 + high));
    }
    case 0x35a:  // v_interp_p2_f16: the interpolated value (from p1, in src2) as a half
      if (stage_ != Stage::Pixel) return fail("half interpolation outside a pixel shader");
      return set16(op1(dx::kF32tof16, s(2)));
    case 0x140: {  // v_mad_legacy_f32: a zero factor gives a zero product whatever the other
      const Operand fa = f(0), fb = f(1);
      const Operand zero = bor(op2(dx::kEq, fa, dxbc::immf(0.0f)), op2(dx::kEq, fb, dxbc::immf(0.0f)));
      return setf(fadd(select(zero, dxbc::immf(0.0f), fmul(fa, fb)), f(2)));
    }
    case 0x144: return setf(cube(0));
    case 0x145: return setf(cube(1));
    case 0x146: return setf(cube(2));
    case 0x147: return setf(cube(3));
    case 0x15e: {  // v_cvt_pk_u8_f32: src0 as an 8-bit integer into byte src1[1:0] of src2
      const Operand clamped = op2(dx::kMin, op2(dx::kMax, f(0), dxbc::immf(0.0f)), dxbc::immf(255.0f));
      const Operand byte = op1(dx::kFtou, op1(dx::kRoundNe, clamped));
      return set(op4(dx::kBfi, I(8), shl(band(b, I(3)), I(3)), byte, c));
    }
    case 0x311: {  // v_pack_b32_f16: two halves, abs/neg applied to their sign bits
      auto bits = [&](int i) {
        Operand v = h16(i);
        if ((inst.abs >> i) & 1) v = band(v, I(0x7FFF));
        if ((inst.neg >> i) & 1) v = bxor(v, I(0x8000));
        return v;
      };
      return set(bor(bits(0), shl(bits(1), I(16))));
    }
    case 0x373: return set(add(mul_lo(h16(0), h16(1)), c));
    case 0x375: return set(add(mul_lo(sext16(h16(0)), sext16(h16(1))), c));
    case 0x303: case 0x30d: return set16(add(h16(0), h16(1)));
    case 0x304: case 0x30e: return set16(sub(h16(0), h16(1)));
    case 0x305: return set16(mul_lo(h16(0), h16(1)));
    case 0x307: return set16(shr(h16(1), band(h16(0), I(15))));
    case 0x308: return set16(sar(sext16(h16(1)), band(h16(0), I(15))));
    case 0x309: return set16(op2(dx::kUmax, h16(0), h16(1)));
    case 0x30a: return set16(op2(dx::kImax, sext16(h16(0)), sext16(h16(1))));
    case 0x30b: return set16(op2(dx::kUmin, h16(0), h16(1)));
    case 0x30c: return set16(op2(dx::kImin, sext16(h16(0)), sext16(h16(1))));
    case 0x314: return set16(shl(h16(1), band(h16(0), I(15))));
    case 0x340: return set16(add(mul_lo(h16(0), h16(1)), h16(2)));
    case 0x35e: return set16(add(mul_lo(sext16(h16(0)), sext16(h16(1))), sext16(h16(2))));
    case 0x34b: return setf16(fmad(f16(0), f16(1), f16(2)));
    case 0x351: return setf16(op2(dx::kMin, op2(dx::kMin, f16(0), f16(1)), f16(2)));
    case 0x354: return setf16(op2(dx::kMax, op2(dx::kMax, f16(0), f16(1)), f16(2)));
    case 0x357: return setf16(med3(dx::kMin, dx::kMax, f16(0), f16(1), f16(2)));
    case 0x352: return set16(op2(dx::kImin, op2(dx::kImin, sext16(h16(0)), sext16(h16(1))), sext16(h16(2))));
    case 0x355: return set16(op2(dx::kImax, op2(dx::kImax, sext16(h16(0)), sext16(h16(1))), sext16(h16(2))));
    case 0x358: return set16(med3(dx::kImin, dx::kImax, sext16(h16(0)), sext16(h16(1)), sext16(h16(2))));
    case 0x353: return set16(op2(dx::kUmin, op2(dx::kUmin, h16(0), h16(1)), h16(2)));
    case 0x356: return set16(op2(dx::kUmax, op2(dx::kUmax, h16(0), h16(1)), h16(2)));
    case 0x359: return set16(med3(dx::kUmin, dx::kUmax, h16(0), h16(1), h16(2)));
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
    case 0x164: return set64(dop(dx::kDadd, d64(0), d64(1)));  // v_add_f64
    case 0x165: return set64(dop(dx::kDmul, d64(0), d64(1)));  // v_mul_f64
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
      put_select(d1, on, r1, R(d1));
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
      put_select(d1, on, r1, R(d1));
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
    case 0x368: case 0x369: {
      // v_cvt_pknorm_i16_f32 / v_cvt_pknorm_u16_f32: S0 and S1 as 16-bit
      // signed / unsigned normalised values in the low and high halves.
      const bool is_signed = op == 0x368;
      auto norm = [&](const Operand& v) {
        const Operand clamped = is_signed ? op2(dx::kMax, op2(dx::kMin, v, dxbc::immf(1.0f)), dxbc::immf(-1.0f))
                                          : op1(dx::kMov, v, dxbc::kSaturate);
        const Operand scaled = op1(dx::kRoundNe, fmul(clamped, dxbc::immf(is_signed ? 32767.0f : 65535.0f)));
        return band(is_signed ? op1(dx::kFtoi, scaled) : op1(dx::kFtou, scaled), I(0xFFFF));
      };
      return set(bor(norm(f(0)), shl(norm(f(1)), I(16))));
    }
    case 0x36a:  // v_cvt_pk_u16_u32: S0 and S1 saturated to 16 bits
      return set(bor(op2(dx::kUmin, a, I(0xFFFF)), shl(op2(dx::kUmin, b, I(0xFFFF)), I(16))));
    case 0x36b: {  // v_cvt_pk_i16_i32
      auto sat = [&](const Operand& v) {
        return band(op2(dx::kImax, op2(dx::kImin, v, I(0x7FFF)), I(0xFFFF8000u)), I(0xFFFF));
      };
      return set(bor(sat(a), shl(sat(b), I(16))));
    }
    case 0x361: {
      // v_writelane_b32: the lane named by src1 gets src0, whatever EXEC says.
      const Operand lane = band(b, I(info_.wave_size - 1));
      put_select(dreg, eq(R(kLaneR), lane), a, dval);
      return true;
    }
    case 0x365: case 0x366: {
      // v_mbcnt_lo/hi_u32_b32: the mask's bits below this lane (of lanes 0-31
      // / 32-63), plus src1. A constant mask counts by the lane's index in the
      // wave (-1: the lane-index idiom). A mask in a register is a lane mask,
      // which holds only this lane's bit: the wave of one lane it describes
      // has no lane below this one (consistent with s_bcnt1 of such masks
      // counting 1 and v_readfirstlane reading the lane's own value, as the
      // console compiler's wave-coalesced atomics expect).
      const uint16_t m = work.src[0];
      const bool constant = (m >= 128 && m <= 208) || (m >= 240 && m <= 248) || m == 255;
      if (!constant) return set(b);
      const Operand lane = R(kLaneR);
      const Operand below_lo = select(ult(lane, I(32)), sub(shl(I(1), lane), I(1)), I(0xFFFFFFFFu));
      const Operand below_hi = select(ult(lane, I(32)), I(0), sub(shl(I(1), sub(lane, I(32))), I(1)));
      return set(add(op1(dx::kCountbits, band(a, op == 0x365 ? below_lo : below_hi)), b));
    }
    case 0x36d: return set(add(add(a, b), c));
    case 0x36f: return set(bor(shl(a, b), c));
    case 0x371: return set(bor(band(a, b), c));
    case 0x372: return set(bor(bor(a, b), c));
    case 0x376: return set(sub(a, b));
    case 0x37f: return set(add(a, b));
    default: return fail(format("VOP3 op 0x%x", op));
  }
}

bool Translator::emit_export(const Instruction& inst) {
  if (stage_ == Stage::Ngg) return ngg_export(inst);
  const uint32_t target = inst.target;
  if (target == 9) return true;  // null export
  auto vgpr = [&](int i) { return R(vreg(inst.src[i] - rdna::kVgpr0)); };
  if (stage_ == Stage::Vertex) {
    uint32_t reg;
    if (target == 12) {
      reg = 0;
      iface_.position = true;
    } else if (target >= 13 && target <= 15) {
      return true;  // further position exports (point size, clip distances): not used
    } else if (target >= 32 && target < 64) {
      reg = target - 32 + 1;
      iface_.params |= 1u << (target - 32);
    } else {
      return fail(format("vertex export target %u", target));
    }
    for (int c = 0; c < 4; ++c) {
      if (inst.enable & (1 << c)) set_output(reg, static_cast<uint32_t>(c), vgpr(c));
    }
    return true;
  }
  if (stage_ != Stage::Pixel) return fail("export in a compute program");
  // Lanes no longer in EXEC are not written by the hardware: discard them.
  emit(dx::kDiscard, {active()});
  put(kExportedR, I(1));
  pixel_exports_ = true;
  if (target < 8) {
    iface_.targets |= 1u << target;
    for (int c = 0; c < 4; ++c) {
      if (!(inst.enable & (1 << c))) continue;
      Operand value;
      if (inst.compressed) {
        const Operand packed = vgpr(c / 2);
        value = op1(dx::kF16tof32, c % 2 ? shr(packed, I(16)) : packed);
      } else {
        value = vgpr(c);
      }
      set_output(target, static_cast<uint32_t>(c), value);
    }
    return true;
  }
  if (target == 8) {
    iface_.depth = true;
    if (inst.enable & 1) set_output(kDepthRegister, 0, vgpr(0));
    return true;
  }
  return fail(format("pixel export target %u", target));
}

// Image instructions. Descriptor registers: the resource index is dword 0
// of the image descriptor (s[srsrc]), the sampler index dword 0 of the
// sampler descriptor (s[ssamp]). Address registers in the hardware order:
// bias or depth reference first, then the coordinates, then the LOD.
//
// D16 (bit 63, documented): data is 16 bits a component, two components a
// register (component 2k in the low half of data register k, 2k + 1 in the
// high half). Host views are float views, so values pass as half floats.
bool Translator::emit_image(const Instruction& inst) {
  const uint32_t op = inst.op;
  const bool d16 = (inst.raw[1] >> 31) & 1;
  const uint32_t nsa = (inst.raw[0] >> 1) & 3;
  struct Dim {
    uint32_t coords;
    uint32_t slot;
  };
  // Dimensions: 3 cube (face coordinates s, t and the face; see cube_direction),
  // 6 multisampled 2D (x, y, then the sample for loads).
  static const Dim kDims[8] = {{1, kImage1D}, {2, kImage2D}, {3, kImage3D}, {3, kImageCube}, {2, kImage1DArray},
                               {3, kImage2DArray}, {2, kImage2DMS}, {3, 0xFF}};
  const Dim dim = kDims[inst.dim & 7];
  if (dim.slot == 0xFF) return fail(format("image dimension %u (multisampled array)", inst.dim));
  const bool cube = dim.slot == kImageCube;
  if (dim.slot == kImage2DMS && op != 0 && op != 14) return fail(format("image instruction %u on a multisampled image", op));
  if (cube && (op == 0 || op == 1 || op == 8 || op == 9)) return fail("image load or store on a cube");
  auto address = [&](uint32_t i) {
    if (i == 0 || !nsa) return R(vreg(inst.src[0] - rdna::kVgpr0 + i));
    return R(vreg(inst.nsa[(i - 1) % 12]));
  };
  const uint32_t data = inst.src[1] - rdna::kVgpr0;
  const Operand index = copy(R(inst.src[2]));
  const uint32_t index_reg = kScratch + scratch_ - 1;
  // A temp holding the address components packed (coordinates, then extra).
  auto pack = [&](uint32_t first, uint32_t count, const Operand* extra) {
    const uint32_t t = fresh();
    for (uint32_t c = 0; c < count; ++c) emit(dx::kMov, {dxbc::temp_dst_mask(t, 1u << c), address(first + c)});
    if (extra) emit(dx::kMov, {dxbc::temp_dst_mask(t, 1u << count), *extra});
    for (uint32_t c = count + (extra ? 1 : 0); c < 4; ++c) emit(dx::kMov, {dxbc::temp_dst_mask(t, 1u << c), I(0)});
    return t;
  };
  // Result components in order into the data registers (packed with D16).
  auto write_results = [&](uint32_t result, uint32_t mask) {
    uint32_t k = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      if (!(mask & (1u << c))) continue;
      if (!d16) {
        const uint32_t d = vreg(data + k++);
        put_select(d, active(), dxbc::temp(result, c), R(d));
        continue;
      }
      const uint32_t d = vreg(data + k / 2);
      const Operand half = op1(dx::kF32tof16, dxbc::temp(result, c));
      const Operand merged = op4(dx::kBfi, I(16), I(k % 2 ? 16 : 0), half, R(d));
      put_select(d, active(), merged, R(d));
      ++k;
    }
  };
  auto results = [&](uint32_t result) { write_results(result, inst.dmask); };
  const uint32_t texture_id = 10 + dim.slot, uav_id = 2 + dim.slot;
  const bool atomic = op >= 15 && op <= 26 && op != 19;
  if (op != 8 && op != 9 && !atomic) textures_used_ |= 1u << dim.slot;
  switch (op) {
    case 0: case 1: {  // image_load, image_load_mip: integer coordinates (+ mip)
      if (dim.slot == kImage2DMS) {
        // Multisampled: x, y, then the sample index.
        const uint32_t coords = pack(0, 2, nullptr);
        const uint32_t result = fresh();
        emit(dx::kLdMs, {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords), dxbc::texture_indexed(texture_id, index_reg), address(2)});
        results(result);
        return true;
      }
      const Operand mip = op == 1 ? address(dim.coords) : I(0);
      const uint32_t coords = pack(0, dim.coords, &mip);
      const uint32_t result = fresh();
      emit(dx::kLd, {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords), dxbc::texture_indexed(texture_id, index_reg)});
      results(result);
      return true;
    }
    case 8: case 9: {  // image_store, image_store_mip
      uavs_used_ |= 1u << dim.slot;
      if (op == 9) {
        // The view selects the mip level; only level 0 of it is expressible.
        if_nz(ne(address(dim.coords), I(0)));
        emit(dx::kMov, {D(index_reg), I(0xFFFFFFFFu)});  // out of the heap: the store is dropped
        end_if();
      }
      const uint32_t coords = pack(0, dim.coords, nullptr);
      const uint32_t value = fresh();
      uint32_t k = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        Operand v = I(0);
        if (inst.dmask & (1u << c)) {
          if (d16) {
            const Operand packed = R(vreg(data + k / 2));
            v = op1(dx::kF16tof32, k % 2 ? shr(packed, I(16)) : packed);
          } else {
            v = R(vreg(data + k));
          }
          ++k;
        }
        emit(dx::kMov, {dxbc::temp_dst_mask(value, 1u << c), v});
      }
      if_nz(active());
      emit(dx::kStoreUavTyped,
           {dxbc::typed_uav_indexed(uav_id, index_reg, true), dxbc::temp_xyzw(coords), dxbc::temp_xyzw(value)});
      end_if();
      return true;
    }
    case 15: case 16: case 17: case 18: case 20: case 21: case 22: case 23: case 24: case 25: case 26: {
      // Image atomics (documented): on the 32-bit texel at integer
      // coordinates, with the data in vdata (cmpswap: the new value, then the
      // value compared); with GLC the texel's previous value returns in vdata.
      if (cube || dim.slot == kImage2DMS) return fail(format("image atomic %u on a cube or multisampled image", op));
      if (!d16 && inst.dmask == 3 && op != 16) return image_atomic64(inst, dim.slot, dim.coords, index_reg);
      if (d16 || (inst.dmask != 1 && !(op == 16 && inst.dmask == 3))) {
        return fail(format("image atomic %u of other than 32 bits", op));
      }
      integer_uavs_used_ |= 1u << dim.slot;
      const uint32_t coords = pack(0, dim.coords, nullptr);
      const Operand uav = dxbc::typed_uav_atomic(30 + dim.slot, index_reg);
      const Operand value = R(vreg(data));
      const uint32_t old = fresh();
      if_nz(active());
      if (op == 16) {
        emit(dx::kImmAtomicCmpExch, {D(old), uav, dxbc::temp_xyzw(coords), R(vreg(data + 1)), value});
      } else if (op == 18) {
        emit(dx::kImmAtomicIadd, {D(old), uav, dxbc::temp_xyzw(coords), op1(dx::kIneg, value)});
      } else {
        static const uint32_t kOps[] = {dx::kImmAtomicExch, 0, dx::kImmAtomicIadd, 0, 0, dx::kImmAtomicImin,
                                        dx::kImmAtomicUmin, dx::kImmAtomicImax, dx::kImmAtomicUmax,
                                        dx::kImmAtomicAnd, dx::kImmAtomicOr, dx::kImmAtomicXor};
        emit(kOps[op - 15], {D(old), uav, dxbc::temp_xyzw(coords), value});
      }
      if (inst.glc) put(vreg(data), R(old));
      end_if();
      return true;
    }
    case 14: {  // image_get_resinfo: width, height, depth or layers, mip levels of the given level
      if (d16) return fail("image_get_resinfo with 16-bit data");
      const uint32_t result = fresh();
      emit(dx::kResinfo, {dxbc::temp_dst_mask(result, 0xF), address(0), dxbc::texture_indexed(texture_id, index_reg)}, 2u << 11);
      // resinfo_uint gives width, height, depth/elements, levels.
      uint32_t k = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(inst.dmask & (1u << c))) continue;
        const uint32_t d = vreg(data + k++);
        put_select(d, active(), dxbc::temp(result, c), R(d));
      }
      return true;
    }
    default: break;
  }
  // Sampling (loads and stores take integer coordinates whatever the
  // unnormalised bit says). Unnormalised coordinates (texels) are divided by
  // the size of level 0 (array slices and cube faces stay as they are).
  const bool unnormalised = (inst.raw[0] >> 12) & 1;
  if (unnormalised && cube) return fail("unnormalised cube coordinates");
  uint32_t size_reg = kNoRegister;
  if (unnormalised) {
    size_reg = fresh();
    emit(dx::kResinfo, {dxbc::temp_dst_mask(size_reg, 0xF), I(0), dxbc::texture_indexed(texture_id, index_reg)});
  }
  const uint32_t scaled = dim.slot == kImage1D || dim.slot == kImage1DArray ? 1 : dim.slot == kImage3D ? 3 : 2;
  // Coordinates from address `first` on, as a vector: cube face coordinates
  // become a direction, unnormalised ones normalised.
  auto coordinates = [&](uint32_t first) {
    const uint32_t t = pack(first, dim.coords, nullptr);
    if (cube) cube_direction(t);
    if (unnormalised) {
      for (uint32_t c = 0; c < scaled; ++c) {
        emit(dx::kDiv, {dxbc::temp_dst_mask(t, 1u << c), dxbc::temp(t, c), dxbc::temp(size_reg, c)});
      }
    }
    return t;
  };
  samplers_used_ = true;
  const Operand sampler_index = copy(R(inst.src[3]));
  const uint32_t sampler_reg = kScratch + scratch_ - 1;
  const bool pixel = stage_ == Stage::Pixel;
  // Gathers (level 0 whatever the variant asks, as on the desktop): the
  // offsets (6-bit signed x in bits 0-5, y in bits 8-13), then the depth
  // reference, then the coordinates; dmask names the gathered component.
  // (gather4_l 68 / 84 with offsets and gather4_c_l 76 / 92 read level 0
  // too; their level follows the coordinates and is not read.)
  const bool gather = op == 64 || op == 68 || op == 71 || op == 72 || op == 76 || op == 79 || op == 80 || op == 84 ||
                      op == 87 || op == 88 || op == 92 || op == 95;
  if (gather) {
    const bool offsets = op >= 80;
    const bool compare = op == 72 || op == 76 || op == 79 || op == 88 || op == 92 || op == 95;
    uint32_t component = 0;
    while (component < 4 && !(inst.dmask & (1u << component))) ++component;
    if (component == 4 || (inst.dmask & ~(1u << component))) return fail("gather of several components");
    const uint32_t at = (offsets ? 1 : 0) + (compare ? 1 : 0);
    const uint32_t coords = coordinates(at);
    const uint32_t result = fresh();
    std::vector<Operand> operands = {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords)};
    if (offsets) {
      const uint32_t offset = fresh();
      emit(dx::kIbfe, {dxbc::temp_dst_mask(offset, 1), I(6), I(0), address(0)});
      emit(dx::kIbfe, {dxbc::temp_dst_mask(offset, 2), I(6), I(8), address(0)});
      emit(dx::kMov, {dxbc::temp_dst_mask(offset, 0xC), I(0)});
      operands.push_back(dxbc::temp_xyzw(offset));
    }
    operands.push_back(dxbc::texture_indexed(texture_id, index_reg));
    operands.push_back(dxbc::sampler_indexed_select(0, sampler_reg, component));
    if (compare) operands.push_back(address(offsets ? 1 : 0));
    const uint32_t opcode = offsets ? (compare ? dx::kGather4PoC : dx::kGather4Po) : (compare ? dx::kGather4C : dx::kGather4);
    emit(opcode, operands);
    // All four gathered texels come back, in registers data .. data + 3
    // (data .. data + 1 with D16).
    write_results(result, 0xF);
    return true;
  }
  uint32_t first = 0;
  Operand extra = I(0);
  uint32_t opcode = 0;
  bool with_extra = false;
  switch (op) {
    case 32: opcode = pixel ? dx::kSample : dx::kSampleL; break;             // image_sample
    case 36: opcode = dx::kSampleL; with_extra = true; break;               // image_sample_l
    case 37: opcode = pixel ? dx::kSampleB : dx::kSampleL; first = 1; break;  // image_sample_b
    case 39: opcode = dx::kSampleL; break;                                   // image_sample_lz
    case 40: opcode = pixel ? dx::kSampleC : dx::kSampleCLz; first = 1; break;  // image_sample_c
    case 47: opcode = dx::kSampleCLz; first = 1; break;                      // image_sample_c_lz
    case 34: case 35: case 42: case 43: {
      // image_sample_d (c_d: a depth reference first, sampled at level 0 as
      // the desktop has no compare with gradients): the x gradients, the y
      // gradients (one per dimension), then the coordinates (_cl: then a
      // level clamp, not applied).
      // (Cubes: face-coordinate gradients, two each, made direction gradients
      // on the coordinates' face.)
      const uint32_t grads = scaled;
      const uint32_t at = op >= 42 ? 1 : 0;
      const uint32_t dx_reg = pack(at, grads, nullptr), dy_reg = pack(at + grads, grads, nullptr);
      if (cube) {
        const Operand face = in_temp(band(op1(dx::kFtou, address(at + 2 * grads + 2)), I(7)));
        for (const uint32_t g : {dx_reg, dy_reg}) {
          const Operand gs = in_temp(dxbc::temp(g, 0)), gt = in_temp(dxbc::temp(g, 1));
          cube_vector(g, face, gs, gt, 0.0f);
        }
      }
      const uint32_t coords = coordinates(at + 2 * grads);
      const uint32_t result = fresh();
      if (op <= 35) {
        emit(dx::kSampleD, {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords), dxbc::texture_indexed(texture_id, index_reg),
                            dxbc::sampler_indexed(0, sampler_reg), dxbc::temp_xyzw(dx_reg), dxbc::temp_xyzw(dy_reg)});
      } else {
        emit(dx::kSampleCLz, {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords), dxbc::texture_indexed(texture_id, index_reg),
                              dxbc::sampler_indexed(0, sampler_reg), address(0)});
      }
      results(result);
      return true;
    }
    case 96: {
      // image_get_lod: the level a sample would use (clamped, unclamped).
      // Outside pixel shaders the console derives it from quads of lanes;
      // partial: level 0 here, the level the implicit samples above take
      // there (observed: 65 compute programs of a title query it).
      if (!pixel) {
        const uint32_t result = fresh();
        emit(dx::kMov, {dxbc::temp_dst_mask(result, 0xF), dxbc::immf(0.0f)});
        results(result);
        return true;
      }
      const uint32_t coords = coordinates(0);
      const uint32_t result = fresh();
      emit(dx::kLod, {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords), dxbc::texture_indexed(texture_id, index_reg),
                      dxbc::sampler_indexed(0, sampler_reg)});
      results(result);
      return true;
    }
    default: return fail(format("image instruction %u", op));
  }
  const uint32_t coords = coordinates(first);
  const uint32_t result = fresh();
  std::vector<Operand> operands = {dxbc::temp_dst_mask(result, 0xF), dxbc::temp_xyzw(coords), dxbc::texture_indexed(texture_id, index_reg),
                                   dxbc::sampler_indexed(0, sampler_reg)};
  if (opcode == dx::kSampleL) {
    // Explicit level: the last address (image_sample_l), the bias as a level
    // outside pixel shaders, else 0.
    operands.push_back(with_extra ? address(first + dim.coords) : op == 37 ? address(0) : dxbc::immf(0.0f));
  } else if (opcode == dx::kSampleB) {
    operands.push_back(address(0));
  } else if (opcode == dx::kSampleC || opcode == dx::kSampleCLz) {
    operands.push_back(address(0));  // depth reference
  }
  (void)extra;
  (void)sampler_index;
  (void)index;
  emit(opcode, operands);
  results(result);
  return true;
}

// 64-bit image atomics (documented: dmask 0x3, the value in two registers,
// low dword first; observed: titles' visibility buffers keep the maximum of
// depth and an id this way). The host bytecode has no 64-bit atomics and no
// typed atomics on 64-bit texels: the texel (two 32-bit channels) is read,
// combined and written. In pixel programs the view is rasterizer-ordered,
// which makes that exclusive and ordered per pixel (exact when, as observed,
// lanes access their own pixel); elsewhere it is not atomic.
bool Translator::image_atomic64(const Instruction& inst, uint32_t slot, uint32_t coord_count, uint32_t index_reg) {
  const uint32_t op = inst.op;
  if (op == 27 || op == 28 || op > 26) return fail(format("64-bit image atomic %u", op));
  const uint32_t data = inst.src[1] - rdna::kVgpr0;
  integer_uavs_used_ |= 1u << slot;
  features_ |= dxbc::kRequiresTypedUavLoadAdditionalFormats;
  if (stage_ == Stage::Pixel) {
    ordered_uavs_ = true;
    features_ |= dxbc::kRequiresRovs;
  }
  const uint32_t coords = fresh();
  for (uint32_t c = 0; c < 4; ++c) {
    const Operand v = c < coord_count ? R(vreg(inst.src[0] - rdna::kVgpr0 + c)) : I(0);
    emit(dx::kMov, {dxbc::temp_dst_mask(coords, 1u << c), v});
  }
  if (inst.raw[0] & 6) return fail("64-bit image atomic with separate address registers");
  if_nz(active());
  const uint32_t old = fresh();
  emit(dx::kLdUavTyped,
       {dxbc::temp_dst_mask(old, 0x3), dxbc::temp_xyzw(coords), dxbc::typed_uav_indexed(30 + slot, index_reg, false)});
  const Operand lo_old = in_temp(dxbc::temp(old, 0)), hi_old = in_temp(dxbc::temp(old, 1));
  const Operand lo = in_temp(R(vreg(data))), hi = in_temp(R(vreg(data + 1)));
  Operand lo_new, hi_new;
  if (op == 20 || op == 21 || op == 22 || op == 23) {
    // Signed (20 min, 22 max) or unsigned (21, 23) 64-bit comparisons: the
    // high dwords decide, the low ones (unsigned) when they are equal.
    const bool is_signed = op == 20 || op == 22, is_max = op == 22 || op == 23;
    const Operand hi_less = is_signed ? op2(dx::kIlt, hi, hi_old) : ult(hi, hi_old);
    const Operand hi_more = is_signed ? op2(dx::kIlt, hi_old, hi) : ult(hi_old, hi);
    const Operand lo_less = ult(lo, lo_old), lo_more = ult(lo_old, lo);
    const Operand wins = in_temp(is_max ? bor(hi_more, band(eq(hi, hi_old), lo_more))
                                        : bor(hi_less, band(eq(hi, hi_old), lo_less)));
    lo_new = select(wins, lo, lo_old);
    hi_new = select(wins, hi, hi_old);
  } else if (op == 24) {
    lo_new = band(lo, lo_old);
    hi_new = band(hi, hi_old);
  } else if (op == 25) {
    lo_new = bor(lo, lo_old);
    hi_new = bor(hi, hi_old);
  } else if (op == 26) {
    lo_new = bxor(lo, lo_old);
    hi_new = bxor(hi, hi_old);
  } else if (op == 15) {
    lo_new = lo;
    hi_new = hi;
  } else if (op == 17 || op == 18) {
    // Add (sub: of the negated value) with the carry into the high dword.
    const Operand nlo = op == 18 ? in_temp(op1(dx::kIneg, lo)) : lo;
    const Operand nhi = op == 18 ? in_temp(sub(bnot(hi), select(eq(lo, I(0)), I(0xFFFFFFFFu), I(0)))) : hi;
    const Operand sum = in_temp(add(lo_old, nlo));
    lo_new = sum;
    hi_new = add(add(hi_old, nhi), band(ult(sum, lo_old), I(1)));
  } else {
    end_if();
    return fail(format("64-bit image atomic %u", op));
  }
  const uint32_t value = fresh();
  emit(dx::kMov, {dxbc::temp_dst_mask(value, 1), lo_new});
  emit(dx::kMov, {dxbc::temp_dst_mask(value, 2), hi_new});
  emit(dx::kMov, {dxbc::temp_dst_mask(value, 0xC), I(0)});
  emit(dx::kStoreUavTyped,
       {dxbc::typed_uav_indexed(30 + slot, index_reg, true), dxbc::temp_xyzw(coords), dxbc::temp_xyzw(value)});
  if (inst.glc) {
    put(vreg(data), lo_old);
    put(vreg(data + 1), hi_old);
  }
  end_if();
  return true;
}

// Packed and mixed-precision operations (VOP3P).
bool Translator::emit_vop3p(const Instruction& inst) {
  const uint32_t d = vreg(inst.dst);
  switch (inst.op) {
    case 32: case 33: case 34: {
      // v_fma_mix_f32, v_fma_mixlo_f16, v_fma_mixhi_f16: each source is f32
      // (opsel_hi clear) or the f16 half opsel names; neg_hi is |x|.
      Operand s[3];
      for (int i = 0; i < 3; ++i) {
        Operand v = src(inst, inst.src[i]);
        if ((inst.opsel_hi >> i) & 1) {
          v = op1(dx::kF16tof32, (inst.opsel >> i) & 1 ? shr(v, I(16)) : band(v, I(0xFFFF)));
        }
        if (((inst.neg_hi | inst.neg) >> i) & 1) {
          v = in_temp(v);
          if ((inst.neg_hi >> i) & 1) v = dxbc::abs(v);
          if ((inst.neg >> i) & 1) v = dxbc::neg(v);
        }
        s[i] = v;
      }
      Operand r = op3(dx::kMad, s[0], s[1], s[2]);
      if (inst.clamp) r = op1(dx::kMov, r, dxbc::kSaturate);
      if (inst.op == 32) {
        put_select(d, active(), r, R(d));
        return true;
      }
      const Operand half = op1(dx::kF32tof16, r);
      const Operand merged = inst.op == 33 ? op4(dx::kBfi, I(16), I(0), half, R(d))
                                           : op4(dx::kBfi, I(16), I(16), half, R(d));
      put_select(d, active(), merged, R(d));
      return true;
    }
    default: break;
  }
  // Packed 16-bit operations (documented): each half of the result takes, from
  // each source, the half op_sel (low result) or op_sel_hi (high result) names;
  // float sources are negated by neg (low) or neg_hi (high).
  const uint32_t op = inst.op;
  auto half = [&](int i, bool high) {
    const Operand v = src(inst, inst.src[i]);
    const bool upper = ((high ? inst.opsel_hi : inst.opsel) >> i) & 1;
    return upper ? shr(v, I(16)) : band(v, I(0xFFFF));
  };
  auto fhalf = [&](int i, bool high) {
    Operand f = op1(dx::kF16tof32, half(i, high));
    if (((high ? inst.neg_hi : inst.neg) >> i) & 1) f = dxbc::neg(f);
    return f;
  };
  auto sext16 = [&](const Operand& v) { return ibfe(I(16), I(0), v); };
  auto lane = [&](bool high) -> Operand {
    switch (op) {
      case 0: return add(mul_lo(sext16(half(0, high)), sext16(half(1, high))), sext16(half(2, high)));
      case 1: return mul_lo(half(0, high), half(1, high));
      case 2: case 10: return add(half(0, high), half(1, high));
      case 3: case 11: return sub(half(0, high), half(1, high));
      case 4: return shl(half(1, high), band(half(0, high), I(15)));
      case 5: return shr(half(1, high), band(half(0, high), I(15)));
      case 6: return sar(sext16(half(1, high)), band(half(0, high), I(15)));
      case 7: return op2(dx::kImax, sext16(half(0, high)), sext16(half(1, high)));
      case 8: return op2(dx::kImin, sext16(half(0, high)), sext16(half(1, high)));
      case 9: return add(mul_lo(half(0, high), half(1, high)), half(2, high));
      case 12: return op2(dx::kUmax, half(0, high), half(1, high));
      case 13: return op2(dx::kUmin, half(0, high), half(1, high));
      default: break;
    }
    Operand f;
    switch (op) {
      case 14: f = op3(dx::kMad, fhalf(0, high), fhalf(1, high), fhalf(2, high)); break;
      case 15: f = op2(dx::kAdd, fhalf(0, high), fhalf(1, high)); break;
      case 16: f = op2(dx::kMul, fhalf(0, high), fhalf(1, high)); break;
      case 17: f = op2(dx::kMin, fhalf(0, high), fhalf(1, high)); break;
      default: f = op2(dx::kMax, fhalf(0, high), fhalf(1, high)); break;
    }
    if (inst.clamp) f = op1(dx::kMov, f, dxbc::kSaturate);
    return op1(dx::kF32tof16, f);
  };
  if (op <= 18) {
    if (inst.clamp && op <= 13) return fail(format("VOP3P op %u with clamp", op));
    const Operand lo = band(lane(false), I(0xFFFF));
    const Operand hi = shl(lane(true), I(16));
    put_select(d, active(), bor(lo, hi), R(d));
    return true;
  }
  // Dot products into 32 bits: halves (bytes, nibbles) in order, plus src2.
  if (op == 19) {  // v_dot2_f32_f16
    Operand sum = op3(dx::kMad, fhalf(0, false), fhalf(1, false), src(inst, inst.src[2]));
    sum = op3(dx::kMad, fhalf(0, true), fhalf(1, true), sum);
    if (inst.clamp) sum = op1(dx::kMov, sum, dxbc::kSaturate);
    put_select(d, active(), sum, R(d));
    return true;
  }
  if (op >= 20 && op <= 25) {
    if (inst.clamp) return fail(format("VOP3P op %u with clamp", op));
    const bool is_signed = op == 20 || op == 22 || op == 24;
    const uint32_t bits = op <= 21 ? 16 : op <= 23 ? 8 : 4;
    const Operand a = in_temp(src(inst, inst.src[0])), b = in_temp(src(inst, inst.src[1]));
    Operand sum = src(inst, inst.src[2]);
    for (uint32_t at = 0; at < 32; at += bits) {
      const Operand x = is_signed ? ibfe(I(bits), I(at), a) : ubfe(I(bits), I(at), a);
      const Operand y = is_signed ? ibfe(I(bits), I(at), b) : ubfe(I(bits), I(at), b);
      sum = add(mul_lo(x, y), sum);
    }
    put_select(d, active(), sum, R(d));
    return true;
  }
  return fail(format("VOP3P op %u", op));
}

bool Translator::cross_lane(const Instruction& inst) {
  const bool vector = inst.encoding == Encoding::VOP1 || inst.encoding == Encoding::VOP2 ||
                      inst.encoding == Encoding::VOPC;
  if (vector && (inst.src[0] == rdna::kDpp16 || inst.src[0] == rdna::kDpp8 || inst.src[0] == rdna::kDpp8Fi)) {
    return true;
  }
  // ds_swizzle_b32 and ds_bpermute_b32 read other lanes through the exchange.
  if (inst.encoding == Encoding::DS && (inst.op == 53 || inst.op == 179)) return true;
  return inst.encoding == Encoding::VOP3 && (inst.op == 0x360 || inst.op == 0x377 || inst.op == 0x378);
}

namespace {
// The VGPR whose value an exchange carries.
uint16_t exchanged_vgpr(const Instruction& inst) {
  if (inst.encoding == Encoding::DS) return inst.src[1];
  if (inst.encoding == Encoding::VOP3) return inst.src[0];
  return static_cast<uint16_t>(rdna::kVgpr0 + (inst.ext & 0xFF));
}
}  // namespace

void Translator::exchange_write(const Instruction& inst) {
  exchange_ = true;
  const uint32_t buffer = exchange_parity_ * info_.threads[0] * info_.threads[1] * info_.threads[2] * 8;
  const Operand address =
      add(shl(dxbc::input(dxbc::kThreadIdInGroupFlattened, 0), I(3)), I(buffer));
  emit(dx::kStoreRaw, {dxbc::group_shared(1, dxbc::Access::Store), address, src(inst, exchanged_vgpr(inst))});
  emit(dx::kStoreRaw, {dxbc::group_shared(1, dxbc::Access::Store), add(address, I(4)), active()});
}

bool Translator::exchange_read(const Instruction& inst) {
  const uint32_t buffer = exchange_parity_ * info_.threads[0] * info_.threads[1] * info_.threads[2] * 8;
  exchange_parity_ ^= 1;
  const Operand lane = R(kLaneR);
  // Source lane and whether this lane writes, per operation.
  Operand source, valid = I(0xFFFFFFFFu), write = I(0xFFFFFFFFu);
  bool fetch_inactive = false, bound_zero = false;
  if (inst.encoding == Encoding::DS) {
    // Documented: ds_bpermute_b32 reads lane (address + offset) / 4;
    // ds_swizzle_b32 reads within groups of four lanes (offset bit 15: two
    // bits a lane in offset[7:0]) or within groups of 32 lanes by masks
    // (((lane & and) | or) ^ xor, the masks in offset[4:0], [9:5], [14:10]).
    // An inactive source lane gives 0.
    const uint32_t offset = static_cast<uint32_t>(inst.imm) & 0xFFFF;
    if (inst.op == 179) {
      const Operand address = add(R(vreg(inst.src[0] - rdna::kVgpr0)), I(offset));
      source = band(shr(address, I(2)), I(info_.wave_size - 1));
    } else if (offset & 0x8000) {
      source = add(band(lane, I(~3u)), band(shr(I(offset), shl(band(lane, I(3)), I(1))), I(3)));
    } else {
      const uint32_t and_mask = offset & 31, or_mask = (offset >> 5) & 31, xor_mask = (offset >> 10) & 31;
      source = add(band(lane, I(~31u)), bxor(bor(band(lane, I(and_mask)), I(or_mask)), I(xor_mask)));
    }
    bound_zero = true;
  } else if (inst.encoding == Encoding::VOP3 && inst.op == 0x360) {
    // v_readlane_b32 sdst, vsrc, lane: every lane reads the same lane.
    source = band(src(inst, inst.src[1]), I(info_.wave_size - 1));
    fetch_inactive = true;
  } else if (inst.encoding == Encoding::VOP3) {
    // v_permlane16 / v_permlanex16: 4-bit selects from src1 (lanes 0-7 of a
    // row) and src2 (lanes 8-15); x16 reads the other row of each pair.
    const Operand in_row = band(lane, I(15));
    const Operand word = select(ult(in_row, I(8)), src(inst, inst.src[1]), src(inst, inst.src[2]));
    const Operand pick = ubfe(I(4), shl(band(in_row, I(7)), I(2)), word);
    Operand row = band(lane, I(~15u));
    if (inst.op == 0x378) row = bxor(row, I(16));
    source = add(row, pick);
    fetch_inactive = (inst.opsel & 1) != 0;
    bound_zero = (inst.opsel & 2) != 0;
  } else if (inst.src[0] == rdna::kDpp8 || inst.src[0] == rdna::kDpp8Fi) {
    const uint32_t selects = inst.ext >> 8;
    source = add(band(lane, I(~7u)), ubfe(I(3), mul_lo(I(3), band(lane, I(7))), I(selects)));
    fetch_inactive = inst.src[0] == rdna::kDpp8Fi;
    bound_zero = true;
  } else {
    const uint32_t control = (inst.ext >> 8) & 0x1FF;
    fetch_inactive = (inst.ext >> 18) & 1;
    bound_zero = (inst.ext >> 19) & 1;
    const Operand row = band(lane, I(~15u)), in_row = band(lane, I(15));
    const uint32_t n = control & 15;
    if (control < 0x100) {
      const Operand shift = shl(band(lane, I(3)), I(1));
      source = add(band(lane, I(~3u)), band(shr(I(control), shift), I(3)));
    } else if (control >= 0x101 && control <= 0x10F) {
      source = add(lane, I(n));
      valid = ult(add(in_row, I(n)), I(16));
    } else if (control >= 0x111 && control <= 0x11F) {
      source = sub(lane, I(n));
      valid = op2(dx::kUge, in_row, I(n));
    } else if (control >= 0x121 && control <= 0x12F) {
      source = add(row, band(sub(in_row, I(n)), I(15)));
    } else if (control == 0x140) {
      source = add(row, sub(I(15), in_row));
    } else if (control == 0x141) {
      source = add(band(lane, I(~7u)), sub(I(7), band(lane, I(7))));
    } else if (control >= 0x150 && control <= 0x15F) {
      source = add(row, I(n));
    } else if (control >= 0x160 && control <= 0x16F) {
      source = add(row, bxor(in_row, I(n)));
    } else {
      return fail(format("DPP control 0x%x", control));
    }
    const uint32_t bank_mask = (inst.ext >> 24) & 15, row_mask = (inst.ext >> 28) & 15;
    const Operand row_bit = band(shr(I(row_mask), shr(lane, I(4))), I(1));
    const Operand bank_bit = band(shr(I(bank_mask), band(shr(lane, I(2)), I(3))), I(1));
    write = nonzero(band(row_bit, bank_bit));
  }
  // Read the source lane's value and EXEC bit (lanes of this wave only).
  const Operand wave_base = sub(dxbc::input(dxbc::kThreadIdInGroupFlattened, 0), lane);
  const Operand address = add(shl(add(wave_base, source), I(3)), I(buffer));
  const uint32_t value = fresh(), source_active = fresh();
  emit(dx::kLdRaw, {D(value), address, dxbc::group_shared(1, dxbc::Access::Load)});
  emit(dx::kLdRaw, {D(source_active), add(address, I(4)), dxbc::group_shared(1, dxbc::Access::Load)});
  return finish_exchange(inst, value, valid, write, fetch_inactive, bound_zero, R(source_active));
}

// The operation of a cross-lane instruction once the source lane's value is
// in `value`: source validity, DPP modifiers, and the operation itself in
// the lanes that write.
bool Translator::finish_exchange(const Instruction& inst, uint32_t value, Operand valid, Operand write,
                                 bool fetch_inactive, bool bound_zero, const Operand& source_active) {
  if (!fetch_inactive) valid = band(valid, nonzero(source_active));
  if (bound_zero) {
    put_select(value, valid, R(value), I(0));
  } else {
    write = band(write, valid);
  }
  if (inst.encoding == Encoding::VOP3 && inst.op == 0x360) {
    set_s(inst.dst, R(value));
    return true;
  }
  if (inst.encoding == Encoding::DS) {
    const uint32_t d = vreg(inst.dst);
    put_select(d, active(), R(value), R(d));
    return true;
  }
  Instruction work = inst;
  if (inst.encoding != Encoding::VOP3 && inst.src[0] == rdna::kDpp16) {
    if ((inst.ext >> 21) & 1) put(value, band(R(value), I(0x7FFFFFFFu)));  // |src0|
    if ((inst.ext >> 20) & 1) put(value, bxor(R(value), I(0x80000000u)));  // -src0
    if ((inst.ext >> 22) & 3) {
      if (inst.encoding == Encoding::VOP1) return fail("DPP source 1 modifiers without a source 1");
      modified_reg_ = fresh();
      put(modified_reg_, src(inst, inst.src[1]));
      if ((inst.ext >> 23) & 1) put(modified_reg_, band(R(modified_reg_), I(0x7FFFFFFFu)));  // |src1|
      if ((inst.ext >> 22) & 1) put(modified_reg_, bxor(R(modified_reg_), I(0x80000000u)));  // -src1
      work.src[1] = kModifiedOperand;
    }
  }
  // The operation itself, with src0 the exchanged value, in lanes that write.
  const uint32_t saved = fresh();
  put(saved, R(kExecLoR));
  put(kExecLoR, band(R(kExecLoR), bit01(write)));
  exchange_reg_ = value;
  bool ok;
  if (inst.encoding == Encoding::VOP3) {
    // v_permlane: vdst = the exchanged value.
    const uint32_t d = vreg(inst.dst);
    put_select(d, active(), R(value), R(d));
    ok = true;
  } else {
    work.src[0] = kExchangeOperand;
    ok = emit_vector(work);
  }
  put(kExecLoR, R(saved));
  return ok;
}

// Cross-lane operations in pixel (and vertex) programs. A host pixel shader
// has no exchange memory, but fine derivatives carry a quad neighbour's value
// as a difference: with x the value, the horizontal neighbour's is
// x + ddx_fine(x) in the left column and x - ddx_fine(x) in the right one,
// likewise vertically, and the diagonal one is the vertical neighbour of the
// horizontal one. Values are exchanged as two floats holding 16 and 17 bits
// (the low half and the source lane's EXEC bit), so every step is exact. The
// quad position (console lane order: lane 0 top left, 1 top right, 2 bottom
// left, 3 bottom right) comes from the pixel position. Like
// implicit-derivative sampling, this requires the four lanes of a quad to
// reach the instruction together.
//
// The wave is taken to hold this quad alone, as lanes 0-3 (a packing the
// console GPU may also choose: programs cannot rely on others): an operation
// reading a lane outside it reads a lane without a pixel. Such lanes hold, in
// whole-wave code, what the program gives inactive lanes: the console
// compiler's reductions give them the identity of the reduction's operation.
// So a lane outside the quad reads as the identity of the operation consuming
// it where there is one (exact reductions over the quad), as an invalid
// source for moves (no write, or zero with bound control), and as the lane's
// own value for lane reads (the reduced value every quad lane then holds).
// Vertex programs have no quads: their wave is the lane alone.
bool Translator::quad_read(const Instruction& inst) {
  const bool quad = stage_ == Stage::Pixel;
  Operand me = I(0);
  Operand x, y;
  if (quad) {
    quad_ = true;
    x = band(op1(dx::kFtou, dxbc::input_register(0, 0)), I(1));
    y = band(op1(dx::kFtou, dxbc::input_register(0, 1)), I(1));
    me = in_temp(bor(x, shl(y, I(1))));
  }
  const uint32_t wave = info_.wave_size ? info_.wave_size : 64;
  Operand source, valid = I(0xFFFFFFFFu), write = I(0xFFFFFFFFu);
  bool fetch_inactive = false, bound_zero = false;
  enum class Outside { Invalid, Identity, Own } outside = Outside::Invalid;
  uint32_t identity = 0;
  if (inst.encoding == Encoding::DS) {
    // ds_bpermute_b32 / ds_swizzle_b32 (see exchange_read): an inactive
    // source lane gives 0.
    const uint32_t offset = static_cast<uint32_t>(inst.imm) & 0xFFFF;
    if (inst.op == 179) {
      const Operand address = add(R(vreg(inst.src[0] - rdna::kVgpr0)), I(offset));
      source = band(shr(address, I(2)), I(wave - 1));
    } else if (offset & 0x8000) {
      source = band(shr(I(offset), shl(me, I(1))), I(3));
    } else {
      const uint32_t and_mask = offset & 31, or_mask = (offset >> 5) & 31, xor_mask = (offset >> 10) & 31;
      source = bxor(bor(band(me, I(and_mask)), I(or_mask)), I(xor_mask));
    }
    bound_zero = true;
  } else if (inst.encoding == Encoding::VOP3 && inst.op == 0x360) {
    // v_readlane_b32: the named lane; outside the quad, the lane's own value.
    source = band(src(inst, inst.src[1]), I(wave - 1));
    fetch_inactive = true;
    outside = Outside::Own;
  } else if (inst.encoding == Encoding::VOP3) {
    // v_permlane16 / v_permlanex16 (see exchange_read).
    const Operand word = select(ult(me, I(8)), src(inst, inst.src[1]), src(inst, inst.src[2]));
    const Operand pick = ubfe(I(4), shl(band(me, I(7)), I(2)), word);
    source = inst.op == 0x378 ? add(pick, I(16)) : pick;
    fetch_inactive = (inst.opsel & 1) != 0;
    bound_zero = (inst.opsel & 2) != 0;
    outside = consumer_identity(at_, inst.dst, &identity) ? Outside::Identity : Outside::Own;
  } else if (inst.src[0] == rdna::kDpp8 || inst.src[0] == rdna::kDpp8Fi) {
    source = ubfe(I(3), mul_lo(I(3), me), I(inst.ext >> 8));
    fetch_inactive = inst.src[0] == rdna::kDpp8Fi;
    bound_zero = true;
    if (source_identity(inst, &identity)) outside = Outside::Identity;
  } else {
    const uint32_t control = (inst.ext >> 8) & 0x1FF;
    fetch_inactive = (inst.ext >> 18) & 1;
    bound_zero = (inst.ext >> 19) & 1;
    const uint32_t n = control & 15;
    if (control < 0x100) {
      source = band(shr(I(control), shl(me, I(1))), I(3));
    } else if (control >= 0x101 && control <= 0x10F) {
      source = add(me, I(n));
      valid = ult(source, I(16));  // within the row
    } else if (control >= 0x111 && control <= 0x11F) {
      source = sub(me, I(n));
      valid = op2(dx::kUge, me, I(n));
    } else if (control >= 0x121 && control <= 0x12F) {
      source = band(sub(me, I(n)), I(15));
    } else if (control == 0x140) {
      source = sub(I(15), me);
    } else if (control == 0x141) {
      source = sub(I(7), me);
    } else if (control >= 0x150 && control <= 0x15F) {
      source = I(n);
    } else if (control >= 0x160 && control <= 0x16F) {
      source = bxor(me, I(n));
    } else {
      return fail(format("DPP control 0x%x in a %s program", control, quad ? "pixel" : "vertex"));
    }
    // Lanes 0-3 are in row 0 and bank 0.
    const uint32_t bank_mask = (inst.ext >> 24) & 15, row_mask = (inst.ext >> 28) & 15;
    if (!(bank_mask & 1) || !(row_mask & 1)) write = I(0);
    if (source_identity(inst, &identity)) outside = Outside::Identity;
  }
  source = in_temp(source);
  const uint32_t lanes = quad ? 4 : 1;
  const Operand inside = in_temp(ult(source, I(lanes)));
  const Operand own = in_temp(src(inst, exchanged_vgpr(inst)));
  // The value and EXEC bit of the source lane when it is in the quad.
  const uint32_t value = fresh(), source_active = fresh();
  if (quad) {
    const Operand relative = bxor(band(source, I(3)), me);
    const Operand x_sign = select(x, I(0xBF800000u), I(0x3F800000u));  // -1 in the right column
    const Operand y_sign = select(y, I(0xBF800000u), I(0x3F800000u));  // -1 in the bottom row
    const Operand halves[2] = {op1(dx::kUtof, bor(band(own, I(0xFFFF)), shl(active(), I(16)))),
                               op1(dx::kUtof, shr(own, I(16)))};
    Operand picked[2];
    for (int i = 0; i < 2; ++i) {
      const Operand f = in_temp(halves[i]);
      const Operand h = op3(dx::kMad, x_sign, op1(dx::kDerivRtxFine, f), f);
      const Operand c = op3(dx::kMad, y_sign, op1(dx::kDerivRtyFine, f), f);
      const Operand d = op3(dx::kMad, y_sign, op1(dx::kDerivRtyFine, h), h);
      picked[i] = in_temp(op1(dx::kFtou, select(band(relative, I(2)), select(band(relative, I(1)), d, c),
                                                 select(band(relative, I(1)), h, f))));
    }
    put(value, bor(band(picked[0], I(0xFFFF)), shl(picked[1], I(16))));
    put(source_active, band(shr(picked[0], I(16)), I(1)));
  } else {
    put(value, own);
    put(source_active, active());
  }
  // Lanes outside the quad.
  switch (outside) {
    case Outside::Identity:
      put_select(value, inside, R(value), I(identity));
      put_select(source_active, inside, R(source_active), I(1));
      break;
    case Outside::Own:
      put_select(value, inside, R(value), own);
      put_select(source_active, inside, R(source_active), I(1));
      break;
    case Outside::Invalid:
      put_select(source_active, inside, R(source_active), I(0));
      valid = band(valid, inside);
      break;
  }
  return finish_exchange(inst, value, valid, write, fetch_inactive, bound_zero, R(source_active));
}

namespace {

// The identity of VOP2 operation `op` for one of its operands (`first`:
// src0), if it has one: op(identity, x) = x.
bool vop2_identity(uint32_t op, bool first, uint32_t* identity) {
  switch (op) {
    case 0x03: *identity = 0x80000000u; return true;            // v_add_f32 (-0)
    case 0x05: *identity = 0; return first;                      // v_subrev_f32: src1 - src0
    case 0x07: case 0x08: *identity = 0x3F800000u; return true;  // v_mul_legacy_f32, v_mul_f32
    case 0x09: case 0x0B: *identity = 1; return true;            // v_mul_i32_i24, v_mul_u32_u24
    case 0x0F: *identity = 0x7F800000u; return true;             // v_min_f32
    case 0x10: *identity = 0xFF800000u; return true;             // v_max_f32
    case 0x11: *identity = 0x7FFFFFFFu; return true;             // v_min_i32
    case 0x12: *identity = 0x80000000u; return true;             // v_max_i32
    case 0x13: *identity = 0xFFFFFFFFu; return true;             // v_min_u32
    case 0x14: *identity = 0; return true;                       // v_max_u32
    case 0x16: case 0x18: case 0x1A: *identity = 0; return first;  // shifts by src0
    case 0x1B: case 0x1E: *identity = 0xFFFFFFFFu; return true;  // v_and_b32, v_xnor_b32
    case 0x1C: case 0x1D: *identity = 0; return true;            // v_or_b32, v_xor_b32
    case 0x25: *identity = 0; return true;                       // v_add_nc_u32
    case 0x27: *identity = 0; return first;                      // v_subrev_nc_u32: src1 - src0
    case 0x32: *identity = 0x8000u; return true;                 // v_add_f16 (-0)
    case 0x35: *identity = 0x3C00u; return true;                 // v_mul_f16
    case 0x39: *identity = 0xFC00u; return true;                 // v_max_f16
    case 0x3A: *identity = 0x7C00u; return true;                 // v_min_f16
    default: return false;
  }
}

}  // namespace

bool Translator::source_identity(const Instruction& inst, uint32_t* identity) const {
  if (inst.encoding != Encoding::VOP2) return false;
  // Modifiers on the exchanged operand change what an identity is.
  if (inst.src[0] == rdna::kDpp16 && ((inst.ext >> 20) & 3)) return false;
  return vop2_identity(inst.op, true, identity);
}

bool Translator::consumer_identity(size_t from, uint16_t vgpr, uint32_t* identity) const {
  for (size_t i = from + 1; i < program_.size() && i < from + 8; ++i) {
    const Instruction& next = program_[i];
    uint32_t op = 0xFFFFFFFFu;
    if (next.encoding == Encoding::VOP2 && next.src[0] != rdna::kDpp16 && next.src[0] != rdna::kDpp8 &&
        next.src[0] != rdna::kDpp8Fi && next.src[0] != rdna::kSdwa) {
      op = next.op;
    } else if (next.encoding == Encoding::VOP3 && next.op >= 0x100 && next.op < 0x140 && !next.abs && !next.neg &&
               !next.omod && !next.clamp) {
      op = next.op - 0x100u;
    }
    const uint16_t reg = static_cast<uint16_t>(rdna::kVgpr0 + vgpr);
    const bool reads0 = next.src[0] == reg, reads1 = next.src[1] == reg;
    if (op != 0xFFFFFFFFu && (reads0 || reads1)) return vop2_identity(op, reads0, identity);
    if (reads0 || reads1 || next.src[2] == reg) return false;  // another kind of use
    if (next.dst == vgpr && (next.encoding == Encoding::VOP1 || next.encoding == Encoding::VOP2 ||
                             next.encoding == Encoding::VOP3)) {
      return false;  // overwritten first
    }
  }
  return false;
}

bool Translator::emit_interp(const Instruction& inst) {
  if (stage_ != Stage::Pixel) return fail("interpolation outside a pixel shader");
  const uint32_t attribute = static_cast<uint32_t>(inst.imm), channel = inst.src[1] & 3;
  if (attribute >= 31) return fail("interpolation attribute");
  iface_.params |= 1u << attribute;
  if (inst.op == 2) {
    iface_.flat_params |= 1u << attribute;
  } else if (inst.op > 2) {
    return fail(format("VINTRP op %u", inst.op));
  }
  const uint32_t d = vreg(inst.dst);
  put_select(d, active(), dxbc::input_register(attribute + 1, channel), R(d));
  return true;
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
    case Encoding::DS:
      if (!computeish()) return fail("group shared memory in a graphics stage");
      return emit_ds(inst);
    case Encoding::EXP: return emit_export(inst);
    case Encoding::MIMG: return emit_image(inst);
    case Encoding::VOP3P: return emit_vop3p(inst);
    case Encoding::VINTRP: return emit_interp(inst);
    default: return fail(rdna::mnemonic(inst));
  }
}

bool Translator::run(std::vector<uint8_t>* out, std::string* error) {
  // Decode what control flow reaches, from the entry: programs keep data
  // after s_endpgm (tables they read through the program counter), which is
  // not code. Calls and jumps through registers (s_swappc_b64, s_setpc_b64)
  // reach the address the compiler's idiom forms: s_getpc_b64 into the
  // register pair, then s_add_u32 of a literal to its low half.
  {
    std::map<uint32_t, Instruction> decoded;
    std::vector<uint32_t> work = {0};
    while (!work.empty()) {
      uint32_t pc = work.back();
      work.pop_back();
      uint16_t pc_reg = 0xFFFF;  // SGPR pair holding a program address, and that address
      uint32_t pc_value = 0;
      while (pc + 4 <= size_ && !decoded.count(pc)) {
        Instruction inst;
        if (!rdna::decode(code_, size_, pc, &inst)) break;
        if (inst.encoding == Encoding::SOPP && inst.op == 31) break;  // s_code_end
        decoded[pc] = inst;
        const uint32_t next = pc + 4 * inst.words;
        bool stop = false;
        if (inst.encoding == Encoding::SOPP) {
          const uint32_t target = static_cast<uint32_t>(pc + 4 + 4 * inst.imm);
          if (inst.op == 1) stop = true;  // s_endpgm
          if (inst.op == 2) {             // s_branch
            work.push_back(target);
            stop = true;
          }
          if (inst.op >= 4 && inst.op <= 9) work.push_back(target);
        } else if (inst.encoding == Encoding::SOP1 && inst.op == 31) {
          pc_reg = inst.dst;
          pc_value = next;
        } else if (inst.encoding == Encoding::SOP2 && inst.op == 0 && inst.dst == pc_reg && inst.src[0] == pc_reg &&
                   inst.src[1] == 255) {
          pc_value += inst.literal;  // s_add_u32 of a literal
        } else if (inst.encoding == Encoding::SOP1 && (inst.op == 32 || inst.op == 33)) {
          if (inst.src[0] == pc_reg) {
            work.push_back(pc_value);
            indirect_targets_.insert(pc_value);
          }
          stop = inst.op == 32;  // s_setpc_b64 does not return; s_swappc_b64 returns after itself
        } else if (inst.dst == pc_reg || inst.dst == pc_reg + 1) {
          if (inst.encoding == Encoding::SOP1 || inst.encoding == Encoding::SOP2 || inst.encoding == Encoding::SOPK ||
              inst.encoding == Encoding::SMEM) {
            pc_reg = 0xFFFF;  // overwritten
          }
        }
        if (stop) break;
        pc = next;
      }
    }
    for (auto& [pc, inst] : decoded) program_.push_back(inst);
  }
  if (program_.empty()) {
    *error = "empty program";
    return false;
  }
  for (const Instruction& inst : program_) {
    if (inst.encoding == Encoding::SOP1 && inst.op == 31) code_data_ = true;  // s_getpc_b64
  }
  if (code_data_) {
    // Tables follow the code: the last 64 KiB of a larger program are kept.
    code_first_ = size_ > kCodeDataLimit ? static_cast<uint32_t>(size_ - kCodeDataLimit) & ~15u : 0;
    code_bytes_ = static_cast<uint32_t>(size_) - code_first_;
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

  if (stage_ == Stage::Pixel) {
    // Attributes read whole and as 16-bit halves, so half attributes get
    // their registers before the code is translated.
    for (const Instruction& inst : program_) {
      if (inst.encoding == Encoding::VINTRP && inst.imm < 31) scan_params_ |= 1u << inst.imm;
      if (inst.encoding == Encoding::VOP3 && (inst.op == 0x342 || inst.op == 0x343 || inst.op == 0x35A) &&
          (inst.src[0] & 0x3F) < 31) {
        iface_.half_params |= 1u << (inst.src[0] & 0x3F);
      }
    }
  }
  if (stage_ == Stage::Ngg && !ngg_scan()) {
    *error = error_;
    return false;
  }

  // Block leaders: entry, branch targets, instructions after branches.
  std::set<uint32_t> leaders = {0};
  leaders.insert(indirect_targets_.begin(), indirect_targets_.end());
  auto is_branch = [](const Instruction& i) {
    return (i.encoding == Encoding::SOPP && (i.op == 2 || (i.op >= 4 && i.op <= 9) || i.op == 1 || i.op == 10)) ||
           (i.encoding == Encoding::SOP1 && (i.op == 32 || i.op == 33));
  };
  for (size_t i = 0; i < program_.size(); ++i) {
    const Instruction& inst = program_[i];
    if (!is_branch(inst)) continue;
    if (inst.encoding == Encoding::SOPP && inst.op != 1 && inst.op != 10) {
      leaders.insert(static_cast<uint32_t>(inst.pc + 4 + 4 * inst.imm));
    }
    if (i + 1 < program_.size()) leaders.insert(program_[i + 1].pc);
  }

  // Blocks, each translated on its own, then laid out (below).
  std::vector<std::pair<uint32_t, dxbc::Program>> blocks;
  std::vector<char> barrier_after;  // a group barrier follows the block
  for (size_t i = 0; i < program_.size(); ++i) {
    const Instruction& inst = program_[i];
    at_ = i;
    scratch_ = 0;
    if (leaders.count(inst.pc)) {
      if (i != 0) {
        if (!is_branch(program_[i - 1])) put(kNextR, I(inst.pc));
        blocks.back().second = std::move(p_);
        p_ = dxbc::Program();
      }
      blocks.emplace_back(inst.pc, dxbc::Program());
      barrier_after.push_back(0);
    }
    if (inst.encoding == Encoding::SOPP) {
      const uint32_t target = static_cast<uint32_t>(inst.pc + 4 + 4 * inst.imm);
      const uint32_t next = i + 1 < program_.size() ? program_[i + 1].pc : 0xFFFFFFFFu;
      auto branch = [&](const Operand& taken) { put_select(kNextR, taken, I(target), I(next)); };
      switch (inst.op) {
        case 1: end_program(); continue;
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
          barrier_after.back() = 1;
          continue;
        default: continue;  // waits, priorities, messages, prefetch hints
      }
    }
    if (cross_lane(inst)) {
      if ((stage_ == Stage::Pixel || stage_ == Stage::Vertex) && cross_lane(inst)) {
        if (!quad_read(inst) || !error_.empty()) {
          *error = format("at 0x%x: %s", inst.pc, error_.c_str());
          return false;
        }
        continue;
      }
      if (!computeish()) {
        *error = format("at 0x%x: cross-lane operation outside a compute program", inst.pc);
        return false;
      }
      exchange_write(inst);
      barrier_ = true;
      put(kNextR, I(inst.pc + 2));
      put(kBarrierR, I(1));
      barrier_after.back() = 1;
      blocks.back().second = std::move(p_);
      p_ = dxbc::Program();
      blocks.emplace_back(inst.pc + 2, dxbc::Program());
      barrier_after.push_back(0);
      scratch_ = 0;
      if (!exchange_read(inst) || !error_.empty()) {
        *error = format("at 0x%x: %s", inst.pc, error_.c_str());
        return false;
      }
      continue;
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
  {
    // Past the last instruction the program ends, unless it jumps (a
    // subroutine's return at the end of the program, for instance).
    const Instruction& last = program_.back();
    const bool jumps = (last.encoding == Encoding::SOPP && (last.op == 1 || last.op == 2)) ||
                       (last.encoding == Encoding::SOP1 && (last.op == 32 || last.op == 33));
    if (!jumps) end_program();
  }
  blocks.back().second = std::move(p_);
  p_ = dxbc::Program();
  dxbc::Program body;
  const bool linear = linear_layout(blocks, barrier_after, body);
  std::function<void(size_t, size_t)> tree = [&](size_t first, size_t last) {
    if (last - first == 1) {
      body.append(blocks[first].second);
      return;
    }
    const size_t middle = (first + last) / 2;
    body.emit(dx::kUlt, {D(kScratch), R(kNextR), I(blocks[middle].first)});
    body.emit(dx::kIf, {R(kScratch)}, dxbc::kTestNonZero);
    tree(first, middle);
    body.emit(dx::kElse, {});
    tree(middle, last);
    body.emit(dx::kEndif, {});
  };
  if (!linear) {
    // The end marker (and any address past the program) selects no block.
    body.emit(dx::kUlt, {D(kScratch), R(kNextR), I(kEndBlock)});
    body.emit(dx::kIf, {R(kScratch)}, dxbc::kTestNonZero);
    tree(0, blocks.size());
    body.emit(dx::kEndif, {});
  }

  // Program: declarations, register initialisation, dispatcher loop.
  if (stage_ == Stage::Pixel && (iface_.params & iface_.flat_params) != iface_.flat_params) {
    *error = "an attribute is read both with and without interpolation";
    return false;
  }
  dxbc::Program program(computeish()              ? dxbc::ProgramType::Compute
                        : stage_ == Stage::Vertex ? dxbc::ProgramType::Vertex
                                                  : dxbc::ProgramType::Pixel);
  std::vector<dxbc::SignatureElement> inputs, outputs;
  // Refactoring allowed; double-precision operations.
  program.dcl_global_flags(doubles_ ? 1u | 2u : 1u);
  if (doubles_) features_ |= dxbc::kRequiresDoubles;
  program.dcl_constant_buffer(0, kTranslatedSpace, kSystemVectors, true);
  program.dcl_uav_raw_range(0, 0, kMemoryViews - 1, kTranslatedSpace);
  if (page_table_used_) program.dcl_resource_raw_range(kPageTableId, 0, 0, kTranslatedSpace);
  if (code_data_ || format_table_) {
    // The format table: format f's widths and numeric type in words 2f and
    // 2f + 1. Then program data.
    std::vector<uint32_t> words(kFormatTableVectors * 4, 0);
    for (uint32_t f = 0; f < 128; ++f) {
      BufferFormat layout;
      Numeric type;
      words[2 * f + 1] = kInvalidFormat;
      if (f == 0 || !buffer_format(f, &layout, &type)) continue;
      words[2 * f] = layout.bits[0] | (layout.bits[1] << 8) | (layout.bits[2] << 16) | (layout.bits[3] << 24);
      words[2 * f + 1] = static_cast<uint32_t>(type);
    }
    if (code_data_) {
      words.resize(words.size() + (code_bytes_ + 3) / 4, 0);
      std::memcpy(words.data() + kFormatTableVectors * 4, code_ + code_first_, code_bytes_);
    }
    program.dcl_immediate_constant_buffer(words);
  }
  static const dxbc::ResourceDimension kSlotDimension[kImageSlots] = {
      dxbc::kTexture2D, dxbc::kTexture2DArray, dxbc::kTexture3D, dxbc::kTextureCube,
      dxbc::kTexture1D, dxbc::kTexture1DArray, dxbc::kTexture2DMS};
  if (samplers_used_) program.dcl_sampler_range(0, kTextureSpace);
  for (uint32_t slot = 0; slot < kImageSlots; ++slot) {
    if (textures_used_ & (1u << slot)) program.dcl_texture_range(10 + slot, kSlotDimension[slot], kTextureSpace + slot);
    if (uavs_used_ & (1u << slot)) program.dcl_typed_uav_range(2 + slot, kSlotDimension[slot], kTextureSpace + slot);
    if (integer_uavs_used_ & (1u << slot)) {
      program.dcl_typed_uav_range(30 + slot, kSlotDimension[slot], kIntegerImageSpace + slot, true,
                                  ordered_uavs_);
    }
  }
  if (stage_ == Stage::Ngg) program.dcl_uav_raw_range(1, 0, 0, kNggSpace);
  if (computeish()) {
    program.dcl_input(dxbc::kThreadGroupId, 0x7);
    program.dcl_input(dxbc::kThreadIdInGroup, 0x7);
    program.dcl_input(dxbc::kThreadIdInGroupFlattened, 0);
  } else if (stage_ == Stage::Vertex) {
    program.dcl_input_sgv(0, 1, dxbc::kVertexId);
    program.dcl_input_sgv(1, 1, dxbc::kInstanceId);
    inputs.push_back({"SV_VertexID", 0, dxbc::kVertexId, dxbc::kUint32, 0, 1, 1});
    inputs.push_back({"SV_InstanceID", 0, dxbc::kInstanceId, dxbc::kUint32, 1, 1, 1});
    program.dcl_output_siv(0, 0xF, dxbc::kPosition);
    outputs.push_back({"SV_Position", 0, dxbc::kPosition, dxbc::kFloat32, 0, 0xF, 0});
    for (uint32_t n = 0; n < 32; ++n) {
      if (!(iface_.params & (1u << n)) || !outputs_register(n + 1)) continue;
      program.dcl_output(n + 1, 0xF);
      outputs.push_back({"TEXCOORD", n, dxbc::kUndefined, dxbc::kFloat32, n + 1, 0xF, 0});
    }
    for (uint32_t n = 0; consumer_ && n < 32; ++n) {
      if (!((consumer_->half_params & iface_.params) & (1u << n))) continue;
      const uint32_t reg = half_param_register(consumer_->params, consumer_->half_params, n);
      if (reg + 1 >= 32) {
        *error = "too many attributes for the host's outputs";
        return false;
      }
      for (uint32_t j = 0; j < 2; ++j) {
        program.dcl_output(reg + j, 0xF);
        outputs.push_back({"HALFPARAM", 2 * n + j, dxbc::kUndefined, dxbc::kFloat32, reg + j, 0xF, 0});
      }
    }
  } else {
    if (quad_) {
      // The pixel position, in the register the vertex stage writes it to.
      program.dcl_input_ps_siv(0, 0x3, dxbc::kLinearNoPerspective, dxbc::kPosition);
      inputs.push_back({"SV_Position", 0, dxbc::kPosition, dxbc::kFloat32, 0, 0xF, 0x3});
    }
    for (uint32_t n = 0; n < 32; ++n) {
      if (!(iface_.params & (1u << n))) continue;
      const bool flat = (iface_.flat_params & (1u << n)) != 0;
      program.dcl_input_ps(n + 1, 0xF, flat ? dxbc::kConstant : dxbc::kLinear);
      inputs.push_back({"TEXCOORD", n, dxbc::kUndefined, dxbc::kFloat32, n + 1, 0xF, 0xF});
    }
    if ((iface_.params & ~scan_params_) != 0 && iface_.half_params) {
      *error = "attributes read whole beyond those the scan found";
      return false;
    }
    for (uint32_t n = 0; n < 32; ++n) {
      if (!(iface_.half_params & (1u << n))) continue;
      const uint32_t reg = half_param_register(scan_params_, iface_.half_params, n);
      if (reg + 1 >= 32) {
        *error = "too many attributes for the host's inputs";
        return false;
      }
      for (uint32_t j = 0; j < 2; ++j) {
        program.dcl_input_ps(reg + j, 0xF, dxbc::kLinear);
        inputs.push_back({"HALFPARAM", 2 * n + j, dxbc::kUndefined, dxbc::kFloat32, reg + j, 0xF, 0xF});
      }
    }
    for (uint32_t t = 0; t < 8; ++t) {
      if (!(iface_.targets & (1u << t))) continue;
      program.dcl_output(t, 0xF);
      outputs.push_back({"SV_Target", t, dxbc::kUndefined, dxbc::kFloat32, t, 0xF, 0});
    }
    if (iface_.depth) {
      program.dcl_output_depth();
      outputs.push_back({"SV_Depth", 0, dxbc::kUndefined, dxbc::kFloat32, 0xFFFFFFFFu, 1, 0xE});
    }
  }
  bool any_output = false;
  for (uint8_t mask : written_) any_output = any_output || mask != 0;
  program.dcl_temps(any_output ? kOutputBase + 4 * kOutputRegisters : kVgprBase + vgprs_);
  if (computeish()) {
    // Group shared memory: the program's (up to the host's 32 KiB, less the
    // exchange area) and the exchange area (two buffers of 8 bytes a thread).
    const uint32_t exchange = exchange_ ? 2 * 8 * info_.threads[0] * info_.threads[1] * info_.threads[2] : 0;
    const uint32_t lds_limit = 32768 - exchange;
    if (lds_) program.dcl_tgsm_raw(0, info_.lds_bytes > lds_limit ? lds_limit : info_.lds_bytes);
    if (exchange_) program.dcl_tgsm_raw(1, exchange);
    program.dcl_thread_group(info_.threads[0], info_.threads[1], info_.threads[2]);
  }
  auto init = [&](uint32_t reg, const Operand& value) { program.emit(dx::kMov, {D(reg), value}); };
  // Every temp a path may read before writing starts at zero: host drivers
  // have refused programs that read undefined temps (outputs exported on
  // some paths only, scratch values).
  for (uint32_t i = 0; i < 106; ++i) init(i, I(0));
  for (uint32_t i = kScratch; i < kVgprBase; ++i) init(i, I(0));
  if (any_output) {
    for (uint32_t i = kOutputBase; i < kOutputBase + 4 * kOutputRegisters; ++i) init(i, I(0));
  }
  init(kLaneR, I(0));
  init(kBarrierR, I(0));
  init(kExportedR, I(0));
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
  if (computeish()) {
    uint32_t next_sgpr = info_.user_sgprs;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (info_.tgid_enabled[axis]) init(next_sgpr++, dxbc::input(dxbc::kThreadGroupId, axis));
    }
    if (info_.tg_size_enabled) {
      // Thread group information: waves in the group (bits 0-5) and this
      // lane's wave within it (bits 6-11).
      const uint32_t threads = info_.threads[0] * info_.threads[1] * info_.threads[2];
      const uint32_t waves = (threads + info_.wave_size - 1) / info_.wave_size;
      const uint32_t reg = next_sgpr++;
      program.emit(dx::kUshr, {D(reg), dxbc::input(dxbc::kThreadIdInGroupFlattened, 0),
                               I(info_.wave_size == 64 ? 6 : 5)});
      program.emit(dx::kIshl, {D(reg), R(reg), I(6)});
      program.emit(dx::kOr, {D(reg), R(reg), I(waves & 0x3F)});
    }
    if (stage_ == Stage::Ngg) {
      ngg_prologue(program);
    } else {
      for (uint32_t axis = 0; axis < 3; ++axis) init(kVgprBase + axis, dxbc::input(dxbc::kThreadIdInGroup, axis));
    }
    program.emit(dx::kAnd, {D(kLaneR), dxbc::input(dxbc::kThreadIdInGroupFlattened, 0), I(info_.wave_size - 1)});
  } else {
    init(kLaneR, I(0));
    init(kExportedR, I(0));
    if (stage_ == Stage::Vertex) {
      init(kVgprBase + 0, dxbc::input_register(0, 0));
      if (vgprs_ > 3) init(kVgprBase + 3, dxbc::input_register(1, 0));
    }
  }
  if (linear) {
    program.append(body);
    epilogue(program);
    *out = program.container(inputs, outputs, features_);
    return true;
  }
  if (barrier_) {
    // The dispatcher stops at barriers; every thread reaches the same number
    // of them in programs that are correct on the console.
    program.emit(dx::kLoop, {});
    program.emit(dx::kMov, {D(kBarrierR), I(0)});
  }
  // kLaneR is free after initialisation in graphics stages; kScratch holds
  // the loop exit test.
  program.emit(dx::kLoop, {});
  program.append(body);
  program.emit(dx::kIeq, {D(kScratch), R(kNextR), I(kEndBlock)});
  program.emit(dx::kBreakc, {R(kScratch)}, dxbc::kTestNonZero);
  if (barrier_) program.emit(dx::kBreakc, {R(kBarrierR)}, dxbc::kTestNonZero);
  program.emit(dx::kEndloop, {});
  if (barrier_) {
    program.emit(dx::kIeq, {D(kScratch), R(kNextR), I(kEndBlock)});
    program.emit(dx::kBreakc, {R(kScratch)}, dxbc::kTestNonZero);
    program.emit(dx::kSync, {}, dxbc::kSyncThreads | dxbc::kSyncGroupMemory | dxbc::kSyncUavGlobal);
    program.emit(dx::kEndloop, {});
  }
  epilogue(program);
  *out = program.container(inputs, outputs, features_);
  return true;
}

}  // namespace

bool translate_compute(const uint8_t* code, size_t size, const ComputeShaderInfo& info, std::vector<uint8_t>* dxbc,
                       std::string* error) {
  Translator translator(code, size, info);
  return translator.run(dxbc, error);
}

bool native_graphics_stage(const uint8_t* code, size_t size, GraphicsStage* stage) {
  bool vertex = false, pixel = false;
  rdna::Instruction inst;
  for (uint32_t pc = 0; rdna::decode(code, size, pc, &inst); pc += 4 * inst.words) {
    if (inst.encoding == Encoding::EXP) {
      // Targets: 0-7 colour, 8 depth, 9 null, 12-15 position, 32+ parameters.
      if (inst.target >= 12 && inst.target <= 15) vertex = true;
      if (inst.target <= 9) pixel = true;
    } else if (inst.encoding == Encoding::VINTRP) {
      pixel = true;
    } else if (inst.encoding == Encoding::SOPP && inst.op == 31) {
      break;  // s_code_end
    }
  }
  if (vertex == pixel) return false;
  *stage = vertex ? GraphicsStage::Vertex : GraphicsStage::Pixel;
  return true;
}

bool native_ngg_program(const uint8_t* code, size_t size) {
  rdna::Instruction inst;
  for (uint32_t pc = 0; rdna::decode(code, size, pc, &inst); pc += 4 * inst.words) {
    if (inst.encoding == Encoding::EXP && inst.target == 20) return true;  // a primitive export
    if (inst.encoding == Encoding::SOPP && inst.op == 31) break;           // s_code_end
  }
  return false;
}

bool translate_ngg(const uint8_t* code, size_t size, std::vector<uint8_t>* dxbc, ShaderInterface* link, NggInfo* info,
                   std::string* error) {
  Translator translator(code, size, Translator::NggTag{});
  if (!translator.run(dxbc, error)) return false;
  if (link) *link = translator.link();
  if (info) *info = translator.ngg();
  return true;
}

uint32_t half_param_register(uint32_t params, uint32_t half_params, uint32_t n) {
  uint32_t top = 0;
  for (uint32_t i = 0; i < 32; ++i) {
    if (params & (1u << i)) top = i + 1;
  }
  uint32_t rank = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (half_params & (1u << i)) ++rank;
  }
  return top + 1 + 2 * rank;
}

std::vector<uint8_t> ngg_vertex_program(uint32_t stride, uint32_t params, uint32_t half_params) {
  dxbc::Program program(dxbc::ProgramType::Vertex);
  std::vector<dxbc::SignatureElement> inputs, outputs;
  program.dcl_global_flags(1);
  program.dcl_resource_raw_range(0, 0, 0, kNggSpace);
  program.dcl_input_sgv(0, 1, dxbc::kVertexId);
  inputs.push_back({"SV_VertexID", 0, dxbc::kVertexId, dxbc::kUint32, 0, 1, 1});
  program.dcl_output_siv(0, 0xF, dxbc::kPosition);
  outputs.push_back({"SV_Position", 0, dxbc::kPosition, dxbc::kFloat32, 0, 0xF, 0});
  for (uint32_t n = 0; n < 32; ++n) {
    if (!(params & (1u << n))) continue;
    program.dcl_output(n + 1, 0xF);
    outputs.push_back({"TEXCOORD", n, dxbc::kUndefined, dxbc::kFloat32, n + 1, 0xF, 0});
  }
  for (uint32_t n = 0; n < 32; ++n) {
    if (!(half_params & (1u << n))) continue;
    const uint32_t reg = half_param_register(params, half_params, n);
    for (uint32_t j = 0; j < 2; ++j) {
      program.dcl_output(reg + j, 0xF);
      outputs.push_back({"HALFPARAM", 2 * n + j, dxbc::kUndefined, dxbc::kFloat32, reg + j, 0xF, 0});
    }
  }
  program.dcl_temps(3);
  // r0.x: the record's address; r1: a loaded vector; r2.x: an address.
  program.emit(dx::kUmad, {dxbc::temp_dst(0), dxbc::input_register(0, 0), I(stride), I(0)});
  auto copy_vector = [&](uint32_t offset, uint32_t out) {
    program.emit(dx::kIadd, {dxbc::temp_dst(2), dxbc::temp(0), I(offset)});
    program.emit(dx::kLdRaw, {dxbc::temp_dst_mask(1, 0xF), dxbc::temp(2), dxbc::srv_xyzw(0, 0)});
    program.emit(dx::kMov, {dxbc::output_register(out, 0xF), dxbc::temp_xyzw(1)});
  };
  copy_vector(0, 0);
  for (uint32_t n = 0; n < 32; ++n) {
    if (params & (1u << n)) copy_vector(16 * (1 + n), n + 1);
  }
  // Half attributes: each channel's two halves as floats.
  for (uint32_t n = 0; n < 32; ++n) {
    if (!(half_params & (1u << n))) continue;
    const uint32_t reg = half_param_register(params, half_params, n);
    program.emit(dx::kIadd, {dxbc::temp_dst(2), dxbc::temp(0), I(16 * (1 + n))});
    program.emit(dx::kLdRaw, {dxbc::temp_dst_mask(1, 0xF), dxbc::temp(2), dxbc::srv_xyzw(0, 0)});
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t out = reg + (c >> 1), component = (c & 1) * 2;
      program.emit(dx::kF16tof32, {dxbc::output_register(out, 1u << component), dxbc::temp(1, c)});
      program.emit(dx::kUshr, {dxbc::temp_dst(2), dxbc::temp(1, c), I(16)});
      program.emit(dx::kF16tof32, {dxbc::output_register(out, 1u << (component + 1)), dxbc::temp(2)});
    }
  }
  program.emit(dx::kRet, {});
  return program.container(inputs, outputs);
}

bool translate_graphics(const uint8_t* code, size_t size, GraphicsStage stage, std::vector<uint8_t>* dxbc,
                        ShaderInterface* link, std::string* error, const ShaderInterface* consumer) {
  Translator translator(code, size, stage);
  if (stage == GraphicsStage::Vertex) translator.set_consumer(consumer);
  if (!translator.run(dxbc, error)) return false;
  if (link) *link = translator.link();
  return true;
}

}  // namespace holy::d3d12

namespace holy::d3d12 {
const char* dxbc_build();
const char* rdna_build();

namespace {
TranslatorOptions g_options;
}  // namespace

void set_translator_options(const TranslatorOptions& options) {
  g_options = options;
}

const TranslatorOptions& translator_options() {
  return g_options;
}

std::string translator_build() {
  return std::string(__DATE__ " " __TIME__) + "/" + dxbc_build() + "/" + rdna_build() +
         (g_options.page_cache_reads ? "/page cache reads" : "");
}
}  // namespace holy::d3d12
