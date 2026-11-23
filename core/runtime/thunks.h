// Small generated entry points placed in front of imports.
//
// A reporting thunk records the call (library, symbol, the four register
// arguments and the caller) and then continues at the real target with all
// argument registers intact. For an import that has no implementation the
// thunk is the implementation: it logs what the title asked for and applies
// the unimplemented-import policy.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace holy::rt {

struct ImportRecord {
  std::string library;
  std::string symbol;
  void* target = nullptr;  // nullptr: no implementation exists
  std::atomic<uint64_t> calls{0};
};

// Returns an executable entry point for `record`, which must stay alive for
// the rest of the process. Returns nullptr if executable memory is unavailable.
void* make_reporting_thunk(ImportRecord* record);

}  // namespace holy::rt
