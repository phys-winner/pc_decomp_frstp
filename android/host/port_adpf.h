/* CPU producer hints. Define PORT_ADPF_IMPLEMENTATION in one producer translation unit.
 * end() runs before Present; begin(fps) after Present and its limiter return.
 * block(1, paused) / block(2, surface_missing) invalidate cross-lifecycle samples.
 * Android entry points are resolved dynamically, preserving the port's minSdk.
 */
#ifndef PORT_ADPF_H
#define PORT_ADPF_H
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__ANDROID__) || defined(PORT_ADPF_TEST)
__attribute__((visibility("default"))) void port_adpf_begin(double fps);
__attribute__((visibility("default"))) void port_adpf_end(void);
__attribute__((visibility("default"))) void port_adpf_block(unsigned reason, int blocked);
#else
static inline void port_adpf_begin(double fps) { (void)fps; }
static inline void port_adpf_end(void) {}
static inline void port_adpf_block(unsigned reason, int blocked) { (void)reason; (void)blocked; }
#endif
#ifdef __cplusplus
}
#endif

#if defined(PORT_ADPF_IMPLEMENTATION) && (defined(__ANDROID__) || defined(PORT_ADPF_TEST))
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#ifndef PORT_ADPF_TEST
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef PORT_ADPF_LOG
/* libfrostpunk_runtime.so links without liblog in every Frostpunk build route (fastbuild, the capped
 * release wrapper, build_startup): its stderr is native.log, where the other FP_* lines go too. */
#include <stdio.h>
#define PORT_ADPF_LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif
#endif
#ifdef __cplusplus
#define PORT_ADPF_TLS thread_local
#else
#define PORT_ADPF_TLS _Thread_local
#endif

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
static struct {
    APerformanceHintManager *(*manager)(void);
    int64_t (*rate)(APerformanceHintManager *);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*update)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    void (*close)(APerformanceHintSession *);
    APerformanceHintManager *instance;
    int ready;
} port_adpf_api;
static pthread_once_t port_adpf_once = PTHREAD_ONCE_INIT;
static pthread_key_t port_adpf_key;
/* Low two bits are independent blockers; upper bits retain every transition. */
static uint64_t port_adpf_epoch;
static PORT_ADPF_TLS struct {
    APerformanceHintSession *session;
    uint64_t epoch;
    int64_t start, target;
    int failed;
} port_adpf_thread;

static void port_adpf_destroy(void *session) {
    if (session) port_adpf_api.close((APerformanceHintSession *)session);
}
static void port_adpf_load(void) {
    const char *option = getenv("PORT_OPTION_ADPF");
    if (!option || (strcmp(option, "1") && strcmp(option, "true"))) {
        PORT_ADPF_LOG("[adpf] off (launcher option %s)", option ? option : "unset");
        return;
    }
    /* Keep the DSO alive for session destructors. */
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { PORT_ADPF_LOG("[adpf] unavailable: library"); return; }
#define PORT_ADPF_SYMBOL(field, name) *(void **)(&port_adpf_api.field) = dlsym(library, name)
    PORT_ADPF_SYMBOL(manager, "APerformanceHint_getManager");
    PORT_ADPF_SYMBOL(rate, "APerformanceHint_getPreferredUpdateRateNanos");
    PORT_ADPF_SYMBOL(create, "APerformanceHint_createSession");
    PORT_ADPF_SYMBOL(update, "APerformanceHint_updateTargetWorkDuration");
    PORT_ADPF_SYMBOL(report, "APerformanceHint_reportActualWorkDuration");
    PORT_ADPF_SYMBOL(close, "APerformanceHint_closeSession");
#undef PORT_ADPF_SYMBOL
    if (!port_adpf_api.manager || !port_adpf_api.create || !port_adpf_api.report || !port_adpf_api.close) {
        PORT_ADPF_LOG("[adpf] unavailable: API"); return;
    }
    port_adpf_api.instance = port_adpf_api.manager();
    if (!port_adpf_api.instance || (port_adpf_api.rate && port_adpf_api.rate(port_adpf_api.instance) < 0)) {
        PORT_ADPF_LOG("[adpf] unavailable: hint sessions"); return;
    }
    if (pthread_key_create(&port_adpf_key, port_adpf_destroy)) {
        PORT_ADPF_LOG("[adpf] unavailable: TLS key"); return;
    }
    port_adpf_api.ready = 1;
}
static int64_t port_adpf_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000 || t.tv_sec > (INT64_MAX - t.tv_nsec) / INT64_C(1000000000)) return 0;
    return (int64_t)t.tv_sec * INT64_C(1000000000) + t.tv_nsec;
}
void port_adpf_block(unsigned reason, int blocked) {
    uint64_t before = __atomic_load_n(&port_adpf_epoch, __ATOMIC_ACQUIRE), after;
    reason &= 3u;
    do {
        after = ((before & ~UINT64_C(3)) + 4) | ((before & 3u) & ~reason) | (blocked ? reason : 0u);
    } while (!__atomic_compare_exchange_n(&port_adpf_epoch, &before, after, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}
static void port_adpf_close(void) {
    if (port_adpf_thread.session) {
        pthread_setspecific(port_adpf_key, NULL);
        port_adpf_api.close(port_adpf_thread.session);
        port_adpf_thread.session = NULL;
    }
    port_adpf_thread.start = 0;
}
void port_adpf_begin(double fps) {
    int saved_errno = errno;
    port_adpf_thread.start = 0;
    uint64_t epoch = __atomic_load_n(&port_adpf_epoch, __ATOMIC_ACQUIRE);
    if (epoch & 3u) { port_adpf_close(); goto done; }
    if (port_adpf_thread.failed) goto done;
    pthread_once(&port_adpf_once, port_adpf_load);
    if (!port_adpf_api.ready) goto done;
    {
        /* 75% leaves room for spikes instead of teaching the HAL to fill the entire period. */
        int64_t target = (int64_t)(750000000.0 / ((fps >= 1 && fps <= 1000) ? fps : 60.0));
        if (!port_adpf_thread.session) {
            int32_t tid = (int32_t)syscall(SYS_gettid);
            port_adpf_thread.session = tid > 0 ? port_adpf_api.create(port_adpf_api.instance, &tid, 1, target) : NULL;
            if (!port_adpf_thread.session) {
                port_adpf_thread.failed = 1;
                PORT_ADPF_LOG("[adpf] session unavailable"); goto done;
            }
            if (pthread_setspecific(port_adpf_key, port_adpf_thread.session)) {
                port_adpf_close(); port_adpf_thread.failed = 1; goto done;
            }
            port_adpf_thread.target = target;
            PORT_ADPF_LOG("[adpf] session tid %d target %lld ns", tid, (long long)target);
            goto done; /* Session setup is not a frame sample. */
        }
        if (target != port_adpf_thread.target) {
            if (!port_adpf_api.update || port_adpf_api.update(port_adpf_thread.session, target)) {
                port_adpf_close(); port_adpf_thread.failed = 1; goto done;
            }
            port_adpf_thread.target = target;
        }
    }
    port_adpf_thread.epoch = epoch;
    port_adpf_thread.start = port_adpf_now();
done:
    errno = saved_errno;
}
void port_adpf_end(void) {
    int saved_errno = errno;
    int64_t start = port_adpf_thread.start;
    port_adpf_thread.start = 0;
    if (start) {
        uint64_t epoch = __atomic_load_n(&port_adpf_epoch, __ATOMIC_ACQUIRE);
        int64_t end = port_adpf_now();
        if (epoch == port_adpf_thread.epoch && !(epoch & 3u) && end > start) {
            if (port_adpf_api.report(port_adpf_thread.session, end - start)) {
                port_adpf_close(); port_adpf_thread.failed = 1;
                PORT_ADPF_LOG("[adpf] report failed; session closed");
            }
        } else if (epoch & 3u) port_adpf_close();
    }
    errno = saved_errno;
}
#undef PORT_ADPF_TLS
#endif
#endif
