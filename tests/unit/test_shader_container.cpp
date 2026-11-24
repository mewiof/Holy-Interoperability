#include <windows.h>

#include <cstring>
#include <vector>

#include "core/libs/d3d12_xs/shader_container.h"
#include "tests/unit/test.h"

using namespace holy::d3d12;

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

uint32_t get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// A container in the console layout: an RTS0 part with a console flag bit
// and a DXIL part holding an XDAT block with a program header and bitcode.
std::vector<uint8_t> console_container(bool with_bitcode) {
  std::vector<uint8_t> rts0(24, 0);
  rts0[0] = 2;                                        // version
  const uint32_t flags = 0x1000000 | 0x1;             // console bit + a desktop bit
  std::memcpy(rts0.data() + 20, &flags, 4);
  std::vector<uint8_t> dxil;
  dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
  put32(dxil, 5);
  const std::vector<uint8_t> bitcode = with_bitcode ? std::vector<uint8_t>{'B', 'C', 0xC0, 0xDE, 1, 2, 3, 4}
                                                    : std::vector<uint8_t>{'N', 'O', 'B', 'C'};
  put32(dxil, 0x50065);  // compute 6.5
  put32(dxil, static_cast<uint32_t>((24 + bitcode.size()) / 4));
  dxil.insert(dxil.end(), {'D', 'X', 'I', 'L'});
  put32(dxil, 0x105);
  put32(dxil, 16);
  put32(dxil, static_cast<uint32_t>(bitcode.size()));
  dxil.insert(dxil.end(), bitcode.begin(), bitcode.end());

  std::vector<uint8_t> out = {'D', 'X', 'B', 'C'};
  out.resize(20, 0);
  out.insert(out.end(), {1, 0, 0, 0});
  put32(out, 0);
  put32(out, 2);
  put32(out, 40);
  put32(out, static_cast<uint32_t>(40 + 8 + rts0.size()));
  out.insert(out.end(), {'R', 'T', 'S', '0'});
  put32(out, static_cast<uint32_t>(rts0.size()));
  out.insert(out.end(), rts0.begin(), rts0.end());
  out.insert(out.end(), {'D', 'X', 'I', 'L'});
  put32(out, static_cast<uint32_t>(dxil.size()));
  out.insert(out.end(), dxil.begin(), dxil.end());
  const auto total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  return out;
}

}  // namespace

// The checksum must reproduce the digest the host's own shader compiler
// writes into the containers it produces.
TEST(dxbc_checksum_matches_the_host_compiler) {
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  EXPECT(compiler != nullptr);
  if (!compiler) return;
  using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, void**, void**);
  const auto compile = reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile"));
  EXPECT(compile != nullptr);
  if (!compile) return;
  const char source[] = "RWByteAddressBuffer b : register(u0); [numthreads(1,1,1)] void main() { b.Store(0, 1); }";
  void* blob = nullptr;
  const HRESULT hr = compile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob,
                             nullptr);
  EXPECT(SUCCEEDED(hr) && blob);
  if (!blob) return;
  using PointerFn = void*(STDMETHODCALLTYPE*)(void*);
  using SizeFn = SIZE_T(STDMETHODCALLTYPE*)(void*);
  void** vtable = *static_cast<void***>(blob);
  const auto* data = static_cast<const uint8_t*>(reinterpret_cast<PointerFn>(vtable[3])(blob));
  const SIZE_T size = reinterpret_cast<SizeFn>(vtable[4])(blob);
  uint8_t digest[16];
  dxbc_checksum(data, size, digest);
  EXPECT(std::memcmp(digest, data + 4, 16) == 0);

  std::vector<uint8_t> out;
  EXPECT(rebuild_desktop_container(data, size, out) == ShaderFormat::Desktop);
  EXPECT(out.size() == size && std::memcmp(out.data(), data, size) == 0);
  reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(vtable[2])(blob);
}

TEST(console_container_with_bitcode_is_rebuilt) {
  const std::vector<uint8_t> input = console_container(true);
  std::vector<uint8_t> out;
  EXPECT(rebuild_desktop_container(input.data(), input.size(), out) == ShaderFormat::Rebuilt);
  EXPECT(out.size() > 32 && std::memcmp(out.data(), "DXBC", 4) == 0);
  EXPECT(get32(out.data() + 24) == out.size());
  uint8_t digest[16];
  dxbc_checksum(out.data(), out.size(), digest);
  EXPECT(std::memcmp(digest, out.data() + 4, 16) == 0);
  const uint32_t parts = get32(out.data() + 28);
  bool saw_program = false, saw_masked_flags = false;
  for (uint32_t i = 0; i < parts; ++i) {
    const uint8_t* part = out.data() + get32(out.data() + 32 + 4 * i);
    if (!std::memcmp(part, "DXIL", 4)) {
      saw_program = !std::memcmp(part + 8 + 8, "DXIL", 4) && !std::memcmp(part + 8 + 24, "BC\xC0\xDE", 4);
    }
    if (!std::memcmp(part, "RTS0", 4)) saw_masked_flags = get32(part + 8 + 20) == 0x1;
  }
  EXPECT(saw_program);
  EXPECT(saw_masked_flags);
}

TEST(console_container_without_bitcode_is_native_only) {
  const std::vector<uint8_t> input = console_container(false);
  std::vector<uint8_t> out;
  EXPECT(rebuild_desktop_container(input.data(), input.size(), out) == ShaderFormat::NativeOnly);
}

TEST(malformed_containers_are_rejected) {
  std::vector<uint8_t> input = console_container(true);
  input.resize(36);
  std::vector<uint8_t> out;
  EXPECT(rebuild_desktop_container(input.data(), input.size(), out) == ShaderFormat::Unknown);
  const uint8_t junk[8] = {1, 2, 3};
  EXPECT(rebuild_desktop_container(junk, sizeof(junk), out) == ShaderFormat::Unknown);
}

namespace {

// A container whose DXIL part starts with a desktop program header and keeps
// the console block between the header and the program, as console graphics
// shaders do; the program is either bitcode or the "NOBC" marker.
std::vector<uint8_t> header_first_container(bool with_bitcode) {
  const std::vector<uint8_t> program = with_bitcode ? std::vector<uint8_t>{'B', 'C', 0xC0, 0xDE, 1, 2, 3, 4}
                                                    : std::vector<uint8_t>{'N', 'O', 'B', 'C'};
  std::vector<uint8_t> dxil;
  put32(dxil, 0x10066);  // vertex 6.6
  put32(dxil, 0);        // size in dwords, patched below
  dxil.insert(dxil.end(), {'D', 'X', 'I', 'L'});
  put32(dxil, 0x106);
  put32(dxil, 16 + 16);  // program offset from the "DXIL" tag: past a 16-byte console block
  put32(dxil, static_cast<uint32_t>(program.size()));
  dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
  put32(dxil, 3);
  put32(dxil, 0);
  put32(dxil, 0);
  dxil.insert(dxil.end(), program.begin(), program.end());
  const auto dwords = static_cast<uint32_t>(dxil.size() / 4);
  std::memcpy(dxil.data() + 4, &dwords, 4);
  std::vector<uint8_t> out = {'D', 'X', 'B', 'C'};
  out.resize(20, 0);
  out.insert(out.end(), {1, 0, 0, 0});
  put32(out, 0);
  put32(out, 1);
  put32(out, 36);
  out.insert(out.end(), {'D', 'X', 'I', 'L'});
  put32(out, static_cast<uint32_t>(dxil.size()));
  out.insert(out.end(), dxil.begin(), dxil.end());
  const auto total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  return out;
}

}  // namespace

TEST(header_first_console_containers) {
  std::vector<uint8_t> out;
  const std::vector<uint8_t> native = header_first_container(false);
  EXPECT(rebuild_desktop_container(native.data(), native.size(), out) == ShaderFormat::NativeOnly);
  const std::vector<uint8_t> desktop = header_first_container(true);
  EXPECT(rebuild_desktop_container(desktop.data(), desktop.size(), out) == ShaderFormat::Desktop);
}

TEST(native_graphics_description_with_sparse_stage_slots) {
  // exp pos0 v0..v3 done; s_endpgm: a vertex program.
  const uint32_t vs_code[] = {0xF80008CFu, 0x03020100u, 0xBF810000u};
  uint32_t meta[12] = {};
  uint32_t state[0x17C / 4] = {};
  state[0x1E] = 0xF;
  uint32_t pipeline[0xBC / 4] = {};
  pipeline[3] = 1;   // one target ...
  pipeline[4] = 0;   // ... of format 0: not written
  pipeline[7] = 0xFFFFFFFFu;
  pipeline[9] = 0x00130100u;  // triangles, with the flag in bit 4 of byte 2
  uint64_t desc[17] = {};
  desc[1] = reinterpret_cast<uint64_t>(state);
  desc[2] = sizeof(state);
  desc[3] = reinterpret_cast<uint64_t>(pipeline);
  desc[4] = sizeof(pipeline);
  // Slot 0 empty, the program in slot 1.
  desc[9] = reinterpret_cast<uint64_t>(vs_code);
  desc[10] = sizeof(vs_code);
  desc[11] = reinterpret_cast<uint64_t>(meta);
  desc[12] = sizeof(meta);
  NativeGraphicsDescription out;
  std::string error;
  EXPECT(native_graphics_description(desc, &out, &error));
  EXPECT(out.vertex_code == reinterpret_cast<const uint8_t*>(vs_code));
  EXPECT(out.pixel_code == nullptr);
  EXPECT(out.render_targets == 0);
  EXPECT(out.target_mask == 0xF);
  EXPECT(out.topology_type == 3);
  // A state block shorter than the dwords read is refused.
  desc[2] = 0xEC;
  EXPECT(!native_graphics_description(desc, &out, &error));
}
