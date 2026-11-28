// Console shader containers (see shader_container.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/shader/translate.h"

namespace holy::d3d12 {

enum class ShaderFormat {
  Unknown,     // not a shader container
  Desktop,     // already a desktop container; copied unchanged
  Rebuilt,     // console container with bitcode; rebuilt as a desktop container
  NativeOnly,  // console container without bitcode: only native GPU code
};

ShaderFormat rebuild_desktop_container(const void* blob, size_t size, std::vector<uint8_t>& out);

// The native program of a console shader container.
struct NativeProgram {
  std::vector<uint8_t> block;    // the decompressed native program block
  size_t code_offset = 0;        // machine code start within `block`
  uint32_t root_parameters = 0;  // matches the root signature's parameter count
};
bool native_program(const void* blob, size_t size, NativeProgram* out);

// A compute shader's native program and its thread group size.
struct NativeComputeProgram : NativeProgram {
  uint32_t threads[3] = {};
};
bool native_compute_program(const void* blob, size_t size, NativeComputeProgram* out);

// The documented DXBC container checksum of `container` (written to digest).
void dxbc_checksum(const uint8_t* container, size_t size, uint8_t digest[16]);

// The compute registers of a pipeline created from native code (device slot
// 67): dword 3 the thread group width | height << 16, dword 4 its depth,
// dword 6 COMPUTE_PGM_RSRC2 (user SGPR count, group id and group information
// enables, LDS size).
// Observed layout; the RSRC2 fields follow the public RDNA register layout.
inline constexpr size_t kNativeComputeRegisterBytes = 28;
bool native_compute_registers(const uint32_t* registers, size_t size, ComputeShaderInfo* info, std::string* error);

// A graphics pipeline created from native code (device slot 57). Observed
// description: the root signature, (pointer, size) of a block of render
// state registers and of a pipeline block, then three stage slots, each a
// (code, code size, metadata, metadata size) quadruple or zero. Slots can be
// empty before a used one, and stages come in no fixed slot order; the stage
// is told by the code (native_graphics_stage). Metadata: dword 1 its size less 12, dword 3 high
// half the number of 32-byte records of kind 0x41 that follow.
//
// Render state block (dword offsets), whose values follow the public RDNA
// register layouts: 0x1E CB_TARGET_MASK, 0x28 PA_SU_SC_MODE_CNTL, 0x34-0x3B
// CB_BLEND0-7_CONTROL. Pipeline block: dword 3 the render target count,
// bytes 16-23 one DXGI format per render target, dword 6 zero in every
// pipeline seen (assumed: the depth format), 7 the sample mask, 9 byte 2 the
// primitive topology type in bits 0-3 (bit 4 is set in a few pipelines and
// not understood), 10 the sample count.
struct NativeGraphicsDescription {
  const uint8_t* vertex_code = nullptr;
  size_t vertex_size = 0;
  const uint8_t* pixel_code = nullptr;
  size_t pixel_size = 0;
  uint32_t render_targets = 0;
  uint32_t formats[8] = {};
  uint32_t sample_mask = 0;
  uint32_t sample_count = 1;
  uint32_t topology_type = 0;
  uint32_t target_mask = 0;
  uint32_t mode_control = 0;
  uint32_t blend[8] = {};
  uint32_t depth_control = 0;    // DB_DEPTH_CONTROL (observed values; see native_graphics_description)
  uint32_t stencil_control = 0;  // DB_STENCIL_CONTROL
  uint32_t stencil_masks = 0;    // DB_STENCILREFMASK (front faces)
  uint32_t stencil_masks_back = 0;  // DB_STENCILREFMASK_BF
  const uint32_t* state = nullptr;  // the render state block, for descriptions
  size_t state_bytes = 0;
};
// The render state block has been seen at 0x17C, 0x184 and 0x18C bytes with
// the same dwords up to the blend controls; only those are required (in the
// later layout, which has its registers 0x15 dwords further on, 0x144).
inline constexpr size_t kNativeRenderStateBytes = 0xF0;
inline constexpr size_t kNativeRenderStateLaterBytes = 0x144;
inline constexpr size_t kNativePipelineBlockBytes = 0x30;
bool native_graphics_description(const uint64_t* desc, NativeGraphicsDescription* out, std::string* error);

}  // namespace holy::d3d12
