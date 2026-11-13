#pragma once
#include "qemu/osdep.h"
typedef union { void *host_ptr; int host_int; } run_on_cpu_data;
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){ .host_ptr = (p) })
typedef void (*run_on_cpu_func)(CPUState *cpu, run_on_cpu_data data);
/* The renderer never changes at run time here. */
static inline void async_safe_run_on_cpu(CPUState *cpu, run_on_cpu_func f, run_on_cpu_data d) { f(cpu, d); }
