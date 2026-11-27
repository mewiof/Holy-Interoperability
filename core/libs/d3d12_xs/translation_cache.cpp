#include "core/libs/d3d12_xs/translation_cache.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>

#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

constexpr uint32_t kMagic = 0x544C5848;  // "HXLT"
constexpr uint32_t kVersion = 3;

struct Header {
  uint32_t magic;
  uint32_t version;
  uint64_t key;
  uint32_t bytecode_size;
  uint32_t has_link;
  uint32_t link[6];  // params, flat_params, targets, position, depth, half_params
  uint32_t ngg[2];   // group, stride
};
static_assert(sizeof(Header) == 56);

// The translator build the translations come from.
uint64_t library_stamp() {
  static const uint64_t stamp = [] {
    const std::string build = translator_build();
    return TranslationKey().add(build.data(), build.size()).value();
  }();
  return stamp;
}

std::wstring directory() {
  const wchar_t* data_dir = HolyDataDir();
  if (!data_dir || !*data_dir) return {};
  return std::wstring(data_dir) + L"\\translated";
}

std::wstring path_of(uint64_t key) {
  wchar_t name[32];
  std::swprintf(name, 32, L"\\%016llx.bin", static_cast<unsigned long long>(key));
  return directory() + name;
}

std::atomic<uint32_t> g_hits{0}, g_misses{0};

}  // namespace

TranslationKey& TranslationKey::add(const void* bytes, size_t size) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) hash_ = (hash_ ^ data[i]) * 0x100000001b3ull;
  return *this;
}

uint64_t TranslationKey::value() const {
  return hash_;
}

TranslationKey compute_translation_key(const uint8_t* code, size_t size, const ComputeShaderInfo& info) {
  TranslationKey key;
  const uint64_t stamp = library_stamp();
  key.add(&stamp, sizeof(stamp)).add(0u).add(code, size);
  for (uint32_t t : info.threads) key.add(t);
  key.add(info.user_sgprs).add(info.wave_size).add(info.lds_bytes).add(info.tg_size_enabled ? 1u : 0u);
  for (bool enabled : info.tgid_enabled) key.add(enabled ? 1u : 0u);
  return key;
}

TranslationKey ngg_translation_key(const uint8_t* code, size_t size) {
  TranslationKey key;
  const uint64_t stamp = library_stamp();
  key.add(&stamp, sizeof(stamp)).add(3u).add(code, size);
  return key;
}

TranslationKey graphics_translation_key(const uint8_t* code, size_t size, GraphicsStage stage,
                                       const ShaderInterface* consumer) {
  TranslationKey key;
  const uint64_t stamp = library_stamp();
  key.add(&stamp, sizeof(stamp)).add(stage == GraphicsStage::Vertex ? 1u : 2u).add(code, size);
  if (consumer) key.add(4u).add(consumer->params).add(consumer->half_params);
  return key;
}

bool load_translation(uint64_t key, std::vector<uint8_t>* bytecode, ShaderInterface* link, NggInfo* ngg) {
  const std::wstring dir = directory();
  if (dir.empty()) return false;
  const HANDLE file =
      CreateFileW(path_of(key).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    g_misses.fetch_add(1);
    return false;
  }
  Header header{};
  DWORD read = 0;
  bool ok = ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header) &&
            header.magic == kMagic && header.version == kVersion && header.key == key &&
            header.bytecode_size > 0 && header.bytecode_size < (64u << 20);
  if (ok) {
    bytecode->resize(header.bytecode_size);
    ok = ReadFile(file, bytecode->data(), header.bytecode_size, &read, nullptr) && read == header.bytecode_size;
  }
  CloseHandle(file);
  if (!ok) {
    g_misses.fetch_add(1);
    return false;
  }
  if (link) {
    *link = ShaderInterface{};
    if (header.has_link) {
      link->params = header.link[0];
      link->flat_params = header.link[1];
      link->targets = header.link[2];
      link->position = header.link[3] != 0;
      link->depth = header.link[4] != 0;
      link->half_params = header.link[5];
    }
  }
  if (ngg) {
    ngg->group = header.ngg[0];
    ngg->stride = header.ngg[1];
  }
  const uint32_t hits = g_hits.fetch_add(1) + 1;
  if (hits == 1 || hits % 1000 == 0) {
    LIB_LOG(lib::kInfo, "d3d12", "translated programs from the cache: %u (%u translated afresh)", hits,
            g_misses.load());
  }
  return true;
}

void store_translation(uint64_t key, const std::vector<uint8_t>& bytecode, const ShaderInterface* link,
                       const NggInfo* ngg) {
  const std::wstring dir = directory();
  if (dir.empty() || bytecode.empty()) return;
  CreateDirectoryW(dir.c_str(), nullptr);
  Header header{kMagic, kVersion, key, static_cast<uint32_t>(bytecode.size()), link ? 1u : 0u, {}, {}};
  if (ngg) {
    header.ngg[0] = ngg->group;
    header.ngg[1] = ngg->stride;
  }
  if (link) {
    header.link[0] = link->params;
    header.link[1] = link->flat_params;
    header.link[2] = link->targets;
    header.link[3] = link->position ? 1u : 0u;
    header.link[4] = link->depth ? 1u : 0u;
    header.link[5] = link->half_params;
  }
  // Written under a temporary name and renamed, so a reader never sees half
  // a file (several threads translate at once).
  const std::wstring final_path = path_of(key);
  const std::wstring temporary = final_path + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
  const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  const bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
                  WriteFile(file, bytecode.data(), static_cast<DWORD>(bytecode.size()), &written, nullptr) &&
                  written == bytecode.size();
  CloseHandle(file);
  if (!ok || !MoveFileExW(temporary.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(temporary.c_str());
  }
}

}  // namespace holy::d3d12
