/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <sched.h>
#include <time.h>
#include "renderer.h"

void pgraph_vk_init_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    r->num_queries_in_flight = 0;
    r->max_queries_in_flight = 1024;
    r->new_query_needed = false;
    r->query_in_flight = false;
    r->zpass_pixel_count_result = 0;

    VkQueryPoolCreateInfo pool_create_info = (VkQueryPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_OCCLUSION,
        .queryCount = r->max_queries_in_flight,
    };
    for (int i = 0; i < 2; i++) {
        VK_CHECK(vkCreateQueryPool(r->device, &pool_create_info, NULL,
                                   &r->slot_qpool[i]));
        QSIMPLEQ_INIT(&r->slot_reports[i]);
        r->slot_nq[i] = 0;
    }
    r->query_pool = r->slot_qpool[r->slot];
}

void pgraph_vk_finalize_reports(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    for (int i = 0; i < 2; i++) {
        while ((report = QSIMPLEQ_FIRST(&r->slot_reports[i])) != NULL) {
            QSIMPLEQ_REMOVE_HEAD(&r->slot_reports[i], entry);
            g_free(report);
        }
        vkDestroyQueryPool(r->device, r->slot_qpool[i], NULL);
    }
}

void pgraph_vk_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueryReport *report = g_malloc(sizeof(QueryReport)); // FIXME: Pre-allocate
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

/* last real count per report offset (small direct-mapped table) */
static struct { uint32_t off, val; } s_rep_last[256];
unsigned nv2a_report_provisional;
unsigned nv2a_rep_zero, nv2a_rep_nonzero;
static uint32_t nv2a_report_last(uint32_t off)
{
    unsigned i = (off >> 4) & 255;
    return s_rep_last[i].off == off ? s_rep_last[i].val : 0;
}
static void nv2a_report_note(uint32_t off, uint32_t val)
{
    unsigned i = (off >> 4) & 255;
    s_rep_last[i].off = off; s_rep_last[i].val = val;
}

void pgraph_vk_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    /* nv2a-port: semaphores are released when parsed, so the title can read
     * a report before the frame carrying it has finished on the GPU (it is
     * written when the slot retires): it read the old value -- 0 at a scene
     * start -- took the character for occluded, and drew nothing until its
     * next visibility test (a camera cut). Write a provisional value now:
     * the last result for this report, or "visible" when there is none or
     * it was 0; the real count overwrites it later. A hidden object may be
     * drawn a frame too long; nothing visible goes missing.
     * NV2A_NO_PROVISIONAL_REPORT=1: old way. */
    static int prov = -1;
    if (prov < 0) prov = getenv("NV2A_NO_PROVISIONAL_REPORT") == NULL;
    if (prov) {
        uint32_t off = GET_MASK(parameter, NV097_GET_REPORT_OFFSET);
        uint32_t v = nv2a_report_last(off);
        if (!v) v = 4096;   /* a 64x64 patch: visible */
        pgraph_write_zpass_pixel_cnt_report(d, parameter, v);
        nv2a_report_provisional++;
    }
    {   extern int nv2a_framelog_on;
        if (nv2a_framelog_on)
            fprintf(stderr, "[FRAME] REPORT off=%x last=%u provisional=%d queries=%d\n",
                    GET_MASK(parameter, NV097_GET_REPORT_OFFSET),
                    nv2a_report_last(GET_MASK(parameter, NV097_GET_REPORT_OFFSET)), prov,
                    r->num_queries_in_flight);
    }

    QueryReport *report = g_malloc(sizeof(QueryReport)); // FIXME: Pre-allocate
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

static void process_reports(NV2AState *d, VkQueryPool pool, int *nq_io,
                            void *queue_head);

void pgraph_vk_process_pending_reports_internal(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    assert(!r->in_command_buffer);
    process_reports(d, r->query_pool, &r->num_queries_in_flight, &r->report_queue);
}

/* nv2a-port: the reports a frame slot carried, once its fence passed */
void pgraph_vk_process_slot_reports(NV2AState *d, int s)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (QSIMPLEQ_EMPTY(&r->slot_reports[s]) && !r->slot_nq[s]) return;
    process_reports(d, r->slot_qpool[s], &r->slot_nq[s], &r->slot_reports[s]);
}

typedef QSIMPLEQ_HEAD(, QueryReport) ReportQueue;

static void process_reports(NV2AState *d, VkQueryPool pool, int *nq_io,
                            void *queue_head)
{
    PGRAPHState *pg = &d->pgraph;
    ReportQueue *queue = (ReportQueue *)queue_head;
    int nq = *nq_io;

    NV2A_VK_DGROUP_BEGIN("Processing queries");

    // Fetch all query results
    g_autofree uint64_t *query_results = NULL;

    PGRAPHVkState *r = pg->vk_renderer_state;
    if (nq > 0) {
        size_t size_of_results = nq * sizeof(uint64_t);
        query_results = g_malloc_n(nq, sizeof(uint64_t)); // FIXME: Pre-allocate
        VkResult result;
        /* nv2a-port: bounded. A query that was never recorded or submitted
         * never becomes available, and Mali answers NOT_READY to a WAIT
         * read of it at once, every time: the pfifo thread spun here
         * forever with the picture frozen (Pixel 6, 0.7.14). The frame's
         * fence has signalled by now, so whatever is still unavailable
         * after a short while never will be: such a query counts as
         * visible (an object drawn rather than wrongly hidden). */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            result = vkGetQueryPoolResults(
                r->device, pool, 0, nq,
                size_of_results, query_results, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result != VK_NOT_READY) break;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 100) break;
            sched_yield();
        }
        if (result != VK_SUCCESS) {
            uint64_t *av = g_malloc_n((size_t)nq * 2, sizeof(uint64_t));
            VkResult r2 = vkGetQueryPoolResults(
                r->device, pool, 0, nq, (size_t)nq * 2 * sizeof(uint64_t), av,
                2 * sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            int missing = 0;
            for (int i = 0; i < nq; i++) {
                bool ok = (r2 == VK_SUCCESS || r2 == VK_NOT_READY) && av[i * 2 + 1];
                query_results[i] = ok ? av[i * 2] : 4096ull * pg->surface_scale_factor *
                                                    pg->surface_scale_factor;
                missing += !ok;
            }
            g_free(av);
            static unsigned nlog;
            if (nlog++ < 20)
                fprintf(stderr, "[REPORT] %d of %d visibility queries never became "
                                "available (result %d): counted as visible\n",
                        missing, nq, (int)result);
        }
    }

    // Write out queries
    int num_results_counted = 0;
    const int result_divisor =
        pg->surface_scale_factor * pg->surface_scale_factor;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(queue)) != NULL) {
        assert(report->query_count >= num_results_counted);
        assert(report->query_count <= nq);

        while (num_results_counted < report->query_count) {
            r->zpass_pixel_count_result +=
                query_results[num_results_counted++];
        }

        if (report->clear) {
            NV2A_VK_DPRINTF("Cleared");
            r->zpass_pixel_count_result = 0;
        } else {
            {   /* experiment: NV2A_REPORT_VISIBLE=1 -- every object visible */
                static int allvis = -1;
                if (allvis < 0) allvis = getenv("NV2A_REPORT_VISIBLE") != NULL;
                uint32_t v = r->zpass_pixel_count_result / result_divisor;
                if (allvis && v < 4096) v = 4096;
                pgraph_write_zpass_pixel_cnt_report(d, report->parameter, v);
            }
            nv2a_report_note(GET_MASK(report->parameter, NV097_GET_REPORT_OFFSET),
                             r->zpass_pixel_count_result / result_divisor);
            {   extern unsigned nv2a_rep_zero, nv2a_rep_nonzero;
                if (r->zpass_pixel_count_result) nv2a_rep_nonzero++; else nv2a_rep_zero++; }
            {   static int nlog;
                extern volatile int nv2a_capture_log_reports;
                if (nv2a_capture_log_reports > 0 && nlog++ < 4000)
                    fprintf(stderr, "[REPORT] off=%x result=%u\n",
                            GET_MASK(report->parameter, NV097_GET_REPORT_OFFSET),
                            (unsigned)(r->zpass_pixel_count_result / result_divisor));
            }
        }

        QSIMPLEQ_REMOVE_HEAD(queue, entry);
        g_free(report);
    }

    // Add remaining results
    while (num_results_counted < nq) {
        r->zpass_pixel_count_result += query_results[num_results_counted++];
    }

    *nq_io = 0;
    NV2A_VK_DGROUP_END();
}

void pgraph_vk_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (*dma_get == *dma_put && r->in_command_buffer &&
        !QSIMPLEQ_EMPTY(&r->report_queue)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_STALLED);
    } else if (*dma_get == *dma_put &&
               (!QSIMPLEQ_EMPTY(&r->slot_reports[0]) ||
                !QSIMPLEQ_EMPTY(&r->slot_reports[1]))) {
        /* the title waits for a report a frame in flight carries.
         * nv2a-port: not with provisional reports (pgraph_vk_get_report):
         * the title already has a value and the done mark, and the real
         * count lands when the frame retires. Waiting here held the pfifo
         * thread on the GPU's fence for 20-40 ms whenever the title paused
         * its command stream with a visibility test in flight -- 15 ms a
         * frame in the lava levels on a throttled phone. Retire what has
         * finished, without blocking. NV2A_REPORT_WAIT=1: wait, as before. */
        /* 0.7.27: back to waiting by default -- a freeze at a cutscene's
         * end (title in D3D's fence wait, GPU idle) came with the
         * non-blocking way, and the two are not proven unrelated.
         * NV2A_REPORT_NOWAIT=1: the non-blocking way. */
        static int wait = -1;
        if (wait < 0) wait = getenv("NV2A_REPORT_NOWAIT") == NULL ||
                             getenv("NV2A_NO_PROVISIONAL_REPORT") != NULL;
        if (wait) pgraph_vk_wait_deferred();
        else pgraph_vk_poll_slots();
    }
}

/* a frame in flight carries visibility reports (pfifo.c, idle wait) */
bool pgraph_vk_slot_reports_pending(void)
{
    NV2AState *d = g_nv2a;
    if (!d || !d->pgraph.vk_renderer_state) return false;
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    return !QSIMPLEQ_EMPTY(&r->slot_reports[0]) || !QSIMPLEQ_EMPTY(&r->slot_reports[1]);
}
