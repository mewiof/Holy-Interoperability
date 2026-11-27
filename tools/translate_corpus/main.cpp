// holy_translate_corpus: translates every shader in a directory (files *.bin:
// console shader containers, or native code as pipelines created from native
// code pass it; the runtime saves such files in a title's data directory,
// shaders/) the way the runtime does, and reports what translates, what does
// not and why, and how fast. Native code that shows no graphics exports is
// translated as a compute program of 64 threads (pipelines from native code
// give the size apart; it is not saved), so blocks that are not code show as
// failures of raw compute programs. It reads the directory only; with
// --report it writes one line per file to the given path (name, kind, result,
// milliseconds, bytes in, bytes out, host compile milliseconds or -1, error). --extract writes
// one shader's native code (for a disassembler) to the given path; --decode
// lists its instructions as the translator decodes them (operand codes as in
// shader/rdna.h); --translate writes one shader's host container. --compile
// also compiles each compute translation into a host pipeline and reports the
// host compiler's time by translation size (each program gets a store of a
// unique value, so the host's shader caches, which key on the code, do not
// answer).
//
//   holy_translate_corpus <directory> [--threads N] [--report <file>] [--compile]
//   holy_translate_corpus --extract <shader file> <code file>
//   holy_translate_corpus --decode <shader file>
//   holy_translate_corpus --translate <shader file> <container file>
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/libs/d3d12_xs/shader/rdna.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/d3d12_xs/shader_container.h"

namespace {

using namespace holy::d3d12;

enum class Kind { NotNative, Compute, Vertex, Ngg, Pixel, RawCompute, RawVertex, RawNgg, RawPixel, Unreadable };

const char* kind_name(Kind kind) {
  switch (kind) {
    case Kind::NotNative: return "desktop or bitcode";
    case Kind::Compute: return "compute";
    case Kind::Vertex: return "vertex";
    case Kind::Ngg: return "NGG vertex";
    case Kind::Pixel: return "pixel";
    case Kind::RawCompute: return "raw compute";
    case Kind::RawVertex: return "raw vertex";
    case Kind::RawNgg: return "raw NGG vertex";
    case Kind::RawPixel: return "raw pixel";
    case Kind::Unreadable: return "unreadable";
  }
  return "?";
}

struct Result {
  std::wstring name;
  Kind kind = Kind::Unreadable;
  bool ok = false;
  std::string error;
  double ms = 0;
  size_t bytes_in = 0, bytes_out = 0;
  double compile_ms = -1;  // --compile: the host compiler's time (-1: not compiled)
  HRESULT compile_result = S_OK;
};

// --compile: a host device and the root signature translated programs bind
// through (the runtime's translated_root_signature, same ranges).
struct Host {
  ID3D12Device* device = nullptr;
  ID3D12RootSignature* root = nullptr;
  bool caches_off = false;
};

bool open_host(Host* host) {
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                               reinterpret_cast<void**>(&host->device)))) {
    return false;
  }
  ID3D12Device9* device9 = nullptr;
  if (SUCCEEDED(host->device->QueryInterface(__uuidof(ID3D12Device9), reinterpret_cast<void**>(&device9)))) {
    host->caches_off = SUCCEEDED(device9->ShaderCacheControl(
        D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED | D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER,
        D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE));
    device9->Release();
  }
  constexpr uint32_t kSlots = kImageSlots;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kMemoryViews, 0, kTranslatedSpace, 0};
  D3D12_DESCRIPTOR_RANGE images[3 * kSlots]{};
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    images[slot] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, kTextureSpace + slot, 0};
    images[kSlots + slot] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, kTextureSpace + slot, 0};
    images[2 * kSlots + slot] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, kIntegerImageSpace + slot, 0};
  }
  D3D12_DESCRIPTOR_RANGE samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, kTextureSpace, 0};
  D3D12_ROOT_PARAMETER parameters[7]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[1].Descriptor = {0, kTranslatedSpace};
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[2].Descriptor = {0, kTranslatedSpace};
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[3].DescriptorTable = {3 * kSlots, images};
  parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[4].DescriptorTable = {1, &samplers};
  parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[5].Descriptor = {0, kNggSpace};
  parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[6].Descriptor = {0, kNggSpace};
  const D3D12_ROOT_SIGNATURE_DESC desc{7, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* blob = nullptr;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
  if (SUCCEEDED(hr)) {
    hr = host->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&host->root));
    blob->Release();
  }
  return SUCCEEDED(hr);
}

// The container with a store of `nonce` before the program's final ret:
// program code the host's shader caches have not seen (they key on the code
// as optimised: a dead instruction would not do), so compile times are of
// compiling even with the caches on.
std::vector<uint8_t> with_nonce(const std::vector<uint8_t>& container, uint64_t nonce) {
  auto read32 = [&](const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
  };
  if (container.size() < 32) return container;
  const uint32_t count = read32(container, 28);
  if (32 + 4ull * count > container.size()) return container;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = read32(container, 32 + 4 * i);
    if (at + 8ull > container.size()) return container;
    const uint32_t fourcc = read32(container, at), size = read32(container, at + 4);
    if (at + 8ull + size > container.size()) return container;
    parts.emplace_back(fourcc, std::vector<uint8_t>(container.begin() + at + 8, container.begin() + at + 8 + size));
  }
  bool changed = false;
  for (auto& [fourcc, data] : parts) {
    if (fourcc != 0x58454853u /* SHEX */ || data.size() < 12) continue;
    const uint32_t ret = 0x0100003Eu;
    if (read32(data, data.size() - 4) != ret) return container;
    // store_raw u0[0].x, l(0), l(nonce): kept by the compiler (a store), and
    // the program is compiled, never run.
    const uint32_t code[8] = {0x080000A6u, 0x0021E012u, 0u, 0u, 0x00004001u, 0u, 0x00004001u,
                              static_cast<uint32_t>(nonce ^ (nonce >> 32))};
    const auto* bytes = reinterpret_cast<const uint8_t*>(code);
    data.insert(data.end() - 4, bytes, bytes + sizeof(code));
    const uint32_t length = read32(data, 4) + 8;
    std::memcpy(data.data() + 4, &length, 4);
    changed = true;
  }
  if (!changed) return container;
  std::vector<uint8_t> out(container.begin(), container.begin() + 32 + 4 * count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 32 + 4 * i, &at, 4);
    const uint32_t header[2] = {parts[i].first, static_cast<uint32_t>(parts[i].second.size())};
    const auto* h = reinterpret_cast<const uint8_t*>(header);
    out.insert(out.end(), h, h + 8);
    out.insert(out.end(), parts[i].second.begin(), parts[i].second.end());
  }
  const uint32_t total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  dxbc_checksum(out.data(), out.size(), out.data() + 4);
  return out;
}

void compile_one(const Host& host, const std::vector<uint8_t>& translated, Result* r) {
  static const uint64_t run = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  static std::atomic<uint64_t> serial{0};
  const std::vector<uint8_t> bytecode = with_nonce(translated, run ^ (serial.fetch_add(1) << 40));
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = host.root;
  desc.CS = {bytecode.data(), bytecode.size()};
  ID3D12PipelineState* pipeline = nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  r->compile_result = host.device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                             reinterpret_cast<void**>(&pipeline));
  r->compile_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (pipeline) pipeline->Release();
}

bool read_file(const std::wstring& path, std::vector<uint8_t>* out) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  out->resize(static_cast<size_t>(size.QuadPart));
  DWORD got = 0;
  const bool ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
  CloseHandle(file);
  return ok;
}

// What the runtime does with a shader passed to pipeline creation.
void translate_one(const std::vector<uint8_t>& blob, Result* r, std::vector<uint8_t>* kept = nullptr) {
  r->bytes_in = blob.size();
  std::vector<uint8_t> rebuilt;
  const ShaderFormat format = rebuild_desktop_container(blob.data(), blob.size(), rebuilt);
  if (format == ShaderFormat::Desktop || format == ShaderFormat::Rebuilt) {
    r->kind = Kind::NotNative;
    r->ok = true;
    return;
  }
  const bool raw = format == ShaderFormat::Unknown;
  NativeComputeProgram program;
  const uint8_t* code = blob.data();
  size_t size = blob.size();
  if (!raw) {
    if (!native_program(blob.data(), blob.size(), &program)) {
      r->kind = Kind::Unreadable;
      r->error = "no native program";
      return;
    }
    code = program.block.data() + program.code_offset;
    size = program.block.size() - program.code_offset;
  }
  std::vector<uint8_t> out;
  GraphicsStage stage;
  if (native_graphics_stage(code, size, &stage)) {
    ShaderInterface link;
    if (stage == GraphicsStage::Vertex && native_ngg_program(code, size)) {
      r->kind = raw ? Kind::RawNgg : Kind::Ngg;
      NggInfo info;
      r->ok = translate_ngg(code, size, &out, &link, &info, &r->error);
    } else {
      r->kind = stage == GraphicsStage::Vertex ? (raw ? Kind::RawVertex : Kind::Vertex)
                                               : (raw ? Kind::RawPixel : Kind::Pixel);
      r->ok = translate_graphics(code, size, stage, &out, &link, &r->error);
    }
  } else {
    r->kind = raw ? Kind::RawCompute : Kind::Compute;
    ComputeShaderInfo info;
    info.threads[0] = 64;
    if (!raw) {
      NativeComputeProgram compute;
      if (!native_compute_program(blob.data(), blob.size(), &compute)) {
        r->error = "no thread group size";
        return;
      }
      for (int axis = 0; axis < 3; ++axis) info.threads[axis] = compute.threads[axis];
    }
    r->ok = translate_compute(code, size, info, &out, &r->error);
  }
  r->bytes_out = out.size();
  if (kept) *kept = std::move(out);
}

// Errors grouped: the part after "at 0x...: " (the instruction's offset).
std::string reason(const std::string& error) {
  std::string text = error;
  if (text.rfind("at 0x", 0) == 0) {
    const size_t colon = text.find(": ");
    if (colon != std::string::npos) text = text.substr(colon + 2);
  }
  return text;
}

}  // namespace

// The native code of a shader file: a container's, or the file as it is.
bool native_code(const std::wstring& in, std::vector<uint8_t>* code) {
  std::vector<uint8_t> blob;
  if (!read_file(in, &blob)) return false;
  std::vector<uint8_t> rebuilt;
  if (rebuild_desktop_container(blob.data(), blob.size(), rebuilt) != ShaderFormat::NativeOnly) {
    *code = std::move(blob);
    return true;
  }
  NativeProgram program;
  if (!native_program(blob.data(), blob.size(), &program)) return false;
  code->assign(program.block.begin() + static_cast<std::ptrdiff_t>(program.code_offset), program.block.end());
  return true;
}

int decode_listing(const std::wstring& in) {
  std::vector<uint8_t> code;
  if (!native_code(in, &code)) return 1;
  rdna::Instruction inst;
  for (uint32_t pc = 0; rdna::decode(code.data(), code.size(), pc, &inst); pc += 4 * inst.words) {
    std::printf("%5x  %-28s dst %3u  src %3u %3u %3u %3u  imm %d  raw %08x %08x\n", pc, rdna::mnemonic(inst).c_str(),
                inst.dst, inst.src[0], inst.src[1], inst.src[2], inst.src[3], inst.imm, inst.raw[0], inst.raw[1]);
    if (inst.encoding == rdna::Encoding::Unknown) break;
  }
  return 0;
}

// Writes the native code of a shader file to `out`.
int extract(const std::wstring& in, const std::wstring& out) {
  std::vector<uint8_t> bytes;
  if (!native_code(in, &bytes)) return 1;
  const uint8_t* code = bytes.data();
  const size_t size = bytes.size();
  FILE* file = _wfopen(out.c_str(), L"wb");
  if (!file) return 1;
  std::fwrite(code, 1, size, file);
  std::fclose(file);
  return 0;
}

// Translates one shader file and writes the host container to `out`.
int translate_file(const std::wstring& in, const std::wstring& out) {
  std::vector<uint8_t> blob, bytecode;
  if (!read_file(in, &blob)) return 1;
  Result r;
  translate_one(blob, &r, &bytecode);
  if (!r.ok) {
    std::fprintf(stderr, "%s: %s\n", kind_name(r.kind), r.error.c_str());
    return 1;
  }
  FILE* file = _wfopen(out.c_str(), L"wb");
  if (!file) return 1;
  std::fwrite(bytecode.data(), 1, bytecode.size(), file);
  std::fclose(file);
  std::printf("%s: %zu bytes in, %zu bytes out\n", kind_name(r.kind), r.bytes_in, r.bytes_out);
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && !std::wcscmp(argv[1], L"--extract")) return extract(argv[2], argv[3]);
  if (argc == 4 && !std::wcscmp(argv[1], L"--translate")) return translate_file(argv[2], argv[3]);
  if (argc == 3 && !std::wcscmp(argv[1], L"--decode")) return decode_listing(argv[2]);
  if (argc < 2) {
    std::fprintf(stderr, "usage: holy_translate_corpus <directory> [--threads N] [--report <file>]\n");
    return 2;
  }
  const std::wstring dir = argv[1];
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::wstring report;
  bool compile = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::wcscmp(argv[i], L"--compile")) compile = true;
    else if (i + 1 < argc && !std::wcscmp(argv[i], L"--threads")) {
      threads = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
    } else if (i + 1 < argc && !std::wcscmp(argv[i], L"--report")) {
      report = argv[++i];
    }
  }
  Host host;
  if (compile && !open_host(&host)) {
    std::fprintf(stderr, "no host device for --compile\n");
    return 1;
  }
  std::vector<Result> results;
  WIN32_FIND_DATAW entry;
  const HANDLE find = FindFirstFileW((dir + L"\\*.bin").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      Result r;
      r.name = entry.cFileName;
      results.push_back(r);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
  }
  if (results.empty()) {
    std::fprintf(stderr, "no *.bin files in the directory\n");
    return 1;
  }
  std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.name < b.name; });
  std::atomic<size_t> next{0};
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  for (unsigned t = 0; t < threads; ++t) {
    workers.emplace_back([&] {
      std::vector<uint8_t> blob, bytecode;
      for (size_t i; (i = next.fetch_add(1)) < results.size();) {
        Result& r = results[i];
        if (!read_file(dir + L"\\" + r.name, &blob)) {
          r.error = "unreadable file";
          continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        translate_one(blob, &r, compile ? &bytecode : nullptr);
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (compile && r.ok && (r.kind == Kind::Compute || r.kind == Kind::RawCompute)) compile_one(host, bytecode, &r);
      }
    });
  }
  for (std::thread& w : workers) w.join();
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::map<Kind, std::pair<size_t, size_t>> by_kind;  // translated, failed
  std::map<std::string, size_t> reasons;
  double total_ms = 0, slowest = 0;
  std::wstring slowest_name;
  size_t in = 0, out = 0;
  for (const Result& r : results) {
    (r.ok ? by_kind[r.kind].first : by_kind[r.kind].second)++;
    if (!r.ok) reasons[std::string(kind_name(r.kind)) + ": " + reason(r.error)]++;
    total_ms += r.ms;
    if (r.ms > slowest) {
      slowest = r.ms;
      slowest_name = r.name;
    }
    in += r.bytes_in;
    out += r.bytes_out;
  }
  std::printf("%zu containers, %u threads: %.2f s wall, %.1f s of translation (%.0f per second per thread), slowest "
              "%.1f ms (%ls)\n",
              results.size(), threads, wall, total_ms / 1000, results.size() / (total_ms / 1000), slowest,
              slowest_name.c_str());
  std::printf("%.1f MiB of containers in, %.1f MiB of host bytecode out\n", in / 1048576.0, out / 1048576.0);
  for (const auto& [kind, counts] : by_kind) {
    std::printf("  %-20s %6zu translated, %5zu failed\n", kind_name(kind), counts.first, counts.second);
  }
  std::vector<std::pair<size_t, std::string>> sorted;
  for (const auto& [text, count] : reasons) sorted.emplace_back(count, text);
  std::sort(sorted.rbegin(), sorted.rend());
  if (!sorted.empty()) std::printf("failures by reason:\n");
  for (const auto& [count, text] : sorted) std::printf("  %6zu  %s\n", count, text.c_str());
  if (compile) {
    // The host compiler's time by translation size.
    static const size_t kLimits[] = {64 << 10, 256 << 10, 1 << 20, 4 << 20, SIZE_MAX};
    static const char* const kNames[] = {"under 64 KiB", "64-256 KiB", "256 KiB-1 MiB", "1-4 MiB", "4 MiB and up"};
    size_t counts[5] = {}, refused = 0;
    double sums[5] = {}, bytes[5] = {}, total = 0;
    for (const Result& r : results) {
      if (r.compile_ms < 0) continue;
      if (FAILED(r.compile_result)) {
        ++refused;
        continue;
      }
      size_t b = 0;
      while (r.bytes_out >= kLimits[b]) ++b;
      ++counts[b];
      sums[b] += r.compile_ms;
      bytes[b] += static_cast<double>(r.bytes_out);
      total += r.compile_ms;
    }
    std::printf("host compiler (shader caches %s; each program made unique): %.1f s over the compute "
                "translations, %zu refused\n",
                host.caches_off ? "off" : "on", total / 1000, refused);
    for (int b = 0; b < 5; ++b) {
      if (!counts[b]) continue;
      std::printf("  %-14s %6zu programs, %8.1f ms each, %6.1f ms per MiB\n", kNames[b], counts[b],
                  sums[b] / counts[b], sums[b] / (bytes[b] / 1048576.0));
    }
  }
  if (!report.empty()) {
    FILE* file = _wfopen(report.c_str(), L"w");
    if (file) {
      for (const Result& r : results) {
        std::fprintf(file, "%ls\t%s\t%s\t%.3f\t%zu\t%zu\t%.3f\t%s\n", r.name.c_str(), kind_name(r.kind),
                     r.ok ? "ok" : "failed", r.ms, r.bytes_in, r.bytes_out, r.compile_ms, r.error.c_str());
      }
      std::fclose(file);
    }
  }
  return 0;
}
