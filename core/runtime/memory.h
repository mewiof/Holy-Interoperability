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
// yet committed. Returns false if the host refused to commit them. Which
// chunks are committed is remembered; `verify` asks the host again (after a
// kernel call reported an access to the buffer as an error anyway).
bool ensure_committed(const void* address, size_t size, bool verify = false);

// The title decommitted or released [address, address + size): what was
// committed there is forgotten (and released ranges unregistered).
void note_decommit(const void* address, size_t size);
void note_release(const void* address, size_t size);

// A library's hook that backs lazily mapped memory (see HolySetBackingHook);
// ensure_committed calls it first.
// The release hook (HolySetReleaseHook) and calling it.
void set_release_hook(void (*hook)(const void* address, size_t size));
void before_release(const void* address, size_t size);
void set_backing_hook(int (*hook)(const void* address, size_t size));

}  // namespace holy::rt
