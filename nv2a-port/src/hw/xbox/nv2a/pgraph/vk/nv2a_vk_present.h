#ifndef NV2A_VK_PRESENT_H
#define NV2A_VK_PRESENT_H
/* nv2a-port: host window presentation for the Vulkan renderer. */
void nv2a_vk_present_set_window(void *native_window);
void nv2a_vk_present_set_aspect(float aspect);
#ifdef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
void nv2a_vk_present(PGRAPHState *pg, VkImage src, int width, int height);
void nv2a_vk_present_finalize(PGRAPHState *pg);
#endif
#endif
