// Draws with NGG vertex programs (shader/translate.h, translate_ngg).
//
// Observed: titles' native graphics pipelines may carry an NGG vertex
// program: it groups vertex ids (v5 >> k), requests output space for each
// group, shares data through group shared memory and exports primitives.
// Host model: such a pipeline is two host pipelines. The program, translated
// into a compute program, writes the draw's vertex and primitive records into
// scratch memory; the host graphics pipeline draws those primitives as an
// indexed triangle list with a vertex program that reads the records
// (ngg_vertex_program) and the pipeline's pixel program.
#pragma once

#include <d3d12.h>

#include <cstdint>

#include "core/libs/d3d12_xs/shader/translate.h"

namespace holy::d3d12 {

struct CommandList;
struct HostList;

struct NggPipeline {
  void* compute = nullptr;  // the compute pass's pipeline (a Wrapper)
  NggInfo info;
};

// The NGG part of a pipeline (pipeline_cache.h, Pipeline::ngg_compute);
// false if it has none.
bool ngg_pipeline(const void* graphics, NggPipeline* out);

// A draw of `vertices` vertex ids per instance with the list's current NGG
// pipeline. False (logged) when it cannot be drawn.
bool draw_ngg(CommandList* list, const NggPipeline& ngg, uint32_t vertices, uint32_t instances,
              uint32_t first_vertex, uint32_t first_instance);

// Scratch memory a host list used for NGG records goes back to the pool;
// called when the host list is reset, its submissions being complete.
void release_ngg_scratch(HostList* host);

}  // namespace holy::d3d12
