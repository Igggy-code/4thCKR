/*
 * nv2a-port: xemu's NV2A as a standalone GPU for a statically recompiled
 * Xbox title. Replaces nv2a.c (PCI device, VGA console, VM state) with a
 * flat host API (nv2a_host.h):
 *
 *   - the 16 MiB register aperture (0xFD000000) as read/write calls,
 *     dispatched to xemu's block handlers; PRAMIN (0x700000) is a 1 MiB
 *     buffer as in xemu
 *   - guest RAM handed in as one pointer (physical address 0)
 *   - the interrupt line as a host callback
 *   - vblank: raises PCRTC VBLANK and asks the renderer for a frame
 */
#include "hw/xbox/nv2a/nv2a_int.h"
#include "nv2a_host.h"
#include "nv2a_shim.h"

static NV2AState *s_d;
static nv2a_host_irq_fn s_irq_cb;
static int s_irq_level;

/* ── IRQ ─────────────────────────────────────────────────────────────── */

void pci_irq_assert(PCIDevice *dev)
{
    (void)dev;
    if (!s_irq_level) { s_irq_level = 1; if (s_irq_cb) s_irq_cb(1); }
}
void pci_irq_deassert(PCIDevice *dev)
{
    (void)dev;
    if (s_irq_level) { s_irq_level = 0; if (s_irq_cb) s_irq_cb(0); }
}

void nv2a_update_irq(NV2AState *d)
{
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;

    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;

    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;

    if (d->ptimer.pending_interrupts & d->ptimer.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PTIMER;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PTIMER;

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts)
        pci_irq_assert(PCI_DEVICE(d));
    else
        pci_irq_deassert(PCI_DEVICE(d));
}

/* ── DMA objects (verbatim from nv2a.c) ──────────────────────────────── */

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    dma.address &= 0x07FFFFFF;
    if (dma.address >= memory_region_size(d->vram))
        dma.address &= memory_region_size(d->vram) - 1;   /* 64 MiB mirror */
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

hwaddr nv_clip_gpu_tile_blit(NV2AState *d, hwaddr blit_base_address, hwaddr len)
{
    const uint32_t *regs = d->pfb.regs;
    hwaddr blit_end = blit_base_address + len;
    for (int i = 0; i < NV_NUM_GPU_TILES; ++i) {
        uint32_t base_and_flags = regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)];
        if (!(base_and_flags & NV_PFB_TILE_FLAGS_VALID)) continue;
        uint32_t limit = regs[NV_PFB_TILE_LIMIT(i)];
        if (blit_base_address < limit && blit_end > limit)
            return limit + 1 - blit_base_address;
    }
    return len;
}

/* ── Register blocks ─────────────────────────────────────────────────── */

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = {                    \
        .name   = #NAME,                                                    \
        .offset = OFFSET,                                                   \
        .size   = SIZE,                                                     \
        .ops    = { .read = LNAME ## _read, .write = LNAME ## _write },     \
    }
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    ENTRY(PRMA,     prma,     0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    ENTRY(PCOUNTER, pcounter, 0x00a000, 0x001000),
    ENTRY(PVPE,     pvpe,     0x00b000, 0x001000),
    ENTRY(PTV,      ptv,      0x00d000, 0x001000),
    ENTRY(PRMFB,    prmfb,    0x0a0000, 0x020000),
    ENTRY(PRMVIO,   prmvio,   0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    ENTRY(PSTRAPS,  pstraps,  0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    ENTRY(PRMCIO,   prmcio,   0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    ENTRY(PRMDIO,   prmdio,   0x681000, 0x001000),
    ENTRY(USER,     user,     0x800000, 0x800000),
#undef ENTRY
};

#define RAMIN_OFFSET 0x700000u
#define RAMIN_SIZE   0x100000u

static uint64_t nv2a_host_mmio_read_inner(uint32_t offset, unsigned size);
static void trace_access(char rw, uint32_t offset, uint64_t val, unsigned size);
uint64_t nv2a_host_mmio_read(uint32_t offset, unsigned size)
{
    uint64_t v = nv2a_host_mmio_read_inner(offset, size);
    trace_access('R', offset & 0xFFFFFF, v, size);
    return v;
}

static uint64_t nv2a_host_mmio_read_inner(uint32_t offset, unsigned size)
{
    NV2AState *d = s_d;
    if (!d) return 0;
    offset &= 0xFFFFFF;
    if (offset >= RAMIN_OFFSET && offset < RAMIN_OFFSET + RAMIN_SIZE) {
        uint64_t v = 0;
        memcpy(&v, d->ramin_ptr + (offset - RAMIN_OFFSET), size > 8 ? 8 : size);
        return v;
    }
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (!b->name) continue;
        if (offset >= b->offset && offset < b->offset + b->size) {
            if (size == 8) {
                uint64_t lo = b->ops.read(d, offset - b->offset, 4);
                uint64_t hi = b->ops.read(d, offset - b->offset + 4, 4);
                return lo | (hi << 32);
            }
            uint32_t aligned = (offset - b->offset) & ~3u;
            uint64_t v = b->ops.read(d, aligned, 4);
            if (size < 4) v = (v >> (8 * (offset & 3))) & ((1ull << (8 * size)) - 1);
            return v;
        }
    }
    return 0;
}

static int s_trace = -1;
static long s_trace_n;
static void trace_access(char rw, uint32_t offset, uint64_t val, unsigned size)
{
    if (s_trace < 0) s_trace = getenv("NV2A_TRACE") ? atoi(getenv("NV2A_TRACE")) : 0;
    if (offset >= 0x700000 && offset < 0x800000) return;
    if (offset == 0x100 || offset == 0x140 || offset == 0x600100 || (offset & ~1u) == 0x6013D4) return;
    if (s_trace && s_trace_n < s_trace) {
        s_trace_n++;
        fprintf(stderr, "[nv2a-mmio] %c %06X = %08llX (%u)\n", rw, offset,
                (unsigned long long)val, size);
    }
}

void nv2a_host_mmio_write(uint32_t offset, uint64_t val, unsigned size)
{
    NV2AState *d = s_d;
    if (!d) return;
    offset &= 0xFFFFFF;
    trace_access('W', offset, val, size);
    if (offset >= RAMIN_OFFSET && offset < RAMIN_OFFSET + RAMIN_SIZE) {
        memcpy(d->ramin_ptr + (offset - RAMIN_OFFSET), &val, size > 8 ? 8 : size);
        return;
    }
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (!b->name) continue;
        if (offset >= b->offset && offset < b->offset + b->size) {
            hwaddr rel = offset - b->offset;
            if (size == 8) {
                b->ops.write(d, rel, val & 0xFFFFFFFFu, 4);
                b->ops.write(d, rel + 4, val >> 32, 4);
            } else if (size < 4 && b->offset != 0x601000 && b->offset != 0x0c0000
                       && b->offset != 0x681000) {
                /* Byte/word store to a dword register: merge. The VGA-style
                 * blocks (PRMCIO/PRMVIO/PRMDIO) take byte accesses natively. */
                uint32_t aligned = rel & ~3u;
                uint32_t cur = (uint32_t)b->ops.read(d, aligned, 4);
                uint32_t sh = 8 * (rel & 3), m = (uint32_t)(((1ull << (8 * size)) - 1) << sh);
                cur = (cur & ~m) | (((uint32_t)val << sh) & m);
                b->ops.write(d, aligned, cur, 4);
            } else {
                b->ops.write(d, rel, val, size);
            }
            return;
        }
    }
}

/* ── VGA register file (what PRMCIO/PRMVIO/PRMDIO forward) ───────────── */

uint32_t vga_ioport_read(void *opaque, uint32_t addr)
{
    VGACommonState *s = opaque;
    switch (addr) {
    case 0x3c0: return s->ar_flip_flop ? 0 : s->ar_index;
    case 0x3c1: return s->ar[s->ar_index & 0x1f];
    case 0x3c2: return s->st00;
    case 0x3c4: return s->sr_index;
    case 0x3c5: return s->sr[s->sr_index];
    case 0x3cc: return s->msr;
    case 0x3ce: return s->gr_index;
    case 0x3cf: return s->gr[s->gr_index];
    case 0x3b4: case 0x3d4: return s->cr_index;
    case 0x3b5: case 0x3d5: return s->cr[s->cr_index];
    case 0x3ba: case 0x3da:
        s->ar_flip_flop = 0;
        s->st01 ^= 0x09;    /* toggle retrace bits so polling loops progress */
        return s->st01;
    default: return 0;
    }
}

void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    VGACommonState *s = opaque;
    switch (addr) {
    case 0x3c0:
        if (s->ar_flip_flop == 0) s->ar_index = val & 0x3f;
        else s->ar[s->ar_index & 0x1f] = (uint8_t)val;
        s->ar_flip_flop ^= 1;
        break;
    case 0x3c2: s->msr = (uint8_t)val; break;
    case 0x3c4: s->sr_index = (uint8_t)val; break;
    case 0x3c5: s->sr[s->sr_index] = (uint8_t)val; break;
    case 0x3ce: s->gr_index = (uint8_t)val; break;
    case 0x3cf: s->gr[s->gr_index] = (uint8_t)val; break;
    case 0x3b4: case 0x3d4: s->cr_index = (uint8_t)val; break;
    case 0x3b5: case 0x3d5: s->cr[s->cr_index] = (uint8_t)val; break;
    default: break;
    }
}

static int nv2a_get_bpp(VGACommonState *s)
{
    NV2AState *d = container_of(s, NV2AState, vga);
    switch (s->cr[0x28] & 3) {
    case 2: return d->pramdac.general_control & NV_PRAMDAC_GENERAL_CONTROL_ALT_MODE_SEL ? 16 : 15;
    case 3: return 32;
    default: return 0;
    }
}

static void nv2a_get_params(VGACommonState *s, VGADisplayParams *params)
{
    NV2AState *d = container_of(s, NV2AState, vga);
    params->line_offset = (s->cr[0x13] | ((s->cr[0x19] & 0xe0) << 3) |
                           ((s->cr[0x25] & 0x20) << 6)) << 3;
    params->start_addr = d->pcrtc.start / 4;
    params->line_compare = s->cr[0x18] | ((s->cr[0x07] & 0x10) << 4) |
                           ((s->cr[0x09] & 0x40) << 3);
}

static void nv2a_get_resolution(VGACommonState *s, int *pw, int *ph)
{
    /* Standard VGA CRTC with NV extensions (CR2D bit 1: +256 chars on the
     * horizontal display end, CR25 bit 1: bit 10 of the vertical one). */
    int w = (s->cr[0x01] + 1 + ((s->cr[0x2d] & 0x02) ? 256 : 0)) * 8;
    int h = s->cr[0x12] | ((s->cr[0x07] & 0x02) << 7) | ((s->cr[0x07] & 0x40) << 3)
            | ((s->cr[0x25] & 0x02) << 9);
    h += 1;
    if (w < 64 || h < 64) { w = 640; h = 480; }
    *pw = w; *ph = h;
}

const uint8_t *nv2a_get_dac_palette(void) { return s_d ? s_d->puserdac.palette : NULL; }
int nv2a_get_screen_off(void) { return s_d ? (s_d->vga.sr[VGA_SEQ_CLOCK_MODE] & VGA_SR01_SCREEN_OFF) : 0; }

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void nv2a_reset_regs(NV2AState *d)
{
    memset(d->pfifo.regs, 0, sizeof(d->pfifo.regs));
    memset(d->pgraph.regs_, 0, sizeof(d->pgraph.regs_));
    memset(d->pvideo.regs, 0, sizeof(d->pvideo.regs));

    d->pcrtc.start = 0;
    d->pramdac.core_clock_coeff = 0x00011C01;
    d->pramdac.core_clock_freq = 233333324;
    d->pramdac.memory_clock_coeff = 0;
    d->pramdac.video_clock_coeff = 0x0003C20D;
    d->pfifo.regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    d->vga.msr = VGA_MIS_COLOR;

    d->pgraph.waiting_for_nop = false;
    d->pgraph.waiting_for_flip = false;
    d->pgraph.waiting_for_context_switch = false;
    d->pgraph.vsh_carry_fog[3] = 1.0f;

    d->pmc.pending_interrupts = 0;
    d->pfifo.pending_interrupts = 0;
    d->ptimer.pending_interrupts = 0;
    d->pcrtc.pending_interrupts = 0;
    for (int i = 0; i < 256; i++)
        d->puserdac.palette[i * 3] = d->puserdac.palette[i * 3 + 1] =
            d->puserdac.palette[i * 3 + 2] = (uint8_t)i;
}

static MemoryRegion s_ram;

int nv2a_host_init(const NV2AHostConfig *cfg)
{
    if (s_d) return 0;
    NV2AState *d = g_malloc0(sizeof *d);
    s_irq_cb = cfg->irq;

    g_config.display.renderer = cfg->use_null_renderer ? CONFIG_DISPLAY_RENDERER_NULL
                                                       : CONFIG_DISPLAY_RENDERER_VULKAN;
    g_config.display.quality.surface_scale = cfg->surface_scale > 0 ? cfg->surface_scale : 1;
    g_config.display.vulkan.validation_layers = cfg->validation;
    if (cfg->cache_dir) nv2a_shim_set_base_path(cfg->cache_dir);

    s_ram.ptr = cfg->ram;
    s_ram.size = cfg->ram_size;
    d->vram = &s_ram;
    d->vram_ptr = cfg->ram;
    d->ramin.size = RAMIN_SIZE;
    d->ramin.ptr = g_malloc0(RAMIN_SIZE);
    d->ramin_ptr = d->ramin.ptr;

    nv2a_shim_dirty_init(cfg->ram_size, cfg->protect);

    d->vga.get_bpp = nv2a_get_bpp;
    d->vga.get_params = nv2a_get_params;
    d->vga.get_resolution = nv2a_get_resolution;
    /* Progressive scan: 0x00 here reads as interlaced and doubles the height. */
    d->vga.cr[NV_PRMCIO_INTERLACE_MODE] = NV_PRMCIO_INTERLACE_MODE_DISABLED;

    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    s_d = d;
    {
        extern void nv2a_register_null_renderer(void);
        nv2a_register_null_renderer();
#if defined(NV2A_HAVE_VULKAN)
        extern void nv2a_register_vk_renderer(void);
        nv2a_register_vk_renderer();
#endif
    }
    nv2a_context_init();     /* renderer early_context_init */
    pgraph_init(d);
    nv2a_reset_regs(d);
    ptimer_init(d);

    qemu_thread_create(&d->pfifo.thread, "nv2a.pfifo", pfifo_thread, d, QEMU_THREAD_JOINABLE);
    fprintf(stderr, "[nv2a] xemu NV2A up: %s renderer, RAM %llu MiB\n",
            cfg->use_null_renderer ? "null" : "Vulkan",
            (unsigned long long)(cfg->ram_size >> 20));
    return 1;
}

void nv2a_host_vblank(void)
{
    NV2AState *d = s_d;
    if (!d) return;
    { extern void nv2a_tl(int, uint32_t); nv2a_tl(13, 0); }
    d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
    d->pcrtc.raster = 0;
    nv2a_update_irq(d);
}

extern volatile int nv2a_renderer_ready;
void nv2a_host_present(void)
{
    if (!s_d || !__atomic_load_n(&nv2a_renderer_ready, __ATOMIC_ACQUIRE)) return;
    nv2a_get_framebuffer_surface();
}

uint32_t nv2a_host_pcrtc_start(void) { return s_d ? (uint32_t)s_d->pcrtc.start : 0; }

void nv2a_host_mark_dirty(uint64_t addr, uint64_t size) { nv2a_shim_mark_dirty(addr, size); }

int nv2a_host_irq_pending(void) { return s_irq_level; }

#if defined(NV2A_HAVE_VULKAN)
#include "pgraph/vk/nv2a_vk_present.h"
void nv2a_host_set_window(void *native_window) { nv2a_vk_present_set_window(native_window); }
#else
void nv2a_host_set_window(void *native_window) { (void)native_window; }
#endif

void nv2a_host_debug_dump(void)
{
    NV2AState *d = s_d;
    if (!d) return;
    uint32_t *r = d->pfifo.regs;
    fprintf(stderr,
            "[nv2a-dbg] PFIFO intr=%08X/%08X CACHE1 PUSH0=%X PULL0=%X PUT=%X GET=%X "
            "DMA_PUSH=%X DMA_STATE=%08X DMA_PUT=%X DMA_GET=%X | PGRAPH intr=%08X/%08X "
            "wait nop=%d flip=%d ctx=%d sem_dma=%llX | PMC %08X/%08X irq=%d\n",
            d->pfifo.pending_interrupts, d->pfifo.enabled_interrupts,
            r[NV_PFIFO_CACHE1_PUSH0], r[NV_PFIFO_CACHE1_PULL0],
            r[NV_PFIFO_CACHE1_PUT], r[NV_PFIFO_CACHE1_GET],
            r[NV_PFIFO_CACHE1_DMA_PUSH], r[NV_PFIFO_CACHE1_DMA_STATE],
            r[NV_PFIFO_CACHE1_DMA_PUT], r[NV_PFIFO_CACHE1_DMA_GET],
            d->pgraph.pending_interrupts, d->pgraph.enabled_interrupts,
            d->pgraph.waiting_for_nop, d->pgraph.waiting_for_flip,
            d->pgraph.waiting_for_context_switch,
            (unsigned long long)d->pgraph.dma_semaphore,
            d->pmc.pending_interrupts, d->pmc.enabled_interrupts, s_irq_level);
}
