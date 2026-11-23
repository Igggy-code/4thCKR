#include <sched.h>
#include <math.h>
#include <time.h>
#include <string.h>
/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

#include "../../xboxrecomp/src/kernel/recomp_cpu_tls.h" /* was: extern __thread uint32_t g_eax, g_ecx; */
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* TEMP diagnostics: every indirect call (virtual methods, callbacks) of the
 * title passes here. While a CUT / GAME label is set (main.c), the call
 * count of each target is kept, and dumped every 3 s -- gameplay-only and
 * cutscene-only methods to detect the state by. */
#define IC_HN (1 << 16)
uint32_t g_ic_key[IC_HN];
volatile uint32_t g_ic_cnt[IC_HN];
volatile int g_ic_on;
static inline void ic_count(uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 16;
    for (int k = 0; k < 64; k++, h = (h + 1) & (IC_HN - 1)) {
        uint32_t cur = g_ic_key[h];
        if (cur == va) { __atomic_fetch_add(&g_ic_cnt[h], 1, __ATOMIC_RELAXED); return; }
        if (!cur) {
            if (__atomic_compare_exchange_n(&g_ic_key[h], &cur, va, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED) || cur == va) {
                __atomic_fetch_add(&g_ic_cnt[h], 1, __ATOMIC_RELAXED);
                return;
            }
        }
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (g_ic_on) ic_count(xbox_va);
    /* gameplay: the player's per-frame updates, dispatched as virtual calls.
     * In none of 130 intervals of intro cutscenes and loading, every frame
     * of gameplay. */
    if (xbox_va == 0x0030DAA0u || xbox_va == 0x00331710u) {
        extern volatile int64_t g_conker_last_play_ms;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        g_conker_last_play_ms = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
        extern volatile unsigned nv2a_flip_seq, g_conker_play_flip;
        g_conker_play_flip = nv2a_flip_seq;
    }
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /* per address: the first few in full, then one line per 2^n -- a
     * failing call inside a loop logged 100 lines a second and the logging
     * itself froze the game */
    enum { SLOTS = 64 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;
    for (i = 0; i < count; i++) if (seen[i] == va) break;
    if (i == count && count < SLOTS) { seen[count] = va; hits[count] = 0; count++; }
    if (i < SLOTS) {
        uint64_t h = ++hits[i];
        if (h > 4) {
            if ((h & (h - 1)) == 0)
                fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X: %llu times\n",
                        va, (unsigned long long)h);
            return;
        }
    }
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Conker: Live and Reloaded — title-specific overrides ─────────────────
 *
 * XDK D3D8LTCG (XDK 5849). The device lives at MEM32(0x005499E8); D3D's
 * fence wait D3D_BlockOnTime is sub_0053C190 (called from Present at
 * 0x0053D50B). Its wait reads the GPU semaphore through device+0x30. With
 * RECOMP_PB_EXEC the executor writes BACK_END_WRITE_SEMAPHORE_RELEASE values
 * there -- once it has been told where "there" is. Tell it on the first wait,
 * and again whenever D3D moves the semaphore (device reset), then run the
 * original wait. See docs/technical/d3d8ltcg-device-context.md.
 */
#define CONKER_D3D_DEVICE_PTR 0x005499E8u
#define CONKER_GMEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_0053C190_gen(void);

static void conker_arm_gpu_semaphore(void)
{
    static uint32_t s_sem;
    uint32_t dev = CONKER_GMEM32(CONKER_D3D_DEVICE_PTR);
    if (!dev) return;
    uint32_t sem = CONKER_GMEM32(dev + 0x30);
    if (sem && sem != s_sem) {
        s_sem = sem;
        nv2a_pb_set_semaphore_target(sem);
        fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n", sem, dev);
    }
}

/* D3D_BlockOnTime(fence, flags) -- ret 8 */
#include <time.h>
#include "../../xboxrecomp/src/kernel/recomp_cpu_tls.h" /* was: extern __thread uint32_t g_esp; */
/* CONKER_PROF: who waits for the GPU, how long, and how far behind it was */
static struct { uint32_t ra; unsigned n; double ms; double lag; double ahead; } s_bot[16];
static double s_bot_t0;
static double bot_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
extern void nv2a_tl(int, uint32_t);
void sub_0053C190(void)
{
    conker_arm_gpu_semaphore();
    static int prof = -1;
    if (prof < 0) prof = getenv("CONKER_PROF") != NULL;
    uint32_t ra = CONKER_GMEM32(g_esp), fence = CONKER_GMEM32(g_esp + 4);
    static int nap = -1;
    if (nap < 0) { const char *e = getenv("CONKER_BOT_NAP_US"); nap = e ? atoi(e) : 150; }
    extern __thread int g_xbox_spin_nap_us;
    /* nv2a-port: sleep on the GPU's semaphore release instead of spinning
     * in the title's poll loop, when the awaited fence is already submitted
     * (fence != current time). A release wakes us at once; if the
     * semaphore stops moving for 4 ms (nothing submitted?) the title's own
     * wait takes over and kicks the push buffer. CONKER_NO_SEM_WAIT=1: off. */
    static int semwait = -1;
    if (semwait < 0) semwait = !getenv("CONKER_NO_SEM_WAIT");
    if (semwait) {
        uint32_t dev = CONKER_GMEM32(CONKER_D3D_DEVICE_PTR);
        if (dev) {
            uint32_t now_t = CONKER_GMEM32(dev + 0x2C);
            uint32_t semp = CONKER_GMEM32(dev + 0x30);
            if (semp && fence != now_t) {
                extern void nv2a_sem_wait(uint32_t semva, uint32_t fence, int stall_ms);
                static int stall = -1;
                if (stall < 0) { const char *e = getenv("CONKER_SEM_STALL_MS"); stall = e ? atoi(e) : 4; }
                nv2a_sem_wait(semp, fence, stall);
            }
        }
    }
    if (!prof) {
        nv2a_tl(1, ra);
        g_xbox_spin_nap_us = nap;
        { extern volatile uint32_t nv2a_semwait_fence, nv2a_semwait_active;
          nv2a_semwait_fence = fence; nv2a_semwait_active = 2; }
        { extern double g_xmv_bot_s; double b0 = bot_now();
          sub_0053C190_gen();
          g_xmv_bot_s += (bot_now() - b0) * 1e-3; }
        { extern volatile uint32_t nv2a_semwait_active; nv2a_semwait_active = 0; }
        g_xbox_spin_nap_us = 0;
        nv2a_tl(2, fence);
        return;
    }
    g_xbox_spin_nap_us = nap;
    uint32_t dev = CONKER_GMEM32(CONKER_D3D_DEVICE_PTR);
    uint32_t now_t = dev ? CONKER_GMEM32(dev + 0x2C) : 0;
    uint32_t sem = dev ? CONKER_GMEM32(CONKER_GMEM32(dev + 0x30)) : 0;
    double t0 = bot_now();
    nv2a_tl(1, ra);
    sub_0053C190_gen();
    g_xbox_spin_nap_us = 0;
    nv2a_tl(2, fence);
    double t1 = bot_now();
    int k;
    for (k = 0; k < 16 && s_bot[k].ra && s_bot[k].ra != ra; k++) ;
    if (k < 16) {
        s_bot[k].ra = ra; s_bot[k].n++; s_bot[k].ms += t1 - t0;
        s_bot[k].lag += (double)(int32_t)(now_t - fence);   /* how old the awaited fence is */
        s_bot[k].ahead += (double)(int32_t)(fence - sem);   /* how far the GPU still had to go */
    }
    if (!s_bot_t0) s_bot_t0 = t1;
    if (t1 - s_bot_t0 > 5000) {
        char line[1200]; int o = 0;
        for (k = 0; k < 16 && s_bot[k].ra; k++) {
            o += snprintf(line + o, sizeof line - o, " ra=%08X n=%u wait=%.0fms lag=%.0f ahead=%.0f;",
                          s_bot[k].ra, s_bot[k].n, s_bot[k].ms, s_bot[k].lag / s_bot[k].n,
                          s_bot[k].ahead / s_bot[k].n);
            s_bot[k].n = 0; s_bot[k].ms = s_bot[k].lag = s_bot[k].ahead = 0;
        }
        fprintf(stderr, "[BLOCKONTIME] 5s:%s\n", line);
        s_bot_t0 = t1;
    }
}

/* D3D resource IsBusy (sub_0053B940, ecx = resource; eax = 1 while the GPU
 * still uses it). The title polls it in tight loops (BlockUntilNotBusy-style
 * waits from 0x003F5649, 0x003FA50C, ...) -- with D3D_BlockOnTime the spin
 * that kept a big core at 100% and the phone throttling. A resource found
 * busy several times in a row makes the poll nap. CONKER_NO_BUSY_NAP=1
 * restores the spin. */
/* Camera frustum (sub_003FD7F0, ecx = camera, eax = out: left, right,
 * bottom, top, near, far, from fov at +0x104 and aspect at +0x108). The
 * game's cameras are 4:3. On a wider screen the 4:3 cameras get the
 * screen's aspect here, so the projection and the game's own culling both
 * see the wider view (no geometry missing at the edges). Other aspects
 * (shadow and effect cameras) are left alone. conker_cam_aspect is set at
 * boot from the screen; 0 = off. Logs the first aspects it sees. */
float conker_cam_aspect;
extern char g_xbox_level_name[48];
/* Shots kept 4:3 (then stretched to the screen like the pause screen):
 * the opening of the Throne Room, the great doors framed to fill a 4:3
 * screen -- widened, the walls, candles and pillars showed beside them.
 * Its camera: aspect 1.2120 (fov 50), the only one the scene uses.
 * CONKER_WIDE_DOORS=1 widens it like the rest. */
/* Set by the projection hook from the camera that owns the matrix: the
 * opening shot straight down the hall at the doors -- the camera looks
 * along -z with no tilt, at x -21..-25, y ~144, dollying from z 2153 to ~580
 * (measured in the CAMMAT logs). The Throne Room's other cutscenes (between
 * chapters, the castle) use the same lens but other camera paths and stay
 * widened. */
static int s_door_shot;
static int conker_door_cam(uint32_t out)
{
    const float *c = (const float *)((uintptr_t)g_xbox_mem_offset + out - 0x140);
    return fabsf(c[48] + 1.0f) < 0.03f && fabsf(c[50]) < 0.03f &&
           fabsf(c[56]) < 0.03f && fabsf(c[58] + 1.0f) < 0.03f &&
           c[60] > -32.0f && c[60] < -14.0f && c[61] > 136.0f && c[61] < 152.0f &&
           c[62] > 450.0f && c[62] < 2250.0f;
}
static int conker_keep43(float a)
{
    static int off = -1;
    if (off < 0) off = getenv("CONKER_WIDE_DOORS") != NULL;
    if (off || *(uint32_t *)&a != 0x3f9b22d1u) return 0;
    if (strcmp(g_xbox_level_name, "ThroneRoom")) return 0;
    if (!s_door_shot) return 0;
    static int logged;
    if (!logged++) fprintf(stderr, "[CAM] Throne Room doors: 4:3 camera, stretched\n");
    return 1;
}
/* Perspective matrix (sub_003E2A30: eax = out 4x4, [esp+4] = fov in
 * degrees, [esp+8] = aspect; m[0] = cot(fov/2) / aspect). 4:3 matrices get
 * the screen's aspect. */
extern void sub_003E2A30_gen(void);
void sub_003E2A30(void)
{
    uint32_t out = g_eax;
    float a = *(float *)((uintptr_t)g_xbox_mem_offset + g_esp + 8);
    float fov = *(float *)((uintptr_t)g_xbox_mem_offset + g_esp + 4);
    sub_003E2A30_gen();
    static int logged;
    {   /* the log starts over in each level, so its shots can be found */
        static char lvl[48];
        if (strcmp(lvl, g_xbox_level_name)) { strcpy(lvl, g_xbox_level_name); logged = 0;
            fprintf(stderr, "[CAM] level %s\n", lvl);
            extern void conker_phys_level_change(void); conker_phys_level_change(); }
    }
    if (logged < 120) {
        static float seen[120];
        int k, known = 0;
        for (k = 0; k < logged; k++) if (seen[k] == a + fov * 1000.0f) known = 1;
        if (!known) { seen[logged++] = a + fov * 1000.0f;
            fprintf(stderr, "[CAM] projection aspect %.4f fov %.2f (%08x %08x) in %s\n", a, fov,
                    *(uint32_t *)&a, *(uint32_t *)&fov, g_xbox_level_name); }
    }
    /* the game's aspects for a 4:3 TV run 1.21..1.33 (it adjusts with the
     * fov); scale each for the wider screen, keeping its own adjustment */
    /* The in-game pause screen is a 4:3 3D scene: a picture frame on a wall,
     * with the paused frame behind it. Widened, the wall ends short of the
     * screen edge and the picture slips out of its frame. Paused = in a
     * level, no gameplay for 0.4 s, the UI drawing (cutscenes hide it): keep
     * its camera 4:3 and show it pillarboxed. */
    extern volatile int g_conker_in_level;
    extern volatile int64_t g_conker_last_play_ms, g_conker_last_ui_ms;
    extern volatile int nv2a_force_pillar;
    {   struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        /* paused: the scene keeps its 4:3 camera, so the picture sits in its
         * frame, and is stretched to the screen like the rest (no side bars,
         * the user's choice). CONKER_PAUSE_43=1: pillarboxed instead;
         * CONKER_PAUSE_WIDE=1: widened like gameplay. */
        static int mode = -1;
        if (mode < 0) mode = getenv("CONKER_PAUSE_WIDE") ? 0 : getenv("CONKER_PAUSE_43") ? 2 : 1;
        extern int conker_paused(int64_t now);
        int paused = mode && conker_paused(now);
        static int was;
        if (paused != was) {
            was = paused;
            nv2a_force_pillar = paused && mode == 2;
            fprintf(stderr, "[PAUSE] %s\n", paused ? "pause screen: 4:3 camera" : "back to widescreen");
        }
        if (paused) {
            /* still a 3D scene: no 4:3 art pillarbox for it (present.c) */
            extern volatile int64_t nv2a_wide_cam_us;
            nv2a_wide_cam_us = now * 1000;
            return;
        }
    }
    /* In a level, an exact 4:3 projection is a HUD camera: the 3D tail
     * of the lives counter (top right) is drawn through one, next to its 2D
     * number. Widened, the tail slid toward the centre and over the number;
     * left 4:3 it is stretched with the rest of the HUD and stays beside it.
     * (Gameplay cameras adjust the aspect with the fov: 1.19..1.27.) */
    if (g_conker_in_level && fabsf(a - 4.0f / 3.0f) < 0.002f) {
        static int nlog;
        if (nlog++ < 3) fprintf(stderr, "[CAM] in-level 4:3 projection (fov %.1f): HUD camera, not widened\n", fov);
        return;
    }
    if (*(uint32_t *)&a == 0x3f9b22d1u && !strcmp(g_xbox_level_name, "ThroneRoom")) {
        /* the memory around the matrix (the camera holding it), logged as it
         * changes: which shots of this camera are the doors */
        static int64_t last; static float prev[96];
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        const float *c = (const float *)((uintptr_t)g_xbox_mem_offset + out - 0x140);
        int moved = 0;
        for (int i = 0; i < 96; i++) if (!(fabsf(c[i] - prev[i]) <= 0.01f * (fabsf(prev[i]) + 1.0f))) moved = 1;
        if (moved && now - last > 250) {
            last = now; memcpy(prev, c, sizeof prev);
            char buf[1800]; int o = 0;
            for (int i = 0; i < 96 && o < 1750; i++) o += snprintf(buf + o, sizeof buf - o, "%.4g ", c[i]);
            fprintf(stderr, "[CAMMAT] out %08x esp %08x: %s\n", out, g_esp, buf);
        }
    }
    if (*(uint32_t *)&a == 0x3f9b22d1u)
        s_door_shot = conker_door_cam(out);
    if (conker_keep43(a)) {
        extern volatile int64_t nv2a_wide_cam_us;   /* a 3D scene: no pillarbox */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        nv2a_wide_cam_us = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
        return;
    }
    if (conker_cam_aspect > 0.0f && a > 1.0f && a < 1.5f) {
        float *m = (float *)((uintptr_t)g_xbox_mem_offset + out);
        m[0] *= (4.0f / 3.0f) / conker_cam_aspect;
        /* an exact 4:3 projection (any fov) is what movies and 2D screens
         * set; the game's 3D scenes adjust the aspect with the fov
         * (1.21..1.27) -- the exact ones widened the intro movie */
        if (!(fabsf(a - 4.0f / 3.0f) < 0.002f)) {   /* 3D scenes adjust it: 1.21..1.27 */
            extern volatile int64_t nv2a_wide_cam_us;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            nv2a_wide_cam_us = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000; }
    }
}

/* Culling frustum from the same camera (sub_003FAE90: eax -> fov, aspect,
 * near, far): the same widening, or the game culls everything outside the
 * old 4:3 view and the screen edges show holes. */
extern void sub_003FAE90_gen(void);
void sub_003FAE90(void)
{
    float *asp = (float *)((uintptr_t)g_xbox_mem_offset + g_eax + 4);
    float a = *asp;
    extern volatile int g_conker_in_level;
    int w = conker_cam_aspect > 0.0f && a > 1.0f && a < 1.5f &&
            !(g_conker_in_level && fabsf(a - 4.0f / 3.0f) < 0.002f) && !conker_keep43(a);
    if (w) *asp = a * conker_cam_aspect / (4.0f / 3.0f);
    sub_003FAE90_gen();
    if (w) *asp = a;
}

extern void sub_003FD7F0_gen(void);
void sub_003FD7F0(void)
{
    uint32_t cam = g_ecx, out = g_eax;
    sub_003FD7F0_gen();
    float a = *(float *)((uintptr_t)g_xbox_mem_offset + cam + 0x108);
    /* the Throne Room's one camera, logged as it moves: which of its shots
     * are the doors (to keep only those 4:3) */
    if (*(uint32_t *)&a == 0x3f9b22d1u && !strcmp(g_xbox_level_name, "ThroneRoom")) {
        static int64_t last; static float prev[68];
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        const float *c = (const float *)((uintptr_t)g_xbox_mem_offset + cam);
        int moved = 0;
        for (int i = 0; i < 68; i++) if (fabsf(c[i] - prev[i]) > 0.01f * (fabsf(prev[i]) + 1.0f)) moved = 1;
        if (moved && now - last > 250) {
            last = now; memcpy(prev, c, sizeof prev);
            char buf[1200]; int o = 0;
            for (int i = 0; i < 68 && o < 1150; i++) o += snprintf(buf + o, sizeof buf - o, "%g ", c[i]);
            fprintf(stderr, "[CAMPOS] cam %08x: %s\n", cam, buf);
        }
    }
    static int logged;
    if (logged < 12) {
        static float seen[12];
        int k, known = 0;
        for (k = 0; k < logged; k++) if (seen[k] == a) known = 1;
        if (!known) { seen[logged++] = a;
            fprintf(stderr, "[CAM] aspect %.4f (cam %08x)\n", a, cam); }
    }
    /* the same range and factor as the projection hook above: the game's
     * gameplay cameras run aspects 1.19..1.27 (adjusted with the fov), and
     * left at 4:3 here, what is drawn through this frustum (the ladder's
     * side rails, among others) sat narrower than the widened rest -- rails
     * shifted off their rungs, or out of view */
    extern volatile int g_conker_in_level;
    if (conker_cam_aspect > 0.0f && a > 1.0f && a < 1.5f &&
        !(g_conker_in_level && fabsf(a - 4.0f / 3.0f) < 0.002f) && !conker_keep43(a)) {   /* HUD camera */
        float *o = (float *)((uintptr_t)g_xbox_mem_offset + out);
        float k = conker_cam_aspect / (4.0f / 3.0f);
        o[2] *= k;   /* left */
        o[3] *= k;   /* right */
    }
}

extern void sub_0053B940_gen(void);
/* g_eax, g_ecx: declared by the recomp headers */
void sub_0053B940(void)
{
    static __thread uint32_t last_res;
    static __thread unsigned streak;
    static int nap = -1;
    if (nap < 0) nap = getenv("CONKER_NO_BUSY_NAP") ? 0 : 1;
    uint32_t res = g_ecx;
    sub_0053B940_gen();
    if (!nap) return;
    if (g_eax == 1) {
        if (res == last_res) {
            if (++streak >= 4) {
                struct timespec t = { 0, 150000 };
                nanosleep(&t, NULL);
            }
        } else {
            last_res = res; streak = 1;
        }
    } else {
        last_res = 0; streak = 0;
    }
}

/* Game-state hooks: functions the title runs only in gameplay (the player's
 * control update: 0x00025460, 0x00025090, 0x002C4AF0) or in gameplay and
 * menus (the front end / HUD: 0x002D3370), found by sampling labelled
 * gameplay and cutscenes. The time of the last call tells the touch
 * overlay what is on screen (conker_game_state in main.c). */
#include <time.h>
volatile int64_t g_conker_last_play_ms, g_conker_last_ui_ms;
volatile int g_conker_in_level;
static inline int64_t conker_now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
extern void sub_00025460_gen(void), sub_00025090_gen(void),
            sub_002C4AF0_gen(void), sub_002D3370_gen(void);
volatile unsigned g_conker_hook_calls[4];
void sub_00025460(void) { g_conker_hook_calls[0]++; sub_00025460_gen(); }
void sub_00025090(void) { g_conker_hook_calls[1]++; sub_00025090_gen(); }
volatile int64_t g_conker_last_world_ms;
/* the frame number at each hook's last call: "quiet" means frames went by
 * without it, not just time (a long hitch is no cutscene) */
volatile unsigned g_conker_play_flip, g_conker_world_flip, g_conker_ui_flip;
extern volatile unsigned nv2a_flip_seq;
void sub_002C4AF0(void) { g_conker_hook_calls[2]++; g_conker_last_world_ms = conker_now_ms(); g_conker_world_flip = nv2a_flip_seq; sub_002C4AF0_gen(); }
void sub_002D3370(void) { g_conker_hook_calls[3]++; g_conker_last_ui_ms = conker_now_ms(); g_conker_ui_flip = nv2a_flip_seq; sub_002D3370_gen(); }

/* GPU semaphore releases (pgraph BACK_END_WRITE_SEMAPHORE_RELEASE) wake
 * threads waiting for a fence. */
#include <pthread.h>
static pthread_mutex_t s_sem_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_sem_cv = PTHREAD_COND_INITIALIZER;
static volatile unsigned s_sem_gen;
void nv2a_sem_signal(void)
{
    pthread_mutex_lock(&s_sem_lock);
    s_sem_gen++;
    pthread_cond_broadcast(&s_sem_cv);
    pthread_mutex_unlock(&s_sem_lock);
}
extern volatile uint32_t nv2a_semwait_va, nv2a_semwait_fence, nv2a_semwait_active;
void nv2a_sem_wait(uint32_t semva, uint32_t fence, int stall_ms)
{
    nv2a_semwait_va = semva; nv2a_semwait_fence = fence; nv2a_semwait_active = 1;
    uint32_t last = CONKER_GMEM32(semva);
    struct timespec moved; clock_gettime(CLOCK_MONOTONIC, &moved);
    pthread_mutex_lock(&s_sem_lock);
    for (;;) {
        uint32_t sem = CONKER_GMEM32(semva);
        if ((int32_t)(sem - fence) >= 0) break;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (sem != last) { last = sem; moved = now; }
        long since = (now.tv_sec - moved.tv_sec) * 1000 + (now.tv_nsec - moved.tv_nsec) / 1000000;
        if (since >= stall_ms) break;
        /* a DPC the GPU waits on (its notify ack) may be queued behind this
         * very thread, which holds the one-CPU gate while it waits here: run
         * it (outside the lock: it may release this semaphore itself) */
        {   extern void xbox_DpcPollPoint(void), xbox_IrqSafePoint(void);
            pthread_mutex_unlock(&s_sem_lock);
            xbox_IrqSafePoint();   /* an interrupt posted to this holder, too */
            xbox_DpcPollPoint();
            pthread_mutex_lock(&s_sem_lock);
            if ((int32_t)(CONKER_GMEM32(semva) - fence) >= 0) break;
        }
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 2000000; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&s_sem_cv, &s_sem_lock, &dl);
    }
    pthread_mutex_unlock(&s_sem_lock);
    nv2a_semwait_active = 0;
}

/* nv2a-port: frame-rate modes. D3D's flip command carries the presentation
 * interval in bits 5..7 (Conker: 2, a flip every other 60 Hz vblank = 30
 * fps). Mode 60: interval 1, a flip on every 60 Hz vblank. Mode 40: the
 * vblanks run at 120 Hz (nv2a_xemu_glue.c) and the interval becomes 3. */
int conker_flip_interval(int iv)
{
    extern int xbox_FpsMode(void);
    int m = xbox_FpsMode();
    static unsigned cnt[8]; static time_t t0;
    cnt[iv & 7]++;
    time_t tn = time(NULL);
    if (!t0) t0 = tn;
    if (tn - t0 >= 10) {
        fprintf(stderr, "[FPS] flips by interval in 10 s: 0:%u 1:%u 2:%u 3:%u 4:%u (mode %d)\n",
                cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], m);
        {   /* the title's frame clock (sub_004162F0): frames counted, its
             * millisecond clock, last dt and time scale -- does game time
             * follow real time at 60 fps? */
            static uint32_t f0, ms0;
            uint32_t f = CONKER_GMEM32(0x84BE40), ms = CONKER_GMEM32(0x828B78);
            uint32_t dtb = CONKER_GMEM32(0x828B40), scb = CONKER_GMEM32(0x75861C), rdb = CONKER_GMEM32(0x828B80);
            float dt, sc, rd; memcpy(&dt, &dtb, 4); memcpy(&sc, &scb, 4); memcpy(&rd, &rdb, 4);
            fprintf(stderr, "[GCLOCK] in %ld s: %u frames, game clock +%u ms; dt %.4f (real %.4f) scale %.3f fixed %08x\n",
                    (long)(tn - t0), f - f0, ms - ms0, dt, rd, sc, CONKER_GMEM32(0x84BE48));
            f0 = f; ms0 = ms;
        }
        memset(cnt, 0, sizeof cnt); t0 = tn;
    }
    if (iv <= 0) return iv;                  /* immediate */
    if (m == 60) return 1;                   /* every 60 Hz vblank */
    if (m == 40) return iv == 1 ? 2 : 3;     /* 120 Hz vblanks: 60 or 40 fps */
    return iv;
}

/* nv2a-port: D3D_BlockOnTime ends in a tight loop that only reads the GPU
 * semaphore (0x0053C2C0: no kernel call, so no nap point): a quarter of the
 * game thread spun there, a core turned into heat. The generated code calls
 * this first: sleep on the semaphore release instead, then the title's own
 * loop runs its (now immediate) check. Loop condition: (now - sem) > diff.
 * CONKER_BOT_SPIN=1: the old spin. */
void conker_bot_spin(uint32_t semva, uint32_t now, uint32_t diff)
{
    static int spin = -1;
    if (spin < 0) spin = getenv("CONKER_BOT_SPIN") != NULL;
    if (spin || !semva) return;
    uint32_t fence = now - diff;
    struct timespec a_, b_; clock_gettime(CLOCK_MONOTONIC, &a_);
    for (int i = 0; i < 2000; i++) {
        if ((uint32_t)(now - CONKER_GMEM32(semva)) <= diff) break;
        nv2a_sem_wait(semva, fence, 2);
        if ((uint32_t)(now - CONKER_GMEM32(semva)) <= diff) break;
        sched_yield();
    }
    clock_gettime(CLOCK_MONOTONIC, &b_);
    extern void xbox_wait_prof(uint32_t, uint64_t, int);
    xbox_wait_prof(0x0053C2C0u, (uint64_t)((b_.tv_sec - a_.tv_sec) * 1000000000ll + (b_.tv_nsec - a_.tv_nsec)), 6);
}

/* nv2a-port: DirectSound checks every frame that the APU's DSP keeps a
 * marker word (0xCCCCCC) in memory it shares with it (pointer at 0x4E1C60).
 * Our DSP is a passthrough stub that never writes it, so DSound concluded
 * every frame that the APU had hung and reset it -- a reset that ends in a
 * 10 ms busy-wait, a third of the 30 fps frame. Let the first reset run,
 * then keep the marker in place. CONKER_APU_RESET_EACH=1: old behaviour. */
extern void sub_004C7DBF_gen(void);
void sub_004C7DBF(void)
{
    static int each = -1;
    static unsigned calls;
    if (each < 0) each = getenv("CONKER_APU_RESET_EACH") != NULL;
    sub_004C7DBF_gen();
    calls++;
    uint32_t p = CONKER_GMEM32(0x4E1C60);
    if (calls <= 3)
        fprintf(stderr, "[APU] DirectSound reset #%u, marker at 0x%08X = 0x%08X\n",
                calls, p, p ? CONKER_GMEM32(p) : 0);
    if (!each && p)
        CONKER_GMEM32(p) = 0xCCCCCC;
}

/* The title's allocator front end (malloc.cpp: size, file, line, flags).
 * When it comes back empty the title gives up and relaunches itself through
 * XLaunchNewImage (the 19:15 "crash" on the BarnOutside -> Windmill load).
 * Say what was asked for and by whom. */
void sub_002AF0E0_gen(void);
void sub_002AF0E0(void)
{
    uint32_t ra = CONKER_GMEM32(g_esp), sz = CONKER_GMEM32(g_esp + 4), file = CONKER_GMEM32(g_esp + 8), line = CONKER_GMEM32(g_esp + 12);
    sub_002AF0E0_gen();
    if (g_eax == 0) {
        char f[96] = "", ctx[96] = "";
        for (int i = 0; file && i < 95 && (f[i] = (char)*(volatile uint8_t *)((uintptr_t)(file + i) + g_xbox_mem_offset)); i++) {}
        for (int i = 0; i < 95 && (ctx[i] = (char)*(volatile uint8_t *)((uintptr_t)(0x84B680u + i) + g_xbox_mem_offset)); i++) {}
        f[95] = ctx[95] = 0;
        fprintf(stderr, "[MALLOC] FAILED: %u bytes (%u KB) for %s:%u, caller %08X, context '%s'\n",
                sz, sz / 1024, f, line, ra, ctx);
        extern void xbox_HeapReportState(const char *why);
        xbox_HeapReportState("title malloc failed");
    }
}

/* ---------------------------------------------------------------------------
 * Physical heap census. The title's 32 MB physical heap (created by
 * sub_002AF000 with MmAllocateContiguousMemoryEx(0x2070000), object at
 * [0x84A188]) serves textures, models and level data. When an allocation
 * from it fails, sub_00063230 calls the out-of-memory handler sub_001F05D0,
 * which relaunches the title unless D:\dvddata\DontRebootOnException.txt
 * exists -- the "restart while loading a level" seen after ~20 minutes and
 * several level changes. Track what is alive in that heap, by caller and
 * allocation context, and report it at every level change and on failure,
 * to tell a leak from fragmentation. CONKER_PHYSCENSUS=0 turns it off.
 * ------------------------------------------------------------------------- */
#define PC_SLOTS (1 << 17)
typedef struct { uint32_t ptr, size, ra; char ctx[12]; } PcEnt;
static PcEnt *s_pc;
static pthread_mutex_t s_pc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_pc_live, s_pc_live_pow2;
static uint32_t s_pc_count, s_pc_peak_mb;
static __thread uint32_t t_pc_ra, t_pc_ra2;

static int pc_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("CONKER_PHYSCENSUS"); on = !(e && *e == '0'); }
    return on;
}
static uint32_t pc_pow2(uint32_t n) { uint32_t p = 16; while (p < n && p) p <<= 1; return p ? p : n; }
static uint32_t pc_hash(uint32_t p) { return (p * 2654435761u) >> 15; }

static void pc_add(uint32_t ptr, uint32_t size, uint32_t ra)
{
    if (!s_pc) s_pc = calloc(PC_SLOTS, sizeof(PcEnt));
    if (!s_pc) return;
    uint32_t h = pc_hash(ptr) & (PC_SLOTS - 1);
    for (int i = 0; i < PC_SLOTS; i++, h = (h + 1) & (PC_SLOTS - 1)) {
        if (s_pc[h].ptr == 0 || s_pc[h].ptr == 1) {
            s_pc[h].ptr = ptr; s_pc[h].size = size; s_pc[h].ra = ra;
            for (int k = 0; k < 11; k++) {
                char c = (char)*(volatile uint8_t *)((uintptr_t)(0x84B680u + k) + g_xbox_mem_offset);
                s_pc[h].ctx[k] = c; if (!c) break;
            }
            s_pc[h].ctx[11] = 0;
            s_pc_live += size; s_pc_live_pow2 += pc_pow2(size); s_pc_count++;
            if (s_pc_live_pow2 >> 20 > s_pc_peak_mb) s_pc_peak_mb = (uint32_t)(s_pc_live_pow2 >> 20);
            return;
        }
    }
}
static void pc_del(uint32_t ptr)
{
    if (!s_pc || !ptr) return;
    uint32_t h = pc_hash(ptr) & (PC_SLOTS - 1);
    for (int i = 0; i < PC_SLOTS; i++, h = (h + 1) & (PC_SLOTS - 1)) {
        if (s_pc[h].ptr == 0) return;
        if (s_pc[h].ptr == ptr) {
            s_pc_live -= s_pc[h].size; s_pc_live_pow2 -= pc_pow2(s_pc[h].size); s_pc_count--;
            s_pc[h].ptr = 1;   /* tombstone */
            return;
        }
    }
}

typedef struct { uint32_t ra; char ctx[12]; uint64_t bytes; uint32_t n; } PcTop;
static void pc_report(const char *why)
{
    if (!pc_on()) return;
    pthread_mutex_lock(&s_pc_lock);
    static PcTop top[512]; int nt = 0;
    for (int i = 0; s_pc && i < PC_SLOTS; i++) {
        if (s_pc[i].ptr <= 1) continue;
        int k;
        for (k = 0; k < nt; k++)
            if (top[k].ra == s_pc[i].ra && !strcmp(top[k].ctx, s_pc[i].ctx)) break;
        if (k == nt) { if (nt == 512) continue; top[nt].ra = s_pc[i].ra; memcpy(top[nt].ctx, s_pc[i].ctx, 12);
                       top[nt].bytes = 0; top[nt].n = 0; nt++; }
        top[k].bytes += s_pc[i].size; top[k].n++;
    }
    uint32_t outstanding = CONKER_GMEM32(0x7FA4B4);
    fprintf(stderr, "[PHYSHEAP] %s (%s): %u blocks live, %u KB (%u KB as buddy blocks), peak %u MB; "
                    "title counters alloc %u free %u live %u\n",
            why, g_xbox_level_name, s_pc_count, (unsigned)(s_pc_live >> 10),
            (unsigned)(s_pc_live_pow2 >> 10), s_pc_peak_mb,
            CONKER_GMEM32(0x7FA494), CONKER_GMEM32(0x7FA490), outstanding);
    for (int r = 0; r < 12 && nt; r++) {
        int b = 0;
        for (int k = 1; k < nt; k++) if (top[k].bytes > top[b].bytes) b = k;
        if (!top[b].bytes) break;
        fprintf(stderr, "[PHYSHEAP]   %6u KB in %5u from %08X ctx '%s'\n",
                (unsigned)(top[b].bytes >> 10), top[b].n, top[b].ra, top[b].ctx);
        top[b].bytes = 0;
    }
    pthread_mutex_unlock(&s_pc_lock);
}

/* sub_002AF430(size, flags): the title's physical-allocation front end.
 * Remember who asked, for the heap allocation it makes. */
extern void sub_002AF430_gen(void);
extern uint32_t xbox_ContiguousAllocLastResort(uint32_t size, uint32_t alignment, uint32_t lo, uint32_t hi);
extern uint32_t xbox_ContiguousInUseBytes(void);
static unsigned s_pc_fallbacks;
void sub_002AF430(void)
{
    uint32_t save = t_pc_ra;
    uint32_t ra = CONKER_GMEM32(g_esp), size = CONKER_GMEM32(g_esp + 4), flags = CONKER_GMEM32(g_esp + 8);
    t_pc_ra = ra;
    sub_002AF430_gen();
    t_pc_ra = save;
    /* The physical heap is full: take the block from the kernel's contiguous
     * memory instead, and enter it in the title's own table of direct
     * contiguous blocks (0x7FA390, 60 slots, used-range count at 0x84A198),
     * so its free (sub_002AF670) hands it back with MmFreeContiguousMemory.
     * CONKER_PHYS_FALLBACK=0: the title's behaviour (relaunch). */
    static int fb = -1;
    if (fb < 0) { const char *e = getenv("CONKER_PHYS_FALLBACK"); fb = !(e && *e == '0'); }
    if (g_eax || !fb || !(flags & 0x80000000u) || !size) return;
    int slot = -1;
    for (int i = 0; i < 60; i++) if (!CONKER_GMEM32(0x7FA390 + i * 4)) { slot = i; break; }
    if (slot < 0) { fprintf(stderr, "[PHYSHEAP] fallback: no free slot for %u bytes\n", size); return; }
    uint32_t va = xbox_ContiguousAllocLastResort(size, 0x1000, 0, 0x03FFFFFF);
    s_pc_fallbacks++;
    fprintf(stderr, "[PHYSHEAP] heap full: %u bytes (%u KB, flags %08X, caller %08X/%08X, level %s) "
                    "-> contiguous 0x%08X slot %d (#%u; contiguous in use %u KB)\n",
            size, size / 1024, flags, ra, t_pc_ra2, g_xbox_level_name, va, slot, s_pc_fallbacks,
            xbox_ContiguousInUseBytes() / 1024);
    if (!va) return;
    memset((void *)((uintptr_t)va + g_xbox_mem_offset), 0, size);
    CONKER_GMEM32(0x7FA390 + slot * 4) = va;
    uint32_t used = 0;
    for (int i = 0; i < 60; i++) if (CONKER_GMEM32(0x7FA390 + i * 4)) used = (uint32_t)i + 1;
    CONKER_GMEM32(0x84A198) = used;
    g_eax = va;
}
/* sub_00063230: resource loader that falls back to the physical heap. */
extern void sub_00063230_gen(void);
void sub_00063230(void)
{
    uint32_t save = t_pc_ra2;
    t_pc_ra2 = CONKER_GMEM32(g_esp);
    sub_00063230_gen();
    t_pc_ra2 = save;
}
/* sub_003E6BD0(heap, size): heap allocation (every heap of the title). */
extern void sub_003E6BD0_gen(void);
void sub_003E6BD0(void)
{
    uint32_t heap = CONKER_GMEM32(g_esp + 4), size = CONKER_GMEM32(g_esp + 8);
    sub_003E6BD0_gen();
    if (!pc_on() || !g_eax || heap != CONKER_GMEM32(0x84A188)) return;
    pthread_mutex_lock(&s_pc_lock);
    pc_add(g_eax, size, t_pc_ra2 ? t_pc_ra2 : t_pc_ra);
    pthread_mutex_unlock(&s_pc_lock);
}
/* sub_003E7080(ptr): heap free (the heap is found from the pointer). */
extern void sub_003E7080_gen(void);
void sub_003E7080(void)
{
    uint32_t ptr = CONKER_GMEM32(g_esp + 4);
    sub_003E7080_gen();
    if (!pc_on()) return;
    pthread_mutex_lock(&s_pc_lock);
    pc_del(ptr);
    pthread_mutex_unlock(&s_pc_lock);
}
/* sub_001F05D0(size, what): out of physical memory -> relaunch. */
extern void sub_001F05D0_gen(void);
void sub_001F05D0(void)
{
    uint32_t sz = CONKER_GMEM32(g_esp + 4), what = CONKER_GMEM32(g_esp + 8), ra = CONKER_GMEM32(g_esp);
    char w[64] = "";
    for (int i = 0; what && i < 63 && (w[i] = (char)*(volatile uint8_t *)((uintptr_t)(what + i) + g_xbox_mem_offset)); i++) {}
    fprintf(stderr, "[PHYSHEAP] OUT OF MEMORY: %u bytes (%u KB) for '%s', caller %08X\n", sz, sz / 1024, w, ra);
    pc_report("allocation failed");
    sub_001F05D0_gen();
}
void conker_phys_level_change(void) { pc_report("level change"); }
