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

/* --- Tunables --- */
#ifndef FANTARGET_POLL_MS
#define FANTARGET_POLL_MS 5000          /* 5 s – less polling */
#endif

#ifndef FANTARGET_LOG_SECONDS
#define FANTARGET_LOG_SECONDS 120       /* log every 2 min */
#endif

/* Idle detection with 3 °C hysteresis:
 *   enter idle when 15-min avg < IDLE_ENTER
 *   leave idle when 15-min avg >= IDLE_EXIT
 *   IDLE_EXIT = IDLE_ENTER + HYSTERESIS
 */
#ifndef FANTARGET_IDLE_ENTER_C
#define FANTARGET_IDLE_ENTER_C 55
#endif

#ifndef FANTARGET_HYSTERESIS_C
#define FANTARGET_HYSTERESIS_C 3
#endif

#define FANTARGET_IDLE_EXIT_C (FANTARGET_IDLE_ENTER_C + FANTARGET_HYSTERESIS_C)

#ifndef FANTARGET_HISTORY_SEC
#define FANTARGET_HISTORY_SEC 900       /* 15 minutes of history */
#endif

/* Only change the fan target when the curve result differs by at least
 * this many degrees from the last applied target (reduces chatter). */
#ifndef FANTARGET_TARGET_HYST_C
#define FANTARGET_TARGET_HYST_C 3
#endif

/* Max samples we keep (poll every 5 s → 180 samples for 15 min) */
#define HISTORY_MAX_SAMPLES ((FANTARGET_HISTORY_SEC * 1000) / FANTARGET_POLL_MS + 8)

#ifndef FANTARGET_SMOKE_LOOPS
#define FANTARGET_SMOKE_LOOPS 0
#endif

#if FANTARGET_POLL_MS < 1000
#error FANTARGET_POLL_MS must be at least 1000
#endif

#if FANTARGET_LOG_SECONDS < 1
#error FANTARGET_LOG_SECONDS must be positive
#endif

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelGetCpuTemperature(int *temperature);
int sceKernelGetSocSensorTemperature(int sensor, int *temperature);
int sceKernelGetCurrentFanDuty(uint16_t *duty, uint64_t *chassis);

static volatile sig_atomic_t stop_requested;

/* Ring buffer for 15-minute temperature average */
typedef struct {
  int16_t samples[HISTORY_MAX_SAMPLES];
  uint32_t count;
  uint32_t head;
  int64_t sum;
} temp_history_t;

static temp_history_t g_history;

static void history_init(temp_history_t *h) {
  memset(h, 0, sizeof(*h));
}

static void history_push(temp_history_t *h, int temp) {
  if (temp < 0)
    return;
  if (h->count < HISTORY_MAX_SAMPLES) {
    h->samples[h->count++] = (int16_t)temp;
    h->sum += temp;
  } else {
    h->sum -= h->samples[h->head];
    h->samples[h->head] = (int16_t)temp;
    h->sum += temp;
    h->head = (h->head + 1) % HISTORY_MAX_SAMPLES;
  }
}

static int history_avg(const temp_history_t *h) {
  if (h->count == 0)
    return -1;
  return (int)(h->sum / (int64_t)h->count);
}

static bool history_full_enough(const temp_history_t *h) {
  /* Need at least ~2 minutes of data before trusting idle detection */
  return h->count >= (120000 / FANTARGET_POLL_MS);
}

/*
 * Quiet + cool curve (optimised for lower noise at idle/light load and
 * earlier fan ramp so peak temps stay lower).
 *
 * Design goals:
 *  - Stay near system default (~91) while cool → quieter menus / light use
 *  - Start lowering the target earlier than stock so the fan reacts before
 *    the SoC is already hot → cooler under sustained load
 *  - Never force an absurdly low target that would spin the fan at full
 *    blast for mild temps
 *
 * Anchors (temp → target):
 *    ≤ 42 °C  → 91 °C   quiet floor
 *      52 °C  → 84 °C   early gentle ramp
 *      58 °C  → 78 °C
 *      64 °C  → 72 °C
 *      70 °C  → 67 °C
 *      76 °C  → 63 °C
 *     ≥ 85 °C → 60 °C   aggressive ceiling
 *
 * Linear interpolation between anchors.
 */
static int target_from_temp(int temp_c) {
  if (temp_c < 0)
    return 85; /* safe fallback */

  static const struct { int t; int target; } anchors[] = {
      {  0, 91 },
      { 42, 91 },
      { 52, 84 },
      { 58, 78 },
      { 64, 72 },
      { 70, 67 },
      { 76, 63 },
      { 85, 60 },
      {100, 60 },
  };
  const int n = (int)(sizeof(anchors) / sizeof(anchors[0]));

  if (temp_c <= anchors[0].t)
    return anchors[0].target;
  if (temp_c >= anchors[n - 1].t)
    return anchors[n - 1].target;

  for (int i = 0; i < n - 1; ++i) {
    if (temp_c >= anchors[i].t && temp_c <= anchors[i + 1].t) {
      int dt = anchors[i + 1].t - anchors[i].t;
      int dtarget = anchors[i + 1].target - anchors[i].target;
      if (dt == 0)
        return anchors[i].target;
      return anchors[i].target + (temp_c - anchors[i].t) * dtarget / dt;
    }
  }
  return 85;
}

/* Apply 3 °C hysteresis on the target itself so we don't thrash the fan
 * controller when temperature is near an interpolation boundary. */
static int apply_target_hysteresis(int raw_desired, int last_applied) {
  if (last_applied < 0)
    return raw_desired;
  int delta = raw_desired - last_applied;
  if (delta > FANTARGET_TARGET_HYST_C || delta < -FANTARGET_TARGET_HYST_C)
    return raw_desired;
  return last_applied;
}

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
                      uint8_t desired_target,
                      uint8_t verified[FAN_CONFIG_SIZE]) {
  uint8_t request[FAN_CONFIG_SIZE];

  memcpy(request, current, sizeof(request));
  request[5] = desired_target;
  if (ioctl(fd, FAN_SET_AUTOSERVO, request) != 0) {
    return -1;
  }
  if (get_fan_config(fd, verified) != 0) {
    return -1;
  }
  return verified[5] == desired_target ? 0 : -1;
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

/* Returns the higher of CPU temp and max SoC temp, or -1 on failure */
static int read_system_temp(const bool sensors[SOC_SENSOR_COUNT]) {
  int cpu = 0;
  int cpu_rc = sceKernelGetCpuTemperature(&cpu);
  int soc_max = 0;
  int soc_max_id = -1;

  for (unsigned sensor = 0; sensor < SOC_SENSOR_COUNT; ++sensor) {
    int temperature = 0;
    if (sensors[sensor] &&
        sceKernelGetSocSensorTemperature((int)sensor, &temperature) == 0) {
      if (soc_max_id < 0 || temperature > soc_max) {
        soc_max = temperature;
        soc_max_id = (int)sensor;
      }
    }
  }

  if (cpu_rc == 0 && soc_max_id >= 0) {
    return cpu > soc_max ? cpu : soc_max;
  }
  if (cpu_rc == 0)
    return cpu;
  if (soc_max_id >= 0)
    return soc_max;
  return -1;
}

static void log_status(const bool sensors[SOC_SENSOR_COUNT],
                       int current_target, int desired_target,
                       int system_temp, int avg_temp, bool idle) {
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
  char target_text[56];
  char avg_text[24];

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
    if (idle) {
      (void)snprintf(target_text, sizeof(target_text),
                     "%d C (idle, not fighting)", current_target);
    } else {
      (void)snprintf(target_text, sizeof(target_text), "%d C (want %d C)",
                     current_target, desired_target);
    }
  } else {
    (void)snprintf(target_text, sizeof(target_text), "unavailable");
  }
  if (avg_temp >= 0) {
    (void)snprintf(avg_text, sizeof(avg_text), "%d C", avg_temp);
  } else {
    (void)snprintf(avg_text, sizeof(avg_text), "warming up");
  }

  log_line("status CPU=%s, SoC=%s, fan=%s, target=%s, avg15m=%s%s",
           cpu_text, soc_text, fan_text, target_text, avg_text,
           idle ? " [IDLE]" : "");
}

int main(void) {
  bool sensors[SOC_SENSOR_COUNT];
  struct sigaction action;
  int fan_fd = -1;
  int last_target = -1;
  int last_applied_desired = -1; /* last target we actually wrote */
  bool device_error_logged = false;
  bool correction_error_logged = false;
  bool idle = false; /* latched with hysteresis */
  bool idle_logged = false;
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

  history_init(&g_history);
  discover_soc_sensors(sensors);
  next_log = monotonic_seconds();
  log_line("started; quiet+cool curve, idle enter<%d C exit>=%d C (hyst=%d C)",
           FANTARGET_IDLE_ENTER_C, FANTARGET_IDLE_EXIT_C,
           FANTARGET_HYSTERESIS_C);
  log_line("poll=%d ms, log every %d s, target hyst=%d C",
           FANTARGET_POLL_MS, FANTARGET_LOG_SECONDS, FANTARGET_TARGET_HYST_C);

  while (!stop_requested) {
    uint8_t config[FAN_CONFIG_SIZE];
    int current_target = -1;
    int system_temp = -1;
    int avg_temp = -1;
    int raw_desired = -1;
    int desired_target = -1;

    system_temp = read_system_temp(sensors);
    if (system_temp >= 0) {
      history_push(&g_history, system_temp);
    }
    avg_temp = history_avg(&g_history);

    /* Idle latch with 3 °C hysteresis */
    if (history_full_enough(&g_history) && avg_temp >= 0) {
      if (!idle && avg_temp < FANTARGET_IDLE_ENTER_C) {
        idle = true;
      } else if (idle && avg_temp >= FANTARGET_IDLE_EXIT_C) {
        idle = false;
      }
    }

    if (system_temp >= 0) {
      raw_desired = target_from_temp(system_temp);
      desired_target = apply_target_hysteresis(raw_desired, last_applied_desired);
    } else {
      desired_target = 85; /* safe fallback */
    }

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

    if (idle) {
      /* Do not fight the system's target while idle */
      if (!idle_logged) {
        log_line("idle (avg15m=%d C < %d C); leaving system target alone "
                 "(exit when avg>=%d C)",
                 avg_temp, FANTARGET_IDLE_ENTER_C, FANTARGET_IDLE_EXIT_C);
        idle_logged = true;
      }
      correction_error_logged = false;
    } else {
      if (idle_logged) {
        log_line("left idle (avg15m=%d C >= %d C); resuming curve control",
                 avg_temp, FANTARGET_IDLE_EXIT_C);
        idle_logged = false;
      }
      if (current_target != desired_target) {
        uint8_t verified[FAN_CONFIG_SIZE];
        int old_target = current_target;
        if (set_target(fan_fd, config, (uint8_t)desired_target, verified) == 0) {
          current_target = verified[5];
          last_applied_desired = desired_target;
          correction_error_logged = false;
          log_line("target set: %d C -> %d C (temp=%d C, avg15m=%d C, raw=%d C)",
                   old_target, current_target, system_temp, avg_temp,
                   raw_desired);
        } else {
          if (!correction_error_logged) {
            log_line("cannot change target from %d C to %d C", old_target,
                     desired_target);
            correction_error_logged = true;
          }
          close(fan_fd);
          fan_fd = -1;
        }
      } else {
        last_applied_desired = desired_target;
        correction_error_logged = false;
      }
    }

    last_target = current_target;

  wait_for_next_poll:
    if (monotonic_seconds() >= next_log) {
      log_status(sensors, current_target, desired_target, system_temp,
                 avg_temp, idle);
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
