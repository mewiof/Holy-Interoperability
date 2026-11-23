#include <unknwn.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
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
  EXPECT(text.find("dcl_uav_raw U0[0:63], space=8") != std::string::npos);
  // Blocks at 0, 12 and 16, selected by comparisons on the next block.
  EXPECT(text.find("loop") != std::string::npos);
  EXPECT(text.find("l(16)") != std::string::npos);
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

TEST(untranslatable_code_is_reported) {
  // v_mov_b32 with a DPP16 operand (needs other lanes), then s_endpgm.
  const std::vector<uint8_t> code = words({0x7E0202FAu, 0x000000E4u, 0xBF810000u});
  ComputeShaderInfo info;
  std::vector<uint8_t> bytecode;
  std::string error;
  EXPECT(!translate_compute(code.data(), code.size(), info, &bytecode, &error));
  EXPECT(error.find("DPP") != std::string::npos);
  EXPECT(error.find("0x0") != std::string::npos);
}
