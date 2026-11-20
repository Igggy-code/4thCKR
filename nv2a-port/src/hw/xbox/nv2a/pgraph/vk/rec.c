/*
 * nv2a-port: command recording on a second thread.
 *
 * The pfifo thread decodes the title's push buffer and, until now, also
 * called the Vulkan driver for every command it recorded. On Mali about a
 * third of that thread's time went inside the driver's vkCmd* entry points,
 * and the thread was the one thing holding the frame rate (100 % of a core).
 *
 * Here the vkCmd* entry points of the loaded device (volk's globals) are
 * replaced, for the pfifo thread only, by functions that copy their
 * arguments into a ring; a worker thread ("nv2a.rec") replays them into the
 * real driver in the same order. Anything that needs the recorded commands
 * to exist -- a submit, a wait on a fence, a query result, command buffer
 * allocation or freeing (the pool is shared) -- first waits until the ring
 * is empty ("drain"). Begin/End/Reset of a command buffer go through the
 * ring too, so the order of everything touching a command buffer is kept.
 *
 * A call this file cannot copy (a pNext chain it does not know) drains and
 * runs directly on the calling thread, exactly as before.
 *
 * NV2A_NO_REC=1: off (everything direct, as before).
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/prctl.h>
#include <volk.h>

/* ---- the real entry points ---------------------------------------------- */
#define REC_FUNCS(X) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdDraw) \
    X(vkCmdBindPipeline) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) \
    X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCmdPushConstants) \
    X(vkCmdDrawIndexed) X(vkCmdBindIndexBuffer) X(vkCmdBindDescriptorSets) \
    X(vkCmdSetScissor) X(vkCmdEndRenderPass) X(vkCmdCopyImage) \
    X(vkCmdCopyBuffer) X(vkCmdBeginRenderPass) X(vkCmdSetViewport) \
    X(vkCmdSetBlendConstants) X(vkCmdDispatch) X(vkCmdClearColorImage) \
    X(vkCmdClearAttachments) X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) \
    X(vkCmdEndQuery) X(vkCmdClearDepthStencilImage) X(vkCmdBindVertexBuffers) \
    X(vkCmdBeginQuery) X(vkCmdInsertDebugUtilsLabelEXT) \
    X(vkCmdBeginDebugUtilsLabelEXT) X(vkCmdEndDebugUtilsLabelEXT) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkWaitForFences) X(vkGetFenceStatus) X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) X(vkResetCommandPool) X(vkGetQueryPoolResults) \
    X(vkQueuePresentKHR) X(vkDestroyCommandPool) X(vkUpdateDescriptorSets) \
    X(vkFreeDescriptorSets) X(vkResetDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkResetFences) X(vkDestroyImageView) X(vkDestroySampler) X(vkDestroyFence) \
    X(vkDestroySemaphore) X(vkDestroySwapchainKHR)

static struct {
#define X(n) PFN_##n n;
    REC_FUNCS(X)
#undef X
} R;

/* ---- ring ---------------------------------------------------------------- */
#define RING_SIZE (8u << 20)
#define ALIGN8(x) (((x) + 7u) & ~7u)

enum {
    OP_WRAP = 1, OP_BARRIER, OP_COPY_B2I, OP_DRAW, OP_BIND_PIPE, OP_COPY_I2B,
    OP_BLIT, OP_TIMESTAMP, OP_RESET_QP, OP_PUSH, OP_DRAW_IDX, OP_BIND_IDX,
    OP_BIND_DS, OP_SCISSOR, OP_END_RP, OP_COPY_I, OP_COPY_B, OP_BEGIN_RP,
    OP_VIEWPORT, OP_BLEND_C, OP_DISPATCH, OP_CLEAR_COLOR, OP_CLEAR_ATT,
    OP_LINE_W, OP_DEPTH_BIAS, OP_END_Q, OP_CLEAR_DS, OP_BIND_VB, OP_BEGIN_Q,
    OP_BEGIN_CB, OP_END_CB, OP_RESET_CB, OP_LABEL_END, OP_SUBMIT, OP_UPDATE_DS, OP_PRESENT,
};

typedef struct { uint32_t op, size; VkCommandBuffer cb; } Hdr;

static uint8_t *s_ring;
static volatile uint32_t s_head;   /* producer: bytes written (mod RING) */
static volatile uint32_t s_tail;   /* consumer: bytes done */
static volatile int s_sleeping;
static int s_draining;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_cv_done = PTHREAD_COND_INITIALIZER;
static bool s_on;
static __thread int t_producer;
static uint64_t s_ops, s_direct;
static uint64_t s_drains;   /* atomic: any thread may drain */
/* the queue: the worker submits, the present thread submits and presents,
 * and they no longer share the pgraph lock -- one lock for every queue call */
static pthread_mutex_t s_qmx = PTHREAD_MUTEX_INITIALIZER;
/* command pools are externally synchronized: the present thread records
 * its display pass into a buffer of the same pool as the pfifo thread's,
 * and used to be kept apart from it by the pgraph lock -- the worker does
 * not hold that lock. Every real call that touches a command buffer or
 * pool, from the worker or from any other thread, holds this one. (Without
 * it: VK_ERROR_DEVICE_LOST after some minutes, the picture froze.) */
static pthread_mutex_t s_rmx = PTHREAD_MUTEX_INITIALIZER;
#define RLOCK(call) do { pthread_mutex_lock(&s_rmx); call; pthread_mutex_unlock(&s_rmx); } while (0)
static volatile uint32_t s_sub_q, s_sub_done;   /* submits queued / done */
static volatile uint32_t s_ds_q, s_ds_drained;  /* descriptor updates queued; at the last drain */
static bool s_async_submit = true, s_async_ds = true;
static uint64_t s_subs;

static inline uint32_t ld(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void wake_worker(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* head store before the sleeping load */
    if (__atomic_load_n(&s_sleeping, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&s_mx);
        pthread_cond_signal(&s_cv_work);
        pthread_mutex_unlock(&s_mx);
    }
}

/* nv2a-port: drain cost per call site, in the [REC] line */
#include <dlfcn.h>
typedef struct { void *a, *b; uint32_t n; uint64_t ns; } DrainSite;
static DrainSite s_dsite[16];
static pthread_mutex_t s_dsite_mx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dr_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void drain_impl(void);
__attribute__((noinline)) static void drain(void)
{
    static int prof = -1;
    if (prof < 0) prof = getenv("NV2A_REC_DRAINPROF") != NULL;
    if (!prof || ld(&s_tail) == ld(&s_head)) { drain_impl(); return; }
    void *a = __builtin_return_address(0), *b = __builtin_return_address(1);
    uint64_t t0 = dr_now();
    drain_impl();
    uint64_t ns = dr_now() - t0;
    pthread_mutex_lock(&s_dsite_mx);
    int k, fr = -1;
    for (k = 0; k < 16; k++) { if (s_dsite[k].n && s_dsite[k].a == a && s_dsite[k].b == b) break; if (!s_dsite[k].n && fr < 0) fr = k; }
    if (k == 16) k = fr;
    if (k >= 0) { s_dsite[k].a = a; s_dsite[k].b = b; s_dsite[k].n++; s_dsite[k].ns += ns; }
    pthread_mutex_unlock(&s_dsite_mx);
}
static void drain_report(double sec)
{
    char line[1024]; int o = 0;
    pthread_mutex_lock(&s_dsite_mx);
    for (int r = 0; r < 6; r++) {
        int best = -1;
        for (int k = 0; k < 16; k++) if (s_dsite[k].n && (best < 0 || s_dsite[k].ns > s_dsite[best].ns)) best = k;
        if (best < 0) break;
        Dl_info i1, i2; const char *n1 = "?", *n2 = "?";
        if (dladdr(s_dsite[best].a, &i1) && i1.dli_sname) n1 = i1.dli_sname;
        if (dladdr(s_dsite[best].b, &i2) && i2.dli_sname) n2 = i2.dli_sname;
        o += snprintf(line + o, sizeof line - o, " %s<-%s %.0f/s %.2fms/s;", n1, n2,
                      s_dsite[best].n / sec, s_dsite[best].ns / 1e6 / sec);
        s_dsite[best].n = 0;
    }
    memset(s_dsite, 0, sizeof s_dsite);
    pthread_mutex_unlock(&s_dsite_mx);
    fprintf(stderr, "[REC] drains:%s\n", line);
}
/* wait until the worker has replayed everything written so far */
static void drain_impl(void)
{
    uint32_t h = ld(&s_head);
    __atomic_store_n(&s_ds_drained, ld(&s_ds_q), __ATOMIC_RELAXED);
    if (ld(&s_tail) == h) return;
    __atomic_fetch_add(&s_drains, 1, __ATOMIC_RELAXED);
    wake_worker();
    for (int i = 0; i < 600; i++) {
        if (ld(&s_tail) == h) return;
        if ((i & 63) == 63) sched_yield();
    }
    pthread_mutex_lock(&s_mx);
    __atomic_fetch_add(&s_draining, 1, __ATOMIC_SEQ_CST);   /* a count: two threads may wait */
    pthread_cond_signal(&s_cv_work);
    while (ld(&s_tail) != h) pthread_cond_wait(&s_cv_done, &s_mx);
    __atomic_fetch_sub(&s_draining, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s_mx);
}

/* room for n bytes at the head (contiguous); wraps when the end is near */
static uint8_t *reserve(uint32_t n)
{
    n = ALIGN8(n);
    for (;;) {
        uint32_t h = s_head, t = ld(&s_tail);
        uint32_t used = h - t;
        uint32_t pos = h % RING_SIZE;
        uint32_t to_end = RING_SIZE - pos;
        uint32_t need = n <= to_end ? n : to_end + n;
        if (used + need + 64 <= RING_SIZE) {
            if (n > to_end) {
                Hdr *w = (Hdr *)(s_ring + pos);
                w->op = OP_WRAP; w->size = to_end;
                __atomic_store_n(&s_head, h + to_end, __ATOMIC_RELEASE);
                pos = 0;
            }
            return s_ring + pos;
        }
        wake_worker();
        sched_yield();
    }
}

static uint32_t s_woke_at;   /* head at the last wake */
static inline void commit(uint32_t n)
{
    __atomic_store_n(&s_head, s_head + ALIGN8(n), __ATOMIC_RELEASE);
    s_ops++;
    /* a futex wake per command cost the producer more than the command:
     * wake a sleeping worker once 16 KB have piled up; it also wakes on
     * its own every half millisecond, and every drain wakes it at once */
    if (s_head - s_woke_at >= 16384) { s_woke_at = s_head; wake_worker(); }
}

/* a packet: header, fixed args, then copied arrays */
typedef struct { uint8_t *base; uint32_t off; } Pk;
static inline void *pk_put(Pk *p, const void *src, size_t n)
{
    void *d = p->base + p->off;
    if (n) memcpy(d, src, n);
    p->off += ALIGN8((uint32_t)n);
    return d;
}
#define PK_BEGIN(op_, cb_, total_) \
    uint32_t total__ = ALIGN8(sizeof(Hdr)) + (total_); \
    Pk pk_ = { reserve(total__), ALIGN8(sizeof(Hdr)) }; \
    ((Hdr *)pk_.base)->op = (op_); ((Hdr *)pk_.base)->size = ALIGN8(total__); \
    ((Hdr *)pk_.base)->cb = (cb_)
#define PK_END() commit(total__)
#define A8(x) ALIGN8((uint32_t)(x))

static inline bool rec_here(void) { return s_on && t_producer; }
#define DIRECT_IF_NOT_HERE(call) do { if (!rec_here()) { RLOCK(call); return; } } while (0)
#define DIRECT_FALLBACK(call) do { drain(); s_direct++; RLOCK(call); return; } while (0)

/* ---- recording wrappers -------------------------------------------------- */
typedef struct { VkPipelineStageFlags s, d; VkDependencyFlags f; uint32_t nm, nb, ni; } ABar;
static VKAPI_ATTR void VKAPI_CALL w_barrier(VkCommandBuffer cb, VkPipelineStageFlags s,
    VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
    uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im)
{
    DIRECT_IF_NOT_HERE(R.vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, b, ni, im));
    bool odd = false;
    for (uint32_t i = 0; i < nm; i++) odd |= m[i].pNext != NULL;
    for (uint32_t i = 0; i < nb; i++) odd |= b[i].pNext != NULL;
    for (uint32_t i = 0; i < ni; i++) odd |= im[i].pNext != NULL;
    if (odd) DIRECT_FALLBACK(R.vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, b, ni, im));
    PK_BEGIN(OP_BARRIER, cb, A8(sizeof(ABar)) + A8(nm * sizeof *m) + A8(nb * sizeof *b) + A8(ni * sizeof *im));
    ABar a = { s, d, f, nm, nb, ni };
    pk_put(&pk_, &a, sizeof a);
    pk_put(&pk_, m, nm * sizeof *m); pk_put(&pk_, b, nb * sizeof *b); pk_put(&pk_, im, ni * sizeof *im);
    PK_END();
}

typedef struct { VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; } AB2I;
static VKAPI_ATTR void VKAPI_CALL w_copy_b2i(VkCommandBuffer cb, VkBuffer b, VkImage i,
    VkImageLayout l, uint32_t n, const VkBufferImageCopy *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdCopyBufferToImage(cb, b, i, l, n, r));
    PK_BEGIN(OP_COPY_B2I, cb, A8(sizeof(AB2I)) + A8(n * sizeof *r));
    AB2I a = { b, i, l, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; } AI2B;
static VKAPI_ATTR void VKAPI_CALL w_copy_i2b(VkCommandBuffer cb, VkImage i, VkImageLayout l,
    VkBuffer b, uint32_t n, const VkBufferImageCopy *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdCopyImageToBuffer(cb, i, l, b, n, r));
    PK_BEGIN(OP_COPY_I2B, cb, A8(sizeof(AI2B)) + A8(n * sizeof *r));
    AI2B a = { i, l, b, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { uint32_t vc, ic, fv, fi; } ADraw;
static VKAPI_ATTR void VKAPI_CALL w_draw(VkCommandBuffer cb, uint32_t vc, uint32_t ic,
                                         uint32_t fv, uint32_t fi)
{
    DIRECT_IF_NOT_HERE(R.vkCmdDraw(cb, vc, ic, fv, fi));
    PK_BEGIN(OP_DRAW, cb, A8(sizeof(ADraw)));
    ADraw a = { vc, ic, fv, fi }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { uint32_t ic, inst, fi; int32_t vo; uint32_t fin; } ADrawI;
static VKAPI_ATTR void VKAPI_CALL w_draw_idx(VkCommandBuffer cb, uint32_t ic, uint32_t inst,
                                             uint32_t fi, int32_t vo, uint32_t fin)
{
    DIRECT_IF_NOT_HERE(R.vkCmdDrawIndexed(cb, ic, inst, fi, vo, fin));
    PK_BEGIN(OP_DRAW_IDX, cb, A8(sizeof(ADrawI)));
    ADrawI a = { ic, inst, fi, vo, fin }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkPipelineBindPoint bp; VkPipeline p; } APipe;
static VKAPI_ATTR void VKAPI_CALL w_bind_pipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBindPipeline(cb, bp, p));
    PK_BEGIN(OP_BIND_PIPE, cb, A8(sizeof(APipe)));
    APipe a = { bp, p }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkImage si; VkImageLayout sl; VkImage di; VkImageLayout dl; uint32_t n; VkFilter f; } ABlit;
static VKAPI_ATTR void VKAPI_CALL w_blit(VkCommandBuffer cb, VkImage si, VkImageLayout sl,
    VkImage di, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBlitImage(cb, si, sl, di, dl, n, r, f));
    PK_BEGIN(OP_BLIT, cb, A8(sizeof(ABlit)) + A8(n * sizeof *r));
    ABlit a = { si, sl, di, dl, n, f }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { VkImage si; VkImageLayout sl; VkImage di; VkImageLayout dl; uint32_t n; } ACopyI;
static VKAPI_ATTR void VKAPI_CALL w_copy_i(VkCommandBuffer cb, VkImage si, VkImageLayout sl,
    VkImage di, VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdCopyImage(cb, si, sl, di, dl, n, r));
    PK_BEGIN(OP_COPY_I, cb, A8(sizeof(ACopyI)) + A8(n * sizeof *r));
    ACopyI a = { si, sl, di, dl, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { VkBuffer s, d; uint32_t n; } ACopyB;
static VKAPI_ATTR void VKAPI_CALL w_copy_b(VkCommandBuffer cb, VkBuffer s, VkBuffer d,
                                           uint32_t n, const VkBufferCopy *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdCopyBuffer(cb, s, d, n, r));
    PK_BEGIN(OP_COPY_B, cb, A8(sizeof(ACopyB)) + A8(n * sizeof *r));
    ACopyB a = { s, d, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { VkPipelineStageFlagBits st; VkQueryPool qp; uint32_t q; } ATs;
static VKAPI_ATTR void VKAPI_CALL w_timestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st,
                                              VkQueryPool qp, uint32_t q)
{
    DIRECT_IF_NOT_HERE(R.vkCmdWriteTimestamp(cb, st, qp, q));
    PK_BEGIN(OP_TIMESTAMP, cb, A8(sizeof(ATs)));
    ATs a = { st, qp, q }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkQueryPool qp; uint32_t f, n; } AResQ;
static VKAPI_ATTR void VKAPI_CALL w_reset_qp(VkCommandBuffer cb, VkQueryPool qp, uint32_t f, uint32_t n)
{
    DIRECT_IF_NOT_HERE(R.vkCmdResetQueryPool(cb, qp, f, n));
    PK_BEGIN(OP_RESET_QP, cb, A8(sizeof(AResQ)));
    AResQ a = { qp, f, n }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkQueryPool qp; uint32_t q; VkQueryControlFlags fl; } AQ;
static VKAPI_ATTR void VKAPI_CALL w_begin_q(VkCommandBuffer cb, VkQueryPool qp, uint32_t q,
                                            VkQueryControlFlags fl)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBeginQuery(cb, qp, q, fl));
    PK_BEGIN(OP_BEGIN_Q, cb, A8(sizeof(AQ)));
    AQ a = { qp, q, fl }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}
static VKAPI_ATTR void VKAPI_CALL w_end_q(VkCommandBuffer cb, VkQueryPool qp, uint32_t q)
{
    DIRECT_IF_NOT_HERE(R.vkCmdEndQuery(cb, qp, q));
    PK_BEGIN(OP_END_Q, cb, A8(sizeof(AQ)));
    AQ a = { qp, q, 0 }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, sz; } APush;
static VKAPI_ATTR void VKAPI_CALL w_push(VkCommandBuffer cb, VkPipelineLayout l,
    VkShaderStageFlags st, uint32_t off, uint32_t sz, const void *v)
{
    DIRECT_IF_NOT_HERE(R.vkCmdPushConstants(cb, l, st, off, sz, v));
    PK_BEGIN(OP_PUSH, cb, A8(sizeof(APush)) + A8(sz));
    APush a = { l, st, off, sz }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, v, sz);
    PK_END();
}

typedef struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } AIdx;
static VKAPI_ATTR void VKAPI_CALL w_bind_idx(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBindIndexBuffer(cb, b, o, t));
    PK_BEGIN(OP_BIND_IDX, cb, A8(sizeof(AIdx)));
    AIdx a = { b, o, t }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; } ADs;
static VKAPI_ATTR void VKAPI_CALL w_bind_ds(VkCommandBuffer cb, VkPipelineBindPoint bp,
    VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *ds,
    uint32_t nd, const uint32_t *dyn)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBindDescriptorSets(cb, bp, l, first, n, ds, nd, dyn));
    PK_BEGIN(OP_BIND_DS, cb, A8(sizeof(ADs)) + A8(n * sizeof *ds) + A8(nd * sizeof *dyn));
    ADs a = { bp, l, first, n, nd }; pk_put(&pk_, &a, sizeof a);
    pk_put(&pk_, ds, n * sizeof *ds); pk_put(&pk_, dyn, nd * sizeof *dyn);
    PK_END();
}

typedef struct { uint32_t first, n; } ARange;
static VKAPI_ATTR void VKAPI_CALL w_scissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdSetScissor(cb, first, n, r));
    PK_BEGIN(OP_SCISSOR, cb, A8(sizeof(ARange)) + A8(n * sizeof *r));
    ARange a = { first, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}
static VKAPI_ATTR void VKAPI_CALL w_viewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    DIRECT_IF_NOT_HERE(R.vkCmdSetViewport(cb, first, n, v));
    PK_BEGIN(OP_VIEWPORT, cb, A8(sizeof(ARange)) + A8(n * sizeof *v));
    ARange a = { first, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, v, n * sizeof *v);
    PK_END();
}

typedef struct { uint32_t first, n; } AVb;
static VKAPI_ATTR void VKAPI_CALL w_bind_vb(VkCommandBuffer cb, uint32_t first, uint32_t n,
    const VkBuffer *b, const VkDeviceSize *o)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBindVertexBuffers(cb, first, n, b, o));
    PK_BEGIN(OP_BIND_VB, cb, A8(sizeof(AVb)) + A8(n * sizeof *b) + A8(n * sizeof *o));
    AVb a = { first, n }; pk_put(&pk_, &a, sizeof a);
    pk_put(&pk_, b, n * sizeof *b); pk_put(&pk_, o, n * sizeof *o);
    PK_END();
}

static VKAPI_ATTR void VKAPI_CALL w_end_rp(VkCommandBuffer cb)
{
    DIRECT_IF_NOT_HERE(R.vkCmdEndRenderPass(cb));
    PK_BEGIN(OP_END_RP, cb, 0);
    PK_END();
}

typedef struct { VkRenderPass rp; VkFramebuffer fb; VkRect2D area; uint32_t ncv; VkSubpassContents c; } ARp;
static VKAPI_ATTR void VKAPI_CALL w_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                             VkSubpassContents c)
{
    DIRECT_IF_NOT_HERE(R.vkCmdBeginRenderPass(cb, bi, c));
    if (bi->pNext) DIRECT_FALLBACK(R.vkCmdBeginRenderPass(cb, bi, c));
    uint32_t ncv = bi->clearValueCount;
    PK_BEGIN(OP_BEGIN_RP, cb, A8(sizeof(ARp)) + A8(ncv * sizeof(VkClearValue)));
    ARp a = { bi->renderPass, bi->framebuffer, bi->renderArea, ncv, c };
    pk_put(&pk_, &a, sizeof a); pk_put(&pk_, bi->pClearValues, ncv * sizeof(VkClearValue));
    PK_END();
}

typedef struct { float c[4]; } ABc;
static VKAPI_ATTR void VKAPI_CALL w_blend_c(VkCommandBuffer cb, const float c[4])
{
    DIRECT_IF_NOT_HERE(R.vkCmdSetBlendConstants(cb, c));
    PK_BEGIN(OP_BLEND_C, cb, A8(sizeof(ABc)));
    ABc a; memcpy(a.c, c, sizeof a.c); pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { uint32_t x, y, z; } ADisp;
static VKAPI_ATTR void VKAPI_CALL w_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{
    DIRECT_IF_NOT_HERE(R.vkCmdDispatch(cb, x, y, z));
    PK_BEGIN(OP_DISPATCH, cb, A8(sizeof(ADisp)));
    ADisp a = { x, y, z }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { VkImage i; VkImageLayout l; VkClearColorValue c; uint32_t n; } ACc;
static VKAPI_ATTR void VKAPI_CALL w_clear_color(VkCommandBuffer cb, VkImage i, VkImageLayout l,
    const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdClearColorImage(cb, i, l, c, n, r));
    PK_BEGIN(OP_CLEAR_COLOR, cb, A8(sizeof(ACc)) + A8(n * sizeof *r));
    ACc a = { i, l, *c, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { VkImage i; VkImageLayout l; VkClearDepthStencilValue c; uint32_t n; } ACds;
static VKAPI_ATTR void VKAPI_CALL w_clear_ds(VkCommandBuffer cb, VkImage i, VkImageLayout l,
    const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdClearDepthStencilImage(cb, i, l, c, n, r));
    PK_BEGIN(OP_CLEAR_DS, cb, A8(sizeof(ACds)) + A8(n * sizeof *r));
    ACds a = { i, l, *c, n }; pk_put(&pk_, &a, sizeof a); pk_put(&pk_, r, n * sizeof *r);
    PK_END();
}

typedef struct { uint32_t na, nr; } ACa;
static VKAPI_ATTR void VKAPI_CALL w_clear_att(VkCommandBuffer cb, uint32_t na,
    const VkClearAttachment *at, uint32_t nr, const VkClearRect *r)
{
    DIRECT_IF_NOT_HERE(R.vkCmdClearAttachments(cb, na, at, nr, r));
    PK_BEGIN(OP_CLEAR_ATT, cb, A8(sizeof(ACa)) + A8(na * sizeof *at) + A8(nr * sizeof *r));
    ACa a = { na, nr }; pk_put(&pk_, &a, sizeof a);
    pk_put(&pk_, at, na * sizeof *at); pk_put(&pk_, r, nr * sizeof *r);
    PK_END();
}

typedef struct { float w; } ALw;
static VKAPI_ATTR void VKAPI_CALL w_line_w(VkCommandBuffer cb, float w)
{
    DIRECT_IF_NOT_HERE(R.vkCmdSetLineWidth(cb, w));
    PK_BEGIN(OP_LINE_W, cb, A8(sizeof(ALw)));
    ALw a = { w }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

typedef struct { float c, cl, s; } ADb;
static VKAPI_ATTR void VKAPI_CALL w_depth_bias(VkCommandBuffer cb, float c, float cl, float s)
{
    DIRECT_IF_NOT_HERE(R.vkCmdSetDepthBias(cb, c, cl, s));
    PK_BEGIN(OP_DEPTH_BIAS, cb, A8(sizeof(ADb)));
    ADb a = { c, cl, s }; pk_put(&pk_, &a, sizeof a);
    PK_END();
}

/* debug labels: only useful under a debugger; on the recording thread they
 * are dropped (begin/end stay balanced since both are dropped). */
static VKAPI_ATTR void VKAPI_CALL w_label_ins(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l)
{
    if (!rec_here()) { RLOCK(R.vkCmdInsertDebugUtilsLabelEXT(cb, l)); return; }
}
static VKAPI_ATTR void VKAPI_CALL w_label_begin(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l)
{
    if (!rec_here()) { RLOCK(R.vkCmdBeginDebugUtilsLabelEXT(cb, l)); return; }
}
static VKAPI_ATTR void VKAPI_CALL w_label_end(VkCommandBuffer cb)
{
    if (!rec_here()) { RLOCK(R.vkCmdEndDebugUtilsLabelEXT(cb)); return; }
}

typedef struct { VkCommandBufferUsageFlags fl; } ABeg;
static VKAPI_ATTR VkResult VKAPI_CALL w_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    VkResult res;
    if (!rec_here()) { RLOCK(res = R.vkBeginCommandBuffer(cb, bi)); return res; }
    if (bi->pNext || bi->pInheritanceInfo) { drain(); s_direct++; RLOCK(res = R.vkBeginCommandBuffer(cb, bi)); return res; }
    PK_BEGIN(OP_BEGIN_CB, cb, A8(sizeof(ABeg)));
    ABeg a = { bi->flags }; pk_put(&pk_, &a, sizeof a);
    PK_END();
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_end_cb(VkCommandBuffer cb)
{
    if (!rec_here()) { VkResult res; RLOCK(res = R.vkEndCommandBuffer(cb)); return res; }
    PK_BEGIN(OP_END_CB, cb, 0);
    PK_END();
    return VK_SUCCESS;
}
typedef struct { VkCommandBufferResetFlags fl; } ARes;
static VKAPI_ATTR VkResult VKAPI_CALL w_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags fl)
{
    if (!rec_here()) { VkResult res; RLOCK(res = R.vkResetCommandBuffer(cb, fl)); return res; }
    PK_BEGIN(OP_RESET_CB, cb, A8(sizeof(ARes)));
    ARes a = { fl }; pk_put(&pk_, &a, sizeof a);
    PK_END();
    return VK_SUCCESS;
}

/* ---- sync points: everything recorded so far must exist first ----------- */
/* any thread may call these; the drain waits for what the pfifo thread has
 * queued up to now (a submit from another thread may carry its buffers).
 * Waits on fences, queries and idle only need the queued submits made. */
static inline void sync_point(void) { if (s_on) drain(); }
static inline void sync_submits(void)
{
    if (s_on && ld(&s_sub_q) != ld(&s_sub_done)) drain();
}
/* a fence only needs its own submit made */
#define PF_N 64
static VkFence s_pf[PF_N];
static void sync_fences(uint32_t n, const VkFence *f)
{
    if (!s_on) return;
    uint32_t q = ld(&s_sub_q), dn = ld(&s_sub_done);
    if (q == dn) return;
    if (q - dn >= PF_N) { drain(); return; }
    for (uint32_t i = dn; i != q; i++)
        for (uint32_t k = 0; k < n; k++)
            if (s_pf[i % PF_N] == f[k]) { drain(); return; }
}
/* nv2a-port: a status poll on a fence whose submit is still in the ring
 * can answer "not yet" without waiting for the worker (pgraph_vk_poll_slots
 * drained ~25 times a second, 2 ms each, on the pfifo thread).
 * NV2A_REC_POLL_DRAIN=1: the old drain. */
static bool fence_queued(VkFence f)
{
    static int old = -1;
    if (old < 0) old = getenv("NV2A_REC_POLL_DRAIN") != NULL;
    if (!s_on || old) return false;
    uint32_t q = ld(&s_sub_q), dn = ld(&s_sub_done);
    if (q == dn || q - dn >= PF_N) return false;
    for (uint32_t i = dn; i != q; i++)
        if (s_pf[i % PF_N] == f) return true;
    return false;
}
/* destroying a view or sampler a queued descriptor update still names */
static inline void sync_ds(void)
{
    if (s_on && ld(&s_ds_q) != ld(&s_ds_drained)) drain();
}

typedef struct { VkQueue q; VkFence f; uint32_t n; } ASub;
typedef struct { uint32_t nw, ncb, ns; } ASubInfo;
static VKAPI_ATTR VkResult VKAPI_CALL w_submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    if (rec_here() && s_async_submit) {
        bool odd = false; uint32_t bytes = A8(sizeof(ASub));
        for (uint32_t i = 0; i < n; i++) {
            odd |= si[i].pNext != NULL;
            bytes += A8(sizeof(ASubInfo)) + A8(si[i].waitSemaphoreCount * sizeof(VkSemaphore))
                   + A8(si[i].waitSemaphoreCount * sizeof(VkPipelineStageFlags))
                   + A8(si[i].commandBufferCount * sizeof(VkCommandBuffer))
                   + A8(si[i].signalSemaphoreCount * sizeof(VkSemaphore));
        }
        if (!odd) {
            /* queued behind the commands it submits; the caller goes on */
            PK_BEGIN(OP_SUBMIT, VK_NULL_HANDLE, bytes);
            ASub a = { q, f, n }; pk_put(&pk_, &a, sizeof a);
            for (uint32_t i = 0; i < n; i++) {
                ASubInfo b = { si[i].waitSemaphoreCount, si[i].commandBufferCount,
                               si[i].signalSemaphoreCount };
                pk_put(&pk_, &b, sizeof b);
                pk_put(&pk_, si[i].pWaitSemaphores, b.nw * sizeof(VkSemaphore));
                pk_put(&pk_, si[i].pWaitDstStageMask, b.nw * sizeof(VkPipelineStageFlags));
                pk_put(&pk_, si[i].pCommandBuffers, b.ncb * sizeof(VkCommandBuffer));
                pk_put(&pk_, si[i].pSignalSemaphores, b.ns * sizeof(VkSemaphore));
            }
            s_pf[s_sub_q % PF_N] = f;
            PK_END();
            __atomic_store_n(&s_sub_q, s_sub_q + 1, __ATOMIC_RELEASE);
            /* the GPU should start on it soon: wake the worker now */
            s_woke_at = s_head; wake_worker();
            return VK_SUCCESS;
        }
    }
    sync_point();
    pthread_mutex_lock(&s_qmx);
    VkResult r = R.vkQueueSubmit(q, n, si, f);
    pthread_mutex_unlock(&s_qmx);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_queue_idle(VkQueue q)
{
    sync_submits();
    pthread_mutex_lock(&s_qmx);
    VkResult r = R.vkQueueWaitIdle(q);
    pthread_mutex_unlock(&s_qmx);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_dev_idle(VkDevice d)
{
    sync_submits();
    pthread_mutex_lock(&s_qmx);
    VkResult r = R.vkDeviceWaitIdle(d);
    pthread_mutex_unlock(&s_qmx);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_wait_fences(VkDevice d, uint32_t n, const VkFence *f,
                                                    VkBool32 all, uint64_t to)
{
    if (to == 0) for (uint32_t i = 0; i < n; i++) {
        bool qd = fence_queued(f[i]);
        if (qd && (all || n == 1)) return VK_TIMEOUT;
    }
    sync_fences(n, f); return R.vkWaitForFences(d, n, f, all, to); }
static VKAPI_ATTR VkResult VKAPI_CALL w_fence_status(VkDevice d, VkFence f)
{ if (fence_queued(f)) return VK_NOT_READY; sync_fences(1, &f); return R.vkGetFenceStatus(d, f); }
static VKAPI_ATTR VkResult VKAPI_CALL w_reset_fences(VkDevice d, uint32_t n, const VkFence *f)
{ sync_fences(n, f); return R.vkResetFences(d, n, f); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_fence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{ sync_fences(1, &f); R.vkDestroyFence(d, f, a); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_sem(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{ sync_submits(); R.vkDestroySemaphore(d, s, a); }
static VKAPI_ATTR VkResult VKAPI_CALL w_alloc_cb(VkDevice d, const VkCommandBufferAllocateInfo *ai,
                                                 VkCommandBuffer *cb)
{ sync_point(); VkResult r; RLOCK(r = R.vkAllocateCommandBuffers(d, ai, cb)); return r; }
static VKAPI_ATTR void VKAPI_CALL w_free_cb(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *cb)
{ sync_point(); RLOCK(R.vkFreeCommandBuffers(d, p, n, cb)); }
static VKAPI_ATTR VkResult VKAPI_CALL w_reset_pool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags fl)
{ sync_point(); VkResult r; RLOCK(r = R.vkResetCommandPool(d, p, fl)); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL w_query_res(VkDevice d, VkQueryPool qp, uint32_t f, uint32_t n,
    size_t sz, void *data, VkDeviceSize st, VkQueryResultFlags fl)
{ sync_submits(); return R.vkGetQueryPoolResults(d, qp, f, n, sz, data, st, fl); }
/* nv2a-port: the present goes into the ring behind the submit it waits for,
 * so the pfifo thread neither drains (~1 ms, 30-40 a second) nor sits in the
 * driver's present. Its result arrives one present late, which the caller's
 * OUT_OF_DATE handling tolerates. NV2A_REC_SYNC_PRESENT=1: the old way. */
typedef struct { VkQueue q; uint32_t nw, ns; } APres;
static volatile int s_pres_res = VK_SUCCESS;
static VKAPI_ATTR VkResult VKAPI_CALL w_present(VkQueue q, const VkPresentInfoKHR *pi)
{
    static int sync = -1;
    if (sync < 0) sync = getenv("NV2A_REC_SYNC_PRESENT") != NULL;
    if (!sync && rec_here() && s_async_submit && !pi->pNext && !pi->pResults &&
        pi->swapchainCount <= 4 && pi->waitSemaphoreCount <= 8) {
        uint32_t bytes = A8(sizeof(APres)) + A8(pi->waitSemaphoreCount * sizeof(VkSemaphore)) +
                         A8(pi->swapchainCount * sizeof(VkSwapchainKHR)) +
                         A8(pi->swapchainCount * sizeof(uint32_t));
        PK_BEGIN(OP_PRESENT, VK_NULL_HANDLE, bytes);
        APres a = { q, pi->waitSemaphoreCount, pi->swapchainCount };
        pk_put(&pk_, &a, sizeof a);
        pk_put(&pk_, pi->pWaitSemaphores, a.nw * sizeof(VkSemaphore));
        pk_put(&pk_, pi->pSwapchains, a.ns * sizeof(VkSwapchainKHR));
        pk_put(&pk_, pi->pImageIndices, a.ns * sizeof(uint32_t));
        PK_END();
        s_woke_at = s_head; wake_worker();
        return (VkResult)__atomic_exchange_n(&s_pres_res, VK_SUCCESS, __ATOMIC_ACQ_REL);
    }
    sync_submits();
    pthread_mutex_lock(&s_qmx);
    VkResult r = R.vkQueuePresentKHR(q, pi);
    pthread_mutex_unlock(&s_qmx);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL w_destroy_swapchain(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a)
{ sync_point(); R.vkDestroySwapchainKHR(d, sc, a); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{ sync_point(); RLOCK(R.vkDestroyCommandPool(d, p, a)); }

/* descriptor updates: queued in order with the binds that follow them */
typedef struct { uint32_t nw, nc; } AUds;
typedef struct { VkDescriptorSet set; uint32_t binding, elem, count; VkDescriptorType type; uint32_t kind; } AWr;
static VKAPI_ATTR void VKAPI_CALL w_update_ds(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w,
                                              uint32_t nc, const VkCopyDescriptorSet *c)
{
    if (!rec_here() || !s_async_ds) { R.vkUpdateDescriptorSets(d, nw, w, nc, c); return; }
    bool odd = false; uint32_t bytes = A8(sizeof(VkDevice)) + A8(sizeof(AUds)) + A8(nc * sizeof *c);
    for (uint32_t i = 0; i < nw; i++) {
        odd |= w[i].pNext != NULL;
        bytes += A8(sizeof(AWr));
        if (w[i].pImageInfo && (w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))
            bytes += A8(w[i].descriptorCount * sizeof(VkDescriptorImageInfo));
        else if (w[i].pBufferInfo && (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))
            bytes += A8(w[i].descriptorCount * sizeof(VkDescriptorBufferInfo));
        else odd = true;   /* texel buffers, inline uniforms...: direct */
    }
    for (uint32_t i = 0; i < nc; i++) odd |= c[i].pNext != NULL;
    if (odd) { drain(); s_direct++; R.vkUpdateDescriptorSets(d, nw, w, nc, c); return; }
    PK_BEGIN(OP_UPDATE_DS, VK_NULL_HANDLE, bytes);
    pk_put(&pk_, &d, sizeof d);
    AUds a = { nw, nc }; pk_put(&pk_, &a, sizeof a);
    for (uint32_t i = 0; i < nw; i++) {
        bool img = w[i].pImageInfo && (w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
            w[i].descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
        AWr b = { w[i].dstSet, w[i].dstBinding, w[i].dstArrayElement, w[i].descriptorCount,
                  w[i].descriptorType, img ? 1u : 2u };
        pk_put(&pk_, &b, sizeof b);
        if (img) pk_put(&pk_, w[i].pImageInfo, b.count * sizeof(VkDescriptorImageInfo));
        else pk_put(&pk_, w[i].pBufferInfo, b.count * sizeof(VkDescriptorBufferInfo));
    }
    pk_put(&pk_, c, nc * sizeof *c);
    __atomic_store_n(&s_ds_q, s_ds_q + 1, __ATOMIC_RELEASE);
    PK_END();
}
static VKAPI_ATTR VkResult VKAPI_CALL w_free_ds(VkDevice d, VkDescriptorPool p, uint32_t n,
                                                const VkDescriptorSet *ds)
{ sync_point(); return R.vkFreeDescriptorSets(d, p, n, ds); }
static VKAPI_ATTR VkResult VKAPI_CALL w_reset_dpool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags fl)
{ sync_point(); return R.vkResetDescriptorPool(d, p, fl); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_dpool(VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a)
{ sync_point(); R.vkDestroyDescriptorPool(d, p, a); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{ sync_ds(); R.vkDestroyImageView(d, v, a); }
static VKAPI_ATTR void VKAPI_CALL w_destroy_sampler(VkDevice d, VkSampler sm, const VkAllocationCallbacks *a)
{ sync_ds(); R.vkDestroySampler(d, sm, a); }

/* ---- the worker ---------------------------------------------------------- */
static void replay(Hdr *h)
{
    uint8_t *p = (uint8_t *)h + ALIGN8(sizeof(Hdr));
    VkCommandBuffer cb = h->cb;
#define ARG(T) T *a = (T *)p; p += A8(sizeof(T))
#define ARR(T, n) (T *)p; p += A8((n) * sizeof(T))
    switch (h->op) {
    case OP_BARRIER: { ARG(ABar);
        VkMemoryBarrier *m = ARR(VkMemoryBarrier, a->nm);
        VkBufferMemoryBarrier *b = ARR(VkBufferMemoryBarrier, a->nb);
        VkImageMemoryBarrier *im = ARR(VkImageMemoryBarrier, a->ni);
        R.vkCmdPipelineBarrier(cb, a->s, a->d, a->f, a->nm, a->nm ? m : NULL,
                               a->nb, a->nb ? b : NULL, a->ni, a->ni ? im : NULL);
        break; }
    case OP_COPY_B2I: { ARG(AB2I); VkBufferImageCopy *r = ARR(VkBufferImageCopy, a->n);
        R.vkCmdCopyBufferToImage(cb, a->b, a->i, a->l, a->n, r); break; }
    case OP_COPY_I2B: { ARG(AI2B); VkBufferImageCopy *r = ARR(VkBufferImageCopy, a->n);
        R.vkCmdCopyImageToBuffer(cb, a->i, a->l, a->b, a->n, r); break; }
    case OP_DRAW: { ARG(ADraw); R.vkCmdDraw(cb, a->vc, a->ic, a->fv, a->fi); break; }
    case OP_DRAW_IDX: { ARG(ADrawI); R.vkCmdDrawIndexed(cb, a->ic, a->inst, a->fi, a->vo, a->fin); break; }
    case OP_BIND_PIPE: { ARG(APipe); R.vkCmdBindPipeline(cb, a->bp, a->p); break; }
    case OP_BLIT: { ARG(ABlit); VkImageBlit *r = ARR(VkImageBlit, a->n);
        R.vkCmdBlitImage(cb, a->si, a->sl, a->di, a->dl, a->n, r, a->f); break; }
    case OP_COPY_I: { ARG(ACopyI); VkImageCopy *r = ARR(VkImageCopy, a->n);
        R.vkCmdCopyImage(cb, a->si, a->sl, a->di, a->dl, a->n, r); break; }
    case OP_COPY_B: { ARG(ACopyB); VkBufferCopy *r = ARR(VkBufferCopy, a->n);
        R.vkCmdCopyBuffer(cb, a->s, a->d, a->n, r); break; }
    case OP_TIMESTAMP: { ARG(ATs); R.vkCmdWriteTimestamp(cb, a->st, a->qp, a->q); break; }
    case OP_RESET_QP: { ARG(AResQ); R.vkCmdResetQueryPool(cb, a->qp, a->f, a->n); break; }
    case OP_BEGIN_Q: { ARG(AQ); R.vkCmdBeginQuery(cb, a->qp, a->q, a->fl); break; }
    case OP_END_Q: { ARG(AQ); R.vkCmdEndQuery(cb, a->qp, a->q); break; }
    case OP_PUSH: { ARG(APush); R.vkCmdPushConstants(cb, a->l, a->st, a->off, a->sz, p); break; }
    case OP_BIND_IDX: { ARG(AIdx); R.vkCmdBindIndexBuffer(cb, a->b, a->o, a->t); break; }
    case OP_BIND_DS: { ARG(ADs); VkDescriptorSet *ds = ARR(VkDescriptorSet, a->n);
        uint32_t *dyn = ARR(uint32_t, a->nd);
        R.vkCmdBindDescriptorSets(cb, a->bp, a->l, a->first, a->n, ds, a->nd, a->nd ? dyn : NULL); break; }
    case OP_SCISSOR: { ARG(ARange); VkRect2D *r = ARR(VkRect2D, a->n);
        R.vkCmdSetScissor(cb, a->first, a->n, r); break; }
    case OP_VIEWPORT: { ARG(ARange); VkViewport *v = ARR(VkViewport, a->n);
        R.vkCmdSetViewport(cb, a->first, a->n, v); break; }
    case OP_BIND_VB: { ARG(AVb); VkBuffer *b = ARR(VkBuffer, a->n); VkDeviceSize *o = ARR(VkDeviceSize, a->n);
        R.vkCmdBindVertexBuffers(cb, a->first, a->n, b, o); break; }
    case OP_END_RP: R.vkCmdEndRenderPass(cb); break;
    case OP_BEGIN_RP: { ARG(ARp); VkClearValue *cv = ARR(VkClearValue, a->ncv);
        VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = a->rp, .framebuffer = a->fb, .renderArea = a->area,
            .clearValueCount = a->ncv, .pClearValues = a->ncv ? cv : NULL };
        R.vkCmdBeginRenderPass(cb, &bi, a->c); break; }
    case OP_BLEND_C: { ARG(ABc); R.vkCmdSetBlendConstants(cb, a->c); break; }
    case OP_DISPATCH: { ARG(ADisp); R.vkCmdDispatch(cb, a->x, a->y, a->z); break; }
    case OP_CLEAR_COLOR: { ARG(ACc); VkImageSubresourceRange *r = ARR(VkImageSubresourceRange, a->n);
        R.vkCmdClearColorImage(cb, a->i, a->l, &a->c, a->n, r); break; }
    case OP_CLEAR_DS: { ARG(ACds); VkImageSubresourceRange *r = ARR(VkImageSubresourceRange, a->n);
        R.vkCmdClearDepthStencilImage(cb, a->i, a->l, &a->c, a->n, r); break; }
    case OP_CLEAR_ATT: { ARG(ACa); VkClearAttachment *at = ARR(VkClearAttachment, a->na);
        VkClearRect *r = ARR(VkClearRect, a->nr);
        R.vkCmdClearAttachments(cb, a->na, at, a->nr, r); break; }
    case OP_LINE_W: { ARG(ALw); R.vkCmdSetLineWidth(cb, a->w); break; }
    case OP_DEPTH_BIAS: { ARG(ADb); R.vkCmdSetDepthBias(cb, a->c, a->cl, a->s); break; }
    case OP_BEGIN_CB: { ARG(ABeg);
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = a->fl };
        VkResult res = R.vkBeginCommandBuffer(cb, &bi);
        if (res != VK_SUCCESS) fprintf(stderr, "[REC] vkBeginCommandBuffer: %d\n", res);
        break; }
    case OP_END_CB: { VkResult res = R.vkEndCommandBuffer(cb);
        if (res != VK_SUCCESS) fprintf(stderr, "[REC] vkEndCommandBuffer: %d\n", res);
        break; }
    case OP_RESET_CB: { ARG(ARes); R.vkResetCommandBuffer(cb, a->fl); break; }
    case OP_SUBMIT: { ARG(ASub);
        VkSubmitInfo si[8]; uint32_t n = a->n < 8 ? a->n : 8;
        for (uint32_t i = 0; i < n; i++) {
            ASubInfo *b = (ASubInfo *)p; p += A8(sizeof(ASubInfo));
            VkSemaphore *ws = ARR(VkSemaphore, b->nw);
            VkPipelineStageFlags *st = ARR(VkPipelineStageFlags, b->nw);
            VkCommandBuffer *cbs = ARR(VkCommandBuffer, b->ncb);
            VkSemaphore *ss = ARR(VkSemaphore, b->ns);
            si[i] = (VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = b->nw, .pWaitSemaphores = b->nw ? ws : NULL,
                .pWaitDstStageMask = b->nw ? st : NULL,
                .commandBufferCount = b->ncb, .pCommandBuffers = b->ncb ? cbs : NULL,
                .signalSemaphoreCount = b->ns, .pSignalSemaphores = b->ns ? ss : NULL };
        }
        pthread_mutex_lock(&s_qmx);
        VkResult res = R.vkQueueSubmit(a->q, n, si, a->f);
        pthread_mutex_unlock(&s_qmx);
        if (res != VK_SUCCESS) fprintf(stderr, "[REC] vkQueueSubmit: %d\n", res);
        s_subs++;
        __atomic_store_n(&s_sub_done, s_sub_done + 1, __ATOMIC_RELEASE);
        break; }
    case OP_PRESENT: { ARG(APres);
        VkSemaphore *ws = ARR(VkSemaphore, a->nw);
        VkSwapchainKHR *sc = ARR(VkSwapchainKHR, a->ns);
        uint32_t *ix = ARR(uint32_t, a->ns);
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = a->nw, .pWaitSemaphores = a->nw ? ws : NULL,
            .swapchainCount = a->ns, .pSwapchains = sc, .pImageIndices = ix };
        pthread_mutex_lock(&s_qmx);
        VkResult res = R.vkQueuePresentKHR(a->q, &pi);
        pthread_mutex_unlock(&s_qmx);
        if (res != VK_SUCCESS) __atomic_store_n(&s_pres_res, (int)res, __ATOMIC_RELEASE);
        break; }
    case OP_UPDATE_DS: {
        VkDevice dev = *(VkDevice *)p; p += A8(sizeof(VkDevice));
        ARG(AUds);
        enum { MAXW = 32 };
        VkWriteDescriptorSet w[MAXW]; uint32_t nw = a->nw < MAXW ? a->nw : MAXW;
        for (uint32_t i = 0; i < a->nw; i++) {
            AWr *b = (AWr *)p; p += A8(sizeof(AWr));
            void *info = p;
            p += A8(b->count * (b->kind == 1 ? sizeof(VkDescriptorImageInfo) : sizeof(VkDescriptorBufferInfo)));
            if (i >= MAXW) continue;
            w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = b->set, .dstBinding = b->binding, .dstArrayElement = b->elem,
                .descriptorCount = b->count, .descriptorType = b->type,
                .pImageInfo = b->kind == 1 ? info : NULL,
                .pBufferInfo = b->kind == 2 ? info : NULL };
        }
        VkCopyDescriptorSet *c = ARR(VkCopyDescriptorSet, a->nc);
        if (a->nw > MAXW) fprintf(stderr, "[REC] %u descriptor writes, only %u kept\n", a->nw, MAXW);
        R.vkUpdateDescriptorSets(dev, nw, w, a->nc, a->nc ? c : NULL);
        break; }
    default:
        fprintf(stderr, "[REC] bad op %u size %u\n", h->op, h->size);
        abort();
    }
#undef ARG
#undef ARR
}

static void *worker(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "nv2a.rec", 0, 0, 0);
    for (;;) {
        uint32_t t = s_tail, h = ld(&s_head);
        if (t == h) {
            /* nothing: spin a little (the next command is usually
             * microseconds away), then sleep until woken */
            int got = 0;
            /* short: a busy-waiting core is heat, and on a Pixel 6 heat is
             * what ends in the 400 MHz clamp (the producer wakes us anyway) */
            for (int i = 0; i < 300; i++) {
                if (ld(&s_head) != t) { got = 1; break; }
                if (__atomic_load_n(&s_draining, __ATOMIC_ACQUIRE)) break;
                if ((i & 127) == 127) sched_yield();
            }
            if (got) continue;
            pthread_mutex_lock(&s_mx);
            if (__atomic_load_n(&s_draining, __ATOMIC_ACQUIRE)) pthread_cond_broadcast(&s_cv_done);
            __atomic_store_n(&s_sleeping, 1, __ATOMIC_SEQ_CST);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* sleeping store before the head load */
            while (ld(&s_head) == ld(&s_tail)) {
                if (__atomic_load_n(&s_draining, __ATOMIC_ACQUIRE)) pthread_cond_broadcast(&s_cv_done);
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 500000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&s_cv_work, &s_mx, &ts);
            }
            __atomic_store_n(&s_sleeping, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&s_mx);
            continue;
        }
        while (t != h) {
            pthread_mutex_lock(&s_rmx);
            for (int k = 0; k < 256 && t != h; k++) {
                Hdr *hd = (Hdr *)(s_ring + t % RING_SIZE);
                uint32_t sz = hd->size;
                if (hd->op != OP_WRAP) replay(hd);
                t += sz;
                __atomic_store_n(&s_tail, t, __ATOMIC_RELEASE);
            }
            pthread_mutex_unlock(&s_rmx);
        }
        {   static struct timespec t0; static uint64_t o0, d0, x0;
            struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
            if (!t0.tv_sec) t0 = tn;
            if (tn.tv_sec - t0.tv_sec >= 10) {
                uint64_t o = s_ops, dr = __atomic_load_n(&s_drains, __ATOMIC_RELAXED), x = s_direct;
                double sec = (tn.tv_sec - t0.tv_sec) + (tn.tv_nsec - t0.tv_nsec) / 1e9;
                fprintf(stderr, "[REC] %.0f commands/s, %.0f waits for the queue/s, %llu direct\n",
                        (o - o0) / sec, (dr - d0) / sec, (unsigned long long)(x - x0));
                drain_report(sec);
                t0 = tn; o0 = o; d0 = dr; x0 = x;
            } }
        if (__atomic_load_n(&s_draining, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&s_mx);
            pthread_cond_broadcast(&s_cv_done);
            pthread_mutex_unlock(&s_mx);
        }
    }
    return NULL;
}

/* ---- setup --------------------------------------------------------------- */
void nv2a_rec_install(void)
{
    if (getenv("NV2A_NO_REC")) {
        fprintf(stderr, "[REC] off (NV2A_NO_REC)\n");
        return;
    }
#define X(n) R.n = n; if (!R.n) { fprintf(stderr, "[REC] %s missing\n", #n); }
    REC_FUNCS(X)
#undef X
    /* the ones every path needs */
    if (!R.vkCmdDraw || !R.vkBeginCommandBuffer || !R.vkEndCommandBuffer ||
        !R.vkQueueSubmit || !R.vkWaitForFences) {
        fprintf(stderr, "[REC] off: entry points missing\n");
        return;
    }
    s_async_submit = !getenv("NV2A_REC_SYNC_SUBMIT");
    s_async_ds = !getenv("NV2A_REC_SYNC_DS");
    s_ring = aligned_alloc(64, RING_SIZE + 64);   /* slack: a wrap marker may sit in the last 8 bytes */
    if (!s_ring) return;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL)) { free(s_ring); s_ring = NULL; return; }
    pthread_detach(th);
#define SWAP(n, w) if (R.n) n = w
    SWAP(vkCmdPipelineBarrier, w_barrier); SWAP(vkCmdCopyBufferToImage, w_copy_b2i);
    SWAP(vkCmdDraw, w_draw); SWAP(vkCmdBindPipeline, w_bind_pipe);
    SWAP(vkCmdCopyImageToBuffer, w_copy_i2b); SWAP(vkCmdBlitImage, w_blit);
    SWAP(vkCmdWriteTimestamp, w_timestamp); SWAP(vkCmdResetQueryPool, w_reset_qp);
    SWAP(vkCmdPushConstants, w_push); SWAP(vkCmdDrawIndexed, w_draw_idx);
    SWAP(vkCmdBindIndexBuffer, w_bind_idx); SWAP(vkCmdBindDescriptorSets, w_bind_ds);
    SWAP(vkCmdSetScissor, w_scissor); SWAP(vkCmdEndRenderPass, w_end_rp);
    SWAP(vkCmdCopyImage, w_copy_i); SWAP(vkCmdCopyBuffer, w_copy_b);
    SWAP(vkCmdBeginRenderPass, w_begin_rp); SWAP(vkCmdSetViewport, w_viewport);
    SWAP(vkCmdSetBlendConstants, w_blend_c); SWAP(vkCmdDispatch, w_dispatch);
    SWAP(vkCmdClearColorImage, w_clear_color); SWAP(vkCmdClearAttachments, w_clear_att);
    SWAP(vkCmdSetLineWidth, w_line_w); SWAP(vkCmdSetDepthBias, w_depth_bias);
    SWAP(vkCmdEndQuery, w_end_q); SWAP(vkCmdClearDepthStencilImage, w_clear_ds);
    SWAP(vkCmdBindVertexBuffers, w_bind_vb); SWAP(vkCmdBeginQuery, w_begin_q);
    SWAP(vkCmdInsertDebugUtilsLabelEXT, w_label_ins);
    SWAP(vkCmdBeginDebugUtilsLabelEXT, w_label_begin);
    SWAP(vkCmdEndDebugUtilsLabelEXT, w_label_end);
    SWAP(vkBeginCommandBuffer, w_begin_cb); SWAP(vkEndCommandBuffer, w_end_cb);
    SWAP(vkResetCommandBuffer, w_reset_cb); SWAP(vkQueueSubmit, w_submit);
    SWAP(vkQueueWaitIdle, w_queue_idle); SWAP(vkDeviceWaitIdle, w_dev_idle);
    SWAP(vkWaitForFences, w_wait_fences); SWAP(vkGetFenceStatus, w_fence_status);
    SWAP(vkAllocateCommandBuffers, w_alloc_cb); SWAP(vkFreeCommandBuffers, w_free_cb);
    SWAP(vkResetCommandPool, w_reset_pool); SWAP(vkGetQueryPoolResults, w_query_res);
    SWAP(vkQueuePresentKHR, w_present); SWAP(vkDestroyCommandPool, w_destroy_pool);
    SWAP(vkUpdateDescriptorSets, w_update_ds); SWAP(vkFreeDescriptorSets, w_free_ds);
    SWAP(vkResetDescriptorPool, w_reset_dpool); SWAP(vkDestroyDescriptorPool, w_destroy_dpool);
    SWAP(vkResetFences, w_reset_fences); SWAP(vkDestroyImageView, w_destroy_view);
    SWAP(vkDestroySampler, w_destroy_sampler); SWAP(vkDestroyFence, w_destroy_fence);
    SWAP(vkDestroySemaphore, w_destroy_sem); SWAP(vkDestroySwapchainKHR, w_destroy_swapchain);
#undef SWAP
    s_on = true;
    fprintf(stderr, "[REC] command recording on its own thread (submits %s, descriptor updates %s)\n",
            s_async_submit ? "there too" : "direct", s_async_ds ? "there too" : "direct");
}

/* the pfifo thread calls this once at its start */
void nv2a_rec_thread_enter(void) { t_producer = 1; }

/* for the per-second report */
void nv2a_rec_stats(uint64_t *ops, uint64_t *drains, uint64_t *direct)
{
    *ops = s_ops; *drains = s_drains; *direct = s_direct;
}
