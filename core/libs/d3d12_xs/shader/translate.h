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
  uint32_t user_sgprs = 2;  // s[0:1] = root table address
  // 32 or 64; 0: decided from the code (assumed: 64 when it writes EXEC with
  // 64-bit scalar operations, 32 otherwise).
  uint32_t wave_size = 0;
  bool tgid_enabled[3] = {true, true, true};
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

bool translate_compute(const uint8_t* code, size_t size, const ComputeShaderInfo& info, std::vector<uint8_t>* dxbc,
                       std::string* error);

}  // namespace holy::d3d12
