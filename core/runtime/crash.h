// Fault reporting for the title process: logs hardware exceptions with a
// symbol-free stack trace (module + offset) so a failed boot leaves evidence.
#pragma once

#include <windows.h>

namespace holy::rt {

void install_crash_handlers();

// The title's own top-level filter, kept so the runtime's filter can chain to it.
LPTOP_LEVEL_EXCEPTION_FILTER exchange_title_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter);

}  // namespace holy::rt
