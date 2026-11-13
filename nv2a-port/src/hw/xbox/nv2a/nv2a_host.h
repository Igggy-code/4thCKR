/*
 * nv2a-port public API: xemu's NV2A GPU embedded in a host program.
 */
#ifndef NV2A_HOST_H
#define NV2A_HOST_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nv2a_host_irq_fn)(int level);
/* Re-arm write tracking on [addr, addr+size) of guest RAM (physical). */
typedef void (*nv2a_host_protect_fn)(uint64_t addr, uint64_t size);

typedef struct NV2AHostConfig {
    uint8_t *ram;              /* guest physical address 0 */
    uint64_t ram_size;         /* 64 MiB */
    nv2a_host_irq_fn irq;      /* PCI INTA level */
    nv2a_host_protect_fn protect;
    const char *cache_dir;     /* shader cache */
    int use_null_renderer;     /* 1: no GPU (headless checks) */
    int surface_scale;         /* 1 = native 640x480 */
    int validation;            /* Vulkan validation layers */
} NV2AHostConfig;

int nv2a_host_init(const NV2AHostConfig *cfg);
/* Register aperture at 0xFD000000: offset within the 16 MiB window. */
uint64_t nv2a_host_mmio_read(uint32_t offset, unsigned size);
void nv2a_host_mmio_write(uint32_t offset, uint64_t val, unsigned size);
/* 60 Hz: PCRTC vblank interrupt. */
void nv2a_host_vblank(void);
/* Render the scanout surface and present it (blocks until done). */
void nv2a_host_present(void);
void nv2a_host_set_window(void *native_window);
/* The CPU wrote guest RAM [addr, addr+size): textures/surfaces may be stale. */
void nv2a_host_mark_dirty(uint64_t addr, uint64_t size);
int nv2a_host_irq_pending(void);
uint32_t nv2a_host_pcrtc_start(void);
void nv2a_host_debug_dump(void);

#ifdef __cplusplus
}
#endif
#endif
