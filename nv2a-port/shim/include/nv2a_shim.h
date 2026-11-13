#ifndef NV2A_SHIM_H
#define NV2A_SHIM_H
#include <stdint.h>
typedef void (*nv2a_shim_protect_fn)(uint64_t addr, uint64_t size);
void nv2a_shim_dirty_init(uint64_t ram_size, nv2a_shim_protect_fn protect);
void nv2a_shim_mark_dirty(uint64_t addr, uint64_t size);
void nv2a_shim_set_base_path(const char *path);
#endif
