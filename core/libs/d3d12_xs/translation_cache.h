// Translated programs kept on disk in the title's data directory
// (translated/<translator build>/<key>.bin), so that later runs skip
// translating them. Directories of other translator builds are deleted in the
// background when a run first uses the cache.
//
// A key hashes the native code, the parameters it was translated with and
// the translator's build (translator_build): a rebuilt translator misses
// every older entry, so a changed translator never serves stale bytecode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/libs/d3d12_xs/shader/translate.h"

namespace holy::d3d12 {

class TranslationKey {
 public:
  TranslationKey& add(const void* bytes, size_t size);
  TranslationKey& add(uint32_t value) { return add(&value, sizeof(value)); }
  uint64_t value() const;

 private:
  uint64_t hash_ = 0xcbf29ce484222325ull;
};

TranslationKey compute_translation_key(const uint8_t* code, size_t size, const ComputeShaderInfo& info);
TranslationKey graphics_translation_key(const uint8_t* code, size_t size, GraphicsStage stage,
                                       const ShaderInterface* consumer = nullptr);
TranslationKey ngg_translation_key(const uint8_t* code, size_t size);

// The bytecode (and, for graphics stages, the stage interface) stored under
// `key`, or false.
bool load_translation(uint64_t key, std::vector<uint8_t>* bytecode, ShaderInterface* link, NggInfo* ngg = nullptr);
// True once the bytecode is stored (it can be loaded again).
bool store_translation(uint64_t key, const std::vector<uint8_t>& bytecode, const ShaderInterface* link,
                       const NggInfo* ngg = nullptr);

}  // namespace holy::d3d12
