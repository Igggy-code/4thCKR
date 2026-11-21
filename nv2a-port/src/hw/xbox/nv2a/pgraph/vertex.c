/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include <time.h>
#include "hw/xbox/nv2a/nv2a_int.h"
#include "nv2a_vsh_emulator.h"
#include "nv2a_vsh_disassembler.h"

void pgraph_update_inline_value(VertexAttribute *attr, const uint8_t *data)
{
    assert(attr->count <= 4);
    attr->inline_value[0] = 0.0f;
    attr->inline_value[1] = 0.0f;
    attr->inline_value[2] = 0.0f;
    attr->inline_value[3] = 1.0f;

    switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            for (uint32_t i = 0; i < attr->count; ++i) {
                attr->inline_value[i] = (float)data[i] / 255.0f;
            }
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1: {
            const int16_t *val = (const int16_t *) data;
            for (uint32_t i = 0; i < attr->count; ++i, ++val) {
                attr->inline_value[i] = MAX(-1.0f, (float) *val / 32767.0f);
            }
            break;
        }
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            memcpy(attr->inline_value, data, attr->size * attr->count);
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K: {
            const int16_t *val = (const int16_t *) data;
            for (uint32_t i = 0; i < attr->count; ++i, ++val) {
                attr->inline_value[i] = (float)*val;
            }
            break;
        }
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP: {
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            const int32_t val = *(const int32_t *)data;
            int32_t x = val & 0x7FF;
            if (x & 0x400) {
                x |= 0xFFFFF800;
            }
            int32_t y = (val >> 11) & 0x7FF;
            if (y & 0x400) {
                y |= 0xFFFFF800;
            }
            int32_t z = (val >> 22) & 0x7FF;
            if (z & 0x200) {
                z |= 0xFFFFFC00;
            }

            attr->inline_value[0] = MAX(-1.0f, (float)x / 1023.0f);
            attr->inline_value[1] = MAX(-1.0f, (float)y / 1023.0f);
            attr->inline_value[2] = MAX(-1.0f, (float)z / 511.0f);
            break;
        }
    default:
        fprintf(stderr, "Unknown vertex attribute type: for format 0x%x\n",
                attr->format);
        assert(!"Unsupported attribute type");
        break;
    }
}

void pgraph_get_inline_values(PGRAPHState *pg, uint16_t attrs,
                               float values[NV2A_VERTEXSHADER_ATTRIBUTES][4],
                               int *count)
{
    int num_attributes = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (attrs & (1 << i)) {
            memcpy(values[num_attributes],
                   pg->vertex_attributes[i].inline_value, 4 * sizeof(float));
            num_attributes += 1;
        }
    }

    if (count) {
        *count = num_attributes;
    }
}


void pgraph_allocate_inline_buffer_vertices(PGRAPHState *pg, unsigned int attr)
{
    VertexAttribute *attribute = &pg->vertex_attributes[attr];

    if (attribute->inline_buffer_populated || pg->inline_buffer_length == 0) {
        return;
    }

    /* Now upload the previous attribute value */
    attribute->inline_buffer_populated = true;
    for (int i = 0; i < pg->inline_buffer_length; i++) {
        memcpy(&attribute->inline_buffer[i * 4], attribute->inline_value,
               sizeof(float) * 4);
    }
}

void pgraph_finish_inline_buffer_vertex(PGRAPHState *pg)
{
    pgraph_check_within_begin_end_block(pg);
    assert(pg->inline_buffer_length < NV2A_MAX_BATCH_LENGTH);

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attribute = &pg->vertex_attributes[i];
        if (attribute->inline_buffer_populated) {
            memcpy(&attribute->inline_buffer[pg->inline_buffer_length * 4],
                   attribute->inline_value, sizeof(float) * 4);
        }
    }

    pg->inline_buffer_length++;
}

void pgraph_reset_inline_buffers(PGRAPHState *pg)
{
    pg->inline_elements_length = 0;
    pg->inline_array_length = 0;
    pg->inline_buffer_length = 0;
    pgraph_reset_draw_arrays(pg);
}

void pgraph_reset_draw_arrays(PGRAPHState *pg)
{
    pg->draw_arrays_length = 0;
    pg->draw_arrays_min_start = -1;
    pg->draw_arrays_max_count = 0;
    pg->draw_arrays_prevent_connect = false;
}

static Nv2aVshProgram *s_carry_slice[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
static uint8_t s_carry_checks[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
/* nv2a-port: the instructions each cached program was parsed from. Titles
 * upload (often the same) programs every frame, and each upload threw the
 * whole cache away: re-parse, new slice, 32 checking runs of the full
 * program -- every frame. Now a cached program stays while its words in
 * program memory are unchanged. NV2A_VSH_CARRY_FLUSH=1: the old flush. */
static uint32_t *s_carry_src[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
static uint16_t s_carry_len[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
static void carry_drop(PGRAPHState *pg, int i);
static unsigned s_parses, s_memo_hits, s_memo_runs;

void pgraph_vsh_carryover_invalidate_cache(PGRAPHState *pg)
{
    for (int i = 0; i < ARRAY_SIZE(pg->vsh_carry_cached_programs); ++i)
        carry_drop(pg, i);
}
static void carry_drop(PGRAPHState *pg, int i)
{
    {
        g_free(s_carry_src[i]); s_carry_src[i] = NULL; s_carry_len[i] = 0;
        if (s_carry_slice[i]) {   /* nv2a-port: the slice of the old program */
            g_free(s_carry_slice[i]->steps);
            g_free(s_carry_slice[i]);
            s_carry_slice[i] = NULL;
        }
        if (pg->vsh_carry_cached_programs[i]) {
            nv2a_vsh_program_destroy(pg->vsh_carry_cached_programs[i]);
            g_free(pg->vsh_carry_cached_programs[i]);
            pg->vsh_carry_cached_programs[i] = NULL;
        }
    }
}

void pgraph_vsh_carryover_reset(PGRAPHState *pg)
{
    pg->vsh_carry_fog[0] = 0.0f;
    pg->vsh_carry_fog[1] = 0.0f;
    pg->vsh_carry_fog[2] = 0.0f;
    pg->vsh_carry_fog[3] = 1.0f;
    pgraph_vsh_carryover_invalidate_cache(pg);
    pg->vsh_carry_cache_dirty = false;
}

static bool s_carry_writes_fog[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
static bool getenv_carry_nomemo(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NV2A_VSH_CARRY_NOMEMO") != NULL;
    return v;
}
static bool getenv_carry_all(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NV2A_VSH_CARRY_ALL") != NULL;
    return v;
}

/* nv2a-port: only the steps that feed oFog. The carried fog needs one
 * output, but the whole program ran on the CPU for every draw (skinning,
 * lighting, texture coordinates: ~10 % of the PFIFO thread). A backward
 * pass keeps the operations that write a live register and makes their
 * inputs live; the others become NOPs. R12 reads oPos; relative constant
 * reads make A0 live. The first 32 runs of each program run both versions
 * and compare: any difference drops the slice for that program for good.
 * NV2A_VSH_NO_SLICE=1: off. */
static unsigned s_slice_kept, s_slice_total;
/* nv2a-port: what each slice reads, for the memo below */
typedef struct { uint64_t cread[4]; uint16_t iread; bool rel; } CarryReads;
static CarryReads s_carry_reads[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
static CarryReads s_build_reads;

static Nv2aVshProgram *carry_build_slice(const Nv2aVshProgram *full)
{
    if (getenv("NV2A_VSH_NO_SLICE")) return NULL;
    int n = 0;
    for (const Nv2aVshStep *st = full->steps; ; st++) { n++; if (st->is_final) break; if (n > NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) return NULL; }
    Nv2aVshProgram *sl = g_new0(Nv2aVshProgram, 1);
    sl->steps = g_new(Nv2aVshStep, n);
    memcpy(sl->steps, full->steps, sizeof(Nv2aVshStep) * n);
    bool live_t[16] = {0}, live_a0 = false, live_pos = false, live_fog = true;
    memset(&s_build_reads, 0, sizeof s_build_reads);
    unsigned kept = 0, total = 0;
    for (int i = n - 1; i >= 0; i--) {
        Nv2aVshOperation *ops[2] = { &sl->steps[i].mac, &sl->steps[i].ilu };
        /* both units of a step read the registers as they were before it:
         * decide what each needs first, then add the inputs */
        bool need[2] = { false, false };
        for (int u = 0; u < 2; u++) {
            Nv2aVshOperation *op = ops[u];
            if (op->opcode == NV2AOP_NOP) continue;
            total++;
            for (int o = 0; o < 2; o++) {
                const Nv2aVshOutput *out = &op->outputs[o];
                if (out->type == NV2ART_TEMPORARY) {
                    if (out->index == 12 ? live_pos : (out->index < 16 && live_t[out->index])) need[u] = true;
                } else if (out->type == NV2ART_OUTPUT) {
                    if ((out->index == NV2AOR_FOG_COORD && live_fog) ||
                        (out->index == 0 && live_pos)) need[u] = true;
                } else if (out->type == NV2ART_ADDRESS) {
                    if (live_a0) need[u] = true;
                }
            }
            if (op->opcode == NV2AOP_ARL && live_a0) need[u] = true;
        }
        for (int u = 0; u < 2; u++) {
            Nv2aVshOperation *op = ops[u];
            if (op->opcode == NV2AOP_NOP) continue;
            if (!need[u]) { op->opcode = NV2AOP_NOP; continue; }
            kept++;
            for (int k = 0; k < 3; k++) {
                const Nv2aVshInput *in = &op->inputs[k];
                if (in->type == NV2ART_TEMPORARY) {
                    if (in->index == 12) live_pos = true;
                    else if (in->index < 16) live_t[in->index] = true;
                }
                if (in->is_relative) { live_a0 = true; s_build_reads.rel = true; }
                if (in->type == NV2ART_CONTEXT && in->index < 256)
                    s_build_reads.cread[in->index >> 6] |= 1ull << (in->index & 63);
                if (in->type == NV2ART_INPUT && in->index < 16)
                    s_build_reads.iread |= 1u << in->index;
            }
        }
    }
    s_slice_kept += kept; s_slice_total += total;
    return sl;
}

static void carry_run(PGRAPHState *pg, Nv2aVshProgram *program, float fog_out[4])
{
    Nv2aVshCPUFullExecutionState execution_state;
    Nv2aVshExecutionState state =
        nv2a_vsh_emu_initialize_full_execution_state(&execution_state);
    for (int i = 0; i < ARRAY_SIZE(pg->vertex_attributes); ++i) {
        memcpy(&execution_state.input_regs[i * 4],
               pg->vertex_attributes[i].inline_value, sizeof(float) * 4);
    }
    for (int i = 0; i < ARRAY_SIZE(execution_state.output_regs) / 4; ++i) {
        execution_state.output_regs[i * 4 + 0] = 0.0f;
        execution_state.output_regs[i * 4 + 1] = 0.0f;
        execution_state.output_regs[i * 4 + 2] = 0.0f;
        execution_state.output_regs[i * 4 + 3] = 1.0f;
    }
    memcpy(&execution_state.output_regs[NV2AOR_FOG_COORD * 4],
           pg->vsh_carry_fog, sizeof(pg->vsh_carry_fog));
    memcpy(execution_state.context_regs, pg->vsh_constants,
           sizeof(execution_state.context_regs));
    nv2a_vsh_emu_execute(&state, program);
    memcpy(fog_out, &execution_state.output_regs[NV2AOR_FOG_COORD * 4], sizeof(float) * 4);
}

/* nv2a-port: parsed programs by content. Conker loads a different program
 * into the same program memory for most objects (~3,500 loads a second), so
 * a cache by start address re-parsed nearly every draw only to find the
 * program never writes fog. A program pushed out of its start slot is kept
 * here, keyed by a hash of its words, and taken back when those words come
 * back. NV2A_VSH_CARRY_NOCONTENT=1: off. */
#include "qemu/fast-hash.h"
typedef struct {
    uint64_t h; uint16_t len; uint32_t *src;
    Nv2aVshProgram *prog, *slice; bool wf; CarryReads rd; uint8_t checks;
} CarryEnt;
enum { CE_N = 512 };
static CarryEnt s_ce[CE_N];
static unsigned s_ce_hits;
static int s_ce_off = -1;
static void ce_free(CarryEnt *e)
{
    g_free(e->src);
    if (e->slice) { g_free(e->slice->steps); g_free(e->slice); }
    if (e->prog) { nv2a_vsh_program_destroy(e->prog); g_free(e->prog); }
    memset(e, 0, sizeof *e);
}
/* the slot's program into the content cache (ownership moves) */
static void ce_stash(PGRAPHState *pg, int i)
{
    if (!pg->vsh_carry_cached_programs[i] || !s_carry_src[i]) { carry_drop(pg, i); return; }
    CarryEnt e = {
        .h = fast_hash((const uint8_t *)s_carry_src[i], (size_t)s_carry_len[i] * 16),
        .len = s_carry_len[i], .src = s_carry_src[i],
        .prog = pg->vsh_carry_cached_programs[i], .slice = s_carry_slice[i],
        .wf = s_carry_writes_fog[i], .rd = s_carry_reads[i], .checks = s_carry_checks[i],
    };
    CarryEnt *slot = &s_ce[e.h % CE_N];
    ce_free(slot);
    *slot = e;
    s_carry_src[i] = NULL; s_carry_len[i] = 0;
    pg->vsh_carry_cached_programs[i] = NULL; s_carry_slice[i] = NULL;
}
/* words at program start i match a kept program: move it into the slot */
static bool ce_take(PGRAPHState *pg, int i)
{
    int len = 0;
    while (i + len < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        len++;
        if (pg->program_data[i + len - 1][3] & 1) break;   /* final bit */
    }
    uint64_t h = fast_hash((const uint8_t *)pg->program_data[i], (size_t)len * 16);
    CarryEnt *e = &s_ce[h % CE_N];
    if (!e->prog || e->h != h || e->len != len ||
        memcmp(e->src, pg->program_data[i], (size_t)len * 16))
        return false;
    pg->vsh_carry_cached_programs[i] = e->prog; s_carry_slice[i] = e->slice;
    s_carry_src[i] = e->src; s_carry_len[i] = e->len;
    s_carry_writes_fog[i] = e->wf; s_carry_reads[i] = e->rd; s_carry_checks[i] = e->checks;
    memset(e, 0, sizeof *e);
    s_ce_hits++;
    return true;
}

void pgraph_vsh_carryover_update(PGRAPHState *pg)
{
    if (!pg->draw_arrays_length && !pg->inline_elements_length &&
        !pg->inline_buffer_length && !pg->inline_array_length) {
        return;
    }

    static int flush_all = -1;
    if (flush_all < 0) flush_all = getenv("NV2A_VSH_CARRY_FLUSH") != NULL;
    bool recheck = false;
    if (pg->vsh_carry_cache_dirty) {
        if (flush_all) pgraph_vsh_carryover_invalidate_cache(pg);
        else recheck = true;   /* each cached program is compared below */
        pg->vsh_carry_cache_dirty = false;
    }

    bool is_vertex_program = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                                      NV_PGRAPH_CSV0_D_MODE) == 2;
    if (!is_vertex_program) {
        return;
    }

    uint32_t program_start = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                                      NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    if (program_start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        return;
    }

    /* program memory changed since: drop the cached programs whose words did */
    static uint32_t s_seen_gen, s_gen = 1, s_checked_gen[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH];
    if (recheck) s_gen++;
    if (pg->vsh_carry_cached_programs[program_start] && s_checked_gen[program_start] != s_gen) {
        s_checked_gen[program_start] = s_gen;
        if (!s_carry_src[program_start] ||
            program_start + s_carry_len[program_start] > NV2A_MAX_TRANSFORM_PROGRAM_LENGTH ||
            memcmp(s_carry_src[program_start], pg->program_data[program_start],
                   (size_t)s_carry_len[program_start] * sizeof(pg->program_data[0]))) {
            if (s_ce_off < 0) s_ce_off = getenv("NV2A_VSH_CARRY_NOCONTENT") != NULL;
            if (s_ce_off) carry_drop(pg, program_start);
            else ce_stash(pg, program_start);
        }
    }
    if (!pg->vsh_carry_cached_programs[program_start] && s_ce_off != 1 &&
        ce_take(pg, program_start))
        s_checked_gen[program_start] = s_gen;
    (void)s_seen_gen;
    if (!pg->vsh_carry_cached_programs[program_start]) {
        Nv2aVshProgram *program = g_new0(Nv2aVshProgram, 1);
        Nv2aVshParseResult result = nv2a_vsh_parse_program(
            program, pg->program_data[program_start],
            NV2A_MAX_TRANSFORM_PROGRAM_LENGTH - program_start);
        if (result != NV2AVPR_SUCCESS) {
            g_free(program);
            return;
        }
        pg->vsh_carry_cached_programs[program_start] = program;
        s_parses++;
        {   int n = 0;
            for (Nv2aVshStep *st = program->steps; ; st++) { n++; if (st->is_final || n >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH - program_start) break; }
            s_carry_len[program_start] = (uint16_t)n;
            s_carry_src[program_start] = g_malloc((size_t)n * sizeof(pg->program_data[0]));
            memcpy(s_carry_src[program_start], pg->program_data[program_start], (size_t)n * sizeof(pg->program_data[0]));
            s_checked_gen[program_start] = s_gen; }
        /* nv2a-port: does it write oFog at all? Only then can the carried
         * fog change, and only then is the program worth running on the
         * CPU (every draw, 3 KB of constants copied each time). */
        bool w = false;
        for (Nv2aVshStep *st = program->steps; st; st++) {
            for (int o = 0; o < 2; o++) {
                if ((st->mac.outputs[o].type == NV2ART_OUTPUT &&
                     st->mac.outputs[o].index == NV2AOR_FOG_COORD) ||
                    (st->ilu.outputs[o].type == NV2ART_OUTPUT &&
                     st->ilu.outputs[o].index == NV2AOR_FOG_COORD))
                    w = true;
            }
            if (st->is_final) break;
        }
        s_carry_writes_fog[program_start] = w;
        s_carry_slice[program_start] = w ? carry_build_slice(program) : NULL;
        s_carry_reads[program_start] = s_build_reads;
        s_carry_checks[program_start] = 0;
    }

    if (!s_carry_writes_fog[program_start] && !getenv_carry_all()) {
        return;
    }

    Nv2aVshProgram *program = pg->vsh_carry_cached_programs[program_start];
    Nv2aVshProgram *slice = s_carry_slice[program_start];
    float fog[4];
    if (slice && s_carry_checks[program_start] < 32) {
        float ref[4];
        carry_run(pg, program, ref);
        carry_run(pg, slice, fog);
        s_carry_checks[program_start]++;
        if (memcmp(ref, fog, sizeof ref)) {
            fprintf(stderr, "[VSHSLICE] program at %u: sliced fog differs (%g vs %g), full program from now on\n",
                    program_start, fog[0], ref[0]);
            g_free(slice->steps); g_free(slice);
            s_carry_slice[program_start] = NULL;
            memcpy(fog, ref, sizeof fog);
        }
        static int said;
        if (!said++)
            fprintf(stderr, "[VSHSLICE] fog carry runs %u of %u operations so far\n", s_slice_kept, s_slice_total);
    } else if (slice && !s_carry_reads[program_start].rel && !getenv_carry_nomemo()) {
        /* nv2a-port: the slice's result depends only on the carried fog, the
         * constants and inline inputs it reads: when those are as last time
         * for this program, so is the result. The key is built from just
         * those, not the 3 KB of constants. NV2A_VSH_CARRY_NOMEMO=1: off. */
        const CarryReads *rd = &s_carry_reads[program_start];
        float key[4 + 16 * 4 + 64 * 4];
        int k = 0;
        memcpy(key, pg->vsh_carry_fog, 16); k = 4;
        for (int i = 0; i < 16; i++)
            if (rd->iread & (1u << i)) { memcpy(key + k, pg->vertex_attributes[i].inline_value, 16); k += 4; }
        bool fits = true;
        for (int c = 0; c < 256 && fits; c++)
            if (rd->cread[c >> 6] & (1ull << (c & 63))) {
                if (k + 4 > (int)(sizeof key / sizeof key[0]) || c >= NV2A_VERTEXSHADER_CONSTANTS) { fits = false; break; }
                memcpy(key + k, pg->vsh_constants[c], 16); k += 4;
            }
        static struct { int n; float key[4 + 16 * 4 + 64 * 4]; float res[4]; uint32_t prog; bool ok; } memo[4];
        static unsigned hits, runs; static time_t t0;
        int m = program_start & 3;
        if (fits && memo[m].ok && memo[m].prog == program_start && memo[m].n == k &&
            !memcmp(memo[m].key, key, k * sizeof(float))) {
            memcpy(fog, memo[m].res, sizeof fog); hits++;
        } else {
            carry_run(pg, slice, fog); runs++;
            if (fits) { memo[m].n = k; memcpy(memo[m].key, key, k * sizeof(float));
                        memcpy(memo[m].res, fog, sizeof fog); memo[m].prog = program_start; memo[m].ok = true; }
        }
        s_memo_hits += hits; s_memo_runs += runs; hits = runs = 0; (void)t0;
    } else {
        carry_run(pg, slice ? slice : program, fog);
    }
    memcpy(pg->vsh_carry_fog, fog, sizeof(pg->vsh_carry_fog));
    {   static time_t t0; time_t tn = time(NULL);
        if (!t0) t0 = tn;
        if (tn - t0 >= 10) {
            fprintf(stderr, "[VSHSLICE] in 10 s: %u programs parsed, %u taken back by content, fog memo %u reused / %u run\n",
                    s_parses, s_ce_hits, s_memo_hits, s_memo_runs);
            s_parses = s_ce_hits = s_memo_hits = s_memo_runs = 0; t0 = tn;
        } }
}
