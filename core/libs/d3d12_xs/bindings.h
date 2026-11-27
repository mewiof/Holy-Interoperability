// Root arguments, and how translated pipelines receive them (bindings.cpp).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace holy::d3d12 {

struct CommandList;
struct HostList;

// A title root signature's parameters as the console passes them to native
// code: packed in parameter order, a descriptor table as one dword (its
// 32-bit GPU address), constants as their values, a root view as two dwords
// (its 64-bit address).
struct RootParameterLayout {
  D3D12_ROOT_PARAMETER_TYPE type;
  uint32_t offset;       // in the packed dwords
  uint32_t dwords;
  uint32_t descriptors;  // tables: descriptors in its ranges (unbounded ones counted as 64)
};
struct RootLayout {
  std::vector<RootParameterLayout> parameters;
  uint32_t dwords = 0;
};

void register_root_layout(const void* signature, const D3D12_ROOT_SIGNATURE_DESC1& desc);
const RootLayout* root_layout(const void* signature);

// Per list and pipeline kind (graphics, compute): the title's root signature
// and the arguments set for it.
struct RootBinding {
  const void* signature = nullptr;  // title wrapper
  uint32_t packed[64] = {};
  uint64_t values[64] = {};  // per parameter: table GPU handle or view address (title)
  bool host_is_title = false;  // the host list has the title's root signature bound
};

// Binds what a translated pipeline reads (user SGPRs, region table, images
// and samplers) for the next draw or dispatch. False (logged) if it cannot.
// An NGG draw's compute pass (shader/translate.h, translate_ngg): what it
// adds to the graphics bindings, which it binds on the compute side.
struct NggPass {
  uint32_t index_offset = 0;  // byte offset of the primitive records
  uint32_t groups = 0;        // vertex groups per instance
  uint32_t first_vertex = 0;
  uint32_t first_instance = 0;
  uint64_t records = 0;  // host GPU address of the records
};

bool bind_translated_graphics(CommandList* list, uint32_t base_vertex, uint32_t start_instance,
                              const NggPass* ngg = nullptr);
bool bind_translated_compute(CommandList* list);

// Re-binds the title's root signature and arguments on the host list after a
// translated pipeline replaced them.
void bind_title_root(CommandList* list, bool compute);

// Gives back the upload ring memory a host list's calls used: once the GPU is
// done with the list (it is reset for new commands).
void release_ring_chunks(HostList* host);

}  // namespace holy::d3d12
