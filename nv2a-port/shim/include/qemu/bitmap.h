#ifndef NV2A_SHIM_BITMAP_H
#define NV2A_SHIM_BITMAP_H

#include <stdlib.h>
#include <limits.h>
#define BITS_PER_LONG (sizeof(unsigned long) * CHAR_BIT)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static inline unsigned long *bitmap_new(long nbits)
{ return (unsigned long *)calloc(BITS_TO_LONGS(nbits), sizeof(unsigned long)); }
static inline void set_bit(long nr, unsigned long *a) { a[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG); }
static inline void clear_bit(long nr, unsigned long *a) { a[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG)); }
static inline int test_bit(long nr, const unsigned long *a) { return (a[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1; }
static inline void bitmap_set(unsigned long *a, long start, long n) { for (long i = 0; i < n; i++) set_bit(start + i, a); }
static inline void bitmap_clear(unsigned long *a, long start, long n) { for (long i = 0; i < n; i++) clear_bit(start + i, a); }

#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off)
{ for (; off < size; off++) if (test_bit(off, a)) return off; return size; }
static inline unsigned long find_first_bit(const unsigned long *a, unsigned long size) { return find_next_bit(a, size, 0); }
static inline unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off)
{ for (; off < size; off++) if (!test_bit(off, a)) return off; return size; }
#endif
