/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2025 Matt Borgerson
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

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "shaders.h"

ShaderState pgraph_glsl_get_shader_state(PGRAPHState *pg)
{
    pg->program_data_dirty = false; /* fixme */

    ShaderState state;

    // We will hash it, so make sure any padding is zeroed
    memset(&state, 0, sizeof(ShaderState));

    pgraph_glsl_set_vsh_state(pg, &state.vsh);
    pgraph_glsl_set_geom_state(pg, &state.geom);
    pgraph_glsl_set_psh_state(pg, &state.psh);

    return state;
}

/* nv2a-port: by value. The game rewrites these registers with the same
 * values all the time, and each "dirty" register made bind_shaders build the
 * whole ShaderState (several KB) and compare it, ~700 times a frame. The
 * values are snapshotted when bind_shaders builds the state; a register is
 * changed when it differs from that. NV2A_SHADER_DIRTY_OLD=1: old test. */
static const unsigned int s_sh_regs[] = {
    NV_PGRAPH_COMBINECTL,      NV_PGRAPH_COMBINESPECFOG0,
    NV_PGRAPH_COMBINESPECFOG1, NV_PGRAPH_CONTROL_0,
    NV_PGRAPH_CONTROL_3,       NV_PGRAPH_CSV0_C,
    NV_PGRAPH_CSV0_D,          NV_PGRAPH_CSV1_A,
    NV_PGRAPH_CSV1_B,          NV_PGRAPH_POINTSIZE,
    NV_PGRAPH_SETUPRASTER,     NV_PGRAPH_SHADERCLIPMODE,
    NV_PGRAPH_SHADERCTL,       NV_PGRAPH_SHADERPROG,
    NV_PGRAPH_SHADOWCTL,       NV_PGRAPH_ZCOMPRESSOCCLUDE,
};
enum { SH_NREG = 16 + 8 * 4 + 4 * 3 };
static uint32_t s_sh_snap[SH_NREG];
static bool s_sh_snap_ok;
static int sh_old(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NV2A_SHADER_DIRTY_OLD") != NULL;
    return v;
}
static void sh_values(PGRAPHState *pg, uint32_t *v)
{
    int k = 0;
    for (int i = 0; i < 16; i++) v[k++] = pgraph_reg_r(pg, s_sh_regs[i]);
    for (int i = 0; i < 8; i++) {
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4);
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4);
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4);
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4);
    }
    for (int i = 0; i < 4; i++) {
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4);
        v[k++] = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
    }
}
/* bind_shaders: the state was just built from the current registers */
void pgraph_glsl_snapshot_shader_regs(PGRAPHState *pg)
{
    sh_values(pg, s_sh_snap);
    s_sh_snap_ok = true;
}

bool pgraph_glsl_check_shader_state_dirty(PGRAPHState *pg,
                                          const ShaderState *state)
{
    if (pg->program_data_dirty) {
        return true;
    }
    if (!sh_old()) {
        if (!s_sh_snap_ok) return true;
        uint32_t v[SH_NREG];
        sh_values(pg, v);
        if (memcmp(v, s_sh_snap, sizeof v)) return true;
        goto fields;
    }

    unsigned int regs[] = {
        NV_PGRAPH_COMBINECTL,      NV_PGRAPH_COMBINESPECFOG0,
        NV_PGRAPH_COMBINESPECFOG1, NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_3,       NV_PGRAPH_CSV0_C,
        NV_PGRAPH_CSV0_D,          NV_PGRAPH_CSV1_A,
        NV_PGRAPH_CSV1_B,          NV_PGRAPH_POINTSIZE,
        NV_PGRAPH_SETUPRASTER,     NV_PGRAPH_SHADERCLIPMODE,
        NV_PGRAPH_SHADERCTL,       NV_PGRAPH_SHADERPROG,
        NV_PGRAPH_SHADOWCTL,       NV_PGRAPH_ZCOMPRESSOCCLUDE,
    };
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    int num_stages = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL) & 0xFF;
    for (int i = 0; i < num_stages; i++) {
        if (pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4)) {
            return true;
        }
    }

fields:
    { extern int nv2a_dbg_mode_now(void); if (nv2a_dbg_mode_now() != state->psh.dbg) return true; }
    if (pg->uniform_attrs != state->vsh.uniform_attrs ||
        pg->swizzle_attrs != state->vsh.swizzle_attrs ||
        pg->w1_attrs != state->vsh.w1_attrs ||
        pg->int_attrs != state->vsh.int_attrs ||
        pg->snorm_attrs != state->vsh.snorm_attrs ||
        pg->compressed_attrs != state->vsh.compressed_attrs ||
        pg->primitive_mode != state->geom.primitive_mode ||
        pg->surface_scale_factor != state->vsh.surface_scale_factor ||
        pg->surface_shape.zeta_format != state->psh.surface_zeta_format) {
        return true;
    }

    for (int i = 0; i < 4; i++) {
        if (sh_old() && (pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXFILTER0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXFMT0 + i * 4))) {
            return true;
        }

        if (pg->texture_matrix_enable[i] !=
            state->vsh.fixed_function.texture_matrix_enable[i]) {
            return true;
        }
    }

    return false;
}
