#include "inject_toolbox.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

__attribute__((weak)) int inject_elf(void *proc, void *elf);
__attribute__((weak)) int sceKernelGetProcessName(int pid, char *name);
__attribute__((weak)) void *get_proc_by_pid(int pid); /* returns struct proc* */

int find_pid_by_name(const char *name) {
  if (!sceKernelGetProcessName || !name)
    return -1;
  char buf[256];
  for (int pid = 1; pid <= 9999; ++pid) {
    memset(buf, 0, sizeof(buf));
    if (sceKernelGetProcessName(pid, buf) != 0)
      continue;
    if (strcmp(buf, name) == 0)
      return pid;
  }
  return -1;
}

bool Inject_Toolbox(int pid, uint8_t *elf) {
  if (pid <= 0 || !elf)
    return false;
  if (!inject_elf)
    return false;

  /* Prefer real kernel proc object (etaHEN get_proc_by_pid) so pid offset is correct */
  void *proc = NULL;
  if (get_proc_by_pid)
    proc = get_proc_by_pid(pid);
  if (!proc) {
    /* Minimal stand-in: many builds place p_pid early; inject_elf only needs ->pid */
    struct { int pid; } local = { .pid = pid };
    return inject_elf(&local, elf) != 0;
  }
  int ok = inject_elf(proc, elf);
  free(proc);
  return ok != 0;
}
