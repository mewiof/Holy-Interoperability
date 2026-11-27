// Writer for Direct3D shader bytecode (DXBC, shader model 5.1).
//
// The token format is the one documented for the desktop runtime's
// tokenized program format: an opcode token (opcode in bits 0-10,
// instruction-specific controls in bits 11-23, length in dwords in bits
// 24-30), followed by operand tokens (component count and selection in
// bits 0-11, operand type in bits 12-19, index dimension in bits 20-21,
// index representations in bits 22-30, extended-operand flag in bit 31)
// and their indices. The translator uses only scalar (single component)
// operands; every register value lives in the x component of a temp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace holy::d3d12::dxbc {

enum Opcode : uint32_t {
  kAdd = 0, kAnd = 1, kBreak = 2, kBreakc = 3, kCase = 6, kDefault = 10, kDiv = 14, kElse = 18, kEndif = 21,
  kLd = 45, kLdMs = 46, kSampleD = 73, kLod = 108, kResinfo = 61, kGather4 = 109, kGather4C = 126, kGather4Po = 127, kGather4PoC = 128, kSample = 69, kSampleC = 70, kSampleCLz = 71, kSampleL = 72, kSampleB = 74,
  kLdUavTyped = 163, kStoreUavTyped = 164, kDclResource = 88, kDclSampler = 90, kDclUavTyped = 156,
  kEndloop = 22, kEndswitch = 23, kEq = 24, kExp = 25, kFrc = 26, kFtoi = 27, kFtou = 28, kGe = 29, kIadd = 30,
  kIf = 31, kIeq = 32, kIge = 33, kIlt = 34, kImad = 35, kImax = 36, kImin = 37, kImul = 38, kIne = 39,
  kIneg = 40, kIshl = 41, kIshr = 42, kItof = 43, kLog = 47, kLoop = 48, kLt = 49, kMad = 50, kMin = 51,
  kMax = 52, kMov = 54, kMovc = 55, kMul = 56, kNe = 57, kNot = 59, kOr = 60, kRet = 62, kRoundNe = 64,
  kRoundNi = 65, kRoundPi = 66, kRoundZ = 67, kRsq = 68, kSqrt = 75, kSwitch = 76, kSincos = 77, kUdiv = 78,
  kUlt = 79, kUge = 80, kUmul = 81, kUmad = 82, kUmax = 83, kUmin = 84, kUshr = 85, kUtof = 86, kXor = 87,
  kDiscard = 13, kDclConstantBuffer = 89, kDclInput = 95, kDclInputSgv = 96, kDclInputPs = 98,
  kDclInputPsSiv = 100, kDclOutput = 101, kDclOutputSiv = 103, kDclTemps = 104, kDclGlobalFlags = 106,
  kDclThreadGroup = 155, kDclResourceRaw = 161,
  kDclUavRaw = 157, kDclTgsmRaw = 159, kRcp = 129, kF32tof16 = 130, kF16tof32 = 131, kUaddc = 132,
  kUsubb = 133, kCountbits = 134, kFirstbitHi = 135, kFirstbitLo = 136, kFirstbitShi = 137, kUbfe = 138,
  kIbfe = 139, kBfi = 140, kBfrev = 141, kLdRaw = 165, kStoreRaw = 166, kAtomicAnd = 169, kAtomicOr = 170,
  kImmAtomicIadd = 180, kImmAtomicAnd = 181, kImmAtomicOr = 182, kImmAtomicXor = 183, kImmAtomicExch = 184,
  kImmAtomicCmpExch = 185, kImmAtomicImax = 186, kImmAtomicImin = 187, kImmAtomicUmax = 188,
  kImmAtomicUmin = 189, kSync = 190, kDerivRtxFine = 123, kDerivRtyFine = 125,
  // Doubles (measured from the desktop compiler's output): a double is a
  // register's x and y (or z and w) components, low dword first.
  kDadd = 191, kDmul = 194, kDtof = 201, kFtod = 202,
  // Geometry programs.
  kDclGsOutputTopology = 92, kDclGsInputPrimitive = 93, kDclMaxOutputVertexCount = 94, kDclInputSiv = 97,
  kEmitStream = 117, kCutStream = 118, kDclStream = 143,
};

// Instruction controls (bits 11-23 of the opcode token).
inline constexpr uint32_t kSaturate = 1u << 13;
inline constexpr uint32_t kTestNonZero = 1u << 18;  // if/breakc: test for non-zero
inline constexpr uint32_t kSyncThreads = 1u << 11;
inline constexpr uint32_t kSyncGroupMemory = 1u << 12;
inline constexpr uint32_t kSyncUavGroup = 1u << 13;
inline constexpr uint32_t kSyncUavGlobal = 1u << 14;

// Operand types.
enum OperandType : uint32_t {
  kTemp = 0, kInput = 1, kOutput = 2, kImmediate32 = 4, kSampler = 6, kResource = 7, kConstantBuffer = 8,
  kImmediateConstantBuffer = 9, kOutputDepth = 12, kNull = 13,
  kUav = 30, kGroupShared = 31,
  kThreadId = 32, kThreadGroupId = 33, kThreadIdInGroup = 34, kThreadIdInGroupFlattened = 36,
};

// An operand's tokens, kept inline: translation builds millions of them, and
// heap-allocated ones made it slow and kept threads waiting on the heap
// (measured). No operand needs more than kCapacity (an extended token, a
// modifier, the operand token, indices and a relative register's tokens).
class Operand {
 public:
  static constexpr size_t kCapacity = 8;
  Operand() = default;
  Operand(std::initializer_list<uint32_t> tokens) {
    for (uint32_t t : tokens) push_back(t);
  }
  void push_back(uint32_t token) {
    if (size_ == kCapacity) std::abort();  // a writer bug: no operand is this long
    data_[size_++] = token;
  }
  // Inserts before `at` (shifting the rest up).
  void insert(uint32_t* at, uint32_t token) {
    const size_t index = static_cast<size_t>(at - data_);
    push_back(0);
    for (size_t i = size_ - 1; i > index; --i) data_[i] = data_[i - 1];
    data_[index] = token;
  }
  // Appends [first, last) (`at` must be end()).
  void insert(uint32_t*, const uint32_t* first, const uint32_t* last) {
    for (; first != last; ++first) push_back(*first);
  }
  uint32_t& operator[](size_t i) { return data_[i]; }
  uint32_t operator[](size_t i) const { return data_[i]; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  uint32_t* begin() { return data_; }
  uint32_t* end() { return data_ + size_; }
  const uint32_t* begin() const { return data_; }
  const uint32_t* end() const { return data_ + size_; }
  uint32_t& back() { return data_[size_ - 1]; }

 private:
  uint32_t data_[kCapacity] = {};
  uint8_t size_ = 0;
};

// Sources select one component; destinations write the x component.
Operand temp(uint32_t index, uint32_t component = 0);
Operand temp_dst(uint32_t index);
Operand null_dst();
Operand imm(uint32_t value);
Operand immf(float value);
Operand neg(Operand operand);
Operand abs(Operand operand);
Operand input(OperandType type, uint32_t component);
// vN.c and oN with a write mask; the depth output.
Operand input_register(uint32_t index, uint32_t component);
Operand output_register(uint32_t index, uint32_t mask);
Operand output_depth();

// System-value names in declarations and signatures.
enum SystemValue : uint32_t { kUndefined = 0, kPosition = 1, kVertexId = 6, kInstanceId = 8 };
// Interpolation modes of pixel shader inputs.
enum Interpolation : uint32_t { kConstant = 1, kLinear = 2, kLinearNoPerspective = 4 };
// Signature component types.
enum ComponentType : uint32_t { kUint32 = 1, kFloat32 = 3 };

struct SignatureElement {
  const char* name;
  uint32_t index;
  uint32_t system_value;
  uint32_t component_type;
  uint32_t reg;  // 0xFFFFFFFF for registers without an index (depth)
  uint8_t mask;
  uint8_t rw_mask;  // inputs: components read; outputs: components never written
};

enum class ProgramType : uint32_t { Pixel = 0, Vertex = 1, Geometry = 2, Compute = 5 };

// vN[vertex][reg].xyzw (geometry program inputs), oN.xyzw, and stream m0.
Operand gs_input_xyzw(uint32_t vertex, uint32_t reg);
Operand output_xyzw(uint32_t reg);
Operand stream0();
// CBn[0][register].component and CBn[0][temp.x + register].component.
Operand cb(uint32_t id, uint32_t reg, uint32_t component);
Operand cb_indexed(uint32_t id, uint32_t temp_index, uint32_t reg, uint32_t component);

// icb[temp.x].component: the immediate constant buffer indexed by a temp.
Operand icb_indexed(uint32_t temp_index, uint32_t component);

// Load selects .xxxx (one dword); LoadVector .xyzw (up to four, by the
// destination's mask).
enum class Access { Load, LoadVector, Store, Atomic };

// Shader feature bits of the SFI0 part (the desktop runtime's documented
// D3D_SHADER_REQUIRES_* values).
inline constexpr uint64_t kRequiresDoubles = 0x1;
inline constexpr uint64_t kRequiresTypedUavLoadAdditionalFormats = 0x800;
inline constexpr uint64_t kRequiresRovs = 0x1000;
// Un[0][temp.x] (non-uniform) and gN.
Operand uav_indexed(uint32_t id, uint32_t temp_index, Access access);
Operand group_shared(uint32_t id, Access access);
// Un[index] (static index) and Tn[index].xyzw.
Operand uav(uint32_t id, uint32_t index, Access access);
Operand srv_xyzw(uint32_t id, uint32_t index);

// rN with a write mask, and rN.xyzw as a four-component source.
Operand temp_dst_mask(uint32_t index, uint32_t mask);
Operand temp_xyzw(uint32_t index);
// rN with a swizzle (2 bits a component, x in bits 0-1): rN.xyxy (0x44) is a
// double in x and y.
Operand temp_swizzle(uint32_t index, uint32_t swizzle);
// Tn[temp.x], Un[temp.x] (typed, .xyzw) and Sn[temp.x], all non-uniform.
Operand texture_indexed(uint32_t id, uint32_t temp_index);
Operand typed_uav_indexed(uint32_t id, uint32_t temp_index, bool store);
// A typed UAV array element as the destination of an atomic operation.
Operand typed_uav_atomic(uint32_t id, uint32_t temp_index);
Operand sampler_indexed(uint32_t id, uint32_t temp_index);
// Sn[temp.x].c: the sampler of a gather, selecting the gathered component.
Operand sampler_indexed_select(uint32_t id, uint32_t temp_index, uint32_t component);

// Resource dimensions of declarations.
enum ResourceDimension : uint32_t {
  kTexture1D = 2, kTexture2D = 3, kTexture2DMS = 4, kTexture3D = 5, kTextureCube = 6, kTexture1DArray = 7,
  kTexture2DArray = 8,
};

class Program {
 public:
  // Version 5.1 of the given program type.
  explicit Program(ProgramType type = ProgramType::Compute);

  void emit(uint32_t opcode, std::initializer_list<Operand> operands, uint32_t controls = 0) {
    emit(opcode, operands.begin(), operands.size(), controls);
  }
  void emit(uint32_t opcode, const std::vector<Operand>& operands, uint32_t controls = 0) {
    emit(opcode, operands.data(), operands.size(), controls);
  }
  void emit(uint32_t opcode, const Operand* operands, size_t count, uint32_t controls);
  void emit(uint32_t opcode) { emit(opcode, nullptr, 0, 0); }

  void dcl_global_flags(uint32_t flags);
  // An immediate constant buffer of `data` (padded to whole vectors; at most
  // 4096 vectors).
  void dcl_immediate_constant_buffer(const std::vector<uint32_t>& data);
  void dcl_constant_buffer(uint32_t id, uint32_t space, uint32_t vectors, bool dynamic);
  void dcl_uav_raw_range(uint32_t id, uint32_t first, uint32_t last, uint32_t space);
  void dcl_resource_raw_range(uint32_t id, uint32_t first, uint32_t last, uint32_t space);
  // Float textures and typed UAVs of a dimension, and samplers, at [0, unbounded).
  void dcl_texture_range(uint32_t id, ResourceDimension dimension, uint32_t space);
  // `ordered`: a rasterizer-ordered view (pixel shaders: one pixel's accesses
  // happen in primitive order, one at a time).
  void dcl_typed_uav_range(uint32_t id, ResourceDimension dimension, uint32_t space, bool integer = false,
                           bool ordered = false);
  void dcl_sampler_range(uint32_t id, uint32_t space);
  void dcl_input(OperandType type, uint32_t mask);
  void dcl_temps(uint32_t count);
  void dcl_tgsm_raw(uint32_t id, uint32_t bytes);
  void dcl_thread_group(uint32_t x, uint32_t y, uint32_t z);
  void dcl_input_sgv(uint32_t reg, uint32_t mask, SystemValue name);
  void dcl_input_ps(uint32_t reg, uint32_t mask, Interpolation mode);
  void dcl_input_ps_siv(uint32_t reg, uint32_t mask, Interpolation mode, SystemValue name);
  void dcl_output(uint32_t reg, uint32_t mask);
  void dcl_output_siv(uint32_t reg, uint32_t mask, SystemValue name);
  void dcl_output_depth();
  // Geometry programs: inputs v[vertices][reg] (`position`: the SV_Position
  // input), triangles in, triangle strips of at most `max_vertices` out, on
  // stream 0.
  void dcl_gs_input(uint32_t vertices, uint32_t reg, uint32_t mask, bool position);
  void dcl_gs_triangles_to_strips(uint32_t max_vertices);

  // Appends `other`'s instructions (no version/length header).
  void append(const Program& other);
  size_t size() const { return tokens_.size(); }

  // The complete DXBC container (input/output signatures, SHEX, checksum;
  // with `features`, the SFI0 part declaring them: kRequires* bits).
  // Geometry programs' outputs go in a stream-aware signature (OSG5).
  std::vector<uint8_t> container(const std::vector<SignatureElement>& inputs = {},
                                 const std::vector<SignatureElement>& outputs = {}, uint64_t features = 0) const;

 private:
  std::vector<uint32_t> tokens_;
};

}  // namespace holy::d3d12::dxbc
