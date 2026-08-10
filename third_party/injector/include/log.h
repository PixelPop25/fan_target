#pragma once
#include <stdio.h>
#include <errno.h>
#include <string.h>
#define LOG_PUTS(s) do { fputs("[elfldr] " s "\n", stdout); fflush(stdout); } while (0)
#define LOG_PERROR(s) do { fprintf(stdout, "[elfldr] %s: %s\n", s, strerror(errno)); fflush(stdout); } while (0)
#define LOG_PT_PERROR(pid, s) do { fprintf(stdout, "[elfldr] pid=%d %s: %s\n", (int)(pid), s, strerror(errno)); fflush(stdout); } while (0)
static inline void klog_perror(const char *s) { LOG_PERROR(s); }
