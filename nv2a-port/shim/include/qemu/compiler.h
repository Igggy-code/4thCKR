#ifndef NV2A_SHIM_COMPILER_H
#define NV2A_SHIM_COMPILER_H
#define QEMU_BUILD_BUG_MSG(x, msg) _Static_assert(!(x), msg)
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), "not expecting: " #x)
#define QEMU_PACKED __attribute__((packed))
#define QEMU_NORETURN __attribute__((noreturn))
#define QEMU_FALLTHROUGH __attribute__((fallthrough))
#define G_NORETURN __attribute__((noreturn))
#define glue(a, b) xglue(a, b)
#define xglue(a, b) a##b
#define stringify(s) tostring(s)
#define tostring(s) #s
#endif
