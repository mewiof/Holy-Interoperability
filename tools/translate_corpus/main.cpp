// holy_translate_corpus: translates every shader in a directory (files *.bin:
// console shader containers, or native code as pipelines created from native
// code pass it; the runtime saves such files in a title's data directory,
// shaders/) the way the runtime does, and reports what translates, what does
// not and why, and how fast. Native code that shows no graphics exports is
// translated as a compute program of 64 threads (pipelines from native code
// give the size apart; it is not saved), so blocks that are not code show as
// failures of raw compute programs. It reads the directory only; with
// --report it writes one line per file to the given path. --extract writes
// one shader's native code (for a disassembler) to the given path; --decode
// lists its instructions as the translator decodes them (operand codes as in
// shader/rdna.h).
//
//   holy_translate_corpus <directory> [--threads N] [--report <file>]
//   holy_translate_corpus --extract <shader file> <code file>
//   holy_translate_corpus --decode <shader file>
#include <windows.h>

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
};

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
void translate_one(const std::vector<uint8_t>& blob, Result* r) {
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

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && !std::wcscmp(argv[1], L"--extract")) return extract(argv[2], argv[3]);
  if (argc == 3 && !std::wcscmp(argv[1], L"--decode")) return decode_listing(argv[2]);
  if (argc < 2) {
    std::fprintf(stderr, "usage: holy_translate_corpus <directory> [--threads N] [--report <file>]\n");
    return 2;
  }
  const std::wstring dir = argv[1];
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::wstring report;
  for (int i = 2; i + 1 < argc; ++i) {
    if (!std::wcscmp(argv[i], L"--threads")) threads = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
    else if (!std::wcscmp(argv[i], L"--report")) report = argv[++i];
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
      std::vector<uint8_t> blob;
      for (size_t i; (i = next.fetch_add(1)) < results.size();) {
        Result& r = results[i];
        if (!read_file(dir + L"\\" + r.name, &blob)) {
          r.error = "unreadable file";
          continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        translate_one(blob, &r);
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
  if (!report.empty()) {
    FILE* file = _wfopen(report.c_str(), L"w");
    if (file) {
      for (const Result& r : results) {
        std::fprintf(file, "%ls\t%s\t%s\t%.3f\t%s\n", r.name.c_str(), kind_name(r.kind), r.ok ? "ok" : "failed", r.ms,
                     r.error.c_str());
      }
      std::fclose(file);
    }
  }
  return 0;
}
