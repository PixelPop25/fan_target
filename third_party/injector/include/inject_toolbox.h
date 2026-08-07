#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* etaHEN Inject_Toolbox equivalent: inject ELF bytes into target PID.
 * Returns true on success. Requires HEN/ptrace (pt_attach + elfldr_load). */
bool Inject_Toolbox(int pid, uint8_t *elf);

/* Optional: resolve PID by process name (SceShellUI, etc.) */
int find_pid_by_name(const char *name);

#ifdef __cplusplus
}
#endif
