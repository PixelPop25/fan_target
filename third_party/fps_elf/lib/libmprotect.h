/* libmprotect.h — shim for fan_target build environment
 *
 * The fps_elf code originated from etaHEN's monorepo where this header lived
 * under a sibling `lib/` directory alongside the daemon.  When building
 * standalone inside fan_target we redirect to the PS5 Payload SDK's own
 * kernel helpers which provide an equivalent `kernel_mprotect` call.
 *
 * Usage in Detour.cpp:
 *   kernel_mprotect(pid, address, size, prot);
 * Falls back silently when sceKernelMprotect succeeds (no kernel helper
 * needed); otherwise the SDK's ps5/kernel.h function is used.
 */
#pragma once

#include <sys/types.h>
#include <sys/mman.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ps5-payload-sdk exposes this symbol – it performs a kernel-assisted
 * mprotect on another process (or the current one when pid == getpid()).
 * Prototype matches the SDK header; we declare it here so Detour.cpp
 * compiles without pulling in the full daemon include tree.               */
int kernel_mprotect(pid_t pid, uint64_t addr, uint64_t len, int prot);

#ifdef __cplusplus
}
#endif
