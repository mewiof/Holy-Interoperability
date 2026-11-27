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
  uint32_t position = 0xFFFFFFFF;
  for (const SignatureEntry& e : vertex_outputs) {
    program.dcl_gs_input(3, e.reg, e.mask, e.system_value == dx::kPosition);
    if (e.system_value == dx::kPosition) position = e.reg;
  }
  program.dcl_temps(11);
  program.dcl_gs_triangles_to_strips(6);
  for (const SignatureEntry& e : vertex_outputs) {
    if (e.system_value == dx::kPosition) {
      program.dcl_output_siv(e.reg, e.mask, dx::kPosition);
    } else {
      program.dcl_output(e.reg, e.mask);
    }
  }
  // The given triangle as it is.
  for (uint32_t vertex = 0; vertex < 3; ++vertex) {
    for (const SignatureEntry& e : vertex_outputs) {
      program.emit(dx::kMov, {dx::output_register(e.reg, e.mask), dx::gs_input_xyzw(vertex, e.reg)});
    }
    program.emit(dx::kEmitStream, {dx::stream0()});
  }
  program.emit(dx::kCutStream, {dx::stream0()});
  // The corner the rectangle's two given sides meet at is the vertex opposite
  // the longest edge (the diagonal). Observed: titles give the corner as v1
  // (top-left, bottom-left, bottom-right), where v0 + v2 - v1 completes the
  // rectangle; other code gives it as v0. Screen positions (x / w, y / w)
  // decide: r0-r2 the positions, r3 the squared lengths of the edges
  // opposite v0, v1 and v2, r5.x "the corner is v0", r5.y "it is v1".
  constexpr uint32_t kXyxx = 0x04, kWwww = 0xFF, kXxxx = 0x00, kYyyy = 0x55;
  if (position != 0xFFFFFFFF) {
    for (uint32_t v = 0; v < 3; ++v) {
      program.emit(dx::kMov, {dx::temp_dst_mask(v, 0xF), dx::gs_input_xyzw(v, position)});
      program.emit(dx::kDiv, {dx::temp_dst_mask(v, 0x3), dx::temp_swizzle(v, kXyxx), dx::temp_swizzle(v, kWwww)});
    }
    const uint32_t ends[3][2] = {{1, 2}, {2, 0}, {0, 1}};
    for (uint32_t c = 0; c < 3; ++c) {
      program.emit(dx::kAdd, {dx::temp_dst_mask(4, 0x3), dx::temp_swizzle(ends[c][0], kXyxx),
                              dx::neg(dx::temp_swizzle(ends[c][1], kXyxx))});
      program.emit(dx::kMul, {dx::temp_dst_mask(3, 1u << c), dx::temp(4, 0), dx::temp(4, 0)});
      program.emit(dx::kMad, {dx::temp_dst_mask(3, 1u << c), dx::temp(4, 1), dx::temp(4, 1), dx::temp(3, c)});
    }
    program.emit(dx::kGe, {dx::temp_dst_mask(5, 0x1), dx::temp(3, 0), dx::temp(3, 1)});
    program.emit(dx::kGe, {dx::temp_dst_mask(5, 0x4), dx::temp(3, 0), dx::temp(3, 2)});
    program.emit(dx::kAnd, {dx::temp_dst_mask(5, 0x1), dx::temp(5, 0), dx::temp(5, 2)});
    program.emit(dx::kLt, {dx::temp_dst_mask(5, 0x2), dx::temp(3, 0), dx::temp(3, 1)});
    program.emit(dx::kGe, {dx::temp_dst_mask(5, 0x4), dx::temp(3, 1), dx::temp(3, 2)});
    program.emit(dx::kAnd, {dx::temp_dst_mask(5, 0x2), dx::temp(5, 1), dx::temp(5, 2)});
  } else {
    program.emit(dx::kMov, {dx::temp_dst_mask(5, 0x3), dx::imm(0xFFFFFFFFu)});  // no position: the corner is v0
  }
  // The other triangle shares the diagonal, walked the other way (the
  // winding stays): with P -> Q the diagonal in the given triangle and C the
  // corner, it is Q, P, P + Q - C (= v0 + v1 + v2 - 2 C), for every output
  // (attributes that vary affinely over the rectangle complete the same way).
  // Corner v0: Q v2, P v1; corner v1: Q v0, P v2; corner v2: Q v1, P v0.
  const uint32_t pick[3][3] = {{2, 0, 1}, {1, 2, 0}, {0, 1, 2}};  // Q, P, C: (if v0, if v1, else)
  for (uint32_t which = 0; which < 3; ++which) {
    for (const SignatureEntry& e : vertex_outputs) {
      for (uint32_t v = 0; v < 3; ++v) {
        program.emit(dx::kMov, {dx::temp_dst_mask(6 + v, 0xF), dx::gs_input_xyzw(v, e.reg)});
      }
      const uint32_t* sel = pick[which];
      program.emit(dx::kMovc, {dx::temp_dst_mask(9, 0xF), dx::temp_swizzle(5, kYyyy), dx::temp_xyzw(6 + sel[1]),
                               dx::temp_xyzw(6 + sel[2])});
      program.emit(dx::kMovc, {dx::temp_dst_mask(9, 0xF), dx::temp_swizzle(5, kXxxx), dx::temp_xyzw(6 + sel[0]),
                               dx::temp_xyzw(9)});
      if (which < 2) {
        program.emit(dx::kMov, {dx::output_register(e.reg, e.mask), dx::temp_xyzw(9)});
      } else {
        program.emit(dx::kAdd, {dx::temp_dst_mask(10, 0xF), dx::temp_xyzw(6), dx::temp_xyzw(7)});
        program.emit(dx::kAdd, {dx::temp_dst_mask(10, 0xF), dx::temp_xyzw(10), dx::temp_xyzw(8)});
        program.emit(dx::kAdd, {dx::temp_dst_mask(9, 0xF), dx::temp_xyzw(9), dx::temp_xyzw(9)});
        program.emit(dx::kAdd, {dx::output_register(e.reg, e.mask), dx::temp_xyzw(10), dx::neg(dx::temp_xyzw(9))});
      }
    }
    program.emit(dx::kEmitStream, {dx::stream0()});
  }
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
