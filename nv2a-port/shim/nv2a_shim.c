/*
 * nv2a-port: the parts of QEMU and GLib that xemu's NV2A needs, for running
 * the GPU model inside a statically recompiled title (xboxrecomp runtime).
 *
 *  - GLib: memory, GString, GArray, GByteArray, printf helpers
 *  - QEMU threads: mutex, cond, event, thread, spin
 *  - clocks and timers (PTIMER alarm) on one timer thread
 *  - guest RAM dirty tracking per client (VGA, NV2A, NV2A_TEX), fed by
 *    nv2a_shim_mark_dirty() from the host's write-protect fault handler
 *  - xemu settings / notifications
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "xemu-config.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include "nv2a_shim.h"

#include <sys/stat.h>
#include <time.h>

/* ── GLib ─────────────────────────────────────────────────────────────── */

void *g_malloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
void *g_malloc0(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) abort(); return p; }
void *g_realloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
void g_free(void *p) { free(p); }
char *g_strdup(const char *s) { return s ? strdup(s) : NULL; }
char *g_strdup_vprintf(const char *fmt, va_list ap)
{
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0) abort();
    return out;
}
char *g_strdup_printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return s;
}

static void gs_reserve(GString *s, size_t extra)
{
    size_t need = s->len + extra + 1;
    if (need <= s->allocated_len) return;
    size_t cap = s->allocated_len ? s->allocated_len : 64;
    while (cap < need) cap *= 2;
    s->str = g_realloc(s->str, cap);
    s->allocated_len = cap;
}
GString *g_string_sized_new(size_t n)
{
    GString *s = g_malloc0(sizeof *s);
    s->allocated_len = n + 1;
    s->str = g_malloc(s->allocated_len);
    s->str[0] = 0;
    return s;
}
GString *g_string_new(const char *init)
{
    GString *s = g_string_sized_new(init ? strlen(init) : 16);
    if (init) g_string_append(s, init);
    return s;
}
GString *g_string_new_take(char *init)
{
    if (!init) return g_string_new(NULL);
    GString *s = g_malloc0(sizeof *s);
    s->str = init;
    s->len = strlen(init);
    s->allocated_len = s->len + 1;
    return s;
}
char *g_string_free(GString *s, bool free_segment)
{
    if (!s) return NULL;
    char *r = s->str;
    if (free_segment) { free(r); r = NULL; }
    free(s);
    return r;
}
GString *g_string_append_len(GString *s, const char *val, ssize_t len)
{
    if (len < 0) len = (ssize_t)strlen(val);
    gs_reserve(s, (size_t)len);
    memcpy(s->str + s->len, val, (size_t)len);
    s->len += (size_t)len;
    s->str[s->len] = 0;
    return s;
}
GString *g_string_append(GString *s, const char *val) { return g_string_append_len(s, val, -1); }
GString *g_string_append_c(GString *s, char c) { return g_string_append_len(s, &c, 1); }
void g_string_append_vprintf(GString *s, const char *fmt, va_list ap)
{
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;
    gs_reserve(s, (size_t)n);
    vsnprintf(s->str + s->len, (size_t)n + 1, fmt, ap);
    s->len += (size_t)n;
}
void g_string_append_printf(GString *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_string_append_vprintf(s, fmt, ap);
    va_end(ap);
}
void g_string_printf(GString *s, const char *fmt, ...)
{
    s->len = 0; if (s->str) s->str[0] = 0;
    va_list ap; va_start(ap, fmt);
    g_string_append_vprintf(s, fmt, ap);
    va_end(ap);
}
GString *g_string_truncate(GString *s, size_t len)
{
    if (len < s->len) { s->len = len; s->str[len] = 0; }
    return s;
}

static void ga_reserve(GArray *a, unsigned int n)
{
    unsigned int need = n + (a->zero_term ? 1 : 0);
    if (need <= a->cap) return;
    unsigned int cap = a->cap ? a->cap : 16;
    while (cap < need) cap *= 2;
    a->data = g_realloc(a->data, (size_t)cap * a->elt);
    if (a->clear || a->zero_term)
        memset(a->data + (size_t)a->cap * a->elt, 0, (size_t)(cap - a->cap) * a->elt);
    a->cap = cap;
}
GArray *g_array_sized_new(bool zero_terminated, bool clear, unsigned int elt_size, unsigned int reserved)
{
    GArray *a = g_malloc0(sizeof *a);
    a->elt = elt_size; a->zero_term = zero_terminated; a->clear = clear;
    ga_reserve(a, reserved);
    return a;
}
GArray *g_array_new(bool zero_terminated, bool clear, unsigned int elt_size)
{ return g_array_sized_new(zero_terminated, clear, elt_size, 0); }
GArray *g_array_append_vals(GArray *a, const void *data, unsigned int len)
{
    ga_reserve(a, a->len + len);
    memcpy(a->data + (size_t)a->len * a->elt, data, (size_t)len * a->elt);
    a->len += len;
    if (a->zero_term) memset(a->data + (size_t)a->len * a->elt, 0, a->elt);
    return a;
}
GArray *g_array_set_size(GArray *a, unsigned int len)
{
    ga_reserve(a, len);
    if (len > a->len && (a->clear || a->zero_term))
        memset(a->data + (size_t)a->len * a->elt, 0, (size_t)(len - a->len) * a->elt);
    a->len = len;
    return a;
}
char *g_array_free(GArray *a, bool free_segment)
{
    if (!a) return NULL;
    char *d = a->data;
    if (free_segment) { free(d); d = NULL; }
    free(a);
    return d;
}

GByteArray *g_byte_array_new(void) { return g_malloc0(sizeof(GByteArray)); }
GByteArray *g_byte_array_new_take(uint8_t *data, size_t len)
{
    GByteArray *a = g_byte_array_new();
    a->data = data; a->len = (unsigned int)len;
    return a;
}
uint8_t *g_byte_array_free(GByteArray *a, bool free_segment)
{
    if (!a) return NULL;
    uint8_t *d = a->data;
    if (free_segment) { free(d); d = NULL; }
    free(a);
    return d;
}
void g_byte_array_unref(GByteArray *a) { g_byte_array_free(a, true); }

/* ── Threads ──────────────────────────────────────────────────────────── */

/* nv2a-port: plain (fast-path) mutexes unless NV2A_MUTEX_CHECK=1 asks for
 * the error-checking kind, whose lock and unlock always take bionic's slow
 * path. Contended locks are counted by call site for the [GPERF] report. */
void qemu_mutex_init(QemuMutex *m)
{
    static int check = -1;
    if (check < 0) check = getenv("NV2A_MUTEX_CHECK") != NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, check ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
}
void qemu_mutex_destroy(QemuMutex *m) { pthread_mutex_destroy(&m->m); }

typedef struct { void *site; uint32_t n; uint64_t ns; } MutexContend;
static MutexContend s_contend[32];
static void contend_note(void *site, uint64_t ns)
{
    unsigned h = (unsigned)(((uintptr_t)site >> 2) * 2654435761u) >> 27;
    for (int i = 0; i < 32; i++) {
        MutexContend *c = &s_contend[(h + i) & 31];
        void *cur = __atomic_load_n(&c->site, __ATOMIC_RELAXED);
        if (cur == site || (!cur && __atomic_compare_exchange_n(&c->site, &cur, site, 0,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) || cur == site) {
            __atomic_fetch_add(&c->n, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&c->ns, ns, __ATOMIC_RELAXED);
            return;
        }
    }
}
/* Prints and clears the contended-lock table (called from the GPERF report). */
void nv2a_mutex_contend_report(double frames)
{
    for (int i = 0; i < 32; i++) {
        MutexContend *c = &s_contend[i];
        if (!c->site || !c->n) continue;
        Dl_info di; const char *sym = "?"; uintptr_t off = 0;
        if (dladdr(c->site, &di) && di.dli_sname) { sym = di.dli_sname; off = (uintptr_t)c->site - (uintptr_t)di.dli_saddr; }
        fprintf(stderr, "[GPERF]   lock contended from %s+0x%lx: %.1f/frame, %.2f ms/frame\n",
                sym, (unsigned long)off, c->n / frames, c->ns / 1e6 / frames);
        c->n = 0; c->ns = 0;
    }
}
/* nv2a-port: lock calls per call site (NV2A_LOCKCOUNT=1), in the GPERF report */
typedef struct { void *site; void *m; uint64_t n; } LockCount;
static LockCount s_lockcnt[64];
static int s_lockcnt_on = -1;
static void lockcount_note(void *site, void *m)
{
    unsigned h = (unsigned)(((uintptr_t)site >> 2) * 2654435761u) >> 26;
    for (int i = 0; i < 64; i++) {
        LockCount *c = &s_lockcnt[(h + i) & 63];
        void *cur = __atomic_load_n(&c->site, __ATOMIC_RELAXED);
        if (cur == site) { __atomic_fetch_add(&c->n, 1, __ATOMIC_RELAXED); return; }
        if (!cur) {
            void *exp = NULL;
            if (__atomic_compare_exchange_n(&c->site, &exp, site, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) || exp == site) {
                c->m = m; __atomic_fetch_add(&c->n, 1, __ATOMIC_RELAXED); return;
            }
        }
    }
}
void nv2a_lockcount_report(double frames)
{
    if (s_lockcnt_on != 1) return;
    for (int r = 0; r < 10; r++) {
        int best = -1;
        for (int i = 0; i < 64; i++) if (s_lockcnt[i].site && s_lockcnt[i].n && (best < 0 || s_lockcnt[i].n > s_lockcnt[best].n)) best = i;
        if (best < 0) break;
        Dl_info di; const char *sym = "?"; uintptr_t off = 0;
        if (dladdr(s_lockcnt[best].site, &di) && di.dli_sname) { sym = di.dli_sname; off = (uintptr_t)s_lockcnt[best].site - (uintptr_t)di.dli_saddr; }
        fprintf(stderr, "[GPERF]   lock calls from %s+0x%lx (mutex %p): %.0f/frame\n", sym, (unsigned long)off,
                s_lockcnt[best].m, s_lockcnt[best].n / frames);
        s_lockcnt[best].n = 0;
    }
    for (int i = 0; i < 64; i++) s_lockcnt[i].n = 0;
}
void qemu_mutex_lock(QemuMutex *m)
{
    if (__builtin_expect(s_lockcnt_on != 0, 0)) {
        if (s_lockcnt_on < 0) s_lockcnt_on = getenv("NV2A_LOCKCOUNT") ? 1 : 0;
        if (s_lockcnt_on == 1) lockcount_note(__builtin_return_address(0), m);
    }
    if (pthread_mutex_trylock(&m->m) == 0) return;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    /* someone holding it for a run of work (the pfifo thread) checks this */
    __atomic_fetch_add(&m->waiters, 1, __ATOMIC_SEQ_CST);
    int e = pthread_mutex_lock(&m->m);
    __atomic_fetch_sub(&m->waiters, 1, __ATOMIC_RELAXED);
    if (e) { fprintf(stderr, "[nv2a] mutex lock error %d\n", e); abort(); }
    clock_gettime(CLOCK_MONOTONIC, &b);
    contend_note(__builtin_return_address(0),
                 (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (b.tv_nsec - a.tv_nsec));
}
int qemu_mutex_trylock(QemuMutex *m) { return pthread_mutex_trylock(&m->m); }
void qemu_mutex_unlock(QemuMutex *m)
{
    int e = pthread_mutex_unlock(&m->m);
    if (e) { fprintf(stderr, "[nv2a] mutex unlock error %d\n", e); abort(); }
}
void qemu_cond_init(QemuCond *c) { pthread_cond_init(&c->c, NULL); }
void qemu_cond_destroy(QemuCond *c) { pthread_cond_destroy(&c->c); }
void qemu_cond_signal(QemuCond *c) { pthread_cond_signal(&c->c); }
void qemu_cond_broadcast(QemuCond *c) { pthread_cond_broadcast(&c->c); }
void qemu_cond_wait(QemuCond *c, QemuMutex *m) { pthread_cond_wait(&c->c, &m->m); }
bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}
void qemu_event_init(QemuEvent *e, bool init)
{
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->set = init;
}
void qemu_event_destroy(QemuEvent *e) { pthread_mutex_destroy(&e->m); pthread_cond_destroy(&e->c); }
void qemu_event_set(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = true;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}
void qemu_event_reset(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = false;
    pthread_mutex_unlock(&e->m);
}
void qemu_event_wait(QemuEvent *e)
{
    pthread_mutex_lock(&e->m);
    while (!e->set) pthread_cond_wait(&e->c, &e->m);
    pthread_mutex_unlock(&e->m);
}
struct thread_start { void *(*fn)(void *); void *arg; char name[16]; };
static void *thread_trampoline(void *p)
{
    struct thread_start s = *(struct thread_start *)p;
    free(p);
#if defined(__linux__) || defined(__ANDROID__)
    pthread_setname_np(pthread_self(), s.name);
#endif
    return s.fn(s.arg);
}
void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    struct thread_start *s = g_malloc0(sizeof *s);
    s->fn = fn; s->arg = arg;
    snprintf(s->name, sizeof s->name, "%s", name ? name : "nv2a");
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    if (mode == QEMU_THREAD_DETACHED) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->t, &a, thread_trampoline, s) != 0) abort();
    pthread_attr_destroy(&a);
}
void *qemu_thread_join(QemuThread *t) { void *r = NULL; pthread_join(t->t, &r); return r; }
void qemu_spin_init(QemuSpin *s) { pthread_mutex_init(&s->m, NULL); }
void qemu_spin_lock(QemuSpin *s) { pthread_mutex_lock(&s->m); }
void qemu_spin_unlock(QemuSpin *s) { pthread_mutex_unlock(&s->m); }

/* ── Clocks and timers ────────────────────────────────────────────────── */

int64_t qemu_clock_get_ns(int clock)
{
    struct timespec ts;
    clock_gettime(clock == QEMU_CLOCK_REALTIME ? CLOCK_MONOTONIC : CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static pthread_mutex_t s_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_timer_cond = PTHREAD_COND_INITIALIZER;
static QEMUTimer *s_timers;
static bool s_timer_thread_up;

static void *timer_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_timer_lock);
    for (;;) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = INT64_MAX;
        for (QEMUTimer *t = s_timers; t; t = t->next) {
            if (!t->armed) continue;
            if (t->expire_ns <= now) {
                t->armed = false;
                QEMUTimerCB *cb = t->cb; void *op = t->opaque;
                pthread_mutex_unlock(&s_timer_lock);
                cb(op);
                pthread_mutex_lock(&s_timer_lock);
                next = 0;          /* rescan: the list may have changed */
                break;
            }
            if (t->expire_ns < next) next = t->expire_ns;
        }
        if (next == 0) continue;
        if (next == INT64_MAX) {
            pthread_cond_wait(&s_timer_cond, &s_timer_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t wait = next - now;
            int64_t abs = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + wait;
            ts.tv_sec = abs / 1000000000LL; ts.tv_nsec = abs % 1000000000LL;
            pthread_cond_timedwait(&s_timer_cond, &s_timer_lock, &ts);
        }
    }
    return NULL;
}
void timer_init_ns(QEMUTimer *t, int clock, QEMUTimerCB *cb, void *opaque)
{
    (void)clock;
    pthread_mutex_lock(&s_timer_lock);
    t->cb = cb; t->opaque = opaque; t->armed = false;
    t->next = s_timers; s_timers = t;
    if (!s_timer_thread_up) {
        pthread_t th;
        pthread_create(&th, NULL, timer_thread, NULL);
        pthread_detach(th);
        s_timer_thread_up = true;
    }
    pthread_mutex_unlock(&s_timer_lock);
}
void timer_mod(QEMUTimer *t, int64_t expire_ns)
{
    pthread_mutex_lock(&s_timer_lock);
    t->expire_ns = expire_ns; t->armed = true;
    pthread_cond_broadcast(&s_timer_cond);
    pthread_mutex_unlock(&s_timer_lock);
}
void timer_del(QEMUTimer *t)
{
    pthread_mutex_lock(&s_timer_lock);
    t->armed = false;
    pthread_mutex_unlock(&s_timer_lock);
}
bool timer_pending(QEMUTimer *t) { return t->armed; }

/* ── Guest RAM dirty tracking ─────────────────────────────────────────── *
 * One bit per 4 KiB page per client. Every bit starts set (QEMU marks all
 * of VRAM dirty at init). The host clears nothing itself: the write-protect
 * fault handler calls nv2a_shim_mark_dirty() for the page a write hit, and
 * test_and_clear asks the host to re-protect pages it cleared, so the next
 * CPU write is seen again. */

#define DIRTY_PAGE_BITS 12
static unsigned long *s_dirty[DIRTY_MEMORY_NUM];
static uint64_t s_ram_size;
static nv2a_shim_protect_fn s_protect;

extern uint32_t *nv2a_page_wepoch;
void nv2a_shim_dirty_init(uint64_t ram_size, nv2a_shim_protect_fn protect)
{
    s_ram_size = ram_size;
    s_protect = protect;
    size_t pages = (size_t)(ram_size >> DIRTY_PAGE_BITS);
    size_t longs = (pages + 63) / 64;
    for (int c = 0; c < DIRTY_MEMORY_NUM; c++) {
        s_dirty[c] = g_malloc(longs * sizeof(unsigned long));
        memset(s_dirty[c], 0xff, longs * sizeof(unsigned long));
    }
    nv2a_page_wepoch = g_malloc0(pages * sizeof(uint32_t));
}

/* nv2a-port: bumped after any page is marked dirty for the texture client,
 * so the texture cache can skip a test-and-clear (a walk of the dirty
 * bitmap plus re-protecting pages) for a texture checked since the last
 * write anywhere -- it ran for every texture of every draw. */
volatile uint32_t nv2a_tex_dirty_gen = 1;
/* nv2a-port: per 4 KB page, the write generation of the last write seen
 * there (any client). A cache of data copied out of guest RAM keeps the
 * generation it copied at and is valid while no page it spans is newer. */
uint32_t *nv2a_page_wepoch;
volatile uint32_t nv2a_wepoch_gen = 1;
static inline void stamp_pages(uint64_t first, uint64_t last)
{
    if (!nv2a_page_wepoch) return;
    uint32_t e = __atomic_add_fetch(&nv2a_wepoch_gen, 1, __ATOMIC_ACQ_REL);
    for (uint64_t p = first; p <= last; p++)
        __atomic_store_n(&nv2a_page_wepoch[p], e, __ATOMIC_RELEASE);
}
void nv2a_shim_mark_dirty(uint64_t addr, uint64_t size)
{
    if (!s_dirty[0] || addr >= s_ram_size) return;
    if (addr + size > s_ram_size) size = s_ram_size - addr;
    uint64_t first = addr >> DIRTY_PAGE_BITS, last = (addr + size - 1) >> DIRTY_PAGE_BITS;
    for (uint64_t p = first; p <= last; p++)
        for (int c = 0; c < DIRTY_MEMORY_NUM; c++)
            __atomic_or_fetch(&s_dirty[c][p / 64], 1UL << (p % 64), __ATOMIC_RELAXED);
    stamp_pages(first, last);
    __atomic_fetch_add(&nv2a_tex_dirty_gen, 1, __ATOMIC_RELEASE);
}

bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    (void)mr;
    if (!s_dirty[client] || !size || addr >= s_ram_size) return false;
    if (addr + size > s_ram_size) size = s_ram_size - addr;
    uint64_t first = addr >> DIRTY_PAGE_BITS, last = (addr + size - 1) >> DIRTY_PAGE_BITS;
    bool dirty = false;
    uint64_t run_lo = 0, run_n = 0;   /* consumed pages, re-armed per run */
    /* nv2a-port: a word (64 pages) at a time, and a plain load first: the
     * renderer asks this of a whole 640x480 target on every draw, and one
     * atomic per page was a sizeable part of the PFIFO thread. Re-arming
     * (s_protect) is per run of consumed pages, as before: the page is
     * re-armed as soon as this client has consumed it, or vertex pages the
     * texture client never looks at stayed open and later CPU writes were
     * missed (stale vertices: the spikes and fans). */
    for (uint64_t w = first / 64; w <= last / 64; w++) {
        uint64_t lo = (w == first / 64) ? first % 64 : 0;
        uint64_t hi = (w == last / 64) ? last % 64 : 63;
        unsigned long mask = (hi - lo == 63) ? ~0UL : (((1UL << (hi - lo + 1)) - 1) << lo);
        if (!(__atomic_load_n(&s_dirty[client][w], __ATOMIC_RELAXED) & mask)) continue;
        unsigned long old = __atomic_fetch_and(&s_dirty[client][w], ~mask, __ATOMIC_RELAXED) & mask;
        while (old) {
            unsigned b = (unsigned)__builtin_ctzl(old);
            old &= old - 1;
            uint64_t p = w * 64 + b;
            dirty = true;
            if (run_n && run_lo + run_n == p) { run_n++; continue; }
            if (run_n && s_protect)
                s_protect(run_lo << DIRTY_PAGE_BITS, run_n << DIRTY_PAGE_BITS);
            run_lo = p; run_n = 1;
        }
    }
    if (run_n && s_protect)
        s_protect(run_lo << DIRTY_PAGE_BITS, run_n << DIRTY_PAGE_BITS);
    return dirty;
}

bool memory_region_get_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    (void)mr;
    if (!s_dirty[client] || !size || addr >= s_ram_size) return false;
    uint64_t first = addr >> DIRTY_PAGE_BITS, last = (addr + size - 1) >> DIRTY_PAGE_BITS;
    if (addr + size > s_ram_size) last = (s_ram_size - 1) >> DIRTY_PAGE_BITS;
    for (uint64_t w = first / 64; w <= last / 64; w++) {
        uint64_t lo = (w == first / 64) ? first % 64 : 0;
        uint64_t hi = (w == last / 64) ? last % 64 : 63;
        unsigned long mask = (hi - lo == 63) ? ~0UL : (((1UL << (hi - lo + 1)) - 1) << lo);
        if (__atomic_load_n(&s_dirty[client][w], __ATOMIC_RELAXED) & mask) return true;
    }
    return false;
}

void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    (void)mr;
    if (!s_dirty[client] || !size || addr >= s_ram_size) return;
    if (addr + size > s_ram_size) size = s_ram_size - addr;
    uint64_t first = addr >> DIRTY_PAGE_BITS, last = (addr + size - 1) >> DIRTY_PAGE_BITS;
    for (uint64_t p = first; p <= last; p++)
        __atomic_or_fetch(&s_dirty[client][p / 64], 1UL << (p % 64), __ATOMIC_RELAXED);
    stamp_pages(first, last);
    __atomic_fetch_add(&nv2a_tex_dirty_gen, 1, __ATOMIC_RELEASE);
}

void memory_region_set_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size)
{
    for (int c = 0; c < DIRTY_MEMORY_NUM; c++)
        memory_region_set_client_dirty(mr, addr, size, (unsigned)c);
}

/* ── Settings, files, notifications ───────────────────────────────────── */

struct config g_config = {
    .display = {
        .renderer = CONFIG_DISPLAY_RENDERER_VULKAN,
        .quality = { .surface_scale = 1 },
        .vulkan = { .validation_layers = false, .debug_shaders = false,
                    .assert_on_validation_msg = false,
                    .preferred_physical_device = "" },
    },
    .perf = { .cache_shaders = true },
};
const int xemu_version_major = 0, xemu_version_minor = 8, xemu_version_patch = 0;
const char *xemu_version = "nv2a-port";
static char s_base_path[1024] = "./";

void nv2a_shim_set_base_path(const char *path)
{
    snprintf(s_base_path, sizeof s_base_path, "%s/", path);
}
const char *xemu_settings_get_base_path(void) { return s_base_path; }
void xemu_settings_set_string(const char **str, const char *new_str) { *str = strdup(new_str ? new_str : ""); }
void xemu_queue_notification(const char *msg) { fprintf(stderr, "[nv2a] %s\n", msg); }
void xemu_queue_error_message(const char *msg) { fprintf(stderr, "[nv2a] error: %s\n", msg); }
FILE *qemu_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int qemu_mkdir(const char *path) { return mkdir(path, 0755); }
int qemu_unlink(const char *path) { return unlink(path); }

/* ── Event timeline (NV2A_TL=1) ─────────────────────────────────────────────
 * Who waits on whom, frame by frame: the title's BlockOnTime waits and PUT
 * kicks, the PFIFO thread's idle and fence waits, Vulkan submits, semaphore
 * releases, GPU interrupts and vblanks, timestamped on one clock. Records a
 * window NV2A_TL_START seconds (default 175) after the first event, for
 * NV2A_TL_MS (default 400) ms, then prints it once as [TL] lines. */
#include <stdatomic.h>
#include <unistd.h>
#include <sys/syscall.h>
typedef struct { uint64_t ns; uint32_t arg; uint16_t tid; uint8_t ev; } TlEv;
#define TL_MAX 65536
static TlEv *s_tl;
static atomic_uint s_tl_n;
static int s_tl_state = -1;        /* -1 unknown, 0 off, 1 waiting, 2 recording, 3 done */
static uint64_t s_tl_start_ns, s_tl_end_ns;
static uint64_t tl_now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static const char *const s_tl_names[] = {
    "?", "BOT_IN", "BOT_OUT", "PUT", "IDLE_IN", "IDLE_OUT", "SEM", "SUBMIT",
    "FENCE_IN", "FENCE_OUT", "IRQ_RAISE", "ISR_IN", "ISR_OUT", "VBLANK",
    "FLIP_STALL", "FLIP_DONE", "NOP_TRAP", "BUSY_IN", "BUSY_OUT", "PRESENT",
    "W_INTR", "W_FIFO", "DPC_IN", "DPC_OUT", "APU_SECTL", "APU_POST", "APU_ISR_IN", "APU_ISR_OUT", "DPC_QUEUE", "DELAY_IN", "DELAY_OUT", "WAIT1", "APU_RD", "APU_WR"
};
static void tl_dump(void)
{
    unsigned n = atomic_load(&s_tl_n);
    if (n > TL_MAX) n = TL_MAX;
    /* to a file in the dump folder the autotest pulls: logcat drops lines
     * when thousands arrive at once */
    char path[512]; FILE *f = NULL;
    const char *dir = getenv("NV2A_DUMP_DIR");
    if (dir) { snprintf(path, sizeof path, "%s/tl.txt", dir); f = fopen(path, "w"); }
    fprintf(stderr, "[TL] %u events over %.1f ms -> %s\n", n,
            (s_tl_end_ns - s_tl_start_ns) / 1e6, f ? path : "logcat");
    FILE *o = f ? f : stderr;
    for (unsigned i = 0; i < n; i++) {
        TlEv *e = &s_tl[i];
        if (!e->ns) continue;
        fprintf(o, "%9.3f %5u %-10s %08X\n", (e->ns - s_tl_start_ns) / 1e6,
                e->tid, e->ev < sizeof s_tl_names / sizeof *s_tl_names ? s_tl_names[e->ev] : "?",
                e->arg);
        if (!f && (i & 255) == 255) usleep(2000);
    }
    if (f) fclose(f);
}
void nv2a_tl(int ev, uint32_t arg)
{
    int st = __atomic_load_n(&s_tl_state, __ATOMIC_ACQUIRE);
    if (st == 0 || st == 3) return;
    uint64_t now = tl_now();
    if (st < 0) {
        static atomic_int once;
        if (atomic_exchange(&once, 1)) return;
        if (!getenv("NV2A_TL")) { __atomic_store_n(&s_tl_state, 0, __ATOMIC_RELEASE); return; }
        const char *e = getenv("NV2A_TL_START");
        double start = e ? atof(e) : 198.0;
        /* a switch file can only say 1: NV2A_TL_S<seconds>=1 sets the start */
        extern char **environ;
        for (char **v = environ; v && *v; v++)
            if (!strncmp(*v, "NV2A_TL_S", 9) && (*v)[9] >= '0' && (*v)[9] <= '9')
                start = atof(*v + 9);
        s_tl_start_ns = now + (uint64_t)(start * 1e9);
        e = getenv("NV2A_TL_MS");
        s_tl_end_ns = s_tl_start_ns + (uint64_t)((e ? atof(e) : 1500.0) * 1e6);
        s_tl = calloc(TL_MAX, sizeof *s_tl);
        __atomic_store_n(&s_tl_state, s_tl ? 1 : 0, __ATOMIC_RELEASE);
        return;
    }
    if (now < s_tl_start_ns) return;
    if (now >= s_tl_end_ns) {
        int exp = st;
        if (st == 2 && __atomic_compare_exchange_n(&s_tl_state, &exp, 3, 0,
                                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            tl_dump();
        else if (st == 1)
            __atomic_store_n(&s_tl_state, 3, __ATOMIC_RELEASE);
        return;
    }
    if (st == 1) __atomic_store_n(&s_tl_state, 2, __ATOMIC_RELEASE);
    unsigned i = atomic_fetch_add(&s_tl_n, 1);
    if (i >= TL_MAX) return;
    s_tl[i].arg = arg; s_tl[i].ev = (uint8_t)ev;
    s_tl[i].tid = (uint16_t)syscall(SYS_gettid);
    __atomic_store_n(&s_tl[i].ns, now, __ATOMIC_RELEASE);
}
