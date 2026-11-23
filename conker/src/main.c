#define _GNU_SOURCE
#include <sys/mman.h>
/**
 * Conker: Live and Reloaded — static recompilation host (Linux / Android).
 *
 * Title:       Conker: Live and Reloaded (Rare, 2005)
 * Title ID:    0x4D530051
 * Base addr:   0x00010000
 * Entry point: 0x0042F0E9
 * XDK:         5849 (D3D8LTCG — rendering goes through the NV2A pushbuffer executor)
 *
 * Boot order follows templates/new-game/src/main.c: load XBE, map the Xbox
 * memory layout, bring up the kernel shim and its bridge, set the stack, build
 * the flat dispatch table and enter the recompiled entry point. On POSIX the
 * guest runs on its own pthread and the process main thread owns the SDL
 * window (xbox_HostWindowMain); on Android SDL calls SDL_main on its thread.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <math.h>
#include <ctype.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dlfcn.h>
#include <ucontext.h>

#include <xbox/xboxrecomp.h>
#include "video_player.h"
#include "input/keyboard_sdl.h"
#include "input/keyboard.h"

#ifdef __ANDROID__
#include <SDL.h>
#include <SDL_syswm.h>
#include <android/log.h>
#endif
#if defined(XBOXRECOMP_NV2A_XEMU)
#include "nv2a_host.h"
extern int xbox_Nv2aXemuInit(const char *cache_dir, int null_renderer);
extern int xbox_Nv2aXemuFault(void *ucv, uintptr_t fault);
#endif

#define CONKER_ENTRY_POINT 0x0042F0E9u

#include "../../xboxrecomp/src/kernel/recomp_cpu_tls.h" /* was: extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp; */
#include "../../xboxrecomp/src/kernel/recomp_cpu_tls.h" /* was: extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi; */
extern ptrdiff_t g_xbox_mem_offset;

extern void xbe_entry_point(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern int recomp_dispatch_init(void);

static char  s_game_dir[1024];
static int   s_gpu_xemu, s_gpu_null;
static char  s_save_dir[1024];
static void *s_xbe_data;
static size_t s_xbe_size;

static int load_file(const char *path, void **out, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return 0; }
    *out = buf;
    *out_size = (size_t)sz;
    return 1;
}

/* ── Faults ──────────────────────────────────────────────────────────────
 * The runtime traps the MCPX apertures (APU, AC'97, OHCI) by leaving them
 * unmapped and decoding the faulting load/store; on POSIX the title owns the
 * signal handler and hands it each fault first (xbox_PosixMmioFault). What is
 * left is a real crash: report the host PC, the lifted function it is in
 * (sub_XXXXXXXX names the guest function) and the guest registers. */
static void crash_sym(const char *tag, void *pc)
{
    Dl_info di;
    if (pc && dladdr(pc, &di) && di.dli_sname)
        fprintf(stderr, "  %s %p %s+0x%lx\n", tag, pc, di.dli_sname,
                (unsigned long)((uintptr_t)pc - (uintptr_t)di.dli_saddr));
    else
        fprintf(stderr, "  %s %p\n", tag, pc);
}

/* ── Matrix watch (CONKER_MATWATCH) ─────────────────────────────────────
 * The renderer hands over the 3x4 world-view matrix of the world vertex
 * program (g_conker_mat). A thread finds where in guest memory those floats
 * live and write-protects those pages; the fault handler names the lifted
 * function that writes them. */
volatile int g_conker_mat_ready;
float g_conker_mat[12];
#define MW_RING 6000
static volatile uint32_t s_mw_only_lo, s_mw_only_hi;
static volatile uint32_t s_mw2_va;
typedef struct { uint32_t va; void *pc, *lr; void *fr[6]; } MwRec;
static MwRec s_mw_ring[MW_RING];
static volatile int s_mw_cnt;
static volatile uintptr_t s_mw_lo, s_mw_hi;   /* host range watched */
static volatile uint8_t s_mw_open[1024];      /* page opened since last re-arm */
static int matwatch_fault(void *ucv, uintptr_t fa)
{
    if (fa < s_mw_lo || fa >= s_mw_hi) return 0;
    uintptr_t pg = fa & ~(uintptr_t)4095;
    uint32_t fva = (uint32_t)(fa - (uintptr_t)g_xbox_mem_offset);
    int i = (s_mw_only_hi && (fva < s_mw_only_lo || fva >= s_mw_only_hi)) ? MW_RING :
            __atomic_fetch_add(&s_mw_cnt, 1, __ATOMIC_RELAXED);
    if (i < MW_RING) {
        MwRec *r = &s_mw_ring[i];
        r->va = (uint32_t)(fa - (uintptr_t)g_xbox_mem_offset);
#if defined(__aarch64__)
        ucontext_t *uc = (ucontext_t *)ucv;
        r->pc = (void *)uc->uc_mcontext.pc;
        r->lr = (void *)uc->uc_mcontext.regs[30];
        uintptr_t fp = uc->uc_mcontext.regs[29];
        for (int k = 0; k < 6; k++) {
            r->fr[k] = NULL;
            if (!fp || (fp & 7)) continue;
            uintptr_t *f = (uintptr_t *)fp;
            r->fr[k] = (void *)f[1];
            if (f[0] <= fp) { fp = 0; continue; }
            fp = f[0];
        }
#else
        (void)ucv;
#endif
    }
    mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE);
    s_mw_open[(pg - s_mw_lo) >> 12] = 1;
    return 1;
}
static void mw_print_rec(MwRec *r)
{
    fprintf(stderr, "[MATW] rec VA %08X\n", r->va);
    crash_sym("pc", r->pc);
    crash_sym("lr", r->lr);
    for (int k = 0; k < 6; k++) if (r->fr[k]) crash_sym("  ", r->fr[k]);
}
static int mw_find_clusters(const uint32_t *want, uint32_t *out, int max, uintptr_t lo, uintptr_t hi)
{
    int n = 0;
    for (uintptr_t a = lo; a + 48 <= hi && n < max; a += 4) {
        const uint32_t *w = (const uint32_t *)a;
        if (w[0] == want[0] && w[1] == want[1] && w[4] == want[4] && w[8] == want[8])
            out[n++] = (uint32_t)(a - (uintptr_t)g_xbox_mem_offset);
    }
    return n;
}
static void *matwatch2_thread(void *arg)
{
    (void)arg;
    while (!s_mw2_va) usleep(100000);
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t va = s_mw2_va;
    uintptr_t lo = (base + va) & ~(uintptr_t)4095, hi = ((base + va + 64 + 4095) & ~(uintptr_t)4095);
    fprintf(stderr, "[MATW2] watching writes to VA %08X..+64 (pages %08X..%08X)\n", va,
            (unsigned)(lo - base), (unsigned)(hi - base));
    s_mw_lo = lo; s_mw_hi = hi;
    mprotect((void *)lo, hi - lo, PROT_READ);
    s_mw_only_lo = va; s_mw_only_hi = va + 64;
    for (int t = 0; t < 60000 && s_mw_cnt < 3000; t++) {
        usleep(1000);
        for (uintptr_t p = lo; p < hi; p += 4096) {
            int idx = (int)((p - lo) >> 12);
            if (s_mw_open[idx]) { s_mw_open[idx] = 0; mprotect((void *)p, 4096, PROT_READ); }
        }
    }
    mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE);
    s_mw_lo = s_mw_hi = 0;
    int cnt = s_mw_cnt < MW_RING ? s_mw_cnt : MW_RING;
    fprintf(stderr, "[MATW2] %d faults\n", cnt);
    int shown = 0;
    for (int i = 0; i < cnt && shown < 40; i++) {
        if (s_mw_ring[i].va < va || s_mw_ring[i].va >= va + 64) continue;
        int dup = 0;
        for (int j = 0; j < i && !dup; j++)
            if (s_mw_ring[j].va >= va && s_mw_ring[j].va < va + 64 &&
                s_mw_ring[j].pc == s_mw_ring[i].pc && s_mw_ring[j].fr[0] == s_mw_ring[i].fr[0] &&
                s_mw_ring[j].fr[1] == s_mw_ring[i].fr[1]) dup = 1;
        if (dup) continue;
        shown++;
        mw_print_rec(&s_mw_ring[i]);
    }
    fprintf(stderr, "[MATW2] done\n");
    return NULL;
}
static void *matwatch_thread(void *arg)
{
    (void)arg;
    while (!g_conker_mat_ready) usleep(200000);
    sleep(2);
    uint32_t want[12];
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    /* where the push buffer holds the matrix now */
    uint32_t hits[64];
    int nh = 0;
    for (int tries = 0; tries < 40 && !nh; tries++) {
        memcpy(want, g_conker_mat, sizeof want);
        nh = mw_find_clusters(want, hits, 64, base + 0x80000000u, base + 0x84000000u);
        if (!nh) usleep(250000);
    }
    if (!nh) { fprintf(stderr, "[MATW] matrix not in the push buffer\n"); return NULL; }
    uint32_t mn = hits[0], mx = hits[0];
    for (int i = 0; i < nh; i++) { if (hits[i] < mn) mn = hits[i]; if (hits[i] > mx) mx = hits[i]; }
    uintptr_t lo = (base + mn - 0x40000) & ~(uintptr_t)4095;
    uintptr_t hi = (base + mx + 0x40000) & ~(uintptr_t)4095;
    if ((hi - lo) >> 12 > 1024) hi = lo + (1024 << 12);
    fprintf(stderr, "[MATW] %d copies in PB %08X..%08X; watching %08X..%08X\n", nh, mn, mx,
            (unsigned)(lo - base), (unsigned)(hi - base));
    s_mw_lo = lo; s_mw_hi = hi;
    mprotect((void *)lo, hi - lo, PROT_READ);
    for (int t = 0; t < 4000 && s_mw_cnt < MW_RING; t++) {   /* ~2 s */
        usleep(500);
        for (uintptr_t p = lo; p < hi; p += 4096) {
            int idx = (int)((p - lo) >> 12);
            if (s_mw_open[idx]) { s_mw_open[idx] = 0; mprotect((void *)p, 4096, PROT_READ); }
        }
    }
    mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE);
    s_mw_lo = s_mw_hi = 0;
    int cnt = s_mw_cnt < MW_RING ? s_mw_cnt : MW_RING;
    fprintf(stderr, "[MATW] %d faults recorded\n", cnt);
    /* matrix copies written during the window */
    nh = mw_find_clusters(want, hits, 64, lo, hi);
    fprintf(stderr, "[MATW] %d copies after the window\n", nh);
    int printed = 0;
    for (int h = 0; h < nh && printed < 12; h++) {
        /* the last fault at or before the copy, on its page or the one before */
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            uint32_t va = s_mw_ring[i].va;
            if (va <= hits[h] + 48 && va + 8192 > hits[h]) best = i;
        }
        if (best >= 0) {
            fprintf(stderr, "[MATW] copy at %08X, nearest write %08X:\n", hits[h], s_mw_ring[best].va);
            mw_print_rec(&s_mw_ring[best]);
            printed++;
        }
    }
    /* distinct writer chains overall */
    int shown = 0;
    for (int i = 0; i < cnt && shown < 30; i++) {
        int dup = 0;
        for (int j = 0; j < i && !dup; j++)
            if (s_mw_ring[j].pc == s_mw_ring[i].pc && s_mw_ring[j].fr[1] == s_mw_ring[i].fr[1]) dup = 1;
        if (dup) continue;
        shown++;
        mw_print_rec(&s_mw_ring[i]);
    }
    fprintf(stderr, "[MATW] done\n");
    return NULL;
}

/* CONKER_VSCPROBE: SetVertexShaderConstant (sub_00536C00) calls that load
 * c0..c2 (the world-view rows): source, caller and the rows. */
void conker_vsc_probe(uint32_t reg, uint32_t src, uint32_t cnt, uint32_t ret, uint32_t ebpv)
{
    static int on = -1, n;
    if (on < 0) on = getenv("CONKER_VSCPROBE") != NULL;
    if (!on || reg != 0x60 || cnt < 12) return;
    static uint32_t seen[64]; static int nseen, cnt_per[64];
    int k;
    for (k = 0; k < nseen; k++) if (seen[k] == ret) break;
    if (k == nseen && nseen < 64) { seen[nseen] = ret; cnt_per[nseen] = 0; nseen++; }
    if (k < 64 && cnt_per[k]++ >= 6) return;
    if (n++ > 400) return;
    const float *f = (const float *)((uintptr_t)g_xbox_mem_offset + src);
    float l0 = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]), l1 = sqrtf(f[4]*f[4]+f[5]*f[5]+f[6]*f[6]),
          l2 = sqrtf(f[8]*f[8]+f[9]*f[9]+f[10]*f[10]);
    float d01 = (f[0]*f[4]+f[1]*f[5]+f[2]*f[6]) / (l0*l1 + 1e-20f);
    uint32_t r2 = ebpv ? *(uint32_t *)((uintptr_t)g_xbox_mem_offset + ebpv + 4) : 0;
    fprintf(stderr, "[VSC] ret=%08X caller2=%08X src=%08X cnt=%u |r|=%.3f %.3f %.3f cos01=%.3f : "
            "%g %g %g %g | %g %g %g %g | %g %g %g %g\n",
            ret, r2, src, cnt, l0, l1, l2, d01, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
            f[8], f[9], f[10], f[11]);
}

/* CONKER_MATPROBE: the matrix-stack top that becomes the world-view rows
 * (sub_00407FF0), and -- with CONKER_MATWATCH2 -- who writes it. */
void conker_cam_probe(uint32_t src)
{
    static int n, on = -1;
    if (on < 0) on = getenv("CONKER_MATPROBE") != NULL;
    if (!on) return;
    const float *m = (const float *)((uintptr_t)g_xbox_mem_offset + src);
    if (n++ < 20)
        fprintf(stderr, "[CAMP] src=%08X M: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                src, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                m[12], m[13], m[14], m[15]);
    if (!s_mw2_va) s_mw2_va = src;
}
void conker_mat_probe(uint32_t edxv)
{
    static int on = -1, n;
    if (on < 0) on = getenv("CONKER_MATPROBE") != NULL;
    if (!on) return;
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t top = *(uint32_t *)(base + 0x84B868u);
    const float *m = (const float *)(base + top + 0x10);
    (void)0;
    if (n++ % 50 || n > 2000) return;
    fprintf(stderr, "[MATP] #%d top=%08X edx=%08X M: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
            n, top, edxv, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
            m[12], m[13], m[14], m[15]);
    if (edxv) {
        const float *e = (const float *)(base + edxv);
        fprintf(stderr, "[MATP]   W: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11],
                e[12], e[13], e[14], e[15]);
    }
}

/* A fault the handlers claim but that comes straight back (same pc, same
 * address) is a livelock: say where, once per site. */
static __thread uintptr_t s_fs_pc, s_fs_addr;
static __thread unsigned s_fs_n;
static void fault_storm_check(void *ucv, uintptr_t fa, int who)
{
#if defined(__aarch64__)
    uintptr_t pc = ((ucontext_t *)ucv)->uc_mcontext.pc;
#else
    uintptr_t pc = 0; (void)ucv;
#endif
    if (pc == s_fs_pc && fa == s_fs_addr) {
        if (++s_fs_n == 20000) {
            uint32_t va = (fa >= (uintptr_t)g_xbox_mem_offset &&
                           fa - (uintptr_t)g_xbox_mem_offset < 0x100000000ull)
                          ? (uint32_t)(fa - (uintptr_t)g_xbox_mem_offset) : 0;
            fprintf(stderr, "[FAULTSTORM] tid %d: 20000x same fault host %p guest VA 0x%08X "
                            "claimed by %s, insn %08X\n", (int)gettid(), (void *)fa, va,
                    who == 1 ? "nv2a" : "mmio", *(uint32_t *)pc);
            crash_sym("pc", (void *)pc);
#if defined(__aarch64__)
            crash_sym("lr", (void *)((ucontext_t *)ucv)->uc_mcontext.regs[30]);
#endif
            fprintf(stderr, "  guest eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                    g_eax, g_ecx, g_edx, g_esi, g_edi);
        }
    } else {
        s_fs_pc = pc; s_fs_addr = fa; s_fs_n = 0;
    }
}

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    if (s_mw_hi && matwatch_fault(ucv, (uintptr_t)si->si_addr))
        return;
#if defined(XBOXRECOMP_NV2A_XEMU)
    if (xbox_Nv2aXemuFault(ucv, (uintptr_t)si->si_addr)) {
        fault_storm_check(ucv, (uintptr_t)si->si_addr, 1);
        return;
    }
#endif
    if (xbox_PosixMmioFault(ucv, (uintptr_t)si->si_addr)) {
        fault_storm_check(ucv, (uintptr_t)si->si_addr, 2);
        return;
    }
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t fa = (uintptr_t)si->si_addr;
    fprintf(stderr, "\n[CRASH] signal %d at host %p", sig, (void *)fa);
    if (fa >= (uintptr_t)g_xbox_mem_offset && fa - (uintptr_t)g_xbox_mem_offset < 0x100000000ull)
        fprintf(stderr, " = guest VA 0x%08X", (unsigned)(fa - (uintptr_t)g_xbox_mem_offset));
    fprintf(stderr, "\n");
unwind:
#if defined(__aarch64__)
    crash_sym("pc", (void *)uc->uc_mcontext.pc);
    crash_sym("lr", (void *)uc->uc_mcontext.regs[30]);
    {   /* frame-pointer walk: lifted calls are native calls */
        /* only inside this thread's own stack: lifted code uses x29 as an
         * ordinary register, and following a garbage chain faulted inside
         * the REC stack dump (the crash on REC) */
        uintptr_t fp = uc->uc_mcontext.regs[29];
        uintptr_t lo = uc->uc_mcontext.sp, hi = lo + 2 * 1024 * 1024;
        for (int i = 0; i < 12 && fp && !(fp & 7) && fp >= lo && fp + 16 <= hi; i++) {
            uintptr_t *f = (uintptr_t *)fp;
            crash_sym("  ", (void *)f[1]);
            if (f[0] <= fp || f[0] - fp > 256 * 1024) break;
            fp = f[0];
        }
    }
#elif defined(__x86_64__)
    crash_sym("pc", (void *)uc->uc_mcontext.gregs[REG_RIP]);
#endif
    fprintf(stderr, "  guest eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_fault_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* CONKER_STACKDUMP_SECS=<n>: every n seconds the guest thread prints its
 * native call chain (the lifted functions it is in). For hangs. */
static pthread_t s_guest_thread;
static void dump_stack_here(void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    if (!pthread_equal(pthread_self(), s_guest_thread)) {
        fprintf(stderr, "[STACK] host thread tid %d\n", (int)gettid());
        goto unwind;
    }
    fprintf(stderr, "[STACK] guest thread, esp=%08X eax=%08X ecx=%08X\n", g_esp, g_eax, g_ecx);
    if (getenv("CONKER_DUMP_AC97")) {
        volatile uint8_t *m = (volatile uint8_t *)((uintptr_t)g_xbox_mem_offset);
        fprintf(stderr, "[STACK] AC97 ctl:");
        for (int ch = 0; ch < 8; ch++) fprintf(stderr, " %02X", m[0xFEC0010Bu + ch * 0x10]);
        fprintf(stderr, " | blk2:");
        for (int ch = 0; ch < 8; ch++) fprintf(stderr, " %02X", m[0xFEC0210Bu + ch * 0x10]);
        volatile uint32_t *t = (volatile uint32_t *)(m + 0x4E1608u);
        fprintf(stderr, " | table");
        for (int i = 0; i < 8; i++) fprintf(stderr, " %X", t[i]);
        fprintf(stderr, "\n[STACK] scan for RR bytes in 0xFEC00000..0xFEC10000:");
        for (uint32_t a = 0xFEC00000u; a < 0xFEC10000u; a++)
            if ((a & 0xF) == 0xB && m[a] == 0x02) fprintf(stderr, " %08X", a);
        fprintf(stderr, "\n");
    }
unwind:
#if defined(__aarch64__)
    crash_sym("pc", (void *)uc->uc_mcontext.pc);
    crash_sym("lr", (void *)uc->uc_mcontext.regs[30]);
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 24 && fp && !(fp & 7); i++) {
        uintptr_t *f = (uintptr_t *)fp;
        crash_sym("  ", (void *)f[1]);
        if (f[0] <= fp) break;
        fp = f[0];
    }
#else
    (void)uc;
#endif
}
static void stackdump_handler(int sig, siginfo_t *si, void *ucv) { (void)sig; (void)si; dump_stack_here(ucv); }
static void *stackdump_thread(void *arg)
{
    int secs = (int)(intptr_t)arg;
    for (int round = 0;; round++) {
        sleep((unsigned)secs);
        pthread_kill(s_guest_thread, SIGUSR2);
        usleep(50000);
        /* Every other round: every thread of the process, for host-side hangs
         * (PFIFO, renderer, present). */
        if (!(round & 1) || !getenv("CONKER_STACKDUMP_ALL")) continue;
        DIR *dir = opendir("/proc/self/task");
        if (!dir) continue;
        struct dirent *de;
        pid_t self = gettid();
        while ((de = readdir(dir))) {
            pid_t tid = (pid_t)atoi(de->d_name);
            if (tid <= 0 || tid == self) continue;
            char path[64], name[32] = "?";
            snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
            FILE *f = fopen(path, "r");
            if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
            fprintf(stderr, "[STACK] --- tid %d \"%s\"\n", (int)tid, name);
            syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
            usleep(30000);
        }
        closedir(dir);
    }
    return NULL;
}
/* ── Thread placement (CONKER_NO_PIN=1 turns it off) ──────────────────────
 * The scheduler kept moving the busy threads (the title's main thread, the
 * GPU's PFIFO thread) onto the little cores, whose clock is half the big
 * ones'. Every 2 s: the two busiest threads get the two biggest cores
 * (Pixel 6: 6-7, Cortex-X1), the next two the middle pair (4-5); the rest
 * may run anywhere. A busiest thread keeps its place when the order only
 * shuffles. Core groups are found from cpuinfo_max_freq. */
/* CONKER_FSCAN=<s1>[,<s2>...]: at those seconds after start, log the
 * title's .data/.bss words that hold an aspect-like float (16:9, 9:16, 4:3,
 * 3:4), and how many the heap holds. Finding the projection's aspect. */
extern ptrdiff_t g_xbox_mem_offset;
static void *fscan_thread(void *arg)
{
    (void)arg;
    sleep(5);
    const char *e = getenv("CONKER_FSCAN");
    if (!e || !*e) return NULL;
    if (!strcmp(e, "1")) e = "100,150,185";
    static const float want[] = { 16.0f/9.0f, 9.0f/16.0f, 4.0f/3.0f, 0.75f };
    int last = 0;
    while (e && *e) {
        int t = atoi(e);
        sleep((unsigned)(t > last + 5 ? t - last - 5 : 0)); last = t - 5;
        const float *base = (const float *)(uintptr_t)(0x0064F980u + g_xbox_mem_offset);
        size_t n = (0x00881A80u - 0x0064F980u) / 4;
        int logged = 0;
        for (size_t i = 0; i < n && logged < 400; i++) {
            float v = base[i];
            for (int k = 0; k < 4; k++)
                if (fabsf(v - want[k]) < 2e-4f) {
                    fprintf(stderr, "[FSCAN] t=%d %08X = %.6f\n", t,
                            (unsigned)(0x0064F980u + i * 4), v);
                    logged++;
                }
        }
        unsigned heap[4] = {0};
        const float *h = (const float *)(uintptr_t)(0x00900000u + g_xbox_mem_offset);
        for (size_t i = 0; i < (0x03F00000u - 0x00900000u) / 4; i++)
            for (int k = 0; k < 4; k++) if (fabsf(h[i] - want[k]) < 2e-5f) heap[k]++;
        fprintf(stderr, "[FSCAN] t=%d heap 16:9=%u 9:16=%u 4:3=%u 3:4=%u\n", t,
                heap[0], heap[1], heap[2], heap[3]);
        e = strchr(e, ',');
        if (e) e++;
    }
    return NULL;
}

/* CONKER_MEMSNAP=1: every 5 s from 20 s, the title's .data/.bss
 * (0x0064F980..0x00891D10) to <ext>/dump/mem_<sec>.bin -- diffed offline
 * against the screenshots to find the "player in control" state. */
static char s_snap_dir[1024];
static void *memsnap_thread(void *arg)
{
    (void)arg;
    sleep(5);
    if (getenv("CONKER_ICALL_CUT") && s_snap_dir[0]) {
        /* autotests: count indirect calls from boot, labelled cutscene */
        extern int conker_label_set(int state);
        conker_label_set(1);
    }
    if (!getenv("CONKER_MEMSNAP") || !s_snap_dir[0]) return NULL;
    sleep(15);
    for (int t = 20; t <= 900; t += 5) {
        char path[1100];
        snprintf(path, sizeof path, "%s/mem_%03d.bin", s_snap_dir, t);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite((const void *)(uintptr_t)(0x0064F980u + g_xbox_mem_offset), 1,
                   0x00891D10u - 0x0064F980u, f);
            fclose(f);
        }
        sleep(5);
    }
    return NULL;
}

#include <sched.h>
static void *pin_thread(void *arg)
{
    (void)arg;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu > 16) ncpu = 16;
    long maxf[16] = {0};
    for (int c = 0; c < ncpu; c++) {
        char path[96]; snprintf(path, sizeof path,
            "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(path, "r");
        if (f) { if (fscanf(f, "%ld", &maxf[c]) != 1) maxf[c] = 0; fclose(f); }
    }
    /* order cores fastest first */
    int order[16];
    for (int c = 0; c < ncpu; c++) order[c] = c;
    for (int i = 0; i < ncpu; i++)
        for (int j = i + 1; j < ncpu; j++)
            if (maxf[order[j]] > maxf[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    if (ncpu < 4 || maxf[order[0]] == maxf[order[ncpu - 1]]) {
        fprintf(stderr, "[PIN] uniform cores, nothing to do\n");
        return NULL;
    }
    cpu_set_t big, mid, all;
    CPU_ZERO(&big); CPU_ZERO(&mid); CPU_ZERO(&all);
    for (int c = 0; c < ncpu; c++) CPU_SET(c, &all);
    /* Cores are chosen by what they can do NOW, not by their rated top
     * speed: under heat the X1 pair is held at ~1 GHz for whole sessions
     * while the A76 pair still runs 2.1+ GHz -- the busiest threads stayed
     * on the throttled pair and every heavy scene ran at half speed.
     * capacity(now) = cpu_capacity * thermal cap / rated max. */
    long capy[16] = {0};
    int cl[16]; int ncl = 0; long clmax[16];   /* clusters by rated max */
    for (int c = 0; c < ncpu; c++) {
        char path[96]; snprintf(path, sizeof path,
            "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        FILE *f = fopen(path, "r");
        if (f) { if (fscanf(f, "%ld", &capy[c]) != 1) capy[c] = 0; fclose(f); }
        int k = 0;
        for (; k < ncl; k++) if (clmax[k] == maxf[c]) break;
        if (k == ncl) clmax[ncl++] = maxf[c];
        cl[c] = k;
    }
    static int nodyn = -1;
    if (nodyn < 0) nodyn = getenv("CONKER_PIN_STATIC") != NULL;
    int cur_cl = -1, want_cl = -1, want_n = 0;
    long obs[16] = {0}; time_t obs_t[16] = {0};   /* per core: speed seen while busy */
    /* first choice: the fastest cluster, as before */
    CPU_SET(order[0], &big); CPU_SET(order[1], &big);
    CPU_SET(order[2], &mid); CPU_SET(order[3], &mid);
    cur_cl = cl[order[0]];
    fprintf(stderr, "[PIN] big cores %d,%d (%ld MHz), mid %d,%d (%ld MHz), capacity %ld/%ld\n",
            order[0], order[1], maxf[order[0]] / 1000, order[2], order[3],
            maxf[order[2]] / 1000, capy[order[0]], capy[order[2]]);
    enum { MAXT = 160 };
    int tids[MAXT]; unsigned long long last[MAXT]; int nt = 0;
    int pinned[4] = { 0, 0, 0, 0 };
    pid_t self = gettid();
    for (;;) {
        sleep(2);
        int cur[MAXT]; unsigned long long now_t[MAXT]; int nc = 0;
        DIR *dir = opendir("/proc/self/task");
        struct dirent *de;
        while (dir && (de = readdir(dir)) && nc < MAXT) {
            int tid = atoi(de->d_name);
            if (tid <= 0 || tid == self) continue;
            char path[64], buf[512];
            snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
            FILE *f = fopen(path, "r"); if (!f) continue;
            size_t rr = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[rr] = 0;
            char *rp = strrchr(buf, ')');
            if (!rp) continue;
            unsigned long ut = 0, st = 0;
            sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
            cur[nc] = tid; now_t[nc] = ut + st; nc++;
        }
        if (dir) closedir(dir);
        /* the four busiest over the last period */
        int top[4] = { 0, 0, 0, 0 }; unsigned long long topv[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < nc; i++) {
            unsigned long long prev = 0; int found = 0;
            for (int j = 0; j < nt; j++) if (tids[j] == cur[i]) { prev = last[j]; found = 1; }
            if (!found) continue;
            unsigned long long d = now_t[i] - prev;
            for (int k = 0; k < 4; k++) {
                if (d > topv[k]) {
                    for (int m = 3; m > k; m--) { topv[m] = topv[m - 1]; top[m] = top[m - 1]; }
                    topv[k] = d; top[k] = cur[i];
                    break;
                }
            }
        }
        /* Audio output threads (SDL's feeder, AAudio's) to audio priority,
         * Android's THREAD_PRIORITY_URGENT_AUDIO: SDL's own request does
         * nothing on Android, and at nice 0 they were starved by the
         * emulator's busy threads -- the output ran dry (crackle, hiss). */
        for (int i = 0; i < nc; i++) {
            static int boosted[32], nboost;
            int done = 0;
            for (int b = 0; b < nboost; b++) if (boosted[b] == cur[i]) done = 1;
            if (done || nboost >= 32) continue;
            char path[64], name[32] = "";
            snprintf(path, sizeof path, "/proc/self/task/%d/comm", cur[i]);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            if (!fgets(name, sizeof name, f)) name[0] = 0;
            fclose(f);
            if (!strncmp(name, "SDLAudio", 8) || !strncmp(name, "AAudio", 6) ||
                !strncmp(name, "AudioTrack", 10) || !strncmp(name, "apu.frame", 9)) {
                int r = setpriority(PRIO_PROCESS, (id_t)cur[i], -19);
                boosted[nboost++] = cur[i];
                name[strcspn(name, "\n")] = 0;
                fprintf(stderr, "[PIN] audio thread %s/%d -> nice -19%s\n", name, cur[i],
                        r ? " (refused)" : "");
            }
        }
        nt = nc; memcpy(tids, cur, sizeof(int) * nc);
        memcpy(last, now_t, sizeof(unsigned long long) * nc);
        long hz = sysconf(_SC_CLK_TCK);
        for (int k = 0; k < 4; k++) {
            /* a thread under 25% of a core is not worth a big core */
            if (!top[k] || topv[k] * 100 < (unsigned long long)hz * 2 * 25) top[k] = 0;
        }
        /* pick the cores with the most capacity right now: per core, not
         * per cluster, so a lone prime core (Snapdragon 1+3+4, 1+5+2...)
         * pairs with the best of the next cluster, and comes back once it
         * cools down. */
        if (!nodyn) {
            long eff[16] = {0};
            time_t tn = time(NULL);
            for (int c = 0; c < ncpu; c++) {
                char path[96]; long cap = 0, fc = 0;
                snprintf(path, sizeof path,
                    "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", c);
                FILE *f = fopen(path, "r");
                if (f) { if (fscanf(f, "%ld", &cap) != 1) cap = 0; fclose(f); }
                if (cap <= 0 || cap > maxf[c]) cap = maxf[c];
                if (CPU_ISSET(c, &big)) {
                    /* busy core: its speed stuck under the cap is a cap
                     * the governor does not show */
                    snprintf(path, sizeof path,
                        "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
                    f = fopen(path, "r");
                    if (f) { if (fscanf(f, "%ld", &fc) != 1) fc = 0; fclose(f); }
                    if (fc > 0 && fc < cap) cap = fc;
                    if (fc > 0) { obs[c] = fc; obs_t[c] = tn; }
                } else if (obs[c] > 0 && tn - obs_t[c] < 30 && obs[c] < cap) {
                    cap = obs[c];   /* left throttled under 30 s ago: still is */
                }
                long cp = capy[c] > 0 ? capy[c] : maxf[c] / 3000;
                eff[c] = maxf[c] ? cp * (cap / 1000) / (maxf[c] / 1000) : 0;
            }
            int rk[16];
            for (int c = 0; c < ncpu; c++) rk[c] = c;
            for (int i = 0; i < ncpu; i++)
                for (int j = i + 1; j < ncpu; j++)
                    if (eff[rk[j]] > eff[rk[i]]) { int t = rk[i]; rk[i] = rk[j]; rk[j] = t; }
            long ec = 0, eb = eff[rk[0]] + eff[rk[1]];
            for (int c = 0; c < ncpu; c++) if (CPU_ISSET(c, &big)) ec += eff[c];
            int same = CPU_ISSET(rk[0], &big) && CPU_ISSET(rk[1], &big);
            /* hysteresis: 20 % more, seen twice in a row (4 s) */
            if (!same && eb * 100 > ec * 120) {
                int key = rk[0] * 16 + rk[1];
                if (want_cl == key) want_n++; else { want_cl = key; want_n = 1; }
                if (want_n >= 2) {
                    CPU_ZERO(&big); CPU_ZERO(&mid);
                    CPU_SET(rk[0], &big); CPU_SET(rk[1], &big);
                    CPU_SET(rk[2], &mid); CPU_SET(rk[3], &mid);
                    fprintf(stderr, "[PIN] cores %d,%d -> %d,%d: capacity now %ld vs %ld\n",
                            order[0], order[1], rk[0], rk[1], ec, eb);
                    order[0] = rk[0]; order[1] = rk[1];
                    want_cl = -1; want_n = 0;
                    for (int m = 0; m < 4; m++) pinned[m] = 0;   /* re-pin all */
                }
            } else { want_cl = -1; want_n = 0; }
        }
        /* release threads that dropped out */
        for (int k = 0; k < 4; k++) {
            if (!pinned[k]) continue;
            int still = 0;
            for (int m = 0; m < 4; m++) if (top[m] == pinned[k]) still = 1;
            if (!still) { sched_setaffinity(pinned[k], sizeof all, &all); pinned[k] = 0; }
        }
        for (int k = 0; k < 4; k++) {
            if (!top[k]) continue;
            /* CONKER_PIN_SPLIT=1: busiest on the big pair, second on the
             * mid pair -- two loaded X1s throttle to ~1.3 GHz in the heavy
             * scenes while the A76s idle at 400 MHz. */
            static int split = -1;
            if (split < 0) split = getenv("CONKER_PIN_SPLIT") != NULL;
            const cpu_set_t *set = split ? (k == 0 ? &big : k == 1 ? &mid : &all)
                                         : (k < 2 ? &big : &mid);
            sched_setaffinity(top[k], sizeof(cpu_set_t), set);
            int have = 0;
            for (int m = 0; m < 4; m++) if (pinned[m] == top[k]) have = 1;
            if (!have) for (int m = 0; m < 4; m++) if (!pinned[m]) { pinned[m] = top[k]; break; }
        }
        static int logged;
        if (logged++ < 5)
            fprintf(stderr, "[PIN] big: %d %d, mid: %d %d\n", top[0], top[1], top[2], top[3]);
    }
    return NULL;
}

/* Resolution governor: x2 while the frame keeps 30 fps, x1 when it does
 * not (sustained, not a load spike); back to x2 only when the GPU time at
 * x1 says x2 would fit, and never sooner than 20 s after a drop. */
/* Resolution fallback: x2 by default; if the game holds under ~21 fps for
 * 6 s outside loads (a phone too weak for x2), drop to x1 once and stay
 * there for the session. One switch at most: no flicker back and forth. */
static void *export_spv_cache(void *arg)
{
    const char *sd = arg;
    sleep(3);
    const char *dd = getenv("NV2A_DUMP_DIR");
    if (!dd) return NULL;
    DIR *dir = opendir(sd);
    if (!dir) return NULL;
    char tmp[1200], fin[1200];
    snprintf(tmp, sizeof tmp, "%s/spv_cache.pack.tmp", dd);
    snprintf(fin, sizeof fin, "%s/spv_cache.pack", dd);
    FILE *fo = fopen(tmp, "wb");
    if (!fo) { closedir(dir); return NULL; }
    uint32_t count = 0;
    fwrite("SPVP", 1, 4, fo); fwrite(&count, 4, 1, fo);
    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t nl = strlen(de->d_name);
        if (nl < 5 || nl > 60 || strcmp(de->d_name + nl - 4, ".spv")) continue;
        char fp[1300]; snprintf(fp, sizeof fp, "%s/%s", sd, de->d_name);
        FILE *fi = fopen(fp, "rb");
        if (!fi) continue;
        fseek(fi, 0, SEEK_END); long len = ftell(fi); fseek(fi, 0, SEEK_SET);
        if (len < 20 || len > (16 << 20)) { fclose(fi); continue; }
        void *buf = malloc(len);
        if (buf && fread(buf, 1, len, fi) == (size_t)len) {
            uint16_t n16 = (uint16_t)nl; uint32_t l32 = (uint32_t)len;
            fwrite(&n16, 2, 1, fo); fwrite(de->d_name, 1, nl, fo);
            fwrite(&l32, 4, 1, fo); fwrite(buf, 1, len, fo);
            count++;
        }
        free(buf); fclose(fi);
    }
    closedir(dir);
    fseek(fo, 4, SEEK_SET); fwrite(&count, 4, 1, fo); fclose(fo);
    rename(tmp, fin);
    fprintf(stderr, "[BOOT] SPIR-V cache exported: %u shaders -> spv_cache.pack\n", count);
    return NULL;
}

static void *drs_thread(void *arg)
{
    (void)arg;
    extern volatile double nv2a_frame_ms_ema, nv2a_gpu_ms_ema;
    extern volatile unsigned nv2a_flips;
    extern void nv2a_host_set_scale(unsigned scale);
    extern unsigned nv2a_host_get_scale(void);
    pthread_setname_np(pthread_self(), "conker.drs");
    /* thermal governor: Android's own forecast of how close the phone is to
     * its throttling step. At ~0.95 the Pixel 6 clamps the mid and big cores
     * to 400-500 MHz at once and the game falls to 5 fps for minutes (seen
     * at 10:04:35, a session 5 minutes in). Rendering at x1 before that
     * (a quarter of the pixels: the GPU is a large share of the heat) keeps
     * the phone under the step; x2 comes back once it has cooled.
     * CONKER_NO_THERMAL=1: off. */
    void *tmgr = NULL; float (*thead)(void *, int) = NULL;
    /* off by default: the player wants x2 always; the fix for heat is
     * less work, not fewer pixels. CONKER_THERMAL=1 turns it on. */
    if (getenv("CONKER_THERMAL")) {
        void *h = dlopen("libandroid.so", RTLD_NOW);
        void *(*acq)(void) = h ? (void *(*)(void))dlsym(h, "AThermal_acquireManager") : NULL;
        thead = h ? (float (*)(void *, int))dlsym(h, "AThermal_getThermalHeadroom") : NULL;
        tmgr = acq ? acq() : NULL;
        if (!tmgr || !thead) fprintf(stderr, "[THERM] no thermal headroom API: governor off\n");
    }
    int therm_x1 = 0, cool_s = 0, tick = 0;
    sleep(30);
    /* only now: at thread start the GPU model does not exist yet and the
     * scale read back 1, which switched the governor off */
    unsigned user_scale = nv2a_host_get_scale();
    fprintf(stderr, "[THERM] render scale x%u, governor %s\n", user_scale,
            tmgr && thead && user_scale >= 2 ? "on" : "off");
    int slow = 0;
    unsigned last_flips = nv2a_flips;
    for (;;) {
        usleep(250000);
        if (tmgr && thead && user_scale >= 2 && (++tick % 4) == 0) {
            float h10 = thead(tmgr, 10);          /* forecast, 10 s ahead */
            extern volatile float nv2a_thermal_headroom;
            static int said;
            if (!said++) fprintf(stderr, "[THERM] governor on, headroom forecast %.2f\n", h10);
            if (h10 == h10) nv2a_thermal_headroom = h10;
            if (h10 == h10) {                     /* NaN: no data yet */
                /* x2 is the picture the player wants: give it up only right
                 * at the clamp (it came at ~0.95; the forecast is 10 s
                 * ahead), take it back as soon as the phone is off that edge.
                 * A warm phone sits at 0.78-0.87 for whole sessions and is
                 * fine there: an earlier 0.75 threshold kept x1 for good. */
                static int dwell = 60;   /* seconds since the last switch (none yet) */
                dwell++;
                if (!therm_x1 && h10 >= 0.88f && dwell >= 60 && nv2a_host_get_scale() >= 2) {
                    fprintf(stderr, "[THERM] phone at the throttling edge (headroom %.2f in 10 s): x1 until it cools\n", h10);
                    nv2a_host_set_scale(1);
                    therm_x1 = 1; cool_s = 0; dwell = 0;
                } else if (therm_x1) {
                    cool_s = h10 <= 0.80f ? cool_s + 1 : 0;
                    if (cool_s >= 20 && dwell >= 60) {
                        dwell = 0;
                        fprintf(stderr, "[THERM] cooled down (headroom %.2f): back to x%u\n", h10, user_scale);
                        nv2a_host_set_scale(user_scale);
                        therm_x1 = 0; cool_s = 0;
                    }
                }
            }
        }
        unsigned fl = nv2a_flips;
        bool moving = fl - last_flips >= 3;
        last_flips = fl;
        double fm = nv2a_frame_ms_ema;
        if (!moving || fm > 90.0) { slow = 0; continue; }   /* loading */
        /* only when the GPU is what is slow: a slow frame with the GPU done
         * in 26 ms (CPU-bound, or pipelines building after a load) gains
         * nothing at x1 but a blurrier picture -- the drop seen 11 s into
         * the barn. Nor in the first 30 s after a level load. */
        extern volatile int64_t g_xbox_load_mark_ms;
        struct timespec dt; clock_gettime(CLOCK_MONOTONIC, &dt);
        int64_t dnow = (int64_t)dt.tv_sec * 1000 + dt.tv_nsec / 1000000;
        if (dnow - g_xbox_load_mark_ms < 30000) { slow = 0; continue; }
        slow = (fm > 47.0 && nv2a_gpu_ms_ema > 36.0) ? slow + 1 : 0;
        if (slow >= 24 && nv2a_host_get_scale() >= 2) {
            fprintf(stderr, "[DRS] too slow for x2 (frame %.1f ms, GPU %.1f ms): x1 for this session\n",
                    fm, nv2a_gpu_ms_ema);
            nv2a_host_set_scale(1);
            user_scale = 1;   /* for the session: the thermal governor stays out */
        }
    }
    return NULL;
}

/* every thread's stack, once (REC: a hang is diagnosable from the log) */
static void *dump_all_threads(void *arg)
{
    (void)arg;
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return NULL;
    struct dirent *de;
    pid_t self = gettid();
    while ((de = readdir(dir))) {
        pid_t tid = (pid_t)atoi(de->d_name);
        if (tid <= 0 || tid == self) continue;
        char path[64], name[32] = "?";
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
        FILE *f = fopen(path, "r");
        if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
        fprintf(stderr, "[STACK] --- tid %d \"%s\"\n", (int)tid, name);
        syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
        usleep(30000);
    }
    closedir(dir);
    return NULL;
}

/* The process log at a freeze, saved to <ext>/dump/hang_<time>.txt: the log
 * itself does not survive a restart (the next launch, or a test runner,
 * clears it), so a hang the user meets would otherwise be lost. Also kept:
 * the previous file of each kind is overwritten only by a newer hang. */
static volatile int s_in_background;
static void save_hang_log(const char *kind)
{
    if (!s_snap_dir[0] || s_in_background) return;
    sleep(2);                         /* let the stack dump reach the log */
    char cmd[1400];
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    snprintf(cmd, sizeof cmd, "logcat -d --pid=%d > '%s/hang_%s_%02d%02d%02d.txt' 2>&1",
             (int)getpid(), s_snap_dir, kind, tm.tm_hour, tm.tm_min, tm.tm_sec);
    int rc = system(cmd);
    fprintf(stderr, "[HANG] log saved to %s/hang_%s_%02d%02d%02d.txt (rc %d)\n",
            s_snap_dir, kind, tm.tm_hour, tm.tm_min, tm.tm_sec, rc);
}

/* hang watchdog: no flip for 6 s -> every thread's stack once, so a freeze
 * the user meets is in the log (REC saves it) */
static void *hang_watch(void *arg)
{
    (void)arg;
    extern volatile unsigned nv2a_flip_seq;
    unsigned last = 0; int still = 0, dumped = 0;
    for (;;) {
        sleep(1);
        unsigned f = nv2a_flip_seq;
        {   /* the game flips but nothing reaches the screen (the picture
             * froze on one frame while the game ran on) */
            extern volatile unsigned nv2a_presented;
            static unsigned lp, lf; static int pstill, pdumped;
            if (nv2a_presented != lp || f == lf) { lp = nv2a_presented; pstill = 0; pdumped = 0; }
            else if (++pstill == 3 && !pdumped) {
                pdumped = 1;
                fprintf(stderr, "[HANG] the game flips (frame %u) but nothing presented for 3 s (presented %u): stacks follow\n",
                        f, nv2a_presented);
                dump_all_threads(NULL);
                save_hang_log("nopresent");
            }
            lf = f;
        }
        if (f != last || f == 0) { last = f; still = 0; dumped = 0; continue; }
        if (++still == 6 && !dumped) {
            dumped = 1;
            fprintf(stderr, "[HANG] no frame for 6 s (frame %u): stacks follow\n", f);
            dump_all_threads(NULL);
            if (f > 1) save_hang_log("noflip");   /* frame 1: still the boot black screen */
        }
    }
    return NULL;
}

static void start_stackdump(void)
{
    {   pthread_t t;
        if (!getenv("CONKER_NO_HANGWATCH") && pthread_create(&t, NULL, hang_watch, NULL) == 0)
            pthread_detach(t); }
    const char *e = getenv("CONKER_STACKDUMP_SECS");
    {   struct sigaction sa;   /* the handler always: REC dumps every thread */
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = stackdump_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGUSR2, &sa, NULL);
        s_guest_thread = pthread_self(); }
    if (!e || atoi(e) <= 0) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = stackdump_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);
    s_guest_thread = pthread_self();
    pthread_t t;
    pthread_create(&t, NULL, stackdump_thread, (void *)(intptr_t)atoi(e));
    pthread_detach(t);
}

/* ── Sampling profiler (CONKER_PROF) ─────────────────────────────────────
 * Every 5 s: CPU use of each thread (from /proc), then for the busy threads
 * a histogram of where they are -- the native function their PC (and the
 * caller from LR) is in. Lifted functions are sub_<guest address>. */
#define PROF_RING 16384
#define PROF_STK 28
typedef struct { int tid; void *pc, *lr, *fp1; uint8_t nst; uintptr_t st[PROF_STK]; } ProfSample;
static ProfSample s_prof[PROF_RING];
static volatile unsigned s_prof_n;
static void prof_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
#if defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    unsigned i = __atomic_fetch_add(&s_prof_n, 1, __ATOMIC_RELAXED);
    if (i >= PROF_RING) return;
    s_prof[i].tid = (int)gettid();
    s_prof[i].pc = (void *)uc->uc_mcontext.pc;
    s_prof[i].lr = (void *)uc->uc_mcontext.regs[30];
    uintptr_t fp = uc->uc_mcontext.regs[29];
    uintptr_t sp = uc->uc_mcontext.sp;
    /* only a frame pointer inside this thread's stack is followed: leaf
     * code in system libraries keeps other values in x29 */
    s_prof[i].fp1 = (fp && !(fp & 7) && fp >= sp && fp - sp < 0x100000)
                    ? (void *)((uintptr_t *)fp)[1] : NULL;
    /* the whole frame-pointer chain, for inclusive (call-tree) costs */
    {
        int k = 0;
        s_prof[i].st[k++] = (uintptr_t)s_prof[i].pc;
        s_prof[i].st[k++] = (uintptr_t)s_prof[i].lr;
        uintptr_t f = fp, lim = sp + 0x100000;
        while (k < PROF_STK && f && !(f & 7) && f >= sp && f < lim) {
            uintptr_t ra = ((uintptr_t *)f)[1];
            uintptr_t nf = ((uintptr_t *)f)[0];
            if (ra) s_prof[i].st[k++] = ra;
            if (nf <= f) break;
            f = nf;
        }
        s_prof[i].nst = (uint8_t)k;
    }
#else
    (void)ucv;
#endif
}
typedef struct { const char *name; unsigned n; } ProfBin;
static int prof_cmp(const void *a, const void *b)
{ return (int)((const ProfBin *)b)->n - (int)((const ProfBin *)a)->n; }
static const char *prof_intern(const char *str)
{
    static char pool[4096][40]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(pool[i], str)) return pool[i];
    if (n == 4096) return "libmain.so";
    snprintf(pool[n], sizeof pool[n], "%s", str);
    return pool[n++];
}
static const char *prof_sym(void *pc)
{
    Dl_info di;
    if (pc && dladdr(pc, &di)) {
        if (di.dli_sname) return di.dli_sname;
        const char *f = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        if (!strcmp(f, "libmain.so")) {   /* static function: offset, for nm */
            char b[40];
            snprintf(b, sizeof b, "main+0x%lx", (unsigned long)(((uintptr_t)pc - (uintptr_t)di.dli_fbase) & ~0x3Ful));
            return prof_intern(b);
        }
        return f;
    }
    return "?";
}
static void prof_hist(int tid, int which, const char *label)
{
    static ProfBin bins[4096];
    int nb = 0; unsigned total = 0;
    unsigned n = s_prof_n < PROF_RING ? s_prof_n : PROF_RING;
    for (unsigned i = 0; i < n; i++) {
        if (s_prof[i].tid != tid) continue;
        void *a = which == 0 ? s_prof[i].pc : (which == 1 || which == 3) ? s_prof[i].lr : s_prof[i].fp1;
        if (which >= 3) {   /* only samples whose pc is in an unnamed system lib */
            const char *pn = prof_sym(s_prof[i].pc);
            if (strcmp(pn, "libc.so") && strncmp(pn, "main+", 5) && strcmp(pn, "libGLES_mali.so")) continue;
        }
        const char *nm = prof_sym(a);
        total++;
        int k;
        for (k = 0; k < nb; k++) if (bins[k].name == nm || !strcmp(bins[k].name, nm)) break;
        if (k == nb) { if (nb == 4096) continue; bins[nb].name = nm; bins[nb].n = 0; nb++; }
        bins[k].n++;
    }
    if (!total) return;
    qsort(bins, nb, sizeof bins[0], prof_cmp);
    char line[1400]; int o = 0;
    for (int k = 0; k < nb && k < 22 && o < 1300; k++)
        o += snprintf(line + o, sizeof line - o, " %s:%u%%", bins[k].name, bins[k].n * 100 / total);
    fprintf(stderr, "[PROF]  %s(%u)%s\n", label, total, line);
}
/* [PROFSTK]: for one thread, every return address in libmain seen on its
 * sampled stacks, counted once per sample (inclusive). Resolved offline with
 * nm on the unstripped libmain.so. */
static void prof_stacks(int tid, const char *label)
{
    enum { HN = 16384 };
    static uintptr_t hk[HN]; static unsigned hv[HN];
    memset(hk, 0, sizeof hk); memset(hv, 0, sizeof hv);
    Dl_info di; uintptr_t base = 0, end = 0;
    if (dladdr((void *)prof_stacks, &di)) base = (uintptr_t)di.dli_fbase;
    end = base + 0x8000000;
    unsigned total = 0;
    unsigned n = s_prof_n < PROF_RING ? s_prof_n : PROF_RING;
    for (unsigned i = 0; i < n; i++) {
        if (s_prof[i].tid != tid) continue;
        total++;
        uintptr_t seen[PROF_STK]; int ns = 0;
        for (int k = 0; k < s_prof[i].nst; k++) {
            uintptr_t a = s_prof[i].st[k];
            if (a < base || a >= end) { if (k == 0) a = 1; else continue; }  /* 1 = leaf outside libmain */
            else a -= base;
            int dup = 0;
            for (int j = 0; j < ns; j++) if (seen[j] == a) { dup = 1; break; }
            if (dup) continue;
            seen[ns++] = a;
            unsigned h = (unsigned)((a * 2654435761u) >> 2) & (HN - 1);
            while (hk[h] && hk[h] != a) h = (h + 1) & (HN - 1);
            hk[h] = a; hv[h]++;
        }
    }
    if (!total) return;
    /* exclusive-ish: the first frame in libmain (time in libc, the driver
     * or the kernel goes to the libmain function that called into it) */
    {
        static uintptr_t sk[HN]; static unsigned sv[HN];
        memset(sk, 0, sizeof sk); memset(sv, 0, sizeof sv);
        for (unsigned i = 0; i < n; i++) {
            if (s_prof[i].tid != tid) continue;
            uintptr_t a = 2;   /* 2 = no libmain frame */
            for (int k = 0; k < s_prof[i].nst; k++) {
                uintptr_t x = s_prof[i].st[k];
                if (x >= base && x < end) { a = x - base; break; }
            }
            unsigned h = (unsigned)((a * 2654435761u) >> 2) & (HN - 1);
            while (sk[h] && sk[h] != a) h = (h + 1) & (HN - 1);
            sk[h] = a; sv[h]++;
        }
        char sl[3200]; int so = snprintf(sl, sizeof sl, "[PROFSELF] %s total=%u", label, total), sp = 0;
        for (int h = 0; h < HN; h++) {
            if (!sk[h] || sv[h] * 400 < total) continue;
            so += snprintf(sl + so, sizeof sl - so, " %lx:%u", (unsigned long)sk[h], sv[h]);
            if (so > 3000) { fprintf(stderr, "%s\n", sl); so = snprintf(sl, sizeof sl, "[PROFSELF] %s +%d", label, ++sp); }
        }
        fprintf(stderr, "%s\n", sl);
    }
    char line[3200]; int o = 0, part = 0;
    o = snprintf(line, sizeof line, "[PROFSTK] %s total=%u", label, total);
    for (int h = 0; h < HN; h++) {
        if (!hk[h] || hv[h] * 400 < total) continue;   /* >= 0.25% */
        o += snprintf(line + o, sizeof line - o, " %lx:%u", (unsigned long)hk[h], hv[h]);
        if (o > 3000) { fprintf(stderr, "%s\n", line); o = snprintf(line, sizeof line, "[PROFSTK] %s +%d", label, ++part); }
    }
    fprintf(stderr, "%s\n", line);
}

static void *prof_thread(void *arg)
{
    (void)arg;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    enum { MAXT = 128 };
    int tids[MAXT]; unsigned long long last[MAXT]; char names[MAXT][24]; int nt = 0;
    pid_t self = gettid();
    long hz = sysconf(_SC_CLK_TCK);
    for (int round = 0;; round++) {
        /* refresh the thread list and their CPU time */
        int busy[MAXT]; int nbusy = 0;
                DIR *dir = opendir("/proc/self/task");
        struct dirent *de;
        int cur[MAXT]; unsigned long long now_t[MAXT]; char cur_names[MAXT][24]; int nc = 0;
        while (dir && (de = readdir(dir)) && nc < MAXT) {
            int tid = atoi(de->d_name);
            if (tid <= 0) continue;
            char path[64], buf[512];
            snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
            FILE *f = fopen(path, "r"); if (!f) continue;
            size_t r = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[r] = 0;
            char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
            if (!lp || !rp) continue;
            int ln = (int)(rp - lp - 1); if (ln > 23) ln = 23;
            memcpy(cur_names[nc], lp + 1, ln); cur_names[nc][ln] = 0;
            unsigned long ut = 0, st = 0;
            /* fields after ')': state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
            sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
            cur[nc] = tid; now_t[nc] = ut + st; nc++;
        }
        if (dir) closedir(dir);
        char line[1600]; int o = 0;
        for (int i = 0; i < nc; i++) {
            unsigned long long prev = 0; int found = 0;
            for (int j = 0; j < nt; j++) if (tids[j] == cur[i]) { prev = last[j]; found = 1; }
            double p = found ? (double)(now_t[i] - prev) * 100.0 / (hz * 5.0) : 0;
            if (p >= 3.0 && o < 1500) o += snprintf(line + o, sizeof line - o, " %s/%d:%.0f%%", cur_names[i], cur[i], p);
            if (p >= 15.0 && cur[i] != self && nbusy < MAXT) busy[nbusy++] = i;
        }
        nt = nc; memcpy(tids, cur, sizeof(int) * nc); memcpy(last, now_t, sizeof(unsigned long long) * nc);
        memcpy(names, cur_names, sizeof cur_names[0] * nc);
        if (round) fprintf(stderr, "[PROF] cpu%s\n", line);
        {   /* clocks, to tell throttling from regressions */
            char fl[256]; int fo = 0;
            for (int c = 0; c < 8; c += 1) {
                char path[96]; snprintf(path, sizeof path,
                    "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
                FILE *f = fopen(path, "r");
                long khz = 0;
                if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
                fo += snprintf(fl + fo, sizeof fl - fo, " %ld", khz / 1000);
            }
            long gpu = 0;
            FILE *g = fopen("/sys/devices/platform/1c500000.mali/cur_freq", "r");
            if (!g) g = fopen("/sys/class/misc/mali0/device/cur_freq", "r");
            if (g) { if (fscanf(g, "%ld", &gpu) != 1) gpu = 0; fclose(g); }
            long temp = 0;
            FILE *t = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
            if (t) { if (fscanf(t, "%ld", &temp) != 1) temp = 0; fclose(t); }
            if (round) fprintf(stderr, "[PROF] MHz cpu0-7:%s gpu %ld temp %ld\n", fl, gpu, temp);
        }
        /* histograms from the samples of the last period */
        static int pb_tid[MAXT], npb; static char pb_name[MAXT][24];
        for (int b = 0; b < npb; b++) {
            char lab[64];
            snprintf(lab, sizeof lab, "%s/%d pc", pb_name[b], pb_tid[b]);
            prof_hist(pb_tid[b], 0, lab);
            snprintf(lab, sizeof lab, "%s/%d caller", pb_name[b], pb_tid[b]);
            prof_hist(pb_tid[b], 2, lab);
            if (!strncmp(pb_name[b], "nv2a.pfifo", 10) || getenv("CONKER_PROF_STK_ALL")) {
                snprintf(lab, sizeof lab, "%s/%d", pb_name[b], pb_tid[b]);
                prof_stacks(pb_tid[b], lab);
            }
            snprintf(lab, sizeof lab, "%s/%d lib<-lr", pb_name[b], pb_tid[b]);
            prof_hist(pb_tid[b], 3, lab);
            snprintf(lab, sizeof lab, "%s/%d lib<-fp", pb_name[b], pb_tid[b]);
            prof_hist(pb_tid[b], 4, lab);
        }
        npb = nbusy;
        for (int b = 0; b < nbusy; b++) { pb_tid[b] = tids[busy[b]]; memcpy(pb_name[b], names[busy[b]], 24); }
        s_prof_n = 0;
        /* sample the busy threads for the next 5 s at ~250 Hz each */
        for (int t = 0; t < 1250; t++) {
            for (int b = 0; b < nbusy; b++) syscall(SYS_tgkill, getpid(), tids[busy[b]], SIGPROF);
            usleep(4000);
        }
        if (!nbusy) sleep(5);
    }
    return NULL;
}

/* Everything that runs guest code. Called on the guest thread. */
static int guest_main(void)
{
    if (!xbox_MemoryLayoutInit(s_xbe_data, s_xbe_size)) {
        fprintf(stderr, "[BOOT] Xbox memory layout failed: the guest address "
                        "range is not available in this process\n");
        return 2;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    fprintf(stderr, "[BOOT] memory mapped, offset 0x%llx\n",
            (unsigned long long)g_xbox_mem_offset);
    {   /* A 32-bit guest access at 0xFFFFFFFD..0xFFFFFFFF reaches past the
         * 4 GB window on the host (the XAPI USB stack reads a dword through
         * a -1 pointer); on the Xbox it wraps. Back the page after the window
         * with zeros instead of crashing. */
        void *past = (void *)((uintptr_t)g_xbox_mem_offset + 0x100000000ull);
        void *m = mmap(past, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        int ok = m == past;
        if (!ok)   /* already reserved (PROT_NONE) by the memory layout: open it */
            ok = mprotect(past, 4096, PROT_READ | PROT_WRITE) == 0;
        fprintf(stderr, "[BOOT] guard page after 4 GB window: %s\n",
                ok ? "mapped" : "unavailable");
    }

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED");
    }

    {   /* USB host controllers (pads). Opt-in through RECOMP_USB. */
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
        fprintf(stderr, "[BOOT] USB model %s\n", getenv("RECOMP_USB") ? "on" : "off");
    }
    {   /* Ethernet controller registers: logged (System Link groundwork) */
        extern void xbox_NicProbeInit(void);
        xbox_NicProbeInit();
    }

    if (getenv("CONKER_AC97_SELFTEST")) {
        volatile uint8_t *r = (volatile uint8_t *)((uintptr_t)g_xbox_mem_offset + 0xFEC0017Bu);
        *r = 0x1D;
        uint8_t a = *r;
        *r = 2;
        uint8_t b = *r;
        fprintf(stderr, "[SELFTEST] AC97 ch70: wrote 1D read %02X; wrote 02 read %02X\n", a, b);
    }
    xbox_kernel_init();
#if defined(XBOXRECOMP_NV2A_XEMU)
    if (s_gpu_xemu) {
        char cache[1100];
        snprintf(cache, sizeof cache, "%s/gpucache", s_save_dir);
        mkdir(cache, 0755);
        if (!xbox_Nv2aXemuInit(cache, s_gpu_null))
            fprintf(stderr, "[BOOT] xemu GPU failed to start\n");
    }
#endif
    xbox_path_init(s_game_dir, s_save_dir);
    {   extern void xbox_path_set_overlay(const char *dir);
        const char *ov = getenv("CONKER_OVERLAY");     /* a language patch (Settings.java) */
        if (ov && ov[0]) xbox_path_set_overlay(ov);
    }
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    xbox_WatchdogStart();
    start_stackdump();
    if (getenv("CONKER_PROF")) {
        pthread_t t; pthread_create(&t, NULL, prof_thread, NULL); pthread_detach(t);
    }
    if (!getenv("CONKER_NO_PIN")) {
        pthread_t t; pthread_create(&t, NULL, pin_thread, NULL); pthread_detach(t);
    }
    {
        pthread_t t; pthread_create(&t, NULL, fscan_thread, NULL); pthread_detach(t);
        pthread_create(&t, NULL, memsnap_thread, NULL); pthread_detach(t);
    }
    if (getenv("CONKER_MATWATCH2")) {
        pthread_t t; pthread_create(&t, NULL, matwatch2_thread, NULL); pthread_detach(t);
    }
    if (getenv("CONKER_MATWATCH")) {
        pthread_t t; pthread_create(&t, NULL, matwatch_thread, NULL); pthread_detach(t);
    }

    { extern void nv2a_hint_register_thread(void); nv2a_hint_register_thread(); }
    if (getenv("CONKER_DRS")) {
        static pthread_t drs;
        pthread_create(&drs, NULL, drs_thread, NULL);
        pthread_detach(drs);
    }
    {   /* the GPU's clock limits, for the record */
        static const char *fs[] = { "max_freq", "min_freq", "scaling_max_freq", "scaling_min_freq",
            "dvfs_governor", "power_policy", "hint_max_freq", "hint_min_freq", "utilization",
            "available_frequencies", "tmu_max_freq" };
        char line[600]; int o = 0;
        for (unsigned i = 0; i < sizeof fs / sizeof fs[0]; i++) {
            char p[128], b[160] = "";
            snprintf(p, sizeof p, "/sys/devices/platform/1c500000.mali/%s", fs[i]);
            FILE *f = fopen(p, "r");
            if (f) { if (fgets(b, sizeof b, f)) b[strcspn(b, "\n")] = 0; fclose(f); }
            else snprintf(b, sizeof b, "-");
            o += snprintf(line + o, sizeof line - o, " %s=%s", fs[i], b);
            if (o > 560) break;
        }
        fprintf(stderr, "[BOOT] mali:%s\n", line);
    }
    fprintf(stderr, "[BOOT] entering 0x%08X, esp 0x%08X\n", CONKER_ENTRY_POINT, g_esp);
    { extern void nv2a_sampler_register(int); nv2a_sampler_register(1); }
    xbe_entry_point();
    fprintf(stderr, "[BOOT] title returned\n");

    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    return 0;
}

static void set_default_env(const char *name, const char *value)
{
    if (!getenv(name)) setenv(name, value, 0);
}

#ifdef __ANDROID__
/* Route stdout/stderr to logcat so the runtime's diagnostics are visible. */
static void *log_pump(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    FILE *in = fdopen(fd, "r");
    while (in && fgets(buf, sizeof buf, in))
        __android_log_write(ANDROID_LOG_INFO, "conker", buf);
    return NULL;
}
static void redirect_logs(void)
{
    int p[2];
    if (pipe(p) != 0) return;
    dup2(p[1], 1);
    dup2(p[1], 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t;
    pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]);
    pthread_detach(t);
}
#endif

#if defined(__ANDROID__) && defined(XBOXRECOMP_NV2A_XEMU)
static volatile pid_t s_guest_tid;
static void *guest_thread(void *arg) { (void)arg; s_guest_tid = gettid(); guest_main(); return NULL; }

static void *window_handle(SDL_Window *w)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return NULL;
    return info.info.android.window;
}

/* Android with the xemu GPU: SDL owns the window (Vulkan, no EGL), the
 * renderer presents into it, this thread pumps events for the pads, and the
 * title runs on its own thread with a large stack. */
#if defined(__ANDROID__) && defined(XBOXRECOMP_NV2A_XEMU)
/* ── Touch controls ──────────────────────────────────────────────────────
 * Invisible zones on the landscape screen, fed into the keyboard-as-pad
 * (input/keyboard.c): a floating left stick (WASD), a camera area (IJKL),
 * A/B/X/Y on the right, Start/Back in the top corners, triggers on the
 * edges. Coordinates are SDL's normalised 0..1. */
#define TOUCH_MAX 10
typedef struct { SDL_FingerID id; int kind; float ox, oy; int keys[4]; } TouchSlot;
static TouchSlot s_touch[TOUCH_MAX];
enum { T_NONE, T_LSTICK, T_RSTICK, T_BUTTON };

static void touch_keys(TouchSlot *t, int k0, int k1, int k2, int k3)
{
    int nk[4] = { k0, k1, k2, k3 };
    for (int i = 0; i < 4; i++)
        if (t->keys[i] && t->keys[i] != nk[0] && t->keys[i] != nk[1] &&
            t->keys[i] != nk[2] && t->keys[i] != nk[3])
            xbox_InputKeySet(t->keys[i], 0);
    for (int i = 0; i < 4; i++) {
        if (nk[i]) xbox_InputKeySet(nk[i], 1);
        t->keys[i] = nk[i];
    }
}

static void touch_stick(TouchSlot *t, float x, float y, int up, int left, int down, int right)
{
    float dx = (x - t->ox) * 16.0f / 9.0f, dy = y - t->oy;
    const float dead = 0.03f;
    touch_keys(t, dy < -dead ? up : 0, dx < -dead ? left : 0,
                  dy > dead ? down : 0, dx > dead ? right : 0);
}

static int touch_button_at(float x, float y)
{
    if (y < 0.15f && x > 0.85f) return XBOX_VK_RETURN;     /* Start */
    if (y < 0.15f && x < 0.15f) return XBOX_VK_BACK;       /* Back */
    if (x > 0.90f && y >= 0.15f && y < 0.45f) return '3';  /* right trigger */
    if (x < 0.10f && y >= 0.15f && y < 0.45f) return '1';  /* left trigger */
    if (x >= 0.72f && y >= 0.45f) {                        /* face buttons */
        float cx = 0.86f, cy = 0.72f;
        float dx = (x - cx) * 16.0f / 9.0f, dy = y - cy;
        if (fabsf(dx) > fabsf(dy)) return dx > 0 ? 'X' : 'C';   /* B : X */
        return dy > 0 ? 'Z' : 'V';                               /* A : Y */
    }
    return 0;
}

static void touch_event(const SDL_Event *e)
{
    const SDL_TouchFingerEvent *f = &e->tfinger;
    TouchSlot *t = NULL;
    for (int i = 0; i < TOUCH_MAX; i++)
        if (s_touch[i].kind != T_NONE && s_touch[i].id == f->fingerId) { t = &s_touch[i]; break; }

    if (e->type == SDL_FINGERDOWN) {
        if (t) return;
        for (int i = 0; i < TOUCH_MAX; i++) if (s_touch[i].kind == T_NONE) { t = &s_touch[i]; break; }
        if (!t) return;
        memset(t, 0, sizeof *t);
        t->id = f->fingerId; t->ox = f->x; t->oy = f->y;
        int b = touch_button_at(f->x, f->y);
        if (b) { t->kind = T_BUTTON; touch_keys(t, b, 0, 0, 0); }
        else if (f->x < 0.45f) t->kind = T_LSTICK;
        else t->kind = T_RSTICK;
        return;
    }
    if (!t) return;
    if (e->type == SDL_FINGERMOTION) {
        if (t->kind == T_LSTICK) touch_stick(t, f->x, f->y, 'W', 'A', 'S', 'D');
        else if (t->kind == T_RSTICK) touch_stick(t, f->x, f->y, 'I', 'J', 'K', 'L');
        return;
    }
    /* SDL_FINGERUP */
    touch_keys(t, 0, 0, 0, 0);
    t->kind = T_NONE;
}
#endif

/* On-screen controller (Java TouchGamepadView): SDL_GameControllerButton /
 * Axis numbering in, Xbox pad out. LB/RB are White/Black on this pad. */
#include <jni.h>
JNIEXPORT jboolean JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeAttach(JNIEnv *env, jclass cls)
{ (void)env; (void)cls; return JNI_TRUE; }

JNIEXPORT void JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeButton(JNIEnv *env, jclass cls, jint b, jboolean down)
{
    (void)env; (void)cls;
    static const signed char analog[15] = { 0, 1, 2, 3, -1, -1, -1, -1, -1, 5, 4, -1, -1, -1, -1 };
    static const unsigned short wb[15]  = { 0, 0, 0, 0, 0x20, 0, 0x10, 0x40, 0x80, 0, 0, 0x1, 0x2, 0x4, 0x8 };
    if (b < 0 || b >= 15) return;
    xbox_InputTouchButton(analog[b], wb[b], down);
}

JNIEXPORT void JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeAxis(JNIEnv *env, jclass cls, jint a, jfloat v)
{
    (void)env; (void)cls;
    if (a == 4 || a == 5) { xbox_InputTouchButton(a == 4 ? 6 : 7, 0, v > 0.5f); return; }
    if (a == 1 || a == 3) v = -v;      /* screen down = stick down */
    xbox_InputTouchAxis(a, v);
}

/* Long hold on the overlay's toggle: guest RAM (64 MB) to
 * <ext>/dump/mark_<n>.bin, numbered in order, for diffing game states. */
/* REC button: mark the log and log the next frames in full (draws, clears,
 * targets, textures, visibility reports); the Java side then saves the
 * process log to <dump>/capture_<n>.txt. */
volatile int nv2a_capture_log_reports;
/* settings menu: 1 when a custom Vulkan driver (Turnip) is in use */
JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeDriverActive(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    extern int nv2a_custom_driver_active;
    return nv2a_custom_driver_active;
}

static void *apu_wav_reset(void *a)
{
    (void)a;
    extern volatile int g_apu_wav_request;
    sleep(2);
    g_apu_wav_request = 0;
    return NULL;
}

JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeCapture(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    static int n;
    extern volatile int nv2a_capture_frames;
    n++;
    fprintf(stderr, "[CAPTURE] %d: user pressed REC\n", n);
    {   extern volatile unsigned nv2a_flip_seq;
        static unsigned last_flip; static int still;
        /* no new frame since the last press: a hang, dump every thread */
        still = (n > 1 && nv2a_flip_seq == last_flip);
        last_flip = nv2a_flip_seq;
        fprintf(stderr, "[CAPTURE] frames so far %u%s\n", nv2a_flip_seq, still ? " (no new frames: hang)" : "");
        /* every thread's stack only for a hang: signalling each thread of
         * a running game is what took it down on REC, twice */
        pthread_t t;   /* off the UI thread: ~30 ms per thread */
        if ((still || getenv("CONKER_REC_STACKS")) &&
            pthread_create(&t, NULL, (void *(*)(void *))dump_all_threads, NULL) == 0) pthread_detach(t);
    }
    nv2a_capture_log_reports = 1;
    {   extern volatile int nv2a_dump_all;
        if (s_snap_dir[0]) { mkdir(s_snap_dir, 0755); setenv("NV2A_DUMP_DIR", s_snap_dir, 1); }
        nv2a_dump_all = 1; }
    nv2a_capture_frames = 4;
    {   /* the sound of the last 12 s, and every voice, for a sound problem */
        extern volatile int g_apu_wav_request;
        g_apu_wav_request = 1;
        pthread_t t2;
        if (pthread_create(&t2, NULL, apu_wav_reset, NULL) == 0) pthread_detach(t2);
    }
    return n;
}

JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeMark(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    static int n;
    if (!s_snap_dir[0]) return -1;
    char path[1100];
    snprintf(path, sizeof path, "%s/mark_%02d.bin", s_snap_dir, ++n);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite((const void *)(uintptr_t)g_xbox_mem_offset, 1, 64u << 20, f);
    fclose(f);
    fprintf(stderr, "[MARK] %d -> %s\n", n, path);
    return n;
}

/* TEMP diagnostics: which lifted functions the game thread runs while the
 * screen is labelled CUT or GAME. ~200 Hz, each sample's whole frame-pointer
 * chain; every 3 s the inclusive count per function goes to
 * <dump>/c_<pid>_<seq>_<state>.txt. Diffed offline for a function that only
 * gameplay runs (the player's control update), to detect gameplay by. */
#define LP_RING 8192
#define LP_DEPTH 40
static uintptr_t s_lp[LP_RING][LP_DEPTH];
static uint8_t s_lp_n[LP_RING];
static volatile unsigned s_lp_w;
static void lp_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
#if defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    unsigned i = __atomic_fetch_add(&s_lp_w, 1, __ATOMIC_RELAXED) % LP_RING;
    int k = 0;
    s_lp[i][k++] = uc->uc_mcontext.pc;
    s_lp[i][k++] = uc->uc_mcontext.regs[30];
    uintptr_t f = uc->uc_mcontext.regs[29], sp = uc->uc_mcontext.sp, lim = sp + 0x400000;
    while (k < LP_DEPTH && f && !(f & 7) && f >= sp && f < lim) {
        uintptr_t ra = ((uintptr_t *)f)[1], nf = ((uintptr_t *)f)[0];
        if (ra) s_lp[i][k++] = ra;
        if (nf <= f) break;
        f = nf;
    }
    s_lp_n[i] = (uint8_t)k;
#else
    (void)ucv;
#endif
}
static volatile int s_label;
static void *labelprof_thread(void *arg)
{
    (void)arg;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = lp_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGRTMIN + 3, &sa, NULL);
    enum { HN = 1 << 15 };
    static uintptr_t key[HN]; static unsigned cnt[HN];
    static const char *names[HN];
    unsigned r = 0, seq = 0, samples = 0;
    struct timespec last; clock_gettime(CLOCK_MONOTONIC, &last);
    for (;;) {
        usleep(5000);
        int st = s_label;
        if (st && s_guest_tid) {
            syscall(SYS_tgkill, getpid(), s_guest_tid, SIGRTMIN + 3);
        }
        usleep(1000);
        /* fold finished samples: each function once per sample */
        unsigned w = s_lp_w;
        while (r < w) {
            unsigned i = r % LP_RING;
            uintptr_t seen[LP_DEPTH]; int ns = 0;
            for (int k = 0; k < s_lp_n[i]; k++) {
                Dl_info di;
                uintptr_t a = s_lp[i][k];
                if (!a || !dladdr((void *)a, &di) || !di.dli_sname || strncmp(di.dli_sname, "sub_", 4))
                    continue;
                uintptr_t fs = (uintptr_t)di.dli_saddr;
                int dup = 0;
                for (int j = 0; j < ns; j++) if (seen[j] == fs) dup = 1;
                if (dup) continue;
                seen[ns++] = fs;
                unsigned h = (unsigned)((fs >> 2) * 2654435761u) & (HN - 1);
                while (key[h] && key[h] != fs) h = (h + 1) & (HN - 1);
                key[h] = fs; names[h] = di.dli_sname; cnt[h]++;
            }
            samples++;
            r++;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last.tv_sec >= 3) {
            last = now;
            if (st && samples && s_snap_dir[0]) {
                char path[1100];
                snprintf(path, sizeof path, "%s/c_%d_%04u_%s.txt", s_snap_dir, (int)getpid(),
                         ++seq, st == 1 ? "cut" : "game");
                FILE *f = fopen(path, "w");
                if (f) {
                    fprintf(f, "samples %u\n", samples);
                    for (int h = 0; h < HN; h++) if (key[h] && cnt[h]) fprintf(f, "%s %u\n", names[h], cnt[h]);
                    fclose(f);
                }
            }
            memset(cnt, 0, sizeof cnt);
            samples = 0;
        }
    }
    return NULL;
}

/* TEMP diagnostics: the CUT / GAME buttons label what the screen shows.
 * Each press saves guest RAM (64 MB) as lbl_<n>_<state>.bin; from then on,
 * every 3 s, the title's .data/.bss as d_<sec>_<state>.bin, until the next
 * press. Diffed offline for a cutscene / gameplay flag. */
static void *label_thread(void *arg)
{
    (void)arg;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        sleep(3);
        int st = s_label;
        if (!st || !s_snap_dir[0]) continue;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        extern uint32_t g_ic_key[]; extern volatile uint32_t g_ic_cnt[];
        static unsigned iseq;
        char path[1100];
        snprintf(path, sizeof path, "%s/i_%d_%04u_%s.txt", s_snap_dir, (int)getpid(),
                 ++iseq, st == 1 ? "cut" : "game");
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "t %ld\n", (long)(t.tv_sec - t0.tv_sec));
            for (int h = 0; h < (1 << 16); h++) {
                uint32_t c = __atomic_exchange_n(&g_ic_cnt[h], 0, __ATOMIC_RELAXED);
                if (g_ic_key[h] && c) fprintf(f, "%08X %u\n", g_ic_key[h], c);
            }
            fclose(f);
        }
    }
    return NULL;
}
int conker_label_set(int state);
JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeLabel(JNIEnv *env, jclass cls, jint state)
{
    (void)env; (void)cls;
    return conker_label_set(state);
}
int conker_label_set(int state)
{
    static int n, started;
    if (!s_snap_dir[0]) return -1;
    if (!started) {
        started = 1;
        /* a fresh session: old snapshots go (they ran to gigabytes) */
        DIR *dd = opendir(s_snap_dir);
        struct dirent *de;
        while (dd && (de = readdir(dd))) {
            if (!strncmp(de->d_name, "lbl_", 4) || !strncmp(de->d_name, "d_", 2) ||
                !strncmp(de->d_name, "c_", 2) || !strncmp(de->d_name, "i_", 2) || !strncmp(de->d_name, "hooks", 5) || !strncmp(de->d_name, "mark_", 5)) {
                char fp[1400]; snprintf(fp, sizeof fp, "%s/%s", s_snap_dir, de->d_name);
                unlink(fp);
            }
        }
        if (dd) closedir(dd);
        pthread_t t; pthread_create(&t, NULL, label_thread, NULL); pthread_detach(t);
    }
    s_label = state;
    { extern volatile int g_ic_on; g_ic_on = 1; }
    char path[1100];
    snprintf(path, sizeof path, "%s/lbl_%d_%02d_%s.bin", s_snap_dir, (int)getpid(), ++n,
             state == 1 ? "cut" : "game");
    fprintf(stderr, "[LABEL] %d %s\n", n, state == 1 ? "cut" : "game");
    return n;
}

/* What is on screen, for the touch overlay: 0 gameplay, 1 cutscene or
 * loading, 3 menu. From the time of the last call into the title's player
 * update / front end (recomp_manual.c). A state holds 400 ms before the
 * next replaces it, so a dropped frame does not flash the controls. */
int conker_paused(int64_t now);
int conker_level_menu(int64_t now);
/* frames shown since a hook last ran */
static unsigned frames_since(volatile unsigned *stamp)
{
    extern volatile unsigned nv2a_flip_seq;
    return nv2a_flip_seq - *stamp;
}
extern volatile unsigned g_conker_play_flip, g_conker_world_flip, g_conker_ui_flip;

JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeGameState(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    extern volatile int64_t g_conker_last_play_ms, g_conker_last_ui_ms;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    /* not playing: the player update has been away for 0.3 s AND for six
     * frames -- a long hitch (haystacks landing) shows no frames, and used
     * to flip the overlay to cutscene mode and back */
    int raw = (now - g_conker_last_play_ms >= 300 && frames_since(&g_conker_play_flip) >= 6) ? 3 : 0;
    static int cur = 1, cand = -1;
    extern volatile int64_t g_conker_last_world_ms;
    /* swimming (and other player states the two update hooks do not cover):
     * the player update is quiet but the world and the HUD run every frame,
     * which a cutscene or the pause never do. In a level, not paused: play. */
    {
        extern volatile int g_conker_in_level;
        if (raw == 3 && g_conker_in_level && now - g_conker_last_world_ms < 300 &&
            now - g_conker_last_ui_ms < 300 && frames_since(&g_conker_world_flip) < 3 &&
            frames_since(&g_conker_ui_flip) < 3 && !conker_paused(now))
            raw = 0;
    }
    if (raw == 3 && cur != 3 && now - g_conker_last_world_ms > 250 && now - g_conker_last_ui_ms > 250 &&
        frames_since(&g_conker_world_flip) >= 6 && frames_since(&g_conker_ui_flip) >= 6) {
        /* every per-frame hook quiet: a cutscene or the pause -- the
         * controls go at once, not after the 0.4 s settle */
        cur = 3; cand = -1;
        fprintf(stderr, "[STATE] menu (all quiet)\n");
        return cur;
    }
    if (raw == 3 && cur != 3 && conker_paused(now)) {   /* the pause screen: at once */
        cur = 3; cand = -1;
        fprintf(stderr, "[STATE] menu (pause)\n");
        return cur;
    }
    static int64_t cand_since, last_log;
    if (now - last_log >= 2000) {   /* TEMP: hook calls per 2 s */
        extern volatile unsigned g_conker_hook_calls[4];
        last_log = now;
        fprintf(stderr, "[HOOKS] 25460=%u 25090=%u 2C4AF0=%u 2D3370=%u state=%d\n",
                g_conker_hook_calls[0], g_conker_hook_calls[1], g_conker_hook_calls[2],
                g_conker_hook_calls[3], cur);
        if (s_snap_dir[0]) {
            char hp[1100]; snprintf(hp, sizeof hp, "%s/hooks.txt", s_snap_dir);
            FILE *hf = fopen(hp, "a");
            if (hf) {
                fprintf(hf, "%lld %u %u %u %u %d\n", (long long)(now / 1000),
                        g_conker_hook_calls[0], g_conker_hook_calls[1],
                        g_conker_hook_calls[2], g_conker_hook_calls[3], cur);
                fclose(hf);
            }
        }
        for (int i = 0; i < 4; i++) g_conker_hook_calls[i] = 0;
    }
    if (raw == cur) { cand = -1; return cur; }
    if (raw != cand) { cand = raw; cand_since = now; }
    if (now - cand_since >= 400) {
        cur = raw; cand = -1;
        fprintf(stderr, "[STATE] %s\n", cur == 0 ? "gameplay" : cur == 3 ? "menu" : "cutscene");
    }
    return cur;
}

/* Touch hints: 0 not a menu, 1 a front-end menu (the UI pass draws, no
 * gameplay), 2 the Chapters list. */
/* The in-game pause screen: in a level and frames still coming, but none
 * of the game's per-frame hooks running -- no player update, no world
 * (2C4AF0), no HUD/2D (2D3370): the pause menu draws through none of them
 * (captures of it: all four zero). Menus that draw the 2D pass (the System
 * Link lobby) and load screens (a level package just opened, heavy reads)
 * are not it. Called from the camera hook and the menu-kind poll. */
int conker_paused(int64_t now)
{
    extern volatile int g_conker_in_level;
    extern volatile int64_t g_conker_last_play_ms, g_conker_last_ui_ms;
    extern volatile int64_t g_conker_last_world_ms, g_xbox_load_mark_ms;
    extern volatile unsigned nv2a_flip_seq;
    extern uint64_t xbox_io_recent_bytes(void) __attribute__((weak));
    static volatile unsigned flip_seen; static volatile int64_t flip_ms;
    if (nv2a_flip_seq != flip_seen) { flip_seen = nv2a_flip_seq; flip_ms = now; }
    int loading = (g_xbox_load_mark_ms && now - g_xbox_load_mark_ms < 4000) ||
                  (xbox_io_recent_bytes && xbox_io_recent_bytes() > (1u << 20));
    /* Entered only through START: the game's movie cutscenes stop all the
     * same hooks (and got the pause hint and taps). Pressed, the game stops
     * its per-frame work on the very next frame, so a few frames without it
     * are enough; then it holds until any of that work comes back. */
    extern volatile int64_t g_xbox_start_ms;
    static volatile int latched;
    int quiet_fast = now - g_conker_last_play_ms > 90 && now - g_conker_last_world_ms > 90 &&
                     now - g_conker_last_ui_ms > 90 &&
                     frames_since(&g_conker_play_flip) >= 2 && frames_since(&g_conker_world_flip) >= 2 &&
                     frames_since(&g_conker_ui_flip) >= 2;
    if (!g_conker_in_level || now - flip_ms >= 300) { latched = 0; return 0; }
    static int64_t latched_at;
    if (latched) {
        /* left: the game runs again, or START / B (Resume) pressed in it --
         * a pause opened in a cutscene goes back to a cutscene, which is as
         * quiet as the pause (Options and back re-reads the menu text and
         * latches again) */
        extern volatile int64_t g_xbox_b_ms;
        if (!quiet_fast ||
            (g_xbox_start_ms > latched_at + 250) || (g_xbox_b_ms > latched_at + 250))
            latched = 0;
        return latched;
    }
    /* ...and pressed in gameplay (the player was updating right then): a
     * START that skips a cutscene is not a pause */
    if (!loading && g_xbox_start_ms && now - g_xbox_start_ms < 1500 && quiet_fast &&
        g_conker_last_play_ms > g_xbox_start_ms - 400) { latched = 1; latched_at = now; }
    /* the pause menu's text was just read: paused, from gameplay or from a
     * cutscene (START there pauses too) */
    extern volatile int64_t g_xbox_pause_file_ms;
    if (g_xbox_pause_file_ms && now - g_xbox_pause_file_ms < 1500 && quiet_fast &&
        g_xbox_pause_file_ms > latched_at) { latched = 1; latched_at = now; }
    return latched;
}

/* A menu inside a level (the System Link lobby, its profile and game
 * lists): no gameplay, the 2D pass drawing. Shows the swipe hints like the
 * front-end menus, not the controller. */
int conker_level_menu(int64_t now)
{
    extern volatile int g_conker_in_level;
    extern volatile int64_t g_conker_last_play_ms, g_conker_last_ui_ms, g_conker_last_world_ms;
    extern volatile int64_t g_xbox_load_mark_ms;
    if (!g_conker_in_level) return 0;
    if (g_xbox_load_mark_ms && now - g_xbox_load_mark_ms < 4000) return 0;
    /* a cutscene with speech bubbles also draws the 2D pass with the player
     * idle, but through a widened 3D camera; the lobby menus have none */
    extern volatile int64_t nv2a_wide_cam_us;
    if (nv2a_wide_cam_us > 0 && now - nv2a_wide_cam_us / 1000 < 500) return 0;
    return now - g_conker_last_play_ms > 700 && now - g_conker_last_ui_ms < 300 &&
           now - g_conker_last_world_ms > 500;
}

/* frames that reached the screen so far (0: still booting) */
JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativePresented(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    extern volatile unsigned nv2a_presented;
    return (jint)nv2a_presented;
}

JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeMenuKind(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    extern volatile int64_t g_conker_last_play_ms, g_conker_last_ui_ms;
    extern volatile int64_t nv2a_chapters_seen_ms;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    extern volatile int64_t nv2a_wide_cam_us;
    /* the bar menus are 3D scenes: logos and movies (UI pass, no camera) are not */
    bool cam = nv2a_wide_cam_us >= 0 && now - nv2a_wide_cam_us / 1000 < 500;
    int k = 0;
    extern volatile int g_conker_in_level;
    /* the in-game pause screen: in a level and frames still coming, but
     * none of the game's per-frame hooks running -- no player update, no
     * world (2C4AF0), no HUD/2D (2D3370): the pause menu draws through
     * none of them (captures of it: all four zero). Menus that draw the 2D
     * pass (the System Link lobby) and load screens (a level package just
     * opened, heavy reads) are not it. */
    extern int conker_paused(int64_t now);
    if (conker_paused(now)) k = 3;
    else if (conker_level_menu(now)) k = 1;
    else if (!g_conker_in_level && cam && now - g_conker_last_play_ms > 600 && now - g_conker_last_ui_ms < 500) {
        /* The bar is also where the story begins: its cutscene draws the 2D
         * pass (speech bubbles) through a 3D camera too. The menus keep the
         * bar's world running (2C4AF0); the cutscene stops it and talks. */
        extern volatile int64_t g_conker_last_world_ms, nv2a_bubble_ms;
        bool talk = now - g_conker_last_world_ms > 500 && nv2a_bubble_ms && now - nv2a_bubble_ms < 6000;
        if (!talk) k = now - nv2a_chapters_seen_ms < 400 ? 2 : 1;
    }
    /* A loading screen: the title draws it once and stops flipping until
     * the level is in (10-30 s). No new frame for 0.8 s outside the pause
     * menu: kind 4, the overlay shows nothing at all. */
    {
        extern volatile unsigned nv2a_flip_seq;
        static unsigned last_flip; static int64_t last_flip_ms;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        if (nv2a_flip_seq != last_flip) { last_flip = nv2a_flip_seq; last_flip_ms = now; }
        else if (k != 3 && last_flip && now - last_flip_ms > 800) {
            extern volatile int64_t g_xmv_last_frame_ms;
            k = now - g_xmv_last_frame_ms > 600 ? 4 : 5;   /* 5: a movie playing (no loading bar) */
        }
        /* a load just ended: have every cached texture checked against
         * guest memory once (see texture.c, nv2a_tex_rehash_epoch) */
        static int was_loading;
        extern volatile uint32_t nv2a_tex_rehash_epoch;
        if (was_loading && k != 4) __atomic_add_fetch(&nv2a_tex_rehash_epoch, 1, __ATOMIC_RELEASE);
        was_loading = k == 4;
    }
    /* after a movie (the boot intro, the attract loop) the front end plays
     * an in-engine cutscene (Conker walking into the bar) that looks like a
     * menu to the tests above: no swipe hints until the main menu itself has
     * been drawn since the movie ended */
    {
        extern volatile int64_t g_conker_movie_end_ms, nv2a_frontmenu_seen_ms;
        if (!g_conker_in_level && (k == 1 || k == 2) &&
            g_conker_movie_end_ms && nv2a_frontmenu_seen_ms < g_conker_movie_end_ms)
            k = 0;
    }
    static int last = -1;
    if (k != last) { last = k; fprintf(stderr, "[MENU] kind %d\n", k); }
    return k;
}

/* the aiming view: the crosshair sprite drawn within the last 300 ms
 * (nv2a draw.c) -- the touch overlay then gives the left thumb the camera */
JNIEXPORT jint JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeAimMode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    extern volatile int64_t nv2a_aim_seen_ms;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    int on = nv2a_aim_seen_ms && now - nv2a_aim_seen_ms < 300;
    /* a System Link match: its crosshair is not the slingshot's, and both
     * sticks stay where they are (the user's call) */
    extern int xbox_NicSessionActive(void);
    if (on && xbox_NicSessionActive()) on = 0;
    static int last = -1;
    if (on != last) { last = on; fprintf(stderr, "[AIM] %s\n", on ? "on" : "off"); }
    return on;
}

JNIEXPORT jfloat JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeFps(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    extern float nv2a_host_fps(void) __attribute__((weak));
    return nv2a_host_fps ? nv2a_host_fps() : -1.0f;
}

static int android_gpu_main(void)
{
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[BOOT] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Conker", 0, 0, 0, 0,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_VULKAN);
    if (!win) {
        fprintf(stderr, "[BOOT] SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    nv2a_host_set_window(window_handle(win));
    /* Screens wider than 16:9 (20:9 on a Pixel 6): widen the 3D view to the
     * screen's shape (nv2a_hor_scale, see vsh-prog.c) and fill the screen.
     * CONKER_169=1: plain 16:9 with side bars. CONKER_43=1: 4:3. */
    if (!getenv("CONKER_43") && !getenv("CONKER_169")) {
        extern float nv2a_hor_scale;
        extern void nv2a_vk_present_set_aspect(float aspect);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        if (wh > ww) { int t = ww; ww = wh; wh = t; }
        float sa = wh > 0 ? (float)ww / (float)wh : 0.0f;
        if (sa > 16.0f / 9.0f + 0.01f) {
            nv2a_hor_scale = (16.0f / 9.0f) / sa;
            nv2a_vk_present_set_aspect(sa);
            /* the game's 4:3 cameras get the screen's aspect (frustum hook,
             * recomp_manual.c): projection and culling widen together.
             * CONKER_SHADER_WIDE=1: the old vertex-shader squeeze. */
            if (!getenv("CONKER_SHADER_WIDE")) {
                extern float conker_cam_aspect;
                conker_cam_aspect = sa;
                nv2a_hor_scale = 1.0f;
            }

            fprintf(stderr, "[BOOT] screen %dx%d (%.3f): 3D widened, x scale %.4f\n",
                    ww, wh, sa, nv2a_hor_scale);
        }
    }

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t;
    pthread_create(&t, &a, guest_thread, NULL);
    pthread_attr_destroy(&a);

    for (;;) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 50)) {
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "[BOOT] quit\n");
                _exit(0);
            case SDL_APP_WILLENTERBACKGROUND:
                s_in_background = 1;
                nv2a_host_set_window(NULL);
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                s_in_background = 0;
                nv2a_host_set_window(window_handle(win));
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                    nv2a_host_set_window(window_handle(win));
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
            case SDL_FINGERMOTION:
                /* The Java overlay owns touches; the old invisible zones
                 * stay available with CONKER_NATIVE_TOUCH=1. */
                if (getenv("CONKER_NATIVE_TOUCH")) touch_event(&e);
                break;
            default:
                /* Keys (adb input keyevent, a Bluetooth keyboard) drive pad 0;
                 * real pads are read by the XInput layer itself. */
                xbox_InputSdlEvent(&e);
                break;
            }
        }
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef __ANDROID__
    redirect_logs();
    /* /sdcard/Android/data/<package>/files: readable by the user over USB
     * and by the app without any storage permission. */
    const char *ext = SDL_AndroidGetExternalStoragePath();
    snprintf(s_game_dir, sizeof s_game_dir, "%s/game", ext ? ext : ".");
    {   char dd[1100]; snprintf(dd, sizeof dd, "%s/dump", ext ? ext : ".");
        mkdir(dd, 0777); setenv("NV2A_DUMP_DIR", dd, 1); }
    {   /* Debug switches as empty files next to game/ (no env on Android):
         * files/nv2a_<name> sets NV2A_<NAME>=1. */
        DIR *dir = opendir(ext ? ext : ".");
        struct dirent *de;
        while (dir && (de = readdir(dir))) {
            if (strncmp(de->d_name, "nv2a_", 5)) continue;
            char env[64]; size_t k;
            for (k = 0; de->d_name[k] && k < sizeof env - 1; k++)
                env[k] = (char)toupper((unsigned char)de->d_name[k]);
            env[k] = 0;
            /* files/nv2a_env_<VAR>: any variable, e.g. nv2a_env_recomp_apu_trace */
            const char *name = strncmp(env, "NV2A_ENV_", 9) ? env : env + 9;
            setenv(name, "1", 1);
            fprintf(stderr, "[BOOT] %s=1\n", name);
        }
        if (dir) closedir(dir);
        if (getenv("NV2A_NO_GS")) setenv("NV2A_VK_NO_GS", "1", 1);
        /* render resolution: flags scale2 / scale3 (x2: 1280x960) */
        if (getenv("NV2A_SCALE3")) setenv("RECOMP_GPU_SCALE", "3", 1);
        else if (getenv("NV2A_SCALE2")) setenv("RECOMP_GPU_SCALE", "2", 1);
        else if (getenv("NV2A_SCALE1")) setenv("RECOMP_GPU_SCALE", "1", 1);
        else {
            /* default: x2 (1280x960) on phones of the Pixel 6 class and up
             * (6 GB of RAM or more as the yardstick), native on smaller ones.
             * Flags scale1 / scale2 / scale3 override. */
            long kb = 0;
            FILE *mf = fopen("/proc/meminfo", "r");
            if (mf) { char ln[128]; while (fgets(ln, sizeof ln, mf))
                        if (sscanf(ln, "MemTotal: %ld kB", &kb) == 1) break;
                      fclose(mf); }
            /* x2 stays opt-in (flag scale2) until the small render targets
             * render at native size in x2: on a Pixel 6 x2 gameplay runs
             * 20-25 fps once the phone is warm. */
            int sc = (kb >= 5500000 && getenv("CONKER_AUTO_X2")) ? 2 : 1;
            /* the in-game settings (Java): x1 / x2, x2 by default */
            const char *us = getenv("CONKER_SET_SCALE");
            if (us && (*us == '1' || *us == '2')) sc = *us - '0';
            setenv("RECOMP_GPU_SCALE", sc == 2 ? "2" : "1", 1);
            if (sc == 2 && !getenv("CONKER_NO_DRS")) setenv("CONKER_DRS", "1", 1);
            fprintf(stderr, "[BOOT] render scale x%d (RAM %ld MB)\n", sc, kb / 1024);
        }
        /* x2 needs the scaled inline uploads (render targets the CPU wrote) */
        if (getenv("RECOMP_GPU_SCALE") && atoi(getenv("RECOMP_GPU_SCALE")) > 1 &&
            !getenv("NV2A_NO_SCALED_INLINE_UP"))
            setenv("NV2A_SCALED_INLINE_UP", "1", 1);
        snprintf(s_snap_dir, sizeof s_snap_dir, "%s/dump", ext ? ext : ".");
        {   /* a switch file can only say 1: the WAV tap goes to dump/ */
            const char *w = getenv("RECOMP_AUDIO_WAV");
            if (w && !strcmp(w, "1")) {
                char wp[1100]; snprintf(wp, sizeof wp, "%s/dump/audio.wav", ext ? ext : ".");
                setenv("RECOMP_AUDIO_WAV", wp, 1);
            }
        }
        if (getenv("NV2A_VALIDATION")) setenv("RECOMP_GPU_VALIDATION", "1", 1);
    }
    {   /* A custom Vulkan driver (e.g. Turnip for Adreno): the first .so in
         * files/driver/. Shared storage cannot be mapped executable, so it is
         * copied into the app's private storage first. */
        char ddir[1100]; snprintf(ddir, sizeof ddir, "%s/driver", ext ? ext : ".");
        DIR *dir = opendir(ddir);
        struct dirent *de;
        const char *in = SDL_AndroidGetInternalStoragePath();
        {   /* SPIR-V and Vulkan pipeline caches, kept between runs (in the
             * app's own external folder: survives reinstalls, and adb can
             * collect the SPIR-V -- GPU-independent -- to ship in the APK) */
            char cd[1100]; snprintf(cd, sizeof cd, "%s/shadercache", ext ? ext : (in ? in : "."));
            setenv("NV2A_CACHE_DIR", cd, 0);
            /* SPIR-V shipped in the APK (assets/spv.pack, built from test
             * runs): shaders of every scene already compiled on the first
             * launch, on any GPU. Files already in the cache are kept. */
            SDL_RWops *rw = SDL_RWFromFile("spv.pack", "rb");
            if (rw) {
                char sd[1200]; snprintf(sd, sizeof sd, "%s/spv", cd);
                mkdir(cd, 0775); mkdir(sd, 0775); chmod(cd, 0775); chmod(sd, 0775);
                char magic[4]; uint32_t count = 0; unsigned added = 0;
                if (SDL_RWread(rw, magic, 4, 1) == 1 && !memcmp(magic, "SPVP", 4) &&
                    SDL_RWread(rw, &count, 4, 1) == 1) {
                    for (uint32_t i = 0; i < count; i++) {
                        uint16_t nl; uint32_t len; char name[64];
                        if (SDL_RWread(rw, &nl, 2, 1) != 1 || nl == 0 || nl >= sizeof name) break;
                        if (SDL_RWread(rw, name, nl, 1) != 1) break;
                        name[nl] = 0;
                        if (SDL_RWread(rw, &len, 4, 1) != 1 || len > (16u << 20)) break;
                        char fp[1300]; snprintf(fp, sizeof fp, "%s/%s", sd, name);
                        struct stat st;
                        if (strchr(name, '/') || stat(fp, &st) == 0) {
                            SDL_RWseek(rw, len, RW_SEEK_CUR);
                            continue;
                        }
                        void *buf = malloc(len);
                        if (!buf || SDL_RWread(rw, buf, len, 1) != 1) { free(buf); break; }
                        FILE *fo = fopen(fp, "wb");
                        if (fo) { fwrite(buf, 1, len, fo); fclose(fo); added++; }
                        free(buf);
                    }
                }
                SDL_RWclose(rw);
                fprintf(stderr, "[BOOT] shader pack: %u entries, %u new in the cache\n", count, added);
            }
            {   /* the device's whole SPIR-V cache as one pack in the dump
             * folder (adb cannot read the cache files themselves): pulled
             * with the captures, merged into the APK's spv.pack */
                static char s_spv_dir[1200];
                snprintf(s_spv_dir, sizeof s_spv_dir, "%s/spv", cd);
                pthread_t th;
                if (pthread_create(&th, NULL, export_spv_cache, s_spv_dir) == 0) pthread_detach(th);
            }
            /* pipelines recorded in test runs, by level (pipeline prewarm,
             * nv2a draw.c): copied next to the device's own record */
            {   /* the device's own record into the dump folder (pulled) */
                char src[1200], dst[1200];
                snprintf(src, sizeof src, "%s/pipekeys.bin", cd);
                snprintf(dst, sizeof dst, "%s/pipekeys.bin", getenv("NV2A_DUMP_DIR") ? getenv("NV2A_DUMP_DIR") : ".");
                FILE *fi = fopen(src, "rb");
                if (fi) {
                    FILE *fo = fopen(dst, "wb");
                    char b[65536]; size_t n;
                    while (fo && (n = fread(b, 1, sizeof b, fi)) > 0) fwrite(b, 1, n, fo);
                    if (fo) fclose(fo);
                    fclose(fi);
                }
            }
            SDL_RWops *pk = SDL_RWFromFile("pipekeys.bin", "rb");
            if (pk) {
                Sint64 sz = SDL_RWsize(pk);
                char pp[1200]; snprintf(pp, sizeof pp, "%s/pipekeys_ship.bin", cd);
                struct stat st;
                mkdir(cd, 0775);
                if (sz > 0 && (stat(pp, &st) != 0 || st.st_size != sz)) {
                    void *buf = malloc((size_t)sz);
                    if (buf && SDL_RWread(pk, buf, (size_t)sz, 1) == 1) {
                        FILE *fo = fopen(pp, "wb");
                        if (fo) { fwrite(buf, 1, (size_t)sz, fo); fclose(fo); }
                    }
                    free(buf);
                }
                SDL_RWclose(pk);
                fprintf(stderr, "[BOOT] pipeline list: %lld bytes shipped\n", (long long)sz);
            }
        }
        /* a driver picked in the settings (adrenotools) wins over files/driver */
        if (getenv("NV2A_ADRENO_DRIVER_NAME")) {
            fprintf(stderr, "[BOOT] GPU driver from the settings: %s%s\n",
                    getenv("NV2A_ADRENO_DRIVER_DIR"), getenv("NV2A_ADRENO_DRIVER_NAME"));
            if (dir) closedir(dir);
            dir = NULL;
        }
        while (dir && in && (de = readdir(dir))) {
            size_t n = strlen(de->d_name);
            if (n < 4 || strcmp(de->d_name + n - 3, ".so")) continue;
            char src[1400], dst[1400];
            snprintf(src, sizeof src, "%s/%s", ddir, de->d_name);
            snprintf(dst, sizeof dst, "%s/vkdriver", in);
            mkdir(dst, 0700);
            snprintf(dst, sizeof dst, "%s/vkdriver/%s", in, de->d_name);
            unlink(dst);
            FILE *fi = fopen(src, "rb"), *fo = fi ? fopen(dst, "wb") : NULL;
            size_t total = 0;
            if (fi && fo) {
                char buf[65536]; size_t k;
                while ((k = fread(buf, 1, sizeof buf, fi)) > 0) total += fwrite(buf, 1, k, fo);
            }
            if (fi) fclose(fi);
            if (fo) fclose(fo);
            if (total) {
                chmod(dst, 0700);
                setenv("NV2A_VK_DRIVER", dst, 1);
                fprintf(stderr, "[BOOT] custom Vulkan driver %s (%zu bytes)\n", src, total);
            }
            break;
        }
        if (dir) closedir(dir);
    }
    snprintf(s_save_dir, sizeof s_save_dir, "%s/saves", ext ? ext : ".");
    set_default_env("RECOMP_FB_WINDOW", "1");
    set_default_env("RECOMP_FULLSCREEN", "1");
    /* The one-CPU DISPATCH gate. XAPI's USB stack allocates transfer
     * descriptors from one free list both on the title thread (the rumble
     * SET_REPORT, at raised IRQL) and in the done-queue DPC; on the console
     * the raise keeps them apart, here they ran at once and handed the same
     * TD to two endpoints. Run after run the interrupt endpoint's chain then
     * went into the control endpoint's: input died, or the USB DPC looped
     * in sub_0055FDF5 forever with the GPU interrupt held off behind it and
     * the game frozen. CONKER_NO_GATE=1 turns it off. */
    if (!getenv("CONKER_NO_GATE"))
        set_default_env("RECOMP_DISPATCH_GATE", "1");
    /* Widescreen: the title renders anamorphic 16:9 when the dashboard's
     * widescreen bit is set; the presenter then shows it 16:9.
     * CONKER_43=1: original 4:3. CONKER_FILL=1: stretch to the whole
     * screen (wider than 16:9 on most phones). */
    {
        extern void nv2a_vk_present_set_aspect(float aspect);
        if (!getenv("CONKER_43")) {
            set_default_env("XBOX_WIDESCREEN", "1");
            nv2a_vk_present_set_aspect(getenv("CONKER_FILL") ? -1.0f : 16.0f / 9.0f);
        }
    }
#else
    const char *dir = argc > 1 ? argv[1] : getenv("CONKER_GAME_DIR");
    snprintf(s_game_dir, sizeof s_game_dir, "%s", dir ? dir : "game");
    snprintf(s_save_dir, sizeof s_save_dir, "%s", argc > 2 ? argv[2] : "saves");
#endif
    (void)argc; (void)argv;
    mkdir(s_save_dir, 0755);
    {   /* the launch data page kept over a self-relaunch (kernel_bridge.c) */
        char rf[1100]; snprintf(rf, sizeof rf, "%s/../relaunch_page.bin", s_game_dir);
        set_default_env("XBOX_RELAUNCH_FILE", rf);
    }
    /* diagnostic build: the sampling profiler on (slowdowns after going
     * back to an earlier area, half-second stalls of the game thread);
     * CONKER_PROF=0 turns it off */
    set_default_env("CONKER_PROF", "1");
    if (!strcmp(getenv("CONKER_PROF"), "0")) unsetenv("CONKER_PROF");

    /* GPU: xemu's NV2A on the host GPU (RECOMP_GPU=xemu, the default where it
     * is built in) or the toolkit's pushbuffer executor and CPU rasteriser
     * (RECOMP_GPU=sw). */
#if defined(XBOXRECOMP_NV2A_XEMU)
    set_default_env("RECOMP_GPU", "xemu");
#else
    set_default_env("RECOMP_GPU", "sw");
#endif
    s_gpu_xemu = strcmp(getenv("RECOMP_GPU"), "xemu") == 0;
    s_gpu_null = getenv("RECOMP_GPU_NULL") != NULL;
    if (s_gpu_xemu) {
        unsetenv("RECOMP_PB_EXEC");
        setenv("RECOMP_VBLANK", "0", 1);       /* the GPU model raises its own */
        unsetenv("RECOMP_FB_WINDOW");          /* frames go to a Vulkan swapchain */
        /* D3D waits on a KEVENT it builds itself; the GPU interrupt's DPC sets it. */
        set_default_env("RECOMP_TITLE_KEVENTS", "1");
#ifdef __ANDROID__
        /* Diagnostic build: GPU state every 2 s, guest stack every 15 s. */
        if (getenv("NV2A_DIAG")) {   /* files/nv2a_diag: GPU state + guest stacks */
            set_default_env("NV2A_DEBUG", "1");
            set_default_env("CONKER_STACKDUMP_SECS", "15");
        }
        if (getenv("NV2A_STACK")) {  /* files/nv2a_stack: guest + host stacks only */
            set_default_env("CONKER_STACKDUMP_SECS", "20");
            set_default_env("CONKER_STACKDUMP_ALL", "1");
        }
        set_default_env("RECOMP_KEYBOARD", "1");   /* keys as pad 0: Enter=Start, Z=A */
        /* Without the OHCI model XAPI enumerates no controller at all and the
         * keyboard/touch pad never reaches the title. */
        set_default_env("RECOMP_USB", "1");
        /* Phones miss the 10 ms audio deadline under load; 21 ms buffers. */
        set_default_env("RECOMP_AUDIO_BUF_SAMPLES", "1024");
#endif
    } else {
        set_default_env("RECOMP_PB_EXEC", "1");
    }
    set_default_env("RECOMP_AC97_READY", "1");
    /* Conker's CRT heap grows with VirtualAlloc across the user address
     * space; without the extended-VMA tracker its free-region scan never
     * ends. D3D's Present waits on vblank interrupts. */
    set_default_env("RECOMP_EXT_VMA", "1");
    /* big contiguous blocks from the top, as the console kernel places them:
     * keeps the free memory in one run for a big level block (xbox_memory_layout.c) */
    set_default_env("RECOMP_CONTIG_TOPDOWN_MB", "4");
    set_default_env("RECOMP_VBLANK", "1");   /* sw path only; xemu set it 0 */

    fprintf(stderr, "=== Conker: Live and Reloaded — static recompilation ===\n");
    fprintf(stderr, "[BOOT] game dir: %s\n", s_game_dir);

    char xbe_path[1100];
    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", s_game_dir);
    if (!load_file(xbe_path, &s_xbe_data, &s_xbe_size)) {
        fprintf(stderr, "[BOOT] cannot read %s — copy the disc contents there\n", xbe_path);
#ifdef __ANDROID__
        char msg[1400];
        snprintf(msg, sizeof msg,
                 "Game files not found.\n\nCopy the contents of the Conker disc "
                 "(default.xbe, dvddata/, media/) to:\n%s", s_game_dir);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Conker", msg, NULL);
#endif
        return 1;
    }
    fprintf(stderr, "[BOOT] XBE loaded: %zu bytes\n", s_xbe_size);

    install_fault_handler();
#if defined(__ANDROID__) && defined(XBOXRECOMP_NV2A_XEMU)
    if (s_gpu_xemu) {
        int rc = android_gpu_main();
        free(s_xbe_data);
        return rc;
    }
#endif
    int rc = xbox_HostWindowMain(guest_main);
    free(s_xbe_data);
    return rc;
}

/* The title relaunching itself (XLaunchNewImage of its own default.xbe, the
 * Xbox's way out of an exhausted heap) or quitting. ExitProcess ran the C++
 * exit handlers under the still-running UI thread, which then took a
 * destroyed text-layout mutex: SIGABRT, an Android crash dialog, a crash
 * report. Instead: a relaunch starts a fresh game process (the console would
 * reboot into the title too), and the process ends without exit handlers. */
#include <SDL_system.h>
#include <unwind.h>
#include <dlfcn.h>
static _Unwind_Reason_Code exit_bt_cb(struct _Unwind_Context *ctx, void *arg)
{
    int *n = (int *)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    Dl_info di;
    if (pc && dladdr((void *)pc, &di) && di.dli_sname)
        fprintf(stderr, "[EXIT]   #%d %s+0x%lx\n", *n, di.dli_sname, (unsigned long)(pc - (uintptr_t)di.dli_saddr));
    return ++*n < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
void xbox_title_exit_hook(unsigned routine)
{
    int relaunch = routine == 2;          /* HalQuickRebootRoutine: XLaunchNewImage */
    fprintf(stderr, "[EXIT] title %s\n", relaunch ? "relaunches itself: restarting the app" : "quits");
    /* who decided it: the recompiled functions on the host stack are the
     * guest call chain (sub_XXXXXXXX = guest address) */
    { int n = 0; _Unwind_Backtrace(exit_bt_cb, &n); }
    { extern void xbox_HeapReportState(const char *why); xbox_HeapReportState("title relaunch"); }
    fflush(stderr);
    /* the restarted game (or a test runner) clears the log: keep this one */
    save_hang_log(relaunch ? "relaunch" : "quit");
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (env && act) {
        jclass c = (*env)->GetObjectClass(env, act);
        jmethodID m = (*env)->GetMethodID(env, c, "titleExited", "(Z)V");
        if (m) (*env)->CallVoidMethod(env, act, m, (jboolean)relaunch);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    usleep(400 * 1000);
    _exit(0);
}

/* Settings: frame-rate mode 30 / 40 / 60, at once */
JNIEXPORT void JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeSetFps(JNIEnv *env, jclass cls, jint fps)
{
    (void)env; (void)cls;
    extern void xbox_SetFpsMode(int);
    xbox_SetFpsMode((int)fps);
}

/* Settings: sharpness / anti-aliasing take effect at once (display shader
 * rebuilt on the next frame, nv2a-port display.c). */
JNIEXPORT void JNICALL
Java_org_recomp_conker_TouchGamepadView_nativeSetDisplayFx(JNIEnv *env, jclass cls, jint sharp, jint aa)
{
    (void)env; (void)cls;
    extern void nv2a_set_display_fx(int sharpness, int aa);
    nv2a_set_display_fx((int)sharp, (int)aa);
}
