/*
 * Minimal GLib subset for building xemu's NV2A outside QEMU.
 * Only what hw/xbox/nv2a uses: memory, GString, GArray, GByteArray,
 * printf helpers and g_autofree/g_autoptr.
 */
#ifndef NV2A_SHIM_GLIB_H
#define NV2A_SHIM_GLIB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char gchar;
typedef int gint;
typedef unsigned int guint;
typedef bool gboolean;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef size_t gsize;
typedef ssize_t gssize;
typedef uint8_t guint8;
typedef uint16_t guint16;
typedef uint32_t guint32;
typedef uint64_t guint64;
typedef int64_t gint64;

#ifndef TRUE
#define TRUE true
#define FALSE false
#endif

#define G_GNUC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define G_GNUC_UNUSED __attribute__((unused))
#define G_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define G_LIKELY(x) __builtin_expect(!!(x), 1)
#define G_N_ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))
#define g_assert(x) do { if (!(x)) { fprintf(stderr, "g_assert failed %s:%d %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define g_assert_not_reached() do { fprintf(stderr, "unreachable %s:%d\n", __FILE__, __LINE__); abort(); } while (0)

void *g_malloc(size_t n);
void *g_malloc0(size_t n);
void *g_realloc(void *p, size_t n);
void g_free(void *p);
char *g_strdup(const char *s);
char *g_strdup_printf(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
char *g_strdup_vprintf(const char *fmt, va_list ap);
#define g_malloc_n(n, sz) g_malloc((size_t)(n) * (size_t)(sz))
#define g_malloc0_n(n, sz) g_malloc0((size_t)(n) * (size_t)(sz))
#define g_new(T, n) ((T *)g_malloc(sizeof(T) * (size_t)(n)))
#define g_new0(T, n) ((T *)g_malloc0(sizeof(T) * (size_t)(n)))
#define g_renew(T, p, n) ((T *)g_realloc((p), sizeof(T) * (size_t)(n)))

/* GString */
typedef struct GString {
    char *str;
    size_t len;
    size_t allocated_len;
} GString;
GString *g_string_new(const char *init);
GString *g_string_new_take(char *init);
GString *g_string_sized_new(size_t n);
char *g_string_free(GString *s, bool free_segment);
GString *g_string_append(GString *s, const char *val);
GString *g_string_append_len(GString *s, const char *val, ssize_t len);
GString *g_string_append_c(GString *s, char c);
void g_string_append_printf(GString *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void g_string_append_vprintf(GString *s, const char *fmt, va_list ap);
void g_string_printf(GString *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
GString *g_string_truncate(GString *s, size_t len);

/* GArray */
typedef struct GArray {
    char *data;
    unsigned int len;
    /* private */
    unsigned int cap;
    unsigned int elt;
    bool zero_term, clear;
} GArray;
GArray *g_array_new(bool zero_terminated, bool clear, unsigned int elt_size);
GArray *g_array_sized_new(bool zero_terminated, bool clear, unsigned int elt_size, unsigned int reserved);
GArray *g_array_append_vals(GArray *a, const void *data, unsigned int len);
GArray *g_array_set_size(GArray *a, unsigned int len);
char *g_array_free(GArray *a, bool free_segment);
#define g_array_append_val(a, v) g_array_append_vals((a), &(v), 1)
#define g_array_index(a, T, i) (((T *)(void *)(a)->data)[(i)])

/* GByteArray */
typedef struct GByteArray {
    uint8_t *data;
    unsigned int len;
} GByteArray;
GByteArray *g_byte_array_new(void);
GByteArray *g_byte_array_new_take(uint8_t *data, size_t len);
void g_byte_array_unref(GByteArray *a);
uint8_t *g_byte_array_free(GByteArray *a, bool free_segment);

/* g_autofree / g_autoptr */
static inline void g_autoptr_cleanup_generic_gfree(void *p)
{
    void **pp = (void **)p;
    g_free(*pp);
}
#define g_autofree __attribute__((cleanup(g_autoptr_cleanup_generic_gfree)))
#define _GLIB_AUTOPTR_FUNC_NAME(T) glib_autoptr_cleanup_##T
#define _GLIB_AUTOPTR_TYPENAME(T) T##_autoptr
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(T, func)                              \
    typedef T *_GLIB_AUTOPTR_TYPENAME(T);                                   \
    static inline void _GLIB_AUTOPTR_FUNC_NAME(T)(T **_ptr)                 \
    {                                                                       \
        if (*_ptr) (func)(*_ptr);                                           \
    }
#define g_autoptr(T) __attribute__((cleanup(_GLIB_AUTOPTR_FUNC_NAME(T)))) _GLIB_AUTOPTR_TYPENAME(T)

static inline void g_string_autoptr_free(GString *s) { g_string_free(s, true); }
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GString, g_string_autoptr_free)
static inline void g_byte_array_autoptr_free(GByteArray *a) { g_byte_array_unref(a); }
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GByteArray, g_byte_array_autoptr_free)
static inline void g_array_autoptr_free(GArray *a) { g_array_free(a, true); }
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GArray, g_array_autoptr_free)

#endif
