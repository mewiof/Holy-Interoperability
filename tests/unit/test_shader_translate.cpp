#include <unknwn.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "core/libs/inflate.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/d3d12_xs/shader_container.h"
#include "tests/unit/test.h"

using namespace holy::d3d12;

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

uint32_t adler32(const std::vector<uint8_t>& data) {
  uint32_t a = 1, b = 0;
  for (uint8_t byte : data) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

// A zlib stream holding `data` in one stored block.
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out = {0x78, 0x01, 0x01};
  const auto length = static_cast<uint16_t>(data.size());
  out.insert(out.end(), {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                         static_cast<uint8_t>(~length), static_cast<uint8_t>(~length >> 8)});
  out.insert(out.end(), data.begin(), data.end());
  const uint32_t check = adler32(data);
  out.insert(out.end(), {static_cast<uint8_t>(check >> 24), static_cast<uint8_t>(check >> 16),
                         static_cast<uint8_t>(check >> 8), static_cast<uint8_t>(check)});
  return out;
}

// Disassembles host bytecode with the system's shader compiler library.
std::string disassemble(const std::vector<uint8_t>& bytecode) {
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  if (!compiler) return {};
  using Disassemble = HRESULT(WINAPI*)(const void*, SIZE_T, UINT, const char*, IUnknown**);
  auto function = reinterpret_cast<Disassemble>(GetProcAddress(compiler, "D3DDisassemble"));
  IUnknown* blob = nullptr;
  std::string text;
  if (function && SUCCEEDED(function(bytecode.data(), bytecode.size(), 0, nullptr, &blob)) && blob) {
    // ID3DBlob: GetBufferPointer at slot 3, GetBufferSize at slot 4.
    auto* table = *reinterpret_cast<void***>(blob);
    auto pointer = reinterpret_cast<const char*(STDMETHODCALLTYPE*)(IUnknown*)>(table[3])(blob);
    auto size = reinterpret_cast<SIZE_T(STDMETHODCALLTYPE*)(IUnknown*)>(table[4])(blob);
    text.assign(pointer, size);
    blob->Release();
  }
  return text;
}

std::vector<uint8_t> words(std::initializer_list<uint32_t> list) {
  std::vector<uint8_t> out;
  for (uint32_t w : list) put32(out, w);
  return out;
}

}  // namespace

TEST(inflate_fixed_huffman) {
  const uint8_t stream[] = {0x78, 0xda, 0x4b, 0x4c, 0x4a, 0x4e, 0x44, 0x45, 0x00, 0x41, 0x7c, 0x06, 0xe5};
  std::vector<uint8_t> out;
  EXPECT(holy::lib::zlib_inflate(stream, sizeof(stream), &out));
  EXPECT(std::string(out.begin(), out.end()) == "abcabcabcabcabcabc");
}

TEST(inflate_dynamic_huffman) {
  // zlib level 9 output for the text below (dynamic Huffman block).
  const uint8_t stream[] = {
      0x78, 0xda, 0x9d, 0x94, 0xdd, 0x09, 0xc3, 0x40, 0x0c, 0x83, 0x57, 0xf1, 0x08, 0xcd, 0xe5, 0x7e, 0xe9, 0x34,
      0x29, 0x38, 0xe4, 0xe0, 0xc8, 0x85, 0xd4, 0x2f, 0xdd, 0xbe, 0x64, 0x84, 0xcf, 0xaf, 0x06, 0x61, 0x59, 0x96,
      0x34, 0xfa, 0xa9, 0xf2, 0x92, 0xb9, 0x8b, 0x1d, 0x2a, 0xc7, 0x1c, 0x3f, 0xe9, 0xa7, 0xe9, 0x3d, 0x2f, 0xbd,
      0xb7, 0x4f, 0x1f, 0xdd, 0x9e, 0xc1, 0x3e, 0x36, 0x53, 0x31, 0xfd, 0xda, 0x5b, 0xc6, 0x83, 0x58, 0x30, 0x22,
      0x62, 0x44, 0xe3, 0xac, 0x32, 0x86, 0x84, 0x84, 0x21, 0x2b, 0xdf, 0x12, 0xf9, 0x2d, 0x99, 0x0b, 0x56, 0xf9,
      0x57, 0x56, 0x2e, 0x18, 0xe7, 0x15, 0x0b, 0x86, 0x94, 0xc0, 0x89, 0xf1, 0xe3, 0xb9, 0x5e, 0x99, 0x6f, 0x69,
      0x0e, 0x87, 0xf1, 0xb7, 0x14, 0x47, 0x86, 0xf9, 0x2d, 0x89, 0x13, 0x6b, 0x8e, 0xb4, 0x38, 0xba, 0xc2, 0x51,
      0x48, 0x8e, 0x5b, 0x38, 0xb1, 0xc4, 0xff, 0x52, 0xb9, 0x2b, 0xb9, 0xc5, 0x02, 0xcf, 0x64, 0xe5, 0xc4, 0x12,
      0x17, 0x2c, 0x70, 0x57, 0x56, 0x47, 0xbb, 0x3a, 0xaa, 0x92, 0x8b, 0xbc, 0xf0, 0x2d, 0x95, 0xc7, 0x25, 0x23,
      0xc8, 0x1f, 0xd7, 0xee, 0xe3, 0xc4};
  std::string expected;
  for (int i = 0; i < 40; ++i) {
    char line[96];
    std::snprintf(line, sizeof(line), "line %d of the holy interoperability inflate test; ", i * i % 97);
    expected += line;
  }
  std::vector<uint8_t> out;
  EXPECT(holy::lib::zlib_inflate(stream, sizeof(stream), &out));
  EXPECT(std::string(out.begin(), out.end()) == expected);
}

TEST(inflate_rejects_bad_input) {
  std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  std::vector<uint8_t> stream = zlib_stored(data);
  std::vector<uint8_t> out;
  EXPECT(holy::lib::zlib_inflate(stream.data(), stream.size(), &out));
  EXPECT(out == data);
  stream.back() ^= 1;  // checksum mismatch
  EXPECT(!holy::lib::zlib_inflate(stream.data(), stream.size(), &out));
  const uint8_t not_zlib[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
  EXPECT(!holy::lib::zlib_inflate(not_zlib, sizeof(not_zlib), &out));
  EXPECT(!holy::lib::zlib_inflate(stream.data(), 3, &out));
}

TEST(native_compute_program_is_found) {
  // Native program block: "FSSM", code record at 0x100 (code at +0x28),
  // three user SGPRs; the code is s_endpgm.
  std::vector<uint8_t> block(0x130, 0);
  std::memcpy(block.data(), "FSSM", 4);
  const uint32_t record = 0x100, user = 3, endpgm = 0xBF810000u;
  std::memcpy(block.data() + 0xC0, &record, 4);
  std::memcpy(block.data() + 0xF8, &user, 4);
  std::memcpy(block.data() + 0x128, &endpgm, 4);
  const std::vector<uint8_t> stream = zlib_stored(block);
  std::vector<uint8_t> dxil = {'X', 'D', 'A', 'T'};
  put32(dxil, 5);
  put32(dxil, 0);
  put32(dxil, 1);
  put32(dxil, 32);  // entry: offset, size, kind 16
  put32(dxil, static_cast<uint32_t>(stream.size()));
  put32(dxil, 16);
  put32(dxil, 0);
  dxil.insert(dxil.end(), stream.begin(), stream.end());
  while (dxil.size() % 4) dxil.push_back(0);
  std::vector<uint8_t> psv(56, 0);
  const uint32_t info_size = 52, x = 8, y = 4, z = 2;
  std::memcpy(psv.data(), &info_size, 4);
  std::memcpy(psv.data() + 40, &x, 4);
  std::memcpy(psv.data() + 44, &y, 4);
  std::memcpy(psv.data() + 48, &z, 4);

  std::vector<uint8_t> container = {'D', 'X', 'B', 'C'};
  container.resize(20, 0);
  put32(container, 1);
  put32(container, 0);
  put32(container, 2);
  put32(container, 40);
  put32(container, static_cast<uint32_t>(40 + 8 + psv.size()));
  container.insert(container.end(), {'P', 'S', 'V', '0'});
  put32(container, static_cast<uint32_t>(psv.size()));
  container.insert(container.end(), psv.begin(), psv.end());
  container.insert(container.end(), {'D', 'X', 'I', 'L'});
  put32(container, static_cast<uint32_t>(dxil.size()));
  container.insert(container.end(), dxil.begin(), dxil.end());

  NativeComputeProgram program;
  EXPECT(native_compute_program(container.data(), container.size(), &program));
  EXPECT(program.root_parameters == 3);
  EXPECT(program.threads[0] == 8 && program.threads[1] == 4 && program.threads[2] == 2);
  EXPECT(program.code_offset == 0x128);
  EXPECT(program.block.size() == block.size());

  // Without a native program stream the container is not usable.
  container[container.size() - dxil.size() + 32] = 0x12;  // stream header byte
  NativeComputeProgram none;
  EXPECT(!native_compute_program(container.data(), container.size(), &none));
}

TEST(translated_program_is_valid_bytecode) {
  // s_mov_b32 s5, 7; s_cmp_eq_u32 s5, 7; s_cbranch_scc1 +1; v_mov_b32 v1, s5; s_endpgm
  const std::vector<uint8_t> code = words({0xBE850387u, 0xBF068705u, 0xBF850001u, 0x7E020205u, 0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 64;
  info.user_sgprs = 2;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.empty());
  EXPECT(bytecode.size() > 64 && std::memcmp(bytecode.data(), "DXBC", 4) == 0);
  uint8_t digest[16];
  dxbc_checksum(bytecode.data(), bytecode.size(), digest);
  EXPECT(std::memcmp(digest, bytecode.data() + 4, 16) == 0);
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("cs_5_1") != std::string::npos);
  EXPECT(text.find("dcl_thread_group 64, 1, 1") != std::string::npos);
  EXPECT(text.find("dcl_uav_raw U0[0:65535], space=8") != std::string::npos);
  // Blocks at 0, 12 and 16, selected by comparisons on the next block.
  EXPECT(text.find("loop") != std::string::npos);
  EXPECT(text.find("l(16)") != std::string::npos);
}

TEST(ngg_vertex_groups_come_from_either_shift_of_the_vertex_id) {
  // v_lshrrev_b32 v1, 7, v5; exp prim v0 done; exp pos0 v0..v3 done; s_endpgm.
  const std::vector<uint8_t> vector_form =
      words({0x2C020A87u, 0xF8000941u, 0x00000000u, 0xF80008CFu, 0x03020100u, 0xBF810000u});
  // v_readfirstlane_b32 s2, v5; s_lshr_b32 s2, s2, 7; the same exports.
  const std::vector<uint8_t> scalar_form =
      words({0x7E040505u, 0x90028702u, 0xF8000941u, 0x00000000u, 0xF80008CFu, 0x03020100u, 0xBF810000u});
  for (const std::vector<uint8_t>* code : {&vector_form, &scalar_form}) {
    EXPECT(native_ngg_program(code->data(), code->size()));
    std::vector<uint8_t> cs;
    ShaderInterface link;
    NggInfo info;
    std::string error;
    EXPECT(translate_ngg(code->data(), code->size(), &cs, &link, &info, &error));
    EXPECT(error.empty());
    EXPECT(info.group == 128);
    EXPECT(disassemble(cs).find("dcl_thread_group 128, 1, 1") != std::string::npos);
  }
}

TEST(translated_graphics_stages_link) {
  // Vertex: exp param0 v0..v3; exp pos0 v0..v3 done; s_endpgm.
  const std::vector<uint8_t> vs_code = words({0xF800020Fu, 0x03020100u, 0xF80008CFu, 0x03020100u, 0xBF810000u});
  // Pixel: v_interp_mov_f32 v2, p0, attr0.x; exp mrt0 v2 done; s_endpgm.
  const std::vector<uint8_t> ps_code = words({0xC80A0002u, 0xF8000801u, 0x00000002u, 0xBF810000u});
  std::vector<uint8_t> vs, ps;
  ShaderInterface vs_link, ps_link;
  std::string error;
  EXPECT(translate_graphics(vs_code.data(), vs_code.size(), GraphicsStage::Vertex, &vs, &vs_link, &error));
  EXPECT(translate_graphics(ps_code.data(), ps_code.size(), GraphicsStage::Pixel, &ps, &ps_link, &error));
  EXPECT(vs_link.position && vs_link.params == 1);
  EXPECT(ps_link.params == 1 && ps_link.flat_params == 1 && ps_link.targets == 1 && !ps_link.depth);
  const std::string vs_text = disassemble(vs), ps_text = disassemble(ps);
  EXPECT(vs_text.find("vs_5_1") != std::string::npos);
  EXPECT(vs_text.find("dcl_input_sgv v0.x, vertex_id") != std::string::npos);
  EXPECT(vs_text.find("dcl_output_siv o0.xyzw, position") != std::string::npos);
  EXPECT(vs_text.find("dcl_output o1.xyzw") != std::string::npos);
  EXPECT(vs_text.find("TEXCOORD") != std::string::npos);
  EXPECT(ps_text.find("ps_5_1") != std::string::npos);
  EXPECT(ps_text.find("dcl_input_ps constant v1.xyzw") != std::string::npos);
  EXPECT(ps_text.find("dcl_output o0.xyzw") != std::string::npos);
  EXPECT(ps_text.find("SV_Target") != std::string::npos);
  // Exports into a stage that has none are reported.
  EXPECT(!translate_compute(vs_code.data(), vs_code.size(), ComputeShaderInfo{}, &vs, &error));
}

TEST(vertex_programs_output_what_their_pixel_program_reads) {
  // Vertex: exp param0 v0..v3; exp param1 v0..v3; exp pos0 v0..v3 done; s_endpgm.
  const std::vector<uint8_t> vs_code =
      words({0xF800020Fu, 0x03020100u, 0xF800021Fu, 0x03020100u, 0xF80008CFu, 0x03020100u, 0xBF810000u});
  // A pixel program reading attribute 0 whole and attribute 1 as halves.
  ShaderInterface consumer;
  consumer.params = 1;
  consumer.half_params = 2;
  std::vector<uint8_t> vs;
  ShaderInterface vs_link;
  std::string error;
  EXPECT(translate_graphics(vs_code.data(), vs_code.size(), GraphicsStage::Vertex, &vs, &vs_link, &error, &consumer));
  EXPECT(error.empty());
  EXPECT(vs_link.params == 3);  // what the program exports
  const std::string text = disassemble(vs);
  EXPECT(text.find("dcl_output o1.xyzw") != std::string::npos);  // TEXCOORD0
  // Attribute 1's halves, after the whole attributes: HALFPARAM2 and 3 in o2 and o3.
  EXPECT(text.find("HALFPARAM") != std::string::npos);
  EXPECT(text.find("dcl_output o2.xyzw") != std::string::npos);
  EXPECT(text.find("dcl_output o3.xyzw") != std::string::npos);
  EXPECT(text.find("f16tof32") != std::string::npos);
  EXPECT(half_param_register(consumer.params, consumer.half_params, 1) == 2);
}

TEST(untranslatable_code_is_reported) {
  // v_mov_b32 with a DPP16 row_bcast:15 operand (a control the hardware
  // generation lacks), then s_endpgm, in each stage.
  const std::vector<uint8_t> broadcast = words({0x7E0202FAu, 0xFF014200u, 0xBF810000u});
  std::vector<uint8_t> bytecode;
  ShaderInterface link;
  std::string error;
  EXPECT(!translate_graphics(broadcast.data(), broadcast.size(), GraphicsStage::Pixel, &bytecode, &link, &error));
  EXPECT(error.find("DPP control 0x142 in a pixel program") != std::string::npos);
  EXPECT(error.find("0x0") != std::string::npos);
  EXPECT(!translate_graphics(broadcast.data(), broadcast.size(), GraphicsStage::Vertex, &bytecode, &link, &error));
  EXPECT(error.find("DPP control 0x142 in a vertex program") != std::string::npos);
  EXPECT(!translate_compute(broadcast.data(), broadcast.size(), ComputeShaderInfo{}, &bytecode, &error));
  EXPECT(error.find("DPP control 0x142") != std::string::npos);
  // Reads outside the quad translate (as lanes without a pixel), in pixel
  // and vertex programs: v_mov_b32 with row_shr:1.
  const std::vector<uint8_t> shifted = words({0x7E0202FAu, 0xFF011100u, 0xBF810000u});
  EXPECT(translate_graphics(shifted.data(), shifted.size(), GraphicsStage::Pixel, &bytecode, &link, &error));
  EXPECT(translate_graphics(shifted.data(), shifted.size(), GraphicsStage::Vertex, &bytecode, &link, &error));
}

TEST(cross_lane_operations_exchange_through_group_memory) {
  // v_mov_b32 v1, v0 quad_perm:[3,2,1,0]; v_readlane_b32 s0, v1, 5;
  // v_permlanex16_b32 v2, v1, s0, s0; s_endpgm.
  const std::vector<uint8_t> code = words({0x7E0202FAu, 0xFF001B00u, 0xD7600000u, 0x00010B01u, 0xD7780002u,
                                           0x00000101u, 0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 32;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  const std::string text = disassemble(bytecode);
  // Two buffers of 8 bytes for each of the 32 threads.
  EXPECT(text.find("dcl_tgsm_raw g1, 512") != std::string::npos);
  EXPECT(text.find("store_raw g1") != std::string::npos);
  EXPECT(text.find("ld_raw") != std::string::npos);
  EXPECT(text.find("sync_uglobal_g_t") != std::string::npos);
}

TEST(native_compute_registers_are_read) {
  // Observed register block: 8 x 8 threads; RSRC2 0xB8A: 5 user SGPRs,
  // group ids x, y and z, thread ids x and y, no LDS.
  uint32_t registers[7] = {0, 0, 0, 8u | (8u << 16), 1, 0, 0xB8Au};
  ComputeShaderInfo info;
  std::string error;
  EXPECT(native_compute_registers(registers, sizeof(registers), &info, &error));
  EXPECT(info.threads[0] == 8 && info.threads[1] == 8 && info.threads[2] == 1);
  EXPECT(info.user_sgprs == 5);
  EXPECT(info.tgid_enabled[0] && info.tgid_enabled[1] && info.tgid_enabled[2]);
  EXPECT(info.lds_bytes == 32768);
  registers[6] = 0xB8Au | (4u << 15);  // 4 x 128 dwords of LDS
  EXPECT(native_compute_registers(registers, sizeof(registers), &info, &error) && info.lds_bytes == 2048);
  registers[6] = 0xB8Bu;  // scratch
  EXPECT(!native_compute_registers(registers, sizeof(registers), &info, &error));
  EXPECT(!native_compute_registers(registers, 20, &info, &error));
}

TEST(graphics_stage_is_told_by_the_code) {
  // exp pos0 v0..v3 done: vertex; v_interp_mov_f32 then exp mrt0: pixel.
  const std::vector<uint8_t> vs_code = words({0xF80008CFu, 0x03020100u, 0xBF810000u});
  const std::vector<uint8_t> ps_code = words({0xC80A0002u, 0xF8000801u, 0x00000002u, 0xBF810000u});
  const std::vector<uint8_t> neither = words({0xBF810000u});
  GraphicsStage stage = GraphicsStage::Pixel;
  EXPECT(native_graphics_stage(vs_code.data(), vs_code.size(), &stage) && stage == GraphicsStage::Vertex);
  EXPECT(native_graphics_stage(ps_code.data(), ps_code.size(), &stage) && stage == GraphicsStage::Pixel);
  EXPECT(!native_graphics_stage(neither.data(), neither.size(), &stage));
}

TEST(typed_loads_gathers_and_half_class_tests_translate) {
  // tbuffer_load_format_xyzw v[0:3], v4, s[0:3] format:56 (8_8_8_8_UNORM) idxen;
  // image_gather4_lz v[0:3], v[4:5], s[8:15], s[16:19] dmask:0x1 dim:2D;
  // v_cmp_class_f16 vcc, v0, v1; s_endpgm.
  const std::vector<uint8_t> code = words({0xE9C32000u, 0x80000004u, 0xF11C0108u, 0x00820004u, 0x7D1E0300u,
                                           0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 64;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.empty());
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("gather4") != std::string::npos);
  EXPECT(text.find("ubfe") != std::string::npos);  // 8-bit components unpacked
  // A format number past the RDNA 2 table is reported.
  const std::vector<uint8_t> bad = words({0xEBC32000u, 0x80000004u, 0xBF810000u});
  EXPECT(!translate_compute(bad.data(), bad.size(), info, &bytecode, &error));
  EXPECT(error.find("typed buffer format 120") != std::string::npos);
}

TEST(image_data_of_16_bits_is_packed_in_halves) {
  // image_sample_lz v[0:1], v[4:5], s[8:15], s[16:19] dmask:0xf dim:2D d16;
  // image_store v[0:1], v[4:5], s[8:15] dmask:0xf dim:2D d16; s_endpgm.
  const std::vector<uint8_t> code = words({0xF09C0F08u, 0x80820004u, 0xF0200F08u, 0x80020004u, 0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 64;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.empty());
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("f32tof16") != std::string::npos);  // sampled values packed as halves
  EXPECT(text.find("f16tof32") != std::string::npos);  // stored halves unpacked
  EXPECT(text.find("store_uav_typed") != std::string::npos);
}

TEST(cube_multisampled_and_gradient_images_translate) {
  // image_sample_d v[0:3], v[4:9], s[8:15], s[16:19] dmask:0xf dim:2D;
  // image_sample_lz v[0:3], v[4:6], s[8:15], s[16:19] dmask:0xf dim:CUBE;
  // image_load v[0:3], v[4:6], s[8:15] dmask:0xf dim:2D_MSAA;
  // image_sample_lz v[0:3], v[4:5], s[8:15], s[16:19] dmask:0xf dim:2D unorm;
  // image_gather4_l v[0:3], v[4:6], s[8:15], s[16:19] dmask:0x1 dim:2D; s_endpgm.
  const std::vector<uint8_t> code =
      words({0xF0880F08u, 0x00820004u, 0xF09C0F18u, 0x00820004u, 0xF0000F30u, 0x00020004u, 0xF09C1F08u,
             0x00820004u, 0xF1100108u, 0x00820004u, 0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 64;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.empty());
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("sample_d") != std::string::npos);
  EXPECT(text.find("texturecube") != std::string::npos);
  EXPECT(text.find("texture2dms") != std::string::npos);
  EXPECT(text.find("ldms") != std::string::npos);
  EXPECT(text.find("resinfo") != std::string::npos);  // unnormalised coordinates over the size
  EXPECT(text.find("div ") != std::string::npos);
  EXPECT(text.find("gather4") != std::string::npos);
  // image_get_lod outside a pixel program: level 0 (partial: the console
  // derives it from quads of lanes), without the host's level instruction,
  // which only pixel shaders have.
  const std::vector<uint8_t> lod = words({0xF1800F08u, 0x00820004u, 0xBF810000u});
  error.clear();
  EXPECT(translate_compute(lod.data(), lod.size(), info, &bytecode, &error));
  EXPECT(error.empty());
  EXPECT(disassemble(bytecode).find("lod ") == std::string::npos);
}

TEST(image_atomics_use_integer_views) {
  // image_atomic_umin v1, v[2:3], s[8:15] dmask:0x1 dim:2D glc;
  // image_atomic_cmpswap v[4:5], v[2:3], s[8:15] dmask:0x3 dim:2D; s_endpgm.
  const std::vector<uint8_t> code = words({0xF0542108u, 0x00020102u, 0xF0400308u, 0x00020402u, 0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 64;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.empty());
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("imm_atomic_umin") != std::string::npos);
  EXPECT(text.find("imm_atomic_cmp_exch") != std::string::npos);
  EXPECT(text.find("(uint,uint,uint,uint)") != std::string::npos);  // an integer declaration
}

TEST(image_atomics_of_64_bits_read_combine_and_write) {
  // image_atomic_umax v[8:9], v[4:5], s[32:39] dmask:0x3 dim:2D (as titles'
  // pixel programs carry it); exp mrt0 off done; s_endpgm.
  const std::vector<uint8_t> code = words({0xF05C1308u, 0x00080804u, 0xF8000800u, 0x00000000u, 0xBF810000u});
  std::vector<uint8_t> bytecode;
  ShaderInterface link;
  std::string error;
  EXPECT(translate_graphics(code.data(), code.size(), GraphicsStage::Pixel, &bytecode, &link, &error));
  EXPECT(error.empty());
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("ld_uav_typed") != std::string::npos);
  EXPECT(text.find("store_uav_typed") != std::string::npos);
  EXPECT(text.find("_rov") != std::string::npos);           // rasterizer-ordered: per pixel
  EXPECT(text.find("Raster Ordered UAVs") != std::string::npos);  // declared in the features part
}

TEST(scalar_64_bit_field_mask_translates) {
  // s_bfm_b64 s[0:1], s2, s3; s_endpgm.
  const std::vector<uint8_t> code = words({0x92800302u, 0xBF810000u});
  ComputeShaderInfo info;
  info.threads[0] = 64;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.empty());
}

TEST(pixel_quad_permutes_use_fine_derivatives) {
  // v_mov_b32_dpp v1, v0 quad_perm:[1,0,3,2]; exp mrt0 v1, v1, v1, v1 done;
  // s_endpgm.
  const std::vector<uint8_t> code = words({0x7E0202FAu, 0xFF00B100u, 0xF800080Fu, 0x01010101u, 0xBF810000u});
  std::vector<uint8_t> bytecode;
  ShaderInterface link;
  std::string error;
  EXPECT(translate_graphics(code.data(), code.size(), GraphicsStage::Pixel, &bytecode, &link, &error));
  EXPECT(error.empty());
  const std::string text = disassemble(bytecode);
  EXPECT(text.find("dcl_input_ps_siv linear noperspective v0.xy, position") != std::string::npos);
  EXPECT(text.find("deriv_rtx_fine") != std::string::npos);
  EXPECT(text.find("deriv_rty_fine") != std::string::npos);
  EXPECT(text.find("SV_Position") != std::string::npos);
}
