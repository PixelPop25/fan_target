/* overlay_elf — runs inside SceShellUI
 * - Reads CPU, SoC, SSD temps & RAM usage in-process (no disk I/O on hot path)
 * - Receives game FPS via UDP on 127.0.0.1:29028 from fps_elf
 * - Draws PUI labels in configured screen corner (top-left, top-right, bottom-left, bottom-right)
 * - Uses mono_thread_attach to ensure thread-safe Mono execution inside SceShellUI
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sysctl.h>

#define FPS_UDP_PORT 29028

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoImage MonoImage;
typedef struct _MonoObject MonoObject;
typedef struct _MonoString MonoString;
typedef struct _MonoClass MonoClass;
typedef struct _MonoMethod MonoMethod;
typedef struct _MonoProperty MonoProperty;

static MonoDomain *(*mono_get_root_domain)(void);
static MonoString *(*mono_string_new)(MonoDomain *, const char *);
static MonoClass *(*mono_class_from_name)(MonoImage *, const char *, const char *);
static MonoImage *(*mono_image_loaded)(const char *);
static MonoObject *(*mono_object_new)(MonoDomain *, MonoClass *);
static void (*mono_runtime_object_init)(MonoObject *);
static MonoMethod *(*mono_class_get_method_from_name)(MonoClass *, const char *, int);
static MonoObject *(*mono_runtime_invoke)(MonoMethod *, void *, void **, MonoObject **);
static MonoProperty *(*mono_class_get_property_from_name)(MonoClass *, const char *);
static MonoMethod *(*mono_property_get_set_method)(MonoProperty *);
static MonoMethod *(*mono_property_get_get_method)(MonoProperty *);
static void *(*mono_object_unbox)(MonoObject *);
static void *(*mono_thread_attach)(MonoDomain *);

static int (*sceKernelDlsym_fn)(int, const char *, void **);
static int (*sceKernelLoadStartModule_fn)(const char *, int, const void *, int, void *, int *);
static int (*sceKernelGetCpuTemperature)(int *);
static int (*sceKernelGetSocSensorTemperature)(int, int *);

static MonoDomain *Root_Domain;
static MonoImage *pui_img;
static MonoObject *rootWidget;
static MonoObject *font;

static MonoObject *lbl_cpu_name, *lbl_cpu_val;
static MonoObject *lbl_soc_name, *lbl_soc_val;
static MonoObject *lbl_ssd_name, *lbl_ssd_val;
static MonoObject *lbl_ram_name, *lbl_ram_val;
static MonoObject *lbl_fps_name, *lbl_fps_val;

static double g_fps = 0.0;
static uint64_t g_last_fps_time = 0;
static bool g_widgets_created = false;

typedef enum {
  POS_TOP_LEFT = 0,
  POS_TOP_RIGHT,
  POS_BOTTOM_LEFT,
  POS_BOTTOM_RIGHT
} overlay_pos_t;

struct OverlayConfig {
  bool show_cpu = true;
  bool show_soc = true;
  bool show_ssd = true;
  bool show_ram = true;
  bool show_fps = true;
  char metric = 'c';
  overlay_pos_t position = POS_TOP_LEFT;
};

static OverlayConfig g_config;

/* ---- colour thresholds (RGBA 0-1 for UIColor) ---- */
struct rgba { float r, g, b, a; };

static rgba color_for_temp(int c) {
  if (c < 0) return {1.0f, 1.0f, 1.0f, 1.0f};
  if (c <= 50) return {0.20f, 0.95f, 0.30f, 1.0f};   /* green */
  if (c <= 60) return {0.95f, 0.90f, 0.15f, 1.0f};   /* yellow */
  if (c <= 70) return {1.00f, 0.55f, 0.10f, 1.0f};   /* orange */
  return {1.00f, 0.15f, 0.12f, 1.0f};                 /* red */
}

static rgba color_for_fps(double fps) {
  if (fps >= 40.0) return {0.20f, 0.95f, 0.30f, 1.0f}; /* green */
  if (fps >= 30.0) return {0.55f, 0.95f, 0.25f, 1.0f}; /* yellowish green */
  if (fps >= 20.0) return {1.00f, 0.75f, 0.15f, 1.0f}; /* orangeish yellow */
  return {1.00f, 0.15f, 0.12f, 1.0f};                   /* red */
}

static rgba color_for_ram(int pct) {
  if (pct < 0) return {1.0f, 1.0f, 1.0f, 1.0f};
  if (pct < 70) return {0.20f, 0.95f, 0.30f, 1.0f};   /* green */
  if (pct < 85) return {0.95f, 0.90f, 0.15f, 1.0f};   /* yellow */
  if (pct < 93) return {1.00f, 0.55f, 0.10f, 1.0f};   /* orange */
  return {1.00f, 0.15f, 0.12f, 1.0f};                 /* red */
}

static uint64_t get_time_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int get_ram_usage_percent(void) {
  uint64_t physmem = 0;
  size_t len = sizeof(physmem);
  int mib[2] = {CTL_HW, HW_PHYSMEM};
  if (sysctl(mib, 2, &physmem, &len, NULL, 0) == 0 && physmem > 0) {
    uint32_t page_size = 4096;
    uint32_t free_count = 0, wire_count = 0, active_count = 0;
    size_t sz = sizeof(uint32_t);

    sysctlbyname("vm.stats.vm.v_free_count", &free_count, &sz, NULL, 0);
    sysctlbyname("vm.stats.vm.v_wire_count", &wire_count, &sz, NULL, 0);
    sysctlbyname("vm.stats.vm.v_active_count", &active_count, &sz, NULL, 0);

    uint64_t total_pages = physmem / page_size;
    if (total_pages > 0) {
      uint64_t used_pages = wire_count + active_count;
      if (used_pages == 0 && free_count > 0 && free_count <= total_pages) {
        used_pages = total_pages - free_count;
      }
      int pct = (int)((used_pages * 100) / total_pages);
      if (pct >= 0 && pct <= 100) return pct;
    }
  }
  return -1;
}

static void load_config(void) {
  FILE *f = fopen("/data/fan_target/config.ini", "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    char *nl = strpbrk(val, "\r\n");
    if (nl) *nl = '\0';
    while (*key == ' ' || *key == '\t') key++;
    while (*val == ' ' || *val == '\t') val++;

    if (strcasecmp(key, "cpu_temp") == 0) g_config.show_cpu = (val[0] == '1');
    else if (strcasecmp(key, "soc_temp") == 0 || strcasecmp(key, "gpu_temp") == 0) g_config.show_soc = (val[0] == '1');
    else if (strcasecmp(key, "ssd_temp") == 0) g_config.show_ssd = (val[0] == '1');
    else if (strcasecmp(key, "ram_usage") == 0) g_config.show_ram = (val[0] == '1');
    else if (strcasecmp(key, "fps") == 0) g_config.show_fps = (val[0] == '1');
    else if (strcasecmp(key, "metric") == 0) g_config.metric = (val[0] == 'f' || val[0] == 'F') ? 'f' : 'c';
    else if (strcasecmp(key, "overlay_position") == 0) {
      if (strcasecmp(val, "top right") == 0 || strcasecmp(val, "top-right") == 0) g_config.position = POS_TOP_RIGHT;
      else if (strcasecmp(val, "bottom left") == 0 || strcasecmp(val, "bottom-left") == 0) g_config.position = POS_BOTTOM_LEFT;
      else if (strcasecmp(val, "bottom right") == 0 || strcasecmp(val, "bottom-right") == 0) g_config.position = POS_BOTTOM_RIGHT;
      else g_config.position = POS_TOP_LEFT;
    }
  }
  fclose(f);
}

static int load_module(const char *path) {
  if (!sceKernelLoadStartModule_fn) return -1;
  return sceKernelLoadStartModule_fn(path, 0, 0, 0, 0, 0);
}

static void *dlsym_mod(int h, const char *name) {
  void *p = nullptr;
  if (h >= 0 && sceKernelDlsym_fn)
    sceKernelDlsym_fn(h, name, &p);
  return p;
}

#define SYM(h, var) do { void *_p = dlsym_mod(h, #var); if (_p) *(void **)&var = _p; } while (0)

static bool resolve_all(void) {
  extern int sceKernelDlsym(int, const char *, void **) __attribute__((weak));
  extern int sceKernelLoadStartModule(const char *, int, const void *, int, void *, int *) __attribute__((weak));
  sceKernelDlsym_fn = sceKernelDlsym;
  sceKernelLoadStartModule_fn = sceKernelLoadStartModule;
  if (!sceKernelDlsym_fn || !sceKernelLoadStartModule_fn) return false;

  int k = load_module("/system/common/lib/libkernel_sys.sprx");
  if (k < 0) k = load_module("libkernel_sys.sprx");
  SYM(k, sceKernelGetCpuTemperature);
  SYM(k, sceKernelGetSocSensorTemperature);

  int m = load_module("/system/common/lib/libmonosgen-2.0.sprx");
  if (m < 0) m = load_module("libmonosgen-2.0.sprx");
  if (m < 0) return false;

  SYM(m, mono_get_root_domain);
  SYM(m, mono_string_new);
  SYM(m, mono_class_from_name);
  SYM(m, mono_image_loaded);
  SYM(m, mono_object_new);
  SYM(m, mono_runtime_object_init);
  SYM(m, mono_class_get_method_from_name);
  SYM(m, mono_runtime_invoke);
  SYM(m, mono_class_get_property_from_name);
  SYM(m, mono_property_get_set_method);
  SYM(m, mono_property_get_get_method);
  SYM(m, mono_object_unbox);
  SYM(m, mono_thread_attach);

  if (!mono_get_root_domain || !mono_string_new) return false;
  Root_Domain = mono_get_root_domain();
  if (mono_image_loaded) {
    pui_img = mono_image_loaded("Sce.PlayStation.PUI.dll");
    if (!pui_img) pui_img = mono_image_loaded("Sce.PlayStation.PUI");
  }
  return Root_Domain != nullptr && pui_img != nullptr;
}

static MonoObject *New_Object(MonoClass *klass) {
  if (!klass || !mono_object_new) return nullptr;
  return mono_object_new(Root_Domain, klass);
}

static void Set_Property_Float(MonoClass *klass, MonoObject *inst, const char *name, float val) {
  if (!klass || !inst || !mono_class_get_property_from_name) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  void *args[] = { &val };
  mono_runtime_invoke(set, inst, args, nullptr);
}

static void Set_Property_Bool(MonoClass *klass, MonoObject *inst, const char *name, bool val) {
  if (!klass || !inst || !mono_class_get_property_from_name) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  int32_t bval = val ? 1 : 0;
  void *args[] = { &bval };
  mono_runtime_invoke(set, inst, args, nullptr);
}

static void Set_Property_Str(MonoClass *klass, MonoObject *inst, const char *name, const char *str) {
  if (!klass || !inst || !mono_class_get_property_from_name) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  MonoString *ms = mono_string_new(Root_Domain, str);
  void *args[] = { ms };
  mono_runtime_invoke(set, inst, args, nullptr);
}

static void Set_Property_Obj(MonoClass *klass, MonoObject *inst, const char *name, MonoObject *obj) {
  if (!klass || !inst || !mono_class_get_property_from_name) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  void *args[] = { obj };
  mono_runtime_invoke(set, inst, args, nullptr);
}

template<typename R>
static R Get_Property(MonoClass *klass, MonoObject *inst, const char *name) {
  if (!klass) return (R)0;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return (R)0;
  MonoMethod *get = mono_property_get_get_method(prop);
  if (!get) return (R)0;
  MonoObject *ret = mono_runtime_invoke(get, inst, nullptr, nullptr);
  if (!ret) return (R)0;
  if (sizeof(R) <= sizeof(void *))
    return (R)(uintptr_t)ret;
  return *(R *)mono_object_unbox(ret);
}

static MonoObject *CreateUIColor(float r, float g, float b, float a) {
  MonoClass *c = mono_class_from_name(pui_img, "Sce.PlayStation.PUI", "UIColor");
  if (!c) return nullptr;
  MonoObject *o = New_Object(c);
  MonoObject *real = (MonoObject *)mono_object_unbox(o);
  MonoMethod *ctor = mono_class_get_method_from_name(c, ".ctor", 4);
  if (ctor) {
    void *args[] = { &r, &g, &b, &a };
    mono_runtime_invoke(ctor, real, args, nullptr);
  }
  return real;
}

static MonoObject *CreateUIFont(int size, int style, int weight) {
  MonoClass *c = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "UIFont");
  if (!c) return nullptr;
  MonoObject *o = New_Object(c);
  MonoObject *real = (MonoObject *)mono_object_unbox(o);
  MonoMethod *ctor = mono_class_get_method_from_name(c, ".ctor", 3);
  if (ctor) {
    void *args[] = { &size, &style, &weight };
    mono_runtime_invoke(ctor, real, args, nullptr);
  }
  return real;
}

static MonoObject *CreateLabel(const char *name, float x, float y, const char *text,
                               MonoObject *fnt, float r, float g, float b, float a) {
  MonoClass *labelClass = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Label");
  if (!labelClass) return nullptr;
  MonoObject *label = New_Object(labelClass);
  if (!label) return nullptr;
  if (mono_runtime_object_init) mono_runtime_object_init(label);

  Set_Property_Str(labelClass, label, "Name", name);
  Set_Property_Float(labelClass, label, "X", x);
  Set_Property_Float(labelClass, label, "Y", y);
  Set_Property_Str(labelClass, label, "Text", text);
  if (fnt) Set_Property_Obj(labelClass, label, "Font", fnt);
  Set_Property_Obj(labelClass, label, "TextColor", CreateUIColor(r, g, b, a));
  Set_Property_Bool(labelClass, label, "FitWidthToText", true);
  Set_Property_Bool(labelClass, label, "FitHeightToText", true);
  return label;
}

static void Widget_Append_Child(MonoObject *widget, MonoObject *child) {
  if (!widget || !child) return;
  MonoClass *wc = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Widget");
  MonoMethod *m = mono_class_get_method_from_name(wc, "AppendChild", 1);
  if (!m) return;
  void *args[] = { child };
  mono_runtime_invoke(m, widget, args, nullptr);
}

static void create_widgets(void) {
  if (g_widgets_created || !pui_img || !Root_Domain) return;

  MonoClass *sceneClass = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Scene");
  if (sceneClass) {
    MonoProperty *gameProp = mono_class_get_property_from_name(sceneClass, "Game");
    if (gameProp) {
      MonoMethod *get = mono_property_get_get_method(gameProp);
      if (get) {
        MonoObject *game = mono_runtime_invoke(get, nullptr, nullptr, nullptr);
        if (game)
          rootWidget = Get_Property<MonoObject *>(sceneClass, game, "RootWidget");
      }
    }
  }
  if (!rootWidget) return;

  font = CreateUIFont(20, 0, 0);
  rgba dim = {0.85f, 0.85f, 0.85f, 1.0f};

  lbl_cpu_name = CreateLabel("id_cpu_label", 0, 0, "CPU", font, dim.r, dim.g, dim.b, dim.a);
  lbl_cpu_val = CreateLabel("id_cpu_temp_value", 0, 0, "--C", font, 0.2f, 0.95f, 0.3f, 1.0f);
  Widget_Append_Child(rootWidget, lbl_cpu_name);
  Widget_Append_Child(rootWidget, lbl_cpu_val);

  lbl_soc_name = CreateLabel("id_soc_label", 0, 0, "SoC", font, dim.r, dim.g, dim.b, dim.a);
  lbl_soc_val = CreateLabel("id_soc_temp_value", 0, 0, "--C", font, 0.2f, 0.95f, 0.3f, 1.0f);
  Widget_Append_Child(rootWidget, lbl_soc_name);
  Widget_Append_Child(rootWidget, lbl_soc_val);

  lbl_ssd_name = CreateLabel("id_ssd_label", 0, 0, "SSD", font, dim.r, dim.g, dim.b, dim.a);
  lbl_ssd_val = CreateLabel("id_ssd_temp_value", 0, 0, "--C", font, 0.2f, 0.95f, 0.3f, 1.0f);
  Widget_Append_Child(rootWidget, lbl_ssd_name);
  Widget_Append_Child(rootWidget, lbl_ssd_val);

  lbl_ram_name = CreateLabel("id_ram_label", 0, 0, "RAM", font, dim.r, dim.g, dim.b, dim.a);
  lbl_ram_val = CreateLabel("id_ram_value", 0, 0, "--%", font, 0.2f, 0.95f, 0.3f, 1.0f);
  Widget_Append_Child(rootWidget, lbl_ram_name);
  Widget_Append_Child(rootWidget, lbl_ram_val);

  lbl_fps_name = CreateLabel("id_fps_label", 0, 0, "FPS", font, dim.r, dim.g, dim.b, dim.a);
  lbl_fps_val = CreateLabel("id_fps_value", 0, 0, "---", font, 0.2f, 0.95f, 0.3f, 1.0f);
  Widget_Append_Child(rootWidget, lbl_fps_name);
  Widget_Append_Child(rootWidget, lbl_fps_val);

  g_widgets_created = true;
}

static void update_labels(void) {
  if (!g_widgets_created || !pui_img) return;

  MonoClass *labelClass = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Label");
  if (!labelClass) return;

  int cpu = -1, soc = -1, ssd = -1;
  if (sceKernelGetCpuTemperature) sceKernelGetCpuTemperature(&cpu);
  if (sceKernelGetSocSensorTemperature) {
    sceKernelGetSocSensorTemperature(0, &soc);
    sceKernelGetSocSensorTemperature(1, &ssd);
  }
  int ram_pct = get_ram_usage_percent();

  uint64_t now = get_time_ms();
  uint64_t last_fps_time = __atomic_load_n(&g_last_fps_time, __ATOMIC_RELAXED);
  bool have_fps = (last_fps_time > 0 && (now - last_fps_time) < 2000);
  double fps_val = __atomic_load_n(&g_fps, __ATOMIC_RELAXED);

  int visible_count = 0;
  if (g_config.show_cpu) visible_count++;
  if (g_config.show_soc) visible_count++;
  if (g_config.show_ssd) visible_count++;
  if (g_config.show_ram) visible_count++;
  if (g_config.show_fps) visible_count++;

  float start_x = 20.0f;
  float start_y = 20.0f;
  float line_height = 26.0f;

  switch (g_config.position) {
    case POS_TOP_LEFT:
      start_x = 20.0f;
      start_y = 20.0f;
      break;
    case POS_TOP_RIGHT:
      start_x = 1740.0f;
      start_y = 20.0f;
      break;
    case POS_BOTTOM_LEFT:
      start_x = 20.0f;
      start_y = 1080.0f - (visible_count * line_height + 20.0f);
      break;
    case POS_BOTTOM_RIGHT:
      start_x = 1740.0f;
      start_y = 1080.0f - (visible_count * line_height + 20.0f);
      break;
  }

  int row = 0;
  char buf[32];

  auto update_pair = [&](MonoObject *lbl_name, MonoObject *lbl_val, bool is_enabled,
                         const char *val_str, rgba color) {
    if (!lbl_name || !lbl_val) return;
    if (is_enabled) {
      float y = start_y + (row * line_height);
      Set_Property_Float(labelClass, lbl_name, "X", start_x);
      Set_Property_Float(labelClass, lbl_name, "Y", y);
      Set_Property_Bool(labelClass, lbl_name, "Visible", true);

      Set_Property_Float(labelClass, lbl_val, "X", start_x + 60.0f);
      Set_Property_Float(labelClass, lbl_val, "Y", y);
      Set_Property_Str(labelClass, lbl_val, "Text", val_str);
      Set_Property_Obj(labelClass, lbl_val, "TextColor", CreateUIColor(color.r, color.g, color.b, color.a));
      Set_Property_Bool(labelClass, lbl_val, "Visible", true);
      row++;
    } else {
      Set_Property_Bool(labelClass, lbl_name, "Visible", false);
      Set_Property_Bool(labelClass, lbl_val, "Visible", false);
    }
  };

  // 1. CPU
  if (cpu >= 0) {
    int disp = (g_config.metric == 'f') ? (cpu * 9 / 5 + 32) : cpu;
    snprintf(buf, sizeof(buf), "%d°%c", disp, (g_config.metric == 'f') ? 'F' : 'C');
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  update_pair(lbl_cpu_name, lbl_cpu_val, g_config.show_cpu, buf, color_for_temp(cpu));

  // 2. SoC
  if (soc >= 0) {
    int disp = (g_config.metric == 'f') ? (soc * 9 / 5 + 32) : soc;
    snprintf(buf, sizeof(buf), "%d°%c", disp, (g_config.metric == 'f') ? 'F' : 'C');
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  update_pair(lbl_soc_name, lbl_soc_val, g_config.show_soc, buf, color_for_temp(soc));

  // 3. SSD
  if (ssd >= 0) {
    int disp = (g_config.metric == 'f') ? (ssd * 9 / 5 + 32) : ssd;
    snprintf(buf, sizeof(buf), "%d°%c", disp, (g_config.metric == 'f') ? 'F' : 'C');
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  update_pair(lbl_ssd_name, lbl_ssd_val, g_config.show_ssd, buf, color_for_temp(ssd));

  // 4. RAM
  if (ram_pct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", ram_pct);
  } else {
    snprintf(buf, sizeof(buf), "--%%");
  }
  update_pair(lbl_ram_name, lbl_ram_val, g_config.show_ram, buf, color_for_ram(ram_pct));

  // 5. FPS
  if (have_fps) {
    snprintf(buf, sizeof(buf), "%.1f", fps_val);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  rgba fps_col = have_fps ? color_for_fps(fps_val) : rgba{0.7f, 0.7f, 0.7f, 1.0f};
  update_pair(lbl_fps_name, lbl_fps_val, g_config.show_fps, buf, fps_col);
}

static void *udp_thread(void *) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return nullptr;
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(FPS_UDP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(s);
    return nullptr;
  }
  for (;;) {
    double fps = 0;
    if (recv(s, &fps, sizeof(fps), 0) == (ssize_t)sizeof(fps)) {
      uint64_t now_ms = get_time_ms();
      __atomic_store_n(&g_fps, fps, __ATOMIC_RELAXED);
      __atomic_store_n(&g_last_fps_time, now_ms, __ATOMIC_RELAXED);
    }
  }
  return nullptr;
}

static void *poll_thread(void *) {
  if (mono_thread_attach && Root_Domain) {
    mono_thread_attach(Root_Domain);
  }
  uint32_t loop_count = 0;
  for (;;) {
    if ((loop_count % 20) == 0) {
      load_config();
    }
    if (!g_widgets_created) create_widgets();
    update_labels();
    usleep(100000); /* 100ms UI refresh */
    loop_count++;
  }
  return nullptr;
}

int main(int argc, const char **argv) {
  (void)argc; (void)argv;
  if (!resolve_all()) {
    for (;;) sleep(60);
  }
  pthread_t th, poll;
  pthread_create(&th, nullptr, udp_thread, nullptr);
  pthread_create(&poll, nullptr, poll_thread, nullptr);
  for (;;) sleep(60);
  return 0;
}

