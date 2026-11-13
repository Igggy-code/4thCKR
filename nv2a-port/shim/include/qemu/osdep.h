/* Minimal qemu/osdep.h for xemu's NV2A outside QEMU (see shim/README). */
#ifndef NV2A_SHIM_OSDEP_H
#define NV2A_SHIM_OSDEP_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include "glib.h"
#include "qemu/compiler.h"

typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define ROUND_DOWN(n, d) ((n) & -(0 ? (n) : (d)))
#define QEMU_ALIGN_UP(n, m) (DIV_ROUND_UP(n, m) * (m))
#define QEMU_ALIGN_DOWN(n, m) ((n) / (m) * (m))
#define QEMU_IS_ALIGNED(n, m) (((n) % (m)) == 0)
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define BIT(nr) (1UL << (nr))
#define MAKE_64BIT_MASK(shift, length) \
    (((~0ULL) >> (64 - (length))) << (shift))

static inline int ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int ctz64(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
static inline int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline uint64_t pow2ceil(uint64_t v) { return v <= 1 ? 1 : 1ULL << (64 - clz64(v - 1)); }
static inline uint32_t extract32(uint32_t v, int s, int l) { return (v >> s) & (~0U >> (32 - l)); }
static inline uint64_t extract64(uint64_t v, int s, int l) { return (v >> s) & (~0ULL >> (64 - l)); }
static inline int32_t sextract32(uint32_t v, int s, int l) { return ((int32_t)(v << (32 - l - s))) >> (32 - l); }
static inline uint32_t deposit32(uint32_t v, int s, int l, uint32_t f)
{ uint32_t m = (~0U >> (32 - l)) << s; return (v & ~m) | ((f << s) & m); }
static inline uint64_t deposit64(uint64_t v, int s, int l, uint64_t f)
{ uint64_t m = (~0ULL >> (64 - l)) << s; return (v & ~m) | ((f << s) & m); }
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }

#define qatomic_read(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define qatomic_set(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define qatomic_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define qatomic_dec(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define qatomic_or(p, v) __atomic_or_fetch((p), (v), __ATOMIC_SEQ_CST)
#define qatomic_and(p, v) __atomic_and_fetch((p), (v), __ATOMIC_SEQ_CST)
#define qatomic_xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1 << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK ((hwaddr)~(TARGET_PAGE_SIZE - 1))
#define TARGET_PAGE_ALIGN(a) (((a) + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK)

FILE *qemu_fopen(const char *path, const char *mode);
int qemu_mkdir(const char *path);
int qemu_unlink(const char *path);

typedef struct Error Error;
#include "nv2a_host_types.h"
#endif
