#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* etaHEN Inject_Toolbox equivalent: inject ELF bytes into target PID */
bool fan_inject_elf(pid_t pid, const uint8_t *elf, uint32_t elf_len);

/* Resolve SceShellUI / current big-app game PID */
pid_t fan_get_shellui_pid(void);
pid_t fan_get_game_pid(void);

#ifdef __cplusplus
}
#endif
