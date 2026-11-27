// Geometry programs for what the console GPU does in fixed function and the
// host does not.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace holy::d3d12 {

struct SignatureEntry {
  std::string name;
  uint32_t index = 0;
  uint32_t system_value = 0;
  uint32_t component_type = 0;
  uint32_t reg = 0;
  uint8_t mask = 0;
};

// The output signature (OSGN) of a DXBC container; false if it has none or
// it cannot be read.
bool read_output_signature(const uint8_t* container, size_t size, std::vector<SignatureEntry>* out);

// Rectangle lists (documented in public RDNA register references as
// DI_PT_RECTLIST): each three vertices are three corners of a rectangle, v1
// and v2 the neighbours of v0, and the GPU adds the fourth corner. The program
// takes the vertex program's outputs (`vertex_outputs`), passes each triangle
// on and adds the fourth corner as v1 + v2 - v0 for every output (attributes
// that vary affinely over the rectangle complete the same way), as a strip of
// four vertices.
std::vector<uint8_t> rectangle_list_program(const std::vector<SignatureEntry>& vertex_outputs);

}  // namespace holy::d3d12
