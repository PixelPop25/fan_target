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
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>

#include "fps_elf_blob.h"
#include "overlay_elf_blob.h"
#include "icon_blob.h"
#include "pic1_blob.h"
#include "elfldr.h"
#include "webui_html.h"

#define PROCESS_NAME "fan_target_pxp.elf"
#define CONFIG_DIR "/data/fan_target_pxp"
#define CONFIG_FILE CONFIG_DIR "/config.ini"
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
#define FAN_TARGET_VERSION "1.0"
#define WEBUI_PORT 25500
#define TITLE_ID "PXPFT0001"
#define APP_INSTALL_DIR "/user/app/" TITLE_ID
#define APP_SYS_DIR APP_INSTALL_DIR "/sce_sys"
/* 66048 = Web Based Media App (Media tab) */
#define APP_CATEGORY_MEDIA 66048
#define FAN_TARGET_GREETING "Greetings by Issu.\n\nFan target " FAN_TARGET_VERSION

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

typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int flags);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *title_id);
int sceAppInstUtilAppInstallTitleDir(const char *title_id, const char *dir, void *opt);

int sceKernelGetCpuTemperature(int *temperature);
int sceKernelGetSocSensorTemperature(int sensor, int *temperature);
int sceKernelGetCurrentFanDuty(uint16_t *duty, uint64_t *chassis);
int sceUserServiceGetGlsOverlayPosition(int *x, int *y);

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
  uint32_t fps_color; 
  char metric;
  overlay_position_t position;
  int curve_temps[MAX_CURVE_ANCHORS];
  int curve_targets[MAX_CURVE_ANCHORS];
  int curve_count;
    int temp_green_max;   /* ≤ this → green */
  int temp_yellow_max;  /* ≤ this → yellow */
  int temp_orange_max;  /* ≤ this → orange; above → red */
  uint32_t temp_color_green;
  uint32_t temp_color_yellow;
  uint32_t temp_color_orange;
  uint32_t temp_color_red;
    int fps_red_max;      /* ≤ this → red */
  int fps_yellow_max;   /* ≤ this → yellow */
  int fps_green_yellow_max; /* ≤ this → green-yellow; above → cyan-green */
  uint32_t fps_color_red;
  uint32_t fps_color_yellow;
  uint32_t fps_color_green_yellow;
  uint32_t fps_color_high;
  bool fan_control;
    bool lightbar_enable;
  int lightbar_duration_sec;   
  bool lightbar_flash_on_red;  /* flash when ≥ lightbar_flash_temp */
  int lightbar_flash_temp;     /* default 75 */
  int lightbar_blue_max;       /* < this → blue */
  int lightbar_green_min;
  int lightbar_green_max;
  int lightbar_orange_min;
  int lightbar_orange_max;
  int lightbar_red_min;        /* ≥ this → red */
  uint32_t lightbar_color_blue;
  uint32_t lightbar_color_green;
  uint32_t lightbar_color_orange;
  uint32_t lightbar_color_red;
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
static atomic_int g_lightbar_temp;
static volatile int g_lightbar_stop = 0;
static pthread_t g_lightbar_thread;
static int g_lightbar_thread_started = 0;


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
static bool match_key(const char *line, const char *key, char *value,
                      size_t value_size);
static void normalize_line(char *line);
static overlay_position_t parse_overlay_position(const char *value);
static void init_default_config(void);
static bool parse_config_file(void);
static bool write_default_config_file(void);
static bool ensure_config_directory(void);

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

  if (temp_c < g_config.lightbar_blue_max)
    return LB_BLUE;
  if (temp_c >= g_config.lightbar_green_min && temp_c <= g_config.lightbar_green_max)
    return LB_GREEN;
  if (temp_c >= g_config.lightbar_orange_min && temp_c <= g_config.lightbar_orange_max)
    return LB_ORANGE;
  if (temp_c >= g_config.lightbar_red_min)
    return LB_RED;
  return previous != LB_NONE ? previous : LB_BLUE;
}

static void color_u32_to_rgb(uint32_t c, ScePadLightBar *out) {
  out->r = (uint8_t)((c >> 16) & 0xFF);
  out->g = (uint8_t)((c >> 8) & 0xFF);
  out->b = (uint8_t)(c & 0xFF);
  out->reserved = 0;
}

static void band_to_rgb(lightbar_band_t band, ScePadLightBar *out) {
  switch (band) {
  case LB_RED:
    color_u32_to_rgb(g_config.lightbar_color_red, out);
    break;
  case LB_ORANGE:
    color_u32_to_rgb(g_config.lightbar_color_orange, out);
    break;
  case LB_GREEN:
    color_u32_to_rgb(g_config.lightbar_color_green, out);
    break;
  case LB_BLUE:
  default:
    color_u32_to_rgb(g_config.lightbar_color_blue, out);
    break;
  }
}

static uint64_t monotonic_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return (uint64_t)now.tv_sec;
}

static bool g_fps_injected = false;
static bool g_overlay_ok = false;


extern int find_pid_by_name(const char *name) __attribute__((weak));
extern _Bool Inject_Toolbox(int pid, unsigned char *elf) __attribute__((weak));

static bool inject_blob_to_pid(int pid, const unsigned char *blob, unsigned int len,
                               const char *tag) {
  if (!blob || len == 0)
    return false;

  if (pid > 0) {
    if (Inject_Toolbox && Inject_Toolbox(pid, (unsigned char *)blob)) {
      log_line("%s: Inject_Toolbox ok pid=%d (%u)", tag, pid, len);
      return true;
    }
    /* Direct ptrace inject into target (ShellUI / game) */
    if (elfldr_exec((pid_t)pid, -1, (uint8_t *)blob) == 0) {
      log_line("%s: elfldr_exec ok pid=%d (%u)", tag, pid, len);
      return true;
    }
    log_line("%s: elfldr_exec failed pid=%d — trying socket fallback", tag, pid);
  }

  return inject_blob_via_elfldr(blob, len, tag);
}

static bool inject_blob_via_elfldr(const unsigned char *blob, unsigned int len,
                                   const char *tag) {
  if (!blob || len == 0)
    return false;

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return false;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ELFLDR_PORT);
  if (inet_pton(AF_INET, ELFLDR_HOST, &addr.sin_addr) != 1) {
    close(sock);
    return false;
  }

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
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

__attribute__((weak)) int sceKernelGetProcessName(int pid, char *name);

static int get_shellui_pid(void) {
  pid_t p = elfldr_find_pid(SHELLUI_PROC_NAME);
  if (p > 0)
    return (int)p;
  if (!sceKernelGetProcessName)
    return -1;
  char name[256];
  for (int pid = 1; pid <= 12000; ++pid) {
    memset(name, 0, sizeof(name));
    if (sceKernelGetProcessName(pid, name) != 0)
      continue;
    if (strcmp(name, SHELLUI_PROC_NAME) == 0)
      return pid;
  }
  return -1;
}

static int get_game_pid(void) {
  extern int sceSystemServiceGetAppIdOfBigApp(void) __attribute__((weak));
  if (sceSystemServiceGetAppIdOfBigApp) {
    int app = sceSystemServiceGetAppIdOfBigApp();
    if (app > 0)
      return app;
  }
  if (find_pid_by_name) {
    /* Common big-app process names when available via toolbox. */
    static const char *candidates[] = {
      "eboot", "SceApplication", "CUSA", NULL
    };
    for (int i = 0; candidates[i]; ++i) {
      int pid = find_pid_by_name(candidates[i]);
      if (pid > 0)
        return pid;
    }
  }
  if (!sceKernelGetProcessName)
    return -1;
  static const char *skip[] = {
    "SceShellUI", "SceShellCore", "SceSysCore", "SceVshDaemon",
    "SceRpcAgent", "SceDiscPlayer", "ScePartyDaemon", "SceRemotePlay",
    "SceGameLiveStreaming", "SceSpCore", "SceAudioSystem", PROCESS_NAME,
    NULL
  };
  char name[256];
  for (int pid = 1; pid <= 12000; ++pid) {
    memset(name, 0, sizeof(name));
    if (sceKernelGetProcessName(pid, name) != 0 || name[0] == '\0')
      continue;
    int ignored = 0;
    for (int i = 0; skip[i]; ++i) {
      if (strcmp(name, skip[i]) == 0) {
        ignored = 1;
        break;
      }
    }
    if (ignored)
      continue;
    if (strncmp(name, "SceShell", 8) == 0 || strncmp(name, "SceSys", 6) == 0)
      continue;
    return pid;
  }
  return -1;
}

static void setup_shellui_overlay(void) {
  extern const unsigned char overlay_elf_blob[];
  extern const unsigned int overlay_elf_blob_len;
  if (overlay_elf_blob_len == 0) {
    log_line("overlay_elf blob empty");
    return;
  }
  int pid = get_shellui_pid();
  if (pid <= 0 && find_pid_by_name)
    pid = find_pid_by_name(SHELLUI_PROC_NAME);
  if (pid <= 0)
    log_line("SceShellUI pid not found — overlay inject deferred");
  else
    log_line("overlay inject target SceShellUI pid=%d (%u bytes)", pid,
             overlay_elf_blob_len);
  g_overlay_ok = inject_blob_to_pid(pid, overlay_elf_blob, overlay_elf_blob_len,
                                   "overlay_elf");
}

static void setup_fps_overlay(void) {
    setup_shellui_overlay();

  if (fps_elf_blob_len == 0)
    return;
  int game_pid = get_game_pid();
  g_fps_injected = inject_blob_to_pid(game_pid, fps_elf_blob, fps_elf_blob_len,
                                     "fps_elf");
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
  if (!g_config.lightbar_enable)
    return false;
  if (g_config.lightbar_duration_sec <= 0)
    return true; 
  uint64_t now = monotonic_seconds();
  if (g_start_sec == 0 || now < g_start_sec)
    return true;
  return (now - g_start_sec) < (uint64_t)g_config.lightbar_duration_sec;
}


static void *lightbar_thread_main(void *arg) {
  (void)arg;
  ScePadLightBar color;
  bool flash_off = false;
  lightbar_band_t last = LB_NONE;

  while (!g_lightbar_stop && !stop_requested) {
    if (!g_config.lightbar_enable || !g_pad_ready) {
      struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
      nanosleep(&ts, NULL);
      continue;
    }
    if (!lightbar_window_active()) {
      struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
      nanosleep(&ts, NULL);
      continue;
    }

    int temp = atomic_load(&g_lightbar_temp);
    lightbar_band_t band = band_from_temp(temp, last);
    bool should_flash = g_config.lightbar_flash_on_red &&
                        temp >= g_config.lightbar_flash_temp &&
                        band == LB_RED;

    if (should_flash) {
      flash_off = !flash_off;
      if (flash_off) {
        color.r = color.g = color.b = 0;
        color.reserved = 0;
      } else {
        band_to_rgb(LB_RED, &color);
      }
      for (int i = 0; i < g_pad_count; ++i) {
        if (g_pad_handles[i] >= 0)
          (void)scePadSetLightBar(g_pad_handles[i], &color);
      }
      last = LB_RED;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 400000000L };
      nanosleep(&ts, NULL);
      continue;
    }

    flash_off = false;
    if (band != last) {
      band_to_rgb(band, &color);
      for (int i = 0; i < g_pad_count; ++i) {
        if (g_pad_handles[i] >= 0)
          (void)scePadSetLightBar(g_pad_handles[i], &color);
      }
      last = band;
    }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
    nanosleep(&ts, NULL);
  }
  return NULL;
}

static void lightbar_thread_start(void) {
  if (g_lightbar_thread_started)
    return;
  g_lightbar_stop = 0;
  if (pthread_create(&g_lightbar_thread, NULL, lightbar_thread_main, NULL) == 0)
    g_lightbar_thread_started = 1;
}

static void lightbar_thread_stop(void) {
  if (!g_lightbar_thread_started)
    return;
  g_lightbar_stop = 1;
  (void)pthread_join(g_lightbar_thread, NULL);
  g_lightbar_thread_started = 0;
}

static void pad_apply_lightbar(int system_temp) {
    if (system_temp >= 0)
    atomic_store(&g_lightbar_temp, system_temp);
  if (!g_lightbar_thread_started && g_config.lightbar_enable)
    lightbar_thread_start();
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


static void send_greeting_notification(void) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message),
           "Greetings by Issu.\n\nFan target %s", FAN_TARGET_VERSION);
  (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
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


static bool parse_hex_color(const char *value, uint32_t *out) {
  if (!value || !out) return false;
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 16);
  if (end == value) return false;
  *out = (uint32_t)(parsed & 0xFFFFFFu);
  return true;
}

static void init_default_config(void) {
  g_config.cpu_temp = true;
  g_config.gpu_temp = true;
  g_config.soc_temp = true;
  g_config.ram_usage = true;
  g_config.ssd_temp = true;
  g_config.fps = true;
  g_config.fps_color = 0xFFB300u;
  g_config.metric = 'c';
  g_config.position = OVERLAY_TOP_LEFT;
  g_config.curve_count = 0;
  for (int i = 0; i < MAX_CURVE_ANCHORS; ++i) {
    g_config.curve_temps[i] = 0;
    g_config.curve_targets[i] = 0;
  }
    g_config.temp_green_max = 50;
  g_config.temp_yellow_max = 60;
  g_config.temp_orange_max = 70;
  g_config.temp_color_green = 0x33F24Du;
  g_config.temp_color_yellow = 0xF2E626u;
  g_config.temp_color_orange = 0xFF5C1Au;
  g_config.temp_color_red = 0xFF261Fu;
    g_config.fps_red_max = 24;
  g_config.fps_yellow_max = 29;
  g_config.fps_green_yellow_max = 45;
  g_config.fps_color_red = 0xFF261Fu;
  g_config.fps_color_yellow = 0xF2E626u;
  g_config.fps_color_green_yellow = 0x8CF23Du;
  g_config.fps_color_high = 0x26F2C2u; /* cyan-green, visible over gameplay */
    g_config.fan_control = true;
  g_config.lightbar_enable = true;
  g_config.lightbar_duration_sec = 300;
  g_config.lightbar_flash_on_red = true;
  g_config.lightbar_flash_temp = 75;
  g_config.lightbar_blue_max = 52;
  g_config.lightbar_green_min = 55;
  g_config.lightbar_green_max = 62;
  g_config.lightbar_orange_min = 64;
  g_config.lightbar_orange_max = 70;
  g_config.lightbar_red_min = 72;
  g_config.lightbar_color_blue = 0x0050FFu;
  g_config.lightbar_color_green = 0x00DC28u;
  g_config.lightbar_color_orange = 0xFF6400u;
  g_config.lightbar_color_red = 0xFF0000u;
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
      parse_hex_color(value, &g_config.fps_color);
    } else if (match_key(line, "temp_green_max", value, sizeof(value))) {
      g_config.temp_green_max = atoi(value);
    } else if (match_key(line, "temp_yellow_max", value, sizeof(value))) {
      g_config.temp_yellow_max = atoi(value);
    } else if (match_key(line, "temp_orange_max", value, sizeof(value))) {
      g_config.temp_orange_max = atoi(value);
    } else if (match_key(line, "temp_color_green", value, sizeof(value))) {
      parse_hex_color(value, &g_config.temp_color_green);
    } else if (match_key(line, "temp_color_yellow", value, sizeof(value))) {
      parse_hex_color(value, &g_config.temp_color_yellow);
    } else if (match_key(line, "temp_color_orange", value, sizeof(value))) {
      parse_hex_color(value, &g_config.temp_color_orange);
    } else if (match_key(line, "temp_color_red", value, sizeof(value))) {
      parse_hex_color(value, &g_config.temp_color_red);
    } else if (match_key(line, "fps_red_max", value, sizeof(value))) {
      g_config.fps_red_max = atoi(value);
    } else if (match_key(line, "fps_yellow_max", value, sizeof(value))) {
      g_config.fps_yellow_max = atoi(value);
    } else if (match_key(line, "fps_green_yellow_max", value, sizeof(value))) {
      g_config.fps_green_yellow_max = atoi(value);
    } else if (match_key(line, "fps_color_red", value, sizeof(value))) {
      parse_hex_color(value, &g_config.fps_color_red);
    } else if (match_key(line, "fps_color_yellow", value, sizeof(value))) {
      parse_hex_color(value, &g_config.fps_color_yellow);
    } else if (match_key(line, "fps_color_green_yellow", value, sizeof(value))) {
      parse_hex_color(value, &g_config.fps_color_green_yellow);
    } else if (match_key(line, "fps_color_high", value, sizeof(value))) {
      parse_hex_color(value, &g_config.fps_color_high);
    } else if (match_key(line, "fan_control", value, sizeof(value))) {
      g_config.fan_control = (value[0] == '1');
    } else if (match_key(line, "lightbar_enable", value, sizeof(value))) {
      g_config.lightbar_enable = (value[0] == '1');
    } else if (match_key(line, "lightbar_duration_sec", value, sizeof(value))) {
      g_config.lightbar_duration_sec = atoi(value);
    } else if (match_key(line, "lightbar_flash_on_red", value, sizeof(value))) {
      g_config.lightbar_flash_on_red = (value[0] == '1');
    } else if (match_key(line, "lightbar_flash_temp", value, sizeof(value))) {
      g_config.lightbar_flash_temp = atoi(value);
    } else if (match_key(line, "lightbar_blue_max", value, sizeof(value))) {
      g_config.lightbar_blue_max = atoi(value);
    } else if (match_key(line, "lightbar_green_min", value, sizeof(value))) {
      g_config.lightbar_green_min = atoi(value);
    } else if (match_key(line, "lightbar_green_max", value, sizeof(value))) {
      g_config.lightbar_green_max = atoi(value);
    } else if (match_key(line, "lightbar_orange_min", value, sizeof(value))) {
      g_config.lightbar_orange_min = atoi(value);
    } else if (match_key(line, "lightbar_orange_max", value, sizeof(value))) {
      g_config.lightbar_orange_max = atoi(value);
    } else if (match_key(line, "lightbar_red_min", value, sizeof(value))) {
      g_config.lightbar_red_min = atoi(value);
    } else if (match_key(line, "lightbar_color_blue", value, sizeof(value))) {
      parse_hex_color(value, &g_config.lightbar_color_blue);
    } else if (match_key(line, "lightbar_color_green", value, sizeof(value))) {
      parse_hex_color(value, &g_config.lightbar_color_green);
    } else if (match_key(line, "lightbar_color_orange", value, sizeof(value))) {
      parse_hex_color(value, &g_config.lightbar_color_orange);
    } else if (match_key(line, "lightbar_color_red", value, sizeof(value))) {
      parse_hex_color(value, &g_config.lightbar_color_red);
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
          "# fan_target_pxp config  (Fan target %s)\n"
          "# Path: /data/fan_target_pxp/config.ini\n"
          "#\n"
          "# Overlay visibility (1=show, 0=hide)\n"
          "cpu_temp=1\n"
          "gpu_temp=1\n"
          "soc_temp=1\n"
          "ram_usage=1\n"
          "ssd_temp=1\n"
          "fps=1\n"
          "metric=c\n"
          "overlay_position=top left\n"
          "#\n"
          "# === Temperature colour thresholds (°C) ===\n"
          "# ≤ temp_green_max          → green\n"
          "# ≤ temp_yellow_max         → yellow\n"
          "# ≤ temp_orange_max         → orange\n"
          "# >  temp_orange_max        → red\n"
          "temp_green_max=50\n"
          "temp_yellow_max=60\n"
          "temp_orange_max=70\n"
          "temp_color_green=33F24D\n"
          "temp_color_yellow=F2E626\n"
          "temp_color_orange=FF5C1A\n"
          "temp_color_red=FF261F\n"
          "#\n"
          "# === FPS colour thresholds ===\n"
          "# ≤ fps_red_max             → red\n"
          "# ≤ fps_yellow_max          → yellow\n"
          "# ≤ fps_green_yellow_max    → green-yellow\n"
          "# >  fps_green_yellow_max   → cyan-green (high FPS)\n"
          "fps_red_max=24\n"
          "fps_yellow_max=29\n"
          "fps_green_yellow_max=45\n"
          "fps_color_red=FF261F\n"
          "fps_color_yellow=F2E626\n"
          "fps_color_green_yellow=8CF23D\n"
          "fps_color_high=26F2C2\n"
          "# legacy single fps_color (unused when band colours present)\n"
          "fps_color=FFB300\n"
          "#\n"
          "# === Fan curve anchors: curve_<n>=<temp_c>,<target_c> ===\n"
          "# Target is the fan controller setpoint (°C). Lower = more aggressive.\n"
          "curve_0=40,91\n"
          "curve_1=52,88\n"
          "curve_2=58,84\n"
          "curve_3=64,75\n"
          "curve_4=70,72\n"
          "curve_5=75,70\n"
          "curve_6=85,68\n"
          "#\n"
          "# === Fan control (0 = overlays/lightbar/FPS only) ===\n"
          "fan_control=1\n"
          "# === Lightbar ===\n"
          "lightbar_enable=1\n"
          "lightbar_duration_sec=300\n"
          "lightbar_flash_on_red=1\n"
          "lightbar_flash_temp=75\n"
          "lightbar_blue_max=52\n"
          "lightbar_green_min=55\n"
          "lightbar_green_max=62\n"
          "lightbar_orange_min=64\n"
          "lightbar_orange_max=70\n"
          "lightbar_red_min=72\n"
          "lightbar_color_blue=0050FF\n"
          "lightbar_color_green=00DC28\n"
          "lightbar_color_orange=FF6400\n"
          "lightbar_color_red=FF0000\n",
          FAN_TARGET_VERSION);
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


static atomic_int g_config_reload_req;
static pthread_t g_web_thread;
static int g_web_thread_started;
static volatile int g_web_stop;

static void http_send(int fd, const char *status, const char *ctype, const char *body, size_t body_len) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                   status, ctype, body_len);
  if (n > 0)
    (void)send(fd, hdr, (size_t)n, 0);
  if (body && body_len)
    (void)send(fd, body, body_len, 0);
}

static int read_file_buf(const char *path, char *buf, size_t cap, size_t *out_len) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  if (out_len)
    *out_len = n;
  return 0;
}

static int write_file_buf(const char *path, const char *buf, size_t len) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  size_t n = fwrite(buf, 1, len, f);
  fclose(f);
  return n == len ? 0 : -1;
}

static int write_bytes(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  size_t n = fwrite(data, 1, len, f);
  fclose(f);
  return n == len ? 0 : -1;
}

static int write_text(const char *path, const char *s) {
  return write_bytes(path, s, strlen(s));
}

/* Media-tab home tile → browser deeplink to config UI (no PKG). */
static void install_home_tile(void) {
  char path[256];

  mkdir("/user/app", 0755);
  mkdir(APP_INSTALL_DIR, 0755);
  mkdir(APP_SYS_DIR, 0755);

  if (icon_blob_len > 0)
    (void)write_bytes(APP_SYS_DIR "/icon0.png", icon_blob, icon_blob_len);
  if (pic1_blob_len > 0)
    (void)write_bytes(APP_SYS_DIR "/pic1.png", pic1_blob, pic1_blob_len);

  {
    char param[1024];
    snprintf(param, sizeof(param),
             "{\n"
             "  \"titleId\": \"%s\",\n"
             "  \"contentId\": \"IV9999-%s_00-FANTARGETPXP000\",\n"
             "  \"contentVersion\": \"01.000.000\",\n"
             "  \"masterVersion\": \"01.00\",\n"
             "  \"applicationCategoryType\": %d,\n"
             "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
             "  \"localizedParameters\": {\n"
             "    \"defaultLanguage\": \"en-US\",\n"
             "    \"en-US\": { \"titleName\": \"Fan Target\" }\n"
             "  }\n"
             "}\n",
             TITLE_ID, TITLE_ID, APP_CATEGORY_MEDIA, WEBUI_PORT);
    (void)write_text(APP_SYS_DIR "/param.json", param);
  }

  snprintf(path, sizeof(path), "%s/WEB_UI.txt", CONFIG_DIR);
  {
    char note[128];
    snprintf(note, sizeof(note), "http://127.0.0.1:%d/\n", WEBUI_PORT);
    (void)write_text(path, note);
  }

  {
    int err = sceAppInstUtilInitialize();
    if (err != 0) {
      log_line("AppInstUtil init failed: 0x%08x", (unsigned)err);
      return;
    }
    (void)sceAppInstUtilAppUnInstall(TITLE_ID);
    err = sceAppInstUtilAppInstallTitleDir(TITLE_ID, "/user/app/", 0);
    if (err == 0) {
      log_line("home tile %s installed (Media)", TITLE_ID);
      notify_request_t nreq;
      memset(&nreq, 0, sizeof(nreq));
      snprintf(nreq.message, sizeof(nreq.message),
               "Fan Target tile installed\nMedia · %s", TITLE_ID);
      (void)sceKernelSendNotificationRequest(0, &nreq, sizeof(nreq), 0);
    } else {
      log_line("home tile install failed: 0x%08x", (unsigned)err);
    }
  }
}

static void handle_client(int cfd) {
  char req[4096];
  ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
  if (n <= 0) {
    close(cfd);
    return;
  }
  req[n] = '\0';

  char method[16], path[256];
  method[0] = path[0] = '\0';
  sscanf(req, "%15s %255s", method, path);

  if (strcmp(method, "OPTIONS") == 0) {
    http_send(cfd, "204 No Content", "text/plain", "", 0);
    close(cfd);
    return;
  }

  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    http_send(cfd, "200 OK", "text/html; charset=utf-8", WEBUI_HTML, strlen(WEBUI_HTML));
    close(cfd);
    return;
  }

  if (strcmp(path, "/api/config") == 0 && strcmp(method, "GET") == 0) {
    char body[16384];
    size_t len = 0;
    if (read_file_buf(CONFIG_FILE, body, sizeof(body), &len) != 0) {
      const char *empty = "# no config yet\n";
      http_send(cfd, "200 OK", "text/plain; charset=utf-8", empty, strlen(empty));
    } else {
      http_send(cfd, "200 OK", "text/plain; charset=utf-8", body, len);
    }
    close(cfd);
    return;
  }

  if (strcmp(path, "/api/config") == 0 && strcmp(method, "POST") == 0) {
    char *body = strstr(req, "\r\n\r\n");
    if (!body) {
      http_send(cfd, "400 Bad Request", "text/plain", "no body", 7);
      close(cfd);
      return;
    }
    body += 4;
    size_t body_len = (size_t)(n - (body - req));
    /* drain remaining if Content-Length larger */
    const char *cl = strcasestr(req, "Content-Length:");
    size_t want = body_len;
    if (cl) {
      want = (size_t)atoi(cl + 15);
    }
    char *acc = (char *)malloc(want + 1);
    if (!acc) {
      http_send(cfd, "500 Internal Server Error", "text/plain", "oom", 3);
      close(cfd);
      return;
    }
    size_t got = body_len;
    if (got > want)
      got = want;
    memcpy(acc, body, got);
    while (got < want) {
      ssize_t r = recv(cfd, acc + got, want - got, 0);
      if (r <= 0)
        break;
      got += (size_t)r;
    }
    acc[got] = '\0';
    if (write_file_buf(CONFIG_FILE, acc, got) != 0) {
      free(acc);
      http_send(cfd, "500 Internal Server Error", "text/plain", "write failed", 12);
      close(cfd);
      return;
    }
    free(acc);
    atomic_store(&g_config_reload_req, 1);
    http_send(cfd, "200 OK", "text/plain", "ok", 2);
    close(cfd);
    return;
  }

  if (strcmp(path, "/api/reload") == 0 && strcmp(method, "POST") == 0) {
    atomic_store(&g_config_reload_req, 1);
    http_send(cfd, "200 OK", "text/plain", "ok", 2);
    close(cfd);
    return;
  }

  http_send(cfd, "404 Not Found", "text/plain", "not found", 9);
  close(cfd);
}

static void *web_thread_main(void *arg) {
  (void)arg;
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0)
    return NULL;
  int on = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(WEBUI_PORT);
  if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(sfd);
    return NULL;
  }
  listen(sfd, 8);
  while (!g_web_stop && !stop_requested) {
    struct pollfd pfd = { .fd = sfd, .events = POLLIN };
    int pr = poll(&pfd, 1, 500);
    if (pr <= 0)
      continue;
    int cfd = accept(sfd, NULL, NULL);
    if (cfd >= 0)
      handle_client(cfd);
  }
  close(sfd);
  return NULL;
}

static void web_thread_start(void) {
  if (g_web_thread_started)
    return;
  g_web_stop = 0;
  atomic_store(&g_config_reload_req, 0);
  if (pthread_create(&g_web_thread, NULL, web_thread_main, NULL) == 0)
    g_web_thread_started = 1;
}

static void web_thread_stop(void) {
  if (!g_web_thread_started)
    return;
  g_web_stop = 1;
  (void)pthread_join(g_web_thread, NULL);
  g_web_thread_started = 0;
}


int main(void) {
  bool sensors[SOC_SENSOR_COUNT];
  struct sigaction action;
  int fan_fd = -1;
  int last_applied_desired = -1;
  bool device_error_logged = false;
  bool correction_error_logged = false;
  bool idle = false;
  unsigned pad_retry = 0;

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
  if (g_config.fps || g_config.cpu_temp || g_config.gpu_temp ||
      g_config.soc_temp || g_config.ram_usage || g_config.ssd_temp) {
    setup_fps_overlay();
  }
  atomic_store(&g_lightbar_temp, -1);
  history_init(&g_history);
  discover_soc_sensors(sensors);
  g_start_sec = monotonic_seconds();
  pad_init_handles();

  install_home_tile();
  web_thread_start();
  send_greeting_notification();
  log_line("fan_target %s started (fan_control=%d lightbar=%ds)",
           FAN_TARGET_VERSION, g_config.fan_control,
           g_config.lightbar_duration_sec);

  while (!stop_requested) {
    if (atomic_exchange(&g_config_reload_req, 0)) {
      init_default_config();
      (void)parse_config_file();
      log_line("config reloaded");
    }
    if (!g_overlay_ok && (g_config.fps || g_config.cpu_temp || g_config.gpu_temp ||
                          g_config.soc_temp || g_config.ram_usage || g_config.ssd_temp)) {
      static int overlay_retry_countdown;
      if (++overlay_retry_countdown >= 5) {
        overlay_retry_countdown = 0;
        setup_shellui_overlay();
      }
    }
    if (!g_fps_injected && g_config.fps) {
      static int fps_retry_countdown;
      if (++fps_retry_countdown >= 8) {
        fps_retry_countdown = 0;
        int gp = get_game_pid();
        if (gp > 0)
          g_fps_injected = inject_blob_to_pid(gp, fps_elf_blob, fps_elf_blob_len, "fps_elf");
      }
    }
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

    if (g_config.fan_control) {
      if (fan_fd < 0) {
        fan_fd = open(FAN_DEVICE, FAN_OPEN_FLAGS);
        if (fan_fd < 0) {
          if (!device_error_logged) {
            log_line("fan controller unavailable");
            device_error_logged = true;
          }
          goto wait_for_next_poll;
        }
        device_error_logged = false;
      }

      if (get_fan_config(fan_fd, config) != 0) {
        close(fan_fd);
        fan_fd = -1;
        goto wait_for_next_poll;
      }
      current_target = config[5];

      if (!idle && current_target != desired_target) {
        uint8_t verified[FAN_CONFIG_SIZE];
        if (set_target(fan_fd, config, (uint8_t)desired_target, verified) == 0) {
          current_target = verified[5];
          last_applied_desired = desired_target;
          correction_error_logged = false;
        } else if (!correction_error_logged) {
          log_line("fan target set failed (%d -> %d)", current_target, desired_target);
          correction_error_logged = true;
          close(fan_fd);
          fan_fd = -1;
        }
      } else if (!idle) {
        last_applied_desired = desired_target;
      }
    }

  wait_for_next_poll:
    sleep_poll_interval();
  }

  web_thread_stop();
  lightbar_thread_stop();
  pad_close_all();
  if (fan_fd >= 0)
    close(fan_fd);
  log_line("stopped");
  return 0;
}
