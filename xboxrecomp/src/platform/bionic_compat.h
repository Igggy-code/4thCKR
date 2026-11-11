/*
 * Android (bionic) stand-ins for the glibc extensions win32_compat.c uses.
 *
 *   pthread_{get,set}affinity_np  -> sched_{get,set}affinity on the thread's tid
 *   pthread_cancel                -> a private real-time signal whose handler
 *                                    calls pthread_exit (TerminateThread only)
 *   explicit_bzero                -> volatile memset
 *   memfd_create                  -> the raw syscall below API 30
 */
#ifndef XBOXRECOMP_BIONIC_COMPAT_H
#define XBOXRECOMP_BIONIC_COMPAT_H
#if defined(__ANDROID__)

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static inline int bionic_pthread_setaffinity_np(pthread_t th, size_t sz, const cpu_set_t *set)
{
    return sched_setaffinity(pthread_gettid_np(th), sz, set) == 0 ? 0 : -1;
}
static inline int bionic_pthread_getaffinity_np(pthread_t th, size_t sz, cpu_set_t *set)
{
    return sched_getaffinity(pthread_gettid_np(th), sz, set) == 0 ? 0 : -1;
}
#define pthread_setaffinity_np bionic_pthread_setaffinity_np
#define pthread_getaffinity_np bionic_pthread_getaffinity_np

#define BIONIC_CANCEL_SIGNAL (SIGRTMIN + 10)
static void bionic_cancel_handler(int sig) { (void)sig; pthread_exit(NULL); }
static inline int bionic_pthread_cancel(pthread_t th)
{
    static int installed;
    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = bionic_cancel_handler;
        sigaction(BIONIC_CANCEL_SIGNAL, &sa, NULL);
        installed = 1;
    }
    return pthread_kill(th, BIONIC_CANCEL_SIGNAL);
}
#define pthread_cancel bionic_pthread_cancel

static inline void bionic_explicit_bzero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}
#define explicit_bzero bionic_explicit_bzero

#if __ANDROID_API__ < 30
static inline int bionic_memfd_create(const char *name, unsigned int flags)
{
    return (int)syscall(__NR_memfd_create, name, flags);
}
#define memfd_create bionic_memfd_create
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#endif /* __ANDROID__ */
#endif
