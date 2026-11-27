// Translation of console GPU machine code into Direct3D shader bytecode.
//
// Execution model: one host thread runs one lane. Vector registers are the
// lane's; scalar registers are recomputed by every lane (they hold uniform
// values in correct programs); lane masks (EXEC, VCC, compare results)
// hold the lane's own bit in bit 0. Instructions whose meaning depends on
// other lanes (cross-lane moves, DPP, reductions over masks) are not
// supported and make translation fail, as do instructions not implemented
// yet. Control flow becomes a block dispatcher: a loop around a switch on
// the address of the next block.
//
// Memory: the title's GPU addresses are resolved through a region table
// (see the HolySystem constant buffer written by the command list
// translation) to raw views of the host heaps over the title's memory.
//
// The output is a DXBC container (shader model 5.1) written directly, so no
// shader compiler is involved; the host runtime converts it for the driver.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace holy::d3d12 {

struct ComputeShaderInfo {
  uint32_t threads[3] = {64, 1, 1};
  // SGPRs initialised from the HolySystem user values; the workgroup ids
  // follow. Assumed for compute programs: 4 (s[0:1] the root table address).
  uint32_t user_sgprs = 4;
  // 32 or 64; 0: decided from the code (assumed: 64 when it writes EXEC with
  // 64-bit scalar operations, 32 otherwise).
  uint32_t wave_size = 0;
  bool tgid_enabled[3] = {true, true, true};
  // The thread group information SGPR after the group ids: the number of
  // waves in the group (bits 0-5) and the wave's index in it (bits 6-11).
  bool tg_size_enabled = false;
  uint32_t lds_bytes = 32768;  // declared group shared memory (host limit 32 KiB)
};

// Register space of the translation's own bindings: u0-u65535 (raw views of
// title memory, one per memory slot), t0 (the page table) and b0
// (HolySystem: user SGPRs, NGG values).
inline constexpr uint32_t kTranslatedSpace = 8;

// Title memory as translated programs reach it. Every piece of title memory
// the GPU can use is a host heap with a buffer spanning it; each 512 MiB of
// such a buffer is a memory slot, a raw view at index `slot` of u0 (slot 0 is
// a null view: reads give zero, writes are dropped). The page table maps
// each 64 KiB page of title addresses (below 2^47) to a slot and an offset:
// - level 1, at byte 0: kPageTableWindows 32-bit entries, one per 1 GiB of
//   addresses (address bits 30-46), each the number of its level 2 table
//   counting from 1, or 0 when no page in the window is mapped;
// - the cache table after it (below);
// - a table of zeros (kPageTableZeroTable, "table 0": translated programs
//   look pages up without branches, so windows with no table and addresses
//   from 2^47 up read their entries there: slot 0);
// - level 2 tables from kPageTableLevel2Base, kPageTableTableBytes each: one
//   entry per 64 KiB page of the window (address bits 16-29), two 32-bit
//   words: the slot (0: not mapped) and the view offset of the page's start
//   (wrapping: the page may start before its heap, whose start need not be a
//   page boundary).
// A title address then reads from view `slot` at offset + (address & 0xFFFF).
//
// Cached pages: an entry whose slot has kCachedPageBit set names line `slot &
// ~kCachedPageBit` of the cache table instead, four 32-bit words: the slot
// and view offset of a copy of the page (video memory), then those of the
// page itself. Translated reads take the copy unless the line's GPU write
// flag (a 32-bit word per line in view kFlagsView) is set; writes and atomics
// take the page itself and set the flag, so the reads that follow them see
// them. Flags stay set: the runtime keeps copies current with the CPU's
// writes only. Programs test for cached pages only when translated with
// TranslatorOptions::page_cache_reads.
inline constexpr uint32_t kMemoryViews = 65536;
inline constexpr uint32_t kFlagsView = kMemoryViews - 1;  // memory slots stop below it
inline constexpr uint32_t kPageTableWindows = 1u << 17;
inline constexpr uint32_t kPageTableLevel1Bytes = kPageTableWindows * 4;
inline constexpr uint32_t kCachedPageBit = 0x80000000u;
inline constexpr uint32_t kPageCacheLines = 65536;
inline constexpr uint32_t kPageTableTableBytes = 16384 * 8;
inline constexpr uint32_t kPageTableZeroTable = kPageTableLevel1Bytes + kPageCacheLines * 16;
inline constexpr uint32_t kPageTableLevel2Base = kPageTableZeroTable + kPageTableTableBytes;
inline constexpr uint32_t kPageTableId = 1;  // T1 in the translation's declarations

// HolySystem layout in 16-byte vectors.
inline constexpr uint32_t kSystemUserVector = 0;  // [4]  initial user SGPRs
inline constexpr uint32_t kSystemNggVector = 4;   // NGG draws: index area offset, groups in x, first vertex,
                                                  // first instance (translate_ngg)
inline constexpr uint32_t kSystemVectors = 5;

// Image instructions address the bound shader-visible heaps through the
// index in descriptor dword 0 (shader/descriptor_layout.h): float texture
// arrays [0, unbounded) of each dimension in spaces kTextureSpace + slot,
// typed UAV arrays likewise, and the sampler array in kTextureSpace.
inline constexpr uint32_t kTextureSpace = 9;
enum ImageSlot : uint32_t {
  kImage2D = 0, kImage2DArray = 1, kImage3D = 2, kImageCube = 3, kImage1D = 4, kImage1DArray = 5, kImage2DMS = 6,
};
inline constexpr uint32_t kImageSlots = 7;
// Image atomics address the same heap through integer UAV arrays, in spaces
// kIntegerImageSpace + slot (typed atomics need integer declarations).
inline constexpr uint32_t kIntegerImageSpace = 21;

bool translate_compute(const uint8_t* code, size_t size, const ComputeShaderInfo& info, std::vector<uint8_t>* dxbc,
                       std::string* error);

// Options the runtime sets once, before translating anything.
struct TranslatorOptions {
  bool page_cache_reads = false;  // memory accesses test for cached pages (above)
  // Tests: translate without the optimisations that change a program's
  // structure (blocks in order, what is known of EXEC, folded moves), as
  // the reference differential tests compare translations with.
  bool reference = false;
};
void set_translator_options(const TranslatorOptions& options);
const TranslatorOptions& translator_options();

// When the translator was built (its source files' compile times) and the
// options it translates with: stored translations are keyed by it, so a
// changed translator misses them.
std::string translator_build();

// Graphics stages. Vertex shaders get the vertex id in v0 and the instance id
// in v3 (assumed); all 16 user SGPRs come from the HolySystem user values.
// Parameter exports (PARAMn) become TEXCOORDn outputs in register n + 1; a
// pixel shader's attribute n is the input TEXCOORDn in register n + 1,
// interpolated by the host (v_interp_p1/p2 read it; v_interp_mov reads it
// without interpolation). Colour exports become SV_Target outputs (packed
// 16-bit exports are taken as half floats), the depth export SV_Depth.
enum class GraphicsStage { Vertex, Pixel };

// What a translated graphics shader exports or reads, to link stages.
struct ShaderInterface {
  uint32_t params = 0;       // vertex: parameters exported; pixel: attributes read
  uint32_t flat_params = 0;  // pixel: attributes read without interpolation
  uint32_t targets = 0;      // pixel: render targets written
  bool position = false;     // vertex: exports a position
  bool depth = false;        // pixel: writes depth
  // pixel: attributes read as pairs of 16-bit floats (v_interp_*_f16), each
  // as two inputs HALFPARAM 2n and 2n + 1: the halves of channels x, y (low,
  // high, low, high), then of z, w, interpolated (half_param_register).
  uint32_t half_params = 0;
};

// The first of the two input registers of half-precision attribute n: after
// the attributes read whole (in registers n + 1), in attribute order.
uint32_t half_param_register(uint32_t params, uint32_t half_params, uint32_t n);

// The stage a graphics program was built for, from its exports (position
// exports: vertex; colour, depth or null exports and attribute
// interpolation: pixel). False when the code shows neither.
bool native_graphics_stage(const uint8_t* code, size_t size, GraphicsStage* stage);

// `consumer` (vertex programs, optional): the interface of the pixel program
// that follows. Only what it reads is output: attributes read whole as
// TEXCOORDn, attributes read as 16-bit halves as HALFPARAM pairs (each
// exported channel's halves as floats, in half_param_register's registers).
bool translate_graphics(const uint8_t* code, size_t size, GraphicsStage stage, std::vector<uint8_t>* dxbc,
                        ShaderInterface* link, std::string* error, const ShaderInterface* consumer = nullptr);

// NGG vertex programs (observed: titles' vertex programs that export
// primitives, request output space with s_sendmsg and share data through
// group shared memory, each group of vertex ids (v5 >> k) being one group of
// vertices and primitives). Translated into a host compute program, a thread
// group per vertex group (x) and instance (y), that writes the draw's records
// into the raw UAV u0 of space kNggSpace: vertex slot n at n * stride
// (position, then parameter k at 16 * (1 + k)), and from HolySystem's NGG
// vector x on three indices per primitive slot, a slot being group * group
// size + thread. Primitive slots the program does not export stay degenerate.
// A host draw of those indices with a vertex program that reads the records
// (u0 as t0 of the same space) draws what the console would.
inline constexpr uint32_t kNggSpace = 20;  // between the image spaces (kTextureSpace + ImageSlot) and the
                                           // integer image spaces
struct NggInfo {
  uint32_t group = 0;   // threads (vertex and primitive slots) per group
  uint32_t stride = 0;  // bytes per vertex record
};
bool native_ngg_program(const uint8_t* code, size_t size);
// The host vertex program of an NGG draw: SV_VertexID's record (t0 of space
// kNggSpace) as SV_Position and TEXCOORDn for the parameters in `params`.
std::vector<uint8_t> ngg_vertex_program(uint32_t stride, uint32_t params, uint32_t half_params = 0);
bool translate_ngg(const uint8_t* code, size_t size, std::vector<uint8_t>* dxbc, ShaderInterface* link, NggInfo* info,
                   std::string* error);

}  // namespace holy::d3d12
