#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define PROCESS_NAME "fan_target.elf"
#define FAN_DEVICE "/dev/icc_fan"
#define FAN_OPEN_FLAGS 0x10002
#define FAN_GET_AUTOSERVO 0xC01C8F08UL
#define FAN_SET_AUTOSERVO 0xC01C8F07UL
#define FAN_CONFIG_SIZE 28
#define SOC_SENSOR_COUNT 16
#define SCE_KERNEL_ERROR_EINVAL 0x80020016u

/* Offsets in the PS5 KERN_PROC_PROC record */
#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447

#ifndef FANTARGET_TARGET_C
#define FANTARGET_TARGET_C 85
#endif

#ifndef FANTARGET_POLL_MS
#define FANTARGET_POLL_MS 2000
#endif

#ifndef FANTARGET_LOG_SECONDS
#define FANTARGET_LOG_SECONDS 60
#endif

#ifndef FANTARGET_SMOKE_LOOPS
#define FANTARGET_SMOKE_LOOPS 0
#endif

#if FANTARGET_TARGET_C < 50 || FANTARGET_TARGET_C > 95
#error FANTARGET_TARGET_C must be between 50 and 95
#endif

#if FANTARGET_POLL_MS < 250
#error FANTARGET_POLL_MS must be at least 250
#endif

#if FANTARGET_LOG_SECONDS < 1
#error FANTARGET_LOG_SECONDS must be positive
#endif

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelGetCpuTemperature(int *temperature);
int sceKernelGetSocSensorTemperature(int sensor, int *temperature);
int sceKernelGetCurrentFanDuty(uint16_t *duty, uint64_t *chassis);

static volatile sig_atomic_t stop_requested;

static void log_line(const char *format, ...) {
  char message[512];
  char line[576];
  va_list args;

  va_start(args, format);
  (void)vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  (void)snprintf(line, sizeof(line), "[fan_target] %s\n", message);
  fputs(line, stdout);
  fflush(stdout);
  (void)sceKernelDebugOutText(0, line);
}

static void on_signal(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

static void sleep_poll_interval(void) {
  struct timespec request = {
      .tv_sec = FANTARGET_POLL_MS / 1000,
      .tv_nsec = (long)(FANTARGET_POLL_MS % 1000) * 1000000L,
  };
  struct timespec remaining;

  while (!stop_requested && nanosleep(&request, &remaining) != 0 &&
         errno == EINTR) {
    request = remaining;
  }
}

static uint64_t monotonic_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec;
}

static bool process_name_matches(const char *name, size_t capacity) {
  size_t length = strnlen(name, capacity);
  if (length == capacity) {
    return false;
  }
  return strcmp(name, PROCESS_NAME) == 0;
}

static pid_t find_old_instance(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  uint8_t *buffer;
  uint8_t *cursor;
  uint8_t *end;
  pid_t self = getpid();
  pid_t found = 0;

  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) {
    return size == 0 ? 0 : -1;
  }
  buffer = malloc(size);
  if (buffer == NULL) {
    return -1;
  }
  if (sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
    free(buffer);
    return -1;
  }

  cursor = buffer;
  end = buffer + size;
  while ((size_t)(end - cursor) >= sizeof(int)) {
    int record_size = 0;
    pid_t pid = 0;
    const char *name;
    size_t name_capacity;

    memcpy(&record_size, cursor, sizeof(record_size));
    if (record_size <= KINFO_TDNAME_OFFSET ||
        (size_t)record_size > (size_t)(end - cursor)) {
      found = -1;
      break;
    }
    memcpy(&pid, cursor + KINFO_PID_OFFSET, sizeof(pid));
    name = (const char *)(cursor + KINFO_TDNAME_OFFSET);
    name_capacity = (size_t)record_size - KINFO_TDNAME_OFFSET;
    if (pid != self && process_name_matches(name, name_capacity)) {
      found = pid;
      break;
    }
    cursor += record_size;
  }

  free(buffer);
  return found;
}

static int stop_old_instances(void) {
  unsigned count = 0;

  while (count < 16) {
    pid_t pid = find_old_instance();
    if (pid == 0) {
      return 0;
    }
    if (pid < 0) {
      log_line("cannot find the previous instance");
      return -1;
    }
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
      log_line("cannot stop the previous instance");
      return -1;
    }
    log_line("replaced the previous instance");
    ++count;
    usleep(100000);
  }

  log_line("too many old instances");
  return -1;
}

static int get_fan_config(int fd, uint8_t config[FAN_CONFIG_SIZE]) {
  memset(config, 0, FAN_CONFIG_SIZE);
  return ioctl(fd, FAN_GET_AUTOSERVO, config);
}

static int set_target(int fd, const uint8_t current[FAN_CONFIG_SIZE],
                      uint8_t verified[FAN_CONFIG_SIZE]) {
  uint8_t request[FAN_CONFIG_SIZE];

  memcpy(request, current, sizeof(request));
  request[5] = (uint8_t)FANTARGET_TARGET_C;
  if (ioctl(fd, FAN_SET_AUTOSERVO, request) != 0) {
    return -1;
  }
  if (get_fan_config(fd, verified) != 0) {
    return -1;
  }
  return verified[5] == (uint8_t)FANTARGET_TARGET_C ? 0 : -1;
}

static void discover_soc_sensors(bool present[SOC_SENSOR_COUNT]) {
  memset(present, 0, sizeof(bool) * SOC_SENSOR_COUNT);
  for (unsigned sensor = 0; sensor < SOC_SENSOR_COUNT; ++sensor) {
    int temperature = 0;
    int rc = sceKernelGetSocSensorTemperature((int)sensor, &temperature);
    if ((uint32_t)rc == SCE_KERNEL_ERROR_EINVAL) {
      break;
    }
    if (rc == 0) {
      present[sensor] = true;
    }
  }
}

static void log_status(const bool sensors[SOC_SENSOR_COUNT],
                       int current_target) {
  int cpu = 0;
  int cpu_rc = sceKernelGetCpuTemperature(&cpu);
  int soc_max = 0;
  int soc_max_id = -1;
  unsigned soc_ok = 0;
  uint16_t duty = 0;
  uint64_t chassis = 0;
  int duty_rc;
  char cpu_text[24];
  char soc_text[24];
  char fan_text[24];
  char target_text[24];

  for (unsigned sensor = 0; sensor < SOC_SENSOR_COUNT; ++sensor) {
    int temperature = 0;
    if (sensors[sensor] &&
        sceKernelGetSocSensorTemperature((int)sensor, &temperature) == 0) {
      if (soc_max_id < 0 || temperature > soc_max) {
        soc_max = temperature;
        soc_max_id = (int)sensor;
      }
      ++soc_ok;
    }
  }
  duty_rc = sceKernelGetCurrentFanDuty(&duty, &chassis);

  if (cpu_rc == 0) {
    (void)snprintf(cpu_text, sizeof(cpu_text), "%d C", cpu);
  } else {
    (void)snprintf(cpu_text, sizeof(cpu_text), "unavailable");
  }
  if (soc_max_id >= 0 && soc_ok > 0) {
    (void)snprintf(soc_text, sizeof(soc_text), "%d C", soc_max);
  } else {
    (void)snprintf(soc_text, sizeof(soc_text), "unavailable");
  }
  if (duty_rc == 0) {
    (void)snprintf(fan_text, sizeof(fan_text), "%.1f%%",
                   (double)duty * 100.0 / 1024.0);
  } else {
    (void)snprintf(fan_text, sizeof(fan_text), "unavailable");
  }
  if (current_target >= 0) {
    (void)snprintf(target_text, sizeof(target_text), "%d C", current_target);
  } else {
    (void)snprintf(target_text, sizeof(target_text), "unavailable");
  }

  log_line("status CPU=%s, SoC=%s, fan=%s, target=%s", cpu_text, soc_text,
           fan_text, target_text);
}

int main(void) {
  bool sensors[SOC_SENSOR_COUNT];
  struct sigaction action;
  int fan_fd = -1;
  int last_target = -1;
  bool device_error_logged = false;
  bool correction_error_logged = false;
  uint64_t next_log;
#if FANTARGET_SMOKE_LOOPS > 0
  unsigned long loops = 0;
#endif

  if (stop_old_instances() != 0) {
    return 1;
  }
  if (syscall(SYS_thr_set_name, -1, PROCESS_NAME) != 0) {
    log_line("cannot set the process name");
    return 1;
  }

  memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGHUP, &action, NULL);

  discover_soc_sensors(sensors);
  next_log = monotonic_seconds();
  log_line("started; target=%d C", FANTARGET_TARGET_C);

  while (!stop_requested) {
    uint8_t config[FAN_CONFIG_SIZE];
    int current_target = -1;

    if (fan_fd < 0) {
      fan_fd = open(FAN_DEVICE, FAN_OPEN_FLAGS);
      if (fan_fd < 0) {
        if (!device_error_logged) {
          log_line("fan controller is unavailable; retrying");
          device_error_logged = true;
        }
        goto wait_for_next_poll;
      }
      device_error_logged = false;
    }

    if (get_fan_config(fan_fd, config) != 0) {
      if (!device_error_logged) {
        log_line("cannot read the target; reconnecting to the fan controller");
        device_error_logged = true;
      }
      close(fan_fd);
      fan_fd = -1;
      goto wait_for_next_poll;
    }
    device_error_logged = false;
    current_target = config[5];

    if (last_target >= 0 && current_target != last_target) {
      log_line("target changed: %d C -> %d C", last_target, current_target);
    }
    if (current_target != FANTARGET_TARGET_C) {
      uint8_t verified[FAN_CONFIG_SIZE];
      int old_target = current_target;
      if (set_target(fan_fd, config, verified) == 0) {
        current_target = verified[5];
        correction_error_logged = false;
        log_line("target restored: %d C -> %d C", old_target,
                 current_target);
      } else {
        if (!correction_error_logged) {
          log_line("cannot change target from %d C to %d C", old_target,
                   FANTARGET_TARGET_C);
          correction_error_logged = true;
        }
        close(fan_fd);
        fan_fd = -1;
      }
    } else {
      correction_error_logged = false;
    }
    last_target = current_target;

  wait_for_next_poll:
    if (monotonic_seconds() >= next_log) {
      log_status(sensors, current_target);
      next_log = monotonic_seconds() + FANTARGET_LOG_SECONDS;
    }
#if FANTARGET_SMOKE_LOOPS > 0
    ++loops;
    if (loops >= FANTARGET_SMOKE_LOOPS) {
      break;
    }
#endif
    sleep_poll_interval();
  }

  if (fan_fd >= 0) {
    close(fan_fd);
  }
  log_line("stopped");
  return 0;
}
