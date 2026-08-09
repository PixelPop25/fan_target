/* libmprotect.h — shim for fan_target build environment
 *
 * Redirects to the PS5 Payload SDK's own kernel helpers.
 */
#pragma once

#include <ps5/kernel.h>

/* SDK already provides:
 * int kernel_mprotect(pid_t pid, intptr_t addr, size_t size, int prot);
 */
