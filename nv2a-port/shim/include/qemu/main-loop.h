#pragma once
#include "qemu/osdep.h"
#define error_report(...) (fprintf(stderr, __VA_ARGS__), fputc(10, stderr))
#define warn_report(...) (fprintf(stderr, __VA_ARGS__), fputc(10, stderr))
static inline void bql_lock(void) {}
static inline void bql_unlock(void) {}
static inline int bql_locked(void) { return 1; }
static inline void rcu_register_thread(void) {}
static inline void rcu_unregister_thread(void) {}
