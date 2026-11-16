/* NIC probe: the MCPX Ethernet controller's registers (0xFEF00000, 4 KB)
 * trapped and logged, with plain-memory semantics -- the title sees exactly
 * what it saw before (the aperture was RAM). Tells which registers the
 * title's network stack touches when System Link starts, before the NIC
 * (nvnet, as in xemu) is emulated for real. CONKER_NO_NIC_PROBE=1 turns it
 * off. */
#include "../platform/mmio_decode.h"
#include "../platform/mmio_decode_a64.h"
#include "../platform/mmio_decode_x86_64.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define NIC_BASE 0xFEF00000u
#define NIC_SIZE 0x1000u

static int s_on;
static uint32_t s_reg[NIC_SIZE / 4];
static unsigned s_logged, s_reads, s_writes;
static uint8_t s_seen[NIC_SIZE / 4];

static void nic_note(int w, uint32_t off, uint64_t v, int size)
{
    /* stdio in a signal handler, deliberately (see apu_mmio_hook.c) */
    if (s_logged < 300 || !s_seen[(off & (NIC_SIZE - 1)) / 4]) {
        s_logged++;
        fprintf(stderr, "[NIC] %s +0x%03X = %08llX (%d)\n", w ? "W" : "R", off,
                (unsigned long long)v, size);
    }
    s_seen[(off & (NIC_SIZE - 1)) / 4] = 1;
}

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    (void)dev;
    uint64_t v = 0;
    if (off + size <= NIC_SIZE) memcpy(&v, (uint8_t *)s_reg + off, size);
    s_reads++;
    nic_note(0, off, v, size);
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;
    if (off + size <= NIC_SIZE) memcpy((uint8_t *)s_reg + off, &val, size);
    s_writes++;
    nic_note(1, off, val, size);
}

void xbox_NicProbeInit(void)
{
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
    if (getenv("CONKER_NO_NIC_PROBE")) return;
    ptrdiff_t off = xbox_GetMemoryOffset();
    if (!off) return;
    DWORD old;
    if (!VirtualProtect((LPVOID)((uintptr_t)off + NIC_BASE), NIC_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "  NIC probe: cannot trap 0x%08X\n", NIC_BASE);
        return;
    }
    s_on = 1;
    fprintf(stderr, "  NIC probe: registers at 0x%08X trapped (logging only)\n", NIC_BASE);
#endif
}

int xbox_NicOwnsAddress(uint32_t va)
{
    return s_on && va >= NIC_BASE && va < NIC_BASE + NIC_SIZE;
}

int xbox_NicHandleMmio(void *ctx, uint32_t va)
{
#if defined(__aarch64__)
    return mmio_emulate_a64((ucontext_t *)ctx, va - NIC_BASE, NULL, nic_read, nic_write);
#elif defined(__x86_64__) && defined(__linux__)
    return mmio_emulate_x86_64((ucontext_t *)ctx, va - NIC_BASE, NULL, nic_read, nic_write);
#else
    (void)ctx; (void)va;
    return 0;
#endif
}
