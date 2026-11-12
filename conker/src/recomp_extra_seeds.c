/**
 * conker - hand-added entries of the CRT's x87 special-case dispatch table
 * (jmp [ebx] at 0x0047B321, table at 0x0075EFA8). The analysis found the
 * neighbouring stubs but not these two starts, so the indirect jump failed
 * ("[ICALL] Failed to resolve VA 0x0047B334", thousands a second in heavy
 * scenes): the x87 stack was left unpopped and the math after it wrong,
 * and the logging itself stalled the frame. Written as the generator writes
 * its neighbours (sub_0047B336, sub_0047B3E9).
 */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <math.h>

#define fp_push(v) do { double _fp_value = (v); \
    g_fp_top = (g_fp_top + 7u) & 7u; \
    g_fp_stack[g_fp_top] = _fp_value; } while (0)
#define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)

/* fstp st(0); fstp st(0); fldz; ret */
void sub_0047B334(void)
{
    fp_pop();
    fp_pop();
    fp_push(0.0);
    esp += 4; return;
}

/* fstp st(0); fstp st(0); fld tbyte [0x655D78];
 * cmp byte [ebp-0x90], 0; jg L; mov byte [ebp-0x90], 1; L: or cl, cl; ret */
void sub_0047B3D6(void)
{
    uint32_t ebp = g_seh_ebp;   /* fpo_leaf: the caller's frame */
    fp_pop();
    fp_pop();
    fp_push(recomp_f80_load(((uint64_t)MEM32((0x655D78) + 4) << 32) | MEM32(0x655D78),
                            MEM16((0x655D78) + 8)));
    if (!((int8_t)MEM8(ebp + -144) > 0))
        MEM8(ebp + -144) = 1;
    uint32_t _fa = (uint32_t)LO8(ecx) & 0xFFu;
    int32_t _fas = (int32_t)(int8_t)_fa;
    g_eflags = (g_eflags & ~0x8C5u) | ((RECOMP_PARITY8(_fa)) ? 0x4u : 0u) |
               ((_fa == 0) ? 0x40u : 0u) | ((_fas < 0) ? 0x80u : 0u);
    g_ebp = ebp; g_seh_ebp = ebp; esp += 4; return;
}
