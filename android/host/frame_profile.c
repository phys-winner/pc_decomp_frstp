#include "frame_profile.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#if defined(__ANDROID__)
extern void frostpunk_android_presented(void) __attribute__((weak));
static unsigned char notified;
#endif

#define PROFILE_SLOTS 16
#define MAX_BINS_PER_FLUSH 4096
#define SECOND_NS UINT64_C(1000000000)
#define VK_SUCCESS_VALUE 0
#define VK_NOT_READY_VALUE 1
#define VK_SUBOPTIMAL_VALUE 1000001003

typedef struct {
    const void *presenter;
    uint64_t generation, start, last, last_id, first_id, bin_last_id;
    uint64_t count, total, maximum;
    int invalid;
} profile;

static profile profiles[PROFILE_SLOTS];
static uint64_t generation;
static unsigned char lock;
static int capacity_reported;

static void acquire(void) {
    while (__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE)) { }
}
static void release(void) {
    __atomic_clear(&lock, __ATOMIC_RELEASE);
}

static void sample(profile *p, uint64_t end, int complete) {
    fprintf(stderr, "PRESENT_SAMPLE method=accepted_unique_vkQueuePresent presenter=%p generation=%" PRIu64
        " start_ns=%" PRIu64 " end_ns=%" PRIu64 " duration_ns=%" PRIu64
        " count=%" PRIu64 " max_interval_ns=%" PRIu64 " idle_ns=%" PRIu64
        " cumulative=%" PRIu64 " first_frame_id=%" PRIu64 " last_frame_id=%" PRIu64
        " complete=%d compositor_delivery_proven=0\n",
        p->presenter, p->generation, p->start, end, end-p->start, p->count,
        p->maximum, end > p->last ? end-p->last : 0, p->total,
        p->first_id, p->bin_last_id, complete);
}

static void invalidate(profile *p, const char *reason) {
    if (!p->invalid)
        fprintf(stderr, "PRESENT_PROFILE_INVALID presenter=%p generation=%" PRIu64 " reason=%s\n",
            p->presenter, p->generation, reason);
    p->invalid = 1;
}

static int flush(profile *p, uint64_t now, int final) {
    if (p->invalid) return 0;
    if (!now || now < p->last || now < p->start) {
        invalidate(p, "clock");
        return 0;
    }
    if ((now-p->start)/SECOND_NS > MAX_BINS_PER_FLUSH) {
        invalidate(p, "unobserved_gap");
        return 0;
    }
    /* Each emitted full bin has exactly one second, including empty bins.
     * max_interval is an adjacent-success gap ending in this bin; idle_ns
     * separately exposes an unfinished gap and an idle final observation. */
    while (now-p->start >= SECOND_NS) {
        sample(p, p->start+SECOND_NS, 1);
        p->start += SECOND_NS;
        p->count = p->maximum = p->first_id = p->bin_last_id = 0;
    }
    if (final && (now != p->start || p->count)) sample(p, now, 0);
    return 1;
}

void frostpunk_frame_profile_present(const void *presenter, uint64_t frame_id,
                                    uint64_t now, int32_t result) {
    int saved_errno = errno;
    if (!presenter) return;
    acquire();
    profile *p = NULL, *empty = NULL;
    for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
        if (profiles[i].presenter == presenter) p = &profiles[i];
        if (!profiles[i].presenter && !empty) empty = &profiles[i];
    }
    if (!frame_id && result == VK_NOT_READY_VALUE) {
        if (p) { flush(p, now, 1); memset(p, 0, sizeof(*p)); }
    } else if (frame_id && (result == VK_SUCCESS_VALUE || result == VK_SUBOPTIMAL_VALUE)) {
        if (!p && now && empty && generation != UINT64_MAX) {
            p = empty;
            *p = (profile){ .presenter=presenter, .generation=++generation, .start=now, .last=now };
        }
        if (!p) {
            if (!capacity_reported) {
                fprintf(stderr, "PRESENT_PROFILE_INVALID reason=capacity_or_initial_clock max_presenters=%d\n", PROFILE_SLOTS);
                capacity_reported = 1;
            }
        } else if (!p->invalid && frame_id != p->last_id) {
            if (frame_id < p->last_id) invalidate(p, "frame_id_regression");
            else if (flush(p, now, 0)) {
                uint64_t gap = p->last_id ? now-p->last : 0;
                if (gap > p->maximum) p->maximum = gap;
                if (!p->count) p->first_id = frame_id;
                ++p->count; ++p->total;
                p->last = now; p->last_id = p->bin_last_id = frame_id;
            }
        }
    }
    release();
#if defined(__ANDROID__)
    if (frame_id && (result == VK_SUCCESS_VALUE || result == VK_SUBOPTIMAL_VALUE)
            && !__atomic_test_and_set(&notified, __ATOMIC_RELAXED) && frostpunk_android_presented)
        frostpunk_android_presented();
#endif
    errno = saved_errno;
}

void fp_frame_profile_flush(uint64_t now) {
    int saved_errno = errno;
    acquire();
    for (unsigned i = 0; i < PROFILE_SLOTS; ++i)
        if (profiles[i].presenter) flush(&profiles[i], now, 1);
    release();
    errno = saved_errno;
}

static uint64_t monotonic_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)
            || counter.QuadPart < 0 || frequency.QuadPart <= 0) return 0;
    uint64_t q = (uint64_t)counter.QuadPart, f = (uint64_t)frequency.QuadPart;
    if (q/f > UINT64_MAX/SECOND_NS || q%f > UINT64_MAX/SECOND_NS) return 0;
    return q/f*SECOND_NS + q%f*SECOND_NS/f;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0
            || t.tv_nsec >= (long)SECOND_NS || (uint64_t)t.tv_sec > (UINT64_MAX-(uint64_t)t.tv_nsec)/SECOND_NS) return 0;
    return (uint64_t)t.tv_sec*SECOND_NS+(uint64_t)t.tv_nsec;
#endif
}

void fp_frame_profile_stop(void) {
    int saved_errno = errno;
    acquire();
    uint64_t now = monotonic_ns();
    for (unsigned i = 0; i < PROFILE_SLOTS; ++i) {
        if (profiles[i].presenter) flush(&profiles[i], now, 1);
        memset(&profiles[i], 0, sizeof(profiles[i]));
    }
    release();
    errno = saved_errno;
}
