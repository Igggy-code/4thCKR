/*
 * nv2a-port: present xemu's composed display image (the scanout surface with
 * the PVIDEO overlay already blended in by display.c) to the host window.
 *
 * Runs on the PFIFO thread, right after pgraph_vk_render_display(), with the
 * renderer's own device and queue. The image is blitted aspect-correct
 * (letterboxed 4:3 or 16:9) into an Android swapchain image.
 */
#include "renderer.h"
#include "nv2a_vk_present.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
volatile int nv2a_force_pillar;
volatile unsigned nv2a_presented;   /* frames that reached the screen */   /* set by the game layer: show 4:3 */
#endif

#define MAX_SC_IMAGES 8

static struct {
    void *window;            /* ANativeWindow* */
    bool window_changed;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t n_images;
    VkImage images[MAX_SC_IMAGES];
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired, rendered;
    VkFence fence;
    bool fence_pending;
    uint64_t frames;
    float aspect;            /* 0: use the image's own */
} s_pr;

static QemuMutex s_win_lock;
static bool s_win_lock_init;

void nv2a_vk_present_set_window(void *native_window)
{
    if (!s_win_lock_init) { qemu_mutex_init(&s_win_lock); s_win_lock_init = true; }
    qemu_mutex_lock(&s_win_lock);
    s_pr.window = native_window;
    s_pr.window_changed = true;
    qemu_mutex_unlock(&s_win_lock);
}

void nv2a_vk_present_set_aspect(float aspect) { s_pr.aspect = aspect; }
/* how much a 2D element drawn into the title's 4:3 frame gets widened on the
 * way to the screen (1: not at all) -- the speech bubbles are squeezed back */
float nv2a_present_2d_squeeze(void)
{
    float a = s_pr.aspect;
    return a > 4.0f / 3.0f + 0.01f ? (4.0f / 3.0f) / a : 1.0f;
}
volatile int64_t nv2a_wide_cam_us = -1;   /* -1: no widened camera mode */

static void destroy_swapchain(PGRAPHVkState *r)
{
    if (s_pr.fence_pending) {
        vkWaitForFences(r->device, 1, &s_pr.fence, VK_TRUE, UINT64_MAX);
        s_pr.fence_pending = false;
    }
    if (s_pr.swapchain) {
        vkDestroySwapchainKHR(r->device, s_pr.swapchain, NULL);
        s_pr.swapchain = VK_NULL_HANDLE;
    }
    s_pr.n_images = 0;
}

static void destroy_surface(PGRAPHVkState *r)
{
    destroy_swapchain(r);
    if (s_pr.surface) {
        vkDestroySurfaceKHR(r->instance, s_pr.surface, NULL);
        s_pr.surface = VK_NULL_HANDLE;
    }
}

static bool create_surface(PGRAPHVkState *r)
{
#if defined(__ANDROID__)
    if (!s_pr.window) return false;
    VkAndroidSurfaceCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = (ANativeWindow *)s_pr.window,
    };
    if (vkCreateAndroidSurfaceKHR(r->instance, &ci, NULL, &s_pr.surface) != VK_SUCCESS) {
        fprintf(stderr, "[present] vkCreateAndroidSurfaceKHR failed\n");
        s_pr.surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
#else
    (void)r;
    return false;
#endif
}

static bool create_swapchain(PGRAPHVkState *r)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical_device, s_pr.surface, &caps) != VK_SUCCESS)
        return false;

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, s_pr.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical_device, s_pr.surface, &nf, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            fmt = fmts[i];
            break;
        }

    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical_device, s_pr.surface, &npm, NULL);
    VkPresentModeKHR pms[16];
    if (npm > 16) npm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical_device, s_pr.surface, &npm, pms);
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < npm; i++)
        if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) pm = VK_PRESENT_MODE_MAILBOX_KHR;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 1280; ext.height = 720; }
    if (ext.width == 0 || ext.height == 0) return false;   /* minimised */

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SC_IMAGES) count = MAX_SC_IMAGES;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_pr.surface,
        .minImageCount = count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        /* Let the compositor rotate: the activity is landscape, the panel
         * may be natively portrait. */
        .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = pm,
        .clipped = VK_TRUE,
        .oldSwapchain = s_pr.swapchain,
    };
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(r->device, &ci, NULL, &sc) != VK_SUCCESS) {
        fprintf(stderr, "[present] vkCreateSwapchainKHR failed\n");
        return false;
    }
    if (s_pr.swapchain) vkDestroySwapchainKHR(r->device, s_pr.swapchain, NULL);
    s_pr.swapchain = sc;
    s_pr.format = fmt.format;
    s_pr.extent = ext;
    s_pr.n_images = MAX_SC_IMAGES;
    vkGetSwapchainImagesKHR(r->device, sc, &s_pr.n_images, s_pr.images);
    fprintf(stderr, "[present] swapchain %ux%u, %u images, format %d, %s\n",
            ext.width, ext.height, s_pr.n_images, fmt.format,
            pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return true;
}

static bool ensure_sync(PGRAPHVkState *r)
{
    if (s_pr.pool) return true;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = (uint32_t)pgraph_vk_find_queue_families(r->physical_device).queue_family,
    };
    if (vkCreateCommandPool(r->device, &pci, NULL, &s_pr.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s_pr.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(r->device, &ai, &s_pr.cmd);
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(r->device, &si, NULL, &s_pr.acquired);
    vkCreateSemaphore(r->device, &si, NULL, &s_pr.rendered);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(r->device, &fi, NULL, &s_pr.fence);
    return true;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* nv2a-port: on-screen touch hints, drawn into the pillarbox bars by
 * blitting one grey texel from a tiny linear image. NV2A_NO_OVERLAY hides
 * them. Rectangles are in window-normalised units, matching the touch zones
 * in conker/src/main.c. */
static VkImage s_ov_img;
static VkDeviceMemory s_ov_mem;
static int s_ov_state;   /* 0 = untried, 1 = ready, -1 = unavailable */

static bool overlay_init(PGRAPHVkState *r)
{
    if (s_ov_state) return s_ov_state > 0;
    s_ov_state = -1;
    if (!getenv("NV2A_OVERLAY")) return false;   /* Java overlay draws the pad now */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r->physical_device, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return false;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 2, 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    if (vkCreateImage(r->device, &ci, NULL, &s_ov_img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(r->device, s_ov_img, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &mp);
    uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t ti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { ti = i; break; }
    if (ti == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = mr.size, .memoryTypeIndex = ti };
    if (vkAllocateMemory(r->device, &ai, NULL, &s_ov_mem) != VK_SUCCESS) return false;
    vkBindImageMemory(r->device, s_ov_img, s_ov_mem, 0);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sl;
    vkGetImageSubresourceLayout(r->device, s_ov_img, &sub, &sl);
    uint8_t *m;
    if (vkMapMemory(r->device, s_ov_mem, 0, VK_WHOLE_SIZE, 0, (void **)&m) != VK_SUCCESS) return false;
    static const uint8_t px[8] = { 90, 90, 90, 255, 200, 200, 200, 255 };
    memcpy(m + sl.offset, px, sizeof px);
    vkUnmapMemory(r->device, s_ov_mem);
    s_ov_state = 1;
    return true;
}

static void overlay_rect(VkCommandBuffer cb, VkImage dst, int dw, int dh,
                         float x0, float y0, float x1, float y1, int texel)
{
    int t = dh / 270 + 1;   /* line thickness */
    int ax = (int)(x0 * dw), ay = (int)(y0 * dh), bx = (int)(x1 * dw), by = (int)(y1 * dh);
    if (ax < 0) ax = 0; if (ay < 0) ay = 0; if (bx > dw) bx = dw; if (by > dh) by = dh;
    int rc[4][4] = { { ax, ay, bx, ay + t }, { ax, by - t, bx, by },
                     { ax, ay, ax + t, by }, { bx - t, ay, bx, by } };
    VkImageBlit b[4];
    for (int i = 0; i < 4; i++) {
        b[i] = (VkImageBlit){
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { texel, 0, 0 }, { texel + 1, 1, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { rc[i][0], rc[i][1], 0 }, { rc[i][2], rc[i][3], 1 } },
        };
    }
    vkCmdBlitImage(cb, s_ov_img, VK_IMAGE_LAYOUT_GENERAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 4, b, VK_FILTER_NEAREST);
}

static void overlay_draw(PGRAPHVkState *r, VkCommandBuffer cb, VkImage dst, int dw, int dh)
{
    static bool transitioned;
    if (!overlay_init(r)) return;
    if (!transitioned) {
        barrier(cb, s_ov_img, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        transitioned = true;
    }
    float a = (float)dh / (float)dw;            /* keep squares square */
    overlay_rect(cb, dst, dw, dh, 0.86f, 0.0f, 1.0f, 0.14f, 1);   /* Start */
    overlay_rect(cb, dst, dw, dh, 0.0f, 0.0f, 0.14f, 0.14f, 0);   /* Back */
    overlay_rect(cb, dst, dw, dh, 0.0f, 0.16f, 0.10f, 0.44f, 0);  /* LT */
    overlay_rect(cb, dst, dw, dh, 0.90f, 0.16f, 1.0f, 0.44f, 0);  /* RT */
    float cx = 0.86f, cy = 0.72f, off = 0.12f, sz = 0.05f;
    struct { float dx, dy; int t; } fb[4] = {
        { 0, off, 1 }, { off * a, 0, 0 }, { -off * a, 0, 0 }, { 0, -off, 0 } };  /* A B X Y */
    for (int i = 0; i < 4; i++) {
        float x = cx + fb[i].dx, y = cy + fb[i].dy;
        overlay_rect(cb, dst, dw, dh, x - sz * a, y - sz, x + sz * a, y + sz, fb[i].t);
    }
    overlay_rect(cb, dst, dw, dh, 0.03f, 0.55f, 0.03f + 0.36f * a, 0.91f, 0);   /* move stick */
}

void nv2a_vk_present(PGRAPHState *pg, VkImage src, int width, int height)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!src || width <= 0 || height <= 0) return;
    static int no_present = -1;
    if (no_present < 0) no_present = getenv("NV2A_NO_PRESENT") != NULL;
    if (no_present) return;   /* nv2a-port experiment */
    if (!s_win_lock_init) return;

    qemu_mutex_lock(&s_win_lock);
    if (s_pr.window_changed) {
        destroy_surface(r);
        s_pr.window_changed = false;
        if (s_pr.window && !create_surface(r)) { qemu_mutex_unlock(&s_win_lock); return; }
    }
    qemu_mutex_unlock(&s_win_lock);
    if (!s_pr.surface) return;
    if (!ensure_sync(r)) return;
    if (!s_pr.swapchain && !create_swapchain(r)) return;

    if (s_pr.fence_pending) {
        vkWaitForFences(r->device, 1, &s_pr.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(r->device, 1, &s_pr.fence);
        s_pr.fence_pending = false;
    }

    /* Acquire with a CPU fence, never a semaphore: on Android the acquire
     * carries SurfaceFlinger's release fence, and a GPU wait on it inside
     * our queue (screen off, compositor busy) stalls every later job until
     * the kernel cancels them (BASE_JD_EVENT_JOB_CANCELLED, DEVICE_LOST).
     * Waited here with a short timeout; a frame the compositor is not ready
     * for is skipped. */
    static VkFence acq_fence;
    if (!acq_fence) {
        VkFenceCreateInfo afi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(r->device, &afi, NULL, &acq_fence);
    }
    uint32_t idx;
    vkResetFences(r->device, 1, &acq_fence);
    VkResult res = vkAcquireNextImageKHR(r->device, s_pr.swapchain, 0,
                                         VK_NULL_HANDLE, acq_fence, &idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) { create_swapchain(r); return; }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        /* none free: skip the frame. But not forever: a swapchain that
         * stops handing out images (the surface lost, or images that never
         * came back) froze the picture on one frame while the game ran on,
         * and after the app was hidden and shown again the screen stayed
         * black. After 1 s of nothing, rebuild the surface and swapchain. */
        static int64_t fail_since; static unsigned nlog;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        if (!fail_since) fail_since = now;
        if (nlog < 40 && (res != VK_NOT_READY && res != VK_TIMEOUT)) {
            nlog++; fprintf(stderr, "[present] acquire failed: VkResult %d\n", (int)res);
        }
        if (now - fail_since > 1000 || res == VK_ERROR_SURFACE_LOST_KHR) {
            fprintf(stderr, "[present] no image for %lld ms (VkResult %d): rebuilding the surface and swapchain\n",
                    (long long)(now - fail_since), (int)res);
            fail_since = 0;
            vkDeviceWaitIdle(r->device);
            qemu_mutex_lock(&s_win_lock);
            destroy_surface(r);
            if (s_pr.window) create_surface(r);
            qemu_mutex_unlock(&s_win_lock);
        }
        return;
    }

    if (vkWaitForFences(r->device, 1, &acq_fence, VK_TRUE, 30000000ull) != VK_SUCCESS) {
        /* The image is ours but not released yet: present it untouched
         * later is not possible without drawing; wait it out on the CPU. */
        vkWaitForFences(r->device, 1, &acq_fence, VK_TRUE, UINT64_MAX);
    }
    VkImage dst = s_pr.images[idx];

    /* Aspect-correct destination rectangle. */
    float src_aspect = s_pr.aspect > 0 ? s_pr.aspect : (float)width / (float)height;
    {   /* nv2a-port: frames with no widened 3D camera in the last 0.4 s
         * (loading screens, 2D menus, movies) are 4:3 art: pillarbox them
         * instead of stretching to the screen. */
        extern volatile int64_t nv2a_wide_cam_us;
        extern volatile int64_t g_conker_last_play_ms __attribute__((weak));
        static bool narrow;
        if (s_pr.aspect > 0) {
            /* no 3D camera yet (boot logos, movies): 4:3 from the start */
            int64_t wide_cam = nv2a_wide_cam_us >= 0 ? nv2a_wide_cam_us : INT64_MIN / 2;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
            bool play = &g_conker_last_play_ms && g_conker_last_play_ms &&
                        now / 1000 - g_conker_last_play_ms < 2000;
            /* 4:3 only after 2 s with no 3D camera and no gameplay; back to
             * wide at once (games may reuse a projection for a while) */
            /* a heavy disc read (over 2 MB in 0.5 s, more than any stream)
             * outside gameplay is a load: its screen is 4:3 art, whatever
             * camera the title sets meanwhile -- that camera flipped it
             * between wide and 4:3. Held 1 s past the reads. */
            extern uint64_t xbox_io_recent_bytes(void) __attribute__((weak));
            static int64_t load_until;
            uint64_t io = xbox_io_recent_bytes ? xbox_io_recent_bytes() : 0;
            if (io > (1536u << 10))   /* movies stream 1 MB bursts: not a load */
                load_until = now + 1000000;
            {   /* a level package opened: a load screen from now, held
                 * while the reads last (above) and at least 3 s */
                extern volatile int64_t g_xbox_load_mark_ms __attribute__((weak));
                static int64_t seen;
                if (&g_xbox_load_mark_ms && g_xbox_load_mark_ms != seen) {
                    seen = g_xbox_load_mark_ms;
                    if (now + 0 < seen * 1000 + 3000000 + 1000000)
                        load_until = seen * 1000 + 3000000 > load_until ? seen * 1000 + 3000000 : load_until;
                }
            }
            {   static int64_t last_io_log; static bool was_loading;
                bool l = !play && now < load_until;
                if (l != was_loading || now - last_io_log > 5000000) {
                    fprintf(stderr, "[ASPECT] io %.0f KB/0.5s, loading %d, play %d, narrow %d, cam age %lld ms, %dx%d\n",
                            io / 1024.0, l, play, narrow,
                            nv2a_wide_cam_us >= 0 ? (long long)(now - nv2a_wide_cam_us) / 1000 : -1LL,
                            width, height);
                    last_io_log = now; was_loading = l;
                }
            }
            bool loading = !play && now < load_until;
            if (loading) narrow = true;
            else if (play || now - wide_cam < 100000) narrow = false;
            else if (now - wide_cam > 2000000) narrow = true;
            /* the user wants every screen filled (movies, loading screens
             * included): the 4:3 pillarbox is opt-in, NV2A_PILLARBOX=1 */
            static int pillar = -1;
            if (pillar < 0) pillar = getenv("NV2A_PILLARBOX") != NULL;
            if (narrow && pillar) src_aspect = 4.0f / 3.0f;
            {   extern volatile int nv2a_force_pillar;   /* the game's pause screen */
                if (nv2a_force_pillar) src_aspect = 4.0f / 3.0f; }
        }
    }
    int dw = (int)s_pr.extent.width, dh = (int)s_pr.extent.height;
    int w = dw, h = (int)(dw / src_aspect);
    if (h > dh) { h = dh; w = (int)(dh * src_aspect); }
    if (s_pr.aspect < 0) { w = dw; h = dh; }   /* fill: stretch to the window */
    int x0 = (dw - w) / 2, y0 = (dh - h) / 2;

    vkResetCommandBuffer(s_pr.cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(s_pr.cmd, &bi);

    barrier(s_pr.cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* Source stage = the stage the acquire semaphore is waited at, so the
     * layout transition is ordered after the acquire (validation hazard). */
    barrier(s_pr.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(s_pr.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    barrier(s_pr.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        /* xemu renders the display image with GL's bottom-up origin: flip. */
        .srcOffsets = { { 0, height, 0 }, { width, 0, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { x0, y0, 0 }, { x0 + w, y0 + h, 1 } },
    };
    vkCmdBlitImage(s_pr.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    overlay_draw(r, s_pr.cmd, dst, dw, dh);

    barrier(s_pr.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(s_pr.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(s_pr.cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    (void)wait_stage;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 0,
        .commandBufferCount = 1,
        .pCommandBuffers = &s_pr.cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &s_pr.rendered,
    };
    {   VkResult sr = vkQueueSubmit(r->queue, 1, &si, s_pr.fence);
        if (sr != VK_SUCCESS) {
            static unsigned nlog;
            if (nlog++ < 20) fprintf(stderr, "[present] submit failed: VkResult %d\n", (int)sr);
            return;
        } }
    s_pr.fence_pending = true;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &s_pr.rendered,
        .swapchainCount = 1,
        .pSwapchains = &s_pr.swapchain,
        .pImageIndices = &idx,
    };
    res = vkQueuePresentKHR(r->queue, &pi);
    /* SUBOPTIMAL is routine on Android (rotation handled by the compositor):
     * recreate only when the swapchain is really out of date. */
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        vkWaitForFences(r->device, 1, &s_pr.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(r->device, 1, &s_pr.fence);
        s_pr.fence_pending = false;
        create_swapchain(r);
    }
    {   extern volatile unsigned nv2a_presented; nv2a_presented++; }
    if ((++s_pr.frames % 600) == 1)
        fprintf(stderr, "[present] frame %llu (%dx%d -> %ux%u)\n",
                (unsigned long long)s_pr.frames, width, height,
                s_pr.extent.width, s_pr.extent.height);
}

void nv2a_vk_present_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (!r) return;
    destroy_surface(r);
    if (s_pr.pool) {
        vkDestroySemaphore(r->device, s_pr.acquired, NULL);
        vkDestroySemaphore(r->device, s_pr.rendered, NULL);
        vkDestroyFence(r->device, s_pr.fence, NULL);
        vkDestroyCommandPool(r->device, s_pr.pool, NULL);
        s_pr.pool = VK_NULL_HANDLE;
    }
}
