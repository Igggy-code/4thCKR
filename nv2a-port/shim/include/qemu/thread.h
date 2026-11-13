#ifndef NV2A_SHIM_THREAD_H
#define NV2A_SHIM_THREAD_H
#include <pthread.h>
#include <stdbool.h>
typedef struct QemuMutex { pthread_mutex_t m; volatile int waiters; } QemuMutex;
typedef struct QemuCond { pthread_cond_t c; } QemuCond;
typedef struct QemuEvent { pthread_mutex_t m; pthread_cond_t c; bool set; } QemuEvent;
typedef struct QemuThread { pthread_t t; } QemuThread;
typedef struct QemuSpin { pthread_mutex_t m; } QemuSpin;
#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1
void qemu_mutex_init(QemuMutex *m);
void qemu_mutex_destroy(QemuMutex *m);
void qemu_mutex_lock(QemuMutex *m);
int qemu_mutex_trylock(QemuMutex *m);
void qemu_mutex_unlock(QemuMutex *m);
void qemu_cond_init(QemuCond *c);
void qemu_cond_destroy(QemuCond *c);
void qemu_cond_signal(QemuCond *c);
void qemu_cond_broadcast(QemuCond *c);
void qemu_cond_wait(QemuCond *c, QemuMutex *m);
bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms);
void qemu_event_init(QemuEvent *e, bool init);
void qemu_event_destroy(QemuEvent *e);
void qemu_event_set(QemuEvent *e);
void qemu_event_reset(QemuEvent *e);
void qemu_event_wait(QemuEvent *e);
void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode);
void *qemu_thread_join(QemuThread *t);
void qemu_spin_init(QemuSpin *s);
void qemu_spin_lock(QemuSpin *s);
void qemu_spin_unlock(QemuSpin *s);
#endif
