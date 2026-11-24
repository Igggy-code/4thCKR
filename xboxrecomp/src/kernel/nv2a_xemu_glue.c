/*
 * The NV2A as a device model: xemu's GPU (PFIFO + PGRAPH + Vulkan renderer,
 * built as libnv2a_xemu from nv2a-port) behind the 0xFD000000 aperture.
 *
 * Enabled with RECOMP_GPU=xemu. It replaces the pushbuffer executor, the
 * busy-bit ack and the kernel's vblank: the title's D3D talks to registers
 * that behave, and the frames it draws are rendered on the host GPU.
 *
 *   aperture   PROT_NONE; each load/store is decoded (mmio_decode_a64.h) and
 *              becomes nv2a_host_mmio_read/write
 *   interrupt  xemu raises its PCI line through gpu_irq(); a delivery thread
 *              runs the title's ISR (vector 3) on a worker stack, as the APU
 *              model does
 *   vblank     60 Hz thread: PCRTC vblank, then render + present the scanout
 *   RAM writes guest RAM pages the renderer has looked at are write-protected
 *              in the three views titles write through (0x00000000,
 *              0x80000000, 0xF0000000); the first write marks the page dirty
 *              for xemu's texture/surface/vertex caches and opens it again
 */
#if !defined(_WIN32) && defined(XBOXRECOMP_NV2A_XEMU)

#define _GNU_SOURCE
#include <sys/syscall.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "nv2a_host.h"
#if defined(__aarch64__)
#include "../platform/mmio_decode_a64.h"
#else
#include "../platform/mmio_decode_x86_64.h"
#endif

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern ptrdiff_t g_xbox_mem_offset;
#include "recomp_cpu_tls.h" /* was: extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi; */

#define GPU_APERTURE_VA   0xFD000000u
#define GPU_APERTURE_SIZE 0x01000000u
#define GPU_VECTOR        3u
#define RAM_SIZE          (64u << 20)
#define PAGE              4096u

int g_nv2a_xemu_active;
unsigned g_nv2a_ram_faults, g_nv2a_protect_calls;   /* [GPERF] */
/* nv2a-port: who last dirtied each page (host pc of the faulting write,
 * 1 = our own read-back landing, else the caller of the host-write hook) */
uintptr_t *g_nv2a_page_dirty_by;
uintptr_t nv2a_page_dirty_by(uint64_t addr)
{
    return g_nv2a_page_dirty_by && addr < RAM_SIZE ? g_nv2a_page_dirty_by[addr / PAGE] : 0;
}

static uint8_t *s_ram;                 /* host address of guest physical 0 */
static uint8_t *s_aperture;
/* GPU physical memory is the contiguous window: in this runtime 0x80000000+P
 * and the tiled aperture 0xF0000000+P are one section, while the 0x0 view is
 * separate storage holding the XBE image (see xbox_memory_layout.c). */
static const uint32_t s_views[] = { 0x80000000u, 0xF0000000u };
static uint8_t *s_protected;           /* one byte per page: 1 = write-protected */

/* ── RAM write tracking ───────────────────────────────────────────────── */

/* The flag and both views' protection change together under one lock:
 * without it the renderer thread could re-protect the second view after a
 * faulting guest thread had already cleared the flag and opened the page,
 * leaving a read-only page nobody claimed (a crash on the next write). */
static volatile int s_prot_lock;
static void prot_lock(void)
{
    /* spin briefly, then yield: the holder may be preempted mid-mprotect,
     * and a game thread spinning on it burnt whole milliseconds */
    for (unsigned n = 0; __atomic_exchange_n(&s_prot_lock, 1, __ATOMIC_ACQUIRE); n++)
        if (n > 200) sched_yield();
}
static void prot_unlock(void) { __atomic_store_n(&s_prot_lock, 0, __ATOMIC_RELEASE); }

static void set_views(uint32_t page, int prot)
{
    for (size_t v = 0; v < sizeof s_views / sizeof s_views[0]; v++)
        mprotect((uint8_t *)g_xbox_mem_offset + s_views[v] + (uintptr_t)page * PAGE,
                 PAGE, prot);
}

static void protect_page(uint32_t page)
{
    prot_lock();
    if (!s_protected[page]) {
        set_views(page, PROT_READ);
        __atomic_store_n(&s_protected[page], 1, __ATOMIC_RELEASE);
    }
    prot_unlock();
}

static void unprotect_page(uint32_t page)
{
    prot_lock();
    if (s_protected[page]) {
        __atomic_store_n(&s_protected[page], 0, __ATOMIC_RELEASE);
        set_views(page, PROT_READ | PROT_WRITE);
    }
    prot_unlock();
}

/* nv2a-port: the GPU model is about to write guest RAM (a surface read
 * back): open the write-protected pages of the range first, with the same
 * effect as the faults it would take (each protected page marked dirty for
 * every client), without a signal and two mprotects per page. */
static void unguard_range(uint64_t addr, uint64_t size);
void nv2a_host_prepare_write(uint64_t addr, uint64_t size)
{
    if (!s_protected || !size || addr >= RAM_SIZE) return;
    unguard_range(addr, size);   /* the read-back is landing */
    uint64_t end = addr + size > RAM_SIZE ? RAM_SIZE : addr + size;
    uint32_t pe = (uint32_t)((end + PAGE - 1) / PAGE);
    /* one mprotect per run of protected pages and view (was two syscalls
     * per page: a 256x256 read-back took 128 of them, every frame) */
    prot_lock();
    for (uint32_t p = (uint32_t)(addr / PAGE); p < pe; ) {
        if (!__atomic_load_n(&s_protected[p], __ATOMIC_ACQUIRE)) { p++; continue; }
        uint32_t q = p;
        while (q < pe && __atomic_load_n(&s_protected[q], __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s_protected[q], 0, __ATOMIC_RELEASE);
            q++;
        }
        for (size_t v = 0; v < sizeof s_views / sizeof s_views[0]; v++)
            mprotect((uint8_t *)g_xbox_mem_offset + s_views[v] + (uintptr_t)p * PAGE,
                     (size_t)(q - p) * PAGE, PROT_READ | PROT_WRITE);
        if (g_nv2a_page_dirty_by)
            for (uint32_t k = p; k < q; k++) g_nv2a_page_dirty_by[k] = 1;
        nv2a_host_mark_dirty((uint64_t)p * PAGE, (uint64_t)(q - p) * PAGE);
        p = q;
    }
    prot_unlock();
}

/* nv2a-port: read guard. A small render target the title reads back is
 * guarded (no access) from the moment its read-back is queued until the
 * read-back lands in RAM; a title access in between waits for it (the
 * renderer finishes that frame part) instead of seeing stale pixels. So
 * the GPU's semaphores can be released when parsed again: only real
 * accesses to those pixels wait for the GPU. */
static uint8_t *s_guard;
static volatile int s_pfifo_tid;
unsigned g_nv2a_guard_faults, g_nv2a_guard_forced;
void nv2a_host_note_pfifo_thread(void) { s_pfifo_tid = (int)syscall(SYS_gettid); }
static void restore_page_prot(uint32_t p)
{
    set_views(p, __atomic_load_n(&s_protected[p], __ATOMIC_ACQUIRE) ? PROT_READ
                                                                   : PROT_READ | PROT_WRITE);
}
void nv2a_host_guard(uint64_t addr, uint64_t size)
{
    if (!s_guard || !size || addr >= RAM_SIZE) return;
    uint64_t end = addr + size > RAM_SIZE ? RAM_SIZE : addr + size;
    prot_lock();
    for (uint32_t p = (uint32_t)(addr / PAGE); (uint64_t)p * PAGE < end; p++) {
        if (!s_guard[p]) { s_guard[p] = 1; set_views(p, PROT_NONE); }
    }
    prot_unlock();
}
static void unguard_range(uint64_t addr, uint64_t size)
{
    if (!s_guard || !size || addr >= RAM_SIZE) return;
    uint64_t end = addr + size > RAM_SIZE ? RAM_SIZE : addr + size;
    prot_lock();
    for (uint32_t p = (uint32_t)(addr / PAGE); (uint64_t)p * PAGE < end; p++)
        if (s_guard[p]) { s_guard[p] = 0; restore_page_prot(p); }
    prot_unlock();
}
void nv2a_host_unguard(uint64_t addr, uint64_t size) { unguard_range(addr, size); }
#define PROBE_MAX 64
static struct { uint32_t addr, size; volatile uint8_t title, own, live; uintptr_t pc, lr; } s_probe[PROBE_MAX];
/* where the last access to a probed target came from (host pc and lr) */
uintptr_t nv2a_probe_last_pc, nv2a_probe_last_lr;
static unsigned s_probe_n;
void nv2a_host_unguard_all(void)
{
    if (!s_guard) return;
    prot_lock();
    for (unsigned i = 0; i < s_probe_n; i++) s_probe[i].live = 0;
    for (uint32_t p = 0; p < RAM_SIZE / PAGE; p++)
        if (s_guard[p]) { s_guard[p] = 0; restore_page_prot(p); }
    prot_unlock();
}

/* nv2a-port: read-back probe (NV2A_RB_PROBE). A render target read back to
 * RAM every frame costs a GPU copy, and the title writing next to it costs
 * write faults; it is worth it only if something reads those pixels from
 * RAM. After a read-back lands its pages are guarded (s_guard 2); the first
 * access opens them and says who it was: a title (or other host) thread,
 * or the renderer's own thread (a texture read from RAM). The renderer
 * asks before the next read-back lands (nv2a_host_probe_take). */
int nv2a_rb_probe_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("NV2A_RB_PROBE") != NULL;
    return on;
}
void nv2a_host_probe(uint64_t addr, uint64_t size)
{
    if (!s_guard || !size || addr >= RAM_SIZE) return;
    uint64_t end = addr + size > RAM_SIZE ? RAM_SIZE : addr + size;
    prot_lock();
    int k = -1;
    for (unsigned i = 0; i < s_probe_n; i++) if (s_probe[i].addr == addr) { k = (int)i; break; }
    if (k < 0) {
        for (unsigned i = 0; i < s_probe_n; i++) if (!s_probe[i].live) { k = (int)i; break; }
        if (k < 0 && s_probe_n < PROBE_MAX) k = (int)s_probe_n++;
    }
    if (k >= 0) {
        s_probe[k].addr = (uint32_t)addr; s_probe[k].size = (uint32_t)(end - addr);
        s_probe[k].title = s_probe[k].own = 0; s_probe[k].live = 1;
        for (uint32_t p = (uint32_t)(addr / PAGE); (uint64_t)p * PAGE < end; p++)
            if (!s_guard[p]) { s_guard[p] = 2; set_views(p, PROT_NONE); }
    }
    prot_unlock();
}
/* who touched it since nv2a_host_probe: bit 0 title/host, bit 1 renderer;
 * -1 when it was not probed */
int nv2a_host_probe_take(uint64_t addr)
{
    int r = -1;
    prot_lock();
    for (unsigned i = 0; i < s_probe_n; i++) {
        if (!s_probe[i].live || s_probe[i].addr != addr) continue;
        r = (s_probe[i].title ? 1 : 0) | (s_probe[i].own ? 2 : 0);
        s_probe[i].live = 0;
        break;
    }
    prot_unlock();
    return r;
}
/* the same without ending the probe */
int nv2a_host_probe_peek(uint64_t addr)
{
    int r = -1;
    prot_lock();
    for (unsigned i = 0; i < s_probe_n; i++) {
        if (!s_probe[i].live || s_probe[i].addr != addr) continue;
        r = (s_probe[i].title ? 1 : 0) | (s_probe[i].own ? 2 : 0);
        break;
    }
    prot_unlock();
    return r;
}
/* fault on a probed page: note who, open the probed ranges on that page */
static void probe_touch(uint32_t page, uint32_t phys, int own, void *ucv)
{
    uintptr_t pc = 0, lr = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc;
    lr = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.regs[30];
#else
    (void)ucv;
#endif
    prot_lock();
    uint64_t a = (uint64_t)page * PAGE, b = a + PAGE;
    for (unsigned i = 0; i < s_probe_n; i++) {
        if (!s_probe[i].live) continue;
        uint64_t pa = s_probe[i].addr, pb = pa + s_probe[i].size;
        /* the accessed byte itself: a page shared with a neighbour (targets
         * are not page aligned) is no access to this target */
        if (phys < pa || phys >= pb) continue;
        (void)a; (void)b;
        if (!s_probe[i].own && !s_probe[i].title) {   /* the first access: who */
            nv2a_probe_last_pc = pc; nv2a_probe_last_lr = lr;
        }
        if (own) s_probe[i].own = 1; else s_probe[i].title = 1;
    }
    if (s_guard[page] == 2) { s_guard[page] = 0; restore_page_prot(page); }
    prot_unlock();
}

/* xemu: "no client still sees these pages dirty, watch them again". */
static void gpu_protect(uint64_t addr, uint64_t size)
{
    if (addr >= RAM_SIZE) return;
    uint64_t end = addr + size > RAM_SIZE ? RAM_SIZE : addr + size;
    uint32_t pe = (uint32_t)((end + PAGE - 1) / PAGE);
    /* nv2a-port: one mprotect per run of open pages and view, under one
     * lock hold, instead of two syscalls per page. */
    prot_lock();
    for (uint32_t p = (uint32_t)(addr / PAGE); p < pe; ) {
        if (s_protected[p]) { p++; continue; }
        uint32_t q = p;
        while (q < pe && !s_protected[q]) q++;
        for (size_t v = 0; v < sizeof s_views / sizeof s_views[0]; v++)
            mprotect((uint8_t *)g_xbox_mem_offset + s_views[v] + (uintptr_t)p * PAGE,
                     (size_t)(q - p) * PAGE, PROT_READ);
        g_nv2a_protect_calls++;
        for (uint32_t k = p; k < q; k++)
            __atomic_store_n(&s_protected[k], 1, __ATOMIC_RELEASE);
        p = q;
    }
    prot_unlock();
}

/* The host is about to write guest RAM itself (a file read into a guest
 * buffer): read() into a write-protected page fails with EFAULT instead of
 * faulting, so open the range and mark it dirty first. */
void xbox_GuestRamPrepareWrite(uint32_t va, uint32_t len)
{
    if (!g_nv2a_xemu_active || !len) return;
    uint32_t phys = va & (RAM_SIZE - 1);
    if (!((va >= 0x80000000u && va < 0x80000000u + RAM_SIZE) ||
          (va >= 0xF0000000u && va < 0xF0000000u + RAM_SIZE)))
        return;                                      /* not GPU-visible memory */
    uint64_t end = (uint64_t)phys + len;
    if (end > RAM_SIZE) end = RAM_SIZE;
    unguard_range(phys, end - phys);   /* a guarded page would fail the read() */
    /* one lock, and one mprotect per run of protected pages and view (was
     * a lock and an mprotect per page and view: a 256 KB audio-stream read
     * took 52 ms here, a visible hitch) */
    {
        uint32_t pe = (uint32_t)((end + PAGE - 1) / PAGE);
        prot_lock();
        for (uint32_t p = (uint32_t)(phys / PAGE); p < pe; ) {
            if (!__atomic_load_n(&s_protected[p], __ATOMIC_ACQUIRE)) { p++; continue; }
            uint32_t q = p;
            while (q < pe && __atomic_load_n(&s_protected[q], __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&s_protected[q], 0, __ATOMIC_RELEASE);
                q++;
            }
            for (size_t v = 0; v < sizeof s_views / sizeof s_views[0]; v++)
                mprotect((uint8_t *)g_xbox_mem_offset + s_views[v] + (uintptr_t)p * PAGE,
                         (size_t)(q - p) * PAGE, PROT_READ | PROT_WRITE);
            p = q;
        }
        prot_unlock();
    }
    if (g_nv2a_page_dirty_by)
        for (uint64_t p = phys / PAGE; p < (end + PAGE - 1) / PAGE; p++)
            g_nv2a_page_dirty_by[p] = (uintptr_t)__builtin_return_address(0);
    nv2a_host_mark_dirty(phys, end - phys);
}

/* ── Interrupt delivery ───────────────────────────────────────────────── */

static pthread_mutex_t s_irq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_irq_cond = PTHREAD_COND_INITIALIZER;
static volatile int s_irq_level;
static uint64_t s_irqs_delivered, s_irqs_declined, s_inline_isrs;

/* raise-to-ISR-done latency, CONKER_PROF/NV2A_PERF: [GPUIRQ] every 5 s */
static double s_irq_raise_ms, s_lat_sum, s_lat_max, s_lat_t0;
static unsigned s_lat_n, s_lat_holds;
static double irq_now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void gpu_irq(int level)
{
    pthread_mutex_lock(&s_irq_lock);
    if (level && !s_irq_level) {
        extern void nv2a_tl(int, uint32_t);
        s_irq_raise_ms = irq_now_ms(); nv2a_tl(10, 0);
    }
    s_irq_level = level;
    if (level) pthread_cond_signal(&s_irq_cond);
    pthread_mutex_unlock(&s_irq_lock);
}

static int gpu_call_isr(void)
{
    uint32_t kint = xbox_GetConnectedInterrupt(GPU_VECTOR);
    if (!kint) return -1;
    uint32_t routine = *(uint32_t *)((uintptr_t)g_xbox_mem_offset + kint + 0);
    uint32_t context = *(uint32_t *)((uintptr_t)g_xbox_mem_offset + kint + 4);
    recomp_func_t fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn) return -1;
    g_esp -= 4; *(uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) = context;
    g_esp -= 4; *(uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp) = 0xDEADBEEFu;
    extern void nv2a_tl(int, uint32_t);
    nv2a_tl(11, 0);
    { int irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(irql); }
    nv2a_tl(12, g_eax);
    return (int)(g_eax & 1u);
}

/* One guest ISR at a time, whichever thread runs it. */
static pthread_mutex_t s_isr_lock = PTHREAD_MUTEX_INITIALIZER;
/* This thread is in the ISR or its DPCs: a posted interrupt run at one of
 * the DPC's safe points must not take the lock again (it would wait on
 * itself); it is left posted, and the line, still up, is taken after. */
static __thread int t_isr_locked;
static void isr_lock(void)   { pthread_mutex_lock(&s_isr_lock); t_isr_locked = 1; }
static int  isr_trylock(void){ if (pthread_mutex_trylock(&s_isr_lock)) return 0; t_isr_locked = 1; return 1; }
static void isr_unlock(void) { t_isr_locked = 0; pthread_mutex_unlock(&s_isr_lock); }

static void gpu_isr_on_holder(void)
{
    if (t_isr_locked) return;
    isr_lock();
    if (s_irq_level) gpu_call_isr();
    isr_unlock();
}

/* The PFIFO thread's own delivery of the interrupt it just raised (a
 * NOP-notify trap), called with no nv2a lock held. Returns -1 when it did
 * not run the ISR (no handler yet, the one-CPU gate held, no worker stack),
 * and the delivery thread takes the interrupt as usual. */
int nv2a_glue_inline_isr(void)
{
    static __thread int tib_done;
    if (!tib_done) {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) return -1;
        g_fs_base = tib; tib_done = 1;
    }
    if (!xbox_GetConnectedInterrupt(GPU_VECTOR)) return -1;
    if (t_isr_locked) return -1;
    if (!isr_trylock()) {
        /* the delivery thread is in the ISR already: let it finish */
        isr_lock();
        isr_unlock();
        return 0;
    }
    if (!s_irq_level) { isr_unlock(); return 0; }
    if (!xbox_DispatchGateTryEnter()) { isr_unlock(); return -1; }
    int slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        if (xbox_DispatchGateOn()) xbox_DispatchGateLeave();
        isr_unlock();
        return -1;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    int claimed = gpu_call_isr();
    if (xbox_DispatchGateOn()) xbox_DispatchGateLeave();
    int ran;
    xbox_DpcDrainTimer(&ran);
    xbox_worker_stack_free(slot);
    isr_unlock();
    s_inline_isrs++;
    return claimed;
}

static void *gpu_irq_thread(void *arg)
{
    (void)arg;
    uint32_t tib = xbox_AllocThreadTib();
    if (tib) g_fs_base = tib;
    unsigned held_off = 0;
    for (;;) {
        /* nv2a-port: spin a little before sleeping: interrupts come in
         * bursts (a NOP-notify trap after another), and each condvar sleep
         * put a wakeup into the GPU's round trip. */
        {
            static int spin_us = -1;
            if (spin_us < 0) { const char *e = getenv("RECOMP_IRQ_SPIN_US"); spin_us = e ? atoi(e) : 500; }
            if (spin_us > 0 && !s_irq_level) {
                double t0 = irq_now_ms();
                for (int i = 0; !s_irq_level; i++) {
#if defined(__aarch64__)
                    __asm__ volatile("yield");
#endif
                    if ((i & 63) == 63 && irq_now_ms() - t0 > spin_us / 1000.0) break;
                }
            }
        }
        pthread_mutex_lock(&s_irq_lock);
        while (!s_irq_level) pthread_cond_wait(&s_irq_cond, &s_irq_lock);
        pthread_mutex_unlock(&s_irq_lock);

        if (!xbox_GetConnectedInterrupt(GPU_VECTOR)) { usleep(1000); continue; }

        int how;
        xbox_IrqSetHandler(XBOX_IRQ_VBLANK, gpu_isr_on_holder);
        if (xbox_IrqSafePointsOn()) {
            how = xbox_IrqPost(XBOX_IRQ_VBLANK);
            if (how == XBOX_IRQ_POSTED) { s_lat_holds++; usleep(200); continue; }
        } else if (!xbox_DispatchGateOn()) {
            /* No gate: hold off briefly while a guest thread sits at raised
             * IRQL, as the console could not take the interrupt then. */
            /* nv2a-port: some guest thread is nearly always found at
             * raised IRQL (one parked in a wait at DISPATCH), so every
             * interrupt sat out the full 50 x 100 us -- 10-12 ms on Android,
             * where usleep(100) sleeps twice that -- and the GPU, stalled
             * on the NOP notify the ISR acknowledges, idled 20 ms a frame.
             * A short hold-off, then deliver, as before. RECOMP_IRQ_HOLDOFF
             * sets the count (50 is the old behaviour). */
            static int holdoff = -1;
            if (holdoff < 0) {
                const char *e = getenv("RECOMP_IRQ_HOLDOFF");
                holdoff = e ? atoi(e) : 3;
            }
            if (xbox_IrqlBlocksInterrupts() && ++held_off <= (unsigned)holdoff) {
                s_lat_holds++; usleep(30); continue;
            }
            held_off = 0;
            how = XBOX_IRQ_GATED;
        } else {
            how = xbox_DispatchGateTryEnter() ? XBOX_IRQ_GATED : XBOX_IRQ_UNGATED;
            if (how == XBOX_IRQ_UNGATED && ++held_off <= 50) { s_lat_holds++; usleep(100); continue; }
            held_off = 0;
        }
        isr_lock();
        if (!s_irq_level) {
            /* the PFIFO thread took it (nv2a_glue_inline_isr) */
            isr_unlock();
            if (how == XBOX_IRQ_GATED && xbox_DispatchGateOn())
                xbox_DispatchGateLeave();
            continue;
        }
        int slot = xbox_worker_stack_alloc();
        int claimed = -1;
        if (slot >= 0) {
            g_esp = XBOX_WORKER_STACK_TOP(slot);
            g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
            claimed = gpu_call_isr();
            if (how == XBOX_IRQ_GATED && xbox_DispatchGateOn())
                xbox_DispatchGateLeave();
            how = -1;
            /* D3D's ISR does the real work in a DPC (it signals the event a
             * thread blocked in BlockOnTime waits on). The guest thread is
             * asleep and never lowers IRQL, so run the queue here, on this
             * worker stack, as the processor would after the interrupt. */
            int ran;
            xbox_DpcDrainTimer(&ran);
            xbox_worker_stack_free(slot);
        }
        isr_unlock();
        if (how == XBOX_IRQ_GATED && xbox_DispatchGateOn())
            xbox_DispatchGateLeave();
        {
            double now = irq_now_ms(), lat = now - s_irq_raise_ms;
            if (s_irq_raise_ms > 0 && lat < 1000) {
                s_lat_sum += lat; s_lat_n++; if (lat > s_lat_max) s_lat_max = lat;
            }
            static int on = -1;
            if (on < 0) on = getenv("CONKER_PROF") || getenv("NV2A_PERF");
            if (!s_lat_t0) s_lat_t0 = now;
            if (on && now - s_lat_t0 > 5000) {
                static uint64_t last_inl;
                fprintf(stderr, "[GPUIRQ] %u irqs in 5 s, latency avg %.3f ms max %.2f ms, holds %u, inline %llu\n",
                        s_lat_n, s_lat_n ? s_lat_sum / s_lat_n : 0, s_lat_max, s_lat_holds,
                        (unsigned long long)(s_inline_isrs - last_inl));
                last_inl = s_inline_isrs;
                s_lat_sum = s_lat_max = 0; s_lat_n = s_lat_holds = 0; s_lat_t0 = now;
            }
        }
        if (claimed > 0) s_irqs_delivered++;
        else { s_irqs_declined++; usleep(500); }   /* line stuck: don't spin */
        if ((s_irqs_delivered + s_irqs_declined) % 3000 == 1)
            fprintf(stderr, "[GPU] interrupts: %llu delivered, %llu declined\n",
                    (unsigned long long)s_irqs_delivered,
                    (unsigned long long)s_irqs_declined);
    }
    return NULL;
}

/* ── Vblank / present ─────────────────────────────────────────────────── */

/* nv2a-port: a late frame does not wait for the next vblank of the grid.
 * The title (D3D, interval TWO) flips on a vblank; a frame ready 35 ms after
 * the last flip waited until 50 ms, so 25 fps of work came out as a jerky
 * 20. When the pfifo thread reaches a FLIP_STALL more than 33.3 ms after the
 * last flip, the vblank thread fires the next vblank at once instead of at
 * its slot, and the one after keeps its own slot: still 60 vblanks a second,
 * the title's clock does not change, only the wait goes. A frame on time is
 * paced as before. CONKER_NO_VBL_PULL=1: off. */
static pthread_mutex_t s_vbl_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_vbl_cv;
static volatile int s_vbl_pull;
static uint64_t s_vbl_pulls;
static uint64_t s_flip_done_ns;

static uint64_t vbl_now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* pfifo thread: the title's flip stall just completed */
void xbox_FlipDone(void) { __atomic_store_n(&s_flip_done_ns, vbl_now_ns(), __ATOMIC_RELEASE); }

/* pfifo thread: the title asks to flip (FLIP_STALL) */
/* nv2a-port: frame-rate mode, 30 (the title's own), 40 or 60. Settings set
 * CONKER_FPS at start and change it live (nativeSetFps). 40 runs the
 * emulated vblank at 120 Hz and flips every third one, so frames are evenly
 * 25 ms apart (even on 120 Hz screens); 60 flips on every 60 Hz vblank. */
volatile int g_conker_fps = 0;
int xbox_FpsMode(void)
{
    int m = __atomic_load_n(&g_conker_fps, __ATOMIC_RELAXED);
    if (!m) {
        const char *e = getenv("CONKER_FPS");
        /* the autotest switches (CONKER_FPS60/40) win over the settings value */
        m = getenv("CONKER_FPS60") ? 60 : getenv("CONKER_FPS40") ? 40 : e ? atoi(e) : 30;
        if (m != 40 && m != 60) m = 30;
        __atomic_store_n(&g_conker_fps, m, __ATOMIC_RELAXED);
        fprintf(stderr, "[FPS] mode %d\n", m);
    }
    return m;
}
void xbox_SetFpsMode(int m)
{
    if (m != 40 && m != 60) m = 30;
    __atomic_store_n(&g_conker_fps, m, __ATOMIC_RELAXED);
    fprintf(stderr, "[FPS] mode %d (settings)\n", m);
}

void xbox_FlipStallBegin(void)
{
    static int off = -1;
    if (off < 0) off = getenv("CONKER_NO_VBL_PULL") != NULL;
    if (off) return;
    uint64_t done = __atomic_load_n(&s_flip_done_ns, __ATOMIC_ACQUIRE);
    if (!done) return;
    uint64_t since = vbl_now_ns() - done;
    /* late for 30 fps, and not an idle screen (a stall seconds later) */
    /* 60 fps mode: late for 60 means past one vblank, not two */
    int fm = xbox_FpsMode();
    uint64_t late = fm == 60 ? 16800000ull : fm == 40 ? 25800000ull : 33600000ull;
    if (since < late || since > 250000000ull) return;
    pthread_mutex_lock(&s_vbl_mx);
    s_vbl_pull = 1;
    pthread_cond_signal(&s_vbl_cv);
    pthread_mutex_unlock(&s_vbl_mx);
}

static void *gpu_vblank_thread(void *arg)
{
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    /* NV2A_UNCAP (autotest flag uncap): vblanks at 240 Hz, so the title's
     * 30 fps cap (a flip every 2 vblanks) becomes 120: the frame rate then
     * shows how much the port has to spare. Benchmark only: the game's own
     * clock may run fast. */
    const int uncap = getenv("NV2A_UNCAP") != NULL;
    if (getenv("NV2A_UNCAP")) fprintf(stderr, "[VBL] uncapped benchmark mode: vblank at 240 Hz\n");
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s_vbl_cv, &ca);
    uint64_t last_report = vbl_now_ns(), pulls0 = 0;
    for (;;) {
        long period = uncap ? 4166667L : xbox_FpsMode() == 40 ? 8333333L : 16666667L;
        next.tv_nsec += period;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        pthread_mutex_lock(&s_vbl_mx);
        for (;;) {
            if (s_vbl_pull) { s_vbl_pull = 0; s_vbl_pulls++; break; }   /* this slot, now */
            int rc = pthread_cond_timedwait(&s_vbl_cv, &s_vbl_mx, &next);
            if (rc == ETIMEDOUT) { s_vbl_pull = 0; break; }
        }
        pthread_mutex_unlock(&s_vbl_mx);
        nv2a_host_vblank();
        uint64_t n_ns = vbl_now_ns();
        if (n_ns - last_report > 10000000000ull) {
            fprintf(stderr, "[VBL] late frames shown without waiting for the vblank: %.1f/s\n",
                    (s_vbl_pulls - pulls0) / ((n_ns - last_report) / 1e9));
            pulls0 = s_vbl_pulls; last_report = n_ns;
        }
        static int dbg = -1, n;
        if (dbg < 0) dbg = getenv("NV2A_DEBUG") != NULL;
        if (dbg && (++n % 120) == 0) nv2a_host_debug_dump();
    }
    return NULL;
}

static void *gpu_present_thread(void *arg)
{
    (void)arg;
    for (;;) {
        nv2a_host_present();      /* blocks until the PFIFO thread drew it */
        usleep(15000);
    }
    return NULL;
}

/* ── Display mode (AvSetDisplayMode) ──────────────────────────────────── */

static void crtc_write(uint8_t index, uint8_t val)
{
    nv2a_host_mmio_write(0x6013D4, index, 1);
    nv2a_host_mmio_write(0x6013D5, val, 1);
}

void xbox_Nv2aXemuSetDisplayMode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t fb)
{
    if (!g_nv2a_xemu_active) return;
    (void)mode;
    unsigned bpp = 4, depth = 3;                       /* X8R8G8B8 / A8R8G8B8 */
    if (format == 0x11 || format == 0x10 || format == 0x28) { bpp = 2; depth = 2; }
    unsigned w = pitch ? pitch / bpp : 640;
    unsigned h = w == 1280 ? 720 : w == 1920 ? 1080 : 480;
    if (w < 320 || w > 1920) { w = 640; h = 480; }
    unsigned off = pitch / 8, hm1 = h - 1;

    crtc_write(0x1F, 0x57);                            /* unlock extended CRTC */
    crtc_write(0x01, (uint8_t)(w / 8 - 1));
    crtc_write(0x12, (uint8_t)(hm1 & 0xFF));
    crtc_write(0x07, (uint8_t)(((hm1 >> 8) & 1) << 1 | ((hm1 >> 9) & 1) << 6));
    crtc_write(0x13, (uint8_t)(off & 0xFF));
    crtc_write(0x19, (uint8_t)(((off >> 8) & 7) << 5));
    crtc_write(0x25, (uint8_t)(((off >> 11) & 1) << 5 | ((hm1 >> 10) & 1) << 1));
    crtc_write(0x28, (uint8_t)depth);
    if (depth == 2 && format == 0x11)                  /* R5G6B5: ALT_MODE_SEL */
        nv2a_host_mmio_write(0x680600, 0x00101000u, 4);
    nv2a_host_mmio_write(0x600800, fb & (RAM_SIZE - 1), 4);  /* PCRTC_START */
    fprintf(stderr, "[GPU] display mode %ux%u, %u bpp, pitch %u, scanout 0x%08X\n",
            w, h, bpp * 8, pitch, fb);
}

/* ── Faults ───────────────────────────────────────────────────────────── */

static uint64_t ap_rd(void *dev, uint32_t off, int size) { (void)dev; return nv2a_host_mmio_read(off, (unsigned)size); }
static void ap_wr(void *dev, uint32_t off, uint64_t v, int size) { (void)dev; nv2a_host_mmio_write(off, v, (unsigned)size); }

int xbox_Nv2aXemuFault(void *ucv, uintptr_t fault)
{
    if (!g_nv2a_xemu_active) return 0;
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    if (fault < base || fault - base >= 0x100000000ull) return 0;
    uint32_t va = (uint32_t)(fault - base);

    if (va >= GPU_APERTURE_VA && va < GPU_APERTURE_VA + GPU_APERTURE_SIZE) {
#if defined(__aarch64__)
        return mmio_emulate_a64((ucontext_t *)ucv, va - GPU_APERTURE_VA, s_aperture, ap_rd, ap_wr);
#else
        return mmio_emulate_x86_64((ucontext_t *)ucv, va - GPU_APERTURE_VA, s_aperture, ap_rd, ap_wr);
#endif
    }

    /* A write to a page the renderer is watching. */
    for (size_t v = 0; v < sizeof s_views / sizeof s_views[0]; v++) {
        if (va >= s_views[v] && va < s_views[v] + RAM_SIZE) {
            uint32_t page = (va - s_views[v]) / PAGE;
            if (s_guard && __atomic_load_n(&s_guard[page], __ATOMIC_ACQUIRE) == 2) {
                probe_touch(page, va - s_views[v], (int)syscall(SYS_gettid) == s_pfifo_tid, ucv);
                return 1;   /* retry the access */
            }
            if (s_guard && __atomic_load_n(&s_guard[page], __ATOMIC_ACQUIRE)) {
                /* a guarded render target: wait for its read-back (not on
                 * the renderer's own thread, which would wait for itself) */
                extern int nv2a_host_resolve_guard(uint64_t addr);
                __atomic_fetch_add(&g_nv2a_guard_faults, 1, __ATOMIC_RELAXED);
                if ((int)syscall(SYS_gettid) == s_pfifo_tid ||
                    !nv2a_host_resolve_guard((uint64_t)page * PAGE)) {
                    __atomic_fetch_add(&g_nv2a_guard_forced, 1, __ATOMIC_RELAXED);
                    unguard_range((uint64_t)page * PAGE, PAGE);
                }
                if (__atomic_load_n(&s_guard[page], __ATOMIC_ACQUIRE))
                    unguard_range((uint64_t)page * PAGE, PAGE);
                return 1;   /* retry the access */
            }
            if (!__atomic_load_n(&s_protected[page], __ATOMIC_ACQUIRE)) {
                /* Guest RAM is otherwise always writable, so a write fault
                 * here is a protection we lost track of: open it again. */
                prot_lock();
                if (!s_protected[page]) set_views(page, PROT_READ | PROT_WRITE);
                prot_unlock();
                return 1;
            }
            unprotect_page(page);
            if (g_nv2a_page_dirty_by) {
#if defined(__aarch64__)
                g_nv2a_page_dirty_by[page] = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc;
#else
                g_nv2a_page_dirty_by[page] = 2;
#endif
            }
            nv2a_host_mark_dirty((uint64_t)page * PAGE, PAGE);
            __atomic_fetch_add(&g_nv2a_ram_faults, 1, __ATOMIC_RELAXED);
            return 1;
        }
    }
    return 0;
}

/* ── Bring-up ─────────────────────────────────────────────────────────── */

int xbox_Nv2aXemuInit(const char *cache_dir, int null_renderer)
{
    if (g_nv2a_xemu_active) return 1;
    s_ram = (uint8_t *)g_xbox_mem_offset + 0x80000000u;
    s_aperture = (uint8_t *)g_xbox_mem_offset + GPU_APERTURE_VA;
    s_protected = calloc(RAM_SIZE / PAGE, 1);
    s_guard = calloc(RAM_SIZE / PAGE, 1);
    g_nv2a_page_dirty_by = calloc(RAM_SIZE / PAGE, sizeof(uintptr_t));

    /* Claim the aperture before trapping it: the legacy ack thread polls
     * NV2A registers from a host thread until it sees this flag, and a poll
     * landing after the mprotect faulted outside any guest context (boot
     * crash at 0xFD100410). Give a pass already under way time to finish. */
    __atomic_store_n(&g_nv2a_xemu_active, 1, __ATOMIC_SEQ_CST);
    usleep(5000);
    if (mprotect(s_aperture, GPU_APERTURE_SIZE, PROT_NONE) != 0) {
        fprintf(stderr, "[GPU] cannot trap the NV2A aperture; xemu GPU off\n");
        g_nv2a_xemu_active = 0;
        return 0;
    }

    NV2AHostConfig cfg = {
        .ram = s_ram,
        .ram_size = RAM_SIZE,
        .irq = gpu_irq,
        .protect = gpu_protect,
        .cache_dir = cache_dir,
        .use_null_renderer = null_renderer,
        .surface_scale = getenv("RECOMP_GPU_SCALE") ? atoi(getenv("RECOMP_GPU_SCALE")) : 1,
        .validation = getenv("RECOMP_GPU_VALIDATION") != NULL,
    };
    g_nv2a_xemu_active = 1;   /* before init: xemu touches nothing guest-side yet */
    if (!nv2a_host_init(&cfg)) {
        g_nv2a_xemu_active = 0;
        mprotect(s_aperture, GPU_APERTURE_SIZE, PROT_READ | PROT_WRITE);
        return 0;
    }

    pthread_t t;
    pthread_create(&t, NULL, gpu_irq_thread, NULL);    pthread_detach(t);
    pthread_create(&t, NULL, gpu_vblank_thread, NULL); pthread_detach(t);
    pthread_create(&t, NULL, gpu_present_thread, NULL); pthread_detach(t);
    fprintf(stderr, "[GPU] xemu NV2A owns the aperture at 0x%08X\n", GPU_APERTURE_VA);
    return 1;
}

#endif /* !_WIN32 && XBOXRECOMP_NV2A_XEMU */
