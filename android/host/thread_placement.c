/* Thread placement on asymmetric CPUs.
 *
 * The guest main thread is the frame's critical path: in a developed city it is busy for the whole
 * frame.  On a phone with one prime core, several middle cores and little cores the scheduler kept
 * it on the prime core only about 80 % of the time (Snapdragon 8s Gen 3: the rest on a Cortex-A720,
 * which runs this code about a quarter slower), and the render thread shared the middle cores.
 *
 * Once the picture runs, the main thread is restricted to the CPUs of the highest capacity and the
 * render thread to the other big cores, so the two never compete for the prime core.  Nothing is
 * changed on a symmetric CPU or when the kernel does not report capacities.  The guest sees the
 * same processor count as before: win64_system.c reads it from /sys/devices/system/cpu/online.
 *
 * Off by default: measured on 2026-11-14 it relieved the main thread (96 % -> 94 % busy on the
 * Snapdragon phone) but took the prime core's spare time from the render thread (84 % -> 92 %), which
 * is the tighter of the two there, and the frame rate did not move.  debug.fp.pin=1 turns it on.
 */
#define _GNU_SOURCE
#include "thread_placement.h"
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

static _Atomic pid_t main_thread;
static _Atomic unsigned frames;
static _Atomic int reported;

void fp_thread_placement_main(void) { atomic_store_explicit(&main_thread, gettid(), memory_order_relaxed); }

static int placement_enabled(void) {
    static _Atomic int mode = -1;
    int value = atomic_load_explicit(&mode, memory_order_relaxed);
    if (value < 0) {
        value = 0;
#ifdef __ANDROID__
        char text[PROP_VALUE_MAX] = {0}; int saved = errno;
        if (__system_property_get("debug.fp.pin", text) > 0) value = text[0] != '0';
        errno = saved;
#endif
        atomic_store_explicit(&mode, value, memory_order_relaxed);
    }
    return value;
}

/* Masks of the CPUs with the highest capacity and of all big CPUs (at least 60 % of it). */
static int capacity_masks(uint64_t *prime, uint64_t *big) {
    unsigned capacity[64] = {0}, highest = 0, count = 0;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpu_capacity", cpu);
        FILE *file = fopen(path, "re");
        if (!file) break;
        if (fscanf(file, "%u", &capacity[cpu]) != 1) capacity[cpu] = 0;
        fclose(file);
        if (capacity[cpu] > highest) highest = capacity[cpu];
        ++count;
    }
    *prime = *big = 0;
    if (!count || !highest) return 0;
    for (unsigned cpu = 0; cpu < count; ++cpu) {
        if (capacity[cpu] == highest) *prime |= UINT64_C(1) << cpu;
        if (capacity[cpu] * 10u >= highest * 6u) *big |= UINT64_C(1) << cpu;
    }
    return 1;
}

static int restrict_thread(pid_t tid, uint64_t mask) {
    cpu_set_t set; CPU_ZERO(&set);
    for (unsigned cpu = 0; cpu < 64; ++cpu) if (mask >> cpu & 1) CPU_SET(cpu, &set);
    return sched_setaffinity(tid, sizeof(set), &set) ? errno : 0;
}

void frostpunk_thread_placement_frame(void) {
    unsigned frame = atomic_fetch_add_explicit(&frames, 1, memory_order_relaxed) + 1;
    /* Four seconds into the picture, then once a minute against changes from outside. */
    if (frame != 240 && frame % 3600) return;
    int saved = errno;
    pid_t main_tid = atomic_load_explicit(&main_thread, memory_order_relaxed), render_tid = gettid();
    uint64_t prime = 0, big = 0;
    if (placement_enabled() && main_tid && main_tid != render_tid && capacity_masks(&prime, &big) && prime && (big & ~prime)) {
        int main_status = restrict_thread(main_tid, prime), render_status = restrict_thread(render_tid, big & ~prime);
        if (!atomic_exchange_explicit(&reported, 1, memory_order_relaxed))
            fprintf(stderr, "FP_THREAD_PLACEMENT main_tid=%d main_mask=0x%" PRIx64 " status=%d render_tid=%d render_mask=0x%" PRIx64 " status=%d\n",
                    (int)main_tid, prime, main_status, (int)render_tid, big & ~prime, render_status);
    } else if (!atomic_exchange_explicit(&reported, 1, memory_order_relaxed)) {
        fprintf(stderr, "FP_THREAD_PLACEMENT unchanged enabled=%d main_tid=%d prime_mask=0x%" PRIx64 " big_mask=0x%" PRIx64 "\n",
                placement_enabled(), (int)main_tid, prime, big);
    }
    errno = saved;
}
