extern volatile double nv2a_frame_ms_ema, nv2a_gpu_ms_ema;
extern volatile unsigned nv2a_flips;
volatile int nv2a_capture_frames;   /* REC button: frames left to log */
volatile int nv2a_dump_all;          /* REC: dump every colour target */
unsigned int nv2a_frame_us_ring[256];   /* frame i ended at flip i: its time */
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

#if HAVE_EXTERNAL_MEMORY
#include "gloffscreen.h"
#endif

#if HAVE_EXTERNAL_MEMORY
static GloContext *g_gl_context;
#endif

static void early_context_init(void)
{
#if HAVE_EXTERNAL_MEMORY
    g_gl_context = glo_context_create();
#endif
}

static void pgraph_vk_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->vk_renderer_state = (PGRAPHVkState *)g_malloc0(sizeof(PGRAPHVkState));

#if HAVE_EXTERNAL_MEMORY
    glo_set_current(g_gl_context);
#endif

    pgraph_vk_debug_init();

    pgraph_vk_init_instance(pg, errp);
    if (*errp) {
        return;
    }

    pgraph_vk_init_command_buffers(pg);
    pgraph_vk_init_buffers(d);
    pgraph_vk_init_surfaces(pg);
    pgraph_vk_init_shaders(pg);
    pgraph_vk_init_pipelines(pg);
    pgraph_vk_init_textures(pg);
    pgraph_vk_init_reports(pg);
    pgraph_vk_init_compute(pg);
    pgraph_vk_init_display(pg);

    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                   memory_region_size(d->vram));

    pgraph_vk_determine_gpu_properties(d);
}

static void pgraph_vk_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finalize_display(pg);
    pgraph_vk_finalize_compute(pg);
    pgraph_vk_finalize_reports(pg);
    pgraph_vk_finalize_textures(pg);
    pgraph_vk_finalize_pipelines(pg);
    pgraph_vk_finalize_shaders(pg);
    pgraph_vk_finalize_surfaces(pg);
    pgraph_vk_finalize_buffers(d);
    pgraph_vk_finalize_command_buffers(pg);
    pgraph_vk_finalize_instance(pg);

    g_free(pg->vk_renderer_state);
    pg->vk_renderer_state = NULL;
}

static void pgraph_vk_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
    pgraph_vk_surface_flush(d);
    pgraph_vk_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    pgraph_vk_update_vertex_ram_buffer(&d->pgraph, 0, d->vram_ptr,
                                       memory_region_size(d->vram));
    for (int i = 0; i < 4; i++) {
        pg->texture_dirty[i] = true;
    }

    /* FIXME: Flush more? */

    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_vk_sync(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    pgraph_vk_render_display(pg);

    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_vk_process_pending(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&d->pgraph.sync_pending) ||
        qatomic_read(&d->pgraph.flush_pending)
    ) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&d->pgraph.lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_vk_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_vk_download_dirty_surfaces(d);
        }
        if (qatomic_read(&d->pgraph.sync_pending)) {
            pgraph_vk_sync(d);
        }
        if (qatomic_read(&d->pgraph.flush_pending)) {
            pgraph_vk_flush(d);
        }
        qemu_mutex_unlock(&d->pgraph.lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

volatile double nv2a_frame_ms_ema = 33.3, nv2a_gpu_ms_ema;   /* for the resolution governor */
volatile unsigned nv2a_flips;
static double fence0, flipw0, idle0;   /* [HITCH] baselines, zeroed with the accumulators */
static void nv2a_perf_frame(int64_t finish_us)
{
    static int on = -1;
    if (on < 0) on = getenv("CONKER_PROF") != NULL || getenv("NV2A_PERF") != NULL;
    /* the frame accounting (ADPF hints, frame-time averages for the
     * resolution governor, the flip count) runs always; only the reports
     * need NV2A_PERF */
    static int64_t t0, last, fin_sum, frame_max, frames;
    static long long sums[NV2A_PROF__COUNT];
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    if (!t0) { t0 = last = now; }
    int64_t ft = now - last; last = now;
    {   extern void nv2a_adpf_frame(int64_t frame_us, double gpu_ms_total);
        extern double nv2a_gpu_ms_acc;
        nv2a_adpf_frame(ft, nv2a_gpu_ms_acc);
        /* smoothed frame and GPU times for the resolution governor */
        static double gpu_prev;
        double g = nv2a_gpu_ms_acc - gpu_prev;
        gpu_prev = nv2a_gpu_ms_acc;
        if (g < 0) g = 0;
        if (ft > 0 && ft < 500000) {
            nv2a_frame_ms_ema += 0.1 * (ft / 1000.0 - nv2a_frame_ms_ema);
            nv2a_gpu_ms_ema += 0.1 * (g - nv2a_gpu_ms_ema);
        }
        {   extern unsigned int nv2a_frame_us_ring[256];
            nv2a_frame_us_ring[nv2a_flips & 255] = (uint32_t)ft; }
        {   extern unsigned nv2a_shad_n, nv2a_shad_all;
            static char sb[1024]; static int sn, sk;
            if (nv2a_shad_all && sn < 980)
                sn += snprintf(sb + sn, sizeof sb - sn, " %u/%u/%.0f", nv2a_shad_n, nv2a_shad_all, ft / 1000.0);
            if (nv2a_shad_all && ++sk >= 30) { fprintf(stderr, "[SHAD]%s\n", sb); sn = sk = 0; sb[0] = 0; }
            nv2a_shad_n = nv2a_shad_all = 0; }
        nv2a_flips++; }
    if (ft > frame_max) frame_max = ft;
    {   /* nv2a-port: a hitch (flip to flip over 45 ms) with what this frame
         * did that a smooth one does not -- the micro-freeze hunt */
        static unsigned pipes0;
        extern double nv2a_fence_ms_acc, nv2a_pfifo_flipwait_ms_acc, nv2a_pfifo_idle_ms_acc;
        extern double nv2a_pfifo_nopwait_ms_acc;
        static double nop0;
        if (nv2a_pfifo_nopwait_ms_acc < nop0) nop0 = 0;
        static int nhitch;   /* at most 30 lines per 5 s report */
        static int64_t hwin;   /* always on (cheap, and only on a hitch) */
        if (now - hwin > 5000000) { hwin = now; nhitch = 0; }
        extern double nv2a_texup_ms, nv2a_shader_gen_ms, nv2a_pipe_sync_ms;
        extern unsigned nv2a_texup_n, nv2a_shader_gen_n;
        static double tex0, sh0, ps0; static unsigned texn0, shn0;
        if (nv2a_texup_ms < tex0 || nv2a_texup_n < texn0) { tex0 = 0; texn0 = 0; }
        if (nv2a_shader_gen_ms < sh0 || nv2a_shader_gen_n < shn0) { sh0 = 0; shn0 = 0; }
        if (nv2a_pipe_sync_ms < ps0) ps0 = 0;
        if (ft > 45000 && nv2a_flips > 2 && nhitch < 30) {
            char why[1024]; int o = 0;
            for (int i = 0; i < NV2A_PROF__COUNT && o < 900; i++) {
                long long c = g_nv2a_stats.frame_working.counters[i];
                const char *nm = nv2a_profile_get_counter_name(i);
                if (c && (strstr(nm, "FINISH") || strstr(nm, "GEN") || strstr(nm, "CREATE")
                          || strstr(nm, "UPLOAD") || strstr(nm, "EVICT") || strstr(nm, "DOWN")))
                    o += snprintf(why + o, sizeof why - o, " %s=%lld", nm, c);
            }
            fprintf(stderr, "[HITCH] %.1f ms: pipelines +%u (%.1f ms), shaders %u (%.1f ms), tex up %u (%.1f ms), fence wait %.1f ms, flip wait %.1f ms, pfifo idle %.1f ms, nop/irq wait %.1f ms;%s\n",
                    ft / 1000.0, nv2a_pipelines_created - pipes0, nv2a_pipe_sync_ms - ps0,
                    nv2a_shader_gen_n - shn0, nv2a_shader_gen_ms - sh0,
                    nv2a_texup_n - texn0, nv2a_texup_ms - tex0,
                    nv2a_fence_ms_acc - fence0, nv2a_pfifo_flipwait_ms_acc - flipw0,
                    nv2a_pfifo_idle_ms_acc - idle0, nv2a_pfifo_nopwait_ms_acc - nop0, why);
            nhitch++;
        }
        tex0 = nv2a_texup_ms; texn0 = nv2a_texup_n; sh0 = nv2a_shader_gen_ms;
        shn0 = nv2a_shader_gen_n; ps0 = nv2a_pipe_sync_ms;
        pipes0 = nv2a_pipelines_created;
        fence0 = nv2a_fence_ms_acc; flipw0 = nv2a_pfifo_flipwait_ms_acc; idle0 = nv2a_pfifo_idle_ms_acc;
        nop0 = nv2a_pfifo_nopwait_ms_acc;
    }
    if (!on) return;
    fin_sum += finish_us; frames++;
    /* GPU clock at every flip (Mali DVFS): sum and histogram */
    static int gfd = -2; static double gsum; static unsigned gn, gmin = ~0u, gmax;
    if (gfd == -2) {
        gfd = open("/sys/devices/platform/1c500000.mali/cur_freq", O_RDONLY);
        if (gfd < 0) gfd = open("/sys/class/misc/mali0/device/cur_freq", O_RDONLY);
    }
    if (gfd >= 0) {
        char gb[32]; ssize_t gl = pread(gfd, gb, sizeof gb - 1, 0);
        if (gl > 0) { gb[gl] = 0; unsigned f = (unsigned)atol(gb); gsum += f; gn++;
                      if (f < gmin) gmin = f; if (f > gmax) gmax = f; }
    }
    for (int i = 0; i < NV2A_PROF__COUNT; i++)
        sums[i] += g_nv2a_stats.frame_working.counters[i];
    if (now - t0 >= 5000000) {
        char line[2048]; int o = 0;
        for (int i = 0; i < NV2A_PROF__COUNT && o < 1900; i++)
            if (sums[i] && frames)
                o += snprintf(line + o, sizeof line - o, " %s=%lld", nv2a_profile_get_counter_name(i),
                              sums[i] / frames);
        extern double nv2a_gpu_ms_acc, nv2a_fence_ms_acc;
        extern double nv2a_pfifo_idle_ms_acc, nv2a_pfifo_flipwait_ms_acc, nv2a_pfifo_nopwait_ms_acc;
        extern unsigned nv2a_pfifo_nopwaits;
        extern unsigned nv2a_user_reads;
        { extern void nv2a_sampler_report(void); nv2a_sampler_report(); }
        {   /* why the clock stays low: the DVFS limits and the temperatures */
            static const char *mf[] = { "max_freq", "min_freq", "scaling_max_freq", "scaling_min_freq",
                                        "hint_max_freq", "hint_min_freq", "utilization", "power_policy",
                                        "dvfs_governor", "available_frequencies" };
            char out[1024]; int oo = 0;
            for (unsigned i = 0; i < sizeof mf / sizeof *mf && oo < 900; i++) {
                char path[128], b[200];
                snprintf(path, sizeof path, "/sys/devices/platform/1c500000.mali/%s", mf[i]);
                int fd = open(path, O_RDONLY);
                if (fd < 0) continue;
                ssize_t l = read(fd, b, sizeof b - 1); close(fd);
                if (l <= 0) continue;
                b[l] = 0; b[strcspn(b, "\n")] = 0;
                oo += snprintf(out + oo, sizeof out - oo, " %s=%s", mf[i], b);
            }
            for (int z = 0; z < 24 && oo < 1000; z++) {
                char path[96], b[32], ty[32] = "";
                snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
                int fd = open(path, O_RDONLY);
                if (fd < 0) continue;
                ssize_t l = read(fd, b, sizeof b - 1); close(fd);
                if (l <= 0) continue;
                b[l] = 0;
                snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", z);
                fd = open(path, O_RDONLY);
                if (fd >= 0) { ssize_t tl = read(fd, ty, sizeof ty - 1); close(fd); if (tl > 0) ty[tl] = 0; ty[strcspn(ty, "\n")] = 0; }
                oo += snprintf(out + oo, sizeof out - oo, " %s=%ld", ty, atol(b) / 1000);
            }
            if (oo) fprintf(stderr, "[GPERF]   GPU dvfs:%s\n", out);
        }
        if (gn) {
            fprintf(stderr, "[GPERF]   GPU clock at flips: avg %.0f MHz (min %u max %u)\n",
                    gsum / gn / 1000.0, gmin / 1000, gmax / 1000);
            gsum = 0; gn = 0; gmin = ~0u; gmax = 0;
        }
        fprintf(stderr, "[GPERF] %.1f fps, frame max %.1f ms, flip-finish avg %.2f ms, GPU %.1f ms/frame, fence wait %.1f, pfifo idle %.1f, flip wait %.1f, nop/irq wait %.1f ms (%u) /frame, GET polls %u/frame; per frame:%s\n",
                frames * 1e6 / (now - t0), frame_max / 1000.0, fin_sum / 1000.0 / frames,
                nv2a_gpu_ms_acc / frames, nv2a_fence_ms_acc / frames,
                nv2a_pfifo_idle_ms_acc / frames, nv2a_pfifo_flipwait_ms_acc / frames,
                nv2a_pfifo_nopwait_ms_acc / frames, nv2a_pfifo_nopwaits / frames,
                (unsigned)(nv2a_user_reads / frames), line);
        nv2a_user_reads = 0;
        {
            unsigned spv_hits = 0, spv_miss = 0, sh_skip = 0, sh_comp = 0;
            unsigned pp_built = 0, pp_wait = 0;
            pgraph_vk_spv_cache_stats(&spv_hits, &spv_miss);
            extern void pgraph_vk_async_shader_stats(unsigned *skipped, unsigned *compiled);
            pgraph_vk_async_shader_stats(&sh_skip, &sh_comp);
            extern void pgraph_vk_async_pipe_stats(unsigned *built, unsigned *waited);
            pgraph_vk_async_pipe_stats(&pp_built, &pp_wait);
            extern unsigned nv2a_rp_loadclears, nv2a_dof_skipped, nv2a_pipelines_created;
            fprintf(stderr, "[GPERF]   passes opened by a clear (load op CLEAR): %.1f/frame\n",
                    (double)nv2a_rp_loadclears / frames);
            nv2a_rp_loadclears = 0;
            if (nv2a_dof_skipped)
                fprintf(stderr, "[GPERF]   soft-focus draws skipped: %.2f/frame\n",
                        (double)nv2a_dof_skipped / frames);
            nv2a_dof_skipped = 0;
            extern unsigned g_nv2a_guard_faults, g_nv2a_guard_forced, nv2a_guard_services;
            if (g_nv2a_guard_faults || nv2a_guard_services)
                fprintf(stderr, "[GPERF]   read guard: %.1f faults/frame (%.1f forced), %.1f services/frame\n",
                        (double)g_nv2a_guard_faults / frames, (double)g_nv2a_guard_forced / frames,
                        (double)nv2a_guard_services / frames);
            g_nv2a_guard_faults = g_nv2a_guard_forced = nv2a_guard_services = 0;
            extern unsigned nv2a_sem_deferred, nv2a_sem_deferred_waits;
            fprintf(stderr, "[GPERF]   pipelines: %u built in background, %u draws waited; semaphores deferred %u, idle waits %u\n",
                    pp_built, pp_wait, nv2a_sem_deferred, nv2a_sem_deferred_waits);
            {   extern double nv2a_pipe_sync_ms; extern unsigned nv2a_pipe_sync_n;
                if (nv2a_pipe_sync_n)
                    fprintf(stderr, "[GPERF]   pipelines built in line (cache): %u, %.1f ms each, %.0f ms in all\n",
                            nv2a_pipe_sync_n, nv2a_pipe_sync_ms / nv2a_pipe_sync_n, nv2a_pipe_sync_ms);
                nv2a_pipe_sync_ms = 0; nv2a_pipe_sync_n = 0;
                extern double nv2a_shader_gen_ms; extern unsigned nv2a_shader_gen_n;
                if (nv2a_shader_gen_n)
                    fprintf(stderr, "[GPERF]   shader sets made: %u, %.1f ms each, %.0f ms in all\n",
                            nv2a_shader_gen_n, nv2a_shader_gen_ms / nv2a_shader_gen_n, nv2a_shader_gen_ms);
                nv2a_shader_gen_ms = 0; nv2a_shader_gen_n = 0;
                extern double nv2a_texup_ms; extern unsigned nv2a_texup_n;
                if (nv2a_texup_n)
                    fprintf(stderr, "[GPERF]   texture uploads: %u, %.2f ms each, %.0f ms in all\n",
                            nv2a_texup_n, nv2a_texup_ms / nv2a_texup_n, nv2a_texup_ms);
                nv2a_texup_ms = 0; nv2a_texup_n = 0; }
            nv2a_sem_deferred = nv2a_sem_deferred_waits = 0;
            {   /* NV2A_GPROF: GPU time per render-target size */
                typedef struct { uint16_t w, h; double ms; unsigned n, draws; } GpAcc;
                extern GpAcc nv2a_gp_acc[32];
                char gl[1024]; int go = 0;
                for (int k = 0; k < 32 && nv2a_gp_acc[k].n && go < 900; k++)
                    go += snprintf(gl + go, sizeof gl - go, " %ux%u:%.2fms/%.1fp/%.0fd",
                                   nv2a_gp_acc[k].w, nv2a_gp_acc[k].h,
                                   nv2a_gp_acc[k].ms / frames, (double)nv2a_gp_acc[k].n / frames,
                                   (double)nv2a_gp_acc[k].draws / frames);
                if (go) fprintf(stderr, "[GPROF] per frame (size:GPU ms/passes/draws):%s\n", gl);
                memset(nv2a_gp_acc, 0, sizeof(GpAcc) * 32);
            }
            static unsigned pipes_rep0;
            fprintf(stderr, "[GPERF]   shaders: %u SPIR-V from disk, %u compiled (%u in background, %u draws waited); %u new pipelines\n",
                    spv_hits, spv_miss, sh_comp, sh_skip, nv2a_pipelines_created - pipes_rep0);
            pipes_rep0 = nv2a_pipelines_created;
        }
        {
            extern unsigned g_nv2a_ram_faults, g_nv2a_protect_calls;
            fprintf(stderr, "[GPERF]   RAM write faults %.1f/frame, re-protect runs %.1f/frame\n",
                    (double)g_nv2a_ram_faults / frames, (double)g_nv2a_protect_calls / frames);
            g_nv2a_ram_faults = g_nv2a_protect_calls = 0;
        }
        {
            extern void *nv2a_wait_site[16]; extern double nv2a_wait_site_ms[16];
            extern unsigned nv2a_wait_site_n[16];
            for (int k = 0; k < 16 && nv2a_wait_site[k]; k++) {
                if (nv2a_wait_site_n[k]) {
                    Dl_info di; const char *nm = "?"; long off = 0;
                    if (dladdr(nv2a_wait_site[k], &di) && di.dli_sname) {
                        nm = di.dli_sname; off = (char *)nv2a_wait_site[k] - (char *)di.dli_saddr;
                    } else if (dladdr(nv2a_wait_site[k], &di)) {
                        off = (char *)nv2a_wait_site[k] - (char *)di.dli_fbase;
                    }
                    fprintf(stderr, "[GPERF]   fence wait from %s+0x%lx: %.1f/frame, %.2f ms/frame\n",
                            nm, off, (double)nv2a_wait_site_n[k] / frames,
                            nv2a_wait_site_ms[k] / frames);
                }
                nv2a_wait_site_n[k] = 0; nv2a_wait_site_ms[k] = 0;
            }
        }
        {
            extern uint32_t nv2a_nop_param[8]; extern unsigned nv2a_nop_cnt[8];
            char nl[256]; int no = 0;
            for (int k = 0; k < 8 && nv2a_nop_cnt[k]; k++) {
                no += snprintf(nl + no, sizeof nl - no, " %08x:%.1f", nv2a_nop_param[k],
                               (double)nv2a_nop_cnt[k] / frames);
                nv2a_nop_cnt[k] = 0;
            }
            if (no) fprintf(stderr, "[GPERF]   NOP traps/frame by parameter:%s\n", nl);
        }
        nv2a_gpu_ms_acc = nv2a_fence_ms_acc = 0;
        nv2a_pfifo_idle_ms_acc = nv2a_pfifo_flipwait_ms_acc = nv2a_pfifo_nopwait_ms_acc = 0;
        fence0 = flipw0 = idle0 = 0;
        nv2a_pfifo_nopwaits = 0;
        { extern void nv2a_sites_print(unsigned); nv2a_sites_print((unsigned)frames); }
        { extern void nv2a_mutex_contend_report(double); nv2a_mutex_contend_report((double)frames); }
        { extern void nv2a_lockcount_report(double); nv2a_lockcount_report((double)frames); }
        { extern void xbox_stall_report(double); xbox_stall_report((double)frames); }
        {   extern unsigned nv2a_rep_zero, nv2a_rep_nonzero;
            if (nv2a_rep_zero || nv2a_rep_nonzero)
                fprintf(stderr, "[GPERF]   visibility reports: %u zero, %u non-zero\n",
                        nv2a_rep_zero, nv2a_rep_nonzero);
            nv2a_rep_zero = nv2a_rep_nonzero = 0; }
        t0 = now; fin_sum = frame_max = frames = 0;
        memset(sums, 0, sizeof sums);
    }
}

volatile unsigned nv2a_flip_seq;   /* every flip (nv2a_flips counts only with NV2A_PERF) */
/* nv2a-port: every 10 s, where the memory is (the game was killed by the
 * low-memory killer in the foreground) */
static void mem_report(PGRAPHVkState *r)
{
    static int64_t last;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ts.tv_sec - last < 10) return;
    last = ts.tv_sec;
    long rss_anon = 0, rss_file = 0, rss_shm = 0, swap = 0, avail = 0;
    char line[160];
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "RssAnon: %ld", &rss_anon); sscanf(line, "RssFile: %ld", &rss_file);
            sscanf(line, "RssShmem: %ld", &rss_shm); sscanf(line, "VmSwap: %ld", &swap);
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) { while (fgets(line, sizeof line, f)) sscanf(line, "MemAvailable: %ld", &avail); fclose(f); }
    VmaTotalStatistics st;
    vmaCalculateStatistics(r->allocator, &st);
    int ntex = 0, nsurf = 0;
    TextureBinding *tb; (void)tb;
    SurfaceBinding *sb;
    QTAILQ_FOREACH(sb, &r->surfaces, entry) nsurf++;
    for (int i = 0; i < 1024; i++) if (r->texture_cache_entries[i].image) ntex++;
    fprintf(stderr, "[MEM] anon %ld MB, swap %ld MB, file %ld MB, shmem %ld MB; vulkan %llu MB in %u blocks; textures %d, surfaces %d; system available %ld MB\n",
            rss_anon >> 10, swap >> 10, rss_file >> 10, rss_shm >> 10,
            (unsigned long long)(st.total.statistics.blockBytes >> 20), st.total.statistics.blockCount,
            ntex, nsurf, avail >> 10);
    {   /* nv2a-port: per second since the last line -- write faults on
         * watched guest pages (texture tracking), read-guard faults, and
         * render-target read-backs: a slowdown that builds up over a
         * session with the GPU idle */
        extern unsigned g_nv2a_ram_faults, g_nv2a_protect_calls, g_nv2a_guard_faults, nv2a_surf_dl_total;
        static unsigned f0, p0, g0, d0; static int64_t t0;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t t = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        if (t0 && t > t0) {
            double s = (t - t0) / 1000.0;
            fprintf(stderr, "[MEM] per second: page write faults %.0f, protects %.0f, guard faults %.0f, read-backs %.0f\n",
                    (g_nv2a_ram_faults - f0) / s, (g_nv2a_protect_calls - p0) / s,
                    (g_nv2a_guard_faults - g0) / s, (nv2a_surf_dl_total - d0) / s);
        }
        f0 = g_nv2a_ram_faults; p0 = g_nv2a_protect_calls; g0 = g_nv2a_guard_faults; d0 = nv2a_surf_dl_total; t0 = t;
    }
    /* nv2a-port: the phone's state -- a slowdown that builds up over a
     * session and goes after a pause is heat: the clocks come down */
    {
        long f[3] = {0, 0, 0};
        const int cpus[3] = {0, 4, 6};
        for (int k = 0; k < 3; k++) {
            char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpus[k]);
            FILE *fq = fopen(p, "r");
            if (fq) { if (fscanf(fq, "%ld", &f[k]) != 1) f[k] = 0; fclose(fq); }
        }
        static void *(*acq)(void); static int (*status)(void *); static float (*head)(void *, int);
        static void *mgr; static int tried;
        if (!tried) {
            tried = 1;
            void *h = dlopen("libandroid.so", RTLD_NOW);
            if (h) {
                acq = (void *(*)(void))dlsym(h, "AThermal_acquireManager");
                status = (int (*)(void *))dlsym(h, "AThermal_getCurrentThermalStatus");
                head = (float (*)(void *, int))dlsym(h, "AThermal_getThermalHeadroom");
                if (acq) mgr = acq();
            }
        }
        int ts_ = mgr && status ? status(mgr) : -1;
        /* the headroom comes from the thermal governor (conker main.c):
         * Android answers it at most once a second, a second caller got NaN */
        extern volatile float nv2a_thermal_headroom;
        float hr = nv2a_thermal_headroom;
        (void)head;
        extern volatile double nv2a_frame_ms_ema, nv2a_gpu_ms_ema;
        fprintf(stderr, "[HEAT] thermal status %d headroom %.2f; cpu MHz little %ld mid %ld big %ld; frame %.1f ms, GPU %.1f ms\n",
                ts_, hr, f[0] / 1000, f[1] / 1000, f[2] / 1000, nv2a_frame_ms_ema, nv2a_gpu_ms_ema);
    }
}

static void pgraph_vk_flip_stall(NV2AState *d)
{
    nv2a_flip_seq++;
    mem_report(d->pgraph.vk_renderer_state);
    { extern void pgraph_vk_prewarm_step(PGRAPHState *pg); pgraph_vk_prewarm_step(&d->pgraph); }
    struct timespec fs0, fs1; clock_gettime(CLOCK_MONOTONIC, &fs0);
    pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_FLIP_STALL);
    clock_gettime(CLOCK_MONOTONIC, &fs1);
    { extern bool nv2a_eager_after_wait; nv2a_eager_after_wait = true; }
    {   /* pipelines added to the disk cache in the background (at most 1/min) */
        extern unsigned nv2a_pipelines_created;
        if (nv2a_pipelines_created)
            pgraph_vk_pipeline_cache_maybe_save(d->pgraph.vk_renderer_state, false);
    }
    nv2a_perf_frame((fs1.tv_sec - fs0.tv_sec) * 1000000LL + (fs1.tv_nsec - fs0.tv_nsec) / 1000);
    {   /* nv2a-port frame log: NV2A_FRAMELOG=1 logs every draw of one frame
         * every 10 s from 60 s on (see nv2a_note_draw). */
        extern int nv2a_framelog_on;
        static double t0, next = 60.0;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (!t0) t0 = t;
        if (nv2a_framelog_on) {
            nv2a_framelog_on = 0;
            char tag[16]; snprintf(tag, sizeof tag, "t%03.0f", next - 10.0);
            if (getenv("NV2A_DRAWSTAT")) pgraph_vk_stat_all_surfaces(d, "end");
            if (getenv("NV2A_FRAMEDUMP")) pgraph_vk_dump_surfaces(d, tag);
            {   /* REC: the targets as the frame left them */
                extern volatile int nv2a_dump_all;
                extern int nv2a_framelog_seq;
                if (nv2a_dump_all) {
                    char ctag[24]; snprintf(ctag, sizeof ctag, "f%04d", nv2a_framelog_seq);
                    pgraph_vk_dump_surfaces(d, ctag);
                }
            }
            fprintf(stderr, "[FRAME] end\n");
        }
        extern volatile int nv2a_capture_frames;
        if (!nv2a_framelog_on && nv2a_capture_frames == 0) nv2a_dump_all = 0;
        if (!nv2a_framelog_on && nv2a_capture_frames > 0) {
            /* the REC button: the next frames logged in full */
            nv2a_capture_frames--;
            nv2a_framelog_on = 1;
            { extern int nv2a_framelog_seq; nv2a_framelog_seq++; }
            fprintf(stderr, "[FRAME] begin t=%.0f (capture)\n", t - t0);
        } else
        if (getenv("NV2A_FRAMELOG") && t - t0 >= next) {
            next += 10.0;
            nv2a_framelog_on = 1;
            { extern int nv2a_framelog_seq; nv2a_framelog_seq++; }
            fprintf(stderr, "[FRAME] begin t=%.0f\n", t - t0);
            if (getenv("NV2A_DRAWSTAT")) pgraph_vk_stat_all_surfaces(d, "begin");
        }
    }
    pgraph_vk_debug_frame_terminator();
}

static void pgraph_vk_pre_savevm_trigger(NV2AState *d)
{
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_vk_pre_shutdown_trigger(NV2AState *d)
{
    // qatomic_set(&d->pgraph.vk_renderer_state->shader_cache_writeback_pending, true);
    // qemu_event_reset(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);
}

static void pgraph_vk_pre_shutdown_wait(NV2AState *d)
{
    // qemu_event_wait(&d->pgraph.vk_renderer_state->shader_cache_writeback_complete);   
}

static int pgraph_vk_get_framebuffer_surface(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_vk_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    assert(surface->color);

    surface->frame_time = pg->frame_time;

#if HAVE_EXTERNAL_MEMORY
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);
    return r->display.gl_texture_id;
#else
    /* nv2a-port: render + present the display on the PFIFO thread. */
    qemu_event_reset(&d->pgraph.sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.sync_complete);
    (void)r;
    return 1;
#endif
}

static PGRAPHRenderer pgraph_vk_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_VULKAN,
    .name = "Vulkan",
    .ops = {
        .init = pgraph_vk_init,
        .early_context_init = early_context_init,
        .finalize = pgraph_vk_finalize,
        .clear_report_value = pgraph_vk_clear_report_value,
        .clear_surface = pgraph_vk_clear_surface,
        .draw_begin = pgraph_vk_draw_begin,
        .draw_end = pgraph_vk_draw_end,
        .flip_stall = pgraph_vk_flip_stall,
        .flush_draw = pgraph_vk_flush_draw,
        .get_report = pgraph_vk_get_report,
        .image_blit = pgraph_vk_image_blit,
        .pre_savevm_trigger = pgraph_vk_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_vk_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_vk_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_vk_pre_shutdown_wait,
        .process_pending = pgraph_vk_process_pending,
        .process_pending_reports = pgraph_vk_process_pending_reports,
        .surface_update = pgraph_vk_surface_update,
        .set_surface_scale_factor = pgraph_vk_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_vk_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_vk_get_framebuffer_surface,
        .get_gpu_properties = pgraph_vk_get_gpu_properties,
    }
};

/* nv2a-port: called explicitly; a constructor in a static library is
 * dropped by the linker when nothing else references the object. */
void nv2a_register_vk_renderer(void)
{
    pgraph_renderer_register(&pgraph_vk_renderer);
}

void pgraph_vk_check_memory_budget(PGRAPHState *pg)
{
#if 0 // FIXME
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties const *props;
    vmaGetMemoryProperties(r->allocator, &props);

    g_autofree VmaBudget *budgets = g_malloc_n(props->memoryHeapCount, sizeof(VmaBudget));
    vmaGetHeapBudgets(r->allocator, budgets);

    const float budget_threshold = 0.8;
    bool near_budget = false;

    for (int i = 0; i < props->memoryHeapCount; i++) {
        VmaBudget *b = &budgets[i];
        float use_to_budget_ratio =
            (double)b->statistics.allocationBytes / (double)b->budget;
        NV2A_VK_DPRINTF("Heap %d: used %lu/%lu MiB (%.2f%%)", i,
                        b->statistics.allocationBytes / (1024 * 1024),
                        b->budget / (1024 * 1024), use_to_budget_ratio * 100);
        near_budget |= use_to_budget_ratio > budget_threshold;
    }

    // If any heaps are near budget, free up some resources
    if (near_budget) {
        pgraph_vk_trim_texture_cache(pg);
    }
#endif

#if 0
    char *s;
    vmaBuildStatsString(r->allocator, &s, VK_TRUE);
    puts(s);
    vmaFreeStatsString(r->allocator, s);
#endif
}

/* nv2a-port: Android Dynamic Performance Framework. The pfifo and game
 * threads are put in one hint session with a 33.3 ms target and the real
 * frame time reported every flip, so the scheduler keeps the CPU clocks up
 * for them. CONKER_NO_ADPF=1: off. */
#include <sys/syscall.h>
int nv2a_hint_tids[8];
static int s_hint_ntids;
void nv2a_hint_register_thread(void)
{
    int tid = (int)syscall(SYS_gettid);
    for (int i = 0; i < s_hint_ntids; i++) if (nv2a_hint_tids[i] == tid) return;
    if (s_hint_ntids < 8) nv2a_hint_tids[s_hint_ntids++] = tid;
}

void nv2a_adpf_frame(int64_t frame_us, double gpu_ms_total)
{
    static int state;   /* 0 untried, 1 on, -1 off */
    static void *(*getManager)(void);
    static void *(*createSession)(void *, const int32_t *, size_t, int64_t);
    static int (*report)(void *, int64_t);
    static int (*report2)(void *, void *);
    static void *(*wd_create)(void);
    static void (*wd_start)(void *, int64_t);
    static void (*wd_total)(void *, int64_t);
    static void (*wd_cpu)(void *, int64_t);
    static void (*wd_gpu)(void *, int64_t);
    static int (*setThreads)(void *, const int32_t *, size_t);
    static void *sess, *wd;
    static int nt_set;
    static unsigned nframes;
    static double gpu_last;
    if (state < 0) return;
    if (state == 0) {
        state = -1;
        if (getenv("CONKER_NO_ADPF") || !s_hint_ntids) { if (!s_hint_ntids) state = 0; return; }
        void *h = dlopen("libandroid.so", RTLD_NOW);
        if (h) {
            getManager = dlsym(h, "APerformanceHint_getManager");
            createSession = dlsym(h, "APerformanceHint_createSession");
            report = dlsym(h, "APerformanceHint_reportActualWorkDuration");
            report2 = dlsym(h, "APerformanceHint_reportActualWorkDuration2");
            setThreads = dlsym(h, "APerformanceHint_setThreads");
            wd_create = dlsym(h, "AWorkDuration_create");
            wd_start = dlsym(h, "AWorkDuration_setWorkPeriodStartTimestampNanos");
            wd_total = dlsym(h, "AWorkDuration_setActualTotalDurationNanos");
            wd_cpu = dlsym(h, "AWorkDuration_setActualCpuDurationNanos");
            wd_gpu = dlsym(h, "AWorkDuration_setActualGpuDurationNanos");
        }
        if (!getManager || !createSession || !report) {
            fprintf(stderr, "[ADPF] not available\n");
            return;
        }
        int32_t tids[8]; memcpy(tids, nv2a_hint_tids, sizeof tids);
        sess = createSession(getManager(), tids, s_hint_ntids, 33333333LL);
        if (!sess) { fprintf(stderr, "[ADPF] session refused\n"); return; }
        nt_set = s_hint_ntids;
        state = 1;
        bool gpu = report2 && wd_create && wd_start && wd_total && wd_cpu && wd_gpu;
        if (gpu) wd = wd_create();
        fprintf(stderr, "[ADPF] hint session on %d threads, target 33.3 ms%s\n",
                s_hint_ntids, wd ? ", with GPU durations" : "");
    }
    if (nt_set != s_hint_ntids && setThreads) {   /* a thread registered later */
        int32_t tids[8]; memcpy(tids, nv2a_hint_tids, sizeof tids);
        if (setThreads(sess, tids, s_hint_ntids) == 0) nt_set = s_hint_ntids;
    }
    if (++nframes < 2 || frame_us <= 0 || frame_us > 1000000) {
        gpu_last = gpu_ms_total;
        return;
    }
    double g = gpu_ms_total - gpu_last;
    gpu_last = gpu_ms_total;
    if (g < 0) g = 0;
    if (wd) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        wd_start(wd, now - frame_us * 1000);
        wd_total(wd, frame_us * 1000);
        wd_cpu(wd, frame_us * 1000);
        wd_gpu(wd, (int64_t)(g * 1e6) > 0 ? (int64_t)(g * 1e6) : 1);
        if (report2(sess, wd) != 0) {
            fprintf(stderr, "[ADPF] report2 failed\n");
            wd = NULL;
        }
        return;
    }
    report(sess, frame_us * 1000);
}

volatile float nv2a_thermal_headroom = -1.0f;   /* 10 s forecast, from the governor */

/* render scale from the host side (the resolution governor) */
void nv2a_host_set_scale(unsigned scale)
{
    NV2AState *d = g_nv2a;
    if (!d) return;
    qemu_mutex_lock(&d->pgraph.renderer_lock);
    pgraph_vk_set_surface_scale_factor(d, scale);
    qemu_mutex_unlock(&d->pgraph.renderer_lock);
    fprintf(stderr, "[DRS] render scale now x%u\n", scale);
}
unsigned nv2a_host_get_scale(void)
{
    NV2AState *d = g_nv2a;
    return d ? d->pgraph.surface_scale_factor : 1;
}

/* nv2a-port: NV2A_SAMPLE=1 — a 1 kHz wall-clock sampler of the pfifo and
 * game threads. Each sample keeps pc, lr and four frame-pointer return
 * addresses; every 5 s the hottest sites (the first two addresses in our
 * own library) are printed as [SAMPLE] lines with library offsets, to be
 * symbolised offline with addr2line. */
#include <signal.h>
#include <ucontext.h>
#define SMP_N 8192
typedef struct { uintptr_t pc, lr, f[4]; unsigned frame; } SmpRec;
static SmpRec s_smp[2][SMP_N];
static volatile unsigned s_smp_w[2];
static volatile int s_smp_on;
static timer_t s_smp_timer[2];
static void smp_handler(int sig, siginfo_t *si, void *uc_)
{
    (void)sig;
    int slot = si->si_value.sival_int;
    if (slot < 0 || slot > 1) return;
    ucontext_t *uc = uc_;
    unsigned w = __atomic_fetch_add(&s_smp_w[slot], 1, __ATOMIC_RELAXED);
    SmpRec *r = &s_smp[slot][w % SMP_N];
    memset(r, 0, sizeof *r);
    r->frame = nv2a_flips;
#if defined(__aarch64__)
    r->pc = uc->uc_mcontext.pc;
    r->lr = uc->uc_mcontext.regs[30];
    uintptr_t fp = uc->uc_mcontext.regs[29];
    uintptr_t sp = uc->uc_mcontext.sp;
    for (int i = 0; i < 4; i++) {
        /* the sampled thread is this one: stay inside its stack */
        if (!fp || (fp & 7) || fp < sp || fp > sp + (8u << 20)) break;
        r->f[i] = ((uintptr_t *)fp)[1];
        uintptr_t nfp = ((uintptr_t *)fp)[0];
        if (nfp <= fp) break;
        fp = nfp;
    }
#else
    (void)uc;
#endif
}
void nv2a_sampler_register(int slot)
{
    static int on = -1;
    if (on < 0) on = getenv("NV2A_SAMPLE") != NULL;
    if (!on || slot < 0 || slot > 1) return;
    static int inst;
    if (!inst) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = smp_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGRTMIN + 3, &sa, NULL);
        inst = 1;
    }
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGRTMIN + 3;
    sev.sigev_value.sival_int = slot;
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_MONOTONIC, &sev, &s_smp_timer[slot]) != 0) {
        fprintf(stderr, "[SAMPLE] timer_create failed\n");
        return;
    }
    struct itimerspec its = { { 0, 1000000 }, { 0, 1000000 } };
    timer_settime(s_smp_timer[slot], 0, &its, NULL);
    s_smp_on = 1;
    fprintf(stderr, "[SAMPLE] sampling thread %d (%s)\n", slot, slot ? "game" : "pfifo");
}
static int smp_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    return (int)y[1] - (int)x[1];
}
void nv2a_sampler_report(void)
{
    if (!s_smp_on) return;
    static unsigned last[2];
    Dl_info me;
    if (!dladdr((void *)nv2a_sampler_report, &me)) return;
    uintptr_t lo = (uintptr_t)me.dli_fbase, hi = lo + (256u << 20);
    enum { H = 4096 };
    static uintptr_t k1[H], k2[H]; static unsigned kn[H];
    static unsigned top[H][2];
    for (int slot = 0; slot < 2; slot++) {
        unsigned w = s_smp_w[slot], n = w - last[slot];
        last[slot] = w;
        if (!n) continue;
        if (n > SMP_N) n = SMP_N;
      for (int pass = 0; pass < 2; pass++) {
        unsigned used = 0;
        memset(kn, 0, sizeof kn);
        for (unsigned i = w - n; i != w; i++) {
            SmpRec r = s_smp[slot][i % SMP_N];
            /* pass 1: only samples of frames that took over 45 ms (the
             * sample's frame ends at flip r.frame) */
            if (pass && (nv2a_flips - r.frame > 200 ||
                         nv2a_frame_us_ring[(r.frame + 1) & 255] < 45000))
                continue;
            used++;
            uintptr_t ch[6] = { r.pc, r.lr, r.f[0], r.f[1], r.f[2], r.f[3] };
            uintptr_t a1 = 0, a2 = 0;
            for (int j = 0; j < 6; j++) {
                if (ch[j] < lo || ch[j] >= hi) continue;
                if (!a1) a1 = ch[j];
                else if (ch[j] != a1) { a2 = ch[j]; break; }
            }
            if (!(r.pc >= lo && r.pc < hi)) a1 |= 1;   /* outside our code */
            unsigned h = (unsigned)((((a1 >> 2) ^ (a2 >> 3)) * 2654435761u) % H);
            for (int p = 0; p < H; p++, h = (h + 1) % H) {
                if (!kn[h]) { k1[h] = a1; k2[h] = a2; kn[h] = 1; break; }
                if (k1[h] == a1 && k2[h] == a2) { kn[h]++; break; }
            }
        }
        int m = 0;
        for (int h = 0; h < H; h++) if (kn[h]) { top[m][0] = h; top[m][1] = kn[h]; m++; }
        qsort(top, m, sizeof top[0], smp_cmp);
        if (!used) continue;
        fprintf(stderr, "[SAMPLE] %s%s: %u samples\n", slot ? "game" : "pfifo",
                pass ? "-hitch" : "", used);
        for (int i = 0; i < m && i < (pass ? 40 : 30); i++) {
            int h = top[i][0];
            uintptr_t a1 = k1[h] & ~(uintptr_t)1;
            fprintf(stderr, "[SAMPLE] %s%s %5.1f%% %s 0x%lx 0x%lx\n",
                    slot ? "game" : "pfifo", pass ? "-hitch" : "", 100.0 * top[i][1] / used,
                    (k1[h] & 1) ? "ext" : "own",
                    a1 ? (unsigned long)(a1 - lo) : 0ul,
                    k2[h] ? (unsigned long)(k2[h] - lo) : 0ul);
        }
      }
    }
}
