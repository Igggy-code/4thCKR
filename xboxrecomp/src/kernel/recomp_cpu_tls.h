/* nv2a-port: the guest CPU state, one thread-local block per thread.
 *
 * It used to be a dozen separate __thread variables. libmain.so is loaded
 * with dlopen, so on Android every thread-local variable is reached through
 * a TLS descriptor call into the dynamic linker -- one per variable per
 * function, about ten in each small translated function: 11% of the game
 * thread was spent in linker64. Fields of one block share a single call. */
#ifndef RECOMP_CPU_TLS_H
#define RECOMP_CPU_TLS_H
#include <stdint.h>
#ifndef RECOMP_TLS
#  if defined(_MSC_VER)
#    define RECOMP_TLS __declspec(thread)
#  else
#    define RECOMP_TLS __thread
#  endif
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

typedef struct RecompCpu {
    uint32_t c_eax, c_ecx, c_edx, c_esp, c_ebx, c_esi, c_edi, c_ebp;
    uint32_t c_seh_ebp, c_eflags, c_fs_base, c_mxcsr;
    int c_df, c_fp_top, c_fp_cmp;
    uint16_t c_fp_control_word, c_fp_cc;
    double c_fp_stack[8];
    RecompXmm c_xmm0, c_xmm1, c_xmm2, c_xmm3, c_xmm4, c_xmm5, c_xmm6, c_xmm7;
    RecompMmx c_mm0, c_mm1, c_mm2, c_mm3, c_mm4, c_mm5, c_mm6, c_mm7;
} RecompCpu;

extern RECOMP_TLS RecompCpu g_cpu;

#define g_eax (g_cpu.c_eax)
#define g_ecx (g_cpu.c_ecx)
#define g_edx (g_cpu.c_edx)
#define g_esp (g_cpu.c_esp)
#define g_ebx (g_cpu.c_ebx)
#define g_esi (g_cpu.c_esi)
#define g_edi (g_cpu.c_edi)
#define g_ebp (g_cpu.c_ebp)
#define g_seh_ebp (g_cpu.c_seh_ebp)
#define g_eflags (g_cpu.c_eflags)
#define g_fs_base (g_cpu.c_fs_base)
#define g_mxcsr (g_cpu.c_mxcsr)
#define g_df (g_cpu.c_df)
#define g_fp_top (g_cpu.c_fp_top)
#define g_fp_cmp (g_cpu.c_fp_cmp)
#define g_fp_control_word (g_cpu.c_fp_control_word)
#define g_fp_cc (g_cpu.c_fp_cc)
#define g_fp_stack (g_cpu.c_fp_stack)
#define g_xmm0 (g_cpu.c_xmm0)
#define g_xmm1 (g_cpu.c_xmm1)
#define g_xmm2 (g_cpu.c_xmm2)
#define g_xmm3 (g_cpu.c_xmm3)
#define g_xmm4 (g_cpu.c_xmm4)
#define g_xmm5 (g_cpu.c_xmm5)
#define g_xmm6 (g_cpu.c_xmm6)
#define g_xmm7 (g_cpu.c_xmm7)
#define g_mm0 (g_cpu.c_mm0)
#define g_mm1 (g_cpu.c_mm1)
#define g_mm2 (g_cpu.c_mm2)
#define g_mm3 (g_cpu.c_mm3)
#define g_mm4 (g_cpu.c_mm4)
#define g_mm5 (g_cpu.c_mm5)
#define g_mm6 (g_cpu.c_mm6)
#define g_mm7 (g_cpu.c_mm7)

#endif
