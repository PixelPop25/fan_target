/* fan_target fps_elf — game process only (etaHEN-derived).
 * Hooks Gnm flip, computes FPS on a 200ms window, sends to ShellUI overlay
 * via localhost UDP. No disk I/O. Dies with the game.
 */
#include "defs.h"
#include "Detour.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define u32 uint32_t
#define s32 int32_t
#define FPS_UDP_PORT 29028
#define FPS_WINDOW_SEC 0.2

extern "C" {
    long ptr_syscall = 0;
    int sceKernelLoadStartModule(const char *, int, const void *, int, void *, int *);
    int sceKernelDlsym(int, const char *, void **);
    s32 sceGnmSubmitAndFlipCommandBuffersForWorkload(
        u32, u32, u32 *[], u32 *[], u32 *[], u32 *[], u32, u32, u32, u32);
}

void __syscall() {
  asm(".intel_syntax noprefix\n"
      "  mov rax, rdi\n"
      "  mov rdi, rsi\n"
      "  mov rsi, rdx\n"
      "  mov rdx, rcx\n"
      "  mov r10, r8\n"
      "  mov r8,  r9\n"
      "  mov r9,  qword ptr [rsp + 8]\n"
      "  call qword ptr [rip + ptr_syscall]\n"
      "  ret\n");
}

typedef struct {
    int32_t type, req_id, priority, msg_id, target_id, user_id;
    int32_t unk1, unk2, app_id, error_num, unk3;
    char use_icon_image_uri;
    char message[1024], uri[1024], unkstr[1024];
} OrbisNotificationRequest;

extern "C" int sceKernelSendNotificationRequest(int, OrbisNotificationRequest *, size_t, int);

static int frame_count = 0;
static double g_last_fps = 0.0;
/* Exported-looking symbol so a debugger/overlay can locate it if needed */
extern "C" volatile double fan_target_fps_value = 0.0;

static int g_udp = -1;
static struct sockaddr_in g_addr;

static void udp_init(void) {
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0) return;
    memset(&g_addr, 0, sizeof(g_addr));
    g_addr.sin_family = AF_INET;
    g_addr.sin_port = htons(FPS_UDP_PORT);
    g_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static void udp_send(double fps) {
    if (g_udp < 0) return;
    sendto(g_udp, &fps, sizeof(fps), 0, (struct sockaddr *)&g_addr, sizeof(g_addr));
}

static void notify_once(const char *msg) {
    OrbisNotificationRequest n{};
    snprintf(n.message, sizeof(n.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &n, sizeof(n), 0);
}

static void on_flip(void) {
    static auto last = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    auto delta = std::chrono::duration<double>(now - last).count();
    frame_count++;
    if (delta >= FPS_WINDOW_SEC) {
        g_last_fps = frame_count / delta;
        fan_target_fps_value = g_last_fps;
        udp_send(g_last_fps);
        frame_count = 0;
        last = now;
        klog_printf("fps_elf: %.1f\n", g_last_fps);
    }
}

static s32 (*orig_gnm)(u32, u32, u32 *[], u32 *[], u32 *[], u32 *[], u32, u32, u32, u32) = nullptr;

static s32 hook_gnm(u32 w, u32 c, u32 *d[], u32 *ds[], u32 *cc[], u32 *cs[],
                    u32 vo, u32 bi, u32 fm, u32 fa) {
    on_flip();
    return orig_gnm(w, c, d, ds, cc, cs, vo, bi, fm, fa);
}

int main(int, char const **) {
    char buf[256];
    klog_puts("fps_elf start");
    udp_init();
    notify_once("fan_target FPS on");
    while (sceKernelMprotect(buf, sizeof(buf), PROT_READ|PROT_WRITE|PROT_EXEC) != 0)
        sleep(1);
    orig_gnm = (decltype(orig_gnm))DetourFunction(
        (uint64_t)&sceGnmSubmitAndFlipCommandBuffersForWorkload, (void *)hook_gnm);
    klog_puts("fps_elf hooked Gnm");
    for (;;) sleep(60);
    return 0;
}
