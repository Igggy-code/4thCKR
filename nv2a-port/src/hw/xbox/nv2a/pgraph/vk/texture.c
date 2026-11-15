#include <time.h>
/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);
uint32_t nv2a_surf_gen = 1;
static uint32_t s_texc_gen = 1;
static bool s_tmemo_ok;   /* set by create_texture on a reusable outcome */

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(!"Invalid texture color format");
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    void *texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                NV2A_VK_DPRINTF("Layer %d Level %d @ %x", layer, level, (int)((char*)texture_data_ptr - (char*)d->vram_ptr));

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int tex_width = width, tex_height = height;
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;

                    size_t converted_size = width * height * 4;
                    uint8_t *converted = s3tc_decompress_2d(
                        kelvin_format_to_s3tc_format(s.color_format),
                        texture_data_ptr, width, height);
                    assert(converted);

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.

                        // glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                        // glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                        tex_width = s.width;
                        tex_height = s.height;
                        // if (physical_width == width) {
                        //     glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        // }

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr +=
                        physical_width / 4 * physical_height / 4 * block_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    unsigned int tex_width = width, tex_height = height;

                    size_t converted_size = height * pitch;
                    uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);

                    if (converted) {
                        g_free(unswizzled);
                    } else {
                        converted = unswizzled;
                    }

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.
                        // glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        tex_width = s.width;
                        tex_height = s.height;
                        // pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            if (is_compressed) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                size_t converted_size = width * height * depth * 4;
                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += physical_width / 4 * physical_height / 4 * depth * block_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);

                if (converted) {
                    g_free(unswizzled);
                } else {
                    converted = unswizzled;
                    converted_size = unswizzled_size;
                }

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

volatile uint32_t nv2a_tex_rehash_epoch = 1;

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test = opaque;

    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (tnode->key.palette_length > 0) {
        uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    tnode->possibly_dirty |= overlapping;
}

/* nv2a-port: page epochs instead of a walk of the whole texture cache.
 * Marking a range dirty walked every cached texture (~1024) to flag the
 * overlapping ones -- once per dirty texture found, tens of thousands of
 * times in ten seconds in a busy hub, a quarter of the PFIFO thread on a hot
 * phone. Now a mark stamps the range's pages with a new epoch, and a texture
 * is possibly dirty when one of its own pages carries an epoch newer than
 * its last clean check (one compare when nothing was marked since). */
#define TEX_PAGE_SHIFT 12
static uint32_t *s_page_epoch;
static size_t s_page_epoch_n;
static uint32_t s_dirty_epoch = 1;

static uint32_t range_epoch(hwaddr addr, hwaddr len)
{
    if (!s_page_epoch || !len) return 0;
    size_t p0 = addr >> TEX_PAGE_SHIFT, p1 = (addr + len - 1) >> TEX_PAGE_SHIFT;
    if (p1 >= s_page_epoch_n) p1 = s_page_epoch_n - 1;
    uint32_t m = 0;
    for (size_t p = p0; p <= p1; p++) if (s_page_epoch[p] > m) m = s_page_epoch[p];
    return m;
}

static inline bool tex_node_possibly_dirty(TextureBinding *n)
{
    if (n->possibly_dirty) return true;
    if (n->clean_epoch == s_dirty_epoch) return false;   /* nothing marked since */
    uint32_t e = range_epoch(n->key.texture_vram_offset, n->key.texture_length);
    if (n->key.palette_length) {
        uint32_t pe = range_epoch(n->key.palette_vram_offset, n->key.palette_length);
        if (pe > e) e = pe;
    }
    if (e > n->clean_epoch) { n->possibly_dirty = true; return true; }
    n->clean_epoch = s_dirty_epoch;   /* checked: clean up to now */
    return false;
}

static inline void tex_node_clean(TextureBinding *n)
{
    n->possibly_dirty = false;
    n->clean_epoch = s_dirty_epoch;
}

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    if (!getenv("NV2A_TEX_WALK")) {
        size_t vn = memory_region_size(d->vram) >> TEX_PAGE_SHIFT;
        if (!s_page_epoch) { s_page_epoch = calloc(vn, sizeof(uint32_t)); s_page_epoch_n = vn; }
        if (s_page_epoch && size < memory_region_size(d->vram)) {
            uint32_t e = ++s_dirty_epoch;
            for (size_t p = addr >> TEX_PAGE_SHIFT; p <= (end >> TEX_PAGE_SHIFT) && p < vn; p++)
                s_page_epoch[p] = e;
            return;
        }
    }

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&d->pgraph.vk_renderer_state->texture_cache,
                     mark_textures_possibly_dirty_visitor,
                     &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, texture_vram_offset, length);
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, palette_vram_offset,
                                            palette_length);
    }
    return possibly_dirty;
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
extern int nv2a_vk_cb_s2t, nv2a_vk_cb_texup, nv2a_vk_cb_surfup;
static void upload_texture_image_impl(PGRAPHState *pg, int texture_idx,
                                      TextureBinding *binding);
double nv2a_texup_ms; unsigned nv2a_texup_n;
static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    upload_texture_image_impl(pg, texture_idx, binding);
    clock_gettime(CLOCK_MONOTONIC, &b);
    nv2a_texup_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    nv2a_texup_n++;
}
static void upload_texture_image_impl(PGRAPHState *pg, int texture_idx,
                                      TextureBinding *binding)
{
    nv2a_vk_cb_texup++;
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    const int num_layers = state->cubemap ? 6 : 1;

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            assert(size);
            texture_data_size += size;
        }
    }

    assert(texture_data_size <=
           r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    /* nv2a-port: recorded into the frame's own command buffer, from the
     * per-slot upload ring, instead of a one-off submission waited on (and
     * a drain of both frames in flight first): ~4 ms a texture, the main
     * cost of the hitch when a scene first shows new textures. A fresh image
     * needs no wait at all; a re-upload into an image frames in flight may
     * still sample waits for them, as before. NV2A_SYNC_TEXUP=1: old path. */
    static int sync_tex = -1;
    if (sync_tex < 0) sync_tex = getenv("NV2A_SYNC_TEXUP") != NULL;
    StorageBuffer *ub = &r->storage_buffers[BUFFER_SURF_UPLOAD];
    size_t need = texture_data_size + 16 * num_layers * state->levels;
    if (!sync_tex && need <= (ub->slot_hi - ub->slot_lo) / 2) {
        if (binding->current_layout != VK_IMAGE_LAYOUT_UNDEFINED)
            pgraph_vk_wait_deferred();
        if (ROUND_UP(ub->buffer_offset, 256) + need > ub->slot_hi) {
            static unsigned nlog;
            if (nlog++ < 20) fprintf(stderr, "[BUFSPACE] texture upload ring full\n");
            { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
        }
        VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
        VkDeviceSize off0 = ROUND_UP(ub->buffer_offset, 256), off = off0;
        int nreg = num_layers * state->levels;
        g_autofree VkBufferImageCopy *regs = g_malloc0_n(nreg, sizeof(VkBufferImageCopy));
        VkBufferImageCopy *rg = regs;
        for (int li = 0; li < num_layers; li++) {
            TextureLayer *layer = &layout->layers[li];
            for (int lv = 0; lv < state->levels; lv++) {
                TextureLevel *level = &layer->levels[lv];
                memcpy(ub->mapped + off, level->decoded_data, level->decoded_size);
                *rg++ = (VkBufferImageCopy){
                    .bufferOffset = off,
                    .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .imageSubresource.mipLevel = lv,
                    .imageSubresource.baseArrayLayer = li,
                    .imageSubresource.layerCount = 1,
                    .imageExtent = (VkExtent3D){ level->width, level->height, level->depth },
                };
                off = ROUND_UP(off + level->decoded_size, 16);
                g_free(level->decoded_data);
            }
        }
        ub->buffer_offset = off;
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = ub->buffer, .offset = off0, .size = off - off0,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
        pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                          binding->current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, ub->buffer, binding->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, regs);
        pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pgraph_vk_end_nondraw_commands(pg, cmd);
        return;
    }

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;

    pgraph_vk_wait_deferred();

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));

    int num_regions = num_layers * state->levels;
    g_autofree VkBufferImageCopy *regions =
        g_malloc0_n(num_regions, sizeof(VkBufferImageCopy));

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = 0;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + buffer_offset, level->decoded_data,
                   level->decoded_size);
            *region = (VkBufferImageCopy){
                .bufferOffset = buffer_offset,
                .bufferRowLength = 0, // Tightly packed
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += level->decoded_size;
            region++;
        }
    }
    assert(buffer_offset <= r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    // FIXME: Use nondraw. Need to fill and copy tex buffer at once
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           binding->image, binding->current_layout,
                           num_regions, regions);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
}

static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    SURF_SCALE_SCOPE(pg, surface);
    surface->read_time = pg->draw_time;
    assert(!surface->color);

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    bool use_compute_to_convert_depth_stencil =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool compute_needs_finish = use_compute_to_convert_depth_stencil &&
                                pgraph_vk_compute_needs_finish(r);
    if (compute_needs_finish) {
        { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);
    nv2a_vk_cb_s2t++;

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    size_t copied_image_size =
        scaled_width * scaled_height * surface->host_fmt.host_bytes_per_pixel;
    size_t stencil_buffer_offset = 0;
    size_t stencil_buffer_size = 0;

    int num_regions = 0;
    VkBufferImageCopy regions[2];
    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0, // Tightly packed
        .bufferImageHeight = 0, // Tightly packed
        .imageSubresource.aspectMask = surface->color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        stencil_buffer_offset =
            ROUND_UP(scaled_width * scaled_height * 4,
                     r->device_props.limits.minStorageBufferOffsetAlignment);
        stencil_buffer_size = scaled_width * scaled_height;
        copied_image_size += stencil_buffer_size;

        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = stencil_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){0, 0, 0},
            .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
        };
    }
    StorageBuffer *dst_storage_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    assert(dst_storage_buffer->buffer_size >= copied_image_size);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    vkCmdCopyImageToBuffer(
        cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_storage_buffer->buffer,
        num_regions, regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkBuffer texture_source_buffer;

    if (use_compute_to_convert_depth_stencil) {
        size_t packed_image_size = scaled_width * scaled_height * 4;

        VkBufferMemoryBarrier pre_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(
            pg, surface, cmd,
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, false);

        VkBufferMemoryBarrier post_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = packed_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_dst_barrier, 0, NULL);

        texture_source_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
    } else {
        VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             1, &barrier, 0, NULL);

        texture_source_buffer = dst_storage_buffer->buffer;
    }

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    regions[0] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, texture_source_buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);

    VkBufferMemoryBarrier post_copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = texture_source_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

// FIXME: Should be able to skip the copy and sample the original surface image
static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    SURF_SCALE_SCOPE(pg, surface);
    surface->read_time = pg->draw_time;
    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = surface->host_fmt.aspect,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                   texture->current_layout, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static unsigned int vk_format_texel_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:   return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:     return 2;
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:   return 2;
    case VK_FORMAT_R16_UNORM:               return 2;
    case VK_FORMAT_B8G8R8A8_UNORM:          return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return 4;
    case VK_FORMAT_R32_UINT:                return 4;
    default:                                return 0;
    }
}

static bool check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height ||
        shape->cubemap ||
        shape->levels > 1) {
        return false;
    }

    if (!surface->color) {
        return true;
    }

    if (shape->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
        return false;
    }

    VkColorFormatInfo tex_vkf = kelvin_color_format_vk_map[shape->color_format];
    return tex_vkf.vk_format &&
           surface->host_fmt.host_bytes_per_pixel == vk_format_texel_size(tex_vkf.vk_format);
}

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    pgraph_vk_wait_deferred();

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));
    memset(mapped_memory_ptr, 0xff, texture_data_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
        .sampler = texture_sampler,
    };
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, texture->allocation, label);
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx); // FIXME: Check for pad issues
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter =
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx*4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;

    // FIXME: Separate sampler from texture
    key.filter = filter;
    key.address = address;
    key.border_color = border_color_pack32;
    key.max_anisotropy = max_anisotropy;

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_to_texture = false;

    // Check active surfaces to see if this texture was a render target
    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        surface_to_texture =
            check_surface_to_texture_compatiblity(surface, &state);

        if (!surface_to_texture && surface->color) {
            static int logged;
            if (logged++ < 12)
                fprintf(stderr, "[vk] surface->texture refused @%llx: surf %ux%u pitch %u swz %d bpp %u | "
                        "tex %ux%u pitch %u fmt %02X levels %u cube %d texel %u\n",
                        (unsigned long long)texture_vram_offset, surface->width, surface->height,
                        surface->pitch, surface->swizzle, surface->host_fmt.host_bytes_per_pixel,
                        state.width, state.height, state.pitch, state.color_format, state.levels,
                        state.cubemap,
                        vk_format_texel_size(kelvin_color_format_vk_map[state.color_format].vk_format));
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format,
                state.color_format);
        }

        static int no_s2t = -1;
        if (no_s2t < 0) no_s2t = getenv("NV2A_NO_S2T") != NULL;
        if (surface_to_texture && no_s2t)
            surface_to_texture = false;   /* nv2a-port experiment */
        /* nv2a-port: memory written behind the surface (a character's 32x32
         * tiles rendered into its atlas, read back to RAM) only became an
         * upload when the atlas was bound as a render target again; read as
         * a texture before that, the stale image was used and the character
         * was invisible until a camera cut redrew the atlas. Take the dirt
         * into account here too. NV2A_NO_S2T_UPLOAD=1: old way. */
        static int s2t_up = -1;
        if (s2t_up < 0) s2t_up = getenv("NV2A_NO_S2T_UPLOAD") == NULL;
        if (surface_to_texture && s2t_up && surface->color && !surface->upload_pending &&
            !surface->draw_dirty &&
            memory_region_test_and_clear_dirty(d->vram, surface->vram_addr,
                                               surface->size, DIRTY_MEMORY_NV2A)) {
            surface->upload_pending = true;
            nv2a_surf_gen++;
            nv2a_site_hit("s2t:upload-dirty", 0, surface->width, surface->height);
        }
        if (surface_to_texture && surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    }

    if (!surface_to_texture) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        pgraph_vk_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length);
    }

    unsigned s2t_scale = 1;
    if (surface_to_texture) {
        s2t_scale = surface && surface->scale ? surface->scale : nv2a_global_scale;
    }
    if (surface_to_texture && s2t_scale > 1) {
        key.scale = s2t_scale;
    }

    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    /* nv2a-port: a full cache whose every entry the current or an in-flight
     * frame still uses has nothing to evict, and lru_lookup's eviction then
     * dereferences NULL (a level load binding hundreds of new textures).
     * Make room first: finish the frame so its textures become evictable. */
    if (r->texture_cache.num_free == 0 && !lru_contains_hash(&r->texture_cache, key_hash) &&
        !lru_try_evict_one(&r->texture_cache)) {
        static unsigned n_full;
        if (n_full++ < 20) fprintf(stderr, "[TEXCACHE] full, nothing evictable: finishing the frame\n");
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        if (!lru_try_evict_one(&r->texture_cache) && n_full < 40)
            fprintf(stderr, "[TEXCACHE] still nothing evictable after finish\n");
    }
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->image != VK_NULL_HANDLE;

    if (binding_found) {
        NV2A_VK_DPRINTF("Cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= tex_node_possibly_dirty(snode);
        /* nv2a-port: after a level load every cached texture is compared
         * with guest memory once (by content hash), whatever the dirty
         * tracking says: revisiting a location showed its grass with another
         * texture's texels, a write the tracking had missed. */
        extern volatile uint32_t nv2a_tex_rehash_epoch;
        uint32_t ep = __atomic_load_n(&nv2a_tex_rehash_epoch, __ATOMIC_ACQUIRE);
        if (!surface_to_texture && snode->rehash_epoch != ep) {
            snode->rehash_epoch = ep;
            possibly_dirty = true;
            possibly_dirty_checked = true;
        }
    } else {
        possibly_dirty = true;
    }

    if (!surface_to_texture && !possibly_dirty_checked) {
        extern volatile uint32_t nv2a_tex_dirty_gen;
        uint32_t gen = __atomic_load_n(&nv2a_tex_dirty_gen, __ATOMIC_ACQUIRE);
        static int nogen = -1;
        if (nogen < 0) nogen = getenv("NV2A_NO_DIRTY_GEN") != NULL;
        if (nogen || !binding_found || snode->dirty_gen_checked != gen) {
            possibly_dirty |= check_texture_possibly_dirty(
                d, texture_vram_offset, texture_length, texture_palette_vram_offset,
                texture_palette_data_size);
            if (binding_found) snode->dirty_gen_checked = gen;
        }
    }

    {   /* nv2a-port: NV2A_AB_TEX = re-hash every bound texture for 5 s,
         * normal for 5 s (is the texture cache missing CPU writes?) */
        static int ab = -1;
        if (ab < 0) ab = getenv("NV2A_AB_TEX") != NULL;
        if (ab && !surface_to_texture) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int on = (ts.tv_sec / 5) & 1;
            static int last = -1;
            if (last != on) { last = on; fprintf(stderr, "[AB] tex_rehash=%d\n", on); }
            if (on) possibly_dirty = true;
        }
        /* probe: linear 8-bit textures (menu UI atlas) -- content summary */
        static int y8probe = -1;
        if (y8probe < 0) y8probe = getenv("NV2A_TEXY8") != NULL;
        if (y8probe && state.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8) {
            static uint32_t last_addr; static uint64_t last_hash; static int n;
            const uint8_t *p = (const uint8_t *)d->vram_ptr + texture_vram_offset;
            uint64_t h = fast_hash(p, texture_length);
            if ((texture_vram_offset != last_addr || h != last_hash) && n < 60) {
                n++; last_addr = texture_vram_offset; last_hash = h;
                unsigned nz = 0;
                for (size_t i = 0; i < texture_length; i++) nz += p[i] != 0;
                fprintf(stderr, "[TEXY8] @%08x %ux%u pitch %u len %zu nonzero %u hash %016llx cached %d dirty %d\n",
                        (unsigned)texture_vram_offset, state.width, state.height, state.pitch,
                        (size_t)texture_length, nz, (unsigned long long)h, binding_found, possibly_dirty);
            }
        }
    }

    // Calculate hash of texture data, if necessary
    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    uint64_t content_hash = 0;
    if (!surface_to_texture && (snode->draw_time || possibly_dirty)) {
        content_hash = fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
    }

    if (binding_found) {
        if (surface_to_texture) {
            if (surface->draw_time != snode->draw_time) {
                copy_surface_to_texture(pg, surface, snode);
            }
        } else {
            if (snode->draw_time || (possibly_dirty && content_hash != snode->hash)) {
                if (!snode->draw_time && snode->rehash_epoch && !tex_node_possibly_dirty(snode)) {
                    static unsigned n_stale;
                    if (n_stale++ < 40)
                        fprintf(stderr, "[TEXSTALE] %08x %ux%u fmt %d: cached copy differed from guest memory\n",
                                (unsigned)texture_vram_offset, state.width, state.height, state.color_format);
                }
                upload_texture_image(pg, texture_idx, snode);
                snode->hash = content_hash;
                snode->draw_time = 0;
            }
        }

        tex_node_clean(snode);
        /* reusable by the slot memo: a plain texture, current now */
        s_tmemo_ok = !surface_to_texture && !snode->draw_time;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    s_texc_gen++;   /* a new binding: a memo may name the node it replaces */

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex_node_clean(snode);
    snode->hash = content_hash;

    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    assert(vkf.vk_format != 0);
    assert(0 < state.dimensionality);
    assert(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    assert(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = state.width, // FIXME: Use adjusted size?
        .extent.height = state.height,
        .extent.depth = state.depth,
        .mipLevels = f_basic.linear ? 1 : state.levels,
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = vkf.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if (surface_to_texture) {
        image_create_info.extent.width *= s2t_scale;
        image_create_info.extent.height *= s2t_scale;
    }

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImageCompressionControlEXT nv2a_icc;
    nv2a_vk_image_no_compress(r, &image_create_info, &nv2a_icc);
    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &snode->image,
                            &snode->allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = snode->image,
        .viewType = state.cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state.dimensionality],
        .format = vkf.vk_format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = vkf.component_map,
    };

    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &snode->image_view));


    void *sampler_next_struct = NULL;

    VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
    VkBorderColor vk_border_color;

    bool is_integer_type = vkf.vk_format == VK_FORMAT_R32_UINT;

    if (r->custom_border_color_extension_enabled) {
        vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                            VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_create_info =
            (VkSamplerCustomBorderColorCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                .format = image_view_create_info.format,
                .pNext = sampler_next_struct
            };
        if (is_integer_type) {
            float rgba[4];
            pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
            for (int i = 0; i < 4; i++) {
                custom_border_color_create_info.customBorderColor.uint32[i] =
                    (uint32_t)((double)rgba[i] * (double)0xffffffff);
            }
        } else {
            pgraph_argb_pack32_to_rgba_float(
                border_color_pack32,
                custom_border_color_create_info.customBorderColor.float32);
        }
        sampler_next_struct = &custom_border_color_create_info;
    } else {
        // FIXME: Handle custom color in shader
        if (is_integer_type) {
            vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0x00000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0xff000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    VkFilter vk_min_filter, vk_mag_filter;
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

    if (is_linear_filter_supported_for_format(r, state.color_format)) {
        vk_mag_filter = pgraph_texture_min_filter_vk_map[mag_filter];
        vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter];
    } else {
        vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        f_basic.linear || image_create_info.mipLevels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    float lod_bias = pgraph_convert_lod_bias_to_float(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    if (lod_bias > r->device_props.limits.maxSamplerLodBias) {
        lod_bias = r->device_props.limits.maxSamplerLodBias;
    } else if (lod_bias < -r->device_props.limits.maxSamplerLodBias) {
        lod_bias = -r->device_props.limits.maxSamplerLodBias;
    }
    {   /* nv2a-port: phone screens sit close to the eyes. A small negative
         * LOD bias at native resolution (the 640x480 frame picks mips for
         * that size, which look soft blown up to 1080p), and 4x anisotropic
         * filtering on mipmapped textures (floors, walls at an angle).
         * NV2A_NO_TEX_SHARPEN=1: as the title asks. */
        static int sharpen = -1;
        if (sharpen < 0) sharpen = !getenv("NV2A_NO_TEX_SHARPEN");
        if (sharpen) {
            if (pg->surface_scale_factor == 1) lod_bias -= 0.5f;
            if (mipmap_en && max_anisotropy < 4) max_anisotropy = 4;
        }
    }
    uint32_t sampler_max_anisotropy =
        MIN(r->device_props.limits.maxSamplerAnisotropy, max_anisotropy);

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_mag_filter,
        .minFilter = vk_min_filter,
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = (state.dimensionality > 2) ? lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) : 0,
        .anisotropyEnable =
            r->enabled_physical_device_features.samplerAnisotropy &&
            sampler_max_anisotropy > 1,
        .maxAnisotropy = sampler_max_anisotropy,
        .borderColor = vk_border_color,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                       VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .minLod = mipmap_en ? MIN(state.min_mipmap_level, state.levels - 1) : 0.0,
        .maxLod = mipmap_en ? MIN(state.max_mipmap_level, state.levels - 1) : 0.0,
        .mipLodBias = lod_bias,
        .pNext = sampler_next_struct,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &snode->sampler));

    set_texture_label(pg, snode);

    r->texture_bindings[texture_idx] = snode;

    if (surface_to_texture) {
        copy_surface_to_texture(pg, surface, snode);
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
    }

    NV2A_VK_DGROUP_END();
}

/* nv2a-port: per-slot memo of the last texture bind. A draw (some 1000 a
 * frame, up to four textures each) re-derived every texture from scratch:
 * key, surface lookups, a range scan for surfaces to download, the cache
 * lookup, the dirty checks -- 11 % of the PFIFO thread, for a texture that
 * almost always had not changed. The slot keeps the registers that define
 * its texture and what they resolved to, and reuses it while nothing that
 * could change the answer happened since: no write to guest memory that the
 * dirty tracking saw (nv2a_tex_dirty_gen), no render surface created, moved,
 * drawn into anew or uploaded (nv2a_surf_gen), no texture evicted or created
 * (s_texc_gen), and the binding itself not marked possibly dirty, not drawn
 * into, not due for a rehash. Surface-backed textures always take the full
 * path. NV2A_NO_TEX_MEMO=1: off. */
typedef struct {
    uint32_t regs[11];
    hwaddr dma_a, dma_b;
    unsigned scale;
} TexSnap;
/* a few entries per slot: draws alternate textures in one slot */
#define TMEMO_N 256   /* direct-mapped by address and format */
typedef struct {
    TexSnap snap;
    TextureBinding *snode;
    uint32_t tex_gen, surf_gen, texc_gen, epoch;
    hwaddr off, poff; uint32_t len, plen;   /* the texture's guest memory */
    bool valid;
} TexMemo;
static TexMemo s_tmemo_e[NV2A_MAX_TEXTURES][TMEMO_N];
unsigned nv2a_tmemo_hits, nv2a_tmemo_misses, nv2a_tmemo_why[9];

static void tex_snap(PGRAPHState *pg, int i, TexSnap *t)
{
    memset(t, 0, sizeof(*t));
    t->regs[0] = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + i * 4);
    t->regs[1] = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
    t->regs[2] = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
    t->regs[3] = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + i * 4);
    t->regs[4] = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4);
    t->regs[5] = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4);
    t->regs[6] = pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + i * 4);
    t->regs[7] = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + i * 4);
    t->regs[8] = pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + i * 4);
    t->regs[9] = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
    t->regs[10] = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL2_0 + (i < 2 ? 0 : 4));
    t->dma_a = pg->dma_a;
    t->dma_b = pg->dma_b;
    t->scale = nv2a_global_scale;
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i]) {
            return true;
        }
    }
    return false;
}

static void update_timestamps(PGRAPHVkState *r)
{
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    if (!check_textures_dirty(pg)) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        update_timestamps(r);
        return;
    }

    static int memo_on = -1;
    if (memo_on < 0) memo_on = getenv("NV2A_NO_TEX_MEMO") == NULL;
    extern volatile uint32_t nv2a_tex_dirty_gen;
    extern volatile uint32_t nv2a_tex_rehash_epoch;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->texture_bindings[i] = &r->dummy_texture;
            continue;
        }

        if (!memo_on) {
            create_texture(pg, i);
            pg->texture_dirty[i] = false;
            continue;
        }
        TexSnap snap;
        tex_snap(pg, i, &snap);
        /* read the generations before the work: a change during it makes
         * the next bind recheck */
        uint32_t tg = __atomic_load_n(&nv2a_tex_dirty_gen, __ATOMIC_ACQUIRE);
        uint32_t ep = __atomic_load_n(&nv2a_tex_rehash_epoch, __ATOMIC_ACQUIRE);
        TexMemo *hit = NULL, *same_regs = NULL;
        uint32_t hix = (snap.regs[0] * 0x9E3779B1u ^ snap.regs[1] * 0x85EBCA77u ^
                        snap.regs[4] * 0xC2B2AE3Du) >> 24;
        TexMemo *slot = &s_tmemo_e[i][hix % TMEMO_N];
        if (slot->valid && !memcmp(&slot->snap, &snap, sizeof snap)) {
            same_regs = slot;
            TextureBinding *m = slot->snode;
            if (m && m->image != VK_NULL_HANDLE &&
                slot->surf_gen == nv2a_surf_gen && slot->texc_gen == s_texc_gen &&
                slot->epoch == ep && !tex_node_possibly_dirty(m) && !m->draw_time) {
                if (slot->tex_gen == tg) {
                    hit = slot;
                } else if (!check_texture_possibly_dirty(d, slot->off, slot->len,
                                                         slot->poff, slot->plen)) {
                    /* a write somewhere, not in this texture (the check is
                     * the full path's own, and marks the texture if dirty) */
                    slot->tex_gen = tg;
                    hit = slot;
                }
            }
        }
        if (hit) {
            r->texture_bindings[i] = hit->snode;
            pg->texture_dirty[i] = false;
            nv2a_tmemo_hits++;
            continue;
        }
        nv2a_tmemo_misses++;
        {   /* why: the first failing condition */
            TexMemo *e = same_regs;
            TextureBinding *m = e ? e->snode : NULL;
            int why = !e ? 8 : (!m || m->image == VK_NULL_HANDLE) ? 1 :
                      e->tex_gen != tg ? 2 : e->surf_gen != nv2a_surf_gen ? 3 :
                      e->texc_gen != s_texc_gen ? 4 : e->epoch != ep ? 5 :
                      m->possibly_dirty ? 6 : m->draw_time ? 7 : 0;
            nv2a_tmemo_why[why]++;
        }
        uint32_t sg = nv2a_surf_gen, cg = s_texc_gen;
        s_tmemo_ok = false;
        create_texture(pg, i);
        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
        TexMemo *e = slot;
        e->valid = s_tmemo_ok;
        if (s_tmemo_ok) {
            e->snap = snap;
            e->snode = r->texture_bindings[i];
            e->tex_gen = tg;
            e->surf_gen = sg;
            e->texc_gen = cg;
            e->epoch = ep;
            TextureBinding *b = r->texture_bindings[i];
            e->off = b->key.texture_vram_offset; e->len = b->key.texture_length;
            e->poff = b->key.palette_vram_offset; e->plen = b->key.palette_length;
        }
    }

    {   static time_t t0; static unsigned h0, m0;
        time_t tn = time(NULL);
        if (!t0) t0 = tn;
        if (tn - t0 >= 10) {
            unsigned h = nv2a_tmemo_hits - h0, m = nv2a_tmemo_misses - m0;
            fprintf(stderr, "[TEXMEMO] %u reused, %u checked in full (%.0f%% reused); why: "
                    "other %u, node %u, guest write %u, surface %u, cache %u, epoch %u, "
                    "marked %u, drawn %u, new registers %u\n",
                    h, m, h + m ? 100.0 * h / (h + m) : 0.0, nv2a_tmemo_why[0], nv2a_tmemo_why[1],
                    nv2a_tmemo_why[2], nv2a_tmemo_why[3], nv2a_tmemo_why[4], nv2a_tmemo_why[5],
                    nv2a_tmemo_why[6], nv2a_tmemo_why[7], nv2a_tmemo_why[8]);
            memset(nv2a_tmemo_why, 0, sizeof nv2a_tmemo_why);
            t0 = tn; h0 = nv2a_tmemo_hits; m0 = nv2a_tmemo_misses;
        }
    }
    r->texture_bindings_changed = true;
    update_timestamps(r);
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->sampler = VK_NULL_HANDLE;
    snode->draw_time = 0;
    snode->hash = 0;
    tex_node_clean(snode);
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    s_texc_gen++;
    vkDestroySampler(r->device, snode->sampler, NULL);
    snode->sampler = VK_NULL_HANDLE;

    vkDestroyImageView(r->device, snode->image_view, NULL);
    snode->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, snode->image, snode->allocation);
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // FIXME: Simplify. We don't really need to check bindings


    // Currently bound
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    // Used in command buffer
    if (r->in_command_buffer && snode->submit_time == r->submit_count) {
        return false;
    }
    /* nv2a-port: or by a frame still in flight */
    if (snode->submit_time >= r->completed_submits &&
        (r->slot_pending[0] || r->slot_pending[1])) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size = 1024;
    lru_init(&r->texture_cache);
    r->texture_cache_entries = g_malloc_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted, r->texture_cache.num_used);
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    create_dummy_texture(pg);

    r->texture_format_properties = g_malloc0_n(
        ARRAY_SIZE(kelvin_color_format_vk_map), sizeof(VkFormatProperties));
    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, kelvin_color_format_vk_map[i].vk_format,
            &r->texture_format_properties[i]);
    }
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    texture_cache_finalize(r);

    assert(r->texture_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}
