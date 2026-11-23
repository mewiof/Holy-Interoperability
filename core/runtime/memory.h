// Commit on demand for title memory.
//
// A console title commits its memory pools up front: several GiB during
// static initialisation, backed by the console's physical memory. On a host
// that is short of commit charge (other processes, a small paging file) the
// same requests fail even though the title touches only part of them for a
// long time. The memory library then reserves the range instead and registers
// it here; the first touch of each 64 KiB chunk commits it from a vectored
// exception handler. Functions that hand title memory to the kernel (file
// reads) commit the buffer first, because the kernel reports an access to
// uncommitted memory as an error instead of raising an exception.
#pragma once

#include <windows.h>

#include <cstddef>

namespace holy::rt {

void install_commit_on_demand();
void register_commit_on_demand(void* base, size_t size, DWORD protect);
// Commits the parts of [address, address + size) that are registered and not
// yet committed. Returns false if the host refused to commit them.
bool ensure_committed(const void* address, size_t size);

}  // namespace holy::rt
