// Conditional hardware watchpoints, a debugging aid (HOLY_WATCH).
//
//   HOLY_WATCH=at=<title offset>;if=<reg>+<offset>:<value>;watch=<reg>+<offset>
//
// When a thread executes the instruction at title+<offset> and the 32-bit
// value at <reg>+<offset> equals <value> (the `if` part is optional), a write
// watch is armed on the 8 bytes at the `watch` address in every thread; each
// write to them is logged with the writing code, its stack and the value
// written. Only the latest armed address is watched. Uses the processor's
// debug registers: nothing in the title is modified.
#pragma once

namespace holy::rt {

// Starts watching if HOLY_WATCH is set (after the title's imports are bound).
void start_watch();

// For new threads (DLL_THREAD_ATTACH): applies the current debug registers.
void watch_thread_started();

}  // namespace holy::rt
