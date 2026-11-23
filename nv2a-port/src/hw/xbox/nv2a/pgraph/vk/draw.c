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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "renderer.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/glsl/common.h"
#include <math.h>
#include <time.h>
static void rc_flush(PGRAPHVkState *r);
#include <stdlib.h>
int nv2a_dbg_mode_now(void);

/* nv2a-port diagnostics: what the command buffer that keeps the GPU busy holds. */
int nv2a_vk_cb_draws, nv2a_vk_cb_gs_draws, nv2a_vk_cb_clears, nv2a_vk_cb_compute;
char nv2a_vk_last_draw[512];
/* nv2a-port: per-draw/per-submit diagnostics only with NV2A_DIAG (they cost
 * a string format per draw and an extra queue wait per submit). */
static int nv2a_diag(void)
{
    static int on = -1;
    if (on < 0) on = getenv("NV2A_DIAG") != NULL;
    return on;
}
static int s_rp_clear = -1, s_rp_clear_z = -1, s_rp_barrier = -1;
static int nv2a_flag(const char *n, int *c) { if (*c < 0) *c = getenv(n) != NULL; return *c; }
static const VkClearValue nv2a_rp_clear_values[2] = {
    { .color = { .float32 = { 0, 0, 0, 1 } } },
    { .depthStencil = { 1.0f, 0 } },
};
int nv2a_vk_cb_s2t, nv2a_vk_cb_texup, nv2a_vk_cb_surfup;
unsigned nv2a_vk_last_count;
static char nv2a_cb_draws_log[1024];
static int nv2a_cb_draws_len;
int nv2a_framelog_on;
/* nv2a-port perf: count (tag, call site) pairs; printed with [GPERF]. */
#include <dlfcn.h>
typedef struct { const char *tag; void *ra; unsigned w, h; unsigned n; } Nv2aSite;
static Nv2aSite s_sites[256];
static int s_nsites;
void nv2a_site_hit(const char *tag, void *ra, unsigned w, unsigned h)
{
    for (int i = 0; i < s_nsites; i++)
        if (s_sites[i].tag == tag && s_sites[i].ra == ra && s_sites[i].w == w && s_sites[i].h == h) {
            s_sites[i].n++; return;
        }
    if (s_nsites < 256) s_sites[s_nsites++] = (Nv2aSite){ tag, ra, w, h, 1 };
}
void nv2a_sites_print(unsigned frames)
{
    if (!frames) frames = 1;
    for (int i = 0; i < s_nsites; i++) {
        if (s_sites[i].n < frames / 4 + 1) continue;
        Dl_info di; const char *nm = "?"; unsigned long off = 0;
        if (dladdr(s_sites[i].ra, &di) && di.dli_sname) { nm = di.dli_sname; off = (unsigned long)((uintptr_t)s_sites[i].ra - (uintptr_t)di.dli_saddr); }
        fprintf(stderr, "[GPERF]   %s from %s+0x%lx %ux%u: %.1f/frame\n", s_sites[i].tag, nm, off,
                s_sites[i].w, s_sites[i].h, (double)s_sites[i].n / frames);
    }
    s_nsites = 0;
}
int nv2a_framelog_seq;
unsigned nv2a_shad_n, nv2a_shad_all;   /* NV2A_SHADSTAT: shadow-receiver draws per frame */
/* Conker's Chapters list (front-end): textures only that screen binds
 * (seen at these addresses in the 0.18 captures). The touch overlay shows
 * the up / down swipe hint there. */
volatile int64_t nv2a_chapters_seen_ms;
volatile int64_t nv2a_aim_seen_ms;
/* the screen box of the inline quad about to be drawn (set just before the
 * draw, used once): the crosshair sits at the middle of the picture, an
 * effect sprite of the same kind (the boss's electric sparks) does not */
static float s_aimq_bb[4]; static bool s_aimq_valid, s_aimq_known;
/* the front-end main menu's panel (512x203, format 0x19; the bar cutscene
 * before it doesn't draw it): the touch overlay's swipe hints wait for it */
volatile int64_t nv2a_frontmenu_seen_ms;
double nv2a_pipe_sync_ms; unsigned nv2a_pipe_sync_n;
static void nv2a_note_draw(PGRAPHState *pg, unsigned count, const char *kind)
{
    nv2a_vk_last_count = count;
    {   /* per frame: how often each menu texture is drawn. The Chapters list
         * draws the two row strips once each and the framed picture; the
         * Game 1-3 screens draw the strips 3-4 times (and the picture too). */
        extern volatile unsigned nv2a_flip_seq;
        static unsigned frame, cnt[4];
        if (frame != nv2a_flip_seq) {
            if (cnt[1] == 1 && cnt[2] == 1 && cnt[0] >= 1 && cnt[3] >= 2) {
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                nv2a_chapters_seen_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
            }
            frame = nv2a_flip_seq;
            cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
        }
        PGRAPHVkState *r = pg->vk_renderer_state;
        TextureBinding *t0 = r->texture_bindings[0];
        if (t0 && t0 != &r->dummy_texture) {
            uint32_t a = (uint32_t)t0->key.texture_vram_offset;
            unsigned w = t0->key.state.width, h = t0->key.state.height;
            int k = (a == 0x02528700 && w == 64 && h == 512) ? 0 :
                    (a == 0x027b1380 && w == 512 && h == 128) ? 1 :
                    (a == 0x027bbe80 && w == 512 && h == 64) ? 2 :
                    ((a == 0x024edb00 || a == 0x02670800) && w == 256 && h == 256) ? 3 : -1;
            if (k >= 0) cnt[k]++;
            if (a == 0x03353000 && w == 512 && h == 203) {
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                nv2a_frontmenu_seen_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
            }
            /* the aiming crosshair: a 64x64 sprite (format 15, alpha-tested
             * at 1) laid over the finished picture -- the touch overlay puts
             * the camera stick under the left thumb while it is up */
            SurfaceBinding *c = r->color_binding;
            /* a draw with no box known (not an inline quad) is judged as before */
            bool centred = !s_aimq_known;
            if (s_aimq_known && s_aimq_valid && c && c->width && c->height) {
                float qw = s_aimq_bb[1] - s_aimq_bb[0], qh = s_aimq_bb[3] - s_aimq_bb[2];
                float qx = (s_aimq_bb[0] + s_aimq_bb[1]) * 0.5f, qy = (s_aimq_bb[2] + s_aimq_bb[3]) * 0.5f;
                float W = 640.0f, H = 480.0f;   /* guest screen units */
                centred = qw > 12 && qw < 200 && qh > 12 && qh < 200 &&
                          fabsf(qx - W * 0.5f) < W * 0.12f && fabsf(qy - H * 0.5f) < H * 0.16f;
            }
            s_aimq_valid = s_aimq_known = false;
            if (w == 64 && h == 64 && (int)t0->key.state.color_format == 15 && kind[0] == 'S' && centred &&
                c && !c->swizzle && c->width >= 640 &&
                (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & 0xFFFF1FFFu) == (0x3e031601u & 0xFFFF1FFFu)) {
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                extern volatile int64_t nv2a_aim_seen_ms;
                nv2a_aim_seen_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
                static uint32_t logged[8]; static int nl;
                bool known = false;
                for (int i = 0; i < nl; i++) if (logged[i] == a) known = true;
                if (!known && nl < 8) { logged[nl++] = a; fprintf(stderr, "[AIM] crosshair sprite at %08x\n", a); }
            }
        }
    }
    if (nv2a_framelog_on) {
        PGRAPHVkState *r = pg->vk_renderer_state;
        SurfaceBinding *c = r->color_binding, *z = r->zeta_binding;
        TextureBinding *t0 = r->texture_bindings[0];
        ShaderBinding *sb = r->shader_binding;
        fprintf(stderr, "[FRAME] %s%u p%u col %08x %ux%u%s zeta %08x %ux%u vp %ux%u "
                "vsh %s%d tex0 %08x %ux%u f%d %s c0 %08x c1 %08x c2 %08x sr %08x\n",
                kind, count, pg->primitive_mode,
                c ? (unsigned)c->vram_addr : 0, c ? c->width : 0, c ? c->height : 0,
                c && c->swizzle ? "s" : "",
                z ? (unsigned)z->vram_addr : 0, z ? z->width : 0, z ? z->height : 0,
                pg->surface_binding_dim.width, pg->surface_binding_dim.height,
                sb && sb->state.vsh.is_fixed_function ? "ff" : "prog",
                sb ? sb->state.vsh.programmable.program_length : -1,
                t0 && t0 != &r->dummy_texture ? (unsigned)t0->key.texture_vram_offset : 0,
                t0 ? t0->key.state.width : 0, t0 ? t0->key.state.height : 0,
                t0 ? (int)t0->key.state.color_format : -1,
                pg->w1_attrs || pg->compressed_attrs ? "cmp" : "",
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2), pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER));
        char ab[256]; int n = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES && n < 230; i++) {
            VertexAttribute *a = &pg->vertex_attributes[i];
            if (a->count)
                n += snprintf(ab + n, sizeof ab - n, " v%d:f%d/c%d/s%d", i,
                              a->format, a->count, a->stride);
        }
        {   NV2AState *dd = container_of(pg, NV2AState, pgraph);
            char tb[200]; int tn = 0;
            for (int i = 0; i < 4; i++) {
                if (!(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) & NV_PGRAPH_TEXCTL0_0_ENABLE)) continue;
                uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
                hwaddr off = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4), len;
                uint8_t *base = (uint8_t *)nv_dma_map(dd, GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CONTEXT_DMA) ?
                                                      pg->dma_b : pg->dma_a, &len);
                if (base) tn += snprintf(tb + tn, sizeof tb - tn, " t%d=%08x%s", i,
                                         (unsigned)(base + off - dd->vram_ptr),
                                         pgraph_vk_surface_get(dd, (hwaddr)(base + off - dd->vram_ptr)) ? "(rt)" : "");
            }
            fprintf(stderr, "[FRAME]   texs%s\n", tn ? tb : " none");
            /* REC: an inline draw's textures as the guest holds them (the
             * striped sky is a skybox of 64x64 DXT1 faces drawn this way):
             * written once per address per capture, decoded offline. */
            const char *ddir = getenv("NV2A_DUMP_DIR");
            if (ddir) {
                /* each texture once per capture (not per frame), up to 512:
                 * the busiest ones were cut off by the old 64-per-frame cap */
                static uint32_t done_a[512]; static int done_n; static int64_t done_ms;
                extern int nv2a_framelog_seq; unsigned nv2a_capture_seq = (unsigned)nv2a_framelog_seq;
                {   struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                    int64_t now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
                    if (now - done_ms > 3000) done_n = 0;      /* a new capture */
                    done_ms = now; }
                for (int i = 0; i < 4; i++) {
                    if (!(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) & NV_PGRAPH_TEXCTL0_0_ENABLE)) continue;
                    TextureBinding *tb_i = r->texture_bindings[i];
                    if (!tb_i || tb_i == &r->dummy_texture) continue;
                    uint32_t a = (uint32_t)tb_i->key.texture_vram_offset;
                    unsigned w = tb_i->key.state.width, h = tb_i->key.state.height;
                    size_t len = tb_i->key.texture_length;
                    if (!len || len > 1u << 20 || a + len > memory_region_size(dd->vram)) continue;
                    bool seen = false;
                    for (int k = 0; k < done_n; k++) if (done_a[k] == a) seen = true;
                    if (seen || done_n >= 512) continue;
                    done_a[done_n++] = a;
                    char path[512];
                    snprintf(path, sizeof path, "%s/tex_f%04u_%08x_%ux%u_f%d_p%u.raw", ddir,
                             nv2a_capture_seq, a, w, h, (int)tb_i->key.state.color_format,
                             (unsigned)tb_i->key.state.pitch);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(dd->vram_ptr + a, 1, len, f); fclose(f); }
                }
                /* and the vertices themselves */
                if (pg->inline_array_length && pg->inline_array_length <= 256) {
                    char vb[1400]; int vn = 0;
                    const float *fv = (const float *)pg->inline_array;
                    for (unsigned k = 0; k < pg->inline_array_length && vn < 1300; k++)
                        vn += snprintf(vb + vn, sizeof vb - vn, " %g", fv[k]);
                    fprintf(stderr, "[FRAME]   inline data%s\n", vb);
                }
            }
        }
        fprintf(stderr, "[FRAME]   attrs%s cmp=%x blend=%08x ctl0=%08x setupraster=%08x combctl=%08x\n",
                ab, pg->compressed_attrs, pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL));
        {
            uint32_t zmin = pgraph_reg_r(pg, NV_PGRAPH_ZCLIPMIN), zmax = pgraph_reg_r(pg, NV_PGRAPH_ZCLIPMAX);
            fprintf(stderr, "[FRAME]   ctl1=%08x ctl2=%08x ctl3=%08x zclip=%g..%g zfmt=%d/%d wclip0=%08x,%08x wclip1=%08x,%08x shprog=%08x psdbg=%d zp=%d dc=%d\n",
                    pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1), pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                    pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3), *(float *)&zmin, *(float *)&zmax,
                    pg->surface_shape.zeta_format, pg->surface_shape.z_format,
                    pgraph_reg_r(pg, NV_PGRAPH_WINDOWCLIPX0), pgraph_reg_r(pg, NV_PGRAPH_WINDOWCLIPY0),
                    pgraph_reg_r(pg, NV_PGRAPH_WINDOWCLIPX0 + 4), pgraph_reg_r(pg, NV_PGRAPH_WINDOWCLIPY0 + 4),
                    pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG),
                    r->shader_binding ? r->shader_binding->state.psh.dbg : -1,
                    r->shader_binding ? r->shader_binding->state.psh.z_perspective : -1,
                    r->shader_binding ? r->shader_binding->state.psh.depth_clipping : -1);
        }
    }
    if (nv2a_framelog_on) {
        /* first draw of each vertex program in the frame: constants and
         * the first vertices' raw position attribute */
        static int seen_frame_tag; static uint8_t seen[256];
        extern int nv2a_framelog_seq;
        if (seen_frame_tag != nv2a_framelog_seq) { seen_frame_tag = nv2a_framelog_seq; memset(seen, 0, sizeof seen); }
        PGRAPHVkState *r = pg->vk_renderer_state;
        ShaderBinding *sb = r->shader_binding;
        int pl = sb ? (sb->state.vsh.is_fixed_function ? 255 : sb->state.vsh.programmable.program_length & 0xff) : 254;
        if (!seen[pl]) {
            seen[pl] = 1;
            int nz = 0;
            for (int i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++)
                nz += (pg->vsh_constants[i][0] | pg->vsh_constants[i][1] |
                       pg->vsh_constants[i][2] | pg->vsh_constants[i][3]) != 0;
            fprintf(stderr, "[VPROBE] prog%d constsNZ=%d\n", pl, nz);
            for (int i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i += 4) {
                char line[512]; int n = 0, any = 0;
                for (int k = i; k < i + 4; k++) {
                    float *f = (float *)pg->vsh_constants[k];
                    any |= (pg->vsh_constants[k][0] | pg->vsh_constants[k][1] | pg->vsh_constants[k][2] | pg->vsh_constants[k][3]) != 0;
                    n += snprintf(line + n, sizeof line - n, " c%d(%g %g %g %g)", k - 96, f[0], f[1], f[2], f[3]);
                }
                if (any) fprintf(stderr, "[VPROBE]  %s\n", line);
            }
            NV2AState *d = container_of(pg, NV2AState, pgraph);
            VertexAttribute *a = &pg->vertex_attributes[0];
            if (a->count && !a->inline_buffer_populated) {
                hwaddr dma_len;
                uint8_t *base = (uint8_t *)nv_dma_map(d, a->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a, &dma_len);
                if (a->offset < dma_len) {
                    uint8_t *v = base + a->offset;
                    for (int j = 0; j < 3; j++, v += a->stride) {
                        if (a->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F)
                            fprintf(stderr, "[VPROBE]  v%d f %g %g %g %g\n", j, ((float *)v)[0], ((float *)v)[1],
                                    a->count > 2 ? ((float *)v)[2] : 0.f, a->count > 3 ? ((float *)v)[3] : 0.f);
                        else
                            fprintf(stderr, "[VPROBE]  v%d fmt%d raw %02x%02x %02x%02x %02x%02x %02x%02x\n", j, a->format,
                                    v[1], v[0], v[3], v[2], v[5], v[4], v[7], v[6]);
                    }
                }
            }
        }
    }
    if (nv2a_framelog_on && pg->inline_elements_length &&
        pg->vertex_attributes[0].count == 3 &&
        pg->vertex_attributes[0].format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K) {
        /* nv2a-port: CPU evaluation of prog33's position transform for
         * every vertex of this draw (R3 = v0 * c13.xxxw; R4 = c0..c2 . R3;
         * clip = R4 dph c3..c6; screen = clip.xyz / w * c-38 + c-37). */
        PGRAPHVkState *r = pg->vk_renderer_state;
        ShaderBinding *sb = r->shader_binding;
        if (sb && !sb->state.vsh.is_fixed_function &&
            sb->state.vsh.programmable.program_length == 33) {
            NV2AState *d = container_of(pg, NV2AState, pgraph);
            VertexAttribute *a = &pg->vertex_attributes[0];
            hwaddr dma_len;
            uint8_t *base = (uint8_t *)nv_dma_map(d, a->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a, &dma_len);
            float (*c)[4] = (float (*)[4])pg->vsh_constants;
            int neg = 0, n = 0, on = 0;
            float wmin = 1e30f, wmax = -1e30f, sxmin = 1e30f, sxmax = -1e30f, symin = 1e30f, symax = -1e30f;
            for (unsigned k = 0; k < pg->inline_elements_length; k++) {
                hwaddr off = a->offset + (hwaddr)pg->inline_elements[k] * a->stride;
                if (off + 6 > dma_len) continue;
                int16_t *v = (int16_t *)(base + off);
                float R3[4] = { v[0] * c[109][0], v[1] * c[109][0], v[2] * c[109][0], c[109][3] };
                float R4[3];
                for (int i = 0; i < 3; i++)
                    R4[i] = R3[0]*c[96+i][0] + R3[1]*c[96+i][1] + R3[2]*c[96+i][2] + R3[3]*c[96+i][3];
                float cl[4];
                for (int i = 0; i < 4; i++)
                    cl[i] = R4[0]*c[99+i][0] + R4[1]*c[99+i][1] + R4[2]*c[99+i][2] + c[99+i][3];
                n++;
                if (cl[3] < 0) neg++;
                if (cl[3] < wmin) wmin = cl[3];
                if (cl[3] > wmax) wmax = cl[3];
                float sx = cl[0] / cl[3] * c[58][0] + c[59][0], sy = cl[1] / cl[3] * c[58][1] + c[59][1];
                if (sx < sxmin) sxmin = sx; if (sx > sxmax) sxmax = sx;
                if (sy < symin) symin = sy; if (sy > symax) symax = sy;
                if (cl[3] > 0 && sx >= 0 && sx < 640 && sy >= 0 && sy < 480) on++;
            }
            {
                extern volatile int g_conker_mat_ready __attribute__((weak));
                extern float g_conker_mat[12] __attribute__((weak));
                if (&g_conker_mat_ready && wmax < 0) {
                    memcpy(g_conker_mat, &c[96][0], 12 * sizeof(float));
                    g_conker_mat_ready = 1;
                }
            }
            fprintf(stderr, "[CPUVS] n=%d wneg=%d onscreen=%d w=[%g,%g] sx=[%g,%g] sy=[%g,%g] off=%llx stride=%u\n",
                    n, neg, on, wmin, wmax, sxmin, sxmax, symin, symax,
                    (unsigned long long)a->offset, a->stride);
        }
    }
    if (nv2a_diag() && nv2a_cb_draws_len < (int)sizeof nv2a_cb_draws_log - 40)
        nv2a_cb_draws_len += snprintf(nv2a_cb_draws_log + nv2a_cb_draws_len,
                                      sizeof nv2a_cb_draws_log - nv2a_cb_draws_len,
                                      " %s%u/p%u", kind, count, pg->primitive_mode);
}
static int nv2a_vk_no_gs = -1;
/* GS-free quads: each quad (q0 q1 q2 q3) as triangles (q3 q0 q1) (q3 q1 q2) --
 * same winding, and q3 leads both, as NV2A's quad provoking vertex. */
static bool nv2a_quads_as_tris(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    /* pg->primitive_mode, not the shader binding: that is chosen later, in
     * begin_pre_draw, and is NULL before the first draw. */
    return r->no_geometry_shader && pg->primitive_mode == PRIM_TYPE_QUADS;
}
static size_t nv2a_emit_quad_indices(uint32_t *out, const uint32_t *src,
                                     uint32_t base, uint32_t n)
{
    size_t k = 0;
    for (uint32_t i = 0; i + 3 < n; i += 4) {
        uint32_t q0 = src ? src[i] : base + i, q1 = src ? src[i + 1] : base + i + 1,
                 q2 = src ? src[i + 2] : base + i + 2, q3 = src ? src[i + 3] : base + i + 3;
        out[k++] = q3; out[k++] = q0; out[k++] = q1;
        out[k++] = q3; out[k++] = q1; out[k++] = q2;
    }
    return k;
}
static bool s_skip_this_draw;
static bool s_sky_skip_now;   /* sky A/B phase "not drawn" */   /* set per draw in begin_draw (experiments) */
static bool nv2a_skip_gs_draw(PGRAPHVkState *r)
{
    if (s_skip_this_draw) return true;
    if (nv2a_vk_no_gs < 0) {
        const char *e = getenv("NV2A_VK_NO_GS");
        nv2a_vk_no_gs = e && *e == '1';
    }
    if (nv2a_vk_no_gs && r->shader_binding && r->shader_binding->geom.module_info)
        return true;
    static int skip_small = -1;
    if (skip_small < 0) skip_small = getenv("NV2A_SKIP_SMALLRT") != NULL;
    if (skip_small && r->color_binding &&
        (r->color_binding->width != 640 || r->color_binding->height != 480))
        return true;   /* experiment: no off-screen passes (bloom etc.) */
    static int skip_post = -1;
    if (skip_post < 0) skip_post = getenv("NV2A_SKIP_POST") != NULL;
    int post_now = 0;
    if (skip_post) {   /* A/B: skipped for 5 s, drawn for 5 s */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        post_now = (ts.tv_sec / 5) & 1;
        static int last = -1;
        if (last != post_now) { last = post_now; fprintf(stderr, "[AB] skip_post=%d\n", post_now); }
    }
    if (post_now && r->color_binding && r->color_binding->width == 640 &&
        r->texture_bindings[0] && r->texture_bindings[0] != &r->dummy_texture &&
        r->texture_bindings[0]->key.state.width <= 256 &&
        r->texture_bindings[0]->key.state.width >= 64 &&
        r->shader_binding && !r->shader_binding->state.vsh.is_fixed_function &&
        r->shader_binding->state.vsh.programmable.program_length <= 3)
        return true;   /* experiment: no full-screen post composites (bloom) */
    static int skip_tex = -1;
    if (skip_tex < 0) skip_tex = getenv("NV2A_SKIP_TEX") != NULL;
    if (skip_tex)   /* experiment: draws that sample a texture are skipped */
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++)
            if (r->texture_bindings[i] && r->texture_bindings[i] != &r->dummy_texture &&
                r->texture_bindings[i]->key.state.width)
                return true;
    return false;
}

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int polygon_mode = r->shader_binding->state.geom.polygon_front_mode;
    int primitive_mode = r->shader_binding->state.geom.primitive_mode;

    // FIXME: Replace with LUT
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINE_LOOP:
        // FIXME: line strips, except that the first and last vertices are also used as a line
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        if (r->no_geometry_shader) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        /* Filled, a quad strip covers what a triangle strip over the same
         * vertices does. */
        if (r->no_geometry_shader) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME
        } else if (polygon_mode == POLY_MODE_FILL) {
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node,
                                      const void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
    snode->async_job = NULL;
}

/* nv2a-port: asynchronous pipeline creation. vkCreateGraphicsPipelines is
 * where Mali compiles the shaders to its ISA: ~60 ms a pipeline, and a new
 * area wants dozens at once (multi-second freezes). A pipeline not in the
 * cache is built by worker threads from a copy of its create info; draws
 * that need it wait up to NV2A_ASYNC_WAIT_MS and are dropped after that
 * until it is ready. NV2A_SYNC_PIPELINES=1 builds them in line. */
#include <pthread.h>
typedef struct AsyncPipeJob {
    VkPipelineShaderStageCreateInfo stages[3];
    ShaderModuleInfo *mods[3];
    int nmods;
    VkVertexInputBindingDescription vb[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription va[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[4];
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo ci;
    VkPipeline result;
    int known;   /* built in an earlier run: should be a cache hit */
    volatile int done;
    struct AsyncPipeJob *next;
} AsyncPipeJob;
static struct {
    int on;
    pthread_mutex_t lock;
    pthread_cond_t cond, done;
    AsyncPipeJob *head, *tail;
    PGRAPHVkState *r;
    unsigned waited, built;
} s_ap = { .on = -1, .lock = PTHREAD_MUTEX_INITIALIZER,
           .cond = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER };

static void *async_pipe_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_ap.lock);
        while (!s_ap.head) pthread_cond_wait(&s_ap.cond, &s_ap.lock);
        AsyncPipeJob *j = s_ap.head;
        s_ap.head = j->next;
        if (!s_ap.head) s_ap.tail = NULL;
        pthread_mutex_unlock(&s_ap.lock);

        VkPipeline p = VK_NULL_HANDLE;
        VkResult res = vkCreateGraphicsPipelines(s_ap.r->device, s_ap.r->vk_pipeline_cache,
                                                 1, &j->ci, NULL, &p);
        if (res != VK_SUCCESS) {
            fprintf(stderr, "[vk] async pipeline failed: %d\n", res);
            p = VK_NULL_HANDLE;
        }
        pthread_mutex_lock(&s_ap.lock);
        j->result = p;
        j->done = 1;
        s_ap.built++;
        pthread_cond_broadcast(&s_ap.done);
        pthread_mutex_unlock(&s_ap.lock);
    }
    return NULL;
}

static bool async_pipes_on(PGRAPHVkState *r)
{
    if (s_ap.on < 0) {
        s_ap.on = !getenv("NV2A_SYNC_PIPELINES");
        if (s_ap.on) {
            s_ap.r = r;
            int n = getenv("NV2A_PIPE_THREADS") ? atoi(getenv("NV2A_PIPE_THREADS")) : 4;
            for (int i = 0; i < n; i++) {
                pthread_t t;
                pthread_create(&t, NULL, async_pipe_thread, NULL);
                pthread_detach(t);
            }
            fprintf(stderr, "[nv2a] asynchronous pipeline creation on (%d threads)\n", n);
        }
    }
    return s_ap.on;
}

/* Take a finished job's pipeline into its node. Render thread only. */
static void async_pipe_adopt(PGRAPHVkState *r, PipelineBinding *snode)
{
    AsyncPipeJob *j = snode->async_job;
    snode->pipeline = j->result;
    for (int i = 0; i < j->nmods; i++) pgraph_vk_unref_shader_module(r, j->mods[i]);
    g_free(j);
    snode->async_job = NULL;
}

/* Whether snode's pipeline is ready, waiting a little for it. Called with
 * snode->async_job set. */
static bool async_pipe_ready(PGRAPHVkState *r, PipelineBinding *snode)
{
    /* nv2a-port: a draw whose pipeline is still building is dropped, and
     * characters popped in a moment late (scene starts, the bar). Wait up
     * to 8 ms for it, at most 12 ms a frame in all, before dropping. */
    static int wait_ms = -1, budget_ms = 12;
    if (wait_ms < 0) {
        const char *e = getenv("NV2A_ASYNC_WAIT_MS");
        wait_ms = e ? atoi(e) : 8;
        e = getenv("NV2A_ASYNC_BUDGET_MS");
        if (e) budget_ms = atoi(e);
    }
    extern volatile unsigned nv2a_flips;
    static unsigned budget_flip;
    static double spent_ms, spent_known_ms;
    if (budget_flip != nv2a_flips) { budget_flip = nv2a_flips; spent_ms = 0; spent_known_ms = 0; }
    AsyncPipeJob *j = snode->async_job;
    /* nv2a-port: pieces of a character drawn a moment apart (the crown, the
     * boots) in a cutscene. A pipeline from an earlier run is a cache hit
     * in well under a millisecond, so it is waited for (up to 60 ms each,
     * 150 ms a frame); and for 8 s after a level package opens -- the load
     * screen and the scene that follows it -- everything is waited for, as
     * the console had it all ready: a longer load, no pop-in. */
    extern volatile int64_t g_xbox_load_mark_ms __attribute__((weak));
    struct timespec nw; clock_gettime(CLOCK_MONOTONIC, &nw);
    int64_t now_ms = nw.tv_sec * 1000LL + nw.tv_nsec / 1000000;
    bool loading = &g_xbox_load_mark_ms && g_xbox_load_mark_ms &&
                   now_ms - g_xbox_load_mark_ms < 8000;
    int this_wait;
    if (loading) {
        this_wait = 400;
    } else if (j->known) {
        this_wait = 60;
        if (spent_known_ms + this_wait > 150) this_wait = (int)(150 - spent_known_ms);
    } else {
        this_wait = wait_ms;
        if (spent_ms + this_wait > budget_ms) this_wait = (int)(budget_ms - spent_ms);
    }
    struct timespec w0; clock_gettime(CLOCK_MONOTONIC, &w0);
    pthread_mutex_lock(&s_ap.lock);
    if (!j->done && this_wait > 0) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)this_wait * 1000000L;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (!j->done)
            if (pthread_cond_timedwait(&s_ap.done, &s_ap.lock, &dl)) break;
    }
    bool done = j->done;
    if (!done) s_ap.waited++;
    pthread_mutex_unlock(&s_ap.lock);
    {   struct timespec w1; clock_gettime(CLOCK_MONOTONIC, &w1);
        double d = (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6;
        if (j->known) spent_known_ms += d; else spent_ms += d; }
    if (done) async_pipe_adopt(r, snode);
    return done && snode->pipeline != VK_NULL_HANDLE;
}

void pgraph_vk_async_pipe_stats(unsigned *built, unsigned *waited)
{
    *built = s_ap.built; *waited = s_ap.waited;
    s_ap.built = s_ap.waited = 0;
}

static bool pipeline_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    /* nv2a-port: one still being built stays */
    if (snode->async_job && !snode->async_job->done) return false;
    /* nv2a-port: a frame in flight may still use it */
    return snode->draw_time < pgraph_vk_oldest_live_draw_time(r) &&
           snode != r->pipeline_binding;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->async_job) async_pipe_adopt(r, snode);   /* done, never used */
    assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node,
                                         const void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

/* nv2a-port: the driver's pipeline cache, kept in NV2A_CACHE_DIR between
 * runs. The file name carries vendor, device and driver version, and the
 * driver itself rejects data whose header (pipelineCacheUUID) is not its
 * own, so a cache from another GPU or driver costs nothing but a miss.
 * Written when pipelines were added, at most every 15 s, from the PFIFO
 * thread (pgraph_vk_pipeline_cache_maybe_save). NV2A_NO_PIPELINE_CACHE=1
 * turns it off. */
#include <sys/stat.h>
#include <unistd.h>
unsigned nv2a_pipelines_created;
static unsigned s_pipelines_saved_at;
static char s_pcache_path[1100];

static void pcache_path_init(PGRAPHVkState *r)
{
    const char *base = getenv("NV2A_CACHE_DIR");
    if (!base || !*base || getenv("NV2A_NO_PIPELINE_CACHE")) return;
    mkdir(base, 0775);
    snprintf(s_pcache_path, sizeof s_pcache_path, "%s/pipelines_%04x_%04x_%08x.bin",
             base, r->device_props.vendorID, r->device_props.deviceID,
             r->device_props.driverVersion);
}

typedef struct { void *data; size_t size; } PcacheWrite;
static void *pcache_write_thread(void *arg)
{
    PcacheWrite *w = arg;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_pcache_path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        size_t n = fwrite(w->data, 1, w->size, f);
        if (fclose(f) == 0 && n == w->size) rename(tmp, s_pcache_path);
        else unlink(tmp);
    }
    g_free(w->data);
    g_free(w);
    return NULL;
}

/* At most every 60 s (force: now), when pipelines were added. Only the
 * driver's serialization runs here; the file is written on a thread of its
 * own, so a save is not a hitch of its own. */
static PcacheWrite *pcache_serialize(PGRAPHVkState *r)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    size_t size = 0;
    if (vkGetPipelineCacheData(r->device, r->vk_pipeline_cache, &size, NULL) != VK_SUCCESS || !size)
        return NULL;
    PcacheWrite *w = g_malloc0(sizeof *w);
    w->data = g_malloc(size);
    w->size = size;
    if (vkGetPipelineCacheData(r->device, r->vk_pipeline_cache, &w->size, w->data) != VK_SUCCESS) {
        g_free(w->data); g_free(w);
        return NULL;
    }
    struct timespec te; clock_gettime(CLOCK_MONOTONIC, &te);
    fprintf(stderr, "[vk] pipeline cache: %zu KB serialized in %.1f ms, %u pipelines this run\n",
            w->size / 1024, (te.tv_sec - ts.tv_sec) * 1e3 + (te.tv_nsec - ts.tv_nsec) / 1e6,
            nv2a_pipelines_created);
    return w;
}

/* nv2a-port: the driver's serialization (~50 ms for 25 MB on Mali) ran on
 * the render thread -- a hitch a minute. Pipeline caches are internally
 * synchronized (the async builders use it at the same time already), so
 * the whole save runs on a thread of its own now. */
static void *pcache_save_thread(void *arg)
{
    static volatile int busy;
    PcacheWrite *w = pcache_serialize((PGRAPHVkState *)arg);
    if (w) pcache_write_thread(w);
    busy = 0;
    return NULL;
}

/* At most every 60 s (force: now), when pipelines were added. */
void pgraph_vk_pipeline_cache_maybe_save(PGRAPHVkState *r, bool force)
{
    static int64_t last;
    if (!r || !s_pcache_path[0] || nv2a_pipelines_created == s_pipelines_saved_at) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec;
    if (!last) last = now;
    if (!force && now - last < 60) return;
    last = now;
    s_pipelines_saved_at = nv2a_pipelines_created;
    if (force) {
        PcacheWrite *w = pcache_serialize(r);
        if (w) pcache_write_thread(w);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, pcache_save_thread, r) == 0) pthread_detach(t);
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    void *initial = NULL;
    size_t initial_size = 0;
    pcache_path_init(r);
    if (s_pcache_path[0]) {
        FILE *f = fopen(s_pcache_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 32 && n < 256L * 1024 * 1024) {
                initial = g_malloc((size_t)n);
                if (fread(initial, 1, (size_t)n, f) == (size_t)n) initial_size = (size_t)n;
                else { g_free(initial); initial = NULL; }
            }
            fclose(f);
        }
        fprintf(stderr, "[vk] pipeline cache %s: %zu KB loaded\n", s_pcache_path,
                initial_size / 1024);
        { extern void pgraph_vk_spv_preload(void); pgraph_vk_spv_preload(); }
    }

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = initial_size,
        .pInitialData = initial,
        .pNext = NULL,
    };
    if (vkCreatePipelineCache(r->device, &cache_info, NULL,
                              &r->vk_pipeline_cache) != VK_SUCCESS) {
        /* a driver may refuse data it does not like instead of ignoring it */
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = NULL;
        VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                       &r->vk_pipeline_cache));
    }
    g_free(initial);

    const size_t pipeline_cache_size = 4096;   /* nv2a-port: the boot warm-up builds every recorded pipeline */
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
    r->pipeline_cache.pre_node_evict = pipeline_cache_entry_pre_evict;
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_pipeline_cache_maybe_save(r, true);
    lru_flush(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    for (int i = 0; i < 2; i++) {
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &r->slot_sem[i]));
        VK_CHECK(vkCreateFence(r->device, &fence_info, NULL, &r->slot_fence[i]));
    }
    r->command_buffer_semaphore = r->slot_sem[0];
    r->command_buffer_fence = r->slot_fence[0];
    r->nslots = getenv("NV2A_ONE_SLOT") ? 1 : 2;
    r->slot = 0;
    r->descriptor_set_base = 0;
    r->descriptor_set_limit = r->nslots == 2 ? ARRAY_SIZE(r->descriptor_sets) / 2
                                             : ARRAY_SIZE(r->descriptor_sets);
    r->descriptor_set_index = 0;
    fprintf(stderr, "[vk] %d frame(s) in flight (NV2A_ONE_SLOT=1 for one)\n", r->nslots);
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    for (int i = 0; i < 2; i++) {
        vkDestroyFence(r->device, r->slot_fence[i], NULL);
        vkDestroySemaphore(r->device, r->slot_sem[i], NULL);
    }
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass_ex(PGRAPHVkState *r, RenderPassState *state, int clear_mask);
static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    return create_render_pass_ex(r, state, 0);
}
static VkRenderPass create_render_pass_ex(PGRAPHVkState *r, RenderPassState *state, int clear_mask)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = (nv2a_flag("NV2A_RP_CLEAR", &s_rp_clear) || (clear_mask & 1)) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = (nv2a_flag("NV2A_RP_CLEAR_Z", &s_rp_clear_z) || (clear_mask & 2)) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                  VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = (nv2a_flag("NV2A_RP_CLEAR_Z", &s_rp_clear_z) || (clear_mask & 2)) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                         VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    if (nv2a_flag("NV2A_RP_BARRIER", &s_rp_barrier)) {
        dependency.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        dependency.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    SurfaceBinding *binding = r->color_binding ? : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
}

__attribute__((unused)) static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    nv2a_pipelines_created++;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* nv2a-port: by value. A texture change alone does not touch the
     * pipeline (formats reach it through the shader state), and the game
     * rewrites these registers with the same values all the time: each
     * "dirty" draw built a multi-KB key and hashed it, ~230 times a frame.
     * Compare the values that go into the key instead.
     * NV2A_PIPE_DIRTY_OLD=1: the old test. */
    static int old_test = -1;
    if (old_test < 0) old_test = getenv("NV2A_PIPE_DIRTY_OLD") != NULL;
    if (!r->pipeline_binding || r->shader_bindings_changed ||
        (old_test && r->texture_bindings_changed) || check_render_pass_dirty(pg)) {
        return true;
    }

    const unsigned int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };

    if (old_test) {
        for (int i = 0; i < ARRAY_SIZE(regs); i++) {
            if (pgraph_is_reg_dirty(pg, regs[i])) {
                return true;
            }
        }
    } else {
        /* the values as init_pipeline_key puts them in the key */
        const uint32_t *kr = r->pipeline_binding->key.regs;
        for (int i = 0; i < ARRAY_SIZE(regs); i++) {
            uint32_t v = pgraph_reg_r(pg, regs[i]);
            if (i == 1 || i == 7 || i == 8) v = 0;
            if (i == 2) v &= ~NV_PGRAPH_CONTROL_0_ALPHAREF;
            if (v != kr[i]) return true;
        }
        if (pg->clearing != r->pipeline_binding->key.clear) return true;
        /* as many attributes as the pipeline was built with */
        int na = r->num_active_vertex_attribute_descriptions;
        int nb = r->num_active_vertex_binding_descriptions;
        static const uint8_t zero[sizeof(VkVertexInputAttributeDescription) > sizeof(VkVertexInputBindingDescription) ?
                                  sizeof(VkVertexInputAttributeDescription) : sizeof(VkVertexInputBindingDescription)];
        if (na < NV2A_VERTEXSHADER_ATTRIBUTES &&
            memcmp(&r->pipeline_binding->key.attribute_descriptions[na], zero, sizeof(VkVertexInputAttributeDescription)))
            return true;
        if (nb < NV2A_VERTEXSHADER_ATTRIBUTES &&
            memcmp(&r->pipeline_binding->key.binding_descriptions[nb], zero, sizeof(VkVertexInputBindingDescription)))
            return true;
    }

    // FIXME: Use dirty bits instead
    if (memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0]))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

/* nv2a-port: polygon offset through the rasteriser where the shader can't
 * write depth (Mali) */
static bool nv2a_hw_bias(PGRAPHVkState *r)
{
    static int off = -1;
    if (off < 0) off = getenv("NV2A_NO_HWBIAS") != NULL;
    return !off && NV2A_MOBILE_PATH(r);
}

static void set_hw_depth_bias(PGRAPHState *pg, PGRAPHVkState *r)
{
    float constant = 0.0f, slope = 0.0f;
    bool enabled = false;
    if (pg->primitive_mode >= PRIM_TYPE_TRIANGLES) {
        uint32_t raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
        uint32_t mode = GET_MASK(raster, NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
        enabled =
            (mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_FILL &&
             (raster & NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE)) ||
            (mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_LINE &&
             (raster & NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE)) ||
            (mode == NV_PGRAPH_SETUPRASTER_FRONTFACEMODE_POINT &&
             (raster & NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE));
    }
    if (enabled && r->zeta_binding) {
        uint32_t bu = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
        uint32_t fu = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
        float zbias, zfactor;
        memcpy(&zbias, &bu, 4); memcpy(&zfactor, &fu, 4);
        if (!isfinite(zbias)) zbias = 0; if (!isfinite(zfactor)) zfactor = 0;
        float cr[4];
        pgraph_glsl_set_clip_range_uniform_value(pg, cr);
        float zmax = cr[1] > 0 ? cr[1] : 16777215.0f;
        /* the vertex shader hands the rasteriser z / zmax; the bias unit r of
         * the host format: 1/65535 for D16, 2^-24 for D24 (and roughly so
         * for a D32F depth near 1) */
        float units = r->zeta_binding->host_fmt.vk_format == VK_FORMAT_D16_UNORM ?
                      65535.0f : 16777216.0f;
        constant = zbias / zmax * units;
        /* NV2A: factor * slope per unscaled pixel; Vulkan: per host pixel */
        slope = zfactor * (pg->surface_scale_factor ? pg->surface_scale_factor : 1);
        static int logged;
        if (logged < 12 && (zbias != 0 || zfactor != 0)) {
            logged++;
            fprintf(stderr, "[ZBIAS] bias %g factor %g zmax %g -> constant %g slope %g\n",
                    zbias, zfactor, zmax, constant, slope);
        }
    }
    /* Visibility tests: a box drawn around an object or a floor section,
     * depth-tested against the last frame's depth. Its faces lie on the
     * floor they bound; the NV2A computed both in the same arithmetic, the
     * box's own shader here comes out a hair deeper than the floor's, so the
     * test counted 0 pixels, the title skipped the section and the floor
     * had a hole (camera-angle dependent). Pull test draws toward the
     * camera a little: at worst a hidden object is drawn.
     * NV2A_NO_QUERY_BIAS=1: none. */
    static int qb = -1;
    if (qb < 0) qb = getenv("NV2A_NO_QUERY_BIAS") == NULL;
    if (qb && pg->zpass_pixel_count_enable) {
        constant -= 512.0f;
        slope -= 2.0f;
    }
    vkCmdSetDepthBias(r->command_buffer, constant, 0.0f, slope);
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, &key->render_pass_state);
    memcpy(&key->shader_state, &r->shader_binding->state, sizeof(ShaderState));
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

    // FIXME: Register masking
    // FIXME: Use more dynamic state updates
    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
    key->regs[1] = 0;   /* NV_PGRAPH_BLENDCOLOR: dynamic state */
    /* nv2a-port: values the shaders take as uniforms (alphaRef,
     * depthOffset), not pipeline state. In the key they made a new pipeline
     * for every alpha reference a fade went through: ~35 new pipelines a
     * frame in cutscenes (1961 of 1979 in one 5 s window), each a stall. */
    key->regs[2] &= ~NV_PGRAPH_CONTROL_0_ALPHAREF;
    key->regs[7] = 0;   /* NV_PGRAPH_ZOFFSETBIAS */
    key->regs[8] = 0;   /* NV_PGRAPH_ZOFFSETFACTOR */
}

/* nv2a-port: pipeline prewarm. Every pipeline the game needs is recorded
 * (its key and fixed-function state) with the level it was first seen in:
 * pipekeys.bin in the cache dir, plus pipekeys_ship.bin from the APK. When
 * a level starts loading, its pipelines are built in the background while
 * the loading screen is up -- as the console's own load would have them
 * ready -- instead of on their first frame (objects missing for a moment,
 * hitches). NV2A_NO_PREWARM=1: off. */
#define PREWARM_MAGIC 0x31524b50u   /* "PKR1" */
typedef struct PrewarmRec {
    uint32_t magic, size;
    char level[48];
    PipelineKey key;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[5];
    uint32_t ndyn, nvb, nva, push_size, has_ds;
} PrewarmRec;
extern char g_xbox_level_name[48];
extern volatile unsigned g_xbox_level_seq;
extern volatile int64_t g_xbox_load_mark_ms;
static FILE *s_pw_out, *s_pw_dump;
static uint64_t *s_pw_seen; static size_t s_pw_seen_n, s_pw_seen_cap;
static bool s_pw_seen_loaded;
static PrewarmRec *s_pw_list; static size_t s_pw_n, s_pw_pos;
static unsigned s_pw_seq, s_pw_built, s_pw_skipped;
static bool s_bw_mode;   /* building the boot warm-up */
static int64_t s_pw_t0;
unsigned nv2a_prewarm_recorded;
static bool prewarm_off(void)
{
    static int off = -1;
    if (off < 0) off = getenv("NV2A_NO_PREWARM") != NULL || !getenv("NV2A_CACHE_DIR");
    return off;
}
static void pw_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", getenv("NV2A_CACHE_DIR"), name);
}
static bool pw_seen(uint64_t h, bool add)
{
    for (size_t i = 0; i < s_pw_seen_n; i++) if (s_pw_seen[i] == h) return true;
    if (add) {
        if (s_pw_seen_n == s_pw_seen_cap) {
            s_pw_seen_cap = s_pw_seen_cap ? s_pw_seen_cap * 2 : 1024;
            s_pw_seen = g_realloc(s_pw_seen, s_pw_seen_cap * sizeof(uint64_t));
        }
        s_pw_seen[s_pw_seen_n++] = h;
    }
    return false;
}
/* every record of both files, through fn */
static void pw_scan(void (*fn)(PrewarmRec *rec, void *arg), void *arg)
{
    const char *names[2] = { "pipekeys_ship.bin", "pipekeys.bin" };
    PrewarmRec *rec = g_malloc(sizeof(*rec));
    for (int f = 0; f < 2; f++) {
        char p[1200]; pw_path(p, sizeof p, names[f]);
        FILE *fp = fopen(p, "rb");
        if (!fp) continue;
        while (fread(rec, sizeof(*rec), 1, fp) == 1) {
            if (rec->magic != PREWARM_MAGIC || rec->size != sizeof(*rec)) break;
            fn(rec, arg);
        }
        fclose(fp);
    }
    g_free(rec);
}
static void pw_note_seen(PrewarmRec *rec, void *arg)
{
    (void)arg;
    pw_seen(fast_hash((void *)&rec->key, sizeof rec->key), true);
}
static void prewarm_record(uint64_t hash, PrewarmRec *pr)
{
    if (prewarm_off()) return;
    if (!s_pw_seen_loaded) { s_pw_seen_loaded = true; pw_scan(pw_note_seen, NULL); }
    if (pw_seen(hash, true)) return;
    if (!s_pw_out) {
        char p[1200]; pw_path(p, sizeof p, "pipekeys.bin");
        s_pw_out = fopen(p, "ab");
        if (!s_pw_out) return;
    }
    pr->magic = PREWARM_MAGIC; pr->size = sizeof(*pr);
    snprintf(pr->level, sizeof pr->level, "%s", g_xbox_level_name);
    fwrite(pr, sizeof(*pr), 1, s_pw_out);
    fflush(s_pw_out);
    /* a copy in the dump folder too, which every pull fetches (the whole
     * record was copied there at start, conker main.c) */
    if (!s_pw_dump && getenv("NV2A_DUMP_DIR")) {
        char p[1200]; snprintf(p, sizeof p, "%s/pipekeys.bin", getenv("NV2A_DUMP_DIR"));
        s_pw_dump = fopen(p, "ab");
    }
    if (s_pw_dump) { fwrite(pr, sizeof(*pr), 1, s_pw_dump); fflush(s_pw_dump); }
    nv2a_prewarm_recorded++;
}
static void pw_collect(PrewarmRec *rec, void *arg)
{
    const char *lvl = arg;
    if (strcmp(rec->level, lvl)) return;
    uint64_t h = fast_hash((void *)&rec->key, sizeof rec->key);
    for (size_t i = 0; i < s_pw_n; i++)   /* both files may hold it */
        if (!memcmp(&s_pw_list[i].key, &rec->key, sizeof rec->key)) return;
    (void)h;
    s_pw_list = g_realloc(s_pw_list, (s_pw_n + 1) * sizeof(PrewarmRec));
    s_pw_list[s_pw_n++] = *rec;
}
static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state);
static bool async_pipes_on(PGRAPHVkState *r);
static void prewarm_build(PGRAPHState *pg, PrewarmRec *rec)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    uint64_t hash = fast_hash((void *)&rec->key, sizeof rec->key);
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &rec->key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->pipeline != VK_NULL_HANDLE || snode->async_job) { s_pw_skipped++; return; }
    if (rec->ndyn > 4 || rec->nvb > NV2A_VERTEXSHADER_ATTRIBUTES ||
        rec->nva > NV2A_VERTEXSHADER_ATTRIBUTES) { s_pw_skipped++; return; }

    extern ShaderBinding *pgraph_vk_shader_binding_for_state(PGRAPHVkState *r, const ShaderState *state);
    ShaderBinding *sb = pgraph_vk_shader_binding_for_state(r, &rec->key.shader_state);
    if (!sb || !sb->vsh.module_info || !sb->psh.module_info) { s_pw_skipped++; return; }

    memcpy(&snode->key, &rec->key, sizeof(PipelineKey));

    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0,
                                .size = rec->push_size };
    if (rec->push_size) { pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; }
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pli, NULL, &layout));

    AsyncPipeJob *j = g_malloc0(sizeof(*j));
    j->known = 1;
    int ns = 0;
    j->stages[ns++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = sb->vsh.module_info->module, .pName = "main" };
    if (sb->geom.module_info)
        j->stages[ns++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = sb->geom.module_info->module, .pName = "main" };
    j->stages[ns++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = sb->psh.module_info->module, .pName = "main" };
    ShaderModuleInfo *mods[3] = { sb->vsh.module_info, sb->geom.module_info, sb->psh.module_info };
    for (int i = 0; i < 3; i++)
        if (mods[i]) { pgraph_vk_ref_shader_module(mods[i]); j->mods[j->nmods++] = mods[i]; }
    memcpy(j->vb, rec->key.binding_descriptions, sizeof(j->vb[0]) * rec->nvb);
    memcpy(j->va, rec->key.attribute_descriptions, sizeof(j->va[0]) * rec->nva);
    j->vi = (VkPipelineVertexInputStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = rec->nvb, .pVertexBindingDescriptions = j->vb,
        .vertexAttributeDescriptionCount = rec->nva, .pVertexAttributeDescriptions = j->va };
    j->ia = rec->ia; j->ia.pNext = NULL;
    j->vp = (VkPipelineViewportStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    j->rs = rec->rs; j->rs.pNext = NULL;
    j->ms = rec->ms; j->ms.pNext = NULL; j->ms.pSampleMask = NULL;
    j->ds = rec->ds; j->ds.pNext = NULL;
    j->cba = rec->cba;
    j->cb = rec->cb; j->cb.pNext = NULL;
    j->cb.pAttachments = j->cb.attachmentCount ? &j->cba : NULL;
    memcpy(j->dyn, rec->dyn, sizeof(VkDynamicState) * rec->ndyn);
    j->dy = (VkPipelineDynamicStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = rec->ndyn, .pDynamicStates = j->dyn };
    j->ci = (VkGraphicsPipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = ns, .pStages = j->stages,
        .pVertexInputState = &j->vi, .pInputAssemblyState = &j->ia,
        .pViewportState = &j->vp, .pRasterizationState = &j->rs,
        .pMultisampleState = &j->ms,
        .pDepthStencilState = rec->has_ds ? &j->ds : NULL,
        .pColorBlendState = &j->cb, .pDynamicState = &j->dy,
        .layout = layout,
        .renderPass = get_render_pass(r, &rec->key.render_pass_state),
        .subpass = 0 };
    snode->layout = layout;
    snode->render_pass = j->ci.renderPass;
    snode->draw_time = s_bw_mode ? 0 : pg->draw_time;   /* boot warm-up: evictable once built */
    snode->has_dynamic_line_width = false;
    for (uint32_t i = 0; i < rec->ndyn; i++)
        if (rec->dyn[i] == VK_DYNAMIC_STATE_LINE_WIDTH) snode->has_dynamic_line_width = true;
    snode->async_job = j;
    pthread_mutex_lock(&s_ap.lock);
    if (s_ap.tail) s_ap.tail->next = j; else s_ap.head = j;
    s_ap.tail = j;
    pthread_cond_signal(&s_ap.cond);
    pthread_mutex_unlock(&s_ap.lock);
    s_pw_built++;
}
/* Boot warm-up: before the first movie, every recorded pipeline of every
 * level (the shipped list plus this phone's own) is built once -- shaders
 * compiled to SPIR-V into the shader cache, pipelines into the driver's
 * pipeline cache -- so a first visit compiles nothing. The guest waits at
 * its first frame meanwhile (a black screen, the first launch after an
 * install or after new pipelines were recorded); prewarm_boot.txt holds
 * how many records were done, and a later boot repeats only when the list
 * has grown. NV2A_NO_BOOT_WARM=1: off. */
static PrewarmRec *s_bw_list; static size_t s_bw_n, s_bw_cap;
static void bw_collect(PrewarmRec *rec, void *arg)
{
    (void)arg;
    uint64_t h = fast_hash((void *)&rec->key, sizeof rec->key);
    for (size_t i = 0; i < s_bw_n; i++)
        if (fast_hash((void *)&s_bw_list[i].key, sizeof rec->key) == h) return;
    if (s_bw_n == s_bw_cap) {
        s_bw_cap = s_bw_cap ? s_bw_cap * 2 : 1024;
        s_bw_list = g_realloc(s_bw_list, s_bw_cap * sizeof(PrewarmRec));
    }
    s_bw_list[s_bw_n++] = *rec;
}
static void prewarm_boot(PGRAPHState *pg)
{
    if (getenv("NV2A_NO_BOOT_WARM")) return;
    pw_scan(bw_collect, NULL);
    char mp[1200]; pw_path(mp, sizeof mp, "prewarm_boot.txt");
    long done = 0;
    FILE *mf = fopen(mp, "r");
    if (mf) { if (fscanf(mf, "%ld", &done) != 1) done = 0; fclose(mf); }
    if (!s_bw_n || (size_t)done >= s_bw_n) {
        fprintf(stderr, "[PREWARM] boot: %zu pipelines, already prepared\n", s_bw_n);
        g_free(s_bw_list); s_bw_list = NULL; s_bw_n = s_bw_cap = 0;
        return;
    }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "[PREWARM] boot: preparing %zu pipelines before the first movie\n", s_bw_n);
    pthread_mutex_lock(&s_ap.lock);
    unsigned built0 = s_ap.built;
    pthread_mutex_unlock(&s_ap.lock);
    unsigned queued0 = s_pw_built;
    /* in batches, each finished before the next: a pipeline still being
     * built cannot leave the cache, and more records than the cache holds
     * (2374 on the Pixel 8 against 2048 slots) left the lookup nothing to
     * evict -- a crash at the boot screen. Built ones carry draw time 0 and
     * may leave; what matters stays in the shader and driver caches. */
    s_bw_mode = true;
    unsigned want = 0;
    for (size_t i = 0; i < s_bw_n; i++) {
        prewarm_build(pg, &s_bw_list[i]);
        if ((i + 1) % 256 == 0 || i + 1 == s_bw_n) {
            want = s_pw_built - queued0;
            pthread_mutex_lock(&s_ap.lock);
            while (s_ap.built - built0 < want) pthread_cond_wait(&s_ap.done, &s_ap.lock);
            pthread_mutex_unlock(&s_ap.lock);
            fprintf(stderr, "[PREWARM] boot: %zu/%zu ready\n", i + 1, s_bw_n);
        }
    }
    s_bw_mode = false;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "[PREWARM] boot: %u pipelines built (%zu records) in %.1f s\n", want, s_bw_n,
            (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    mf = fopen(mp, "w");
    if (mf) { fprintf(mf, "%zu\n", s_bw_n); fclose(mf); }
    g_free(s_bw_list); s_bw_list = NULL; s_bw_n = s_bw_cap = 0;
    s_pw_built = s_pw_skipped = 0;
}

/* once a frame (the flip): start a level's list, build some of it */
void pgraph_vk_prewarm_step(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (prewarm_off() || !async_pipes_on(r)) return;
    { static bool booted; if (!booted) { booted = true; prewarm_boot(pg); } }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (s_pw_seq != g_xbox_level_seq) {
        s_pw_seq = g_xbox_level_seq;
        g_free(s_pw_list); s_pw_list = NULL; s_pw_n = s_pw_pos = 0;
        s_pw_built = s_pw_skipped = 0;
        char lvl[48]; snprintf(lvl, sizeof lvl, "%s", g_xbox_level_name);
        pw_scan(pw_collect, lvl);
        s_pw_t0 = now;
        fprintf(stderr, "[PREWARM] level %s: %zu pipelines to build\n", lvl, s_pw_n);
    }
    if (s_pw_pos >= s_pw_n) return;
    /* not while a movie plays: its decoder runs on the guest CPU, and the
     * workers building pipelines took the cores it needed (the boot intro
     * stuttered on a warm phone). The list waits for the movie to close. */
    {
        extern volatile uint32_t g_xbox_movie_handle;
        if (g_xbox_movie_handle) { s_pw_t0 = now; return; }
    }
    /* while the loading screen is up: most of a frame; later a little */
    bool loading = now - g_xbox_load_mark_ms < 20000 || now - s_pw_t0 < 20000;
    double budget = loading ? 24.0 : 3.0;
    /* the workers' queue stays short so the game's own pipelines wait little */
    unsigned queued = 0;
    pthread_mutex_lock(&s_ap.lock);
    for (AsyncPipeJob *q = s_ap.head; q; q = q->next) queued++;
    pthread_mutex_unlock(&s_ap.lock);
    if (queued > (loading ? 24u : 4u)) return;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (s_pw_pos < s_pw_n) {
        prewarm_build(pg, &s_pw_list[s_pw_pos++]);
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (ms > budget) break;
    }
    if (s_pw_pos >= s_pw_n)
        fprintf(stderr, "[PREWARM] level %s: %u queued, %u already there, %lld ms\n",
                g_xbox_level_name, s_pw_built, s_pw_skipped, (long long)(now - s_pw_t0));
}

static bool create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_bind_textures(d);
    pgraph_vk_bind_shaders(pg);

    // FIXME: If nothing was dirty, don't even try creating the key or hashing.
    //        Just use the same pipeline.
    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    // FIXME: We could clear less

    if (r->pipeline_binding && !pipeline_dirty) {
        NV2A_VK_DPRINTF("Cache hit");
        NV2A_VK_DGROUP_END();
        return true;
    }

    PipelineKey key;
    init_pipeline_key(pg, &key);
    uint64_t hash = fast_hash((void *)&key, sizeof(key));

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->async_job && !async_pipe_ready(r, snode)) {
        /* still building: drop the draw; recheck the key next time */
        if (snode->async_job == NULL && snode->pipeline == VK_NULL_HANDLE) {
            /* the build failed: build it in line below */
        } else {
            r->pipeline_binding = NULL;
            NV2A_VK_DGROUP_END();
            return false;
        }
    }
    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return true;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    memcpy(&snode->key, &key, sizeof(key));

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vsh.module_info->module,
            .pName = "main",
        };
    if (r->shader_binding->geom.module_info) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geom.module_info->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->psh.module_info->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (r->shader_binding->state.psh.dbg & 256) ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST :
                    get_primitive_topology(pg),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    /* nv2a-port: with depth clamp there is no near-plane clipping, and
     * Mali rasterises triangles that cross w = 0 as fans out of the screen
     * centre. Mali has no shaderClipDistance to clip at w = 0 alone, so it
     * clips at the near and far planes instead (NV2A_DEPTH_CLAMP=1 restores
     * clamping; geometry past the far plane is cut otherwise). */
    static int depth_clamp = -1;
    if (depth_clamp < 0) {
        const char *e = getenv("NV2A_DEPTH_CLAMP");
        /* Default on, as xemu: the fans were stale vertex memory (missing
         * flushes), and without the clamp 2D UI at z outside [0,1] was
         * clipped away. NV2A_DEPTH_CLAMP=0 turns it off. */
        extern bool nv2a_glsl_zclip_w;
        /* with the w = 0 cut done by the near plane (vsh.c), clamping is
         * already in the vertex z and the clip itself must stay on */
        depth_clamp = e ? (*e != '0') : !nv2a_glsl_zclip_w;
        fprintf(stderr, "[vk] depth clamp %s\n", depth_clamp ? "on" : "off (near/far clipping)");
    }

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = depth_clamp ? VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = r->enabled_physical_device_features.fillModeNonSolid ?
                           pgraph_polygon_mode_vk_map[r->shader_binding->state
                                                      .geom.polygon_front_mode] :
                           VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .frontFace = (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                      NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                         VK_FRONT_FACE_COUNTER_CLOCKWISE :
                         VK_FRONT_FACE_CLOCKWISE,
        /* nv2a-port: on Mali the fragment shader exports no depth, so the
         * polygon offset it applied (depthOffset/depthFactor) was lost and
         * decals -- the haystacks' projected shadows -- fought the floor and
         * flickered. The rasteriser's depth bias does it instead, set per
         * draw (dynamic). NV2A_NO_HWBIAS=1: none, as before. */
        .depthBiasEnable = nv2a_hw_bias(r) ? VK_TRUE : VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
    } else {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
    };

    if (r->shader_binding->state.psh.dbg & 256) rasterizer.cullMode = VK_CULL_MODE_NONE;
    if (r->shader_binding->state.psh.dbg & (32 | 256)) {
        depth_test = false; stencil_test = false;
        depth_stencil.depthWriteEnable = VK_FALSE;
    }
    if (depth_test) {
        depth_stencil.depthTestEnable = VK_TRUE;
        uint32_t depth_func =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
        depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
    }

    if (stencil_test) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                         NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t mask_read = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                      NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t mask_write = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                       NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

        depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
        depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
        depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
        depth_stencil.front.compareOp =
            pgraph_stencil_func_vk_map[stencil_func];
        depth_stencil.front.compareMask = mask_read;
        depth_stencil.front.writeMask = mask_write;
        depth_stencil.front.reference = stencil_ref;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    if (r->shader_binding->state.psh.dbg & 256)
        write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };

    float blend_constant[4] = { 0, 0, 0, 0 };

    if ((pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) &&
        !(r->shader_binding->state.psh.dbg & 256)) {
        color_blend_attachment.blendEnable = VK_TRUE;

        uint32_t sfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        color_blend_attachment.srcColorBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstColorBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];
        color_blend_attachment.srcAlphaBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstAlphaBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];

        uint32_t equation =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        color_blend_attachment.colorBlendOp =
            pgraph_blend_equation_vk_map[equation];
        color_blend_attachment.alphaBlendOp =
            pgraph_blend_equation_vk_map[equation];

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
        .blendConstants[0] = blend_constant[0],
        .blendConstants[1] = blend_constant[1],
        .blendConstants[2] = blend_constant[2],
        .blendConstants[3] = blend_constant[3],
    };

    /* nv2a-port: blend constants are dynamic (set per draw in begin_pre_draw):
     * fades step NV_PGRAPH_BLENDCOLOR every frame, and as part of the pipeline
     * key each step was a new pipeline -- 800 in 5 s through a cutscene fade,
     * and objects missing while they built. */
    VkDynamicState dynamic_states[5] = { VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    int num_dynamic_states = 3;
    if (nv2a_hw_bias(r))
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_DEPTH_BIAS;

    snode->has_dynamic_line_width =
        (r->enabled_physical_device_features.wideLines == VK_TRUE) &&
        (r->shader_binding->state.geom.polygon_front_mode == POLY_MODE_LINE ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINES ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_LOOP ||
         r->shader_binding->state.geom.primitive_mode == PRIM_TYPE_LINE_STRIP);
    if (snode->has_dynamic_line_width) {
        dynamic_states[num_dynamic_states++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    }

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = num_dynamic_states,
        .pDynamicStates = dynamic_states,
    };

    // FIXME: Dither
    // if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };

    VkPushConstantRange push_constant_range;
    if (r->use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            __builtin_popcount(r->shader_binding->state.vsh.uniform_attrs);
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VkPipeline pipeline;
    nv2a_pipelines_created++;

    {   /* nv2a-port probe: why a key is new. For each field, the key with
         * that field blanked: if that was seen before, this field alone made
         * the pipeline new (a per-frame value in the key churns pipelines). */
        enum { NF = 14, TB = 1 << 15 };
        static uint64_t *seen[NF]; static unsigned only[NF], n, total_new; static int64_t t0;
        static uint32_t ex_old[NF], ex_new[NF];
        if (!seen[0]) for (int f = 0; f < NF; f++) seen[f] = g_malloc0(sizeof(uint64_t) * TB);
        const PipelineKey *k = &snode->key;
        for (int f = 0; f < NF; f++) {
            PipelineKey m = *k;
            if (f == 0) m.clear = 0;
            else if (f == 1) memset(&m.render_pass_state, 0, sizeof m.render_pass_state);
            else if (f == 2) memset(&m.shader_state, 0, sizeof m.shader_state);
            else if (f < 12) m.regs[f - 3] = 0;
            else if (f == 12) memset(m.binding_descriptions, 0, sizeof m.binding_descriptions);
            else memset(m.attribute_descriptions, 0, sizeof m.attribute_descriptions);
            uint64_t h = fast_hash((void *)&m, sizeof m) | 1;
            unsigned i = (unsigned)(h % TB);
            int hit = 0;
            for (int pr = 0; pr < 16; pr++, i = (i + 1) % TB) {
                if (seen[f][i] == h) { hit = 1; break; }
                if (!seen[f][i]) { seen[f][i] = h; break; }
            }
            if (hit) { only[f]++; if (f >= 3 && f < 12) ex_new[f] = k->regs[f - 3]; }
        }
        n++; total_new++;
        struct timespec tsn; clock_gettime(CLOCK_MONOTONIC, &tsn);
        int64_t now = (int64_t)tsn.tv_sec * 1000000 + tsn.tv_nsec / 1000;
        if (now - t0 > 5000000) {
            if (n > 30)
                fprintf(stderr, "[PIPEKEY] %u new (%u this run); new only by: clear %u rp %u shader %u "
                        "blend %u ctl0 %u ctl1 %u ctl2 %u ctl3 %u raster %u zbias %u zfactor %u vb %u va %u "
                        "(last ctl0 %08x ctl3 %08x raster %08x)\n",
                        n, total_new, only[0], only[1], only[2], only[3], only[5], only[6], only[7],
                        only[8], only[9], only[10], only[11], only[12], only[13],
                        ex_new[5], ex_new[8], ex_new[9]);
            memset(only, 0, sizeof only); n = 0; t0 = now;
        }
        (void)ex_old;
    }
    extern bool pgraph_vk_known(int which, uint64_t h);
    extern void pgraph_vk_known_add(int which, uint64_t h);
    bool known = pgraph_vk_known(1, hash);
    pgraph_vk_known_add(1, hash);
    {
        PrewarmRec *pr = g_malloc0(sizeof(*pr));
        pr->key = snode->key;
        pr->rs = rasterizer; pr->ia = input_assembly; pr->ms = multisampling;
        pr->ds = depth_stencil; pr->cba = color_blend_attachment; pr->cb = color_blending;
        pr->ndyn = num_dynamic_states;
        memcpy(pr->dyn, dynamic_states, sizeof(VkDynamicState) * num_dynamic_states);
        pr->nvb = r->num_active_vertex_binding_descriptions;
        pr->nva = r->num_active_vertex_attribute_descriptions;
        pr->push_size = pipeline_layout_info.pushConstantRangeCount ? push_constant_range.size : 0;
        pr->has_ds = pipeline_create_info.pDepthStencilState != NULL;
        prewarm_record(hash, pr);
        g_free(pr);
    }
    /* nv2a-port: "known" (built in an earlier run) is no promise of a cache
     * hit: the Mali driver's cache stops growing at ~25 MB, so a known
     * pipeline can still be a 100-200 ms compile in line -- the hitches on a
     * move first done in a session. Every pipeline goes to the workers; a
     * cache hit is back within the short wait. NV2A_ASYNC_NEW_ONLY=1: old. */
    static int new_only = -1;
    if (new_only < 0) new_only = getenv("NV2A_ASYNC_NEW_ONLY") != NULL;
    if ((!known || !new_only) && async_pipes_on(r) && rasterizer.pNext == NULL &&
        r->num_active_vertex_binding_descriptions <= NV2A_VERTEXSHADER_ATTRIBUTES &&
        r->num_active_vertex_attribute_descriptions <= NV2A_VERTEXSHADER_ATTRIBUTES &&
        num_dynamic_states <= 4) {
        AsyncPipeJob *j = g_malloc0(sizeof(*j));
        j->known = known;
        memcpy(j->stages, shader_stages, sizeof(shader_stages[0]) * num_active_shader_stages);
        ShaderModuleInfo *mods[3] = { r->shader_binding->vsh.module_info,
                                      r->shader_binding->geom.module_info,
                                      r->shader_binding->psh.module_info };
        for (int i = 0; i < 3; i++)
            if (mods[i]) { pgraph_vk_ref_shader_module(mods[i]); j->mods[j->nmods++] = mods[i]; }
        memcpy(j->vb, r->vertex_binding_descriptions,
               sizeof(j->vb[0]) * r->num_active_vertex_binding_descriptions);
        memcpy(j->va, r->vertex_attribute_descriptions,
               sizeof(j->va[0]) * r->num_active_vertex_attribute_descriptions);
        j->vi = vertex_input;
        j->vi.pVertexBindingDescriptions = j->vb;
        j->vi.pVertexAttributeDescriptions = j->va;
        j->ia = input_assembly;
        j->vp = viewport_state;
        j->rs = rasterizer;
        j->ms = multisampling;
        j->ds = depth_stencil;
        j->cba = color_blend_attachment;
        j->cb = color_blending;
        j->cb.pAttachments = color_blending.pAttachments ? &j->cba : NULL;
        memcpy(j->dyn, dynamic_states, sizeof(VkDynamicState) * num_dynamic_states);
        j->dy = dynamic_state;
        j->dy.pDynamicStates = j->dyn;
        j->ci = pipeline_create_info;
        j->ci.pStages = j->stages;
        j->ci.pVertexInputState = &j->vi;
        j->ci.pInputAssemblyState = &j->ia;
        j->ci.pViewportState = &j->vp;
        j->ci.pRasterizationState = &j->rs;
        j->ci.pMultisampleState = &j->ms;
        j->ci.pDepthStencilState = pipeline_create_info.pDepthStencilState ? &j->ds : NULL;
        j->ci.pColorBlendState = &j->cb;
        j->ci.pDynamicState = &j->dy;

        snode->layout = layout;
        snode->render_pass = pipeline_create_info.renderPass;
        snode->draw_time = pg->draw_time;
        snode->async_job = j;

        pthread_mutex_lock(&s_ap.lock);
        if (s_ap.tail) s_ap.tail->next = j; else s_ap.head = j;
        s_ap.tail = j;
        pthread_cond_signal(&s_ap.cond);
        pthread_mutex_unlock(&s_ap.lock);

        if (async_pipe_ready(r, snode)) {
            r->pipeline_binding = snode;
            r->pipeline_binding_changed = true;
            NV2A_VK_DGROUP_END();
            return true;
        }
        r->pipeline_binding = NULL;
        NV2A_VK_DGROUP_END();
        return false;
    }

    struct timespec pc0; clock_gettime(CLOCK_MONOTONIC, &pc0);
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));
    {   extern double nv2a_pipe_sync_ms; extern unsigned nv2a_pipe_sync_n;
        struct timespec pc1; clock_gettime(CLOCK_MONOTONIC, &pc1);
        nv2a_pipe_sync_ms += (pc1.tv_sec - pc0.tv_sec) * 1e3 + (pc1.tv_nsec - pc0.tv_nsec) / 1e6;
        nv2a_pipe_sync_n++; }

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
    return true;
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->use_push_constants_for_uniform_attrs) {
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.vsh.uniform_attrs,
                             values, &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= r->descriptor_set_base + 1);

    extern bool pgraph_vk_dyn_ubo(void);
    uint32_t dyn[2] = { (uint32_t)r->uniform_buffer_offsets[0],
                        (uint32_t)r->uniform_buffer_offsets[1] };
    bool d = pgraph_vk_dyn_ubo();
    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            &r->descriptor_sets[r->descriptor_set_index - 1], d ? 2 : 0,
                            d ? dyn : NULL);
}

static int nv2a_env_flag(const char *name, int *cache)
{
    if (*cache < 0) *cache = getenv(name) != NULL;
    return *cache;
}
static int s_no_query = -1, s_no_precise = -1;

/* nv2a-port: an occlusion query begun outside a render pass counted
 * nothing on Mali (3411 of 3478 reports were 0: the title took everything
 * for hidden -- characters missing at scene starts, the bar's animals
 * popping in). The logical query (which draws feed which report) is as
 * before; the Vulkan queries are begun inside each render pass while it is
 * active and ended before the pass ends -- the reports already sum every
 * query between them. NV2A_QUERY_OUTSIDE_PASS=1: the old way. */
static int s_q_outside = -1;
static bool s_vkq_open;
static bool query_outside(void)
{
    if (s_q_outside < 0) s_q_outside = getenv("NV2A_QUERY_OUTSIDE_PASS") != NULL;
    return s_q_outside;
}
/* nv2a-port: the whole pool of the slot is reset when its command buffer
 * starts, so a visibility test can begin inside a render pass and a new
 * test no longer ends the pass. On a tiled GPU every pass end writes the
 * 640x480 colour and depth tiles back to memory and the next pass reads
 * them again; with ~20 tests per frame that was half of the main target's
 * passes. NV2A_QUERY_SPLIT=1: the old way (a pass per test). */
static int s_q_split = -1;
static bool query_presplit(void)
{
    if (s_q_split < 0) s_q_split = getenv("NV2A_QUERY_SPLIT") != NULL || query_outside();
    return !s_q_split;
}
static void vk_query_reset_next(PGRAPHVkState *r)   /* outside a pass */
{
    if (query_presplit()) return;   /* reset with the command buffer */
    assert(r->num_queries_in_flight < r->max_queries_in_flight);
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        r->num_queries_in_flight, 1);
}
static void vk_query_begin(PGRAPHVkState *r)
{
    vkCmdBeginQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight,
                    nv2a_env_flag("NV2A_NO_PRECISE", &s_no_precise) ? 0 :
                    VK_QUERY_CONTROL_PRECISE_BIT);
    r->num_queries_in_flight++;
    s_vkq_open = true;
}
static void vk_query_end(PGRAPHVkState *r)
{
    if (!s_vkq_open) return;
    vkCmdEndQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight - 1);
    s_vkq_open = false;
}

static void begin_query(PGRAPHVkState *r)
{
    if (nv2a_env_flag("NV2A_NO_QUERY", &s_no_query)) {   /* experiment */
        r->new_query_needed = false;
        return;
    }
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    assert(r->num_queries_in_flight < r->max_queries_in_flight);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    if (query_outside()) {
        vk_query_reset_next(r);
        vk_query_begin(r);
    }
    /* else: begun by the next render pass (begin_render_pass) */

    r->query_in_flight = true;
    r->new_query_needed = false;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(r->query_in_flight);

    vk_query_end(r);   /* inside-pass queries ended with their pass already */
    r->query_in_flight = false;
}

static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];

    if (b_src->buffer_offset <= b_src->slot_lo) {
        b_src->buffer_offset = b_src->slot_lo;
        return;
    }

    VkBufferCopy copy_region = { .srcOffset = b_src->slot_lo,
                                 .dstOffset = b_src->slot_lo,
                                 .size = b_src->buffer_offset - b_src->slot_lo };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);
    if (index_src == BUFFER_VERTEX_INLINE_STAGING) {
        extern uint32_t nv2a_inline_gen;
        nv2a_inline_gen++;   /* invalidates the remap cache */
    }

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        /* nv2a-port: fragment shaders read uniforms too (window clip,
         * depth range, combiner constants) */
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .offset = b_src->slot_lo,
        .size = b_src->buffer_offset - b_src->slot_lo
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);

    b_src->buffer_offset = b_src->slot_lo;
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    { extern void nv2a_vram_flush_dirty(PGRAPHVkState *r); nv2a_vram_flush_dirty(r); }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

/* nv2a-port: the size of the framebuffer actually bound (create_framebuffer
 * uses the colour binding, else the zeta one). renderArea and scissor come
 * from pg->surface_binding_dim / surface_shape, which can disagree with it;
 * a render area or scissor past the framebuffer made Mali's fragment job run
 * until the kernel hard-stopped it and reset the GPU. */
static void nv2a_fb_dims(PGRAPHState *pg, unsigned int *w, unsigned int *h)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *b = r->color_binding ? r->color_binding : r->zeta_binding;
    *w = b ? b->width : 0;
    *h = b ? b->height : 0;
    pgraph_apply_scaling_factor(pg, w, h);
}

/* nv2a-port: GPU time per render pass, by target size (NV2A_GPROF=1):
 * timestamps around every pass, summed per WxH and printed with GPERF. */
#define GP_Q 512
static VkQueryPool s_gp_pool;
static int s_gp_on = -1;
static struct { uint16_t w, h; uint32_t draws0; } s_gp_rec[2][GP_Q / 2];
static int s_gp_n[2];
static uint32_t s_gp_draws[2][GP_Q / 2];
typedef struct { uint16_t w, h; double ms; unsigned n, draws; } GpAcc;
GpAcc nv2a_gp_acc[32];
static void gp_reset_slot(PGRAPHVkState *r)
{
    if (s_gp_on < 0) {
        s_gp_on = getenv("NV2A_GPROF") && r->device_props.limits.timestampComputeAndGraphics;
        if (s_gp_on) {
            VkQueryPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_Q * 2 };
            if (vkCreateQueryPool(r->device, &ci, NULL, &s_gp_pool) != VK_SUCCESS) s_gp_on = 0;
        }
    }
    if (!s_gp_on) return;
    vkCmdResetQueryPool(r->command_buffer, s_gp_pool, r->slot * GP_Q, GP_Q);
    s_gp_n[r->slot] = 0;
}
static void gp_pass_begin(PGRAPHState *pg, unsigned w, unsigned h)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (s_gp_on <= 0) return;
    int i = s_gp_n[r->slot];
    if (i >= GP_Q / 2) return;
    s_gp_rec[r->slot][i].w = w; s_gp_rec[r->slot][i].h = h;
    s_gp_rec[r->slot][i].draws0 = g_nv2a_stats.frame_working.counters[NV2A_PROF_BEGIN_ENDS];
    vkCmdWriteTimestamp(r->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_gp_pool,
                        r->slot * GP_Q + i * 2);
}
static void gp_pass_end(PGRAPHVkState *r)
{
    if (s_gp_on <= 0) return;
    int i = s_gp_n[r->slot];
    if (i >= GP_Q / 2) return;
    vkCmdWriteTimestamp(r->command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_gp_pool,
                        r->slot * GP_Q + i * 2 + 1);
    s_gp_draws[r->slot][i] = g_nv2a_stats.frame_working.counters[NV2A_PROF_BEGIN_ENDS] -
                             s_gp_rec[r->slot][i].draws0;
    s_gp_n[r->slot] = i + 1;
}
static void gp_collect(PGRAPHVkState *r, int s)
{
    if (s_gp_on <= 0 || !s_gp_n[s]) return;
    int n = s_gp_n[s];
    static uint64_t t[GP_Q];
    if (vkGetQueryPoolResults(r->device, s_gp_pool, s * GP_Q, n * 2, sizeof(uint64_t) * n * 2, t,
                              sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) { s_gp_n[s] = 0; return; }
    double per = r->device_props.limits.timestampPeriod / 1e6;
    for (int i = 0; i < n; i++) {
        if (t[i * 2 + 1] <= t[i * 2]) continue;
        double ms = (t[i * 2 + 1] - t[i * 2]) * per;
        int k;
        for (k = 0; k < 32 && nv2a_gp_acc[k].n &&
             !(nv2a_gp_acc[k].w == s_gp_rec[s][i].w && nv2a_gp_acc[k].h == s_gp_rec[s][i].h); k++) ;
        if (k == 32) continue;
        nv2a_gp_acc[k].w = s_gp_rec[s][i].w; nv2a_gp_acc[k].h = s_gp_rec[s][i].h;
        nv2a_gp_acc[k].ms += ms; nv2a_gp_acc[k].n++; nv2a_gp_acc[k].draws += s_gp_draws[s][i];
    }
    s_gp_n[s] = 0;
}

/* nv2a-port: a clear of the whole target at the start of a pass becomes the
 * pass's load op (CLEAR instead of LOAD + vkCmdClearAttachments): Mali
 * then skips reading the old contents into tile memory. Many of Conker's
 * small target passes are exactly that. NV2A_NO_RP_LOADCLEAR=1: off. */
static bool s_rb_flush;   /* read back the small targets at the next pass break */
static int s_pend_clear_mask;          /* 1 color, 2 depth/stencil */
static int s_pend_clear_used;
static VkClearValue s_pend_clear_vals[2];
static struct { RenderPassState st; int mask; VkRenderPass rp; } s_rpv[64];
static int s_rpv_n;
static VkRenderPass rp_variant(PGRAPHVkState *r, RenderPassState *st, int mask)
{
    for (int i = 0; i < s_rpv_n; i++)
        if (s_rpv[i].mask == mask && !memcmp(&s_rpv[i].st, st, sizeof *st)) return s_rpv[i].rp;
    VkRenderPass rp = create_render_pass_ex(r, st, mask);
    if (s_rpv_n < 64) { s_rpv[s_rpv_n].st = *st; s_rpv[s_rpv_n].mask = mask; s_rpv[s_rpv_n].rp = rp; s_rpv_n++; }
    return rp;
}
unsigned nv2a_rp_loadclears;

static long long s_pass_draws0;
static unsigned s_pass_w, s_pass_h;
static uint32_t s_pass_col, s_pass_zeta;
static int s_pass_clear;
static void *s_pass_opener;
unsigned nv2a_empty_passes;
static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    {
        unsigned int fw, fh;
        nv2a_fb_dims(pg, &fw, &fh);
        static int logged;
        if ((vp_width > fw || vp_height > fh) && logged++ < 8)
            fprintf(stderr, "[vk] render area %ux%u > framebuffer %ux%u: clamped\n",
                    vp_width, vp_height, fw, fh);
        if (fw && vp_width > fw) vp_width = fw;
        if (fh && vp_height > fh) vp_height = fh;
    }

    assert(r->framebuffer_index > 0);

    if (nv2a_flag("NV2A_RP_BARRIER", &s_rp_barrier)) {
        /* experiment: everything before is complete and visible before the
         * pass loads its attachments (Mali preloads tiles in its own
         * fragment work, outside COLOR_ATTACHMENT_OUTPUT). */
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        vkCmdPipelineBarrier(r->command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                             0, NULL);
    }

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = (nv2a_flag("NV2A_RP_CLEAR", &s_rp_clear) || nv2a_flag("NV2A_RP_CLEAR_Z", &s_rp_clear_z)) ? 2 : 0,
        .pClearValues = (nv2a_flag("NV2A_RP_CLEAR", &s_rp_clear) || nv2a_flag("NV2A_RP_CLEAR_Z", &s_rp_clear_z)) ?
                        nv2a_rp_clear_values : NULL,
    };
    if (s_rb_flush) {
        extern bool nv2a_rb_no_host_barrier;
        s_rb_flush = false;
        nv2a_rb_no_host_barrier = true;
        pgraph_vk_record_eager_readback(pg, r->slot);
        nv2a_rb_no_host_barrier = false;
    }
    if (s_pend_clear_mask && !render_pass_begin_info.clearValueCount) {
        RenderPassState st;
        init_render_pass_state(pg, &st);
        int mask = s_pend_clear_mask;
        if (st.color_format == VK_FORMAT_UNDEFINED) mask &= ~1;
        if (st.zeta_format == VK_FORMAT_UNDEFINED) mask &= ~2;
        if (mask) {
            render_pass_begin_info.renderPass = rp_variant(r, &st, mask);
            render_pass_begin_info.clearValueCount = 2;
            int zi = st.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
            static VkClearValue cv[2];
            if (zi) cv[0] = s_pend_clear_vals[0];
            cv[zi] = s_pend_clear_vals[1];
            render_pass_begin_info.pClearValues = cv;
            s_pend_clear_used = mask;
            nv2a_rp_loadclears++;
        }
        s_pend_clear_mask = 0;
    }
    gp_pass_begin(pg, vp_width, vp_height);
    {   /* nv2a-port: what opened a pass, for the empty-pass report */
        s_pass_draws0 = g_nv2a_stats.frame_working.counters[NV2A_PROF_BEGIN_ENDS];
        s_pass_w = vp_width; s_pass_h = vp_height;
        s_pass_col = r->color_binding ? (uint32_t)r->color_binding->vram_addr : 0;
        s_pass_zeta = r->zeta_binding ? (uint32_t)r->zeta_binding->vram_addr : 0;
        s_pass_clear = render_pass_begin_info.clearValueCount ? s_pend_clear_used : 0;
        s_pass_opener = __builtin_return_address(0);
    }
    bool q_in_pass = r->query_in_flight && !query_outside() &&
                     r->num_queries_in_flight < r->max_queries_in_flight;
    if (q_in_pass) vk_query_reset_next(r);
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;
    if (q_in_pass) vk_query_begin(r);

}

static void end_render_pass(PGRAPHVkState *r)
{
    if (r->in_render_pass) {
        if (!query_outside()) vk_query_end(r);
        vkCmdEndRenderPass(r->command_buffer);
        gp_pass_end(r);
        r->in_render_pass = false;
        if (g_nv2a_stats.frame_working.counters[NV2A_PROF_BEGIN_ENDS] == s_pass_draws0) {
            nv2a_empty_passes++;
            nv2a_site_hit(s_pass_clear ? "pass:empty-clear" : "pass:empty",
                          __builtin_return_address(0), s_pass_w, s_pass_h);
            static int nlog = -1;
            if (nlog < 0) nlog = getenv("NV2A_EMPTYPASS_LOG") ? 0 : 1000;
            if (nlog < 60) {
                nlog++;
                fprintf(stderr, "[EMPTYPASS] %ux%u col %08x zeta %08x clear %d opened by %p closed by %p\n",
                        s_pass_w, s_pass_h, s_pass_col, s_pass_zeta, s_pass_clear,
                        s_pass_opener, __builtin_return_address(0));
            }
        }
    }
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
    [VK_FINISH_REASON_SEMAPHORE] = NV2A_PROF_FINISH_FLUSH,
};

bool nv2a_eager_after_wait;
uint32_t nv2a_inline_gen;

/* nv2a-port: GPU time per main command buffer (NV2A_PERF / CONKER_PROF) */
double nv2a_gpu_ms_acc, nv2a_fence_ms_acc;
double nv2a_pfifo_idle_ms_acc, nv2a_pfifo_flipwait_ms_acc, nv2a_pfifo_nopwait_ms_acc;
unsigned nv2a_pfifo_nopwaits;
uint32_t nv2a_nop_param[8]; unsigned nv2a_nop_cnt[8];
static VkQueryPool s_ts_pool;
static bool s_ts_on, s_ts_open[2], s_ts_closed[2];
static void ts_begin(PGRAPHVkState *r)
{
    static int on = -1;
    if (on < 0) {
        /* always: the GPU time feeds the ADPF hints and the governor
         * (two timestamps per frame slot; NV2A_NO_GPU_TS=1 turns it off) */
        on = !getenv("NV2A_NO_GPU_TS") && r->device_props.limits.timestampComputeAndGraphics;
        if (on) {
            VkQueryPoolCreateInfo ci = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4 };
            if (vkCreateQueryPool(r->device, &ci, NULL, &s_ts_pool) != VK_SUCCESS)
                on = 0;
        }
    }
    s_ts_on = on;
    if (!on) return;
    int q = r->slot * 2;
    vkCmdResetQueryPool(r->command_buffer, s_ts_pool, q, 2);
    vkCmdWriteTimestamp(r->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_ts_pool, q);
    s_ts_open[r->slot] = true;
}
static void ts_end(PGRAPHVkState *r)
{
    if (!s_ts_on || !s_ts_open[r->slot]) return;
    vkCmdWriteTimestamp(r->command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        s_ts_pool, r->slot * 2 + 1);
    s_ts_open[r->slot] = false;
    s_ts_closed[r->slot] = true;
}
static void ts_collect(PGRAPHVkState *r, int s)
{
    if (!s_ts_closed[s]) return;
    s_ts_closed[s] = false;
    uint64_t t[2];
    if (vkGetQueryPoolResults(r->device, s_ts_pool, s * 2, 2, sizeof t, t, sizeof t[0],
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
        nv2a_gpu_ms_acc += (t[1] - t[0]) * (double)r->device_props.limits.timestampPeriod / 1e6;
}

/* where a blocking fence wait lands, and how long it takes (GPERF) */
void *nv2a_wait_site[16]; double nv2a_wait_site_ms[16]; unsigned nv2a_wait_site_n[16];
static void note_wait_site(void *ra, double ms)
{
    int k;
    for (k = 0; k < 16 && nv2a_wait_site[k] && nv2a_wait_site[k] != ra; k++) ;
    if (k < 16) { nv2a_wait_site[k] = ra; nv2a_wait_site_ms[k] += ms; nv2a_wait_site_n[k]++; }
}

static void fence_hang_report(PGRAPHVkState *r, VkFence fence, int reason)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "[vk] GPU busy >1 s: finish reason %d, cb: %d draws (%d with GS), "
            "%d clears, %d compute\n", reason, nv2a_vk_cb_draws,
            nv2a_vk_cb_gs_draws, nv2a_vk_cb_clears, nv2a_vk_cb_compute);
    fprintf(stderr, "[vk]   last draw: %u verts, %s\n", nv2a_vk_last_count,
            nv2a_vk_last_draw);
    fprintf(stderr, "[vk]   MAIN buffer hung; it holds %d surface->texture copies\n",
            nv2a_vk_cb_s2t);
    fprintf(stderr, "[vk]   draws in it:%s\n", nv2a_cb_draws_log);
    {   /* the shaders of the last draw, once, to files */
        static int dumped;
        const char *dir = getenv("NV2A_DUMP_DIR");
        if (!dumped++ && r->shader_binding && dir) {
            ShaderModuleInfo *mi[2] = { r->shader_binding->vsh.module_info,
                                        r->shader_binding->psh.module_info };
            const char *nm[2] = { "hang_vsh.glsl", "hang_psh.glsl" };
            for (int m = 0; m < 2; m++) {
                if (!mi[m] || !mi[m]->glsl) continue;
                char path[1024];
                snprintf(path, sizeof path, "%s/%s", dir, nm[m]);
                FILE *f = fopen(path, "w");
                if (f) { fputs(mi[m]->glsl, f); fclose(f); }
                fprintf(stderr, "[vk]   wrote %s\n", path);
            }
        }
    }
    VkResult wr = vkWaitForFences(r->device, 1, &fence, VK_TRUE, UINT64_MAX);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "[vk]   ... fence after %.1f s more, result %d\n",
            (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, (int)wr);
    if (wr == VK_ERROR_DEVICE_LOST) nv2a_vk_report_device_fault(r, "main");
}

static void destroy_slot_framebuffers(PGRAPHVkState *r, int s)
{
    for (int i = 0; i < r->slot_fb_n[s]; i++) {
        vkDestroyFramebuffer(r->device, r->slot_fbs[s][i], NULL);
        r->slot_fbs[s][i] = VK_NULL_HANDLE;
    }
    r->slot_fb_n[s] = 0;
}

static bool s_eager_due;
static int s_retire_depth;

/* nv2a-port: deferred semaphore releases. On the Xbox a BACK_END semaphore
 * release lands when the GPU gets there; the title then reads what it drew
 * (Conker: small render targets that decide which characters are shown).
 * Writing it when pfifo parses the method let the title read the targets
 * before our read-back of them, and characters blinked out. Now the
 * release ends the command buffer with the read-backs of those targets
 * (no wait) and is written when that submission retires, after the
 * read-backs are copied to guest RAM. Releases behind one still pending
 * queue behind it, in order. NV2A_SEM_IMMEDIATE=1: old behaviour. */
typedef struct { uint32_t *p; uint32_t v; uint64_t submit; } SemQ;
#define SEMQ_MAX 256
static SemQ s_semq[SEMQ_MAX];
static int s_semq_h, s_semq_n;
unsigned nv2a_sem_deferred, nv2a_sem_deferred_waits;
volatile unsigned s_sem_released_n;
volatile uint32_t s_last_rel;

void sem_q_release(uint64_t upto)
{
    bool any = false;
    while (s_semq_n && s_semq[s_semq_h].submit <= upto) {
        SemQ *e = &s_semq[s_semq_h];
        stl_le_p(e->p, e->v);
        s_last_rel = e->v;
        { extern void nv2a_tl(int, uint32_t); nv2a_tl(6, e->v); }
        s_semq_h = (s_semq_h + 1) % SEMQ_MAX;
        s_semq_n--;
        s_sem_released_n++;
        any = true;
    }
    if (any) { extern void nv2a_sem_signal(void); nv2a_sem_signal(); }
}

bool pgraph_vk_sem_deferred_pending(void) { return s_semq_n != 0; }

/* nv2a-port: read guard (NV2A_SEM_GUARD, see nv2a_xemu_glue.c): semaphores
 * are released when parsed; the small targets' read-backs are queued at
 * each release with their pages guarded, and a title access waits here for
 * the read-back instead. */
static void wait_slot(PGRAPHVkState *r, int s, void *ra);
volatile int nv2a_guard_req;
static volatile unsigned s_guard_gen;
static pthread_mutex_t s_guard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_guard_cv = PTHREAD_COND_INITIALIZER;
unsigned nv2a_guard_services;
int nv2a_sem_guard_mode(void)
{
    static int m = -1;
    if (m < 0) m = getenv("NV2A_SEM_GUARD") != NULL;
    return m;
}
void pgraph_vk_service_guard(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (!r) return;
    nv2a_guard_req = 0;
    nv2a_guard_services++;
    if (r->in_command_buffer)
        pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_SEMAPHORE);
    for (int k = 0; k < 2; k++) {
        int s = (r->slot ^ 1 ^ k) & 1;
        if (r->slot_pending[s]) wait_slot(r, s, __builtin_return_address(0));
    }
    /* every queued read-back has landed: nothing is guarded any more */
    extern void nv2a_host_unguard_all(void);
    nv2a_host_unguard_all();
    pthread_mutex_lock(&s_guard_lock);
    s_guard_gen++;
    pthread_cond_broadcast(&s_guard_cv);
    pthread_mutex_unlock(&s_guard_lock);
}
/* a title thread touched a guarded page: have pfifo land the read-backs */
int nv2a_host_resolve_guard(uint64_t addr)
{
    (void)addr;
    NV2AState *d = g_nv2a;
    if (!d) return 0;
    pthread_mutex_lock(&s_guard_lock);
    unsigned g = s_guard_gen;
    nv2a_guard_req = 1;
    pthread_mutex_unlock(&s_guard_lock);
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_cond_broadcast(&d->pfifo.fifo_cond);
    qemu_mutex_unlock(&d->pfifo.lock);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 60 * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&s_guard_lock);
    while (s_guard_gen == g) {
        if (pthread_cond_timedwait(&s_guard_cv, &s_guard_lock, &dl) != 0) break;
    }
    int ok = s_guard_gen != g;
    pthread_mutex_unlock(&s_guard_lock);
    return ok;
}

/* hang watch: no release for 2 s while some are queued, or the title
 * waiting on one for 2 s: print the state once per second (up to 15). */
volatile uint32_t nv2a_semwait_va, nv2a_semwait_fence, nv2a_semwait_active;
static void *semq_watch(void *arg)
{
    NV2AState *d = arg;
    unsigned last = 0, same = 0, printed = 0;
    for (;;) {
        sleep(1);
        unsigned now = s_sem_released_n;
        if (now == last && (s_semq_n || nv2a_semwait_active)) same++; else same = 0;
        last = now;
        if (same >= 2 && printed < 15) {
            printed++;
            PGRAPHVkState *r = d->pgraph.vk_renderer_state;
            uint32_t semval = nv2a_semwait_va ? *(volatile uint32_t *)(
                d->vram_ptr + (nv2a_semwait_va & 0x07FFFFFF)) : 0;
            extern ptrdiff_t g_xbox_mem_offset;
            uint32_t devp = *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x5499E8);
            const volatile uint32_t *dv = (const volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + devp);
            fprintf(stderr, "[SEMWATCH] sem value %08x, dma put %08x get %08x, last released %08x; "
                    "dev %08x: +0 %08x +4 %08x +2C(time) %08x +30 %08x +44 %08x\n",
                    semval, d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT], d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],
                    s_last_rel, devp, dv[0], dv[1], dv[0x2C/4], dv[0x30/4], dv[0x44/4]);
            fprintf(stderr, "[SEMWATCH] PGRAPH 0xB10 = %08x (sem<<2 = %08x)\n",
                    pgraph_reg_r(&d->pgraph, 0xB10), semval << 2);
            fprintf(stderr, "[SEMWATCH] q %d head %llu/%08x submit_count %u completed %u in_cb %d "
                    "pending %d/%d (%u/%u) nop %d flip %d ctx %d halt %d kick %d | title waits %d "
                    "va %08x fence %08x\n",
                    s_semq_n, s_semq_n ? (unsigned long long)s_semq[s_semq_h].submit : 0ull,
                    s_semq_n ? s_semq[s_semq_h].v : 0,
                    (unsigned)r->submit_count, (unsigned)r->completed_submits, r->in_command_buffer,
                    r->slot_pending[0], r->slot_pending[1], r->slot_submit[0], r->slot_submit[1],
                    d->pgraph.waiting_for_nop, d->pgraph.waiting_for_flip,
                    d->pgraph.waiting_for_context_switch, d->pfifo.halt, d->pfifo.fifo_kick,
                    nv2a_semwait_active, nv2a_semwait_va, nv2a_semwait_fence);
        }
    }
    return NULL;
}

static void wait_slot(PGRAPHVkState *r, int s, void *ra);

/* pfifo has nothing to do: wait for the oldest pending release's frame. */
void pgraph_vk_service_deferred_sems(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (!s_semq_n || !r) return;
    uint64_t want = s_semq[s_semq_h].submit;
    if (r->in_command_buffer && want >= r->submit_count)
        pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_SEMAPHORE);
    bool covered = false;
    for (int k = 0; k < 2; k++) {
        int s = (r->slot ^ 1 ^ k) & 1;    /* older slot first */
        if (r->slot_pending[s] && r->slot_submit[s] <= want) {
            covered = true;
            nv2a_sem_deferred_waits++;
            /* spin a little first: a sleeping fence wait wakes ~1 ms late
             * on Android, and the title is waiting on this */
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            for (;;) {
                if (vkGetFenceStatus(r->device, r->slot_fence[s]) == VK_SUCCESS) break;
                clock_gettime(CLOCK_MONOTONIC, &b);
                if ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) > 4000000L) break;
#if defined(__aarch64__)
                for (int y = 0; y < 32; y++) __asm__ volatile("yield");
#endif
            }
            wait_slot(r, s, __builtin_return_address(0));
        }
    }
    if (s_semq_n && s_semq[s_semq_h].submit < r->completed_submits)
        sem_q_release(r->completed_submits - 1);
    if (s_semq_n && !covered && !(r->in_command_buffer && s_semq[s_semq_h].submit >= r->submit_count)) {
        /* nothing on the GPU stands for it any more: write them all */
        static unsigned warned;
        if (warned++ < 20)
            fprintf(stderr, "[SEMQ] orphan release: want %llu submit_count %u completed %u "
                    "in_cb %d pending %d/%d (%llu/%llu) n %d\n",
                    (unsigned long long)s_semq[s_semq_h].submit, (unsigned)r->submit_count,
                    (unsigned)r->completed_submits, r->in_command_buffer,
                    r->slot_pending[0], r->slot_pending[1],
                    (unsigned long long)r->slot_submit[0], (unsigned long long)r->slot_submit[1],
                    s_semq_n);
        sem_q_release(UINT64_MAX);
    }
}

bool pgraph_vk_defer_semaphore(NV2AState *d, uint32_t *p, uint32_t v)
{
    static int imm = -1;
    if (imm < 0) imm = getenv("NV2A_SEM_IMMEDIATE") != NULL;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (imm || !r || r->nslots != 2 || (pg->surface_scale_factor != 1 && getenv("NV2A_NO_SCALED_RB")))
        return false;
    {   /* default: released now; the small targets drawn so far are read
         * back at the next pass break (or the submit), whatever pass is
         * open is left alone. NV2A_SEM_DEFER=1: the deferred releases. */
        static int defer = -1;
        if (defer < 0) defer = getenv("NV2A_SEM_DEFER") != NULL || nv2a_sem_guard_mode();
        if (!defer) {
            if (r->in_command_buffer) s_rb_flush = true;
            return false;
        }
    }
    if (nv2a_sem_guard_mode()) {
        static int flip_only = -1;
        if (flip_only < 0) flip_only = getenv("NV2A_GUARD_FLIP_ONLY") != NULL;
        if (r->in_command_buffer && !flip_only) {
            bool any = false;
            SurfaceBinding *sb2;
            QTAILQ_FOREACH(sb2, &r->surfaces, entry)
                if (sb2->eager_pending && sb2->download_pending && sb2->color) { any = true; break; }
            if (any) {
                if (r->in_render_pass) end_render_pass(r);
                pgraph_vk_record_eager_readback(pg, r->slot);   /* guards them */
            }
        }
        return false;   /* written now */
    }
    static long maxpix = -1;
    if (maxpix < 0) {
        const char *e = getenv("NV2A_SEM_MAXPIX");
        maxpix = e ? atol(e) : (1L << 30);
        if (getenv("NV2A_SEM_SMALL")) maxpix = 64 * 64;   /* flag sem_small */
        if (getenv("NV2A_SEM_TINY")) maxpix = 32 * 32;    /* flag sem_tiny */
        if (getenv("NV2A_SEM_MID")) maxpix = 128 * 128;   /* flag sem_mid */
    }
    bool dirty = false;
    SurfaceBinding *sb;
    QTAILQ_FOREACH(sb, &r->surfaces, entry) {
        if (sb->eager_pending && sb->download_pending && sb->color &&
            (long)sb->width * sb->height <= maxpix) {
            dirty = true;
            break;
        }
    }
    uint64_t submit;
    if (dirty && r->in_command_buffer) {
        /* the read-backs go into this command buffer right here; it is
         * submitted when pfifo runs dry (the title waiting on this release,
         * typically) or at the flip, whichever comes first */
        static int lazy = -1;
        if (lazy < 0) lazy = getenv("NV2A_SEM_LAZY") != NULL;
        if (lazy) {
            if (r->in_render_pass) end_render_pass(r);
            pgraph_vk_record_eager_readback(pg, r->slot);
            submit = r->submit_count;
        } else {
            /* submit now so the GPU starts on it while pfifo goes on */
            pgraph_vk_finish(pg, VK_FINISH_REASON_SEMAPHORE);
            submit = r->slot_submit[r->slot ^ 1];
        }
    } else if (s_semq_n) {
        submit = r->in_command_buffer ? r->submit_count
                 : s_semq[(s_semq_h + s_semq_n - 1) % SEMQ_MAX].submit;
    } else {
        return false;
    }
    if (!s_semq_n && submit < r->completed_submits)
        return false;   /* already done on the GPU */
    if (s_semq_n == SEMQ_MAX) pgraph_vk_service_deferred_sems(d);
    if (s_semq_n == SEMQ_MAX) return false;
    {   static int started;
        if (!started++) { pthread_t t; pthread_create(&t, NULL, semq_watch, d); pthread_detach(t); }
    }
    s_semq[(s_semq_h + s_semq_n) % SEMQ_MAX] = (SemQ){ p, v, submit };
    s_semq_n++;
    nv2a_sem_deferred++;
    if (submit < r->completed_submits) sem_q_release(r->completed_submits - 1);
    return true;
}

/* Slot s's frame is done on the GPU: release what it held. */
static void retire_slot(PGRAPHVkState *r, int s)
{
    NV2AState *d = g_nv2a;
    r->slot_pending[s] = false;
    ts_collect(r, s);
    gp_collect(r, s);
    destroy_slot_framebuffers(r, s);
    bitmap_clear(r->slot_used_bitmap[s], 0, r->bitmap_size);
    if (r->slot_submit[s] + 1 > r->completed_submits)
        r->completed_submits = r->slot_submit[s] + 1;
    pgraph_vk_complete_eager_readback(d, s);
    { void sem_q_release(uint64_t upto); sem_q_release(r->slot_submit[s]); }
    pgraph_vk_process_slot_reports(d, s);
    if (r->slot_budget[s]) {
        r->slot_budget[s] = false;
        pgraph_vk_check_memory_budget(&d->pgraph);
    }
    if (!r->slot_pending[s ^ 1] && !r->in_command_buffer)
        pgraph_vk_compute_finish_complete(r);
    { extern void nv2a_vtx_shadow_flush_slot(NV2AState *d, int slot);
      nv2a_vtx_shadow_flush_slot(d, s); }
    if (r->slot_eager[s]) {
        r->slot_eager[s] = false;
        s_eager_due = true;
    }
}

static void run_due_eager(PGRAPHVkState *r)
{
    if (nv2a_eager_after_wait) { nv2a_eager_after_wait = false; s_eager_due = true; }
    if (!s_eager_due || s_retire_depth) return;
    s_eager_due = false;
    s_retire_depth++;
    pgraph_vk_download_eager_surfaces(g_nv2a);
    s_retire_depth--;
}

static void wait_slot(PGRAPHVkState *r, int s, void *ra)
{
    if (!r->slot_pending[s]) return;
    struct timespec t0, t1;
    extern void nv2a_tl(int, uint32_t);
    nv2a_tl(8, s);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    VkResult wr = vkWaitForFences(r->device, 1, &r->slot_fence[s], VK_TRUE,
                                  1000000000ull);
    nv2a_tl(9, s);
    if (wr == VK_TIMEOUT) fence_hang_report(r, r->slot_fence[s], r->slot_reason[s]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    nv2a_fence_ms_acc += ms;
    note_wait_site(ra, ms);
    retire_slot(r, s);
}

/* Retire, in submission order, every slot whose frame the GPU has finished;
 * never blocks. */
/* nv2a-port: per draw, ask the GPU about the fences at most every 0.5 ms:
 * each vkGetFenceStatus is a driver ioctl on Mali, and per draw (800 a
 * frame) they were a quarter of the pfifo thread's time. */
static void poll_slots_throttled(void)
{
    static int64_t last_ns;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (now - last_ns < 500000) return;
    last_ns = now;
    pgraph_vk_poll_slots();
}

void pgraph_vk_poll_slots(void)
{
    if (!g_nv2a) return;
    PGRAPHVkState *r = g_nv2a->pgraph.vk_renderer_state;
    int other = r->slot ^ 1;
    if (r->slot_pending[other] &&
        vkGetFenceStatus(r->device, r->slot_fence[other]) == VK_SUCCESS)
        retire_slot(r, other);
    if (r->slot_pending[r->slot] &&
        vkGetFenceStatus(r->device, r->slot_fence[r->slot]) == VK_SUCCESS)
        retire_slot(r, r->slot);
    run_due_eager(r);
}

/* Everything submitted is finished and retired: the old meaning of
 * waiting for the GPU, for one-off submissions and CPU access to memory
 * the GPU may use. */
void pgraph_vk_wait_deferred(void)
{
    if (!g_nv2a) return;
    PGRAPHVkState *r = g_nv2a->pgraph.vk_renderer_state;
    void *ra = __builtin_return_address(0);
    int other = r->slot ^ 1;
    if (r->slot_pending[other]) wait_slot(r, other, ra);   /* older first */
    if (r->slot_pending[r->slot]) wait_slot(r, r->slot, ra);
    run_due_eager(r);
}

/* Objects drawn at or after this draw_time may still be used by the GPU. */
unsigned int pgraph_vk_oldest_live_draw_time(PGRAPHVkState *r)
{
    unsigned int t = r->in_command_buffer ? r->command_buffer_start_time : ~0u;
    for (int i = 0; i < 2; i++)
        if (r->slot_pending[i] && r->slot_start[i] < t) t = r->slot_start[i];
    return t;
}

/* Vertex RAM pages a submitted frame reads: writing them must wait for it. */
void pgraph_vk_slot_note_used(PGRAPHVkState *r, hwaddr addr, hwaddr size)
{
    if (!size) return;
    size_t a = addr / 4096, b = (addr + size + 4095) / 4096;
    if (b > r->bitmap_size) b = r->bitmap_size;
    if (a < b) bitmap_set(r->cb_used_bitmap, a, b - a);
}
void pgraph_vk_slot_wait_used(PGRAPHVkState *r, hwaddr addr, hwaddr size)
{
    size_t a = addr / 4096, b = (addr + size + 4095) / 4096;
    if (b > r->bitmap_size) b = r->bitmap_size;
    if (a >= b) return;
    for (int s = 0; s < 2; s++) {
        int k = (r->slot ^ 1) ^ s;   /* older first */
        if (r->slot_pending[k] &&
            find_next_bit(r->slot_used_bitmap[k], b, a) < b) {
            nv2a_site_hit("wait:vram-inflight", 0, 0, 0);
            wait_slot(r, k, __builtin_return_address(0));
        }
    }
}

static void select_slot(PGRAPHState *pg, int s)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->slot = s;
    r->command_buffer = r->slot_cmd[s];
    r->aux_command_buffer = r->slot_aux[s];
    r->command_buffer_fence = r->slot_fence[s];
    r->command_buffer_semaphore = r->slot_sem[s];
    r->query_pool = r->slot_qpool[s];
    r->descriptor_set_base = (r->nslots == 2) ? s * (int)(ARRAY_SIZE(r->descriptor_sets) / 2) : 0;
    r->descriptor_set_limit = r->descriptor_set_base +
        ((r->nslots == 2) ? (int)(ARRAY_SIZE(r->descriptor_sets) / 2)
                          : (int)ARRAY_SIZE(r->descriptor_sets));
    r->descriptor_set_index = r->descriptor_set_base;
    pgraph_vk_slot_select_buffers(r, s);
}

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_poll_slots();
    {
        static const char *names[] = { "finish:vbdirty", "finish:surfcreate", "finish:surfdown", "finish:bufspace",
                                       "finish:fbdirty", "finish:present", "finish:flip", "finish:flush",
                                       "finish:stalled", "finish:other" };
        nv2a_site_hit(names[(unsigned)finish_reason < 10 ? (unsigned)finish_reason : 9],
                      __builtin_return_address(0), 0, 0);
    }

    assert(!r->in_draw);
    assert(r->debug_depth == 0);

    if (r->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        /* the frame before must be retired (its eager read-backs done)
         * before this one is queued behind it, or those would wait for
         * this one too */
        if (r->nslots == 2 && r->slot_pending[r->slot ^ 1]) {
            wait_slot(r, r->slot ^ 1, __builtin_return_address(0));
        }
        run_due_eager(r);

        if (r->in_render_pass) {
            end_render_pass(r);
        }
        if (r->query_in_flight) {
            end_query(r);
        }
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            finish_reason == VK_FINISH_REASON_SEMAPHORE || s_rb_flush) {
            s_rb_flush = false;
            pgraph_vk_record_eager_readback(pg, r->slot);
        }
        {   extern void pgraph_vk_rb_host_barrier(PGRAPHState *pg, int slot);
            pgraph_vk_rb_host_barrier(pg, r->slot); }
        ts_end(r);
        VK_CHECK(vkEndCommandBuffer(r->command_buffer));

        {   /* nv2a-port: one flush per staging buffer per submit, this
             * slot's part only */
            const int st[] = { BUFFER_INDEX_STAGING, BUFFER_VERTEX_INLINE_STAGING,
                               BUFFER_UNIFORM_STAGING, BUFFER_SURF_UPLOAD };
            for (int k = 0; k < 4; k++) {
                StorageBuffer *b = &r->storage_buffers[st[k]];
                if (b->buffer_offset > b->slot_lo)
                    vmaFlushAllocation(r->allocator, b->allocation, b->slot_lo,
                                       b->buffer_offset - b->slot_lo);
            }
            rc_flush(r);
        }
        /* the slot's aux buffer: staging copies ahead of the frame */
        assert(!r->in_aux_command_buffer);
        VkCommandBufferBeginInfo aux_begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &aux_begin));
        VkCommandBuffer cmd = r->aux_command_buffer;
        sync_staging_buffer(pg, cmd, BUFFER_INDEX_STAGING, BUFFER_INDEX);
        sync_staging_buffer(pg, cmd, BUFFER_VERTEX_INLINE_STAGING,
                                BUFFER_VERTEX_INLINE);
        sync_staging_buffer(pg, cmd, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM);
        bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
        flush_memory_buffer(pg, cmd);
        VK_CHECK(vkEndCommandBuffer(r->aux_command_buffer));

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->aux_command_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &r->command_buffer_semaphore,
            },
            {

                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &r->command_buffer,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &r->command_buffer_semaphore,
                .pWaitDstStageMask = &wait_stage,
            }
        };
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        vkResetFences(r->device, 1, &r->command_buffer_fence);
        VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                               r->command_buffer_fence));
        { extern void nv2a_tl(int, uint32_t); nv2a_tl(7, r->slot); }

        /* the slot now belongs to the GPU until its fence */
        int s = r->slot;
        r->slot_pending[s] = true;
        r->slot_submit[s] = r->submit_count;
        r->slot_start[s] = r->command_buffer_start_time;
        r->slot_reason[s] = finish_reason;
        r->submit_count += 1;
        memcpy(r->slot_fbs[s], r->framebuffers, sizeof(VkFramebuffer) * r->framebuffer_index);
        r->slot_fb_n[s] = r->framebuffer_index;
        for (int i = 0; i < r->framebuffer_index; i++) r->framebuffers[i] = VK_NULL_HANDLE;
        r->framebuffer_index = 0;
        r->slot_nq[s] = r->num_queries_in_flight;
        r->num_queries_in_flight = 0;
        {
            QueryReport *rep;
            while ((rep = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
                QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
                QSIMPLEQ_INSERT_TAIL(&r->slot_reports[s], rep, entry);
            }
        }
        { extern void nv2a_vtx_shadow_seal(int slot); nv2a_vtx_shadow_seal(s); }
        {   unsigned long *t = r->slot_used_bitmap[s];
            r->slot_used_bitmap[s] = r->cb_used_bitmap;
            r->cb_used_bitmap = t;
            bitmap_clear(r->cb_used_bitmap, 0, r->bitmap_size); }

        // Periodically check memory budget
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {

            // VMA queries budget via vmaSetCurrentFrameIndex
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            r->slot_budget[s] = true;
        }

        r->in_command_buffer = false;
        nv2a_vk_cb_draws = nv2a_vk_cb_gs_draws = nv2a_vk_cb_clears = nv2a_vk_cb_compute = 0;
        nv2a_vk_cb_s2t = nv2a_vk_cb_texup = nv2a_vk_cb_surfup = 0;
        nv2a_cb_draws_len = 0; nv2a_cb_draws_log[0] = 0;

        /* nv2a-port: at a flip the GPU keeps the frame while the next one is
         * recorded in the other slot; anything else is a sync point and
         * waits, as xemu does. NV2A_SYNC_FLIP=1 waits at flips too. */
        static int sync_flip = -1;
        if (sync_flip < 0) sync_flip = getenv("NV2A_SYNC_FLIP") != NULL;
        bool async = (finish_reason == VK_FINISH_REASON_FLIP_STALL && !sync_flip) ||
                     finish_reason == VK_FINISH_REASON_SEMAPHORE;
        if (r->nslots == 2) {
            int n = s ^ 1;
            if (r->slot_pending[n]) wait_slot(r, n, __builtin_return_address(0));
            select_slot(pg, n);
        } else {
            select_slot(pg, s);   /* same slot: waited below or before reuse */
            if (async && r->slot_pending[s]) {
                /* one slot: the GPU may run on until the next use */
            }
        }
        if (!async)
            pgraph_vk_wait_deferred();
        return;
    }

    pgraph_vk_wait_deferred();
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_vk_process_pending_reports_internal(d);

    pgraph_vk_compute_finish_complete(r);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    /* the slot must be idle: with one slot that is a wait for the GPU */
    if (r->slot_pending[r->slot]) wait_slot(r, r->slot, __builtin_return_address(0));
    pgraph_vk_poll_slots();
    assert(!r->in_command_buffer);

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
    if (query_presplit())
        vkCmdResetQueryPool(r->command_buffer, r->query_pool, 0, r->max_queries_in_flight);
    ts_begin(r);
    gp_reset_slot(r);
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass(r);
    if (r->query_in_flight) {
        end_query(r);
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (r->in_render_pass)
        nv2a_site_hit("split:nondraw", __builtin_return_address(0), 0, 0);
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

/* False: the draw's shaders are still compiling in the background (see
 * pgraph_vk_shaders_ready); the caller drops the draw. Checked here, after
 * the vertex attributes are bound, because the shader state depends on
 * them. */
static bool begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    poll_slots_throttled();
    extern bool pgraph_vk_shaders_ready(PGRAPHState *pg);
    if (!pg->clearing && !pgraph_vk_shaders_ready(pg))
        return false;

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else if (!create_pipeline(pg)) {
        return false;   /* pipeline still building in the background */
    }

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        if (r->in_render_pass)   /* why passes are split (GPERF site report) */
            nv2a_site_hit(render_pass_dirty ? (r->framebuffer_dirty ? "split:rp+fb" : "split:rp") : "split:fb",
                          NULL, r->color_binding ? r->color_binding->width : 0,
                          r->color_binding ? r->color_binding->height : 0);
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (!pg->clearing) {
        pgraph_vk_update_descriptor_sets(pg);
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
    return true;
}

static float clamp_line_width_to_device_limits(PGRAPHState *pg, float width)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    float min_width = r->device_props.limits.lineWidthRange[0];
    float max_width = r->device_props.limits.lineWidthRange[1];
    float granularity = r->device_props.limits.lineWidthGranularity;

    if (granularity != 0.0f) {
        float steps = roundf((width - min_width) / granularity);
        width = min_width + steps * granularity;
    }
    return fminf(fmaxf(min_width, width), max_width);
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    {   /* experiment NV2A_SKIP_SHRECV=1: no projected-shadow receivers
         * (stage 0 PROJECT2D, multiplied onto the target) */
        static int on = -1;
        if (on < 0) on = getenv("NV2A_SKIP_SHRECV") != NULL;
        uint32_t bl = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
        s_skip_this_draw = on && !pg->clearing &&
            (pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG) & 0x1f) == 1 &&
            (bl & NV_PGRAPH_BLEND_EN) &&
            GET_MASK(bl, NV_PGRAPH_BLEND_SFACTOR) == NV_PGRAPH_BLEND_SFACTOR_DST_COLOR;
    }
    {   /* A/B for the striped sky (flag dome_ab): the two draws right after
         * the skybox -- a 128x512 DXT1 dome (E49) and a 4-texture layer
         * whose stage 0 is 128x128 format 0 (E363) -- skipped in turn,
         * 4 s per phase: 0 all, 1 no dome, 2 no layer, 3 neither. */
        static int ab = -1;
        if (ab < 0) ab = getenv("NV2A_DOME_AB") != NULL;
        if (ab && !pg->clearing) {
            uint32_t tf = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0);
            unsigned c = GET_MASK(tf, NV_PGRAPH_TEXFMT0_COLOR),
                     u = GET_MASK(tf, NV_PGRAPH_TEXFMT0_BASE_SIZE_U),
                     v = GET_MASK(tf, NV_PGRAPH_TEXFMT0_BASE_SIZE_V);
            bool dome = c == 12 && u == 7 && v == 9;
            bool layer = c == 0 && u == 7 && v == 7;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int ph = (int)((ts.tv_sec / 4) % 4);
            static int lastph = -1;
            if (ph != lastph) { lastph = ph; fprintf(stderr, "[AB] dome phase %d\n", ph); }
            if ((dome && (ph == 1 || ph == 3)) || (layer && (ph == 2 || ph == 3)))
                s_skip_this_draw = true;
        }
    }

    assert(r->in_command_buffer);

    // Visibility testing
    if (query_presplit() && !nv2a_env_flag("NV2A_NO_QUERY", &s_no_query)) {
        /* tests begin and end inside the pass (see query_presplit) */
        if (!pg->clearing && pg->zpass_pixel_count_enable) {
            if (r->new_query_needed && r->query_in_flight) {
                if (r->in_render_pass) vk_query_end(r);
                r->query_in_flight = false;
            }
            if (!r->query_in_flight && r->num_queries_in_flight < r->max_queries_in_flight) {
                nv2a_profile_inc_counter(NV2A_PROF_QUERY);
                r->query_in_flight = true;
                r->new_query_needed = false;
                if (r->in_render_pass) vk_query_begin(r);   /* else: by begin_render_pass */
            }
        } else if (r->query_in_flight) {
            if (r->in_render_pass) vk_query_end(r);
            r->query_in_flight = false;
        }
    } else
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (r->new_query_needed && r->query_in_flight) {
            end_render_pass(r);
            end_query(r);
        }
        if (!r->query_in_flight) {
            end_render_pass(r);
            begin_query(r);
        }
    } else if (r->query_in_flight) {
        end_render_pass(r);
        end_query(r);
    }

    if (pg->clearing) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        VkViewport viewport = {
            .width = vp_width,
            .height = vp_height,
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        {
            unsigned int fw, fh;
            nv2a_fb_dims(pg, &fw, &fh);
            if (fw && fh) {
                if (xmin > fw) xmin = fw;
                if (ymin > fh) ymin = fh;
                if (xmin + scissor_width > fw) scissor_width = fw - xmin;
                if (ymin + scissor_height > fh) scissor_height = fh - ymin;
            }
        }
        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);

        if (r->pipeline_binding->has_dynamic_line_width) {
            float line_width =
                clamp_line_width_to_device_limits(pg, pg->surface_scale_factor);
            vkCmdSetLineWidth(r->command_buffer, line_width);
        }
    }
    if (!pg->clearing) {
        float bc[4];
        pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR), bc);
        vkCmdSetBlendConstants(r->command_buffer, bc);
    }
    if (!pg->clearing && nv2a_hw_bias(r))
        set_hw_depth_bias(pg, r);

    if (!pg->clearing) {
        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
    }

    r->in_draw = true;
    if (nv2a_diag()) {   /* nv2a-port: what the last draw of the command buffer looks like. */
        char *o = nv2a_vk_last_draw; size_t n = sizeof nv2a_vk_last_draw; int k;
        k = snprintf(o, n, "prim %u clr %ux%u fmt %u zeta %ux%u tex[",
                     pg->primitive_mode,
                     r->color_binding ? r->color_binding->width : 0,
                     r->color_binding ? r->color_binding->height : 0,
                     r->color_binding ? r->color_binding->shape.color_format : 0,
                     r->zeta_binding ? r->zeta_binding->width : 0,
                     r->zeta_binding ? r->zeta_binding->height : 0);
        for (int i = 0; i < NV2A_MAX_TEXTURES && k > 0 && (size_t)k < n; i++) {
            TextureBinding *t = r->texture_bindings[i];
            if (!t) { k += snprintf(o + k, n - k, " -"); continue; }
            k += snprintf(o + k, n - k, " %ux%ux%u/f%02X/l%u/d%u%s@%llx/a%x/fl%x",
                          t->key.state.width, t->key.state.height, t->key.state.depth,
                          t->key.state.color_format, t->key.state.levels,
                          t->key.state.dimensionality, t->key.state.cubemap ? "c" : "",
                          (unsigned long long)t->key.texture_vram_offset,
                          t->key.address, t->key.filter);
        }
        if (k > 0 && (size_t)k < n && r->shader_binding) {
            PshState *ps = &r->shader_binding->state.psh;
            snprintf(o + k, n - k, " ] q%d conv %d%d%d%d shadow %d%d%d%d zp %d alpha %d prog %08X vsh %d",
                     (int)r->query_in_flight, ps->conv_tex[0], ps->conv_tex[1], ps->conv_tex[2], ps->conv_tex[3],
                     ps->shadow_map[0], ps->shadow_map[1], ps->shadow_map[2], ps->shadow_map[3],
                     ps->z_perspective, ps->alpha_test, ps->shader_stage_program,
                     (int)r->shader_binding->state.vsh.is_fixed_function);
        }
    }
    if (pg->clearing) nv2a_vk_cb_clears++;
    else {
        nv2a_vk_cb_draws++;
        if (r->shader_binding && r->shader_binding->geom.module_info) nv2a_vk_cb_gs_draws++;
    }
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

    pgraph_vk_flush_draw(d);
    pgraph_vsh_carryover_update(pg);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size, start_addr,
                        end_addr - start_addr);

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;
        pgraph_vk_slot_note_used(r, addr, size);   /* this frame reads it */

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

        static int ab_vtx = -1;
        if (ab_vtx < 0) ab_vtx = getenv("NV2A_AB_VTX") != NULL;
        int force = 0;
        if (ab_vtx) {   /* A/B: re-upload every vertex range for 5 s, then 5 s normal */
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            force = (ts.tv_sec / 5) & 1;
            static int last = -1;
            if (last != force) { last = force; fprintf(stderr, "[AB] vtx_force=%d\n", force); }
        }
        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A) || force) {
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            pgraph_vk_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                               size);
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    {
        int dm = nv2a_dbg_mode_now();
        SurfaceBinding *cb = r->color_binding;
        if ((dm & 64) && (parameter & NV097_CLEAR_SURFACE_COLOR) &&
            (!cb || cb->width >= 640))
            parameter &= ~NV097_CLEAR_SURFACE_COLOR;
        if ((dm & 128) && (!cb || cb->width >= 640))
            parameter &= ~(NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
        if (!(parameter & (NV097_CLEAR_SURFACE_COLOR | NV097_CLEAR_SURFACE_Z |
                           NV097_CLEAR_SURFACE_STENCIL)))
            return;
        write_color = parameter & NV097_CLEAR_SURFACE_COLOR;
        write_zeta = parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL);
    }
    pg->clearing = true;

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    r->clear_parameter = parameter;
    if (nv2a_framelog_on)
        fprintf(stderr, "[FRAME] CLEAR %08x param=%08x col=%08x z=%08x clearcolor=%08x rect=%08x,%08x\n",
                r->color_binding ? (unsigned)r->color_binding->vram_addr : 0, parameter,
                r->color_binding ? r->color_binding->width : 0,
                r->zeta_binding ? (unsigned)r->zeta_binding->vram_addr : 0,
                pgraph_reg_r(pg, NV_PGRAPH_COLORCLEARVALUE),
                pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY));
    if (nv2a_framelog_on)
        fprintf(stderr, "[FRAME]   zsclear=%08x zfmt=%d/%d\n", pgraph_reg_r(pg, NV_PGRAPH_ZSTENCILCLEARVALUE),
                pg->surface_shape.zeta_format, pg->surface_shape.z_format);

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    {   /* whole-target clear that may open a pass: make it the load op */
        static int off = -1;
        if (off < 0) off = getenv("NV2A_NO_RP_LOADCLEAR") != NULL;
        s_pend_clear_mask = 0; s_pend_clear_used = 0;
        bool full = xmin == 0 && ymin == 0 && xmax + 1 >= binding->width &&
                    ymax + 1 >= binding->height &&
                    pg->surface_binding_dim.width == binding->width &&
                    pg->surface_binding_dim.height == binding->height &&
                    !r->in_render_pass && !off;
        if (full) {
            if (write_color && r->color_binding &&
                (parameter & NV097_CLEAR_SURFACE_COLOR) ==
                (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
                 NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A)) {
                pgraph_get_clear_color(pg, s_pend_clear_vals[0].color.float32);
                s_pend_clear_mask |= 1;
            }
            if (write_zeta && r->zeta_binding && (parameter & NV097_CLEAR_SURFACE_Z) &&
                ((parameter & NV097_CLEAR_SURFACE_STENCIL) ||
                 !(r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT))) {
                int sv = 0; float dv = 1.0f;
                pgraph_get_clear_depth_stencil_value(pg, &dv, &sv);
                s_pend_clear_vals[1].depthStencil.depth = dv;
                s_pend_clear_vals[1].depthStencil.stencil = sv;
                s_pend_clear_mask |= 2;
            }
            /* a pass the clear does not cover in full keeps its LOAD */
            if ((r->color_binding && !(s_pend_clear_mask & 1)) ||
                (r->zeta_binding && !(s_pend_clear_mask & 2)))
                s_pend_clear_mask = 0;
        }
    }
    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);
    int done_mask = s_pend_clear_used;
    s_pend_clear_mask = 0; s_pend_clear_used = 0;

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding && !(done_mask & 1)) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding && !(done_mask & 2)) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

/* nv2a-port: persistent cache of repacked vertex attributes.
 *
 * Attributes Mali can't fetch as the guest laid them out are repacked by
 * the CPU (copy_remapped_attributes_to_inline_buffer). That copy was redone
 * every frame -- the staging space it lives in is reused by the next frame
 * -- ~8% of the pfifo thread for meshes that never change. This keeps the
 * repacked copies in BUFFER_VERTEX_REMAP, a host-memory vertex buffer the
 * GPU reads in place, from frame to frame: an entry stays valid while no
 * page it was copied from has been written since (nv2a_page_wepoch).
 * The buffer is two halves used in turn; a half is reused (and its entries
 * dropped) only once the GPU has finished every frame that read from it.
 * NV2A_NO_RCACHE=1: off. */
extern uint32_t *nv2a_page_wepoch;
extern volatile uint32_t nv2a_wepoch_gen;
typedef struct {
    hwaddr src; uint32_t ost, nst, cp, n;
    uint32_t off, epoch, vgen; int32_t next; uint8_t half;
} RcEnt;
enum { RC_MAX = 16384, RC_HASH = 8192 };
static RcEnt *s_rc;
static int32_t s_rc_head[RC_HASH];
static int s_rc_n, s_rc_on = -1, s_rc_half;
static uint32_t s_rc_pos, s_rc_maxsub[2];
static bool s_rc_used[2];
static uint32_t s_rc_dirty_lo[2] = { ~0u, ~0u }, s_rc_dirty_hi[2];
static uint16_t s_rc_map;            /* attributes of this draw served from it */
static uint64_t s_rc_hit_b, s_rc_copy_b, s_rc_fall_b, s_rc_hits, s_rc_copies;
#define RC_H(a) ((uint32_t)(((uint64_t)(a) * 0x9E3779B97F4A7C15ull) >> 51) & (RC_HASH - 1))

static size_t rc_half_size(PGRAPHVkState *r)
{
    return (r->storage_buffers[BUFFER_VERTEX_REMAP].buffer_size / 2) & ~(size_t)0xFFF;
}

/* keep only the entries outside half h; rebuild the hash */
static int s_rc_cnt[2];   /* entries per half */
static void rc_drop_half(int h)
{
    int j = 0;
    s_rc_cnt[0] = s_rc_cnt[1] = 0;
    for (int i = 0; i < s_rc_n; i++)
        if (s_rc[i].half != h && s_rc[i].src != (hwaddr)-1) {
            s_rc_cnt[s_rc[i].half]++;
            s_rc[j++] = s_rc[i];
        }
    s_rc_n = j;
    memset(s_rc_head, 0xff, sizeof s_rc_head);
    for (int i = 0; i < s_rc_n; i++) {
        uint32_t k = RC_H(s_rc[i].src);
        s_rc[i].next = s_rc_head[k]; s_rc_head[k] = i;
    }
}

static bool rc_on(void)
{
    if (s_rc_on < 0) {
        s_rc_on = !getenv("NV2A_NO_RCACHE") && nv2a_page_wepoch;
        if (s_rc_on) {
            s_rc = g_new0(RcEnt, RC_MAX);
            memset(s_rc_head, 0xff, sizeof s_rc_head);
        }
    }
    return s_rc_on;
}

/* the newest write generation of any page in [a, a + n) */
static uint32_t rc_span_epoch(hwaddr a, hwaddr n)
{
    uint32_t m = 0;
    for (hwaddr p = a >> 12, e = (a + n - 1) >> 12; p <= e; p++) {
        uint32_t v = __atomic_load_n(&nv2a_page_wepoch[p], __ATOMIC_ACQUIRE);
        if (v > m) m = v;
    }
    return m;
}

static RcEnt *rc_find(hwaddr src, uint32_t ost, uint32_t nst, uint32_t cp, uint32_t n)
{
    for (int k = s_rc_head[RC_H(src)]; k >= 0; k = s_rc[k].next) {
        RcEnt *e = &s_rc[k];
        if (e->src == src && e->ost == ost && e->nst == nst && e->cp == cp && e->n >= n)
            return e;
    }
    return NULL;
}

/* room for size bytes; false: not now (the caller repacks per frame) */
static bool rc_alloc(PGRAPHVkState *r, uint32_t size, uint32_t *off)
{
    size_t half = rc_half_size(r);
    size = (size + 15) & ~15u;
    if (size > half / 8) return false;
    /* a half is also full at half the entry table (many small entries) */
    if (s_rc_pos + size > half || s_rc_cnt[s_rc_half] >= RC_MAX / 2 || s_rc_n >= RC_MAX) {
        int other = s_rc_half ^ 1;
        if (s_rc_used[other] && !(r->completed_submits > s_rc_maxsub[other]))
            return false;   /* the GPU may still read it */
        rc_drop_half(other);
        s_rc_used[other] = false;
        s_rc_half = other; s_rc_pos = 0;
        if (s_rc_n >= RC_MAX) return false;
    }
    *off = (uint32_t)(s_rc_half * half) + s_rc_pos;
    s_rc_pos += size;
    if (*off < s_rc_dirty_lo[s_rc_half]) s_rc_dirty_lo[s_rc_half] = *off;
    if (*off + size > s_rc_dirty_hi[s_rc_half]) s_rc_dirty_hi[s_rc_half] = *off + size;
    return true;
}

static void rc_note_use(PGRAPHVkState *r, int half)
{
    s_rc_used[half] = true;
    s_rc_maxsub[half] = r->submit_count;   /* the command buffer being recorded */
}

/* written ranges to the device, once per submit (non-coherent memory) */
static void rc_flush(PGRAPHVkState *r)
{
    for (int h = 0; h < 2; h++) {
        if (s_rc_dirty_hi[h] > s_rc_dirty_lo[h])
            vmaFlushAllocation(r->allocator, r->storage_buffers[BUFFER_VERTEX_REMAP].allocation,
                               s_rc_dirty_lo[h], s_rc_dirty_hi[h] - s_rc_dirty_lo[h]);
        s_rc_dirty_lo[h] = ~0u; s_rc_dirty_hi[h] = 0;
    }
    static time_t t0; time_t tn = time(NULL);
    if (!t0) t0 = tn;
    if (s_rc_on == 1 && tn - t0 >= 10) {
        double sec = (double)(tn - t0);
        fprintf(stderr, "[RCACHE] per second: %.0f reused (%.1f MB), %.0f copied (%.1f MB), %.1f MB repacked per frame; %d entries\n",
                s_rc_hits / sec, s_rc_hit_b / sec / 1048576.0, s_rc_copies / sec, s_rc_copy_b / sec / 1048576.0,
                s_rc_fall_b / sec / 1048576.0, s_rc_n);
        s_rc_hits = s_rc_copies = s_rc_hit_b = s_rc_copy_b = s_rc_fall_b = 0; t0 = tn;
    }
}

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        if (inline_map != 0xffff && (s_rc_map & (1 << attr_idx)))
            buffer_idx = BUFFER_VERTEX_REMAP;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0,
                           r->num_active_vertex_binding_descriptions, buffers,
                           offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        { extern uint32_t nv2a_surf_gen;
          if (color && !r->color_binding->draw_dirty) nv2a_surf_gen++; }
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        { extern uint32_t nv2a_surf_gen;
          if (zeta && !r->zeta_binding->draw_dirty) nv2a_surf_gen++; }
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1)) {
        static unsigned nlog;
        if (nlog++ < 30) {
            StorageBuffer *b = &pg->vk_renderer_state->storage_buffers[index];
            fprintf(stderr, "[BUFSPACE] buffer %d full: %llu used of %llu, %llu wanted\n", index,
                    (unsigned long long)(b->buffer_offset - b->slot_lo),
                    (unsigned long long)(b->slot_hi - b->slot_lo), (unsigned long long)size);
        }
        { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        /* data of this attribute that the RAM buffer does not hold yet */
        extern bool nv2a_vtx_shadowed(hwaddr addr, hwaddr size);
        bool shadowed = nv2a_vtx_shadowed(r->vertex_attribute_offsets[attr_id],
                                          (hwaddr)desc->stride * num_vertices + element_size * element_count);
        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);
        /* nv2a-port: Mali fetches 16/8-bit attributes from addresses or
         * strides that are not 4-byte multiples wrongly (Conker's S32K
         * positions, stride 6, came out as garbage and every world
         * triangle was clipped). Repack those, and every attribute read
         * through a widened format, tightly at 4-byte alignment.
         * NV2A_NO_REPACK=1 turns this off. */
        static int repack = -1;
        if (repack < 0)
            repack = NV2A_MOBILE_PATH(r) && !getenv("NV2A_NO_REPACK");
        bool mali_bad = repack &&
            ((r->vertex_attribute_offsets[attr_id] % 4) || (desc->stride % 4) ||
             (pg->w1_attrs & (1 << attr_id)));

        if (offset_valid && stride_valid && !mali_bad && !shadowed) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, MAX(element_size, 4));
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = ROUND_UP(element_size * element_count, 4);

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    s_rc_map = 0;   /* before any return: a stale map sent this draw's
                     * attributes to the cache buffer (wrong vertices) */
    if (!remap.attributes) {
        return;
    }

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 256));

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // FIXME: Account for only what is drawn
    assert(start_vertex == 0);
    poll_slots_throttled();
    assert(buffer->mapped);

    /* nv2a-port: per-command-buffer cache of repacked attribute streams.
     * A skinned mesh is drawn in many small batches that index the same
     * vertex buffer, and each batch re-packed the whole buffer (0..max
     * index) -- a fifth of the PFIFO thread in Conker's bar. Reused while
     * the guest has not written the source since (a dirty client of its
     * own, DIRTY_MEMORY_MIGRATION, which nothing else here uses).
     * NV2A_NO_REMAP_CACHE=1 disables. */
    enum { VC_MAX = 512 };
    static struct {
        hwaddr src; uint32_t old_stride, new_stride, cp, n; VkDeviceSize off;
        uint32_t dgen; int16_t next;
    } vc[VC_MAX];
    /* nv2a-port: hashed by source address (the linear scan over up to 512
     * entries ran per attribute per draw: ~800 draws a frame in the heavy
     * scenes), and an entry validated since the last guest write anywhere
     * (nv2a_tex_dirty_gen unchanged) skips the dirty-bitmap walk. */
    enum { VC_HASH = 1024 };
    static int16_t vc_head[VC_HASH];
    static int vc_n;
    static uint32_t vc_gen;
    extern uint32_t nv2a_inline_gen;
    extern volatile uint32_t nv2a_tex_dirty_gen;
    static int vc_on = -1, vc_dg = 1;
    if (vc_on < 0) { vc_on = getenv("NV2A_NO_REMAP_CACHE") ? 0 : 2;
                     vc_dg = !getenv("NV2A_NO_DIRTY_GEN"); }
    if (vc_gen != nv2a_inline_gen || vc_on == 2) {
        if (vc_on == 2) vc_on = 1;
        vc_gen = nv2a_inline_gen; vc_n = 0;
        memset(vc_head, 0xff, sizeof(vc_head));
    }
#define VC_H(a) ((uint32_t)(((uint64_t)(a) * 0x9E3779B97F4A7C15ull) >> 54) & (VC_HASH - 1))

    // Copy vertex data
    s_rc_map = 0;
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        hwaddr src = r->vertex_attribute_offsets[attr_id];
        uint32_t ost = remap.map[attr_id].old_stride;
        uint32_t nst = remap.map[attr_id].new_stride;
        size_t cp = MIN(nst, ost);
        size_t pad = nst - cp;

        if (num_vertices && rc_on()) {
            hwaddr span = (hwaddr)ost * (num_vertices - 1) + cp;
            RcEnt *e = rc_find(src, ost, nst, (uint32_t)cp, num_vertices);
            if (e) {
                uint32_t g = __atomic_load_n(&nv2a_wepoch_gen, __ATOMIC_ACQUIRE);
                hwaddr espan = (hwaddr)ost * (e->n - 1) + cp;
                if (e->vgen == g || rc_span_epoch(src, espan) <= e->epoch) {
                    e->vgen = g;
                    r->vertex_attribute_offsets[attr_id] = e->off;
                    s_rc_map |= 1 << attr_id;
                    rc_note_use(r, e->half);
                    s_rc_hits++; s_rc_hit_b += (uint64_t)nst * num_vertices;
                    continue;
                }
                e->src = (hwaddr)-1;   /* stale: never matches again */
            }
            uint32_t off;
            if (src + span <= memory_region_size(d->vram) &&
                rc_alloc(r, nst * num_vertices, &off)) {
                /* arm the write watch on these pages, then note the
                 * generation, then copy: a write from here on is newer */
                memory_region_test_and_clear_dirty(d->vram, src, span, DIRTY_MEMORY_MIGRATION);
                uint32_t ep = __atomic_load_n(&nv2a_wepoch_gen, __ATOMIC_ACQUIRE);
                uint8_t *out = r->storage_buffers[BUFFER_VERTEX_REMAP].mapped + off;
                const uint8_t *in = d->vram_ptr + src;
                if (!pad && (cp == 4 || cp == 8 || cp == 12 || cp == 16) && !(ost & 3)) {
                    const uint32_t *ip = (const uint32_t *)in;
                    uint32_t *op = (uint32_t *)out;
                    uint32_t w = cp / 4, is = ost / 4;
                    for (uint32_t v = 0; v < num_vertices; v++, ip += is, op += w)
                        for (uint32_t k = 0; k < w; k++) op[k] = ip[k];
                } else {
                    for (uint32_t v = 0; v < num_vertices; v++) {
                        memcpy(out, in, cp);
                        if (pad) memset(out + cp, 0, pad);
                        out += nst; in += ost;
                    }
                }
                int k = s_rc_n++;
                s_rc_cnt[s_rc_half]++;
                s_rc[k] = (RcEnt){ .src = src, .ost = ost, .nst = nst, .cp = (uint32_t)cp,
                                   .n = num_vertices, .off = off, .epoch = ep, .vgen = 0,
                                   .half = (uint8_t)s_rc_half };
                uint32_t hh = RC_H(src);
                s_rc[k].next = s_rc_head[hh]; s_rc_head[hh] = k;
                r->vertex_attribute_offsets[attr_id] = off;
                s_rc_map |= 1 << attr_id;
                rc_note_use(r, s_rc_half);
                s_rc_copies++; s_rc_copy_b += (uint64_t)nst * num_vertices;
                continue;
            }
            s_rc_fall_b += (uint64_t)nst * num_vertices;
        }

        uint32_t vc_armgen = 0;
        if (vc_on && num_vertices) {
            int hit = -1;
            for (int k = vc_head[VC_H(src)]; k >= 0; k = vc[k].next)
                if (vc[k].src == src && vc[k].old_stride == ost &&
                    vc[k].new_stride == nst && vc[k].cp == cp &&
                    vc[k].n >= num_vertices) { hit = k; break; }
            if (hit >= 0) {
                uint32_t g = __atomic_load_n(&nv2a_tex_dirty_gen, __ATOMIC_ACQUIRE);
                hwaddr span = (hwaddr)ost * (vc[hit].n - 1) + cp;
                if ((vc_dg && vc[hit].dgen == g) ||
                    !memory_region_test_and_clear_dirty(d->vram, src, span,
                                                        DIRTY_MEMORY_MIGRATION)) {
                    vc[hit].dgen = g;
                    r->vertex_attribute_offsets[attr_id] = vc[hit].off;
                    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4);
                    continue;
                }
                vc[hit].src = (hwaddr)-1;   /* stale: never matches again */
            }
            /* arm the watch before reading what it guards */
            vc_armgen = __atomic_load_n(&nv2a_tex_dirty_gen, __ATOMIC_ACQUIRE);
            memory_region_test_and_clear_dirty(d->vram, src,
                                               (hwaddr)ost * (num_vertices - 1) + cp,
                                               DIRTY_MEMORY_MIGRATION);
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + src;

        if (!pad && (cp == 4 || cp == 8 || cp == 12 || cp == 16) && !(ost & 3)) {
            /* the common shapes, without a libc call per vertex */
            const uint32_t *ip = (const uint32_t *)in_ptr;
            uint32_t *op = (uint32_t *)out_ptr;
            uint32_t w = cp / 4, is = ost / 4;
            for (uint32_t v = 0; v < num_vertices; v++, ip += is, op += w)
                for (uint32_t k = 0; k < w; k++) op[k] = ip[k];
        } else {
            for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
                memcpy(out_ptr, in_ptr, cp);
                if (pad) memset(out_ptr + cp, 0, pad);
                out_ptr += nst;
                in_ptr += ost;
            }
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;

        if (vc_on && num_vertices) {
            if (vc_n >= VC_MAX) {   /* full: start over for this cb */
                vc_n = 0; memset(vc_head, 0xff, sizeof(vc_head));
            }
            int k = vc_n++;
            vc[k].src = src; vc[k].old_stride = ost; vc[k].new_stride = nst;
            vc[k].cp = (uint32_t)cp; vc[k].n = num_vertices;
            vc[k].off = attr_buffer_offset;
            /* the arm consumed any write before it; one after it bumps gen */
            vc[k].dgen = vc_armgen;
            uint32_t h = VC_H(src);
            vc[k].next = vc_head[h]; vc_head[h] = (int16_t)k;
        }
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

static void flush_draw_impl(NV2AState *d);
void pgraph_vk_flush_draw(NV2AState *d)
{
    flush_draw_impl(d);
    static int drawstat = -1, idx, was_on;
    static SurfaceBinding *last_c; static int last_prog = -2;
    if (drawstat < 0) drawstat = getenv("NV2A_DRAWSTAT") != NULL;
    if (drawstat && nv2a_framelog_on) {
        PGRAPHVkState *r = d->pgraph.vk_renderer_state;
        if (!was_on) { idx = 0; was_on = 1; last_c = NULL; }
        idx++;
        SurfaceBinding *c = r->color_binding;
        int prog = r->shader_binding ? r->shader_binding->state.vsh.programmable.program_length : -1;
        bool change = c != last_c || prog != last_prog;
        /* stat the previous target when the target changes, and the current
         * one every few draws and on program changes */
        if (c != last_c && last_c && last_c->width >= 640)
            pgraph_vk_stat_surface(d, last_c, "prev", idx);
        if (c && c->width >= 640 && (change || idx % 8 == 0 || idx < 30 ||
                                     (r->shader_binding && (r->shader_binding->state.psh.dbg & 256))))
            pgraph_vk_stat_surface(d, c, "draw", idx);
        last_c = c; last_prog = prog;
    } else {
        was_on = 0;
    }
}
/* nv2a-port: Conker's depth-of-field / soft-focus: the finished frame is
 * shrunk into a small target (128x128) and that is drawn back over the
 * frame, blurring the background in many shots. The draw back is skipped
 * (sharp picture): a draw onto a frame-sized target whose texture 0 is a
 * small target that was itself drawn from a frame-sized one. NV2A_KEEP_DOF=1
 * keeps it. */
unsigned nv2a_dof_skipped;
static uint32_t tex_phys(NV2AState *d, int i)
{
    PGRAPHState *pg = &d->pgraph;
    if (!(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) & NV_PGRAPH_TEXCTL0_0_ENABLE)) return 0;
    uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
    hwaddr off = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4), len;
    uint8_t *base = (uint8_t *)nv_dma_map(d, GET_MASK(fmt, NV_PGRAPH_TEXFMT0_CONTEXT_DMA) ?
                                          pg->dma_b : pg->dma_a, &len);
    if (!base || off >= len) return 0;
    return (uint32_t)(base + off - d->vram_ptr);
}
/* The depth-of-field pass (from a frame log of the Berri phone scene): one
 * draw onto the frame-sized colour target, no depth buffer bound, whose
 * texture 0 is a frame-sized depth buffer (the scene's depth, read as a
 * texture) and whose other stages are the small blurred copies of the
 * frame. It blends them in by depth. Skipping it leaves the sharp frame. */
static bool nv2a_skip_dof_composite(NV2AState *d)
{
    static int keep = -1;
    if (keep < 0) keep = getenv("NV2A_KEEP_DOF") != NULL;
    if (keep) return false;
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *c = r->color_binding;
    if (!c || c->width < 600 || r->zeta_binding) return false;
    uint32_t t0 = tex_phys(d, 0);
    if (!t0) return false;
    SurfaceBinding *z = pgraph_vk_surface_get(d, t0);
    if (!z || z->color || z->width < 600) return false;
    bool small = false;
    for (int i = 1; i < 4 && !small; i++) {
        uint32_t t = tex_phys(d, i);
        SurfaceBinding *sb = t ? pgraph_vk_surface_get(d, t) : NULL;
        if (sb && sb->color && sb->width <= 256 && sb->height <= 256) small = true;
    }
    if (!small) return false;
    nv2a_dof_skipped++;
    return true;
}


/* nv2a-port: speech bubbles keep their shape on a wide screen. The title
 * draws its 4:3 frame and the presenter stretches it to the screen (the 3D
 * cameras are widened to match), so flat 2D gets wide: the speech bubbles and
 * their letters look stretched. A bubble is a single screen-space quad with
 * its own texture, drawn just before its text; the text is one quad per
 * letter from the speech font (256x240, format 19). Both are pulled together
 * around the bubble's own centre, so a bubble stays over its speaker. Which
 * textures are bubbles is learned from the text: the quad drawn last before
 * a letter that encloses it. Everything else 2D (menus, fades, the HUD) is
 * left as it is. NV2A_NO_BUBBLE_SQUEEZE=1 turns it off. */
static struct {
    uint32_t tex[8]; int ntex;
    uint32_t hist_tex[6]; float hist[6][4]; unsigned hist_seq[6]; int hn;   /* recent 2D quads (candidates) */
    float act[4]; float cx; unsigned act_seq; bool act_on;   /* the bubble of this frame */
    uint32_t tail[4]; int ntail;   /* textures of the pointers to the speaker */
    uint32_t epoch;
} s_bub;
volatile int64_t nv2a_bubble_ms;   /* last speech bubble drawn (CLOCK_MONOTONIC ms) */
static void quad_bbox(const uint8_t *base, unsigned vsz, unsigned n, float bb[4])
{
    bb[0] = bb[2] = 1e30f; bb[1] = bb[3] = -1e30f;
    for (unsigned v = 0; v < n; v++) {
        const float *f = (const float *)(base + v * vsz);
        if (f[0] < bb[0]) bb[0] = f[0];
        if (f[0] > bb[1]) bb[1] = f[0];
        if (f[1] < bb[2]) bb[2] = f[1];
        if (f[1] > bb[3]) bb[3] = f[1];
    }
}
static void squeeze_x(uint8_t *base, unsigned vsz, unsigned n, float cx, float k)
{
    for (unsigned v = 0; v < n; v++) {
        float *f = (float *)(base + v * vsz);
        f[0] = cx + (f[0] - cx) * k;
    }
}
static void nv2a_bubble_squeeze(PGRAPHState *pg, unsigned vsz, unsigned n)
{
    static int off = -1;
    if (off < 0) off = getenv("NV2A_NO_BUBBLE_SQUEEZE") != NULL;
    extern float nv2a_present_2d_squeeze(void);
    float k = nv2a_present_2d_squeeze();
    if (off || k > 0.99f || n != 4) return;
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *c = r->color_binding;
    VertexAttribute *a0 = &pg->vertex_attributes[0];
    if (!c || c->width < 600 || c->swizzle || a0->size != 4 || a0->count < 3 || vsz < 12) return;
    TextureBinding *t0 = r->texture_bindings[0];
    if (!t0 || t0 == &r->dummy_texture) return;
    uint8_t *base = (uint8_t *)pg->inline_array + a0->inline_array_offset;
    float bb[4]; quad_bbox(base, vsz, n, bb);
    if (!(bb[1] - bb[0] > 0.5f) || bb[0] < -64 || bb[1] > 704 || bb[2] < -64 || bb[3] > 544) return;
    /* flat (screen-space) quads only: equal z everywhere */
    for (unsigned v = 1; v < n; v++)
        if (((const float *)(base + v * vsz))[2] != ((const float *)base)[2]) return;
    extern volatile unsigned nv2a_flip_seq;
    extern volatile uint32_t nv2a_tex_rehash_epoch;
    unsigned seq = nv2a_flip_seq;
    if (s_bub.epoch != nv2a_tex_rehash_epoch) { s_bub.epoch = nv2a_tex_rehash_epoch; s_bub.ntex = 0; s_bub.ntail = 0; }
    if (s_bub.act_seq != seq) s_bub.act_on = false;
    uint32_t tex = (uint32_t)t0->key.texture_vram_offset;
    bool font = t0->key.state.width == 256 && t0->key.state.height == 240 &&
                (int)t0->key.state.color_format == 19;
    float mx = (bb[0] + bb[1]) * 0.5f, my = (bb[2] + bb[3]) * 0.5f;
    if (font) {
        /* learn: the latest recent quad enclosing this letter is a bubble */
        if (!s_bub.act_on) {
            for (int h = 0; h < 6; h++) {
                int i = (s_bub.hn - 1 - h + 60) % 6;
                float *q = s_bub.hist[i];
                if (s_bub.hist_seq[i] != seq || !s_bub.hist_tex[i]) continue;
                if (!(mx > q[0] && mx < q[1] && my > q[2] && my < q[3] && q[1] - q[0] > 60 && q[3] - q[2] > 24 &&
                      q[1] - q[0] < 560 && q[3] - q[2] < 400)) continue;   /* not a full-screen panel */
                bool known = false;
                for (int j = 0; j < s_bub.ntex; j++) if (s_bub.tex[j] == s_bub.hist_tex[i]) known = true;
                if (!known && s_bub.ntex < 8) {
                    s_bub.tex[s_bub.ntex++] = s_bub.hist_tex[i];
                    fprintf(stderr, "[BUBBLE] speech bubble texture %08x (%.0f..%.0f x %.0f..%.0f)\n",
                            s_bub.hist_tex[i], q[0], q[1], q[2], q[3]);
                }
                break;
            }
        }
        if (s_bub.act_on && mx > s_bub.act[0] && mx < s_bub.act[1] && my > s_bub.act[2] && my < s_bub.act[3])
            squeeze_x(base, vsz, n, s_bub.cx, k);   /* its letters */
        return;
    }
    /* The game places a bubble and its pointer from where the speaker is
     * on a 4:3 screen, but the widened camera draws everyone closer to the
     * middle of the frame (x - centre shrinks by k): a speaker off to one
     * side had the bubble far out beside them. So the whole bubble -- the
     * pointer, the bubble, its letters and icons -- is mapped the same way
     * around the frame's centre: it lands by the speaker, narrowed to its
     * own shape, and the pieces stay joined. */
    float cx0 = c->width * 0.5f;
    bool bubble = false;
    for (int i = 0; i < s_bub.ntex; i++) if (s_bub.tex[i] == tex) bubble = true;
    bool tail = false;
    for (int i = 0; i < s_bub.ntail; i++) if (s_bub.tail[i] == tex) tail = true;
    if (bubble && bb[1] - bb[0] > 60 && bb[3] - bb[2] > 24 && bb[1] - bb[0] < 560 && bb[3] - bb[2] < 400) {
        /* learn the pointer: its own quad, drawn just before the bubble and
         * touching it */
        int i = (s_bub.hn - 1 + 60) % 6;
        float *q = s_bub.hist[i];
        if (s_bub.hn && s_bub.hist_seq[i] == seq && s_bub.hist_tex[i] != tex && s_bub.hist_tex[i] &&
            q[1] - q[0] < (bb[1] - bb[0]) * 0.6f && q[3] - q[2] < 300 &&
            q[1] > bb[0] - 4 && q[0] < bb[1] + 4 && q[3] > bb[2] - 4 && q[2] < bb[3] + 4) {
            bool known = false;
            for (int j = 0; j < s_bub.ntail; j++) if (s_bub.tail[j] == s_bub.hist_tex[i]) known = true;
            if (!known && s_bub.ntail < 4) {
                s_bub.tail[s_bub.ntail++] = s_bub.hist_tex[i];
                fprintf(stderr, "[BUBBLE] pointer texture %08x (%.0f..%.0f x %.0f..%.0f)\n",
                        s_bub.hist_tex[i], q[0], q[1], q[2], q[3]);
            }
        }
        memcpy(s_bub.act, bb, sizeof bb);
        s_bub.cx = cx0; s_bub.act_seq = seq; s_bub.act_on = true;
        { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
          nv2a_bubble_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000; }
        squeeze_x(base, vsz, n, cx0, k);
    } else if (tail && !bubble && bb[1] - bb[0] < 300 && bb[3] - bb[2] < 300 &&
               seq - s_bub.act_seq <= 2) {
        squeeze_x(base, vsz, n, cx0, k);   /* the pointer (drawn before its bubble) */
    } else if (s_bub.act_on && mx > s_bub.act[0] && mx < s_bub.act[1] &&
               my > s_bub.act[2] && my < s_bub.act[3] && bb[1] - bb[0] < (s_bub.act[1] - s_bub.act[0]) * 0.5f) {
        squeeze_x(base, vsz, n, s_bub.cx, k);   /* an icon inside the bubble */
    }
    if (!bubble) {
        int i = s_bub.hn % 6; s_bub.hn = i + 1;
        s_bub.hist_tex[i] = tex; memcpy(s_bub.hist[i], bb, sizeof bb); s_bub.hist_seq[i] = seq;
    }
}

static void flush_draw_impl(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return;
    }
    if (nv2a_skip_dof_composite(d)) return;

    r->num_vertex_ram_buffer_syncs = 0;

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        uint32_t *quad_idx = NULL;
        size_t quad_n = 0;
        if (nv2a_quads_as_tris(pg)) {
            size_t total = 0;
            for (int i = 0; i < pg->draw_arrays_length; i++) total += pg->draw_arrays_count[i];
            quad_idx = g_malloc(sizeof(uint32_t) * (total / 4 * 6 + 6));
            for (int i = 0; i < pg->draw_arrays_length; i++)
                quad_n += nv2a_emit_quad_indices(quad_idx + quad_n, NULL,
                                                 pg->draw_arrays_start[i],
                                                 pg->draw_arrays_count[i]);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING, quad_n * sizeof(uint32_t) + 4);
        }

        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);

        if (!begin_pre_draw(pg)) { g_free(quad_idx); NV2A_VK_DGROUP_END(); return; }
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);
        VkDeviceSize quad_off = 0;
        if (quad_idx && quad_n)
            quad_off = pgraph_vk_update_index_buffer(pg, quad_idx, quad_n * sizeof(uint32_t));
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        if (quad_idx) {
            if (quad_n) {
                vkCmdBindIndexBuffer(r->command_buffer,
                                     r->storage_buffers[BUFFER_INDEX].buffer,
                                     quad_off, VK_INDEX_TYPE_UINT32);
                { nv2a_note_draw(pg, quad_n, "Q"); vkCmdDrawIndexed(r->command_buffer, quad_n, 1, 0, 0, 0); }
            }
            g_free(quad_idx);
        } else
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            uint32_t start = pg->draw_arrays_start[i],
                     count = pg->draw_arrays_count[i];
            NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
            if (!nv2a_skip_gs_draw(r)) { nv2a_note_draw(pg, count, "A"); vkCmdDraw(r->command_buffer, count, 1, start, 0); }
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        uint32_t *elems = pg->inline_elements;
        size_t elems_n = pg->inline_elements_length;
        uint32_t *quad_idx = NULL;
        if (nv2a_quads_as_tris(pg)) {
            quad_idx = g_malloc(sizeof(uint32_t) * (elems_n / 4 * 6 + 6));
            elems_n = nv2a_emit_quad_indices(quad_idx, pg->inline_elements, 0,
                                             pg->inline_elements_length);
            elems = quad_idx;
        }
        size_t index_data_size = elems_n * sizeof(uint32_t);

        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size + 4);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);

        if (!begin_pre_draw(pg)) { g_free(quad_idx); NV2A_VK_DGROUP_END(); return; }
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);
        VkDeviceSize buffer_offset = index_data_size ?
            pgraph_vk_update_index_buffer(pg, elems, index_data_size) : 0;
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        if (!nv2a_skip_gs_draw(r) && elems_n)
            { nv2a_note_draw(pg, elems_n, "E"); vkCmdDrawIndexed(r->command_buffer, elems_n, 1, 0, 0, 0); }
        g_free(quad_idx);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);
        if (nv2a_quads_as_tris(pg))
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                (pg->inline_buffer_length / 4 * 6 + 6) * sizeof(uint32_t));

        if (!begin_pre_draw(pg)) { NV2A_VK_DGROUP_END(); return; }
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        VkDeviceSize nv2a_quad_seq_off = (VkDeviceSize)-1;
        size_t nv2a_quad_seq_n = 0;
        if (nv2a_quads_as_tris(pg)) {
            uint32_t *qi = g_malloc(sizeof(uint32_t) * (pg->inline_buffer_length / 4 * 6 + 6));
            nv2a_quad_seq_n = nv2a_emit_quad_indices(qi, NULL, 0, pg->inline_buffer_length);
            nv2a_quad_seq_off = nv2a_quad_seq_n ?
                pgraph_vk_update_index_buffer(pg, qi, nv2a_quad_seq_n * sizeof(uint32_t)) : 0;
            g_free(qi);
        }
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        if (nv2a_quad_seq_off != (VkDeviceSize)-1) {
            if (nv2a_quad_seq_n) {
                vkCmdBindIndexBuffer(r->command_buffer,
                                     r->storage_buffers[BUFFER_INDEX].buffer,
                                     nv2a_quad_seq_off, VK_INDEX_TYPE_UINT32);
                static int skip_sky = -1;   /* A/B: the striped sky */
                if (skip_sky < 0) skip_sky = getenv("NV2A_SKIP_SKYBOX") != NULL;
                TextureBinding *st0 = r->texture_bindings[0];
                bool is_sky = st0 && st0 != &r->dummy_texture && st0->key.state.width == 64 &&
                              st0->key.state.height == 64 && st0->key.state.color_format == 12;
                if (!(skip_sky && is_sky) && !s_sky_skip_now)
                { nv2a_note_draw(pg, nv2a_quad_seq_n, "S"); vkCmdDrawIndexed(r->command_buffer, nv2a_quad_seq_n, 1, 0, 0, 0); }
            }
        } else if (!nv2a_skip_gs_draw(r)) { nv2a_note_draw(pg, pg->inline_buffer_length, "B"); vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0); }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;
        {   /* nv2a-port REC: what the inline vertices hold (wedge hunt) */
            extern int nv2a_framelog_on;
            VertexAttribute *a0 = &pg->vertex_attributes[0];
            if (nv2a_framelog_on && a0->count >= 3 && a0->size == 4 && vertex_size >= 12) {
                float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
                unsigned bad = 0, big = 0;
                const uint8_t *base = (const uint8_t *)pg->inline_array + a0->inline_array_offset;
                for (unsigned v = 0; v < index_count; v++) {
                    const float *f = (const float *)(base + v * vertex_size);
                    for (int k = 0; k < 3; k++) {
                        float x = f[k];
                        if (!isfinite(x)) { bad++; continue; }
                        if (fabsf(x) > 1e5f) big++;
                        if (x < mn[k]) mn[k] = x;
                        if (x > mx[k]) mx[k] = x;
                    }
                }
                fprintf(stderr, "[FRAME]   inline v0 x %g..%g y %g..%g z %g..%g nonfinite %u big %u n %u vsz %u\n",
                        mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], bad, big, index_count, vertex_size);
            }
        }

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);

        if (nv2a_quads_as_tris(pg))
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                (index_count / 4 * 6 + 6) * sizeof(uint32_t));
        if (!begin_pre_draw(pg)) { NV2A_VK_DGROUP_END(); return; }
        {   /* the quad's screen box for the crosshair check (nv2a_note_draw) */
            VertexAttribute *a0 = &pg->vertex_attributes[0];
            s_aimq_valid = false; s_aimq_known = true;
            if (index_count == 4 && vertex_size >= 12 && a0->count >= 2) {
                quad_bbox((const uint8_t *)pg->inline_array + a0->inline_array_offset, vertex_size, 4, s_aimq_bb);
                s_aimq_valid = true;
            }
        }
        nv2a_bubble_squeeze(pg, vertex_size, index_count);
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        VkDeviceSize nv2a_quad_seq_off = (VkDeviceSize)-1;
        size_t nv2a_quad_seq_n = 0;
        if (nv2a_quads_as_tris(pg)) {
            uint32_t *qi = g_malloc(sizeof(uint32_t) * (index_count / 4 * 6 + 6));
            nv2a_quad_seq_n = nv2a_emit_quad_indices(qi, NULL, 0, index_count);
            nv2a_quad_seq_off = nv2a_quad_seq_n ?
                pgraph_vk_update_index_buffer(pg, qi, nv2a_quad_seq_n * sizeof(uint32_t)) : 0;
            g_free(qi);
        }
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        /* A/B for the striped sky (flag sky_ab): every 4 s the skybox
         * (64x64 DXT1 faces drawn inline) is drawn as usual, then with its
         * depth test off (the whole face shows: stripes in it or not), then
         * not at all (what lies under it). */
        static int sky_ab = -1;
        if (sky_ab < 0) sky_ab = getenv("NV2A_SKY_AB") != NULL;
        uint32_t sky_ctl0 = 0; bool sky_ztest_off = false;
        s_sky_skip_now = false;
        if (sky_ab) {
            /* the skybox: inline quads with corners at +-75000 */
            const float *fv0 = (const float *)pg->inline_array;
            bool sky = vertex_size >= 12 && index_count == 4 &&
                       fabsf(fv0[0]) == 75000.0f && fabsf(fv0[1]) == 75000.0f;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int phase = (int)((ts.tv_sec / 4) % 3);
            static int last_phase = -1;
            if (phase != last_phase) { last_phase = phase;
                fprintf(stderr, "[AB] sky phase %d (%s)\n", phase,
                        phase == 0 ? "normal" : phase == 1 ? "no depth test" : "not drawn"); }
            if (sky && phase == 1) {
                sky_ctl0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
                pgraph_reg_w(pg, NV_PGRAPH_CONTROL_0, sky_ctl0 & ~NV_PGRAPH_CONTROL_0_ZENABLE);
                sky_ztest_off = true;
            }
            if (sky && phase == 2) s_sky_skip_now = true;
        }
        bool sky_face = false;
        {
            const float *fv0 = (const float *)pg->inline_array;
            sky_face = vertex_size >= 12 && index_count == 4 &&
                       fabsf(fv0[0]) == 75000.0f && fabsf(fv0[1]) == 75000.0f;
        }
        begin_draw(pg);
        if (sky_ztest_off) pgraph_reg_w(pg, NV_PGRAPH_CONTROL_0, sky_ctl0);
        bind_inline_vertex_buffer(pg, buffer_offset);
        if (nv2a_quad_seq_off != (VkDeviceSize)-1) {
            if (nv2a_quad_seq_n) {
                vkCmdBindIndexBuffer(r->command_buffer,
                                     r->storage_buffers[BUFFER_INDEX].buffer,
                                     nv2a_quad_seq_off, VK_INDEX_TYPE_UINT32);
                { nv2a_note_draw(pg, nv2a_quad_seq_n, "S"); vkCmdDrawIndexed(r->command_buffer, nv2a_quad_seq_n, 1, 0, 0, 0); }
            }
        } else if (!nv2a_skip_gs_draw(r)) { nv2a_note_draw(pg, index_count, "I"); vkCmdDraw(r->command_buffer, index_count, 1, 0, 0); }
        end_draw(pg);
        {   /* REC: the frame right after the 6th skybox face */
            extern int nv2a_framelog_on, nv2a_framelog_seq;
            static int faces, seq = -1;
            if (nv2a_framelog_on && sky_face) {
                if (seq != nv2a_framelog_seq) { seq = nv2a_framelog_seq; faces = 0; }
                if (++faces == 6) {
                    extern volatile int nv2a_dump_all;
                    if (nv2a_dump_all) {
                        char tag[32]; snprintf(tag, sizeof tag, "f%04d_postsky", nv2a_framelog_seq);
                        pgraph_vk_dump_surfaces(container_of(pg, NV2AState, pgraph), tag);
                    }
                }
            }
        }
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
    }
}
