// nv2a-port: VulkanMemoryAllocator implementation. Vulkan entry points come
// from volk's function pointers (VK_NO_PROTOTYPES), as in xemu.
#include <volk.h>
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>
