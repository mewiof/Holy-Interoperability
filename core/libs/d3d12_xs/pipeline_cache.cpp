#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/pipeline_cache.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <iterator>
#include <unordered_set>

#include "core/libs/d3d12_xs/shader/geometry.h"
#include "core/libs/d3d12_xs/shader_dump.h"
#include "core/libs/d3d12_xs/translation_cache.h"
#include "core/libs/support.h"

namespace holy::d3d12 {

// --- counters ---------------------------------------------------------------------------

namespace {

const lib::Counter& c_pipelines() {
  static const lib::Counter c("d3d12.pipelines");
  return c;
}
const lib::Counter& c_host_pipelines() {
  static const lib::Counter c("d3d12.host_pipelines");
  return c;
}
const lib::Counter& c_host_ready() {
  static const lib::Counter c("d3d12.host_pipelines_compiled");
  return c;
}
const lib::Counter& c_compiles() {
  static const lib::Counter c("d3d12.pipeline_compiles");
  return c;
}
const lib::Counter& c_compile_ms() {
  static const lib::Counter c("d3d12.pipeline_compile_ms");
  return c;
}
const lib::Counter& c_compiles_at_use() {
  static const lib::Counter c("d3d12.pipeline_compiles_at_use");
  return c;
}
const lib::Counter& c_wait_ms() {
  static const lib::Counter c("d3d12.pipeline_wait_ms");
  return c;
}
const lib::Counter& c_refused() {
  static const lib::Counter c("d3d12.pipelines_refused");
  return c;
}
const lib::Counter& c_programs() {
  static const lib::Counter c("d3d12.programs");
  return c;
}
const lib::Counter& c_program_bytes() {
  static const lib::Counter c("d3d12.program_bytes_in_memory");
  return c;
}
const lib::Counter& c_program_reloads() {
  static const lib::Counter c("d3d12.program_reloads");
  return c;
}

// Bytes counted while they are held.
std::shared_ptr<const std::vector<uint8_t>> counted_bytes(std::vector<uint8_t> bytes) {
  const auto size = static_cast<int64_t>(bytes.size());
  c_program_bytes().add(size);
  return std::shared_ptr<const std::vector<uint8_t>>(new std::vector<uint8_t>(std::move(bytes)),
                                                     [size](const std::vector<uint8_t>* p) {
                                                       c_program_bytes().add(-size);
                                                       delete p;
                                                     });
}

}  // namespace

// --- programs -----------------------------------------------------------------------------

namespace {

SRWLOCK g_program_lock = SRWLOCK_INIT;  // also guards every Program's weak_
std::unordered_map<uint64_t, std::weak_ptr<const Program>>& programs() {
  static auto* map = new std::unordered_map<uint64_t, std::weak_ptr<const Program>>();
  return *map;
}
size_t g_program_sweep_at = 4096;

}  // namespace

Program::Program(std::shared_ptr<const std::vector<uint8_t>> bytes, uint64_t hash, uint64_t disk_key)
    : hash_(hash), size_(bytes ? bytes->size() : 0), disk_key_(disk_key) {
  if (disk_key_) {
    weak_ = bytes;
  } else {
    strong_ = std::move(bytes);
  }
  c_programs().add(1);
}

Program::~Program() {
  c_programs().add(-1);
}

std::shared_ptr<const std::vector<uint8_t>> Program::bytes() const {
  if (strong_) return strong_;
  AcquireSRWLockShared(&g_program_lock);
  auto held = weak_.lock();
  ReleaseSRWLockShared(&g_program_lock);
  if (held) return held;
  std::vector<uint8_t> loaded;
  if (!load_translation(disk_key_, &loaded, nullptr) || shader_hash(loaded.data(), loaded.size()) != hash_) {
    LIB_LOG(lib::kError, "d3d12", "host program %016llx is no longer in the translation cache",
            static_cast<unsigned long long>(hash_));
    return nullptr;
  }
  c_program_reloads().add(1);
  auto fresh = counted_bytes(std::move(loaded));
  AcquireSRWLockExclusive(&g_program_lock);
  held = weak_.lock();  // another thread may have reloaded it meanwhile
  if (!held) {
    weak_ = fresh;
    held = std::move(fresh);
  }
  ReleaseSRWLockExclusive(&g_program_lock);
  return held;
}

ProgramRef intern_program(std::vector<uint8_t> bytes, uint64_t disk_key) {
  const uint64_t hash = shader_hash(bytes.data(), bytes.size());
  ProgramRef found;
  AcquireSRWLockShared(&g_program_lock);
  {
    auto it = programs().find(hash);
    if (it != programs().end()) found = it->second.lock();
  }
  ReleaseSRWLockShared(&g_program_lock);
  if (found && found->size() == bytes.size()) return found;
  auto made = std::make_shared<const Program>(counted_bytes(std::move(bytes)), hash, disk_key);
  ProgramRef expired;  // dropped outside the lock
  AcquireSRWLockExclusive(&g_program_lock);
  auto& slot = programs()[hash];
  found = slot.lock();
  if (!found) {
    slot = made;
    found = made;
  }
  if (programs().size() >= g_program_sweep_at) {
    for (auto it = programs().begin(); it != programs().end();) {
      it = it->second.expired() ? programs().erase(it) : std::next(it);
    }
    g_program_sweep_at = programs().size() * 2 + 4096;
  }
  ReleaseSRWLockExclusive(&g_program_lock);
  return found;
}

// --- content keys ---------------------------------------------------------------------------

namespace {

struct SubobjectLayout {
  size_t size;
  size_t align;
};

bool layout_of(uint32_t type, SubobjectLayout* out) {
  switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: *out = {sizeof(void*), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: *out = {sizeof(D3D12_SHADER_BYTECODE), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: *out = {sizeof(D3D12_STREAM_OUTPUT_DESC), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: *out = {sizeof(D3D12_BLEND_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: *out = {sizeof(D3D12_RASTERIZER_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: *out = {sizeof(D3D12_DEPTH_STENCIL_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: *out = {sizeof(D3D12_INPUT_LAYOUT_DESC), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: *out = {sizeof(D3D12_RT_FORMAT_ARRAY), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: *out = {sizeof(DXGI_SAMPLE_DESC), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: *out = {sizeof(D3D12_CACHED_PIPELINE_STATE), 8}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: *out = {sizeof(UINT), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: *out = {sizeof(D3D12_DEPTH_STENCIL_DESC1), 4}; return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: *out = {sizeof(D3D12_VIEW_INSTANCING_DESC), 8}; return true;
    default: return false;
  }
}

bool is_shader(uint32_t type) {
  return type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
}

size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// Calls `visit(type, inner offset, size)` for each subobject; false if the
// stream holds a type that is not a desktop one.
template <typename Visit>
bool walk(const std::vector<uint8_t>& stream, Visit visit) {
  for (size_t at = 0; at + 4 <= stream.size();) {
    uint32_t type;
    std::memcpy(&type, stream.data() + at, 4);
    SubobjectLayout layout;
    if (!layout_of(type, &layout)) return false;
    const size_t inner = align_up(at + 4, layout.align);
    if (inner + layout.size > stream.size()) return false;
    visit(type, inner, layout.size);
    at = align_up(inner + layout.size, sizeof(void*));
  }
  return true;
}

std::mutex g_root_lock;
std::unordered_map<const void*, uint64_t>& root_hashes() {
  static auto* map = new std::unordered_map<const void*, uint64_t>();
  return *map;
}

void append(std::vector<uint8_t>* out, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

// The recipe's content with every pointer replaced by what it points at
// (programs and root signatures by content hash): equal for equal pipelines,
// in this run and in later ones.
std::vector<uint8_t> content_of(const Recipe& recipe) {
  std::vector<uint8_t> out;
  out.reserve(recipe.stream.size() + 64);
  const uint32_t translated = recipe.translated ? 1 : 0;
  append(&out, &translated, sizeof(translated));
  size_t program = 0;
  walk(recipe.stream, [&](uint32_t type, size_t inner, size_t size) {
    append(&out, &type, sizeof(type));
    const uint8_t* at = recipe.stream.data() + inner;
    if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
      const void* root = nullptr;
      std::memcpy(&root, at, sizeof(root));
      uint64_t hash = reinterpret_cast<uintptr_t>(root);
      {
        std::lock_guard<std::mutex> guard(g_root_lock);
        auto found = root_hashes().find(root);
        if (found != root_hashes().end()) hash = found->second;
      }
      append(&out, &hash, sizeof(hash));
    } else if (is_shader(type)) {
      D3D12_SHADER_BYTECODE code;
      std::memcpy(&code, at, sizeof(code));
      const uint64_t hash = code.BytecodeLength && program < recipe.programs.size() && recipe.programs[program]
                                ? recipe.programs[program]->hash()
                                : 0;
      if (code.BytecodeLength) ++program;
      append(&out, &hash, sizeof(hash));
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
      D3D12_INPUT_LAYOUT_DESC layout;
      std::memcpy(&layout, at, sizeof(layout));
      append(&out, &layout.NumElements, sizeof(layout.NumElements));
      for (UINT i = 0; i < layout.NumElements && layout.pInputElementDescs; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = layout.pInputElementDescs[i];
        if (e.SemanticName) append(&out, e.SemanticName, std::strlen(e.SemanticName) + 1);
        append(&out, &e.SemanticIndex, sizeof(UINT) * 6);  // index, format, slot, offset, class, step
      }
    } else {
      append(&out, at, size);
    }
  });
  return out;
}

}  // namespace

void register_root_signature_hash(const ID3D12RootSignature* root, uint64_t hash) {
  std::lock_guard<std::mutex> guard(g_root_lock);
  root_hashes()[root] = hash;
}

// --- host pipelines ------------------------------------------------------------------------

enum class Status { Idle, Queued, Compiling, Ready, Failed };

struct HostPipeline {
  Device* device = nullptr;
  std::shared_ptr<const Recipe> recipe;  // null for host objects made elsewhere
  std::vector<uint8_t> content;          // content_of(recipe): tells keys that collide apart
  uint64_t key = 0;
  std::atomic<ID3D12PipelineState*> state{nullptr};
  SRWLOCK lock = SRWLOCK_INIT;
  CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
  Status status = Status::Idle;
  std::atomic<bool> used{false};
  std::wstring name;

  HostPipeline() { c_host_pipelines().add(1); }
  ~HostPipeline() {
    if (ID3D12PipelineState* s = state.load()) {
      s->Release();
      c_host_ready().add(-1);
    }
    c_host_pipelines().add(-1);
  }
};

namespace {

enum class Policy { Warm, Eager, Lazy };

Policy policy() {
  static const Policy value = [] {
    wchar_t text[16] = {};
    if (GetEnvironmentVariableW(L"HOLY_PIPELINES", text, 16) == 0) return Policy::Warm;
    if (_wcsicmp(text, L"eager") == 0) return Policy::Eager;
    if (_wcsicmp(text, L"lazy") == 0) return Policy::Lazy;
    return Policy::Warm;
  }();
  return value;
}

// Content keys of pipelines used in earlier runs, and the file new ones are
// appended to (8 bytes a key after a 16-byte header).
constexpr uint64_t kUsedMagic = 0x4553555045504C48ull;  // "HLPEPUSE"
constexpr uint64_t kUsedVersion = 1;
std::mutex g_used_lock;
std::unordered_set<uint64_t>* g_used = nullptr;
HANDLE g_used_file = INVALID_HANDLE_VALUE;

void load_used_keys() {
  if (g_used) return;
  g_used = new std::unordered_set<uint64_t>();
  const wchar_t* data_dir = HolyDataDir();
  if (!data_dir || !*data_dir) return;
  const std::wstring path = std::wstring(data_dir) + L"\\pipelines-used.bin";
  g_used_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_used_file == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size{};
  GetFileSizeEx(g_used_file, &size);
  uint64_t header[2] = {};
  DWORD read = 0;
  bool valid = size.QuadPart >= 16 && ReadFile(g_used_file, header, sizeof(header), &read, nullptr) &&
               read == sizeof(header) && header[0] == kUsedMagic && header[1] == kUsedVersion;
  if (valid) {
    const auto count = static_cast<size_t>((size.QuadPart - 16) / 8);
    std::vector<uint64_t> keys(count);
    valid = count == 0 || (ReadFile(g_used_file, keys.data(), static_cast<DWORD>(count * 8), &read, nullptr) &&
                           read == count * 8);
    if (valid) g_used->insert(keys.begin(), keys.end());
    // A torn last entry is dropped.
    LARGE_INTEGER end{};
    end.QuadPart = 16 + static_cast<LONGLONG>(count) * 8;
    SetFilePointerEx(g_used_file, end, nullptr, FILE_BEGIN);
    SetEndOfFile(g_used_file);
  }
  if (!valid) {
    g_used->clear();
    SetFilePointer(g_used_file, 0, nullptr, FILE_BEGIN);
    SetEndOfFile(g_used_file);
    const uint64_t fresh[2] = {kUsedMagic, kUsedVersion};
    DWORD written = 0;
    WriteFile(g_used_file, fresh, sizeof(fresh), &written, nullptr);
  }
  LIB_LOG(lib::kInfo, "d3d12", "%zu pipelines used in earlier runs are compiled when created (policy %s)",
          g_used->size(), policy() == Policy::Eager ? "eager" : policy() == Policy::Lazy ? "lazy" : "warm");
}

bool used_before(uint64_t key) {
  std::lock_guard<std::mutex> guard(g_used_lock);
  load_used_keys();
  return g_used->count(key) != 0;
}

void remember_use(uint64_t key) {
  std::lock_guard<std::mutex> guard(g_used_lock);
  load_used_keys();
  if (!g_used->insert(key).second || g_used_file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(g_used_file, &key, sizeof(key), &written, nullptr);
}

std::mutex g_host_lock;
std::unordered_map<uint64_t, std::weak_ptr<HostPipeline>>& host_pipelines() {
  static auto* map = new std::unordered_map<uint64_t, std::weak_ptr<HostPipeline>>();
  return *map;
}
size_t g_host_sweep_at = 4096;

uint64_t fnv(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint8_t b : bytes) hash = (hash ^ b) * 0x100000001b3ull;
  return hash;
}

// Observed host behaviour: compiling several very large translated programs
// at once makes the host driver refuse some with E_OUTOFMEMORY. Programs
// above kHeavyProgram bytes compile one at a time, and a refusal for lack of
// memory is retried once that way.
constexpr size_t kHeavyProgram = 256 * 1024;
std::mutex g_heavy_compile;

HRESULT compile_now(HostPipeline* p, ID3D12PipelineState** out) {
  const Recipe& recipe = *p->recipe;
  std::vector<uint8_t> stream = recipe.stream;
  std::vector<std::shared_ptr<const std::vector<uint8_t>>> held;
  size_t program_bytes = 0, program = 0;
  bool complete = true;
  walk(stream, [&](uint32_t type, size_t inner, size_t) {
    if (!is_shader(type)) return;
    D3D12_SHADER_BYTECODE code;
    std::memcpy(&code, stream.data() + inner, sizeof(code));
    if (!code.BytecodeLength) return;
    auto bytes = program < recipe.programs.size() && recipe.programs[program] ? recipe.programs[program]->bytes()
                                                                               : nullptr;
    ++program;
    if (!bytes) {
      complete = false;
      return;
    }
    code = {bytes->data(), bytes->size()};
    std::memcpy(stream.data() + inner, &code, sizeof(code));
    program_bytes += bytes->size();
    held.push_back(std::move(bytes));
  });
  if (!complete) return E_FAIL;
  ID3D12Device2* device2 = nullptr;
  HRESULT hr = p->device->host->QueryInterface(IID_PPV_ARGS(&device2));
  if (FAILED(hr)) return hr;
  const D3D12_PIPELINE_STATE_STREAM_DESC desc{stream.size(), stream.data()};
  const ULONGLONG start = GetTickCount64();
  if (program_bytes > kHeavyProgram) {
    std::lock_guard<std::mutex> guard(g_heavy_compile);
    hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(out));
  } else {
    hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(out));
    if (hr == E_OUTOFMEMORY) {
      std::lock_guard<std::mutex> guard(g_heavy_compile);
      hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(out));
    }
  }
  device2->Release();
  check_point("pipeline compile");
  c_compiles().add(1);
  c_compile_ms().add(static_cast<int64_t>(GetTickCount64() - start));
  return hr;
}

// Compiles `p`, whose status the caller set to Compiling, and publishes the
// result.
void compile_and_publish(HostPipeline* p) {
  ID3D12PipelineState* state = nullptr;
  const HRESULT hr = compile_now(p, &state);
  if (FAILED(hr) || !state) {
    discard_host_caches_after_run(hr);
    c_refused().add(1);
    LIB_LOG(lib::kError, "d3d12", "the host refused pipeline %016llx (0x%08lX)", static_cast<unsigned long long>(p->key),
            static_cast<unsigned long>(hr));
    if (state) state->Release();
    state = nullptr;
  }
  AcquireSRWLockExclusive(&p->lock);
  if (state) {
    if (!p->name.empty()) state->SetName(p->name.c_str());
    p->state.store(state);
    c_host_ready().add(1);
  }
  p->status = state ? Status::Ready : Status::Failed;
  ReleaseSRWLockExclusive(&p->lock);
  WakeAllConditionVariable(&p->changed);
}

void CALLBACK compile_queued(PTP_CALLBACK_INSTANCE, void* context) {
  auto* job = static_cast<std::weak_ptr<HostPipeline>*>(context);
  std::shared_ptr<HostPipeline> p = job->lock();
  delete job;
  if (!p) return;  // released before its turn
  AcquireSRWLockExclusive(&p->lock);
  const bool mine = p->status == Status::Queued;
  if (mine) p->status = Status::Compiling;
  ReleaseSRWLockExclusive(&p->lock);
  if (mine) compile_and_publish(p.get());
}

PTP_CALLBACK_ENVIRON compile_pool() {
  static PTP_CALLBACK_ENVIRON environment = [] {
    // A third of the host's processors: compiles must not take the
    // processor from the title's own work.
    auto* env = new TP_CALLBACK_ENVIRON();
    InitializeThreadpoolEnvironment(env);
    PTP_POOL pool = CreateThreadpool(nullptr);
    if (!pool) return static_cast<PTP_CALLBACK_ENVIRON>(nullptr);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const DWORD threads = system.dwNumberOfProcessors / 3 > 3 ? system.dwNumberOfProcessors / 3 : 3;
    SetThreadpoolThreadMaximum(pool, threads);
    SetThreadpoolThreadMinimum(pool, 1);
    SetThreadpoolCallbackPool(env, pool);
    SetThreadpoolCallbackPriority(env, TP_CALLBACK_PRIORITY_LOW);
    return env;
  }();
  return environment;
}

void queue_compile(const std::shared_ptr<HostPipeline>& p) {
  AcquireSRWLockExclusive(&p->lock);
  const bool queue = p->status == Status::Idle;
  if (queue) p->status = Status::Queued;
  ReleaseSRWLockExclusive(&p->lock);
  if (!queue) return;
  auto* job = new std::weak_ptr<HostPipeline>(p);
  if (!compile_pool() || !TrySubmitThreadpoolCallback(&compile_queued, job, compile_pool())) {
    compile_queued(nullptr, job);
  }
}

// The shared host pipeline for a recipe's content.
std::shared_ptr<HostPipeline> host_pipeline_for(Device* device, const std::shared_ptr<const Recipe>& recipe,
                                                std::vector<uint8_t> content) {
  std::shared_ptr<HostPipeline> found;
  std::lock_guard<std::mutex> guard(g_host_lock);
  auto& map = host_pipelines();
  auto it = map.find(recipe->key);
  if (it != map.end()) found = it->second.lock();
  if (found && found->content == content) return found;
  if (found) {
    LIB_LOG(lib::kWarn, "d3d12", "pipeline content key %016llx collides; the pipelines are kept apart",
            static_cast<unsigned long long>(recipe->key));
  }
  auto made = std::make_shared<HostPipeline>();
  made->device = device;
  made->recipe = recipe;
  made->content = std::move(content);
  made->key = recipe->key;
  if (!found) map[recipe->key] = made;
  if (map.size() >= g_host_sweep_at) {
    for (auto i = map.begin(); i != map.end();) i = i->second.expired() ? map.erase(i) : std::next(i);
    g_host_sweep_at = map.size() * 2 + 4096;
  }
  return made;
}

std::atomic<void* const*> g_pipeline_vtable{nullptr};

Pipeline* new_pipeline(const lib::VtableInfo* info, void* const* vtable, bool translated) {
  g_pipeline_vtable.store(vtable);
  auto* pipeline = new Pipeline();
  pipeline->vtable = vtable;
  pipeline->info = info;
  pipeline->host = nullptr;  // host objects are reached through pipeline_host
  if (translated) pipeline->flags = kTranslatedPipeline;
  c_pipelines().add(1);
  return pipeline;
}

}  // namespace

Pipeline* as_pipeline(const void* object) {
  if (!object) return nullptr;
  void* const* vtable = g_pipeline_vtable.load();
  return vtable && static_cast<const Wrapper*>(object)->vtable == vtable
             ? const_cast<Pipeline*>(static_cast<const Pipeline*>(object))
             : nullptr;
}

void stream_append(std::vector<uint8_t>* stream, uint32_t type, const void* data, size_t size) {
  SubobjectLayout layout{size, 4};
  layout_of(type, &layout);
  stream->resize(align_up(stream->size(), sizeof(void*)), 0);
  const size_t at = stream->size();
  const size_t inner = align_up(at + 4, layout.align);
  stream->resize(align_up(inner + layout.size, sizeof(void*)), 0);
  std::memcpy(stream->data() + at, &type, 4);
  std::memcpy(stream->data() + inner, data, size < layout.size ? size : layout.size);
}

namespace {

// Takes copies of what the stream points at in title memory (the input
// layout) and clears its program pointers (programs are patched in when
// compiling); false if it points at title memory the recipe cannot copy
// (stream output, view instancing).
bool own_pointers(Recipe* recipe) {
  bool ok = true;
  recipe->elements = std::make_shared<std::vector<D3D12_INPUT_ELEMENT_DESC>>();
  recipe->names = std::make_shared<std::vector<std::string>>();
  walk(recipe->stream, [&](uint32_t type, size_t inner, size_t) {
    uint8_t* at = recipe->stream.data() + inner;
    if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
      D3D12_INPUT_LAYOUT_DESC layout;
      std::memcpy(&layout, at, sizeof(layout));
      if (!layout.NumElements || !layout.pInputElementDescs) return;
      recipe->names->reserve(layout.NumElements);
      for (UINT i = 0; i < layout.NumElements; ++i) {
        recipe->names->push_back(layout.pInputElementDescs[i].SemanticName ? layout.pInputElementDescs[i].SemanticName
                                                                           : "");
      }
      for (UINT i = 0; i < layout.NumElements; ++i) {
        D3D12_INPUT_ELEMENT_DESC e = layout.pInputElementDescs[i];
        e.SemanticName = (*recipe->names)[i].c_str();
        recipe->elements->push_back(e);
      }
      layout.pInputElementDescs = recipe->elements->data();
      std::memcpy(at, &layout, sizeof(layout));
    } else if (is_shader(type)) {
      D3D12_SHADER_BYTECODE code;
      std::memcpy(&code, at, sizeof(code));
      code.pShaderBytecode = nullptr;  // patched in from the recipe's programs
      std::memcpy(at, &code, sizeof(code));
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) {
      D3D12_STREAM_OUTPUT_DESC so;
      std::memcpy(&so, at, sizeof(so));
      if (so.NumEntries) ok = false;
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
      D3D12_VIEW_INSTANCING_DESC vi;
      std::memcpy(&vi, at, sizeof(vi));
      if (vi.ViewInstanceCount) ok = false;
    }
  });
  return ok;
}

HRESULT create_from_recipe(Device* device, std::shared_ptr<Recipe> recipe, const lib::VtableInfo* info,
                           void* const* vtable, void** out) {
  std::vector<uint8_t> content = content_of(*recipe);
  recipe->key = fnv(content);
  std::shared_ptr<const Recipe> fixed = std::move(recipe);
  std::shared_ptr<HostPipeline> shared = host_pipeline_for(device, fixed, std::move(content));
  Pipeline* pipeline = new_pipeline(info, vtable, fixed->translated);
  pipeline->recipe = fixed;
  pipeline->shared = shared;
  if (policy() == Policy::Eager || (policy() == Policy::Warm && used_before(shared->key))) queue_compile(shared);
  *out = pipeline;
  return S_OK;
}

}  // namespace

HRESULT create_recorded_pipeline(Device* device, std::vector<uint8_t> stream, std::vector<ProgramRef> programs,
                                 bool translated, const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                 uint32_t depth_control) {
  if (!pipeline) return E_POINTER;
  *pipeline = nullptr;
  auto recipe = std::make_shared<Recipe>();
  recipe->stream = std::move(stream);
  recipe->programs = std::move(programs);
  recipe->translated = translated;
  recipe->depth_control = depth_control;
  for (const ProgramRef& program : recipe->programs) {
    if (!program) return E_INVALIDARG;
  }
  if (!own_pointers(recipe.get())) {
    // Not derivable (it points at title memory): compiled now, from the
    // title's own description.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> held;
    std::vector<uint8_t> host_stream = recipe->stream;
    size_t program = 0;
    walk(host_stream, [&](uint32_t type, size_t inner, size_t) {
      if (!is_shader(type)) return;
      D3D12_SHADER_BYTECODE code;
      std::memcpy(&code, host_stream.data() + inner, sizeof(code));
      if (!code.BytecodeLength || program >= recipe->programs.size()) return;
      held.push_back(recipe->programs[program++]->bytes());
      if (held.back()) code = {held.back()->data(), held.back()->size()};
      std::memcpy(host_stream.data() + inner, &code, sizeof(code));
    });
    ID3D12Device2* device2 = nullptr;
    if (FAILED(device->host->QueryInterface(IID_PPV_ARGS(&device2)))) return E_NOINTERFACE;
    const D3D12_PIPELINE_STATE_STREAM_DESC host_desc{host_stream.size(), host_stream.data()};
    ID3D12PipelineState* state = nullptr;
    const HRESULT hr = device2->CreatePipelineState(&host_desc, IID_PPV_ARGS(&state));
    device2->Release();
    check_point("native pipeline compile");
    if (FAILED(hr)) return hr;
    *pipeline = wrap_host_pipeline(state, translated, info, vtable);
    return S_OK;
  }
  return create_from_recipe(device, std::move(recipe), info, vtable, pipeline);
}

Pipeline* wrap_host_pipeline(ID3D12PipelineState* state, bool translated, const lib::VtableInfo* info,
                             void* const* vtable) {
  auto shared = std::make_shared<HostPipeline>();
  shared->state.store(state);
  shared->status = Status::Ready;
  shared->used.store(true);  // not keyed: nothing to remember
  c_host_ready().add(1);
  Pipeline* pipeline = new_pipeline(info, vtable, translated);
  pipeline->shared = std::move(shared);
  return pipeline;
}

Pipeline* placeholder_pipeline(const lib::VtableInfo* info, void* const* vtable) {
  static const lib::Counter placeholders("d3d12.placeholder_pipelines");
  placeholders.add(1);
  auto shared = std::make_shared<HostPipeline>();
  shared->status = Status::Failed;
  shared->used.store(true);
  Pipeline* pipeline = new_pipeline(info, vtable, false);
  pipeline->shared = std::move(shared);
  return pipeline;
}

// Observed: device slot 55 takes (base pipeline, count, entries, interface
// id, out) and returns an HRESULT. Entries are 0x14C bytes: a type dword and
// a desktop description: 0 D3D12_DEPTH_STENCIL_DESC, 1 D3D12_RASTERIZER_DESC,
// 2 D3D12_BLEND_DESC (field for field from how titles fill them). Type 5 is
// written next to a depth-stencil entry with zero depth fields and the same
// stencil masks; its meaning is not established and it is not applied.
// The result is the base pipeline with those parts replaced.
HRESULT create_derived_pipeline(Device* device, const void* base, uint32_t count, const uint8_t* entries,
                                const lib::VtableInfo* info, void* const* vtable, void** out, const void* caller) {
  if (!out) return E_POINTER;
  *out = nullptr;
  const Pipeline* from = as_pipeline(base);
  if (!from || !from->recipe) {
    LIB_LOG(lib::kError, "d3d12", "derived pipeline from %p, whose making was not recorded", base);
    HolyUnimplementedFrom(kLibrary, "pipeline derived from an unrecorded pipeline", caller);
    return E_NOTIMPL;
  }
  auto recipe = std::make_shared<Recipe>(*from->recipe);
  constexpr size_t kEntryBytes = 0x14C;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = entries + i * kEntryBytes;
    uint32_t kind;
    std::memcpy(&kind, entry, 4);
    uint32_t types[2] = {0, 0};
    size_t size = 0;
    switch (kind) {
      case 0:
        types[0] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
        types[1] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
        size = sizeof(D3D12_DEPTH_STENCIL_DESC);
        break;
      case 1:
        types[0] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        size = sizeof(D3D12_RASTERIZER_DESC);
        break;
      case 2:
        types[0] = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
        size = sizeof(D3D12_BLEND_DESC);
        break;
      default:
        LIB_LOG(lib::kDebug, "d3d12", "derived pipeline entry of type %u not applied", kind);
        continue;
    }
    bool replaced = false;
    walk(recipe->stream, [&](uint32_t type, size_t inner, size_t) {
      if (type == types[0] || (types[1] && type == types[1])) {
        std::memcpy(recipe->stream.data() + inner, entry + 4, size);
        replaced = true;
      }
    });
    if (!replaced) stream_append(&recipe->stream, types[0], entry + 4, size);
  }
  const HRESULT hr = create_from_recipe(device, std::move(recipe), info, vtable, out);
  if (SUCCEEDED(hr)) {
    // A derived NGG pipeline draws through the same compute pass.
    Pipeline* made = static_cast<Pipeline*>(*out);
    if (from->ngg_compute) {
      made->ngg_compute = from->ngg_compute;
      made->ngg_compute->refs.fetch_add(1);
      made->ngg = from->ngg;
    }
  }
  LIB_LOG(FAILED(hr) ? lib::kError : lib::kDebug, "d3d12", "derived pipeline from %p with %u entries -> 0x%08lX",
          base, count, static_cast<unsigned long>(hr));
  return hr;
}

namespace {

// A host pipeline's object, compiled now if it was not compiled yet (and
// waiting for a compile in progress). Marks it as used.
ID3D12PipelineState* host_state(HostPipeline* p) {
  if (!p->used.load(std::memory_order_relaxed) && !p->used.exchange(true) && p->recipe) remember_use(p->key);
  if (ID3D12PipelineState* state = p->state.load(std::memory_order_acquire)) return state;
  AcquireSRWLockExclusive(&p->lock);
  if (p->status == Status::Idle || p->status == Status::Queued) {
    // Not compiled yet: compiled here (a queued compile finds it taken).
    p->status = Status::Compiling;
    ReleaseSRWLockExclusive(&p->lock);
    c_compiles_at_use().add(1);
    const ULONGLONG start = GetTickCount64();
    compile_and_publish(p);
    c_wait_ms().add(static_cast<int64_t>(GetTickCount64() - start));
    return p->state.load();
  }
  if (p->status == Status::Compiling) {
    const ULONGLONG start = GetTickCount64();
    while (p->status == Status::Compiling) SleepConditionVariableSRW(&p->changed, &p->lock, INFINITE, 0);
    c_wait_ms().add(static_cast<int64_t>(GetTickCount64() - start));
  }
  ReleaseSRWLockExclusive(&p->lock);
  return p->state.load();
}

std::mutex g_variant_lock;

}  // namespace

ID3D12PipelineState* pipeline_host(const void* object) {
  Pipeline* pipeline = as_pipeline(object);
  if (!pipeline || !pipeline->shared) return nullptr;
  return host_state(pipeline->shared.get());
}

ID3D12PipelineState* pipeline_host_rectangles(const void* object) {
  Pipeline* pipeline = as_pipeline(object);
  if (!pipeline || !pipeline->recipe || !pipeline->shared) return nullptr;
  std::shared_ptr<HostPipeline> variant;
  {
    std::lock_guard<std::mutex> guard(g_variant_lock);
    if (!pipeline->rectangles_tried) {
      pipeline->rectangles_tried = true;
      // The vertex program's outputs, from its bytecode.
      const Recipe& base = *pipeline->recipe;
      ProgramRef vertex;
      size_t program = 0;
      bool has_geometry = false;
      walk(base.stream, [&](uint32_t type, size_t inner, size_t) {
        if (!is_shader(type)) return;
        D3D12_SHADER_BYTECODE code;
        std::memcpy(&code, base.stream.data() + inner, sizeof(code));
        if (!code.BytecodeLength) return;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && program < base.programs.size()) {
          vertex = base.programs[program];
        }
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS) has_geometry = true;
        ++program;
      });
      std::vector<SignatureEntry> outputs;
      const auto bytes = vertex ? vertex->bytes() : nullptr;
      if (!has_geometry && bytes && read_output_signature(bytes->data(), bytes->size(), &outputs) && !outputs.empty()) {
        ProgramRef geometry = intern_program(rectangle_list_program(outputs));
        auto recipe = std::make_shared<Recipe>(base);
        const D3D12_SHADER_BYTECODE code{nullptr, geometry->size()};  // patched in when compiling
        stream_append(&recipe->stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, &code, sizeof(code));
        recipe->programs.push_back(std::move(geometry));
        std::vector<uint8_t> content = content_of(*recipe);
        recipe->key = fnv(content);
        pipeline->rectangles = host_pipeline_for(pipeline->shared->device, recipe, std::move(content));
      } else {
        LIB_LOG(lib::kWarn, "d3d12", "a pipeline drawing rectangle lists has no vertex outputs to complete them from");
      }
    }
    variant = pipeline->rectangles;
  }
  return variant ? host_state(variant.get()) : nullptr;
}

ID3D12PipelineState* pipeline_host_depth(const void* object, DXGI_FORMAT format) {
  Pipeline* pipeline = as_pipeline(object);
  if (!pipeline || !pipeline->recipe || !pipeline->shared || format == DXGI_FORMAT_UNKNOWN) return nullptr;
  const uint32_t control = pipeline->recipe->depth_control;
  const bool test = (control >> 1) & 1, write = (control >> 2) & 1;
  if (!test && !write) return nullptr;
  std::shared_ptr<HostPipeline> variant;
  {
    std::lock_guard<std::mutex> guard(g_variant_lock);
    for (const auto& [made_for, made] : pipeline->depth_variants) {
      if (made_for == format) return made ? host_state(made.get()) : nullptr;
    }
    // DB_DEPTH_CONTROL: Z_ENABLE bit 1, Z_WRITE_ENABLE bit 2, ZFUNC bits 4-6
    // (never, less, equal, less or equal, greater, not equal, greater or
    // equal, always: the desktop's comparison functions less one).
    // Partial: the stencil test (STENCIL_ENABLE bit 0) is not applied (the
    // registers of its operations and masks are not established).
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;  // writes need the test on on the host (always passing when only writing)
    depth.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = test ? static_cast<D3D12_COMPARISON_FUNC>(((control >> 4) & 7) + 1) : D3D12_COMPARISON_FUNC_ALWAYS;
    depth.StencilReadMask = depth.StencilWriteMask = 0xFF;
    const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          D3D12_COMPARISON_FUNC_ALWAYS};
    depth.FrontFace = depth.BackFace = keep;
    auto recipe = std::make_shared<Recipe>(*pipeline->recipe);
    bool has_depth = false, has_format = false;
    walk(recipe->stream, [&](uint32_t type, size_t inner, size_t size) {
      if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL && size >= sizeof(depth)) {
        std::memcpy(recipe->stream.data() + inner, &depth, sizeof(depth));
        has_depth = true;
      } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT && size >= sizeof(format)) {
        std::memcpy(recipe->stream.data() + inner, &format, sizeof(format));
        has_format = true;
      }
    });
    if (!has_depth) stream_append(&recipe->stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, &depth, sizeof(depth));
    if (!has_format) {
      stream_append(&recipe->stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &format, sizeof(format));
    }
    std::vector<uint8_t> content = content_of(*recipe);
    recipe->key = fnv(content);
    variant = host_pipeline_for(pipeline->shared->device, recipe, std::move(content));
    pipeline->depth_variants.emplace_back(format, variant);
  }
  return variant ? host_state(variant.get()) : nullptr;
}

ULONG STDMETHODCALLTYPE pipeline_release(Wrapper* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    auto* pipeline = static_cast<Pipeline*>(self);
    if (pipeline->ngg_compute) pipeline_release(pipeline->ngg_compute);
    delete pipeline;
    c_pipelines().add(-1);
  }
  return static_cast<ULONG>(left);
}

HRESULT pipeline_set_name(const void* object, const wchar_t* name) {
  Pipeline* pipeline = as_pipeline(object);
  if (!pipeline || !pipeline->shared) return E_INVALIDARG;
  HostPipeline* p = pipeline->shared.get();
  AcquireSRWLockExclusive(&p->lock);
  p->name = name ? name : L"";
  ID3D12PipelineState* state = p->state.load();
  if (state) state->SetName(p->name.c_str());
  ReleaseSRWLockExclusive(&p->lock);
  return S_OK;
}

std::wstring pipeline_label(const void* object) {
  const Pipeline* pipeline = as_pipeline(object);
  if (!pipeline || !pipeline->recipe) return std::wstring();
  std::wstring label = L"host";
  for (const ProgramRef& program : pipeline->recipe->programs) {
    wchar_t text[24];
    std::swprintf(text, 24, L" %016llx", static_cast<unsigned long long>(program->hash()));
    label += text;
  }
  return label;
}

}  // namespace holy::d3d12
