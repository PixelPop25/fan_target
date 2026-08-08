/* fps_elf — injected into game process only
 * Hooks Gnm flip, computes FPS, UDP to 127.0.0.1:29028
 * No disk I/O. No notifications. Dies with the game.
 */
#include "defs.h"
#include "Detour.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define u32 uint32_t
#define s32 int32_t
#define FPS_UDP_PORT 29028
#define FPS_WINDOW_SEC 0.25

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

static int frame_count = 0;
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
    sendto(g_udp, &fps, sizeof(fps), 0,
           (struct sockaddr *)&g_addr, sizeof(g_addr));
}

static void on_flip(void) {
    static auto last = std::chrono::high_resolution_clock::now();
    frame_count++;
    auto now = std::chrono::high_resolution_clock::now();
    double delta = std::chrono::duration<double>(now - last).count();
    if (delta >= FPS_WINDOW_SEC) {
        double fps = (double)frame_count / delta;
        frame_count = 0;
        last = now;
        udp_send(fps);
    }
}

Detour *flip_detour = nullptr;

s32 sceGnmSubmitAndFlipCommandBuffersForWorkload_hook(
    u32 workload, u32 count, u32 *dcbGpuAddrs[], u32 *dcbSizes[],
    u32 *ccbGpuAddrs[], u32 *ccbSizes[], u32 state, u32 queue, u32 label, u32 labelValue) {
    on_flip();
    return ((s32(*)(u32, u32, u32 *[], u32 *[], u32 *[], u32 *[], u32, u32, u32, u32))
            flip_detour->GetOriginal())(
        workload, count, dcbGpuAddrs, dcbSizes, ccbGpuAddrs, ccbSizes,
        state, queue, label, labelValue);
}

extern "C" int main(int argc, const char **argv) {
    (void)argc; (void)argv;
    udp_init();

    int mod = sceKernelLoadStartModule("/system/common/lib/libSceGnmDriver.sprx", 0, 0, 0, 0, 0);
    if (mod < 0)
        mod = sceKernelLoadStartModule("libSceGnmDriver.sprx", 0, 0, 0, 0, 0);
    void *sym = nullptr;
    if (mod >= 0)
        sceKernelDlsym(mod, "sceGnmSubmitAndFlipCommandBuffersForWorkload", &sym);
    if (!sym) {
        for (;;) sleep(60);
    }

    flip_detour = new Detour();
    flip_detour->HookFunction((void *)sym,
        (void *)sceGnmSubmitAndFlipCommandBuffersForWorkload_hook);

    for (;;) sleep(60);
    return 0;
}
