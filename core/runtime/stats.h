// Process-wide named counters and the periodic statistics report.
//
// Libraries keep counts of what they hold (live objects, bytes) and of events
// (presents, compiles) in named cells. A runtime thread logs the process's
// memory and every counter, with its change since the previous report, at a
// fixed interval: frame rates, memory growth and leaks are read from the log
// instead of being guessed at.
#pragma once

#include <cstdint>

namespace holy::rt {

// The cell for `name`, created at zero on first use; stable for the life of
// the process. Null only for an empty name.
volatile int64_t* counter(const char* name);

// Starts the report thread (once); `interval_ms` 0 disables it.
void start_stats_reports(uint32_t interval_ms);

// Logs one report now (also used when the process exits).
void log_stats(const char* reason);

}  // namespace holy::rt
