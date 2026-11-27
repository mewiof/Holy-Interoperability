// A sampling profiler for the title process, run from the launcher.
//
// It lives outside the title process so that sampling can never deadlock
// with, or be perturbed by, what the title's threads hold: at a fixed
// interval it suspends each thread of the title, walks its stack with the
// system's debugging library, and resumes it. Samples are split into busy
// ones (the thread runs user code) and blocked ones (the thread sits in a
// system call: waits, sleeps, file I/O), and aggregated by function:
//
// - where busy threads spend their time (exclusive and inclusive),
// - where blocked threads wait from (the first frame outside system DLLs),
// - each thread's processor time.
//
// The report goes to <data>/logs/profile.txt, rewritten every 30 seconds and
// when profiling stops. The title executable is called "title" in it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace holy::loader {

class Profiler {
 public:
  // Starts sampling `process` every `interval_ms` (at least 1).
  Profiler(HANDLE process, uint32_t interval_ms, std::wstring report_path, std::wstring exe_path);
  ~Profiler();  // stops sampling and writes the final report

  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;

  struct State;

 private:
  std::unique_ptr<State> state_;
};

}  // namespace holy::loader
