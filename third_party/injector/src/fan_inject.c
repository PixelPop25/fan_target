/* Bridge: prefer ptrace inject_elf when linked; fallback returns false */
#include "fan_inject.h"
#include <string.h>
#include <stdio.h>

__attribute__((weak)) int sceKernelGetProcessName(int pid, char *name);
__attribute__((weak)) int sceSystemServiceGetAppIdOfRunningBigApp(void);

pid_t fan_get_shellui_pid(void) {
  if (!sceKernelGetProcessName)
    return -1;
  char name[256];
  for (int pid = 1; pid <= 9999; ++pid) {
    memset(name, 0, sizeof(name));
    if (sceKernelGetProcessName(pid, name) != 0)
      continue;
    if (strcmp(name, "SceShellUI") == 0)
      return (pid_t)pid;
  }
  return -1;
}

pid_t fan_get_game_pid(void) {
  /* Optional: map app id → pid via sceKernelGetAppInfo when available */
  (void)sceSystemServiceGetAppIdOfRunningBigApp;
  return -1;
}

/* Weak stub — real inject_elf from injector.c overrides when fully linked */
__attribute__((weak)) int inject_elf(void *proc, void *elf);

bool fan_inject_elf(pid_t pid, const uint8_t *elf, uint32_t elf_len) {
  (void)elf_len;
  if (pid <= 0 || !elf)
    return false;
  /* Full build links libNineS inject_elf(struct proc*, void* elf).
   * Until proc* is constructed from pid, callers also use elfldr socket. */
  return false;
}
