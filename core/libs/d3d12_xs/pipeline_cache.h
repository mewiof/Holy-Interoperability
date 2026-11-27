// Pipelines: the title's pipeline objects, the host pipelines behind them,
// and when those are compiled.
//
// Observed: titles create pipelines by the thousand while loading (tens of
// thousands over a session), as creation costs nothing on the console, and
// draw with a small part of them. A host pipeline costs a driver compile and
// driver memory (about a megabyte each was measured for translated ones), so
// the host model is:
//
// - A title pipeline holds a recipe: the host pipeline stream, and the host
//   programs it points at as shared, content-addressed programs. Programs the
//   translation cache holds on disk are not kept in memory between compiles.
// - Title pipelines with the same recipe content share one host pipeline,
//   released with the last of them.
// - A host pipeline is compiled when a title pipeline is first used (bound to
//   a command list, or asked for host state), unless it was used in an earlier
//   run: those (listed in <data>/pipelines-used.bin by content key) compile
//   in the background as soon as the title creates them, so later runs do
//   not wait at first use. HOLY_PIPELINES=eager compiles every pipeline at
//   creation, HOLY_PIPELINES=lazy only at first use.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/shader/translate.h"

namespace holy::d3d12 {

// An immutable host program, shared by every recipe that uses it. Programs
// whose bytes the translation cache can reload (`disk_key` nonzero) drop them
// once no compile needs them.
class Program {
 public:
  uint64_t hash() const { return hash_; }
  size_t size() const { return size_; }
  // The bytes, reloaded from the translation cache when they were dropped;
  // null if they cannot be had.
  std::shared_ptr<const std::vector<uint8_t>> bytes() const;

  // For intern_program and stored_program only.
  Program(std::shared_ptr<const std::vector<uint8_t>> bytes, size_t size, uint64_t hash, uint64_t disk_key);
  ~Program();

 private:
  uint64_t hash_;  // the bytes' hash, or for programs in the translation cache its key's (stored_identity)
  size_t size_;
  uint64_t disk_key_;
  std::shared_ptr<const std::vector<uint8_t>> strong_;  // programs not on disk
  mutable std::weak_ptr<const std::vector<uint8_t>> weak_;
};
using ProgramRef = std::shared_ptr<const Program>;

// The shared program with these bytes. `disk_key` is the translation cache
// key the bytes are stored under (0 if they are not stored): such programs
// are known by the key (which follows from the native code, the
// translation's parameters and the translator's build), so their bytes are
// not hashed, and they are dropped while no compile needs them.
ProgramRef intern_program(std::vector<uint8_t> bytes, uint64_t disk_key = 0);
// The shared program stored in the translation cache under `disk_key`,
// `size` bytes long, read from the cache when first needed.
ProgramRef stored_program(uint64_t disk_key, size_t size);

// How a pipeline is made: a host pipeline stream whose shader subobjects
// point at `programs` (in subobject order; patched in when compiling) and
// whose input layout points at `elements`.
struct Recipe {
  std::vector<uint8_t> stream;
  std::vector<ProgramRef> programs;
  std::shared_ptr<std::vector<D3D12_INPUT_ELEMENT_DESC>> elements;
  std::shared_ptr<std::vector<std::string>> names;
  bool translated = false;
  uint64_t key = 0;  // content key, the same across runs for the same content
  // Native pipelines: their DB_DEPTH_CONTROL. The recipe's own stream has no
  // depth test (no field names the depth format); draws with a depth target
  // bound use a variant with this depth state and the target's format
  // (pipeline_host_depth).
  uint32_t depth_control = 0;
  uint32_t stencil_control = 0;  // DB_STENCIL_CONTROL
  uint32_t stencil_masks = 0;    // DB_STENCILREFMASK
};

struct HostPipeline;

// A title pipeline object (every pipeline the device hands out).
struct Pipeline : Wrapper {
  std::shared_ptr<const Recipe> recipe;  // null for pipelines made from a host object directly
  std::shared_ptr<HostPipeline> shared;
  // NGG pipelines (ngg.h): the compute pass writing the draw's records (a
  // reference held), and what it writes.
  Pipeline* ngg_compute = nullptr;
  NggInfo ngg{};
  // The same pipeline drawing rectangle lists (a geometry program adds each
  // rectangle's fourth corner; shader/geometry.h), made at first such draw.
  std::shared_ptr<HostPipeline> rectangles;
  bool rectangles_tried = false;
  // The same pipeline with its depth state, by depth target format.
  std::vector<std::pair<DXGI_FORMAT, std::shared_ptr<HostPipeline>>> depth_variants;
};

// The pipeline behind a title object, or null if it is not one.
Pipeline* as_pipeline(const void* object);

// Appends a subobject in the desktop stream layout.
void stream_append(std::vector<uint8_t>* stream, uint32_t type, const void* data, size_t size);

// Creates a title pipeline from a host stream whose shader subobjects are
// made from `programs` (in subobject order) and remembers how, so pipelines
// can be derived from it.
HRESULT create_recorded_pipeline(Device* device, std::vector<uint8_t> stream, std::vector<ProgramRef> programs,
                                 bool translated, const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                 uint32_t depth_control = 0, uint32_t stencil_control = 0, uint32_t stencil_masks = 0);

// A title pipeline around a host pipeline made elsewhere (taking over the
// reference); it cannot be derived from.
Pipeline* wrap_host_pipeline(ID3D12PipelineState* state, bool translated, const lib::VtableInfo* info,
                             void* const* vtable);

// A title pipeline with no host pipeline behind it: its draws and dispatches
// are dropped. For exploration only (see device.cpp, `explored`).
Pipeline* placeholder_pipeline(const lib::VtableInfo* info, void* const* vtable);

// Device slot 55: `base` with some of its state replaced.
HRESULT create_derived_pipeline(Device* device, const void* base, uint32_t count, const uint8_t* entries,
                                const lib::VtableInfo* info, void* const* vtable, void** pipeline,
                                const void* caller);

// The host pipeline behind a title pipeline, compiling it now if it was not
// compiled yet (and waiting for a compile in progress). Null if the host
// refused it. Marks the pipeline as used.
ID3D12PipelineState* pipeline_host(const void* pipeline);

// The host pipeline for drawing rectangle lists with a title pipeline (see
// Pipeline::rectangles); null if there cannot be one (the caller then draws
// triangle lists).
ID3D12PipelineState* pipeline_host_rectangles(const void* pipeline);

// The host pipeline for drawing with a title pipeline whose depth state
// tests or writes depth (Recipe::depth_control) into a depth target of
// `format`; null when the base pipeline serves (no depth state, or no target
// bound: the base has the test off).
ID3D12PipelineState* pipeline_host_depth(const void* pipeline, DXGI_FORMAT format);

// Releases a pipeline (vtable slot 2 of pipelines).
ULONG STDMETHODCALLTYPE pipeline_release(Wrapper* self);

// SetName: kept and given to the host pipeline when it exists.
HRESULT pipeline_set_name(const void* pipeline, const wchar_t* name);

// Records the content hash of a host root signature (from its serialized
// form), which pipeline content keys use in place of its address.
void register_root_signature_hash(const ID3D12RootSignature* root, uint64_t hash);

// With debug logging: a label naming a pipeline's host programs by hash
// (translations log which native program each host program comes from), set
// as a host marker before its draws and dispatches so that device-removed
// data can name the work the GPU was doing. Empty when none was recorded.
std::wstring pipeline_label(const void* pipeline);

}  // namespace holy::d3d12
