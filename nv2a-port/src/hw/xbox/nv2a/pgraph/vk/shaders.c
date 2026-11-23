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
#include "qemu/mstring.h"
#include "renderer.h"

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2

/* nv2a-port: the two uniform buffers as dynamic uniform buffers. Their
 * contents change on nearly every draw (matrices per object), and each
 * change wrote a whole new descriptor set -- 6 descriptor writes, recorded,
 * replayed and validated by the driver, ~1,000 times a frame. With dynamic
 * offsets a uniform change is just new offsets at bind time; a set is
 * written only when the shaders or textures change.
 * NV2A_NO_DYN_UBO=1: the old way. */
bool pgraph_vk_dyn_ubo(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NV2A_NO_DYN_UBO") == NULL;
    return v;
}
#define UBO_TYPE (pgraph_vk_dyn_ubo() ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_sets = ARRAY_SIZE(r->descriptor_sets);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = UBO_TYPE,
            .descriptorCount = 2 * num_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES * num_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = ARRAY_SIZE(r->descriptor_sets),
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2 + NV2A_MAX_TEXTURES];

    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = VSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = UBO_TYPE,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    bindings[1] = (VkDescriptorSetLayoutBinding){
        .binding = PSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = UBO_TYPE,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){
            .binding = PSH_TEX_BINDING + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[ARRAY_SIZE(r->descriptor_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool,
        .descriptorSetCount = ARRAY_SIZE(r->descriptor_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets));
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->descriptor_sets), r->descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->descriptor_sets); i++) {
        r->descriptor_sets[i] = VK_NULL_HANDLE;
    }
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    bool need_uniform_write =
        r->uniforms_changed ||
        r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset ==
            r->storage_buffers[BUFFER_UNIFORM_STAGING].slot_lo;

    if (!(r->shader_bindings_changed || r->texture_bindings_changed ||
          (r->descriptor_set_index == r->descriptor_set_base) || need_uniform_write)) {
        return; // Nothing changed
    }

    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };
    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }
    bool need_ubo_staging_buffer_reset =
        r->uniforms_changed &&
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_UNIFORM_STAGING,
                                        ubo_buffer_total_size,
                                        r->device_props.limits.minUniformBufferOffsetAlignment);

    bool need_descriptor_write_reset =
        (r->descriptor_set_index >= r->descriptor_set_limit);

    if (need_descriptor_write_reset || need_ubo_staging_buffer_reset) {
        { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
        need_uniform_write = true;
    }

    VkWriteDescriptorSet descriptor_writes[2 + NV2A_MAX_TEXTURES];

    assert(r->descriptor_set_index < r->descriptor_set_limit);

    bool need_set = !pgraph_vk_dyn_ubo() || r->shader_bindings_changed ||
                    r->texture_bindings_changed || need_descriptor_write_reset ||
                    need_ubo_staging_buffer_reset ||
                    r->descriptor_set_index == r->descriptor_set_base;

    if (need_uniform_write) {
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            void *data = layouts[i]->allocation;
            VkDeviceSize size = layouts[i]->total_size;
            r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                r->device_props.limits.minUniformBufferOffsetAlignment);
        }

        r->uniforms_changed = false;
    }

    if (!need_set) {
        return;   /* same set; the new offsets go in at bind time */
    }

    VkDescriptorBufferInfo ubo_buffer_infos[2];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
            .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
            .offset = pgraph_vk_dyn_ubo() ? 0 : r->uniform_buffer_offsets[i],
            .range = layouts[i]->total_size,
        };
        descriptor_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = i == 0 ? VSH_UBO_BINDING : PSH_UBO_BINDING,
            .dstArrayElement = 0,
            .descriptorType = UBO_TYPE,
            .descriptorCount = 1,
            .pBufferInfo = &ubo_buffer_infos[i],
        };
    }

    VkDescriptorImageInfo image_infos[NV2A_MAX_TEXTURES];
    static int dummy_tex = -1;
    if (dummy_tex < 0) dummy_tex = getenv("NV2A_DUMMY_TEX") != NULL;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        image_infos[i] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            /* nv2a-port experiment NV2A_DUMMY_TEX: every texture -> dummy */
            .imageView = dummy_tex ? r->dummy_texture.image_view :
                                                    r->texture_bindings[i]->image_view,
            .sampler = dummy_tex ? r->dummy_texture.sampler :
                                                  r->texture_bindings[i]->sampler,
        };
        descriptor_writes[2 + i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = PSH_TEX_BINDING + i,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[i],
        };
    }

    vkUpdateDescriptorSets(r->device, 6, descriptor_writes, 0, NULL);

    r->descriptor_set_index++;
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_ref_shader_module(module->module_info);
    return module->module_info;
}

bool pgraph_vk_known(int which, uint64_t h);
void pgraph_vk_known_add(int which, uint64_t h);

static void shader_cache_entry_init_impl(Lru *lru, LruNode *node, const void *state);
double nv2a_shader_gen_ms; unsigned nv2a_shader_gen_n;
static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    shader_cache_entry_init_impl(lru, node, state);
    clock_gettime(CLOCK_MONOTONIC, &b);
    nv2a_shader_gen_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    nv2a_shader_gen_n++;
}
static void shader_cache_entry_init_impl(Lru *lru, LruNode *node, const void *state)
{
    pgraph_vk_known_add(0, fast_hash((void *)state, sizeof(ShaderState)));
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&binding->state.geom) &&
                                !r->no_geometry_shader;
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key.geom.state = binding->state.geom;
        key.geom.glsl_opts.vulkan = true;
        key.geom.glsl_opts.no_point_size =
            !r->enabled_physical_device_features.shaderTessellationAndGeometryPointSize;
        binding->geom.module_info = get_and_ref_shader_module_for_key(r, &key);
    } else {
        binding->geom.module_info = NULL;
    }

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    key.vsh.state = binding->state.vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    binding->vsh.module_info = get_and_ref_shader_module_for_key(r, &key);

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key.psh.state = binding->state.psh;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key.psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    binding->psh.module_info = get_and_ref_shader_module_for_key(r, &key);

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return memcmp(&snode->state, key, sizeof(ShaderState));
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    const char *src = mstring_get_str(code);
    char *patched = NULL;
    if (module->key.kind == VK_SHADER_STAGE_FRAGMENT_BIT &&
        (getenv("NV2A_PSH_TRIVIAL") || getenv("NV2A_PSH_NOLOOP") ||
         getenv("NV2A_PSH_NODEPTH") || getenv("NV2A_PSH_NODISCARD"))) {
        size_t n = strlen(src);
        patched = g_malloc(n * 2 + 128);
        char *o = patched;
        const char *p = src;
        bool noloop = getenv("NV2A_PSH_NOLOOP") != NULL;
        bool trivial = getenv("NV2A_PSH_TRIVIAL") != NULL;
        bool nodepth = getenv("NV2A_PSH_NODEPTH") != NULL;
        bool nodiscard = getenv("NV2A_PSH_NODISCARD") != NULL;
        while (*p) {
            /* experiment: no early-out in the window-clip loop, no precise */
            if (noloop && !strncmp(p, "    break;\n", 11)) { p += 11; continue; }
            if (noloop && !strncmp(p, "precise ", 8)) { p += 8; continue; }
            if (nodepth && !strncmp(p, "gl_FragDepth =", 14)) {
                /* experiment: drop the depth write (comment the line out) */
                memcpy(o, "//", 2); o += 2;
            }
            if (nodiscard && !strncmp(p, "discard;", 8)) {
                const char *rep = "return;";
                memcpy(o, rep, 7); o += 7; p += 8;
                continue;
            }
            if (trivial && !strncmp(p, "void main() {\n", 14)) {
                memcpy(o, p, 14); o += 14; p += 14;
                const char *ins = "fragColor = vec4(1.0, 0.0, 1.0, 1.0);\nreturn;\n";
                memcpy(o, ins, strlen(ins)); o += strlen(ins);
                continue;
            }
            *o++ = *p++;
        }
        *o = 0;
        src = patched;
    }
    {   /* nv2a-port: NV2A_DUMP_VSH writes each vertex program's GLSL to
         * NV2A_DUMP_DIR (first copy per program length). */
        static int dump = -1;
        if (dump < 0) dump = getenv("NV2A_DUMP_VSH") != NULL;
        const char *dir = getenv("NV2A_DUMP_DIR");
        if (dump && dir && module->key.kind == VK_SHADER_STAGE_VERTEX_BIT &&
            !module->key.vsh.state.is_fixed_function) {
            static uint8_t done[256];
            unsigned pl = module->key.vsh.state.programmable.program_length & 0xff;
            if (!done[pl]) {
                done[pl] = 1;
                char path[512];
                snprintf(path, sizeof path, "%s/vsh_prog%u.glsl", dir, pl);
                FILE *f = fopen(path, "w");
                if (f) { fputs(src, f); fclose(f); }
            }
        }
    }
    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, src);
    g_free(patched);
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_unref_shader_module(r, module->module_info);
    module->module_info = NULL;
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const size_t shader_cache_size = 1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((void *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

static void apply_uniform_updates(ShaderUniformLayout *layout,
                                  const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] != -1) {
            uniform_copy(layout, locs[i], (char*)values + info[i].val_offs,
                         4, (info[i].size * info[i].count) / 4);
        }
    }
}

// FIXME: Dirty tracking
static void update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);
    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };

    /* nv2a-port: the 192 vertex constants (3 KB) were copied twice and
     * hashed with the rest of the buffer on every draw. They are copied
     * now only when written since this layout last took them, and a change
     * is noticed by uniform_copy itself (layout->dirty) instead of a hash.
     * NV2A_UBO_HASH=1: the old copy-and-hash. */
    static int ubo_hash = -1;
    if (ubo_hash < 0) ubo_hash = getenv("NV2A_UBO_HASH") != NULL;
    extern uint32_t nv2a_vsh_const_gen;
    ShaderUniformLayout *vl = &binding->vsh.module_info->uniforms;
    int c_loc = binding->vsh.uniform_locs[VshUniform_c];
    static int noskip = -1;
    if (noskip < 0) noskip = getenv("NV2A_UBO_NOSKIPC") != NULL;
    bool skip_c = !ubo_hash && !noskip && c_loc != -1 && vl->c_gen == nv2a_vsh_const_gen;
    /* nv2a-port: rows written since this layout took them, not all 192
     * (a draw typically changes a few matrix rows). NV2A_UBO_ALLROWS=1 off. */
    static int allrows = -1;
    if (allrows < 0) allrows = getenv("NV2A_UBO_ALLROWS") != NULL;
    extern uint32_t nv2a_vsh_row_gen[], nv2a_vsh_all_gen;
    if (!ubo_hash && !noskip && !allrows && c_loc != -1 && !skip_c && vl->c_gen &&
        nv2a_vsh_all_gen <= vl->c_gen) {
        ShaderUniform *u = &vl->uniforms[c_loc - 1];
        if (u->stride == 16 && u->dim_v == 4) {
            uint8_t *base = (uint8_t *)uniform_ptr(vl, c_loc);
            int rows = u->dim_a < NV2A_VERTEXSHADER_CONSTANTS ? u->dim_a : NV2A_VERTEXSHADER_CONSTANTS;
            for (int row = 0; row < rows; row++) {
                if (nv2a_vsh_row_gen[row] <= vl->c_gen) continue;
                if (memcmp(base + row * 16, pg->vsh_constants[row], 16)) {
                    memcpy(base + row * 16, pg->vsh_constants[row], 16);
                    vl->dirty = true;
                }
            }
            vl->c_gen = nv2a_vsh_const_gen;
            skip_c = true;   /* done row by row */
        }
    }
    if (skip_c) binding->vsh.uniform_locs[VshUniform_c] = -1;

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->vsh.uniform_locs, &vsh_values);
    apply_uniform_updates(&binding->vsh.module_info->uniforms, VshUniformInfo,
                          binding->vsh.uniform_locs, &vsh_values,
                          VshUniform__COUNT);
    if (skip_c) binding->vsh.uniform_locs[VshUniform_c] = c_loc;
    else if (c_loc != -1) vl->c_gen = nv2a_vsh_const_gen;

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    for (int i = 0; i < 4; i++) {
        assert(r->texture_bindings[i] != NULL);
        float scale = r->texture_bindings[i]->key.scale;

        BasicColorFormatInfo f_basic =
            kelvin_color_format_info_map[pg->vk_renderer_state
                                             ->texture_bindings[i]
                                             ->key.state.color_format];
        if (!f_basic.linear) {
            scale = 1.0;
        }

        psh_values.texScale[i] = scale;
    }
    apply_uniform_updates(&binding->psh.module_info->uniforms, PshUniformInfo,
                          binding->psh.uniform_locs, &psh_values,
                          PshUniform__COUNT);

    if (ubo_hash) {
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            uint64_t hash =
                fast_hash(layouts[i]->allocation, layouts[i]->total_size);
            r->uniforms_changed |= (hash != r->uniform_buffer_hashes[i]);
            r->uniform_buffer_hashes[i] = hash;
        }
    } else {
        /* changed values, or another layout than the one uploaded last */
        static ShaderUniformLayout *last[2];
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            if (layouts[i]->dirty || last[i] != layouts[i]) r->uniforms_changed = true;
            layouts[i]->dirty = false;
            last[i] = layouts[i];
        }
    }

    nv2a_profile_inc_counter(r->uniforms_changed ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);

    NV2A_VK_DGROUP_END();
}

/* nv2a-port: asynchronous shader compilation. A shader state the renderer
 * has not seen is compiled (GLSL generation + glslang to SPIR-V, written to
 * the SPIR-V disk cache) on a worker thread, and draws that need it are
 * skipped until it is ready -- an object appears a frame or two late instead
 * of the whole game freezing while dozens of shaders compile (1.2 s hitches
 * on entering new areas). The render thread then builds the module from the
 * cached SPIR-V, which takes microseconds. Needs the SPIR-V disk cache
 * (NV2A_CACHE_DIR); NV2A_SYNC_SHADERS=1 turns it off. */
#include <pthread.h>
#include <glslang/Include/glslang_c_interface.h>
extern GByteArray *pgraph_vk_compile_glsl_to_spv(PGRAPHVkState *r,
                                                 glslang_stage_t stage,
                                                 const char *glsl_source);
enum { AS_TABLE = 16384, AS_QUEUE = 1024 };
typedef struct { ShaderModuleCacheKey key[3]; int n; uint64_t hash; } AsyncShaderJob;
static struct {
    int on;
    pthread_mutex_t lock;
    pthread_cond_t cond, done;
    AsyncShaderJob q[AS_QUEUE];
    unsigned qh, qt;
    uint64_t hash[AS_TABLE];
    uint8_t status[AS_TABLE];   /* 0 free, 1 pending, 2 ready */
    PGRAPHVkState *r;
    unsigned skipped, compiled;
} s_as = { .on = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
           .done = PTHREAD_COND_INITIALIZER };

static int as_slot(uint64_t h, bool insert)
{
    unsigned i = (unsigned)(h ^ (h >> 29)) & (AS_TABLE - 1);
    for (unsigned k = 0; k < AS_TABLE; k++, i = (i + 1) & (AS_TABLE - 1)) {
        if (s_as.status[i] && s_as.hash[i] == h) return (int)i;
        if (!s_as.status[i]) return insert ? (int)i : -1;
    }
    return -1;
}

static MString *gen_module_glsl(const ShaderModuleCacheKey *key)
{
    switch (key->kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        return pgraph_glsl_gen_vsh(&key->vsh.state, key->vsh.glsl_opts);
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return pgraph_glsl_gen_geom(&key->geom.state, key->geom.glsl_opts);
    default:
        return pgraph_glsl_gen_psh(&key->psh.state, key->psh.glsl_opts);
    }
}

static void *async_shader_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_as.lock);
        while (s_as.qh == s_as.qt) pthread_cond_wait(&s_as.cond, &s_as.lock);
        AsyncShaderJob job = s_as.q[s_as.qh % AS_QUEUE];
        s_as.qh++;
        pthread_mutex_unlock(&s_as.lock);

        for (int i = 0; i < job.n; i++) {
            MString *code = gen_module_glsl(&job.key[i]);
            glslang_stage_t st =
                job.key[i].kind == VK_SHADER_STAGE_VERTEX_BIT ? GLSLANG_STAGE_VERTEX :
                job.key[i].kind == VK_SHADER_STAGE_GEOMETRY_BIT ? GLSLANG_STAGE_GEOMETRY :
                GLSLANG_STAGE_FRAGMENT;
            GByteArray *spv = pgraph_vk_compile_glsl_to_spv(s_as.r, st, mstring_get_str(code));
            if (spv) g_byte_array_unref(spv);
            mstring_unref(code);
        }
        pthread_mutex_lock(&s_as.lock);
        int k = as_slot(job.hash, false);
        if (k >= 0) s_as.status[k] = 2;
        s_as.compiled++;
        pthread_cond_broadcast(&s_as.done);
        pthread_mutex_unlock(&s_as.lock);
    }
    return NULL;
}

static void build_module_keys(PGRAPHVkState *r, const ShaderState *state,
                              AsyncShaderJob *job)
{
    job->n = 0;
    ShaderModuleCacheKey *key;
    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom) &&
                                !r->no_geometry_shader;
    if (need_geometry_shader) {
        key = &job->key[job->n++];
        memset(key, 0, sizeof(*key));
        key->kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key->geom.state = state->geom;
        key->geom.glsl_opts.vulkan = true;
        key->geom.glsl_opts.no_point_size =
            !r->enabled_physical_device_features.shaderTessellationAndGeometryPointSize;
    }
    key = &job->key[job->n++];
    memset(key, 0, sizeof(*key));
    key->kind = VK_SHADER_STAGE_VERTEX_BIT;
    key->vsh.state = state->vsh;
    key->vsh.glsl_opts.vulkan = true;
    key->vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key->vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key->vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    key = &job->key[job->n++];
    memset(key, 0, sizeof(*key));
    key->kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key->psh.state = state->psh;
    key->psh.glsl_opts.vulkan = true;
    key->psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key->psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
}

/* nv2a-port: hashes of shader states and pipeline keys this device has
 * built before, kept in the cache dir across runs. A known one is bound in
 * line: its SPIR-V is on disk and the driver's pipeline cache holds its
 * binary, so it costs a millisecond or two -- not worth dropping draws for
 * (characters vanished for a moment in every cutscene, every session).
 * Only ones never seen go to the background. */
typedef struct { uint64_t *h; size_t n, cap; FILE *f; const char *name; } KnownSet;
static KnownSet s_known[2] = { { .name = "known_shaders.bin" }, { .name = "known_pipelines.bin" } };
static pthread_mutex_t s_known_lock = PTHREAD_MUTEX_INITIALIZER;

static bool known_find(KnownSet *k, uint64_t h, size_t *slot)
{
    if (!k->cap) { *slot = 0; return false; }
    size_t i = (size_t)(h ^ (h >> 31)) & (k->cap - 1);
    while (k->h[i]) { if (k->h[i] == h) { *slot = i; return true; } i = (i + 1) & (k->cap - 1); }
    *slot = i;
    return false;
}
static void known_insert_mem(KnownSet *k, uint64_t h)
{
    if (!h) h = 1;
    if ((k->n + 1) * 2 > k->cap) {
        size_t ncap = k->cap ? k->cap * 2 : 4096;
        uint64_t *old = k->h; size_t ocap = k->cap;
        k->h = g_malloc0(ncap * sizeof(uint64_t)); k->cap = ncap; k->n = 0;
        for (size_t i = 0; i < ocap; i++) if (old[i]) known_insert_mem(k, old[i]);
        g_free(old);
    }
    size_t slot;
    if (known_find(k, h, &slot)) return;
    k->h[slot] = h; k->n++;
}
static void known_load(void)
{
    static int done;
    if (done) return;
    done = 1;
    extern const char *pgraph_vk_spv_cache_dir_name(void);
    const char *dir = pgraph_vk_spv_cache_dir_name();
    if (!dir) return;
    for (int i = 0; i < 2; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/../%s", dir, s_known[i].name);
        FILE *f = fopen(path, "rb");
        uint64_t v;
        if (f) { while (fread(&v, 8, 1, f) == 1) known_insert_mem(&s_known[i], v); fclose(f); }
        s_known[i].f = fopen(path, "ab");
    }
    fprintf(stderr, "[nv2a] known shader states %zu, pipelines %zu\n", s_known[0].n, s_known[1].n);
}
bool pgraph_vk_known(int which, uint64_t h)
{
    pthread_mutex_lock(&s_known_lock);
    known_load();
    size_t slot;
    bool r = known_find(&s_known[which], h ? h : 1, &slot);
    pthread_mutex_unlock(&s_known_lock);
    return r;
}
void pgraph_vk_known_add(int which, uint64_t h)
{
    pthread_mutex_lock(&s_known_lock);
    known_load();
    size_t slot;
    if (!known_find(&s_known[which], h ? h : 1, &slot)) {
        known_insert_mem(&s_known[which], h);
        if (s_known[which].f) { fwrite(&h, 8, 1, s_known[which].f); fflush(s_known[which].f); }
    }
    pthread_mutex_unlock(&s_known_lock);
}

void pgraph_vk_async_shader_stats(unsigned *skipped, unsigned *compiled)
{
    *skipped = s_as.skipped; *compiled = s_as.compiled;
    s_as.skipped = s_as.compiled = 0;
}

/* Whether the next draw's shaders can be bound without compiling. False
 * (and the compile queued) for a state not seen before. */
bool pgraph_vk_shaders_ready(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (s_as.on < 0) {
        extern const char *pgraph_vk_spv_cache_dir_name(void);
        s_as.on = !getenv("NV2A_SYNC_SHADERS") && pgraph_vk_spv_cache_dir_name() != NULL;
        if (s_as.on) {
            s_as.r = r;
            pthread_t t;
            pthread_create(&t, NULL, async_shader_thread, NULL);
            pthread_detach(t);
            fprintf(stderr, "[nv2a] asynchronous shader compilation on\n");
        }
    }
    if (!s_as.on) return true;
    if (pg->clearing) return true;

    bool zeta_bound = r->zeta_binding != NULL && !NV2A_MOBILE_PATH(r);
    if (r->shader_binding && r->shader_binding->state.psh.zeta_bound == zeta_bound &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state))
        return true;
    /* get_shader_state clears program_data_dirty; bind_shaders must still
     * see it, or it keeps the old vertex program for this draw (garbled and
     * vanishing models since the asynchronous compile went in) */
    bool prog_dirty = pg->program_data_dirty;
    ShaderState st = pgraph_glsl_get_shader_state(pg);
    pg->program_data_dirty = prog_dirty;
    st.psh.zeta_bound = zeta_bound;
    if (r->shader_binding && !memcmp(&r->shader_binding->state, &st, sizeof(st)))
        return true;
    uint64_t h = fast_hash((void *)&st, sizeof(st));
    if (lru_contains_hash(&r->shader_cache, h)) return true;
    if (pgraph_vk_known(0, h)) return true;   /* built here before: in line */

    pthread_mutex_lock(&s_as.lock);
    int k = as_slot(h, true);
    bool ready = false;
    if (k < 0) {
        ready = true;            /* table full: compile in line */
    } else if (s_as.status[k] == 2) {
        ready = true;
    } else if (s_as.status[k] == 0) {
        if (s_as.qt - s_as.qh < AS_QUEUE) {
            AsyncShaderJob *job = &s_as.q[s_as.qt % AS_QUEUE];
            build_module_keys(r, &st, job);
            job->hash = h;
            s_as.qt++;
            s_as.hash[k] = h;
            s_as.status[k] = 1;
            pthread_cond_signal(&s_as.cond);
        } else {
            ready = true;        /* queue full: compile in line */
        }
    }
    if (!ready && k >= 0) {
        /* Most shaders compile in a few ms: wait that long before giving
         * up on the draw, so only the slow ones are dropped. */
        static int wait_ms = -1;
        if (wait_ms < 0) {
            const char *e = getenv("NV2A_ASYNC_WAIT_MS");
            wait_ms = e ? atoi(e) : 2;
        }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)wait_ms * 1000000L;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (s_as.status[k] != 2) {
            if (pthread_cond_timedwait(&s_as.done, &s_as.lock, &dl)) break;
        }
        ready = s_as.status[k] == 2;
    }
    if (!ready) s_as.skipped++;
    pthread_mutex_unlock(&s_as.lock);
    return ready;
}

/* nv2a-port: for the pipeline prewarm (draw.c) */
ShaderBinding *pgraph_vk_shader_binding_for_state(PGRAPHVkState *r, const ShaderState *state)
{
    return get_shader_binding_for_state(r, state);
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    bool zeta_bound = r->zeta_binding != NULL && !NV2A_MOBILE_PATH(r);
    if (!r->shader_binding ||
        r->shader_binding->state.psh.zeta_bound != zeta_bound ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        { extern void pgraph_glsl_snapshot_shader_regs(PGRAPHState *pg);
          pgraph_glsl_snapshot_shader_regs(pg); }
        /* nv2a-port: on Mali (ARM) the shader never writes gl_FragDepth --
         * any depth export eventually hangs G78's fragment jobs; the
         * rasteriser's depth (NV2A depth normalised by the vertex shader) is
         * used instead. */
        new_state.psh.zeta_bound = zeta_bound;
        if (!r->shader_binding || memcmp(&r->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    update_shader_uniforms(pg);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_init_glsl_compiler();
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    shader_cache_init(pg);

    r->use_push_constants_for_uniform_attrs =
        (r->device_props.limits.maxPushConstantsSize >=
         MAX_UNIFORM_ATTR_VALUES_SIZE);
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    shader_cache_finalize(pg);
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    pgraph_vk_finalize_glsl_compiler();
}
