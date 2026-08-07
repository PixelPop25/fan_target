/* fan_target ShellUI overlay
 * Injected into SceShellUI — same model as etaHEN shellui.elf
 *
 * - Mono already loaded in process; we dlsym mono_* / kernel APIs
 * - Create Label widgets on RootWidget (CreateLabel / AppendChild)
 * - Hook Sce.PlayStation.PUI.Application.Update for per-frame refresh
 * - FPS from game via localhost UDP :29028 (sub-ms; value itself is a
 *   rolling window, updated every 250ms from fps_elf)
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
static void *(*mono_compile_method)(MonoMethod *);
static void *(*mono_object_unbox)(MonoObject *);
static MonoMethod *(*mono_class_get_methods)(MonoClass *, void **);
static char *(*mono_method_get_name)(MonoMethod *);

static int (*sceKernelDlsym_fn)(int, const char *, void **);
static int (*sceKernelLoadStartModule_fn)(const char *, int, const void *, int, void *, int *);
static int (*sceKernelGetCpuTemperature)(int *);
static int (*sceKernelGetSocSensorTemperature)(int, int *);
static int (*sceKernelSendNotificationRequest)(int, void *, size_t, int);
static int (*get_page_table_stats)(int, int, int *, int *);

static MonoDomain *Root_Domain;
static MonoImage *pui_img;
static MonoObject *Game;
static MonoObject *rootWidget;
static MonoObject *font;

static MonoObject *lbl_cpu_t, *lbl_cpu_u, *lbl_gpu_t, *lbl_gpu_u;
static MonoObject *lbl_ram, *lbl_fps;

static std::atomic<double> g_fps{0.0};
static std::atomic<int> g_have_fps{0};
static bool g_widgets_created = false;
static void (*OnRender_orig)(MonoObject *) = nullptr;

typedef struct {
  int32_t type, req_id, priority, msg_id, target_id, user_id;
  int32_t unk1, unk2, app_id, error_num, unk3;
  char use_icon_image_uri;
  char message[1024];
  char uri[1024];
  char unkstr[1024];
} OrbisNotificationRequest;

static void notify(const char *fmt, ...) {
  if (!sceKernelSendNotificationRequest) return;
  OrbisNotificationRequest n{};
  va_list ap; va_start(ap, fmt);
  vsnprintf(n.message, sizeof(n.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &n, sizeof(n), 0);
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
  SYM(k, sceKernelSendNotificationRequest);
  SYM(k, get_page_table_stats);

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
  SYM(m, mono_compile_method);
  SYM(m, mono_object_unbox);

  if (!mono_get_root_domain || !mono_string_new) return false;
  Root_Domain = mono_get_root_domain();
  if (mono_image_loaded) {
    pui_img = mono_image_loaded("Sce.PlayStation.PUI.dll");
    if (!pui_img) pui_img = mono_image_loaded("Sce.PlayStation.PUI");
  }
  return Root_Domain != nullptr;
}

static MonoObject *New_Object(MonoClass *klass) {
  if (!klass || !mono_object_new) return nullptr;
  return mono_object_new(Root_Domain, klass);
}

template<typename T>
static void Set_Property(MonoClass *klass, MonoObject *inst, const char *name, T value) {
  if (!klass || !inst || !mono_class_get_property_from_name) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  void *args[] = { &value };
  mono_runtime_invoke(set, inst, args, nullptr);
}

static void Set_Property_Str(MonoClass *klass, MonoObject *inst, const char *name, const char *str) {
  if (!klass || !inst) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  MonoString *ms = mono_string_new(Root_Domain, str);
  void *args[] = { ms };
  mono_runtime_invoke(set, inst, args, nullptr);
}

static void Set_Property_Obj(MonoClass *klass, MonoObject *inst, const char *name, MonoObject *obj) {
  if (!klass || !inst) return;
  MonoProperty *prop = mono_class_get_property_from_name(klass, name);
  if (!prop) return;
  MonoMethod *set = mono_property_get_set_method(prop);
  if (!set) return;
  void *args[] = { obj };
  mono_runtime_invoke(set, inst, args, nullptr);
}

template<typename R>
static R Get_Property(MonoClass *klass, MonoObject *inst, const char *name) {
  if (!klass || !mono_class_get_property_from_name) return (R)0;
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
                               MonoObject *fnt, int bold, float r, float g, float b, float a) {
  MonoClass *labelClass = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Label");
  if (!labelClass) return nullptr;
  MonoObject *label = New_Object(labelClass);
  if (!label) return nullptr;
  if (mono_runtime_object_init) mono_runtime_object_init(label);

  Set_Property_Str(labelClass, label, "Name", name);
  Set_Property<float>(labelClass, label, "X", x);
  Set_Property<float>(labelClass, label, "Y", y);
  Set_Property_Str(labelClass, label, "Text", text);
  if (fnt) Set_Property_Obj(labelClass, label, "Font", fnt);
  Set_Property<int>(labelClass, label, "HorizontalAlignment", bold);
  Set_Property_Obj(labelClass, label, "TextColor", CreateUIColor(r, g, b, a));
  Set_Property<bool>(labelClass, label, "FitWidthToText", true);
  Set_Property<bool>(labelClass, label, "FitHeightToText", true);
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

static MonoObject *FindWidget(const char *name) {
  if (!rootWidget || !pui_img) return nullptr;
  MonoClass *wc = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Widget");
  MonoMethod *m = mono_class_get_method_from_name(wc, "FindWidgetByName", 1);
  if (!m) return nullptr;
  MonoString *s = mono_string_new(Root_Domain, name);
  void *args[] = { s };
  return mono_runtime_invoke(m, rootWidget, args, nullptr);
}

static void SetLabelText(MonoObject *label, const char *text) {
  if (!label || !pui_img) return;
  MonoClass *lc = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Label");
  Set_Property_Str(lc, label, "Text", text);
}

static void create_widgets(void) {
  if (g_widgets_created || !pui_img || !Root_Domain) return;

  /* Game scene — same path as etaHEN */
  MonoClass *appClass = mono_class_from_name(pui_img, "Sce.PlayStation.PUI", "Application");
  /* RootWidget from Scene.Game if available; try common static accessors */
  MonoClass *sceneClass = mono_class_from_name(pui_img, "Sce.PlayStation.PUI.UI2", "Scene");
  if (sceneClass && Game) {
    rootWidget = Get_Property<MonoObject *>(sceneClass, Game, "RootWidget");
  }
  if (!rootWidget) {
    /* Fallback: Application current scene */
    notify("fan_target: waiting for RootWidget");
    return;
  }

  font = CreateUIFont(22, 0, 0);
  float y = 10.0f;
  Widget_Append_Child(rootWidget, CreateLabel("id_cpu_label", 10, y, "CPU", font, 1, 0, 1, 1, 1));
  lbl_cpu_t = CreateLabel("id_cpu_temp_value", 80, y, "--C", font, 0, 1, 0.6f, 0, 1);
  lbl_cpu_u = CreateLabel("id_cpu_usage_value", 130, y, "--%", font, 0, 1, 0.6f, 0, 1);
  Widget_Append_Child(rootWidget, lbl_cpu_t);
  Widget_Append_Child(rootWidget, lbl_cpu_u);
  y += 25;
  Widget_Append_Child(rootWidget, CreateLabel("id_gpu_label", 10, y, "GPU", font, 1, 0, 1, 0, 1));
  lbl_gpu_t = CreateLabel("id_gpu_temp_value", 80, y, "--C", font, 0, 1, 0.6f, 0, 1);
  lbl_gpu_u = CreateLabel("id_gpu_usage_value", 130, y, "--%", font, 0, 1, 0.6f, 0, 1);
  Widget_Append_Child(rootWidget, lbl_gpu_t);
  Widget_Append_Child(rootWidget, lbl_gpu_u);
  y += 25;
  Widget_Append_Child(rootWidget, CreateLabel("id_ram_label", 10, y, "RAM", font, 1, 0, 1, 1, 1));
  lbl_ram = CreateLabel("id_ram_value", 80, y, "--- MB", font, 0, 1, 0.6f, 0, 1);
  Widget_Append_Child(rootWidget, lbl_ram);
  y += 25;
  Widget_Append_Child(rootWidget, CreateLabel("id_fps_label", 10, y, "FPS", font, 1, 1, 0, 1, 1));
  lbl_fps = CreateLabel("id_fps_value", 80, y, "---", font, 0, 1, 1, 1, 1);
  Widget_Append_Child(rootWidget, lbl_fps);

  g_widgets_created = true;
  notify("fan_target HUD labels created");
}

static void update_labels(void) {
  char buf[64];
  int cpu = -1, soc = -1;
  if (sceKernelGetCpuTemperature) sceKernelGetCpuTemperature(&cpu);
  if (sceKernelGetSocSensorTemperature) sceKernelGetSocSensorTemperature(0, &soc);

  if (lbl_cpu_t) { snprintf(buf, sizeof(buf), "%dC", cpu); SetLabelText(lbl_cpu_t, buf); }
  if (lbl_gpu_t) { snprintf(buf, sizeof(buf), "%dC", soc); SetLabelText(lbl_gpu_t, buf); }

  if (lbl_ram && get_page_table_stats) {
    int used = 0, free_ = 0;
    get_page_table_stats(1, 1, &used, &free_);
    snprintf(buf, sizeof(buf), "%d MB", used);
    SetLabelText(lbl_ram, buf);
  }

  if (lbl_fps && g_have_fps.load()) {
    snprintf(buf, sizeof(buf), "%.1f", g_fps.load());
    SetLabelText(lbl_fps, buf);
  }
}

static void OnRender_Hook(MonoObject *instance) {
  static int wait = 0;
  if (!g_widgets_created)
    create_widgets();
  if (wait <= 0) {
    update_labels();
    wait = 15; /* ~every 15 frames — responsive without thrashing Mono */
  } else {
    wait--;
  }
  if (OnRender_orig)
    OnRender_orig(instance);
}

/* ---- UDP receive (game → ShellUI); localhost, no disk ---- */
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
      g_fps.store(fps);
      g_have_fps.store(1);
    }
  }
  return nullptr;
}

static void *poll_thread(void *) {
  for (;;) {
    if (!g_widgets_created) create_widgets();
    update_labels();
    usleep(100000);
  }
  return nullptr;
}

int main(int argc, const char **argv) {
  (void)argc; (void)argv;
  if (!resolve_all()) {
    notify("fan_target overlay: mono resolve failed");
    for (;;) sleep(60);
  }
  pthread_t th;
  pthread_create(&th, nullptr, udp_thread, nullptr);

  pthread_t poll;
  pthread_create(&poll, nullptr, poll_thread, nullptr);

  notify("fan_target overlay in SceShellUI");
  for (;;) sleep(60);
  return 0;
}
