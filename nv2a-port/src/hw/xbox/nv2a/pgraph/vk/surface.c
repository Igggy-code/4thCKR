#include <dlfcn.h>
/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/compiler.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

const int num_invalid_surfaces_to_keep = 10;  // FIXME: Make automatic
const int max_surface_frame_time_delta = 5;

void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    // FIXME: It's just flush
    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor; // FIXME: Move internal to renderer
}

unsigned nv2a_global_scale = 1;
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    pg->surface_scale_factor = MAX(factor, 1);
    nv2a_global_scale = pg->surface_scale_factor;
}
/* small colour targets that get drawn with the big depth buffer (the 32x32
 * tiles of the depth-reuse case below) must match its scale */
static hwaddr s_full_scale_addr[64];
static int s_full_scale_n;
static bool full_scale_forced(hwaddr a)
{
    for (int i = 0; i < s_full_scale_n; i++) if (s_full_scale_addr[i] == a) return true;
    return false;
}
static void force_full_scale(hwaddr a)
{
    if (!full_scale_forced(a) && s_full_scale_n < 64) s_full_scale_addr[s_full_scale_n++] = a;
}
unsigned pgraph_vk_scale_for_dims(unsigned w, unsigned h)
{
    /* small targets at 1x saved little GPU time and broke colour/depth
     * pairing (GPU hang, crashes): opt-in only, NV2A_SMALL_HALFSCALE=1 */
    static int full = -1;
    if (full < 0) full = getenv("NV2A_SMALL_HALFSCALE") == NULL;
    if (nv2a_global_scale > 1 && !full && w <= 256 && h <= 256) return 1;
    return nv2a_global_scale;
}
static unsigned scale_for_surface(hwaddr a, unsigned w, unsigned h)
{
    if (full_scale_forced(a)) return nv2a_global_scale;
    return pgraph_vk_scale_for_dims(w, h);
}
/* the bound targets set the scale for drawing */
static void sync_bound_scale(PGRAPHVkState *r)
{
    extern NV2AState *g_nv2a;
    if (!g_nv2a) return;
    PGRAPHState *pg = &g_nv2a->pgraph;
    SurfaceBinding *b = r->color_binding ? r->color_binding : r->zeta_binding;
    pg->surface_scale_factor = (b && b->scale) ? b->scale : nv2a_global_scale;
}

// FIXME: Move to common
static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

static void memcpy_image(void *dst, void const *src, int dst_stride,
                         int src_stride, int height)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, dst_stride * height);
        return;
    }

    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t const *src_ptr = (uint8_t *)src;

    size_t copy_stride = MIN(src_stride, dst_stride);

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, copy_stride);
        dst_ptr += dst_stride;
        src_ptr += src_stride;
    }
}

static bool check_surface_overlaps_range(const SurfaceBinding *surface,
                                         hwaddr range_start, hwaddr range_len)
{
    hwaddr surface_end = surface->vram_addr + surface->size;
    hwaddr range_end = range_start + range_len;
    return !(surface->vram_addr >= range_end || range_start >= surface_end);
}

unsigned int pgraph_vk_mark_surfaces_in_range_for_upload(NV2AState *d,
                                                         hwaddr start,
                                                         hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface;

    unsigned int num_marked = 0;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (check_surface_overlaps_range(surface, start, size)) {
            surface->upload_pending = true;
            { extern uint32_t nv2a_surf_gen; nv2a_surf_gen++; }
            ++num_marked;
        }
    }

    return num_marked;
}

void pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                   hwaddr start, hwaddr size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (check_surface_overlaps_range(surface, start, size)) {
            if (surface->draw_dirty) nv2a_site_hit("dl:range", __builtin_return_address(0), surface->width, surface->height);
            pgraph_vk_surface_download_if_dirty(
                container_of(pg, NV2AState, pgraph), surface);
        }
    }
}


/* nv2a-port: render-to-vertex-buffer. A small target drawn this frame and
 * then read as vertex data (an effect's mesh, PooCabin: one 128x128 target
 * every frame) forced a GPU drain and a synchronous read-back each frame,
 * ~11 ms. The target is read back at the flip anyway (eager), so from its
 * second frame on the vertices come from the previous frame's read-back: a
 * frame late, without the wait. NV2A_RTV_SYNC=1: the old synchronous way. */
void pgraph_vk_download_surfaces_for_vertices(PGRAPHState *pg, hwaddr start, hwaddr size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int sync = -1;
    if (sync < 0) sync = getenv("NV2A_RTV_SYNC") != NULL;
    extern volatile unsigned nv2a_flip_seq;
    static struct { uint32_t addr; unsigned flip, prev; } seen[16];
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(surface, start, size)) continue;
        if (!sync && surface->draw_dirty && surface->color &&
            (long)surface->width * surface->height <= 256L * 256L) {
            uint32_t addr = (uint32_t)surface->vram_addr;
            unsigned now = nv2a_flip_seq;
            int k, slot = -1;
            for (k = 0; k < 16; k++) if (seen[k].addr == addr) { slot = k; break; }
            if (slot < 0) { slot = (int)(now % 16); seen[slot].addr = addr; seen[slot].flip = now; seen[slot].prev = 0; }
            if (seen[slot].flip != now) { seen[slot].prev = seen[slot].flip; seen[slot].flip = now; }
            /* read back in an earlier frame (1-3 frames ago): RAM holds it */
            if (seen[slot].prev && now - seen[slot].prev <= 3) {
                surface->eager_pending = true;     /* brought to RAM at the flip */
                surface->download_pending = true;
                nv2a_site_hit("rtv:late", 0, surface->width, surface->height);
                continue;
            }
        }
        if (surface->draw_dirty) nv2a_site_hit("dl:range", __builtin_return_address(0), surface->width, surface->height);
        pgraph_vk_surface_download_if_dirty(container_of(pg, NV2AState, pgraph), surface);
    }
}

/* nv2a-port: open write-protected guest pages before the GPU model writes
 * a read-back into them (see nv2a_host_prepare_write) */
#include "xxhash.h"
unsigned nv2a_surf_dl_total;   /* nv2a-port: [MEM] line, read-backs per second */
static bool s_upload_skip(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NV2A_UPLOAD_SKIP") != NULL;   /* opt-in: changed the fur */
    return v;
}
static uint64_t surf_ram_hash(NV2AState *d, const SurfaceBinding *s)
{
    size_t len = (size_t)s->pitch * s->height;
    if (!len || s->vram_addr + len > memory_region_size(d->vram)) return 0;
    return XXH3_64bits(d->vram_ptr + s->vram_addr, len);
}
static void surf_note_ram(NV2AState *d, SurfaceBinding *s)
{
    if (!s_upload_skip()) return;
    s->ram_hash = surf_ram_hash(d, s);
    s->ram_hash_valid = true;
}
static bool s_rb_selfdirty(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NV2A_RB_SELFDIRTY") != NULL;
    return v;
}
static inline void prepare_guest_write(NV2AState *d, const uint8_t *p, size_t len)
{
    extern void nv2a_host_prepare_write(uint64_t addr, uint64_t size);
    if (p >= d->vram_ptr && p < d->vram_ptr + memory_region_size(d->vram))
        nv2a_host_prepare_write((uint64_t)(p - d->vram_ptr), len);
}

__attribute__((noinline)) static void download_surface_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    SURF_SCALE_SCOPE(pg, surface);
    prepare_guest_write(d, pixels, (size_t)surface->pitch * surface->height);

    if (!surface->width || !surface->height) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD); nv2a_surf_dl_total++;

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool no_conversion_necessary =
        surface->color || use_compute_to_convert_depth_stencil_format ||
        surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM;

    assert(no_conversion_necessary);

    bool compute_needs_finish = (use_compute_to_convert_depth_stencil_format &&
                                 pgraph_vk_compute_needs_finish(r));

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        {   /* probe: who needs a surface drawn this frame back in RAM now */
            static unsigned nlog;
            if (nlog++ < 25) {
                Dl_info di;
                void *ra1 = __builtin_return_address(1);
                const char *n1 = dladdr(ra1, &di) && di.dli_sname ? di.dli_sname : "?";
                uintptr_t o1 = di.dli_fbase ? (uintptr_t)ra1 - (uintptr_t)di.dli_fbase : 0;
                fprintf(stderr, "[SURFDOWN] %08x %ux%u%s %s from %s (+0x%lx)\n",
                        (unsigned)surface->vram_addr, surface->width, surface->height,
                        surface->swizzle ? "s" : "", surface->color ? "color" : "zeta",
                        n1, (unsigned long)o1);
            }
        }
        pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    } else if (compute_needs_finish) {
        { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
    }

    bool downscale = (pg->surface_scale_factor != 1);

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    // Read surface into memory
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        assert(pg->surface_scale_factor == 1 || downscale);
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int num_copy_regions = 1;
    VkBufferImageCopy copy_regions[2];
    copy_regions[0] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
    };

    VkImage surface_image_loc;
    if (downscale && !use_compute_to_convert_depth_stencil_format) {
        copy_regions[0].imageExtent =
            (VkExtent3D){ surface->width, surface->height, 1 };

        if (surface->image_scratch_current_layout !=
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image_scratch, surface->host_fmt.vk_format,
                surface->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            surface->image_scratch_current_layout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }

        VkImageBlit blit_region = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},
        };

        vkCmdBlitImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       surface->image_scratch,
                       surface->image_scratch_current_layout, 1, &blit_region,
                       VK_FILTER_NEAREST);

        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        surface_image_loc = surface->image_scratch;
    } else {
        copy_regions[0].imageExtent =
            (VkExtent3D){ scaled_width, scaled_height, 1 };
        surface_image_loc = surface->image;
    }

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        size_t depth_size = scaled_width * scaled_height * 4;
        copy_regions[num_copy_regions++] = (VkBufferImageCopy){
            .bufferOffset = ROUND_UP(
                depth_size,
                r->device_props.limits.minStorageBufferOffsetAlignment),
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
        };
    }

    //
    // Copy image to staging buffer, or to compute_dst if we need to pack it
    //

    size_t downloaded_image_size = surface->host_fmt.host_bytes_per_pixel *
                                   surface->width * surface->height;
    assert((downloaded_image_size) <=
           r->storage_buffers[BUFFER_STAGING_DST].buffer_size);

    int copy_buffer_idx = use_compute_to_convert_depth_stencil_format ?
                             BUFFER_COMPUTE_DST :
                             BUFFER_STAGING_DST;
    VkBuffer copy_buffer = r->storage_buffers[copy_buffer_idx].buffer;

    {
        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);
    }
    vkCmdCopyImageToBuffer(cmd, surface_image_loc,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                           num_copy_regions, copy_regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    // FIXME: Verify output of depth stencil conversion
    // FIXME: Track current layout and only transition when required

    if (use_compute_to_convert_depth_stencil_format) {
        size_t bytes_per_pixel = 4;
        size_t packed_size =
            downscale ? (surface->width * surface->height * bytes_per_pixel) :
                        (scaled_width * scaled_height * bytes_per_pixel);

        //
        // Pack the depth-stencil image into compute_src buffer
        //

        VkBufferMemoryBarrier pre_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_src_barrier, 0, NULL);

        VkBuffer pack_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

        VkBufferMemoryBarrier pre_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(pg, surface, cmd, copy_buffer, pack_buffer,
                                     downscale);

        VkBufferMemoryBarrier post_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_dst_barrier, 0, NULL);

        //
        // Copy packed image over to staging buffer for host download
        //

        copy_buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer;

        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);

        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, pack_buffer, copy_buffer, 1, &buffer_copy_region);

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);
    }

    //
    // Download image data to host
    //

    VkBufferMemoryBarrier post_copy_dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &post_copy_dst_barrier, 0, NULL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    void *mapped_memory_ptr = NULL;
    pgraph_vk_wait_deferred();
    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_DST].allocation,
                          &mapped_memory_ptr));

    vmaInvalidateAllocation(r->allocator,
                            r->storage_buffers[BUFFER_STAGING_DST].allocation,
                            0, VK_WHOLE_SIZE);

    memcpy_image(gl_read_buf, mapped_memory_ptr, surface->pitch,
                 surface->width * surface->fmt.bytes_per_pixel,
                 surface->height);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_DST].allocation);

    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        swizzle_rect(swizzle_buf, surface->width, surface->height, pixels,
                     surface->pitch, surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        g_free(swizzle_buf);
    }
}

static void download_surface_done(NV2AState *d, SurfaceBinding *surface)
{
    surf_note_ram(d, surface);
    /* the vertex remap cache's client: render-to-vertex-buffer */
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_MIGRATION);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);
    /* nv2a-port: the GPU copy is the one just written to RAM, so the
     * surface must not count as CPU-modified: re-uploading it on its next
     * bind cost an upload and a full GPU drain per small render target
     * per frame. Swallow the dirt the write itself left behind as well.
     * NV2A_DL_DIRTY=1 restores xemu's behaviour. */
    static int dl_dirty = -1;
    static long dl_min = 0, dl_max = 0;
    if (dl_dirty < 0) {
        dl_dirty = getenv("NV2A_DL_DIRTY") != NULL;
        const char *e = getenv("NV2A_DL_DIRTY_MAX");
        dl_max = e ? atol(e) : 0;
        e = getenv("NV2A_DL_DIRTY_MIN");
        dl_min = e ? atol(e) : 0;
        if (getenv("NV2A_DL_DIRTY_SMALL")) { dl_min = 0; dl_max = 1024; }
        if (getenv("NV2A_DL_DIRTY_MID")) { dl_min = 1025; dl_max = 65536; }
        if (getenv("NV2A_DL_DIRTY_EAGER")) { dl_min = 0; dl_max = 65536; }
        if (getenv("NV2A_DL_DIRTY_BIG")) { dl_min = 65537; dl_max = 1L << 30; }
        /* Default: the mid-size targets (64x64..256x256) stay dirty after a
         * read-back. Conker's bar draws its characters through them, and
         * without the re-upload the characters vanished at random. */
        if (!dl_dirty && dl_max == 0 && !getenv("NV2A_DL_DIRTY_NONE")) {
            dl_min = 1025; dl_max = 65536;
        }
        if (dl_max > 0 && !dl_dirty) dl_dirty = 2;
    }
    long dl_px = (long)surface->width * (long)surface->height;
    if (dl_dirty == 1 || (dl_dirty == 2 && dl_px <= dl_max && dl_px >= dl_min))
        memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                       surface->pitch * surface->height,
                                       DIRTY_MEMORY_NV2A);
    else {
        memory_region_test_and_clear_dirty(d->vram, surface->vram_addr,
                                           surface->pitch * surface->height,
                                           DIRTY_MEMORY_NV2A);
        /* ...but other surfaces aliasing this memory did not see the write:
         * a title that renders small tiles straight into a larger target
         * (Conker's 32x32 character tiles inside a 128x128/256x256 atlas)
         * needs the larger one re-uploaded on its next bind, or the tiles
         * never reach it and the characters vanish. Dirty just the
         * overlap, for the aliasing surfaces only. */
        PGRAPHVkState *r = d->pgraph.vk_renderer_state;
        hwaddr lo = surface->vram_addr;
        hwaddr hi = lo + (hwaddr)surface->pitch * surface->height;
        SurfaceBinding *o;
        QTAILQ_FOREACH(o, &r->surfaces, entry) {
            if (o == surface) continue;
            hwaddr olo = o->vram_addr, ohi = olo + o->size;
            if (ohi <= lo || olo >= hi) continue;
            hwaddr a = MAX(lo, olo), b = MIN(hi, ohi);
            nv2a_site_hit("alias:dirty", 0, o->width, o->height);
            {   static int nlog;
                if (getenv("NV2A_ALIAS_LOG") && nlog < 400) {
                    nlog++;
                    fprintf(stderr, "[ALIAS] tile %08lx %ux%u pitch %u %s fmt %u bpp %u c%d dd%d -> "
                            "big %08lx %ux%u pitch %u %s fmt %u c%d dd%d dlp%d scale %u/%u off %ld\n",
                            (long)surface->vram_addr, surface->width, surface->height, surface->pitch,
                            surface->swizzle ? "sz" : "ln", surface->shape.color_format,
                            surface->fmt.bytes_per_pixel, surface->color, surface->draw_dirty,
                            (long)o->vram_addr, o->width, o->height, o->pitch,
                            o->swizzle ? "sz" : "ln", o->shape.color_format, o->color, o->draw_dirty,
                            o->download_pending, surface->scale, o->scale,
                            (long)(surface->vram_addr - o->vram_addr));
                }
            }
            memory_region_set_client_dirty(d->vram, a, b - a,
                                           DIRTY_MEMORY_NV2A);
        }
    }

    surface->download_pending = false;
    surface->draw_dirty = false;
    { extern uint32_t nv2a_surf_gen; nv2a_surf_gen++; }
}

__attribute__((noinline)) static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }
    nv2a_site_hit("download", __builtin_return_address(0), surface->width, surface->height);

    // FIXME: Respect write enable at last TOU?

    static int dlcmp = -1;
    if (dlcmp < 0) dlcmp = getenv("NV2A_DLCMP") != NULL;
    if (dlcmp && surface->color && surface->width >= 32 &&
        (long)surface->width * surface->height <= 65536) {
        /* Diagnostic: what did RAM hold just before the GPU copy lands? */
        size_t sz = (size_t)surface->pitch * surface->height;
        uint8_t *tmp = g_malloc(sz);
        uint8_t *ram = d->vram_ptr + surface->vram_addr;
        memcpy(tmp, ram, sz);
        bool was_dirty = memory_region_get_dirty(d->vram, surface->vram_addr, sz,
                                                 DIRTY_MEMORY_NV2A);
        download_surface_to_buffer(d, surface, ram);
        unsigned diff = 0, diff_a = 0, bpp = surface->fmt.bytes_per_pixel;
        for (size_t i = 0; i < sz; i++)
            if (tmp[i] != ram[i]) { diff++; if (bpp == 4 && (i & 3) == 3) diff_a++; }
        static unsigned nlog;
        if (diff && (nlog++ < 300 || (nlog % 200) == 0))
            fprintf(stderr, "[DLCMP] %08x %ux%u%s fmt%u cpu_dirty=%d diff=%u alpha=%u of %zu\n",
                    (unsigned)surface->vram_addr, surface->width, surface->height,
                    surface->swizzle ? "s" : "", (unsigned)surface->shape.color_format,
                    was_dirty, diff, diff_a, sz);
        g_free(tmp);
    } else
    download_surface_to_buffer(d, surface, d->vram_ptr + surface->vram_addr);

    download_surface_done(d, surface);
}

void pgraph_vk_wait_for_surface_download(SurfaceBinding *surface)
{
    NV2AState *d = g_nv2a;

    if (qatomic_read(&surface->draw_dirty)) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&d->pgraph.vk_renderer_state->downloads_complete);
        qatomic_set(&surface->download_pending, true);
        qatomic_set(&d->pgraph.vk_renderer_state->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.vk_renderer_state->downloads_complete);
    }
}

void pgraph_vk_process_pending_downloads(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        download_surface(d, surface, false);
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

/* nv2a-port frame log: write every non-swizzled colour surface >= 128 px
 * wide to NV2A_DUMP_DIR as raw bytes (rt_<tag>_<addr>_<w>x<h>_<bpp>.raw). */
void pgraph_vk_dump_surfaces(NV2AState *d, const char *tag)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    const char *dir = getenv("NV2A_DUMP_DIR");
    if (!dir) return;
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        extern volatile int nv2a_dump_all;   /* REC: every colour target */
        /* REC also takes the depth buffers (z_ prefix): the striped sky is a
         * skybox losing its depth test on alternate rows */
        if (!surface->color && !nv2a_dump_all) continue;
        if (!nv2a_dump_all && (surface->swizzle || surface->width < 128)) continue;
        if (!surface->color && surface->width < 128) continue;
        download_surface(d, surface, true);
        char path[512];
        snprintf(path, sizeof path, "%s/%s_%s_%08x_%ux%u_%u%s.raw", dir,
                 surface->color ? "rt" : "z", tag,
                 (unsigned)surface->vram_addr, surface->width, surface->height,
                 surface->fmt.bytes_per_pixel, surface->swizzle ? "s" : "");
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        for (unsigned y = 0; y < surface->height; y++)
            fwrite(d->vram_ptr + surface->vram_addr + (size_t)y * surface->pitch,
                   1, (size_t)surface->width * surface->fmt.bytes_per_pixel, f);
        fclose(f);
        fprintf(stderr, "[FRAME] dumped %s\n", path);
    }
}

/* nv2a-port: read a colour surface back and log what it holds. */
void pgraph_vk_stat_surface(NV2AState *d, SurfaceBinding *s, const char *label, int idx)
{
    if (!s || !s->color || !s->width || !s->height) return;
    download_surface(d, s, true);
    const uint8_t *base = d->vram_ptr + s->vram_addr;
    unsigned bpp = s->fmt.bytes_per_pixel, nz = 0, nza = 0;
    uint64_t sum[4] = {0};
    for (unsigned y = 0; y < s->height; y += 2) {
        const uint8_t *row = base + (size_t)y * s->pitch;
        for (unsigned x = 0; x < s->width; x += 2) {
            const uint8_t *px = row + x * bpp;
            if (bpp == 4) {
                if (px[0] | px[1] | px[2]) nz++;
                if (px[3]) nza++;
                for (int c = 0; c < 4; c++) sum[c] += px[c];
            } else {
                if (px[0] | px[1]) nz++;
                sum[0] += px[0]; sum[1] += px[1];
            }
        }
    }
    unsigned n = ((s->width + 1) / 2) * ((s->height + 1) / 2);
    fprintf(stderr, "[DSTAT] %s #%d %08x %ux%u%s nzRGB=%u/%u nzA=%u avgBGRA=%u,%u,%u,%u\n",
            label, idx, (unsigned)s->vram_addr, s->width, s->height, s->swizzle ? "s" : "",
            nz, n, nza, (unsigned)(sum[0] / n), (unsigned)(sum[1] / n),
            (unsigned)(sum[2] / n), (unsigned)(sum[3] / n));
}

void pgraph_vk_stat_all_surfaces(NV2AState *d, const char *label)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->color && surface->width >= 640)
            pgraph_vk_stat_surface(d, surface, label, -1);
    }
}

/* nv2a-port: the eager read-backs of one frame in one submission. One
 * round trip to the GPU each (a dozen 32x32 character tiles plus the atlases
 * per frame in Conker's bar) cost more than the frame itself on Mali. Colour
 * targets at scale 1 only; the rest go the usual way.
 * NV2A_NO_DLBATCH=1 disables. */
/* nv2a-port: a frame's eager read-backs recorded at the end of its own
 * command buffer; the CPU copy-out happens when the frame slot retires. No
 * one-off submission and no wait of its own. NV2A_NO_INFRAME_RB=1 uses the
 * one-off batch instead. */
#define RB_MAX 64
static struct { SurfaceBinding *s; hwaddr addr; unsigned draw_time; VkDeviceSize off; }
    s_rb[2][RB_MAX];
static int s_rb_n[2];
static VkDeviceSize s_rb_end[2];
unsigned nv2a_rb_recorded;
bool nv2a_rb_no_host_barrier;

void pgraph_vk_record_eager_readback(PGRAPHState *pg, int slot)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int off_ = -1;
    if (off_ < 0) off_ = getenv("NV2A_NO_INFRAME_RB") != NULL;
    static int scaled_ok = -1;
    if (scaled_ok < 0) scaled_ok = !getenv("NV2A_NO_SCALED_RB");
    if (off_ || (pg->surface_scale_factor != 1 && !scaled_ok) || r->nslots != 2) return;
    StorageBuffer *dst = &r->storage_buffers[BUFFER_STAGING_DST];
    VkDeviceSize half = (dst->buffer_size / 2) & ~(VkDeviceSize)0xFFF;
    /* appends: a frame may read back mid-buffer, at semaphore releases */
    VkDeviceSize base = slot ? half : 0;
    VkDeviceSize off = s_rb_n[slot] ? s_rb_end[slot] : base;
    int first = s_rb_n[slot];
    VkCommandBuffer cmd = r->command_buffer;
    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry) {
        if (!s->eager_pending || !s->download_pending || !s->color ||
            !s->width || !s->height || s_rb_n[slot] >= RB_MAX)
            continue;
        if (s->host_fmt.host_bytes_per_pixel != s->fmt.bytes_per_pixel) continue;
        VkDeviceSize sz = (VkDeviceSize)s->host_fmt.host_bytes_per_pixel *
                          s->width * s->height;
        if (off + sz > base + half) continue;
        nv2a_rb_recorded++;
        {   extern int nv2a_sem_guard_mode(void);
            extern void nv2a_host_guard(uint64_t addr, uint64_t size);
            if (nv2a_sem_guard_mode())
                nv2a_host_guard(s->vram_addr, (uint64_t)s->pitch * s->height); }
        pgraph_vk_transition_image_layout(
            pg, cmd, s->image, s->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = {
            .bufferOffset = off,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ s->width, s->height, 1 },
        };
        SURF_SCALE_SCOPE(pg, s);
        if (pg->surface_scale_factor != 1 && s->image_scratch) {
            /* scaled target: down to native size in its scratch image first
             * (nearest, as the synchronous download does), then copy */
            unsigned sw = s->width, sh = s->height;
            pgraph_apply_scaling_factor(pg, &sw, &sh);
            if (s->image_scratch_current_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                pgraph_vk_transition_image_layout(pg, cmd, s->image_scratch, s->host_fmt.vk_format,
                    s->image_scratch_current_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                s->image_scratch_current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            }
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets[1] = (VkOffset3D){ (int)sw, (int)sh, 1 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets[1] = (VkOffset3D){ (int)s->width, (int)s->height, 1 },
            };
            vkCmdBlitImage(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->image_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
            pgraph_vk_transition_image_layout(pg, cmd, s->image_scratch, s->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            s->image_scratch_current_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdCopyImageToBuffer(cmd, s->image_scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   dst->buffer, 1, &region);
        } else {
            vkCmdCopyImageToBuffer(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   dst->buffer, 1, &region);
        }
        pgraph_vk_transition_image_layout(
            pg, cmd, s->image, s->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        int k = s_rb_n[slot]++;
        s_rb[slot][k].s = s;
        s_rb[slot][k].addr = s->vram_addr;
        s_rb[slot][k].draw_time = s->draw_time;
        s_rb[slot][k].off = off;
        s->eager_pending = false;   /* taken care of by this frame */
        nv2a_site_hit("download:inframe", 0, s->width, s->height);
        off = ROUND_UP(off + sz, 256);
    }
    s_rb_end[slot] = off;
    if (s_rb_n[slot] == first) return;
    if (nv2a_rb_no_host_barrier) return;   /* one at the submit instead */
    VkBufferMemoryBarrier post = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst->buffer,
        .offset = base,
        .size = off - base,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &post, 0, NULL);
}

void pgraph_vk_complete_eager_readback(NV2AState *d, int slot)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    if (!s_rb_n[slot]) return;
    StorageBuffer *dst = &r->storage_buffers[BUFFER_STAGING_DST];
    VkDeviceSize half = (dst->buffer_size / 2) & ~(VkDeviceSize)0xFFF;
    VkDeviceSize base = slot ? half : 0;
    void *mapped = NULL;
    VK_CHECK(vmaMapMemory(r->allocator, dst->allocation, &mapped));
    vmaInvalidateAllocation(r->allocator, dst->allocation, base, s_rb_end[slot] - base);
    for (int k = 0; k < s_rb_n[slot]; k++) {
        SurfaceBinding *s = s_rb[slot][k].s, *it;
        bool live = false;
        QTAILQ_FOREACH(it, &r->surfaces, entry) if (it == s) { live = true; break; }
        if (!live || s->vram_addr != s_rb[slot][k].addr) continue;
        uint8_t *pixels = d->vram_ptr + s->vram_addr;
        prepare_guest_write(d, pixels, (size_t)s->pitch * s->height);
        uint8_t *src = (uint8_t *)mapped + s_rb[slot][k].off;
        if (s->swizzle) {
            /* straight from the tightly packed read-back, no bounce copy */
            swizzle_rect(src, s->width, s->height, pixels,
                         s->width * s->fmt.bytes_per_pixel, s->fmt.bytes_per_pixel);
            nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        } else {
            memcpy_image(pixels, src, s->pitch, s->width * s->fmt.bytes_per_pixel, s->height);
        }
        nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD); nv2a_surf_dl_total++;
        surf_note_ram(d, s);
        /* our own write of the surface's pixels must not mark the surface
         * dirty for itself (that re-uploaded it next frame, sometimes with
         * pixels older than the GPU's). NV2A_RB_SELFDIRTY=1: old way. */
        if (!s_rb_selfdirty())
            memory_region_test_and_clear_dirty(d->vram, s->vram_addr,
                                               (hwaddr)s->pitch * s->height, DIRTY_MEMORY_NV2A);
        if (s->draw_time == s_rb[slot][k].draw_time) {
            download_surface_done(d, s);   /* nothing drawn to it since */
        } else {
            /* drawn again in a later frame: RAM has this frame's pixels now,
             * the later frame's read-back will bring it up to date */
            memory_region_set_client_dirty(d->vram, s->vram_addr,
                                           s->pitch * s->height, DIRTY_MEMORY_VGA);
            memory_region_set_client_dirty(d->vram, s->vram_addr,
                                           s->pitch * s->height, DIRTY_MEMORY_NV2A_TEX);
        }
    }
    vmaUnmapMemory(r->allocator, dst->allocation);
    s_rb_n[slot] = 0;
}

static void download_eager_batched(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    enum { MAX_BATCH = 64 };
    SurfaceBinding *batch[MAX_BATCH];
    VkDeviceSize offs[MAX_BATCH];
    int n = 0;
    VkDeviceSize off = 0;
    StorageBuffer *dst = &r->storage_buffers[BUFFER_STAGING_DST];

    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry) {
        if ((s->scale ? s->scale : nv2a_global_scale) != 1) continue;
        if (!s->eager_pending || !s->download_pending || !s->color ||
            !s->width || !s->height || n >= MAX_BATCH)
            continue;
        if (r->in_command_buffer && s->draw_time >= r->command_buffer_start_time)
            continue;   /* drawn again in the frame being recorded: later */
        VkDeviceSize sz = (VkDeviceSize)s->host_fmt.host_bytes_per_pixel *
                          s->width * s->height;
        if (s->host_fmt.host_bytes_per_pixel != s->fmt.bytes_per_pixel ||
            off + sz > dst->buffer_size)
            continue;
        batch[n] = s;
        offs[n] = off;
        n++;
        off = ROUND_UP(off + sz, 256);
    }
    if (n < 2) return;

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    VkBufferMemoryBarrier pre = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst->buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &pre,
                         0, NULL);
    for (int i = 0; i < n; i++) {
        s = batch[i];
        nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD); nv2a_surf_dl_total++;
        nv2a_site_hit("download:batch", 0, s->width, s->height);
        pgraph_vk_transition_image_layout(
            pg, cmd, s->image, s->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = {
            .bufferOffset = offs[i],
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ s->width, s->height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, s->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst->buffer, 1, &region);
        pgraph_vk_transition_image_layout(
            pg, cmd, s->image, s->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    VkBufferMemoryBarrier post = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst->buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &post, 0,
                         NULL);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_vk_end_single_time_commands(pg, cmd);

    void *mapped = NULL;
    pgraph_vk_wait_deferred();
    VK_CHECK(vmaMapMemory(r->allocator, dst->allocation, &mapped));
    vmaInvalidateAllocation(r->allocator, dst->allocation, 0, off);
    for (int i = 0; i < n; i++) {
        s = batch[i];
        uint8_t *pixels = d->vram_ptr + s->vram_addr;
        prepare_guest_write(d, pixels, (size_t)s->pitch * s->height);
        uint8_t *src = (uint8_t *)mapped + offs[i];
        static int dlcmp = -1;
        if (dlcmp < 0) dlcmp = getenv("NV2A_DLCMP") != NULL;
        uint8_t *before = NULL;
        bool was_dirty = false;
        size_t psz = (size_t)s->pitch * s->height;
        if (dlcmp && s->width >= 32) {
            before = g_malloc(psz);
            memcpy(before, pixels, psz);
            was_dirty = memory_region_get_dirty(d->vram, s->vram_addr, psz,
                                                DIRTY_MEMORY_NV2A);
        }
        if (s->swizzle) {
            uint8_t *tmp = g_malloc(s->size);
            memcpy_image(tmp, src, s->pitch, s->width * s->fmt.bytes_per_pixel,
                         s->height);
            swizzle_rect(tmp, s->width, s->height, pixels, s->pitch,
                         s->fmt.bytes_per_pixel);
            nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
            g_free(tmp);
        } else {
            memcpy_image(pixels, src, s->pitch,
                         s->width * s->fmt.bytes_per_pixel, s->height);
        }
        if (before) {
            unsigned diff = 0, diff_a = 0, ovl = 0;
            for (size_t k = 0; k < psz; k++)
                if (before[k] != pixels[k]) { diff++; if ((k & 3) == 3) diff_a++; }
            SurfaceBinding *o;
            QTAILQ_FOREACH(o, &r->surfaces, entry)
                if (o != s && o->vram_addr < s->vram_addr + psz &&
                    s->vram_addr < o->vram_addr + o->size) ovl++;
            static unsigned nlog;
            if (nlog++ < 400 || (nlog % 100) == 0)
                fprintf(stderr, "[DLCMP] %08x %ux%u%s fmt%u cpu_dirty=%d diff=%u alpha=%u of %zu ovl=%u\n",
                        (unsigned)s->vram_addr, s->width, s->height,
                        s->swizzle ? "s" : "", (unsigned)s->shape.color_format,
                        was_dirty, diff, diff_a, psz, ovl);
            g_free(before);
        }
        s->eager_pending = false;
        download_surface_done(d, s);
    }
    vmaUnmapMemory(r->allocator, dst->allocation);
}

void pgraph_vk_download_eager_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *surface;
    static int batched = -1;
    if (batched < 0) batched = !getenv("NV2A_NO_DLBATCH");
    if (batched) download_eager_batched(d);
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->eager_pending) {
            if (r->in_command_buffer &&
                surface->draw_time >= r->command_buffer_start_time)
                continue;   /* drawn again in the frame being recorded */
            if (!surface->color && pgraph_vk_compute_needs_finish(r))
                continue;   /* would have to finish the frame: later */
            surface->eager_pending = false;
            if (surface->download_pending)
                download_surface(d, surface, false);
        }
    }
}

void pgraph_vk_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        pgraph_vk_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    bool wait_for_downloads = false;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(surface, addr, len)) {
            continue;
        }

        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            surface->download_pending = true;
            wait_for_downloads = true;
        }

        if (write) {
            surface->upload_pending = true;
            { extern uint32_t nv2a_surf_gen; nv2a_surf_gen++; }
        }
    }

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
}

static void bind_surface(PGRAPHVkState *r, SurfaceBinding *surface)
{
    if (surface->color) {
        r->color_binding = surface;
    } else {
        r->zeta_binding = surface;
    }

    r->framebuffer_dirty = true;
    sync_bound_scale(r);
}

static void unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (color) {
        if (r->color_binding) {
            r->color_binding = NULL;
            r->framebuffer_dirty = true;
        }
    } else {
        if (r->zeta_binding) {
            r->zeta_binding = NULL;
            r->framebuffer_dirty = true;
        }
    }
    sync_bound_scale(r);
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    // FIXME: We may be reading from the surface in the current command buffer!
    // Add a detection to handle it. For now, finish to be safe.
    /* nv2a-port: only when this command buffer draws to it or copies from
     * it. An old surface aged out (expire_old_surfaces, ~0.4 a frame in the
     * bar) is in neither, and the finish cost a full GPU sync each time --
     * 4 ms a frame on average. Earlier frames still on the GPU may use it:
     * wait for those (normally done already). NV2A_EVICT_FINISH=1: always. */
    static int evict_finish = -1;
    if (evict_finish < 0) evict_finish = getenv("NV2A_EVICT_FINISH") != NULL;
    if (evict_finish || !r->in_command_buffer ||
        surface->draw_time >= (int)r->command_buffer_start_time ||
        surface->read_time >= (int)r->command_buffer_start_time) {
        pgraph_vk_finish(&d->pgraph, VK_FINISH_REASON_SURFACE_DOWN);
    } else {
        nv2a_site_hit("evict:nofinish", 0, surface->width, surface->height);
        pgraph_vk_wait_deferred();
    }

    assert((!r->in_command_buffer ||
            surface->draw_time < r->command_buffer_start_time) &&
           "Surface evicted while in use!");

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    QTAILQ_INSERT_HEAD(&r->invalid_surfaces, surface, entry);
    { extern uint32_t nv2a_surf_gen; nv2a_surf_gen++; }
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(surface, other_surface->vram_addr,
                                        other_surface->size);
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *other_surface, *next_surface;
    QTAILQ_FOREACH_SAFE (other_surface, &r->surfaces, entry, next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width,
                other_surface->height, other_surface->pitch);
            nv2a_site_hit("evict:overlap", 0, other_surface->width, other_surface->height);
            pgraph_vk_surface_download_if_dirty(d, other_surface);
            invalidate_surface(d, other_surface);
        }
    }
}

static void surface_put(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    assert(pgraph_vk_surface_get(d, surface->vram_addr) == NULL);

    invalidate_overlapping_surfaces(d, surface);
    register_cpu_access_callback(d, surface);

    QTAILQ_INSERT_HEAD(&r->surfaces, surface, entry);
    { extern uint32_t nv2a_surf_gen; nv2a_surf_gen++; }
}

SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

static void set_surface_label(PGRAPHState *pg, SurfaceBinding const *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Surface %" HWADDR_PRIx "h fmt:%s,%02xh %dx%d aa:%d",
        surface->vram_addr, surface->color ? "Color" : "Zeta",
        surface->color ? surface->shape.color_format :
                         surface->shape.zeta_format,
        surface->width, surface->height, pg->surface_shape.anti_aliasing);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)surface->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, surface->allocation, label);

    if (surface->image_scratch) {
        g_autofree gchar *label_scratch =
            g_strdup_printf("%s (scratch)", label);
        name_info.objectHandle = (uint64_t)surface->image_scratch;
        name_info.pObjectName = label_scratch;
        if (r->debug_utils_extension_enabled) {
            vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
        }
        vmaSetAllocationName(r->allocator, surface->allocation_scratch,
                             label_scratch);
    }
}

static void create_surface_image(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width = surface->width ? surface->width : 1;
    unsigned int height = surface->height ? surface->height : 1;
    surface->scale = scale_for_surface(surface->vram_addr, width, height);
    surface->ram_hash_valid = false;
    /* a depth buffer always takes the scale of the colour target it is
     * drawn with: a 1x/2x pair in one pass hangs the Mali GPU */
    if (!surface->color && r->color_binding && r->color_binding->scale &&
        r->color_binding->width == surface->width &&
        r->color_binding->height == surface->height)
        surface->scale = r->color_binding->scale;
    SURF_SCALE_SCOPE(pg, surface);
    pgraph_apply_scaling_factor(pg, &width, &height);

    assert(!surface->image);
    assert(!surface->image_scratch);

    NV2A_VK_DPRINTF(
        "Creating new surface image width=%d height=%d @ %08" HWADDR_PRIx,
        width, height, surface->vram_addr);

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = surface->host_fmt.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | surface->host_fmt.usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImageCompressionControlEXT nv2a_icc;
    nv2a_vk_image_no_compress(r, &image_create_info, &nv2a_icc);
    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image,
                            &surface->allocation, NULL));

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image_scratch,
                            &surface->allocation_scratch, NULL));
    surface->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = surface->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = surface->host_fmt.vk_format,
        .subresourceRange.aspectMask = surface->host_fmt.aspect,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &surface->image_view));

    // FIXME: Go right into main command buffer
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    /* nv2a-port: give every new surface defined contents. Mali keeps
     * render targets AFBC-compressed; reading one that was never written
     * (UNDEFINED -> ATTACHMENT says nothing about its bytes) makes the GPU
     * decode garbage headers and hang until the driver resets it. */
    {
        VkImage imgs[2] = { surface->image, surface->image_scratch };
        for (int i = 0; i < 2; i++) {
            pgraph_vk_transition_image_layout(
                pg, cmd, imgs[i], surface->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageSubresourceRange range = {
                .aspectMask = surface->host_fmt.aspect,
                .levelCount = 1,
                .layerCount = 1,
            };
            if (surface->color) {
                VkClearColorValue c = { .float32 = { 0, 0, 0, 0 } };
                vkCmdClearColorImage(cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &c, 1, &range);
            } else {
                VkClearDepthStencilValue z = { .depth = 1.0f, .stencil = 0 };
                vkCmdClearDepthStencilImage(cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            &z, 1, &range);
            }
        }
        surface->image_scratch_current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_3);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);
    nv2a_profile_inc_counter(NV2A_PROF_SURF_CREATE);
}

static void migrate_surface_image(SurfaceBinding *dst, SurfaceBinding *src)
{
    dst->image = src->image;
    dst->image_view = src->image_view;
    dst->allocation = src->allocation;
    dst->image_scratch = src->image_scratch;
    dst->image_scratch_current_layout = src->image_scratch_current_layout;
    dst->allocation_scratch = src->allocation_scratch;

    src->image = VK_NULL_HANDLE;
    src->image_view = VK_NULL_HANDLE;
    src->allocation = VK_NULL_HANDLE;
    src->image_scratch = VK_NULL_HANDLE;
    src->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    src->allocation_scratch = VK_NULL_HANDLE;
}

static void destroy_surface_image(PGRAPHVkState *r, SurfaceBinding *surface)
{
    vkDestroyImageView(r->device, surface->image_view, NULL);
    surface->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image, surface->allocation);
    surface->image = VK_NULL_HANDLE;
    surface->allocation = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image_scratch,
                    surface->allocation_scratch);
    surface->image_scratch = VK_NULL_HANDLE;
    surface->allocation_scratch = VK_NULL_HANDLE;
}

static bool check_invalid_surface_is_compatibile(SurfaceBinding *surface,
                                                 SurfaceBinding *target)
{
    return surface->host_fmt.vk_format == target->host_fmt.vk_format &&
           surface->width == target->width &&
           surface->height == target->height &&
           surface->host_fmt.usage == target->host_fmt.usage;
}

static SurfaceBinding *
get_any_compatible_invalid_surface(PGRAPHVkState *r, SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        if (check_invalid_surface_is_compatibile(surface, target)) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            return surface;
        }
    }

    return NULL;
}

static void prune_invalid_surfaces(PGRAPHVkState *r, int keep)
{
    int num_surfaces = 0;

    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        num_surfaces += 1;
        if (num_surfaces > keep) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            destroy_surface_image(r, surface);
            g_free(surface);
        }
    }
}

static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            nv2a_site_hit("evict:old", 0, s->width, s->height);
            pgraph_vk_surface_download_if_dirty(d, s);
            invalidate_surface(d, s);
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding const *s1,
                                        SurfaceBinding const *s2, bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->host_fmt.vk_format == s2->host_fmt.vk_format) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        download_surface(d, surface, true);
    }
}

void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force)
{
    SURF_SCALE_SCOPE(&d->pgraph, surface);
    surface->read_time = d->pgraph.draw_time;   /* nv2a-port: used by this cb */
    { extern int nv2a_vk_cb_surfup; nv2a_vk_cb_surfup++; }
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(surface->upload_pending || force)) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);
    surf_note_ram(d, surface);   /* the image is about to equal RAM */

    /* nv2a-port: a colour surface at scale 1 is uploaded inside the frame's
     * command buffer: the copy is ordered after whatever already used the
     * image there, so nothing has to drain first. It was a full GPU round
     * trip (finish plus a one-off submission) per upload, several a frame.
     * NV2A_SYNC_SURFUP=1 restores the old path. */
    static int sync_up = -1;
    if (sync_up < 0) sync_up = getenv("NV2A_SYNC_SURFUP") != NULL;
    StorageBuffer *ub = &r->storage_buffers[BUFFER_SURF_UPLOAD];
    size_t up_size = (size_t)surface->width * surface->height *
                     surface->fmt.bytes_per_pixel;
    static int scaled_inline = -1;   /* the scaled variant lost the device once: opt-in */
    if (scaled_inline < 0) scaled_inline = getenv("NV2A_SCALED_INLINE_UP") != NULL;
    if (!sync_up && surface->color &&
        (pg->surface_scale_factor == 1 || (scaled_inline && surface->image_scratch)) &&
        surface->width && surface->height &&
        surface->host_fmt.host_bytes_per_pixel == surface->fmt.bytes_per_pixel &&
        up_size <= (ub->slot_hi - ub->slot_lo) / 2) {
        nv2a_site_hit("upload:inline", __builtin_return_address(0),
                      surface->width, surface->height);
        surface->upload_pending = false;
        surface->draw_time = pg->draw_time;
        if (ROUND_UP(ub->buffer_offset, 256) + up_size > ub->slot_hi) {
            { static unsigned _bs; if (_bs++ < 20) fprintf(stderr, "[BUFSPACE] %s:%d\n", __FILE__, __LINE__); pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE); }
        }
        pgraph_vk_poll_slots();
        VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
        VkDeviceSize off = ROUND_UP(ub->buffer_offset, 256);
        uint8_t *src = d->vram_ptr + surface->vram_addr;
        if (surface->swizzle) {
            uint8_t *tmp = g_malloc(surface->size);
            unswizzle_rect(src, surface->width, surface->height, tmp,
                           surface->pitch, surface->fmt.bytes_per_pixel);
            memcpy_image(ub->mapped + off, tmp,
                         surface->width * surface->fmt.bytes_per_pixel,
                         surface->pitch, surface->height);
            g_free(tmp);
        } else {
            memcpy_image(ub->mapped + off, src,
                         surface->width * surface->fmt.bytes_per_pixel,
                         surface->pitch, surface->height);
        }
        ub->buffer_offset = off + up_size;

        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = ub->buffer,
            .offset = off,
            .size = up_size,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &hb, 0, NULL);
        VkBufferImageCopy region = {
            .bufferOffset = off,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
        };
        if (pg->surface_scale_factor > 1) {
            /* scaled: through the 1x scratch image, blitted up. This command
             * buffer runs before the frame's, which was recorded against the
             * tracked scratch layout: start from UNDEFINED (its contents are
             * scratch) and hand it back in the layout the tracking says. */
            VkImageLayout keep = surface->image_scratch_current_layout;
            if (keep == VK_IMAGE_LAYOUT_UNDEFINED) keep = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                surface->host_fmt.vk_format, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cmd, ub->buffer, surface->image_scratch,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                surface->host_fmt.vk_format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image, surface->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            unsigned sw = surface->width, sh = surface->height;
            pgraph_apply_scaling_factor(pg, &sw, &sh);
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets[1] = (VkOffset3D){ surface->width, surface->height, 1 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets[1] = (VkOffset3D){ sw, sh, 1 },
            };
            vkCmdBlitImage(cmd, surface->image_scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           surface->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
            if (keep != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                    surface->host_fmt.vk_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, keep);
            surface->image_scratch_current_layout = keep;
        } else {
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image, surface->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cmd, ub->buffer, surface->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
        }
        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image, surface->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        pgraph_vk_end_nondraw_commands(pg, cmd);
        surface->initialized = true;
        return;
    }

    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE); // FIXME: SURFACE_UP

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    {
        extern int nv2a_framelog_on;
        static int n_logged;
        if (surface->color && (nv2a_framelog_on || (surface->width >= 640 && n_logged < 400))) {
            n_logged++;
            const uint8_t *b = d->vram_ptr + surface->vram_addr;
            unsigned nz = 0;
            size_t sz = (size_t)surface->pitch * surface->height;
            for (size_t i = 0; i < sz; i += 256) nz += b[i] != 0;
            fprintf(stderr, "[SURFUP] %08x %ux%u%s force=%d dirtyDraw=%d vramNZ=%u/%zu cleared=%d\n",
                    (unsigned)surface->vram_addr, surface->width, surface->height,
                    surface->swizzle ? "s" : "", force, surface->draw_dirty, nz, sz / 256,
                    surface->cleared);
        }
    }
    nv2a_site_hit(force ? "upload(force)" : "upload", __builtin_return_address(0), surface->width, surface->height);
    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        surface->initialized = true;
        return;
    }

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    g_autofree uint8_t *swizzle_buf = NULL;
    uint8_t *gl_read_buf = NULL;

    if (surface->swizzle) {
        swizzle_buf = (uint8_t*)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       swizzle_buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    } else {
        gl_read_buf = buf;
    }

    //
    // Upload image data from host to staging buffer
    //

    StorageBuffer *copy_buffer = &r->storage_buffers[BUFFER_STAGING_SRC];
    size_t uploaded_image_size = surface->height * surface->width *
                                 surface->fmt.bytes_per_pixel;
    assert(uploaded_image_size <= copy_buffer->buffer_size);

    void *mapped_memory_ptr = NULL;
    pgraph_vk_wait_deferred();
    VK_CHECK(vmaMapMemory(r->allocator, copy_buffer->allocation,
                          &mapped_memory_ptr));

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool no_conversion_necessary =
        surface->color || surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM ||
        use_compute_to_convert_depth_stencil_format;
    assert(no_conversion_necessary);

    memcpy_image(mapped_memory_ptr, gl_read_buf,
                 surface->width * surface->fmt.bytes_per_pixel, surface->pitch,
                 surface->height);

    vmaFlushAllocation(r->allocator, copy_buffer->allocation, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(r->allocator, copy_buffer->allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    // Set up image copy regions (which may be modified by compute unpack)

    VkBufferImageCopy regions[2];
    int num_regions = 0;

    regions[num_regions++] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
        .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        regions[num_regions++] = (VkBufferImageCopy){
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
        };
    }


    unsigned int scaled_width = surface->width, scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    if (use_compute_to_convert_depth_stencil_format) {

        //
        // Copy packed image buffer to compute_dst for unpacking
        //

        size_t packed_size = uploaded_image_size;
        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, copy_buffer->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer, 1,
                        &buffer_copy_region);

        size_t num_pixels = scaled_width * scaled_height;
        size_t unpacked_depth_image_size = num_pixels * 4;
        size_t unpacked_stencil_image_size = num_pixels;
        size_t unpacked_size =
            unpacked_depth_image_size + unpacked_stencil_image_size;

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);

        //
        // Unpack depth-stencil image into compute_src
        //

        VkBufferMemoryBarrier pre_unpack_src_barrier = {
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
                             1, &pre_unpack_src_barrier, 0, NULL);

        StorageBuffer *unpack_buffer = &r->storage_buffers[BUFFER_COMPUTE_SRC];

        VkBufferMemoryBarrier pre_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1,
                             &pre_unpack_dst_barrier, 0, NULL);

        pgraph_vk_unpack_depth_stencil(
            pg, surface, cmd, r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            unpack_buffer->buffer);

        VkBufferMemoryBarrier post_unpack_src_barrier = {
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
                             &post_unpack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unpack_dst_barrier, 0, NULL);

        // Already scaled during compute. Adjust copy regions.
        regions[0].imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 };
        regions[1].imageExtent = regions[0].imageExtent;
        regions[1].bufferOffset =
            ROUND_UP(unpacked_depth_image_size,
                     r->device_props.limits.minStorageBufferOffsetAlignment);

        copy_buffer = unpack_buffer;
    }

    //
    // Copy image data from buffer to staging image
    //

    if (surface->image_scratch_current_layout !=
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    vkCmdCopyBufferToImage(cmd, copy_buffer->buffer, surface->image_scratch,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_regions,
                           regions);

    VkBufferMemoryBarrier post_copy_src_buffer_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_src_buffer_barrier, 0, NULL);

    //
    // Copy staging image to final image
    //

    pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                      surface->host_fmt.vk_format,
                                      surface->image_scratch_current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    surface->image_scratch_current_layout =
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    bool upscale = pg->surface_scale_factor > 1 &&
                   !use_compute_to_convert_depth_stencil_format;

    if (upscale) {
        VkImageBlit blitRegion = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},
        };

        vkCmdBlitImage(cmd, surface->image_scratch,
                       surface->image_scratch_current_layout, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion,
                       VK_FILTER_NEAREST);
    } else {
        // Note: We should be able to vkCmdCopyBufferToImage directly into
        // surface->image, but there is an apparent AMD Windows driver
        // synchronization bug we'll hit when doing this. For this reason,
        // always use a staging image.

        for (int i = 0; i < num_regions; i++) {
            VkImageAspectFlags aspect = regions[i].imageSubresource.aspectMask;
            VkImageCopy copy_region = {
                .srcSubresource.aspectMask = aspect,
                .srcSubresource.layerCount = 1,
                .dstSubresource.aspectMask = aspect,
                .dstSubresource.layerCount = 1,
                .extent = regions[i].imageExtent,
            };
            vkCmdCopyImage(cmd, surface->image_scratch,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy_region);
        }
    }

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_2);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    surface->initialized = true;
}

static void compare_surfaces(SurfaceBinding const *a, SurfaceBinding const *b)
{
    #define DO_CMP(fld) \
        if (a->fld != b->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)a->fld, (long int)b->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(host_fmt.vk_format)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_target_sized(NV2AState *d, bool color,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_vk_map));
        fmt = kelvin_surface_color_format_map[pg->surface_shape.color_format];
        host_fmt = kelvin_surface_color_format_vk_map[pg->surface_shape.color_format];
        if (host_fmt.host_bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            assert(!"unimplemented color surface format");
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(r->kelvin_surface_zeta_vk_map));
        fmt = kelvin_surface_zeta_format_map[pg->surface_shape.zeta_format];
        host_fmt = r->kelvin_surface_zeta_vk_map[pg->surface_shape.zeta_format];
        // FIXME: Support float 16,24b float format surface
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    // There's a bunch of bugs that could cause us to hit this function
    // at the wrong time and get a invalid dma object.
    // Check that it's sane.
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    // assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    target->fmt = fmt;
    target->host_fmt = host_fmt;
    target->color = color;
    target->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    target->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    target->upload_pending = true;
    target->download_pending = false;
    target->draw_dirty = false;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->cleared = false;

    target->initialized = false;
}

static void populate_surface_binding_target(NV2AState *d, bool color,
                                            SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        // Since we determine surface dimensions based on the clipping
        // rectangle, make sure to include the surface offset as well.
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_target_sized(d, color, width, height, target);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    SurfaceBinding target;
    memset(&target, 0, sizeof(target));
    populate_surface_binding_target(d, color, &target);

    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    bool mem_dirty = !tcg_enabled() && memory_region_test_and_clear_dirty(
                                           d->vram, target.vram_addr,
                                           target.size, DIRTY_MEMORY_NV2A);

    SurfaceBinding *current_binding = color ? r->color_binding
                                            : r->zeta_binding;

    if (!current_binding ||
        (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        // FIXME: We don't need to be so aggressive flushing the command list
        // pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE);
        if (r->in_render_pass)
            nv2a_site_hit(!current_binding ? "split:surf-new" : pg_surface->buffer_dirty ? "split:surf-bufdirty" : "split:surf-memdirty",
                          NULL, target.width, target.height);
        pgraph_vk_ensure_not_in_render_pass(pg);

        unbind_surface(d, color);

        SurfaceBinding *surface = pgraph_vk_surface_get(d, target.vram_addr);
        if (surface != NULL) {
            // FIXME: Support same color/zeta surface target? In the mean time,
            // if the surface we just found is currently bound, just unbind it.
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
            if (surface == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", target.vram_addr,
            target.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;

        if (surface != NULL) {
            bool is_compatible =
                check_surface_compatibility(surface, &target, false);

            void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                             const char *layout, uint32_t anti_aliasing,
                             uint32_t clip_x, uint32_t clip_width,
                             uint32_t clip_y, uint32_t clip_height,
                             uint32_t pitch) =
                surface->color ? trace_nv2a_pgraph_surface_match_color :
                               trace_nv2a_pgraph_surface_match_zeta;

            trace_fn(surface->vram_addr, surface->width, surface->height,
                     surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                     surface->shape.clip_x, surface->shape.clip_width,
                     surface->shape.clip_y, surface->shape.clip_height,
                     surface->pitch);

            assert(!(target.swizzle && pg->clearing));

#if 0
            if (surface->swizzle != target.swizzle) {
                // Clears should only be done on linear surfaces. Avoid
                // synchronization by allowing (1) a surface marked swizzled to
                // be cleared under the assumption the entire surface is
                // destined to be cleared and (2) a fully cleared linear surface
                // to be marked swizzled. Strictly match size to avoid
                // pathological cases.
                is_compatible &= (pg->clearing || surface->cleared) &&
                    check_surface_compatibility(surface, &target, true);
                if (is_compatible) {
                    trace_nv2a_pgraph_surface_migrate_type(
                        target.swizzle ? "swizzled" : "linear");
                }
            }
#endif

            if (is_compatible && color && surface->scale_stale) {
                is_compatible = false;   /* recreate at its new scale */
            }
            if (is_compatible && color &&
                !check_surface_compatibility(surface, &target, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_target_sized(
                    d, !color, surface->width, surface->height, &zeta_entry);
                hwaddr color_end = surface->vram_addr + surface->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= surface->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            /* nv2a-port: a small render pass (a 32x32 tile) that points its
             * depth at the main depth buffer's address evicted the 640x480
             * depth surface, downloading and re-uploading it, twice per tile
             * and a dozen tiles a frame. Vulkan allows a depth attachment
             * larger than the framebuffer, so render the tile with the big
             * depth image instead; only its top-left corner is touched.
             * NV2A_NO_ZREUSE=1 restores the eviction. */
            static int zreuse = -1;
            if (zreuse < 0) zreuse = !getenv("NV2A_NO_ZREUSE");
            if (zreuse && !color && !surface->color && r->color_binding &&
                r->color_binding->scale && surface->scale &&
                r->color_binding->scale != surface->scale) {
                /* the tile is native size, the depth buffer scaled: make
                 * the tile scaled from now on (recreated at its next use) */
                force_full_scale(r->color_binding->vram_addr);
                r->color_binding->scale_stale = true;
            } else
            if (zreuse && !color && !surface->color && r->color_binding &&
                surface->host_fmt.vk_format == target.host_fmt.vk_format &&
                target.width == r->color_binding->width &&
                target.height == r->color_binding->height &&
                surface->width > target.width &&
                surface->height > target.height &&
                (long)target.width * target.height <= 256L * 256L &&
                surface->vram_addr == target.vram_addr) {
                nv2a_site_hit("zreuse", 0, target.width, target.height);
                pg->surface_zeta.buffer_dirty = false;
                should_create = false;
                goto bind_found;
            }

            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (surface->width == r->color_binding->width) &&
                                 (surface->height == r->color_binding->height);
                if (surface->scale && r->color_binding->scale &&
                    surface->scale != r->color_binding->scale) {
                    nv2a_site_hit("zscale-mismatch", 0, surface->width, surface->height);
                    is_compatible = false;
                }
            }

            if (is_compatible) {
                // FIXME: Refactor
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.clip_x = surface->shape.clip_x;
                pg->surface_binding_dim.clip_width = surface->shape.clip_width;
                pg->surface_binding_dim.height = surface->height;
                pg->surface_binding_dim.clip_y = surface->shape.clip_y;
                pg->surface_binding_dim.clip_height = surface->shape.clip_height;
                if (mem_dirty && surface->ram_hash_valid && s_upload_skip()) {
                    if (surf_ram_hash(d, surface) == surface->ram_hash) {
                        nv2a_site_hit("upload:skip-same", 0, surface->width, surface->height);
                        mem_dirty = false;
                    } else {
                        nv2a_site_hit("upload:ram-changed", 0, surface->width, surface->height);
                    }
                }
                surface->upload_pending |= mem_dirty;
                if (mem_dirty) {
                    nv2a_site_hit("memdirty", 0, surface->width, surface->height);
                    /* who wrote it: the first page with a record */
                    extern uintptr_t nv2a_page_dirty_by(uint64_t addr);
                    uintptr_t by = 0;
                    for (hwaddr a = surface->vram_addr; a < surface->vram_addr + surface->size && !by; a += 4096)
                        by = nv2a_page_dirty_by(a);
                    nv2a_site_hit("memdirty-by", (void *)by, surface->width, surface->height);
                    {   static int nlog;
                        if (getenv("NV2A_ALIAS_LOG") && nlog < 300) {
                            nlog++;
                            fprintf(stderr, "[MEMDIRTY] %08lx %ux%u pitch %u %s c%d dd%d dlp%d bound-as %s draw_time %u\n",
                                    (long)surface->vram_addr, surface->width, surface->height,
                                    surface->pitch, surface->swizzle ? "sz" : "ln", surface->color,
                                    surface->draw_dirty, surface->download_pending,
                                    color ? "color" : "zeta", surface->draw_time);
                        }
                    }
                }
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", surface->vram_addr);
                compare_surfaces(surface, &target);
                nv2a_site_hit("evict:incompat", 0, surface->width, surface->height);
                pgraph_vk_surface_download_if_dirty(d, surface);
                invalidate_surface(d, surface);
            }
        }

bind_found:
        if (should_create) {
            nv2a_site_hit(color ? "create:color" : "create:zeta", 0, target.width, target.height);
            surface = get_any_compatible_invalid_surface(r, &target);
            if (surface) {
                migrate_surface_image(&target, surface);
            } else {
                surface = g_malloc(sizeof(SurfaceBinding));
                create_surface_image(pg, &target);
            }

            *surface = target;
            set_surface_label(pg, surface);
            surface_put(d, surface);

            // FIXME: Refactor
            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.clip_x = target.shape.clip_x;
            pg->surface_binding_dim.clip_width = target.shape.clip_width;
            pg->surface_binding_dim.height = target.height;
            pg->surface_binding_dim.clip_y = target.shape.clip_y;
            pg->surface_binding_dim.clip_height = target.shape.clip_height;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                         const char *layout, uint32_t anti_aliasing,
                         uint32_t clip_x, uint32_t clip_width, uint32_t clip_y,
                         uint32_t clip_height, uint32_t pitch) =
            color ? (should_create ? trace_nv2a_pgraph_surface_create_color :
                                     trace_nv2a_pgraph_surface_hit_color) :
                    (should_create ? trace_nv2a_pgraph_surface_create_zeta :
                                     trace_nv2a_pgraph_surface_hit_zeta);
        trace_fn(surface->vram_addr, surface->width, surface->height,
                 surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                 surface->shape.clip_x, surface->shape.clip_width,
                 surface->shape.clip_y, surface->shape.clip_height, surface->pitch);

        bind_surface(r, surface);
        pg_surface->buffer_dirty = false;
    }

    if (!upload && pg_surface->draw_dirty) {
        /* nv2a-port: lazy, as xemu does under TCG. A synchronous GPU->RAM
         * readback on every render-target switch stalled each frame and is
         * where Mali kept cancelling jobs. The surface stays the copy of
         * record; textures, blits and eviction download it when needed. */
        SurfaceBinding *sb = color ? r->color_binding : r->zeta_binding;
        if (sb) {
            /* The CPU reads guest RAM directly here (no QEMU access
             * callback), so a render target the title reads back -- a
             * luminance/exposure reduction, say -- must be in RAM when the
             * title switches away from it. Small targets are cheap: download
             * them now. NV2A_EAGER_DL_MAX = max pixels (default 256x256),
             * NV2A_EAGER_DL=1 downloads every target. */
            static long eager_max = -1;
            if (eager_max < 0) {
                const char *e = getenv("NV2A_EAGER_DL_MAX");
                eager_max = e ? atol(e) : 256L * 256L;
                if (getenv("NV2A_EAGER_DL")) eager_max = 1L << 30;
                if (getenv("NV2A_NO_EAGER")) eager_max = 0;
            }
            static int at_flip = -1;
            if (at_flip < 0) at_flip = !getenv("NV2A_EAGER_NOW");
            if ((long)sb->width * (long)sb->height <= eager_max && at_flip) {
                /* Batched: read back once per frame, at the flip, instead of
                 * draining the GPU at every render-target switch. */
                sb->eager_pending = true;
                sb->download_pending = true;
            } else if ((long)sb->width * (long)sb->height <= eager_max) {
                download_surface(d, sb, true);
                /* Log what small read-back targets hold (first sightings
                 * of each address, then every 600th). */
                static uint32_t seen_addr[64]; static unsigned seen_n, dl_n;
                unsigned k; bool known = false;
                for (k = 0; k < seen_n; k++) if (seen_addr[k] == sb->vram_addr) known = true;
                if ((!known && seen_n < 64) || (++dl_n % 600) == 0) {
                    if (!known && seen_n < 64) seen_addr[seen_n++] = sb->vram_addr;
                    const uint8_t *px = d->vram_ptr + sb->vram_addr;
                    unsigned bpp = sb->fmt.bytes_per_pixel, n = sb->width * sb->height;
                    uint64_t sum[4] = {0};
                    for (unsigned y = 0; y < sb->height; y++)
                        for (unsigned x = 0; x < sb->width; x++)
                            for (unsigned c = 0; c < bpp && c < 4; c++)
                                sum[c] += px[y * sb->pitch + x * bpp + c];
                    fprintf(stderr, "[EAGER] %s 0x%08X %ux%u bpp %u sw %d avg %u %u %u %u\n",
                            color ? "color" : "zeta", sb->vram_addr, sb->width, sb->height,
                            bpp, sb->swizzle, (unsigned)(sum[0] / n), (unsigned)(sum[1] / n),
                            (unsigned)(sum[2] / n), (unsigned)(sum[3] / n));
                }
            } else {
                sb->download_pending = true;
            }
        }

        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
    }
}

// FIXME: Move to common?
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_vk_upload_surface_data(d, r->color_binding, false);
            r->color_binding->draw_time = pg->draw_time;
            r->color_binding->swizzle = swizzle;
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_vk_upload_surface_data(d, r->zeta_binding, false);
            r->zeta_binding->draw_time = pg->draw_time;
            r->zeta_binding->swizzle = swizzle;
        }
    }

    if (r->color_binding && r->zeta_binding &&
        r->color_binding->scale != r->zeta_binding->scale) {
        static int warned;
        if (warned++ < 20)
            fprintf(stderr, "[SCALE] colour %ux%u x%u with depth %ux%u x%u\n",
                    r->color_binding->width, r->color_binding->height,
                    r->color_binding->scale, r->zeta_binding->width,
                    r->zeta_binding->height, r->zeta_binding->scale);
    }
    // Sanity check color and zeta dimensions match
    if (r->color_binding && r->zeta_binding) {
        assert(r->color_binding->width == r->zeta_binding->width);
        assert(r->color_binding->height == r->zeta_binding->height);
    }

    expire_old_surfaces(d);
    prune_invalid_surfaces(r, num_invalid_surfaces_to_keep);
}

bool pgraph_vk_is_image_format_supported(VkPhysicalDevice device,
                                         VkFormat format,
                                         VkImageUsageFlags usage)
{
    VkPhysicalDeviceImageFormatInfo2 pdif2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    VkResult result =
        vkGetPhysicalDeviceImageFormatProperties2(device, &pdif2, &props);
    return result == VK_SUCCESS;
}

static bool check_surface_internal_formats_supported(
    PGRAPHVkState *r, const SurfaceFormatInfo *fmts, size_t count)
{
    bool all_supported = true;
    for (int i = 0; i < count; i++) {
        const SurfaceFormatInfo *f = &fmts[i];
        if (f->host_bytes_per_pixel) {
            all_supported &= pgraph_vk_is_image_format_supported(
                r->physical_device, f->vk_format, f->usage);
        }
    }
    return all_supported;
}

void pgraph_vk_init_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Check if the device supports preferred VK_FORMAT_D24_UNORM_S8_UINT
    // format, fall back to D32_SFLOAT_S8_UINT otherwise.
    r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z16] = zeta_d16;
    if (check_surface_internal_formats_supported(r, &zeta_d24_unorm_s8_uint,
                                                 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d24_unorm_s8_uint;
    } else if (check_surface_internal_formats_supported(
                   r, &zeta_d32_sfloat_s8_uint, 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d32_sfloat_s8_uint;
    } else {
        assert(!"No suitable depth-stencil format supported");
    }

    QTAILQ_INIT(&r->surfaces);
    QTAILQ_INIT(&r->invalid_surfaces);

    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->framebuffer_dirty = true;

    pgraph_vk_reload_surface_scale_factor(pg); // FIXME: Move internal
}

void pgraph_vk_finalize_surfaces(PGRAPHState *pg)
{
    pgraph_vk_surface_flush(container_of(pg, NV2AState, pgraph));
}

void pgraph_vk_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Clear last surface shape to force recreation of buffers at next draw
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    unbind_surface(d, true);
    unbind_surface(d, false);

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        pgraph_vk_surface_download_if_dirty(d, s);
        invalidate_surface(d, s);
    }
    prune_invalid_surfaces(r, 0);

    pgraph_vk_reload_surface_scale_factor(pg);
}

/* nv2a-port: the title signals "frame done" with a semaphore release that
 * the emulator writes as soon as it parses the method -- before the GPU has
 * drawn the frame, let alone before the small render targets it reads back
 * (Conker decides which characters are visible from one) are in guest RAM.
 * Reading the stale copy, it hid characters (Berri in the phone scene,
 * everyone in the menu bar). So before the release, finish the frame and
 * download those targets. NV2A_SEM_NO_SYNC_DL=1 turns this off. */
void pgraph_vk_sync_eager_before_semaphore(NV2AState *d)
{
    static int off = -1;
    if (off < 0) off = getenv("NV2A_SEM_NO_SYNC_DL") != NULL;
    if (off) return;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r) return;
    SurfaceBinding *surface;
    bool any = false;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->eager_pending && surface->download_pending && surface->color) {
            any = true;
            break;
        }
    }
    if (!any) return;
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        if (surface->eager_pending && surface->download_pending && surface->color) {
            surface->eager_pending = false;
            download_surface(d, surface, true);
        }
    }
}

/* nv2a-port: the slot's read-backs become visible to the host once, at its
 * submit, instead of a pipeline barrier after every batch of copies. */
void pgraph_vk_rb_host_barrier(PGRAPHState *pg, int slot)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!s_rb_n[slot]) return;
    StorageBuffer *dst = &r->storage_buffers[BUFFER_STAGING_DST];
    VkDeviceSize half = (dst->buffer_size / 2) & ~(VkDeviceSize)0xFFF;
    VkDeviceSize base = slot ? half : 0;
    VkBufferMemoryBarrier post = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst->buffer,
        .offset = base,
        .size = s_rb_end[slot] - base,
    };
    vkCmdPipelineBarrier(r->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &post, 0, NULL);
}
