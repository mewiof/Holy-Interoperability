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

// Register space of the translation's own bindings: u0-u63 (raw views of
// title memory, one per region) and b0 (HolySystem: region table, user
// SGPRs, counts).
inline constexpr uint32_t kTranslatedSpace = 8;
inline constexpr uint32_t kRegionTableSize = 64;

// HolySystem layout in 16-byte vectors.
inline constexpr uint32_t kSystemRegionVector = 0;   // [64] base lo, base hi, size, view index
inline constexpr uint32_t kSystemUserVector = 64;    // [4]  initial user SGPRs
inline constexpr uint32_t kSystemCountsVector = 68;  // x = region count
inline constexpr uint32_t kSystemVectors = 69;

// Image instructions address the bound shader-visible heaps through the
// index in descriptor dword 0 (shader/descriptor_layout.h): float texture
// arrays [0, unbounded) of each dimension in spaces kTextureSpace + slot,
// typed UAV arrays likewise, and the sampler array in kTextureSpace.
inline constexpr uint32_t kTextureSpace = 9;
enum ImageSlot : uint32_t {
  kImage2D = 0, kImage2DArray = 1, kImage3D = 2, kImage1D = 4, kImage1DArray = 5,
};

bool translate_compute(const uint8_t* code, size_t size, const ComputeShaderInfo& info, std::vector<uint8_t>* dxbc,
                       std::string* error);

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
};

// The stage a graphics program was built for, from its exports (position
// exports: vertex; colour, depth or null exports and attribute
// interpolation: pixel). False when the code shows neither.
bool native_graphics_stage(const uint8_t* code, size_t size, GraphicsStage* stage);

bool translate_graphics(const uint8_t* code, size_t size, GraphicsStage stage, std::vector<uint8_t>* dxbc,
                        ShaderInterface* link, std::string* error);

}  // namespace holy::d3d12
