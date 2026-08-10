/*
 * fps_main.cpp — game-process FPS counter for fan_target_pxp
 *
 * Injected into the running game via elfldr_exec (fan_target.elf → injector).
 *
 * DESIGN NOTES
 * ============
 * elfldr_exec maps the ELF into the target and only processes R_X86_64_RELATIVE
 * relocations — PLT entries for external symbols are NOT resolved by the OS
 * dynamic linker.  The ps5-payload-sdk CRT, however, bootstraps a set of
 * libkernel stubs via kernel r/w before calling main().  Any libkernel function
 * declared as  extern T foo(...) __attribute__((weak));  and then null-checked
 * will be available if the CRT resolved it, which it does for the full libkernel
 * surface (sceKernelGetModuleList, sceKernelGetModuleInfo, sceKernelDlsym, mmap,
 * mprotect, sysctlbyname, socket, pthread_*, …).
 *
 * SceGnmDriver functions, however, are NOT in libkernel, so we cannot call them
 * through the PLT.  We must find sceGnmSubmitAndFlipCommandBuffers manually:
 *
 *   Primary path  : sceKernelGetModuleList → find SceGnmDriver handle →
 *                   sceKernelDlsym("sceGnmSubmitAndFlipCommandBuffers")
 *   Fallback path : sysctlbyname("kern.osrelease") → parse FW version →
 *                   look up FW-specific byte-offset in gnm_flip_offsets[] →
 *                   add to SceGnmDriver .text base
 *
 * Once we have the function address we install a 14-byte absolute-JMP inline
 * hook (FF 25 00 00 00 00 <8-byte target>) with a small RWX trampoline so the
 * game's Gnm submit path continues to work normally.
 *
 * The hook increments a per-second frame counter.  A background thread sends
 * the rolling FPS double over UDP to 127.0.0.1:29028 where overlay_elf reads it.
 *
 * HOW TO FILL IN OFFSETS
 * ======================
 * If sceKernelDlsym cannot resolve the function by name on your console
 * (some FW versions may export by NID only), populate gnm_flip_offsets[] below
 * with the offset of sceGnmSubmitAndFlipCommandBuffers inside SceGnmDriver.sprx
 * for each jailbreakable firmware.  Sources:
 *
 *   • etaHEN  — Source Code/fps_elf/src/  (look for FW_VER / offset switch)
 *   • y2jb    — versions 1.3 – 1.5, hooks/gnm_hook.cpp or similar
 *   • luaC0re — 2.2d, same function, different naming
 *
 * The offset is from the module's mapped TEXT base to the entry point of
 * sceGnmSubmitAndFlipCommandBuffers (or whichever internal flip trampoline
 * those projects hook — they are equivalent for frame-counting purposes).
 *
 * NID alternative: if you have the NID for sceGnmSubmitAndFlipCommandBuffers
 * (8-char hex string from the SDK's export table, e.g. "B8B13B92"),
 * replace the name string in dlsym_gnm_flip() below.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* =========================================================================
 * libkernel weak externs — resolved by ps5-payload-sdk CRT before main().
 * Never call these without null-checking first.
 * ========================================================================= */
extern "C" {

/* Module enumeration / symbol resolution */
extern int sceKernelGetModuleList(uint32_t *handles, int max_count,
                                  int *count)
    __attribute__((weak));

struct SceKernelModuleInfoEx {
    size_t   size;               /* set to sizeof before calling */
    char     name[256];
    uint8_t  _pad[12];
    void    *segment_base[4];
    size_t   segment_size[4];
    /* more fields follow, we don't care about them */
    uint8_t  _rest[512];
};

extern int sceKernelGetModuleInfoEx(uint32_t handle,
                                    struct SceKernelModuleInfoEx *info)
    __attribute__((weak));

/* Symbol lookup inside a loaded module */
extern int sceKernelDlsym(int handle, const char *name, void **out)
    __attribute__((weak));

} /* extern "C" */

/* =========================================================================
 * FW version helpers
 * =========================================================================
 * Parses kern.osrelease which looks like "9.60" (or "9.60.00.00.00").
 * Returns the version encoded the same way as PS5_FW_VERSION:
 *   major nibble at [27:24], minor ten-digit at [23:20], ones at [19:16]
 * Examples:
 *   "9.60"  → 0x09600000
 *   "9.00"  → 0x09000000
 *   "10.01" → 0x10010000
 *   "11.02" → 0x11020000
 * ========================================================================= */
static uint32_t detect_fw_version(void)
{
    char buf[64] = {};
    size_t len   = sizeof(buf);

    if (sysctlbyname("kern.osrelease", buf, &len, nullptr, 0) != 0)
        return 0x09600000; /* 9.60 fallback */

    int major = 0, minor_hi = 0, minor_lo = 0;
    /* "9.60" → major=9, then parse "60" as two single digits */
    char minor_str[8] = {};
    if (sscanf(buf, "%d.%7s", &major, minor_str) < 2)
        return 0x09600000;

    /* minor_str could be "60", "00", "01", etc. */
    minor_hi = (minor_str[0] >= '0' && minor_str[0] <= '9')
               ? (minor_str[0] - '0') : 0;
    minor_lo = (minor_str[1] >= '0' && minor_str[1] <= '9')
               ? (minor_str[1] - '0') : 0;

    uint32_t fw = (uint32_t)((major    & 0xFF) << 24) |
                  (uint32_t)((minor_hi & 0x0F) << 20) |
                  (uint32_t)((minor_lo & 0x0F) << 16);
    return fw ? fw : 0x09600000;
}

/* =========================================================================
 * Per-FW offset table
 *
 * Each entry is the byte offset from SceGnmDriver.sprx TEXT base to the
 * entry point of sceGnmSubmitAndFlipCommandBuffers (or the internal flip
 * trampoline used by etaHEN / y2jb / luaC0re — same frame-counting effect).
 *
 * HOW TO FIND OFFSETS:
 *   1. Clone etaHEN, open Source Code/fps_elf/src/  — look for a switch or
 *      #if PS5_FW_VERSION block that selects HOOK_ADDR or similar.
 *   2. Cross-reference with y2jb 1.3-1.5 hooks/gnm_hook.cpp
 *   3. Cross-reference with luaC0re 2.2d
 *
 * Set .offset = 0 to mark "unknown — skip offset fallback for this FW".
 * ========================================================================= */
struct fw_gnm_offset {
    uint32_t  fw_ver;   /* encoded as detect_fw_version() returns */
    uintptr_t offset;   /* bytes from SceGnmDriver TEXT base; 0 = unknown */
};

static const fw_gnm_offset gnm_flip_offsets[] = {
    /* ------- ADD ENTRIES FROM etaHEN / y2jb / luaC0re SOURCE BELOW ------- */

    /* FW 7.61 */ { 0x07610000, 0 /* TODO: fill from etaHEN/y2jb */ },
    /* FW 8.00 */ { 0x08000000, 0 /* TODO */ },
    /* FW 8.20 */ { 0x08200000, 0 /* TODO */ },
    /* FW 8.40 */ { 0x08400000, 0 /* TODO */ },
    /* FW 9.00 */ { 0x09000000, 0 /* TODO */ },

    /* FW 9.60 — PRIMARY TEST TARGET.
     * Replace 0 with the real offset from SceGnmDriver.sprx TEXT base.
     * Look in etaHEN Source Code/fps_elf/src/ for PS5_FW_VERSION == 0x9600000
     * or search y2jb 1.3/1.4/1.5 for "9.60" near "GnmDriver" / "flip". */
    { 0x09600000, 0 /* TODO: replace with actual 9.60 offset */ },

    /* FW 10.01 */ { 0x10010000, 0 /* TODO */ },
    /* FW 10.50 */ { 0x10500000, 0 /* TODO */ },
    /* FW 11.00 */ { 0x11000000, 0 /* TODO */ },

    /* sentinel */
    { 0, 0 }
};

static uintptr_t lookup_gnm_offset(uint32_t fw_ver)
{
    for (int i = 0; gnm_flip_offsets[i].fw_ver != 0; i++) {
        if (gnm_flip_offsets[i].fw_ver == fw_ver)
            return gnm_flip_offsets[i].offset;
    }
    return 0;
}

/* =========================================================================
 * Module helpers — find SceGnmDriver and its TEXT base
 * ========================================================================= */
#define MAX_MODULES 256

static int find_module_handle(const char *partial_name)
{
    if (!sceKernelGetModuleList || !sceKernelGetModuleInfoEx)
        return -1;

    uint32_t handles[MAX_MODULES];
    int count = 0;
    if (sceKernelGetModuleList(handles, MAX_MODULES, &count) != 0 || count <= 0)
        return -1;

    for (int i = 0; i < count; i++) {
        struct SceKernelModuleInfoEx info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfoEx(handles[i], &info) != 0)
            continue;
        if (strstr(info.name, partial_name))
            return (int)handles[i];
    }
    return -1;
}

static void *gnm_module_text_base(int handle)
{
    if (!sceKernelGetModuleInfoEx) return nullptr;
    struct SceKernelModuleInfoEx info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetModuleInfoEx((uint32_t)handle, &info) != 0)
        return nullptr;
    return info.segment_base[0]; /* TEXT is always segment 0 */
}

/* =========================================================================
 * Symbol resolution — try by name, then by NID string, then offset table
 * ========================================================================= */
static void *dlsym_gnm_flip(void)
{
    int h = find_module_handle("SceGnmDriver");
    if (h < 0 || !sceKernelDlsym)
        return nullptr;

    void *fn = nullptr;

    /* Attempt 1: plain function name — works when SceGnmDriver exports by name */
    sceKernelDlsym(h, "sceGnmSubmitAndFlipCommandBuffers", &fn);
    if (fn) return fn;

    /* Attempt 2: NID string.
     * The NID for sceGnmSubmitAndFlipCommandBuffers is the first 8 hex chars
     * of SHA-256("sceGnmSubmitAndFlipCommandBuffers\x00").
     * TODO: replace the placeholder below with the real NID from the SDK headers
     * or the SceGnmDriver export table (e.g. IDA → Exports → sort by name). */
    sceKernelDlsym(h, "PLACEHOLDER_NID", &fn); /* TODO: real NID */
    if (fn) return fn;

    return nullptr;
}

/* =========================================================================
 * Inline hook — 14-byte absolute JMP trampoline (x86-64)
 *
 *   FF 25 00 00 00 00   JMP QWORD PTR [RIP+0]
 *   <8 bytes: target>
 *
 * We also build a mini-trampoline in RWX memory:
 *   [0..13]   = first 14 bytes of original function (prologue)
 *   [14..27]  = JMP back to original+14
 *
 * So g_original_flip() = call the trampoline = execute original normally.
 * ========================================================================= */
#define HOOK_SIZE 14u

static uint8_t  g_orig_bytes[HOOK_SIZE]; /* saved prologue */
static uint8_t *g_trampoline = nullptr;  /* RWX page with trampoline */

/* Signature of sceGnmSubmitAndFlipCommandBuffers (PS5 SDK) */
typedef int (*GnmFlipFn)(uint32_t count,
                          void   **dcb,
                          uint32_t *dcb_size_dw,
                          void   **ccb,
                          uint32_t *ccb_size_dw,
                          uint32_t  video_out_handle,
                          uint32_t  display_buffer_index,
                          uint32_t  flip_mode,
                          int64_t   flip_arg);

static GnmFlipFn g_original_flip = nullptr; /* points into trampoline */

/* Atomic frame counter */
static volatile uint64_t g_frame_count = 0;

/* Our replacement function — called instead of the real Gnm flip */
static int hook_GnmFlip(uint32_t count,
                         void   **dcb,
                         uint32_t *dcb_size_dw,
                         void   **ccb,
                         uint32_t *ccb_size_dw,
                         uint32_t  video_out_handle,
                         uint32_t  display_buffer_index,
                         uint32_t  flip_mode,
                         int64_t   flip_arg)
{
    __atomic_fetch_add(&g_frame_count, 1, __ATOMIC_RELAXED);
    return g_original_flip(count, dcb, dcb_size_dw, ccb, ccb_size_dw,
                            video_out_handle, display_buffer_index,
                            flip_mode, flip_arg);
}

static void write_abs_jmp(void *dst, const void *target)
{
    uint8_t patch[HOOK_SIZE];
    patch[0] = 0xFF; patch[1] = 0x25;
    patch[2] = patch[3] = patch[4] = patch[5] = 0x00; /* [RIP+0] */
    uint64_t addr = (uint64_t)(uintptr_t)target;
    memcpy(patch + 6, &addr, 8);
    memcpy(dst, patch, HOOK_SIZE);
}

static bool install_hook(void *fn_addr)
{
    if (!fn_addr) return false;

    /* Allocate RWX trampoline page */
    g_trampoline = (uint8_t *)mmap(nullptr, 64,
                                    PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!g_trampoline || g_trampoline == MAP_FAILED)
        return false;

    /* Save the first HOOK_SIZE bytes of the original function */
    memcpy(g_orig_bytes, fn_addr, HOOK_SIZE);

    /* Build trampoline: original prologue bytes + JMP back to original+HOOK_SIZE */
    memcpy(g_trampoline, g_orig_bytes, HOOK_SIZE);
    write_abs_jmp(g_trampoline + HOOK_SIZE,
                  (uint8_t *)fn_addr + HOOK_SIZE);

    g_original_flip = (GnmFlipFn)(void *)g_trampoline;

    /* Make the original function's page writable so we can patch it.
     *
     * On a jailbroken PS5 the HV enforces XOM on system library text, so a plain
     * mprotect() may fail.  In that case the caller must use kernel_mprotect()
     * from ps5-payload-sdk.  Try both; if both fail the hook won't install but
     * the game continues normally. */
    uintptr_t page_start = (uintptr_t)fn_addr & ~(uintptr_t)0xFFF;
    int mp_ok = mprotect((void *)page_start, 0x3000,
                          PROT_READ | PROT_WRITE | PROT_EXEC);

    if (mp_ok != 0) {
        /* mprotect denied (HV/XOM).
         * TODO: call kernel_mprotect(getpid(), ...) from ps5-payload-sdk here.
         * For now we skip the hook rather than crashing the game.
         * Linking against <kernel.h>'s kernel_mprotect is all that's needed:
         *   kernel_mprotect(getpid(), (void*)page_start, 0x3000,
         *                   PROT_READ|PROT_WRITE|PROT_EXEC);
         */
        munmap(g_trampoline, 64);
        g_trampoline  = nullptr;
        g_original_flip = nullptr;
        return false;
    }

    /* Patch the first 14 bytes of the original with our JMP */
    write_abs_jmp(fn_addr, (const void *)hook_GnmFlip);

    /* Flush instruction cache on this core (x86-64: no-op but keep for clarity) */
    __builtin___clear_cache((char *)fn_addr, (char *)fn_addr + HOOK_SIZE);

    /* Restore page protection */
    mprotect((void *)page_start, 0x3000, PROT_READ | PROT_EXEC);

    return true;
}

/* =========================================================================
 * FPS sender thread — measures frames per second and sends as a double
 * over UDP to 127.0.0.1:29028 once per second.
 * ========================================================================= */
#define FPS_UDP_PORT 29028
#define FPS_INTERVAL_MS 1000

static void *fps_sender_thread(void *)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return nullptr;

    struct sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(FPS_UDP_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    uint64_t last_frame_count = 0;
    struct timespec ts_last;
    clock_gettime(CLOCK_MONOTONIC, &ts_last);

    for (;;) {
        usleep(FPS_INTERVAL_MS * 1000);

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);

        uint64_t now_frames = __atomic_load_n(&g_frame_count, __ATOMIC_RELAXED);
        uint64_t delta_frames = now_frames - last_frame_count;
        last_frame_count = now_frames;

        double elapsed_s = (double)(ts_now.tv_sec  - ts_last.tv_sec)
                         + (double)(ts_now.tv_nsec - ts_last.tv_nsec) * 1e-9;
        ts_last = ts_now;

        double fps = (elapsed_s > 0.0) ? ((double)delta_frames / elapsed_s) : 0.0;

        sendto(s, &fps, sizeof(fps), 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }

    close(s);
    return nullptr;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
int main(int /*argc*/, const char ** /*argv*/)
{
    /* ---- 1. Detect firmware version ---- */
    uint32_t fw_ver = detect_fw_version();

    /* ---- 2. Resolve the Gnm flip function ---- */
    void *flip_fn = nullptr;

    /* Primary: dynamic symbol lookup (FW-agnostic, no offsets needed) */
    flip_fn = dlsym_gnm_flip();

    /* Fallback: offset table */
    if (!flip_fn) {
        uintptr_t off = lookup_gnm_offset(fw_ver);
        if (off != 0) {
            int h = find_module_handle("SceGnmDriver");
            if (h >= 0) {
                void *base = gnm_module_text_base(h);
                if (base)
                    flip_fn = (void *)((uint8_t *)base + off);
            }
        }
    }

    /* ---- 3. Install the hook ---- */
    if (!flip_fn || !install_hook(flip_fn)) {
        /* Hook failed: exit cleanly without hanging the game.
         * The game will run normally; overlay_elf will just show "--" for FPS. */
        return 0;
    }

    /* ---- 4. Start FPS sender thread and stay alive ---- */
    pthread_t sender;
    if (pthread_create(&sender, nullptr, fps_sender_thread, nullptr) != 0)
        return 0;

    /* Stay alive: we MUST keep running because our hook (and trampoline) live
     * in this ELF's memory.  If we exit, the game crashes on the next flip. */
    for (;;) sleep(60);
    return 0;
}
