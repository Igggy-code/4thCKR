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

#include "renderer.h"

VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    return pgraph_vk_append_to_buffer(pg, BUFFER_INDEX_STAGING, &data, &size, 1,
                                      1);
}

VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count)
{
    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
    return pgraph_vk_append_to_buffer(pg, BUFFER_VERTEX_INLINE_STAGING, data,
                                      sizes, count, 1);
}

/* nv2a-port: vertex memory the title rewrote while the command buffer
 * being built still reads its old contents. xemu drains the GPU there
 * (VERTEX_BUFFER_DIRTY); with Conker's per-frame dynamic geometry that was
 * a dozen full GPU stalls a frame on Mali. Instead the RAM buffer keeps the
 * old bytes for the draws already recorded, and later draws that read the
 * range take a fresh copy through the streamed vertex buffer (see
 * remap_unaligned_attributes). The RAM buffer catches up after the next
 * submit has completed. NV2A_VTX_FINISH=1 restores the drain. */
#define NV2A_VTX_SHADOW_MAX 128
static struct { hwaddr addr, size; int slot; } s_vtx_shadow[NV2A_VTX_SHADOW_MAX];
static int s_vtx_shadow_n;
/* nv2a-port: host writes into the vertex RAM buffer since the last submit;
 * flushed once per submit (flush_memory_buffer) instead of per write, and
 * instead of xemu's flush of the whole buffer -- on Mali's non-coherent
 * memory that was a cache clean of every byte of guest RAM each submit. */
static hwaddr s_vram_lo = (hwaddr)-1, s_vram_hi;
/* nv2a-port: the written ranges themselves, not only their union. The union
 * of a frame's scattered writes could span tens of MB, and the flush cleans
 * the CPU cache over all of it every submit. Up to 64 ranges (merged when
 * within 64 KB); past that, the union. NV2A_VRAM_FLUSH_UNION=1: old way. */
enum { VR_N = 64 };
static hwaddr s_vr_a[VR_N], s_vr_b[VR_N];
static int s_vr_n, s_vr_over;
static void vram_dirty(hwaddr a, hwaddr n)
{
    if (a < s_vram_lo) s_vram_lo = a;
    if (a + n > s_vram_hi) s_vram_hi = a + n;
    if (s_vr_over) return;
    hwaddr b = a + n;
    for (int i = 0; i < s_vr_n; i++) {
        if (a <= s_vr_b[i] + 65536 && s_vr_a[i] <= b + 65536) {
            if (a < s_vr_a[i]) s_vr_a[i] = a;
            if (b > s_vr_b[i]) s_vr_b[i] = b;
            return;
        }
    }
    if (s_vr_n == VR_N) { s_vr_over = 1; return; }
    s_vr_a[s_vr_n] = a; s_vr_b[s_vr_n] = b; s_vr_n++;
}
void nv2a_vram_flush_dirty(PGRAPHVkState *r)
{
    static int un = -1;
    if (un < 0) un = getenv("NV2A_VRAM_FLUSH_UNION") != NULL;
    VmaAllocation al = r->storage_buffers[BUFFER_VERTEX_RAM].allocation;
    if (s_vram_hi > s_vram_lo) {
        if (un || s_vr_over) {
            vmaFlushAllocation(r->allocator, al, s_vram_lo, s_vram_hi - s_vram_lo);
        } else {
            for (int i = 0; i < s_vr_n; i++)
                vmaFlushAllocation(r->allocator, al, s_vr_a[i], s_vr_b[i] - s_vr_a[i]);
        }
    }
    s_vram_lo = (hwaddr)-1; s_vram_hi = 0;
    s_vr_n = 0; s_vr_over = 0;
}

bool nv2a_vtx_shadowed(hwaddr addr, hwaddr size)
{
    for (int i = 0; i < s_vtx_shadow_n; i++)
        if (addr < s_vtx_shadow[i].addr + s_vtx_shadow[i].size &&
            s_vtx_shadow[i].addr < addr + size)
            return true;
    return false;
}

/* nv2a-port: entries belong to the frame slot recording when they were
 * made (-1 until its submit); a slot's retire brings its ranges up to date,
 * unless the other frame in flight still reads them -- then that one's
 * retire does. */
void nv2a_vtx_shadow_seal(int slot)
{
    for (int i = 0; i < s_vtx_shadow_n; i++)
        if (s_vtx_shadow[i].slot < 0) s_vtx_shadow[i].slot = slot;
}

static bool slot_reads(PGRAPHVkState *r, int k, hwaddr a, hwaddr n)
{
    size_t lo = a / 4096, hi = (a + n + 4095) / 4096;
    if (hi > r->bitmap_size) hi = r->bitmap_size;
    return r->slot_pending[k] && lo < hi &&
           find_next_bit(r->slot_used_bitmap[k], hi, lo) < hi;
}

void nv2a_vtx_shadow_flush_slot(NV2AState *d, int slot)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    int j = 0;
    for (int i = 0; i < s_vtx_shadow_n; i++) {
        hwaddr a = s_vtx_shadow[i].addr, n = s_vtx_shadow[i].size;
        int es = s_vtx_shadow[i].slot;
        if (es == slot || (slot < 0 && es >= 0)) {
            int other = (es >= 0) ? (es ^ 1) : -1;
            if (other >= 0 && slot_reads(r, other, a, n)) {
                s_vtx_shadow[i].slot = other;    /* later, with that frame */
                s_vtx_shadow[j++] = s_vtx_shadow[i];
                continue;
            }
            memcpy(r->storage_buffers[BUFFER_VERTEX_RAM].mapped + a, d->vram_ptr + a, n);
            vram_dirty(a, n);
            continue;
        }
        s_vtx_shadow[j++] = s_vtx_shadow[i];
    }
    s_vtx_shadow_n = j;
}

void nv2a_vtx_shadow_flush(NV2AState *d)
{
    nv2a_vtx_shadow_flush_slot(d, -1);
}

void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset,
                                        void *data, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    {   extern void pgraph_vk_download_surfaces_for_vertices(PGRAPHState *, hwaddr, hwaddr);
        pgraph_vk_download_surfaces_for_vertices(pg, offset, size); }

    size_t start_bit = offset / TARGET_PAGE_SIZE;
    size_t end_bit = TARGET_PAGE_ALIGN(offset + size) / TARGET_PAGE_SIZE;
    size_t nbits = end_bit - start_bit;

    static int vtx_finish = -1;
    if (vtx_finish < 0) vtx_finish = getenv("NV2A_VTX_FINISH") != NULL;
    bool conflict_cur =
        find_next_bit(r->uploaded_bitmap, start_bit + nbits, start_bit) < end_bit;
    /* a frame still on the GPU reads these pages: don't touch them under it */
    bool conflict_gpu = slot_reads(r, 0, offset, size) || slot_reads(r, 1, offset, size);
    if (!vtx_finish && (conflict_cur || conflict_gpu)) {
        int k;
        for (k = 0; k < s_vtx_shadow_n; k++)
            if (s_vtx_shadow[k].addr == offset && s_vtx_shadow[k].size == size &&
                s_vtx_shadow[k].slot < 0) break;
        if (k < s_vtx_shadow_n) return;
        if (s_vtx_shadow_n < NV2A_VTX_SHADOW_MAX) {
            s_vtx_shadow[s_vtx_shadow_n].addr = offset;
            s_vtx_shadow[s_vtx_shadow_n].size = size;
            s_vtx_shadow[s_vtx_shadow_n].slot = -1;
            s_vtx_shadow_n++;
            if (conflict_gpu && !conflict_cur) nv2a_site_hit("vtx:shadow-gpu", 0, 0, 0);
            return;
        }
    }

    if (find_next_bit(r->uploaded_bitmap, start_bit + nbits, start_bit) <
        end_bit) {
        // Vertex data changed while building the draw list. Finish drawing
        // before updating RAM buffer.
        pgraph_vk_finish(pg, VK_FINISH_REASON_VERTEX_BUFFER_DIRTY);
    }

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    pgraph_vk_slot_wait_used(r, offset, size);   /* the GPU may still read these */
    memcpy(r->storage_buffers[BUFFER_VERTEX_RAM].mapped + offset, data, size);
    vram_dirty(offset, size);   /* flushed at submit */

    bitmap_set(r->uploaded_bitmap, start_bit, nbits);
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->num_vertex_ram_buffer_syncs <
           ARRAY_SIZE(r->vertex_ram_buffer_syncs));
    r->vertex_ram_buffer_syncs[r->num_vertex_ram_buffer_syncs++] =
        (MemorySyncRequirement){ .addr = addr, .size = size };
}

static const VkFormat float_to_count[] = {
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R32G32B32_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,
};

static const VkFormat ub_to_count[] = {
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8_UNORM,
    VK_FORMAT_R8G8B8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
};

static const VkFormat s1_to_count[] = {
    VK_FORMAT_R16_SNORM,
    VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16B16_SNORM,
    VK_FORMAT_R16G16B16A16_SNORM,
};

static const VkFormat s32k_to_count[] = {
    VK_FORMAT_R16_SSCALED,
    VK_FORMAT_R16G16_SSCALED,
    VK_FORMAT_R16G16B16_SSCALED,
    VK_FORMAT_R16G16B16A16_SSCALED,
};

static char const * const vertex_data_array_format_to_str[] = {
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D] = "UB_D3D",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL] = "UB_OGL",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1] = "S1",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F] = "F",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K] = "S32K",
    [NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP] = "CMP",
};

/* nv2a-port: is this format usable as a vertex attribute here? Cached. */
static bool nv2a_vk_vertex_format_ok(PGRAPHVkState *r, VkFormat f)
{
    static uint8_t cache[256];   /* 0 unknown, 1 ok, 2 not */
    if ((unsigned)f < 256 && cache[f]) return cache[f] == 1;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, f, &p);
    bool ok = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    if (!ok) fprintf(stderr, "[vk] vertex format %d unsupported, widening\n", (int)f);
    if ((unsigned)f < 256) cache[f] = ok ? 1 : 2;
    return ok;
}

void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int num_elements = max_element - min_element + 1;

    if (inline_data) {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_VK_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;
    pg->w1_attrs = 0;
    pg->int_attrs = 0;
    pg->snorm_attrs = 0;
    pg->int_attrs = 0;
    pg->snorm_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        NV2A_VK_DGROUP_BEGIN("[attr %02d] format=%s, count=%d, stride=%d", i,
                             vertex_data_array_format_to_str[attr->format],
                             attr->count, attr->stride);
        r->vertex_attribute_to_description_location[i] = -1;
        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        VkFormat vk_format;
        bool needs_conversion = false;
        bool d3d_swizzle = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            assert(attr->count == 4);
            d3d_swizzle = true;
            /* fallthru */
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            assert(attr->count <= ARRAY_SIZE(ub_to_count));
            vk_format = ub_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            assert(attr->count <= ARRAY_SIZE(s1_to_count));
            vk_format = s1_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            assert(attr->count <= ARRAY_SIZE(float_to_count));
            vk_format = float_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            assert(attr->count <= ARRAY_SIZE(s32k_to_count));
            vk_format = s32k_to_count[attr->count - 1];
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            vk_format =
                VK_FORMAT_R32_SINT; // VK_FORMAT_B10G11R11_UFLOAT_PACK32 ??
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        /* nv2a-port: Mali has no 3-component 8/16-bit vertex formats. Read
         * the 4-component variant (the extra component is the next bytes of
         * the stream) and force w = 1 in the shader. */
        /* Also on every Mali: its 3-component 16/8-bit fetch takes the 4th
         * component from the next vertex instead of supplying w = 1, so
         * S32K positions (stride 6) came out with a random w -- the
         * stretched world geometry. NV2A_NO_W1=1 turns this off. */
        static int mali_w1 = -1;
        if (mali_w1 < 0)
            mali_w1 = NV2A_MOBILE_PATH(r) && !getenv("NV2A_NO_W1");
        {   /* NV2A_AB_W1: A/B test, w1 on for 5 s, off for 5 s */
            static int ab = -1;
            if (ab < 0) ab = getenv("NV2A_AB_W1") != NULL;
            if (ab) {
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                mali_w1 = (ts.tv_sec / 5) & 1;
                static int last = -1;
                if (last != mali_w1) { last = mali_w1; fprintf(stderr, "[AB] w1=%d\n", mali_w1); }
            }
        }
        if (attr->count == 3 && !needs_conversion &&
            ((mali_w1 && attr->format != NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) ||
             !nv2a_vk_vertex_format_ok(r, vk_format))) {
            VkFormat wide = vk_format;
            switch (attr->format) {
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL: wide = ub_to_count[3]; break;
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:     wide = s1_to_count[3]; break;
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:   wide = s32k_to_count[3]; break;
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:      wide = float_to_count[3]; break;
            }
            if (wide != vk_format && nv2a_vk_vertex_format_ok(r, wide)) {
                vk_format = wide;
                pg->w1_attrs |= 1 << i;
            } else if (wide != vk_format) {
                /* the 4-wide variant may itself be unsupported (SSCALED):
                 * keep the width and let the integer fallback below read it */
                vk_format = wide;
                pg->w1_attrs |= 1 << i;
            }
        }

        /* nv2a-port: 16-bit SSCALED / SNORM vertex formats may be missing as
         * vertex input on Mali. Read them as SINT and convert in the shader. */
        if (!needs_conversion && !nv2a_vk_vertex_format_ok(r, vk_format)) {
            static const VkFormat sint16[] = { VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT,
                                               VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT };
            int n = (pg->w1_attrs & (1 << i)) ? 4 : attr->count;
            if (n >= 1 && n <= 4 &&
                (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K ||
                 attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1) &&
                nv2a_vk_vertex_format_ok(r, sint16[n - 1])) {
                vk_format = sint16[n - 1];
                if (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K)
                    pg->int_attrs |= 1 << i;
                else
                    pg->snorm_attrs |= 1 << i;
            }
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;

        hwaddr start = 0;
        if (inline_data) {
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            start = attrib_data_addr + min_element * stride;
            update_memory_buffer(d, start, num_elements * stride);
        }

        uint32_t provoking_element_index = provoking_element - min_element;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry =
                (uint8_t *)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pg->uniform_attrs |= 1 << i;
            pgraph_update_inline_value(attr, last_entry);
            NV2A_VK_DPRINTF("inline_value = {%f, %f, %f, %f}",
                            attr->inline_value[0], attr->inline_value[1],
                            attr->inline_value[2], attr->inline_value[3]);
            NV2A_VK_DGROUP_END();
            continue;
        }

        NV2A_VK_DPRINTF("offset = %08" HWADDR_PRIx, attrib_data_addr);
        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);

        r->vertex_attribute_to_description_location[i] =
            r->num_active_vertex_binding_descriptions;

        r->vertex_binding_descriptions
            [r->num_active_vertex_binding_descriptions++] =
            (VkVertexInputBindingDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .stride = stride,
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };

        r->vertex_attribute_descriptions
            [r->num_active_vertex_attribute_descriptions++] =
            (VkVertexInputAttributeDescription){
                .binding = r->vertex_attribute_to_description_location[i],
                .location = i,
                .format = vk_format,
            };

        r->vertex_attribute_offsets[i] = attrib_data_addr;
        {   /* nv2a-port probe: is this vertex data in RAM the title named
             * (heap memory), which the GPU's window view does not hold? */
            extern int xbox_PhysRamNamedAt(uint32_t pa) __attribute__((weak));
            static unsigned long n_named, n_all;
            n_all++;
            if (xbox_PhysRamNamedAt && xbox_PhysRamNamedAt((uint32_t)attrib_data_addr)) {
                if (n_named++ < 10)
                    fprintf(stderr, "[vk] vertex attr %d at 0x%08llx is heap RAM (not the GPU window)\n",
                            i, (unsigned long long)attrib_data_addr);
            }
            if ((n_all & 0xFFFFF) == 0)
                fprintf(stderr, "[vk] vertex attrs: %lu in heap RAM of %lu\n", n_named, n_all);
        }
        {   /* nv2a-port: Vulkan wants vertex data aligned to the component
             * size; log what the title hands us that is not. */
            static int logged;
            unsigned comp = attr->size ? attr->size : 1;
            if (((attrib_data_addr % comp) || (stride % comp)) && logged < 20) {
                logged++;
                fprintf(stderr, "[vk] unaligned vertex attr %d: fmt %d count %d size %d "
                        "addr 0x%llx stride %zu\n", i, attr->format, attr->count,
                        attr->size, (unsigned long long)attrib_data_addr, stride);
            }
        }

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }
        if (d3d_swizzle) {
            pg->swizzle_attrs |= (1 << i);
        }

        NV2A_VK_DGROUP_END();
    }

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;
    pg->w1_attrs = 0;
    pg->int_attrs = 0;
    pg->snorm_attrs = 0;

    r->num_active_vertex_attribute_descriptions = 0;
    r->num_active_vertex_binding_descriptions = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->inline_buffer_populated) {
            r->vertex_attribute_to_description_location[i] =
                r->num_active_vertex_binding_descriptions;
            r->vertex_binding_descriptions
                [r->num_active_vertex_binding_descriptions++] =
                (VkVertexInputBindingDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .stride = 4 * sizeof(float),
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                };
            r->vertex_attribute_descriptions
                [r->num_active_vertex_attribute_descriptions++] =
                (VkVertexInputAttributeDescription){
                    .binding =
                        r->vertex_attribute_to_description_location[i],
                    .location = i,
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                };
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            r->vertex_attribute_to_description_location[i] = -1;
            pg->uniform_attrs |= 1 << i;
        }
    }
}
