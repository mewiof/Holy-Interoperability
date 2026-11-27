#include "core/libs/d3d12_xs/shader/geometry.h"

#include <cstring>

#include "core/libs/d3d12_xs/shader/dxbc.h"

namespace holy::d3d12 {

bool read_output_signature(const uint8_t* container, size_t size, std::vector<SignatureEntry>* out) {
  out->clear();
  auto read32 = [&](size_t at, uint32_t* value) {
    if (at + 4 > size) return false;
    std::memcpy(value, container + at, 4);
    return true;
  };
  uint32_t chunks = 0;
  if (!container || size < 32 || std::memcmp(container, "DXBC", 4) != 0 || !read32(28, &chunks) || chunks > 64) {
    return false;
  }
  for (uint32_t i = 0; i < chunks; ++i) {
    uint32_t at = 0, length = 0;
    if (!read32(32 + 4 * i, &at) || !read32(at + 4, &length) || at + 8 + static_cast<size_t>(length) > size) {
      return false;
    }
    if (std::memcmp(container + at, "OSGN", 4) != 0) continue;
    const uint8_t* chunk = container + at + 8;
    uint32_t count = 0, first = 0;
    std::memcpy(&count, chunk, 4);
    std::memcpy(&first, chunk + 4, 4);
    if (count > 64 || first + 24ull * count > length) return false;
    for (uint32_t e = 0; e < count; ++e) {
      const uint8_t* element = chunk + first + 24 * e;
      uint32_t fields[5];
      std::memcpy(fields, element, sizeof(fields));
      if (fields[0] >= length) return false;
      SignatureEntry entry;
      const char* name = reinterpret_cast<const char*>(chunk + fields[0]);
      entry.name.assign(name, strnlen(name, length - fields[0]));
      entry.index = fields[1];
      entry.system_value = fields[2];
      entry.component_type = fields[3];
      entry.reg = fields[4];
      entry.mask = element[20];
      out->push_back(std::move(entry));
    }
    return true;
  }
  return false;
}

std::vector<uint8_t> rectangle_list_program(const std::vector<SignatureEntry>& vertex_outputs) {
  namespace dx = dxbc;
  dx::Program program(dx::ProgramType::Geometry);
  program.dcl_global_flags(1);
  for (const SignatureEntry& e : vertex_outputs) {
    program.dcl_gs_input(3, e.reg, e.mask, e.system_value == dx::kPosition);
  }
  program.dcl_temps(1);
  program.dcl_gs_triangles_to_strips(4);
  for (const SignatureEntry& e : vertex_outputs) {
    if (e.system_value == dx::kPosition) {
      program.dcl_output_siv(e.reg, e.mask, dx::kPosition);
    } else {
      program.dcl_output(e.reg, e.mask);
    }
  }
  // The three given corners, then the fourth (v1 + v2 - v0): a strip of two
  // triangles covering the rectangle.
  for (uint32_t vertex = 0; vertex < 3; ++vertex) {
    for (const SignatureEntry& e : vertex_outputs) {
      program.emit(dx::kMov, {dx::output_register(e.reg, e.mask), dx::gs_input_xyzw(vertex, e.reg)});
    }
    program.emit(dx::kEmitStream, {dx::stream0()});
  }
  for (const SignatureEntry& e : vertex_outputs) {
    program.emit(dx::kAdd, {dx::temp_dst_mask(0, 0xF), dx::gs_input_xyzw(1, e.reg), dx::gs_input_xyzw(2, e.reg)});
    program.emit(dx::kAdd, {dx::output_register(e.reg, e.mask), dx::temp_xyzw(0), dx::neg(dx::gs_input_xyzw(0, e.reg))});
  }
  program.emit(dx::kEmitStream, {dx::stream0()});
  program.emit(dx::kCutStream, {dx::stream0()});
  program.emit(dx::kRet, {});
  std::vector<dx::SignatureElement> inputs, outputs;
  for (const SignatureEntry& e : vertex_outputs) {
    inputs.push_back({e.name.c_str(), e.index, e.system_value, e.component_type, e.reg, e.mask, e.mask});
    outputs.push_back({e.name.c_str(), e.index, e.system_value, e.component_type, e.reg, e.mask, 0});
  }
  return program.container(inputs, outputs);
}

}  // namespace holy::d3d12
