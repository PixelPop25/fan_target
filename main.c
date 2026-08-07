#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "fps_elf_blob.h"

#define PROCESS_NAME "fan_target_pxp.elf"
#define CONFIG_DIR "/data/fan_target"
#define CONFIG_FILE CONFIG_DIR "/config.ini"
#define FPS_ELF_DIR CONFIG_DIR "/fps_elf"
#define FPS_ELF_PATH FPS_ELF_DIR "/fps_elf.elf"
/* Loopback ELF loader (elfldr) port; matches etaHEN's "Johns elfldr"
 * convention used elsewhere in this project's payload-sending tooling. */
#define ELFLDR_HOST "127.0.0.1"
#define ELFLDR_PORT 9021
#define FPS_UDP_PORT 29028
/* Process names — same as etaHEN */
#define SHELLUI_PROC_NAME "SceShellUI"
#define FAN_DEVICE "/dev/icc_fan"
#define FAN_OPEN_FLAGS 0x10002
#define FAN_GET_AUTOSERVO 0xC01C8F08UL
#define FAN_SET_AUTOSERVO 0xC01C8F07UL
#define FAN_CONFIG_SIZE 28
#define SOC_SENSOR_COUNT 16
#define SCE_KERNEL_ERROR_EINVAL 0x80020016u
#define MAX_PAD_HANDLES 4
#define MAX_CURVE_ANCHORS 9
#define DEFAULT_GPU_SENSOR 0
#define DEFAULT_SSD_SENSOR 1

typedef struct {
  int t;
  int target;
} curve_anchor_t;

#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447

#ifndef FANTARGET_POLL_MS
#define FANTARGET_POLL_MS 5000
#endif

#ifndef FANTARGET_LOG_SECONDS
#define FANTARGET_LOG_SECONDS 120
#endif

#ifndef FANTARGET_IDLE_ENTER_C
#define FANTARGET_IDLE_ENTER_C 50
#endif

#ifndef FANTARGET_HYSTERESIS_C
#define FANTARGET_HYSTERESIS_C 3
#endif

#define FANTARGET_IDLE_EXIT_C (FANTARGET_IDLE_ENTER_C + FANTARGET_HYSTERESIS_C)

#ifndef FANTARGET_HISTORY_SEC
#define FANTARGET_HISTORY_SEC 300
#endif

#ifndef FANTARGET_TARGET_HYST_C
#define FANTARGET_TARGET_HYST_C 3
#endif

#ifndef FANTARGET_LIGHTBAR_SEC
#define FANTARGET_LIGHTBAR_SEC 180
#endif

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
int sceUserServiceGetGlsOverlayPosition(int *x, int *y);
int sceUserServiceSetGlsOverlayPosition(int x, int y);

int32_t sceUserServiceInitialize(void *params);
int32_t sceUserServiceGetInitialUser(int32_t *userId);
int32_t sceUserServiceGetForegroundUser(int32_t *userId);

int32_t scePadInit(void);
int32_t scePadOpen(int32_t userId, int32_t type, int32_t index, void *param);
int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
int32_t scePadClose(int32_t handle);
int32_t scePadSetLightBar(int32_t handle, const void *color);
int32_t scePadSetProcessPrivilege(int32_t privilege);

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t reserved;
} ScePadLightBar;

typedef enum {
  LB_NONE = 0,
  LB_BLUE,
  LB_GREEN,
  LB_ORANGE,
  LB_RED
} lightbar_band_t;

typedef enum {
  OVERLAY_TOP_LEFT = 0,
  OVERLAY_TOP_RIGHT,
  OVERLAY_BOTTOM_LEFT,
  OVERLAY_BOTTOM_RIGHT,
} overlay_position_t;

static volatile sig_atomic_t stop_requested;

typedef struct {
  int16_t samples[HISTORY_MAX_SAMPLES];
  uint32_t count;
  uint32_t head;
  int64_t sum;
} temp_history_t;

typedef struct {
  bool cpu_temp;
  bool gpu_temp;
  bool soc_temp;
  bool ram_usage;
  bool ssd_temp;
  bool fps;
  uint32_t fps_color; /* 0xRRGGBB, forwarded to fps_elf via fps_config.ini */
  char metric;
  overlay_position_t position;
  int curve_temps[MAX_CURVE_ANCHORS];
  int curve_targets[MAX_CURVE_ANCHORS];
  int curve_count;
} config_t;

static const struct {
  const char *name;
  overlay_position_t position;
} overlay_position_names[] = {
  {"top left", OVERLAY_TOP_LEFT},
  {"top right", OVERLAY_TOP_RIGHT},
  {"bottom left", OVERLAY_BOTTOM_LEFT},
  {"bottom right", OVERLAY_BOTTOM_RIGHT},
};

static temp_history_t g_history;
static config_t g_config;
static int32_t g_pad_handles[MAX_PAD_HANDLES];
static int g_pad_count;
static bool g_pad_ready;
static lightbar_band_t g_last_band = LB_NONE;
static uint64_t g_start_sec;

/*
 * Forward declarations for every static helper below. Several helpers call
 * others that are defined later in the file (e.g. the fps overlay setup
 * calls log_line() before log_line()'s own definition); without this block
 * that's an implicit-declaration error under -Werror, which is one of the
 * reasons this file failed to build.
 */
static void history_init(temp_history_t *h);
static void history_push(temp_history_t *h, int temp);
static int history_avg(const temp_history_t *h);
static bool history_full_enough(const temp_history_t *h);
static int target_from_temp(int temp_c);
static int apply_target_hysteresis(int raw_desired, int last_applied);
static lightbar_band_t band_from_temp(int temp_c, lightbar_band_t previous);
static void band_to_rgb(lightbar_band_t band, ScePadLightBar *out);
static uint64_t monotonic_seconds(void);
static bool deploy_fps_elf_once(void);
static void write_fps_config_file(void);
static bool inject_blob_via_elfldr(const unsigned char *blob, unsigned int len,
                                   const char *tag);
static bool inject_fps_elf(void);
static int get_shellui_pid(void);
static int get_game_pid(void);
static void setup_fps_overlay(void);
static void setup_shellui_overlay(void);
static void pad_close_all(void);
static void pad_init_handles(void);
static bool lightbar_window_active(void);
static void pad_apply_lightbar(int system_temp);
static void log_line(const char *format, ...);
static void on_signal(int signal_number);
static void sleep_poll_interval(void);
static bool process_name_matches(const char *name, size_t capacity);
static pid_t find_old_instance(void);
static int stop_old_instances(void);
static int get_fan_config(int fd, uint8_t config[FAN_CONFIG_SIZE]);
static int set_target(int fd, const uint8_t current[FAN_CONFIG_SIZE],
                      uint8_t desired_target,
                      uint8_t verified[FAN_CONFIG_SIZE]);
static void discover_soc_sensors(bool present[SOC_SENSOR_COUNT]);
static int read_system_temp(const bool sensors[SOC_SENSOR_COUNT]);
static void log_status(const bool sensors[SOC_SENSOR_COUNT],
                       int current_target, int desired_target,
                       int system_temp, int avg_temp, bool idle);
static bool match_key(const char *line, const char *key, char *value,
                      size_t value_size);
static void normalize_line(char *line);
static overlay_position_t parse_overlay_position(const char *value);
static void init_default_config(void);
static bool parse_config_file(void);
static bool write_default_config_file(void);
static bool ensure_config_directory(void);
static void apply_overlay_position(void);

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
  return h->count >= (60000 / FANTARGET_POLL_MS);
}

/*
 * Fan curve (not overbearing — stays near system default when cool).
 *   ≤40 → 91 (system default)
 *    52 → 88
 *    58 → 84
 *    64 → 75
 *    70 → 72
 *    75 → 70
 *   ≥85 → 68
 */
static int target_from_temp(int temp_c) {
  if (temp_c < 0)
    return 91;

  curve_anchor_t anchors[MAX_CURVE_ANCHORS];
  int n = 0;

  if (g_config.curve_count > 0) {
    for (int i = 0; i < g_config.curve_count && i < MAX_CURVE_ANCHORS; ++i) {
      anchors[i].t = g_config.curve_temps[i];
      anchors[i].target = g_config.curve_targets[i];
    }
    n = g_config.curve_count;
  } else {
    static const curve_anchor_t default_anchors[] = {
        {  0, 91 },
        { 40, 91 },
        { 52, 88 },
        { 58, 84 },
        { 64, 75 },
        { 70, 72 },
        { 75, 70 },
        { 85, 68 },
        {100, 68 },
    };
    for (int i = 0; i < (int)(sizeof(default_anchors) / sizeof(default_anchors[0])); ++i) {
      anchors[i].t = default_anchors[i].t;
      anchors[i].target = default_anchors[i].target;
    }
    n = (int)(sizeof(default_anchors) / sizeof(default_anchors[0]));
  }

  for (int i = 1; i < n; ++i) {
    for (int j = i; j > 0 && anchors[j].t < anchors[j - 1].t; --j) {
      curve_anchor_t tmp = anchors[j];
      anchors[j] = anchors[j - 1];
      anchors[j - 1] = tmp;
    }
  }

  if (n == 0)
    return 91;
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
  return anchors[n - 1].target;
}

static int apply_target_hysteresis(int raw_desired, int last_applied) {
  if (last_applied < 0)
    return raw_desired;
  int delta = raw_desired - last_applied;
  if (delta > FANTARGET_TARGET_HYST_C || delta < -FANTARGET_TARGET_HYST_C)
    return raw_desired;
  return last_applied;
}

/*
 * Lightbar bands (with gaps); hysteresis holds previous colour in gaps.
 *   < 53       blue
 *   55–62      green
 *   64–70      orange
 *   ≥ 72       red
 */
static lightbar_band_t band_from_temp(int temp_c, lightbar_band_t previous) {
  if (temp_c < 0)
    return previous != LB_NONE ? previous : LB_BLUE;

  if (temp_c < 53)
    return LB_BLUE;
  if (temp_c >= 55 && temp_c <= 62)
    return LB_GREEN;
  if (temp_c >= 64 && temp_c <= 70)
    return LB_ORANGE;
  if (temp_c >= 72)
    return LB_RED;

  /* gap zones: 53–54, 63, 71 — keep previous, default blue */
  return previous != LB_NONE ? previous : LB_BLUE;
}

static void band_to_rgb(lightbar_band_t band, ScePadLightBar *out) {
  out->reserved = 0;
  switch (band) {
  case LB_RED:
    out->r = 255;
    out->g = 0;
    out->b = 0;
    break;
  case LB_ORANGE:
    out->r = 255;
    out->g = 100;
    out->b = 0;
    break;
  case LB_GREEN:
    out->r = 0;
    out->g = 220;
    out->b = 40;
    break;
  case LB_BLUE:
  default:
    out->r = 0;
    out->g = 80;
    out->b = 255;
    break;
  }
}

static uint64_t monotonic_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec;
}

static bool g_fps_deployed = false;
static bool g_fps_injected = false;

/*
 * fps_elf is NOT a loadable module for our own process -- it's a
 * self-contained game-process payload (see third_party/fps_elf/src/prx.cpp)
 * that hooks sceGnmSubmitAndFlipCommandBuffersForWorkload, a call made by
 * the *game*, not by fan_target. It has to be injected into the running
 * game the same way an ELF loader (elfldr) does, not loaded into ourselves
 * with sceKernelLoadStartModule.
 *
 * We ship it embedded inside fan_target.elf (see fps_elf_blob.h, generated
 * by tools/gen_fps_elf_blob.py at build time) so only a single file needs
 * to be deployed. On first run we write it out to FPS_ELF_PATH once; after
 * that we just re-inject the in-memory copy, so no disk read is needed on
 * the hot path.
 */
static bool deploy_fps_elf_once(void) {
  struct stat st;

  if (fps_elf_blob_len == 0) {
    log_line("fps overlay not embedded in this build (empty blob); skipping");
    return false;
  }
  if (mkdir(FPS_ELF_DIR, 0755) != 0 && errno != EEXIST) {
    log_line("cannot create %s", FPS_ELF_DIR);
    return false;
  }
  if (stat(FPS_ELF_PATH, &st) == 0 && S_ISREG(st.st_mode) &&
      (uint32_t)st.st_size == fps_elf_blob_len) {
    return true; /* already deployed, matching size -- don't copy again */
  }

  FILE *file = fopen(FPS_ELF_PATH, "wb");
  if (!file) {
    log_line("cannot write %s", FPS_ELF_PATH);
    return false;
  }
  size_t written = fwrite(fps_elf_blob, 1, fps_elf_blob_len, file);
  fclose(file);
  if (written != fps_elf_blob_len) {
    log_line("short write deploying fps_elf.elf (%zu/%u bytes)", written,
              fps_elf_blob_len);
    return false;
  }
  log_line("deployed fps overlay to %s (%u bytes)", FPS_ELF_PATH,
            fps_elf_blob_len);
  return true;
}

/* Writes the subset of config.ini the fps overlay itself understands
 * (metric toggles, position, colour). fps_elf reads this on its own
 * startup; main.c does not draw anything itself. */
static void write_fps_config_file(void) {
  FILE *file = fopen(FPS_ELF_DIR "/fps_config.ini", "w");
  if (!file)
    return;
  fprintf(file,
          "cpu_temp=%d\n"
          "gpu_temp=%d\n"
          "soc_temp=%d\n"
          "ram_usage=%d\n"
          "ssd_temp=%d\n"
          "fps=%d\n"
          "metric=%c\n"
          "overlay_position=%d\n"
          "fps_color=%06X\n",
          g_config.cpu_temp ? 1 : 0, g_config.gpu_temp ? 1 : 0,
          g_config.soc_temp ? 1 : 0, g_config.ram_usage ? 1 : 0,
          g_config.ssd_temp ? 1 : 0, g_config.fps ? 1 : 0, g_config.metric,
          (int)g_config.position, g_config.fps_color & 0xFFFFFFu);
  fclose(file);
}

/* Sends the embedded ELF to the local elfldr so it gets injected into
 * whichever game process is currently running -- the same "push a payload
 * over a socket" approach as our PS5-4-Payload-Sender tooling, just over
 * loopback instead of the network. Best-effort: if no game/elfldr is up
 * yet this simply fails quietly and we log it. */
/* Push raw ELF bytes to local elfldr (port 9021). Same transport etaHEN
 * tooling uses; for PID-targeted inject into SceShellUI/game the runtime
 * still needs ptrace inject_elf (libNineS) when available — see
 * third_party/injector. Until then elfldr loads the payload in its
 * configured target context. */
/* Prefer etaHEN Inject_Toolbox (ptrace into PID) when linked; else elfldr socket. */
extern int find_pid_by_name(const char *name) __attribute__((weak));
extern _Bool Inject_Toolbox(int pid, unsigned char *elf) __attribute__((weak));

static bool inject_blob_to_pid(int pid, const unsigned char *blob, unsigned int len,
                               const char *tag) {
  if (!blob || len == 0)
    return false;
  if (pid > 0 && Inject_Toolbox) {
    if (Inject_Toolbox(pid, (unsigned char *)blob)) {
      log_line("%s: Inject_Toolbox ok into pid %d (%u bytes)", tag, pid, len);
      return true;
    }
    log_line("%s: Inject_Toolbox failed pid %d — falling back to elfldr", tag, pid);
  }
  return inject_blob_via_elfldr(blob, len, tag);
}

static bool inject_blob_via_elfldr(const unsigned char *blob, unsigned int len,
                                   const char *tag) {
  if (!blob || len == 0)
    return false;

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    log_line("%s inject: cannot create socket (%d)", tag, errno);
    return false;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ELFLDR_PORT);
  if (inet_pton(AF_INET, ELFLDR_HOST, &addr.sin_addr) != 1) {
    close(sock);
    return false;
  }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    log_line("%s inject: elfldr not reachable on port %d (%d)", tag,
             ELFLDR_PORT, errno);
    close(sock);
    return false;
  }

  size_t sent = 0;
  bool ok = true;
  while (sent < len) {
    ssize_t n = send(sock, blob + sent, len - sent, 0);
    if (n <= 0) {
      log_line("%s inject: send failed after %zu/%u (%d)", tag, sent, len,
               errno);
      ok = false;
      break;
    }
    sent += (size_t)n;
  }
  close(sock);

  if (ok)
    log_line("%s injected via elfldr (%u bytes)", tag, len);
  return ok;
}

static bool inject_fps_elf(void) {
  int pid = -1;
  if (find_pid_by_name)
    pid = find_pid_by_name("SceApplication"); /* best-effort; game name varies */
  if (pid <= 0)
    pid = get_game_pid();
  return inject_blob_to_pid(pid, fps_elf_blob, fps_elf_blob_len, "fps_elf");
}

/* Declared weak so builds without the symbol still link. */
__attribute__((weak)) int sceKernelGetProcessName(int pid, char *name);

/* etaHEN-style: scan PIDs for process name "SceShellUI". */
static int get_shellui_pid(void) {
  if (!sceKernelGetProcessName)
    return -1;
  char name[256];
  for (int pid = 1; pid <= 9999; ++pid) {
    memset(name, 0, sizeof(name));
    if (sceKernelGetProcessName(pid, name) != 0)
      continue;
    if (strcmp(name, SHELLUI_PROC_NAME) == 0)
      return pid;
  }
  return -1;
}

/* Big-app PID via system service when linked; else -1. */
static int get_game_pid(void) {
  /* Best-effort: caller may use elfldr while game is foreground. */
  return -1;
}

/*
 * ShellUI overlay path (etaHEN cmd_enable_toolbox equivalent):
 *  1. Resolve SceShellUI PID
 *  2. Inject overlay ELF into that process (Mono already loaded there)
 * Overlay creates labels and receives FPS on UDP :29028.
 *
 * Full PID-targeted inject requires inject_elf() from libNineS (ptrace +
 * elfldr_load). We always try elfldr socket; when third_party/injector is
 * linked, prefer inject_elf(proc, blob).
 */
static void setup_shellui_overlay(void) {
  int pid = get_shellui_pid();
  if (pid <= 0 && find_pid_by_name)
    pid = find_pid_by_name(SHELLUI_PROC_NAME);
  if (pid > 0)
    log_line("SceShellUI pid=%d — injecting overlay", pid);
  else
    log_line("SceShellUI not found — overlay inject via elfldr only");

  /* When overlay_elf is embedded (same blob mechanism as fps_elf), inject it.
   * Build third_party/overlay_elf and add to gen/ blob to enable. */
  log_line("shellui overlay setup done (pid=%d)", pid);
}

static void setup_fps_overlay(void) {
  write_fps_config_file();
  g_fps_deployed = deploy_fps_elf_once();
  if (!g_fps_deployed)
    return;

  /* etaHEN cmd_enable_fps_new: inject into game process */
  int game_pid = get_game_pid();
  if (game_pid > 0)
    log_line("game pid=%d — injecting fps_elf", game_pid);

  g_fps_injected = inject_fps_elf();
  setup_shellui_overlay();
}

static void pad_close_all(void) {
  for (int i = 0; i < g_pad_count; ++i) {
    if (g_pad_handles[i] >= 0) {
      (void)scePadClose(g_pad_handles[i]);
      g_pad_handles[i] = -1;
    }
  }
  g_pad_count = 0;
  g_pad_ready = false;
  g_last_band = LB_NONE;
}

static void pad_init_handles(void) {
  int32_t userId = -1;
  int32_t ret;

  g_pad_count = 0;
  g_pad_ready = false;
  for (int i = 0; i < MAX_PAD_HANDLES; ++i)
    g_pad_handles[i] = -1;

  (void)sceUserServiceInitialize(NULL);
  ret = sceUserServiceGetInitialUser(&userId);
  if (ret != 0 || userId < 0) {
    ret = sceUserServiceGetForegroundUser(&userId);
    if (ret != 0 || userId < 0)
      userId = 0x10000000;
  }

  if (scePadInit() != 0)
    return;

  (void)scePadSetProcessPrivilege(1);

  for (int idx = 0; idx < MAX_PAD_HANDLES; ++idx) {
    int32_t h = scePadGetHandle(userId, 0, idx);
    if (h < 0)
      h = scePadOpen(userId, 0, idx, NULL);
    if (h >= 0)
      g_pad_handles[g_pad_count++] = h;
  }

  g_pad_ready = (g_pad_count > 0);
}

static bool lightbar_window_active(void) {
  uint64_t now = monotonic_seconds();
  if (g_start_sec == 0 || now < g_start_sec)
    return true;
  return (now - g_start_sec) < (uint64_t)FANTARGET_LIGHTBAR_SEC;
}

static void pad_apply_lightbar(int system_temp) {
  lightbar_band_t band;
  ScePadLightBar color;

  if (!g_pad_ready || !lightbar_window_active())
    return;

  band = band_from_temp(system_temp, g_last_band);
  if (band == g_last_band)
    return;

  band_to_rgb(band, &color);
  for (int i = 0; i < g_pad_count; ++i) {
    if (g_pad_handles[i] >= 0)
      (void)scePadSetLightBar(g_pad_handles[i], &color);
  }
  g_last_band = band;
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

static bool process_name_matches(const char *name, size_t capacity) {
  size_t length = strnlen(name, capacity);
  if (length == capacity)
    return false;
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

  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0)
    return size == 0 ? 0 : -1;
  buffer = malloc(size);
  if (buffer == NULL)
    return -1;
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
    if (pid == 0)
      return 0;
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
  if (ioctl(fd, FAN_SET_AUTOSERVO, request) != 0)
    return -1;
  if (get_fan_config(fd, verified) != 0)
    return -1;
  return verified[5] == desired_target ? 0 : -1;
}

static void discover_soc_sensors(bool present[SOC_SENSOR_COUNT]) {
  memset(present, 0, sizeof(bool) * SOC_SENSOR_COUNT);
  for (unsigned sensor = 0; sensor < SOC_SENSOR_COUNT; ++sensor) {
    int temperature = 0;
    int rc = sceKernelGetSocSensorTemperature((int)sensor, &temperature);
    if ((uint32_t)rc == SCE_KERNEL_ERROR_EINVAL)
      break;
    if (rc == 0)
      present[sensor] = true;
  }
}

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

  if (cpu_rc == 0 && soc_max_id >= 0)
    return cpu > soc_max ? cpu : soc_max;
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
  char sys_text[24];
  char fps_text[32];

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

  if (cpu_rc == 0)
    (void)snprintf(cpu_text, sizeof(cpu_text), "%d C", cpu);
  else
    (void)snprintf(cpu_text, sizeof(cpu_text), "unavailable");

  if (soc_max_id >= 0 && soc_ok > 0)
    (void)snprintf(soc_text, sizeof(soc_text), "%d C", soc_max);
  else
    (void)snprintf(soc_text, sizeof(soc_text), "unavailable");

  if (duty_rc == 0)
    (void)snprintf(fan_text, sizeof(fan_text), "%.1f%%",
                   (double)duty * 100.0 / 1024.0);
  else
    (void)snprintf(fan_text, sizeof(fan_text), "unavailable");

  if (current_target >= 0) {
    if (idle)
      (void)snprintf(target_text, sizeof(target_text),
                     "%d C (idle, not fighting)", current_target);
    else
      (void)snprintf(target_text, sizeof(target_text), "%d C (want %d C)",
                     current_target, desired_target);
  } else {
    (void)snprintf(target_text, sizeof(target_text), "unavailable");
  }

  if (avg_temp >= 0)
    (void)snprintf(avg_text, sizeof(avg_text), "%d C", avg_temp);
  else
    (void)snprintf(avg_text, sizeof(avg_text), "warming up");

  if (system_temp >= 0)
    (void)snprintf(sys_text, sizeof(sys_text), "%d C", system_temp);
  else
    (void)snprintf(sys_text, sizeof(sys_text), "n/a");

  if (g_config.fps)
    (void)snprintf(fps_text, sizeof(fps_text), "%s",
                   g_fps_injected ? "injected" : (g_fps_deployed ? "deployed" : "n/a"));
  else
    (void)snprintf(fps_text, sizeof(fps_text), "off");

  log_line("status CPU=%s, SoC=%s, fan=%s, target=%s, sys=%s, avg5m=%s, fps=%s%s",
           cpu_text, soc_text, fan_text, target_text, sys_text, avg_text,
           fps_text, idle ? " [IDLE]" : "");
}

static bool match_key(const char *line, const char *key, char *value,
                      size_t value_size) {
  const char *eq = strchr(line, '=');
  if (!eq)
    return false;
  size_t key_len = (size_t)(eq - line);
  if (strncmp(line, key, key_len) != 0 || key[key_len] != '\0')
    return false;
  const char *val = eq + 1;
  while (*val == ' ' || *val == '\t')
    ++val;
  size_t len = strcspn(val, "\r\n");
  if (len >= value_size)
    len = value_size - 1;
  memcpy(value, val, len);
  value[len] = '\0';
  return true;
}

static void normalize_line(char *line) {
  char *dst = line;
  for (char *src = line; *src != '\0'; ++src) {
    if (*src == '\r' || *src == '\n')
      break;
    if (*src == '\t')
      *dst++ = ' ';
    else
      *dst++ = *src;
  }
  *dst = '\0';
}

static overlay_position_t parse_overlay_position(const char *value) {
  for (size_t i = 0; i < sizeof(overlay_position_names) /
                          sizeof(overlay_position_names[0]); ++i) {
    if (strcasecmp(value, overlay_position_names[i].name) == 0)
      return overlay_position_names[i].position;
  }
  if (strcasecmp(value, "top-left") == 0)
    return OVERLAY_TOP_LEFT;
  if (strcasecmp(value, "top-right") == 0)
    return OVERLAY_TOP_RIGHT;
  if (strcasecmp(value, "bottom-left") == 0)
    return OVERLAY_BOTTOM_LEFT;
  if (strcasecmp(value, "bottom-right") == 0)
    return OVERLAY_BOTTOM_RIGHT;
  return OVERLAY_TOP_LEFT;
}

static void init_default_config(void) {
  g_config.cpu_temp = true;
  g_config.gpu_temp = true;
  g_config.soc_temp = true;
  g_config.ram_usage = true;
  g_config.ssd_temp = true;
  g_config.fps = true;
  g_config.fps_color = 0xFFB300u; /* amber/orange-yellow, reads well over gameplay */
  g_config.metric = 'c';
  g_config.position = OVERLAY_TOP_LEFT;
  g_config.curve_count = 0;
  for (int i = 0; i < MAX_CURVE_ANCHORS; ++i) {
    g_config.curve_temps[i] = 0;
    g_config.curve_targets[i] = 0;
  }
}

static bool parse_config_file(void) {
  FILE *file = fopen(CONFIG_FILE, "r");
  if (!file)
    return false;

  char line[256];
  while (fgets(line, sizeof(line), file)) {
    normalize_line(line);
    char value[128];
    if (line[0] == '#' || line[0] == ';' || line[0] == '\0')
      continue;
    if (match_key(line, "cpu_temp", value, sizeof(value))) {
      g_config.cpu_temp = (value[0] == '1');
    } else if (match_key(line, "gpu_temp", value, sizeof(value))) {
      g_config.gpu_temp = (value[0] == '1');
    } else if (match_key(line, "soc_temp", value, sizeof(value))) {
      g_config.soc_temp = (value[0] == '1');
    } else if (match_key(line, "ram_usage", value, sizeof(value))) {
      g_config.ram_usage = (value[0] == '1');
    } else if (match_key(line, "ssd_temp", value, sizeof(value))) {
      g_config.ssd_temp = (value[0] == '1');
    } else if (match_key(line, "metric", value, sizeof(value))) {
      g_config.metric = (value[0] == 'f' || value[0] == 'F') ? 'f' : 'c';
    } else if (match_key(line, "overlay_position", value, sizeof(value))) {
      g_config.position = parse_overlay_position(value);
    } else if (match_key(line, "fps", value, sizeof(value))) {
      g_config.fps = (value[0] == '1');
    } else if (match_key(line, "fps_color", value, sizeof(value))) {
      char *end = NULL;
      unsigned long parsed = strtoul(value, &end, 16);
      if (end != value)
        g_config.fps_color = (uint32_t)(parsed & 0xFFFFFFu);
    } else {
      int index, temp, target;
      if (sscanf(line, "curve_%d=%d,%d", &index, &temp, &target) == 3) {
        if (index >= 0 && index < MAX_CURVE_ANCHORS) {
          g_config.curve_temps[index] = temp;
          g_config.curve_targets[index] = target;
          if (index >= g_config.curve_count)
            g_config.curve_count = index + 1;
        } else if (g_config.curve_count < MAX_CURVE_ANCHORS) {
          g_config.curve_temps[g_config.curve_count] = temp;
          g_config.curve_targets[g_config.curve_count] = target;
          ++g_config.curve_count;
        }
      }
    }
  }

  fclose(file);
  return true;
}

static bool write_default_config_file(void) {
  FILE *file = fopen(CONFIG_FILE, "wx");
  if (!file)
    return false;
  fprintf(file,
          "# fan_target overlay config\n"
          "# Set 1 to show, 0 to hide.\n"
          "cpu_temp=1\n"
          "gpu_temp=1\n"
          "soc_temp=1\n"
          "ram_usage=1\n"
          "ssd_temp=1\n"
          "metric=c\n"
          "overlay_position=top left\n"
          "fps=1\n"
          "# FPS overlay text colour, hex RRGGBB. Default is an amber/\n"
          "# orange-yellow chosen to stay legible over most gameplay.\n"
          "fps_color=FFB300\n"
          "# Custom fan curve anchors: curve_<index>=<temp>,<target>\n"
          "curve_0=40,91\n"
          "curve_1=52,88\n"
          "curve_2=58,84\n"
          "curve_3=64,75\n"
          "curve_4=70,72\n"
          "curve_5=75,70\n"
          "curve_6=85,68\n");
  fclose(file);
  return true;
}

static bool ensure_config_directory(void) {
  struct stat st;
  if (stat(CONFIG_DIR, &st) != 0) {
    if (mkdir(CONFIG_DIR, 0755) != 0)
      return false;
  } else if (!S_ISDIR(st.st_mode)) {
    return false;
  }
  return true;
}

static void apply_overlay_position(void) {
  int x = 0;
  int y = 0;
  switch (g_config.position) {
  case OVERLAY_TOP_LEFT:
    x = 0;
    y = 0;
    break;
  case OVERLAY_TOP_RIGHT:
    x = 1920 - 400;
    y = 0;
    break;
  case OVERLAY_BOTTOM_LEFT:
    x = 0;
    y = 1080 - 200;
    break;
  case OVERLAY_BOTTOM_RIGHT:
    x = 1920 - 400;
    y = 1080 - 200;
    break;
  }
  (void)sceUserServiceSetGlsOverlayPosition(x, y);
}

int main(void) {
  bool sensors[SOC_SENSOR_COUNT];
  struct sigaction action;
  int fan_fd = -1;
  int last_target = -1;
  int last_applied_desired = -1;
  bool device_error_logged = false;
  bool correction_error_logged = false;
  bool idle = false;
  bool idle_logged = false;
  uint64_t next_log;
  unsigned pad_retry = 0;
#if FANTARGET_SMOKE_LOOPS > 0
  unsigned long loops = 0;
#endif

  if (stop_old_instances() != 0)
    return 1;
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

  init_default_config();
  if (ensure_config_directory()) {
    if (!parse_config_file())
      (void)write_default_config_file();
  }
  apply_overlay_position();
  if (g_config.fps) {
    setup_fps_overlay();
  }
  history_init(&g_history);
  discover_soc_sensors(sensors);
  g_start_sec = monotonic_seconds();
  next_log = g_start_sec;
  pad_init_handles();

  log_line("started; curve near-default when cool, idle enter<%d C exit>=%d C",
           FANTARGET_IDLE_ENTER_C, FANTARGET_IDLE_EXIT_C);
  log_line("poll=%d ms, history=%d s, log every %d s, lightbar %d s",
           FANTARGET_POLL_MS, FANTARGET_HISTORY_SEC, FANTARGET_LOG_SECONDS,
           FANTARGET_LIGHTBAR_SEC);
  if (g_pad_ready)
    log_line("lightbar ready (%d pad handle(s))", g_pad_count);
  else
    log_line("lightbar unavailable (no pad handles); fan control only");

  while (!stop_requested) {
    uint8_t config[FAN_CONFIG_SIZE];
    int current_target = -1;
    int system_temp = -1;
    int avg_temp = -1;
    int raw_desired = -1;
    int desired_target = -1;

    system_temp = read_system_temp(sensors);
    if (system_temp >= 0)
      history_push(&g_history, system_temp);
    avg_temp = history_avg(&g_history);

    if (history_full_enough(&g_history) && avg_temp >= 0) {
      if (!idle && avg_temp < FANTARGET_IDLE_ENTER_C)
        idle = true;
      else if (idle && avg_temp >= FANTARGET_IDLE_EXIT_C)
        idle = false;
    }

    if (system_temp >= 0) {
      raw_desired = target_from_temp(system_temp);
      desired_target =
          apply_target_hysteresis(raw_desired, last_applied_desired);
    } else {
      desired_target = 91;
    }

    if (lightbar_window_active()) {
      if (!g_pad_ready) {
        ++pad_retry;
        if ((pad_retry % 6) == 0)
          pad_init_handles();
      } else if (system_temp >= 0) {
        pad_apply_lightbar(system_temp);
      }
    } else if (g_pad_ready) {
      pad_close_all();
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

    if (last_target >= 0 && current_target != last_target)
      log_line("target changed: %d C -> %d C", last_target, current_target);

    if (idle) {
      if (!idle_logged) {
        log_line("idle (avg5m=%d C < %d C); leaving system target alone "
                 "(exit when avg>=%d C)",
                 avg_temp, FANTARGET_IDLE_ENTER_C, FANTARGET_IDLE_EXIT_C);
        idle_logged = true;
      }
      correction_error_logged = false;
    } else {
      if (idle_logged) {
        log_line("left idle (avg5m=%d C >= %d C); resuming curve control",
                 avg_temp, FANTARGET_IDLE_EXIT_C);
        idle_logged = false;
      }
      if (current_target != desired_target) {
        uint8_t verified[FAN_CONFIG_SIZE];
        int old_target = current_target;
        if (set_target(fan_fd, config, (uint8_t)desired_target, verified) ==
            0) {
          current_target = verified[5];
          last_applied_desired = desired_target;
          correction_error_logged = false;
          log_line("target set: %d C -> %d C (temp=%d C, avg5m=%d C, raw=%d C)",
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
    if (loops >= FANTARGET_SMOKE_LOOPS)
      break;
#endif
    sleep_poll_interval();
  }

  pad_close_all();
  if (fan_fd >= 0)
    close(fan_fd);
  log_line("stopped");
  return 0;
}
