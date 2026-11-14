#define _GNU_SOURCE 1
#include <dlfcn.h>
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

#include "renderer.h"

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(r->command_buffers),
    };
    VK_CHECK(
        vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers));

    r->slot_cmd[0] = r->command_buffers[0];
    r->slot_aux[0] = r->command_buffers[1];
    r->slot_cmd[1] = r->command_buffers[2];
    r->slot_aux[1] = r->command_buffers[3];
    r->command_buffer = r->command_buffers[0];
    r->aux_command_buffer = r->command_buffers[1];
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeCommandBuffers(r->device, r->command_pool,
                         ARRAY_SIZE(r->command_buffers), r->command_buffers);

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_wait_deferred();
    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    /* nv2a-port diag: a fence with a timeout instead of QueueWaitIdle, so a
     * hung one-off submission names its caller. */
    static VkFence st_fence;
    if (!st_fence) {
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(r->device, &fci, NULL, &st_fence);
    }
    vkResetFences(r->device, 1, &st_fence);
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, st_fence));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    if (vkWaitForFences(r->device, 1, &st_fence, VK_TRUE, 1000000000ull) == VK_TIMEOUT) {
        Dl_info di;
        void *ra = __builtin_return_address(0);
        const char *who = (dladdr(ra, &di) && di.dli_sname) ? di.dli_sname : "?";
        fprintf(stderr, "[vk] ONE-OFF submission hung, from %s (libmain+0x%lx; caller's caller +0x%lx)\n", who,
                (unsigned long)((char *)ra - (char *)di.dli_fbase),
                (unsigned long)((char *)__builtin_return_address(1) - (char *)di.dli_fbase));
        extern char nv2a_vk_last_draw[];
        fprintf(stderr, "[vk]   last draw state: %s\n", nv2a_vk_last_draw);
        VkResult wr = vkWaitForFences(r->device, 1, &st_fence, VK_TRUE, UINT64_MAX);
        fprintf(stderr, "[vk]   ... result %d\n", (int)wr);
        if (wr == VK_ERROR_DEVICE_LOST) nv2a_vk_report_device_fault(r, "one-off");
    }
    VK_CHECK(vkQueueWaitIdle(r->queue));

    r->in_aux_command_buffer = false;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    create_command_pool(pg);
    create_command_buffers(pg);
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    destroy_command_buffers(pg);
    destroy_command_pool(pg);
}
/* nv2a-port: after VK_ERROR_DEVICE_LOST, ask the driver what happened. */
void nv2a_vk_report_device_fault(PGRAPHVkState *r, const char *where)
{
    static int done;
    if (done++) return;
    fprintf(stderr, "[vk] DEVICE LOST (%s)\n", where);
    if (!r->device_fault_enabled || !vkGetDeviceFaultInfoEXT) {
        fprintf(stderr, "[vk]   no VK_EXT_device_fault\n");
        return;
    }
    VkDeviceFaultCountsEXT counts = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    VkResult res = vkGetDeviceFaultInfoEXT(r->device, &counts, NULL);
    fprintf(stderr, "[vk]   fault counts: res %d, %u addresses, %u vendor infos, %llu binary bytes\n",
            (int)res, counts.addressInfoCount, counts.vendorInfoCount,
            (unsigned long long)counts.vendorBinarySize);
    VkDeviceFaultAddressInfoEXT addrs[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (counts.addressInfoCount > 16) counts.addressInfoCount = 16;
    if (counts.vendorInfoCount > 16) counts.vendorInfoCount = 16;
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = addrs,
        .pVendorInfos = vend,
    };
    res = vkGetDeviceFaultInfoEXT(r->device, &counts, &info);
    fprintf(stderr, "[vk]   fault: res %d \"%s\"\n", (int)res, info.description);
    for (uint32_t i = 0; i < counts.addressInfoCount; i++)
        fprintf(stderr, "[vk]   address %u: type %d addr 0x%llx precision 0x%llx\n", i,
                (int)addrs[i].addressType, (unsigned long long)addrs[i].reportedAddress,
                (unsigned long long)addrs[i].addressPrecision);
    for (uint32_t i = 0; i < counts.vendorInfoCount; i++)
        fprintf(stderr, "[vk]   vendor %u: \"%s\" code 0x%llx data 0x%llx\n", i,
                vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
                (unsigned long long)vend[i].vendorFaultData);
}
