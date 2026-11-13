/*
 * Device-model types xemu's NV2A expects from QEMU, reduced to what a
 * standalone GPU needs. Implemented in shim/nv2a_shim.c.
 */
#ifndef NV2A_HOST_TYPES_H
#define NV2A_HOST_TYPES_H
#include <stdint.h>
#include <stdbool.h>

typedef struct Object Object;
typedef struct PCIDevice { uint8_t config[256]; } PCIDevice;
typedef struct PCIBus PCIBus;
typedef void *qemu_irq;

/* Guest RAM as one region: base pointer + size. Dirty tracking is
 * per-client bitmaps fed by write-protect faults (nv2a_shim.c). */
typedef struct MemoryRegion {
    uint8_t *ptr;
    uint64_t size;
} MemoryRegion;

typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned size);
    void (*write)(void *opaque, hwaddr addr, uint64_t val, unsigned size);
} MemoryRegionOps;

enum {
    DIRTY_MEMORY_VGA = 0,
    DIRTY_MEMORY_CODE = 1,
    DIRTY_MEMORY_MIGRATION = 2,
    DIRTY_MEMORY_NV2A = 3,
    DIRTY_MEMORY_NV2A_TEX = 4,
    DIRTY_MEMORY_NUM = 5,
};

static inline uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static inline void *memory_region_get_ram_ptr(MemoryRegion *mr) { return mr->ptr; }
bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);
void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);
void memory_region_set_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size);
bool memory_region_get_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);
static inline void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client) { (void)mr; (void)log; (void)client; }

/* Timers (PTIMER alarm). */
typedef void QEMUTimerCB(void *opaque);
typedef struct QEMUTimer {
    QEMUTimerCB *cb;
    void *opaque;
    int64_t expire_ns;
    bool armed;
    struct QEMUTimer *next;
} QEMUTimer;
enum { QEMU_CLOCK_REALTIME = 0, QEMU_CLOCK_VIRTUAL = 1, QEMU_CLOCK_HOST = 2 };
int64_t qemu_clock_get_ns(int clock);
static inline int64_t qemu_clock_get_us(int clock) { return qemu_clock_get_ns(clock) / 1000; }
static inline int64_t qemu_clock_get_ms(int clock) { return qemu_clock_get_ns(clock) / 1000000; }
void timer_init_ns(QEMUTimer *t, int clock, QEMUTimerCB *cb, void *opaque);
void timer_mod(QEMUTimer *t, int64_t expire_ns);
void timer_del(QEMUTimer *t);
bool timer_pending(QEMUTimer *t);
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{ return (uint64_t)(((__uint128_t)a * b) / c); }
static inline uint64_t muldiv64_round_up(uint64_t a, uint32_t b, uint32_t c)
{ return (uint64_t)((((__uint128_t)a * b) + c - 1) / c); }
#define NANOSECONDS_PER_SECOND 1000000000LL

/* VGA leftovers referenced by register blocks. */
typedef struct VGADisplayParams { uint32_t line_offset, start_addr, line_compare; } VGADisplayParams;
typedef struct VGACommonState {
    uint8_t sr[256], gr[256], ar[21], cr[256];
    uint8_t ar_flip_flop, ar_index, msr, sr_index, gr_index, cr_index, fcr, st00, st01;
    void (*get_params)(struct VGACommonState *, VGADisplayParams *);
    int (*get_bpp)(struct VGACommonState *);
    void (*get_resolution)(struct VGACommonState *, int *w, int *h);
} VGACommonState;
static inline void vga_invalidate_scanlines(VGACommonState *s, int a, int b) { (void)s;(void)a;(void)b; }
typedef struct GraphicHwOps { int dummy; } GraphicHwOps;
uint32_t vga_ioport_read(void *opaque, uint32_t addr);
void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val);

/* No TCG: dirty bitmaps, not CPU access callbacks, keep surfaces honest. */
static inline bool tcg_enabled(void) { return false; }
typedef struct CPUState CPUState;
typedef struct MemAccessCallback MemAccessCallback;
static inline CPUState *qemu_get_cpu(int i) { (void)i; return 0; }
static inline MemAccessCallback *mem_access_callback_insert(CPUState *c, MemoryRegion *mr, hwaddr a, hwaddr l, void *cb, void *o) { (void)c;(void)mr;(void)a;(void)l;(void)cb;(void)o; return 0; }
static inline void mem_access_callback_remove_by_ref(CPUState *c, MemAccessCallback *cb) { (void)c;(void)cb; }

void pci_irq_assert(PCIDevice *d);
void pci_irq_deassert(PCIDevice *d);
#define PCI_DEVICE(d) (&(d)->parent_obj)
#endif
