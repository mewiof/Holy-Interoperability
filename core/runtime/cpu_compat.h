// Instructions of the console CPU that the host CPU may lack.
//
// The console's processor implements a few vendor-specific instructions
// (documented in the vendor's public programmer's manual) that some desktop
// processors do not; executing one there raises an illegal-instruction
// exception. A vectored handler recognises them at the faulting address,
// carries out their documented effect on the thread's context and memory,
// and resumes after the instruction. Title code is not modified.
//
// Handled: CLZERO (0F 01 FC), which zeroes the naturally aligned 64-byte
// cache line containing the address in rAX (eAX with an address-size
// prefix); MONITORX (0F 01 FA), which arms an address monitor and is carried
// out as nothing; MWAITX (0F 01 FB), a wait the manual allows to end early
// for any reason, carried out as yielding the processor. Anything else stays
// an illegal instruction.
#pragma once

namespace holy::rt {

void install_cpu_compatibility();

}  // namespace holy::rt
