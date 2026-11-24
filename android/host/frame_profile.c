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
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
extern void frostpunk_android_presented(void) __attribute__((weak));
extern void frostpunk_android_frame(uint64_t monotonic_ns) __attribute__((weak));
static unsigned char notified;
static void render_accept(int32_t result, uint64_t gap, uint64_t now);
static void render_take(char notes[2][160], int *note_count, char *summary, size_t summary_n, int *have_summary, uint64_t now);
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
#if defined(__ANDROID__)
    char notes[2][160];
    char summary[192];
    int note_count = 0, have_summary = 0;
#endif
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
#if defined(__ANDROID__)
                render_accept(result, gap, now);
                /* The launcher's FPS overlay: one timestamp per presented unique frame. */
                if (frostpunk_android_frame) frostpunk_android_frame(now);
#endif
            }
        }
    }
#if defined(__ANDROID__)
    render_take(notes, &note_count, summary, sizeof summary, &have_summary, now);
#endif
    release();
#if defined(__ANDROID__)
    for (int i = 0; i < note_count; ++i) frostpunk_render_log(notes[i]);
    if (have_summary) frostpunk_render_log(summary);
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

#if defined(__ANDROID__)
#define RENDER_CAP ((off_t)16 * 1024 * 1024)
#define RENDER_KEEP ((off_t)8 * 1024 * 1024)
static pthread_mutex_t render_mu = PTHREAD_MUTEX_INITIALIZER;
static char render_keys[8][80];
static uint64_t render_key_ns[8];
static uint64_t render_ok, render_sub, render_other, render_max_gap_ms;
static uint64_t render_sent_ok, render_sent_sub, render_sent_other, render_sent_gap, render_summary_ns, render_stall_ns;
static int32_t render_last_result;
static unsigned char render_seen;
static char render_pending[2][160];
static int render_pending_n, render_summary_due;

static int render_recent(const char *key, uint64_t now) {
    unsigned free_slot = 8;
    for (unsigned i = 0; i < 8; ++i) {
        if (render_keys[i][0] && !strcmp(render_keys[i], key)) {
            if (now && now - render_key_ns[i] < SECOND_NS) return 1;
            render_key_ns[i] = now;
            return 0;
        }
        if (!render_keys[i][0] && free_slot == 8) free_slot = i;
    }
    if (free_slot == 8) {
        memmove(render_keys, render_keys[1], 7 * 80);
        memmove(render_key_ns, render_key_ns + 1, 7 * sizeof(render_key_ns[0]));
        free_slot = 7;
    }
    snprintf(render_keys[free_slot], sizeof(render_keys[0]), "%s", key);
    render_key_ns[free_slot] = now;
    return 0;
}

/* Rewrites the file down to the last 8 MB. Closes fd when the replacement is in place. */
static int render_trim(int fd, const char *path) {
    struct stat st;
    char chunk[65536], temporary[PATH_MAX], marker[] = "render log trimmed\n";
    off_t start, pos;
    ssize_t n;
    size_t skip = 0;
    int out;
    if (fstat(fd, &st) || st.st_size <= RENDER_CAP) return 0;
    start = st.st_size - RENDER_KEEP;
    n = pread(fd, chunk, sizeof(chunk), start);
    if (n <= 0) return -1;
    while (skip < (size_t)n && chunk[skip] != '\n') ++skip;
    if (skip < (size_t)n) ++skip;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return -1;
    out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) return -1;
    if (write(out, marker, sizeof(marker) - 1) < 0) { close(out); unlink(temporary); return -1; }
    if (skip < (size_t)n && write(out, chunk + skip, (size_t)n - skip) < 0) { close(out); unlink(temporary); return -1; }
    pos = start + (off_t)n;
    while (pos < st.st_size) {
        n = pread(fd, chunk, sizeof(chunk), pos);
        if (n <= 0) { close(out); unlink(temporary); return -1; }
        if (write(out, chunk, (size_t)n) != n) { close(out); unlink(temporary); return -1; }
        pos += n;
    }
    if (close(out)) { unlink(temporary); return -1; }
    close(fd);
    if (rename(temporary, path)) { unlink(temporary); return -1; }
    return 1;
}

static const char *render_result_name(int32_t result) {
    if (result == VK_SUCCESS_VALUE) return "SUCCESS";
    if (result == VK_NOT_READY_VALUE) return "NOT_READY";
    if (result == 2) return "TIMEOUT";
    if (result == VK_SUBOPTIMAL_VALUE) return "SUBOPTIMAL";
    if (result == -4) return "DEVICE_LOST";
    if (result == -1000001004) return "OUT_OF_DATE";
    if (result == -1000000000) return "SURFACE_LOST";
    return "OTHER";
}

/* Hang watchdog (external-theories T10 a, after spore-re's watchdog.cpp): when no frame has been
 * presented for debug.fp.watchdog seconds (default 10, 0 = off) while the game is in the foreground,
 * native.log receives FP_HANG with every thread's state (comm, R/S/D, wchan, CPU ticks) and, through
 * SIGURG, each thread's own backtrace as library+offset.  One report per stall. */
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unwind.h>
extern int frostpunk_android_foreground(void) __attribute__((weak));
static uint64_t watchdog_last_present;
static unsigned char watchdog_started;

typedef struct { uintptr_t pcs[24]; int count; } watchdog_trace;
static _Unwind_Reason_Code watchdog_step(struct _Unwind_Context *context, void *opaque) {
    watchdog_trace *trace = opaque;
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc && trace->count < 24) trace->pcs[trace->count++] = pc;
    return trace->count < 24 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static void watchdog_signal(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info; (void)context;
    int saved_errno = errno;
    watchdog_trace trace = {{0}, 0};
    _Unwind_Backtrace(watchdog_step, &trace);
    /* One write per thread keeps its frames together in the shared log. */
    char out[3072];size_t used = 0;
    int n = snprintf(out, sizeof out, "FP_HANG_STACK tid=%ld frames=%d\n", (long)syscall(SYS_gettid), trace.count);
    if (n > 0) used = (size_t)n < sizeof out ? (size_t)n : sizeof out - 1;
    for (int i = 1; i < trace.count && used < sizeof out - 1; ++i) {   /* #0 is this handler */
        Dl_info where;
        if (dladdr((void *)trace.pcs[i], &where) && where.dli_fname) {
            const char *name = strrchr(where.dli_fname, '/');
            n = snprintf(out + used, sizeof out - used, "FP_HANG_STACK   #%02d %s+0x%lx%s%s\n", i, name ? name + 1 : where.dli_fname,
                (unsigned long)(trace.pcs[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? " " : "",
                where.dli_sname ? where.dli_sname : "");
        } else n = snprintf(out + used, sizeof out - used, "FP_HANG_STACK   #%02d 0x%lx\n", i, (unsigned long)trace.pcs[i]);
        if (n <= 0) break;
        used += (size_t)n < sizeof out - used ? (size_t)n : sizeof out - used - 1;
    }
    if (used) (void)!write(STDERR_FILENO, out, used);
    errno = saved_errno;
}
static unsigned watchdog_seconds(void) {
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.fp.watchdog", value) <= 0) return 10;
    long seconds = strtol(value, NULL, 10);
    return seconds < 0 ? 0 : seconds > 600 ? 600 : (unsigned)seconds;
}
static void watchdog_report(uint64_t stall_ms) {
    fprintf(stderr, "FP_HANG no_present_ms=%llu\n", (unsigned long long)stall_ms);
    pid_t self = (pid_t)syscall(SYS_gettid), pid = getpid();
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks) return;
    struct dirent *entry;unsigned signalled = 0;
    while ((entry = readdir(tasks))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        long tid = strtol(entry->d_name, NULL, 10);
        char path[64], stat[512] = {0}, wchan[64] = {0};
        snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
        FILE *file = fopen(path, "re");
        if (file) { if (!fgets(stat, sizeof stat, file)) stat[0] = 0; fclose(file); }
        snprintf(path, sizeof path, "/proc/self/task/%ld/wchan", tid);
        file = fopen(path, "re");
        if (file) { if (!fgets(wchan, sizeof wchan, file)) wchan[0] = 0; fclose(file); }
        /* stat: "tid (comm) S ppid ... utime(14) stime(15)" */
        const char *open = strchr(stat, '('), *close = strrchr(stat, ')');
        char comm[32] = "?", state = '?';unsigned long long utime = 0, stime = 0;
        if (open && close && close > open) {
            size_t length = (size_t)(close - open - 1); if (length >= sizeof comm) length = sizeof comm - 1;
            memcpy(comm, open + 1, length); comm[length] = 0;
            state = close[2];
            const char *field = close + 2;int index = 3;
            while (*field && index < 14) { if (*field == ' ') ++index; ++field; }
            sscanf(field, "%llu %llu", &utime, &stime);
        }
        fprintf(stderr, "FP_HANG_THREAD tid=%ld name=%s state=%c wchan=%s cpu_ticks=%llu+%llu\n", tid, comm, state,
            wchan[0] ? wchan : "-", utime, stime);
        if (tid != self && signalled < 64) { syscall(SYS_tgkill, pid, (pid_t)tid, SIGURG); ++signalled; }
    }
    closedir(tasks);
    fflush(stderr);
}
static void *watchdog_main(void *unused) {
    (void)unused;
    uint64_t reported = 0;
    for (;;) {
        sleep(1);
        unsigned seconds = watchdog_seconds();
        uint64_t last = __atomic_load_n(&watchdog_last_present, __ATOMIC_ACQUIRE), now = monotonic_ns();
        if (!seconds || !last || !now || now < last) continue;
        if (frostpunk_android_foreground && !frostpunk_android_foreground()) continue;
        if (now - last >= (uint64_t)seconds * SECOND_NS && reported != last) {
            reported = last;
            watchdog_report((now - last) / 1000000ull);
        }
    }
    return NULL;
}
static void watchdog_present(uint64_t now) {
    __atomic_store_n(&watchdog_last_present, now, __ATOMIC_RELEASE);
    if (__atomic_test_and_set(&watchdog_started, __ATOMIC_ACQ_REL)) return;
    struct sigaction action;memset(&action, 0, sizeof action);
    action.sa_sigaction = watchdog_signal; action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGURG, &action, NULL)) return;
    pthread_t thread;pthread_attr_t attributes;pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&thread, &attributes, watchdog_main, NULL)) pthread_setname_np(thread, "fp-watchdog");
    pthread_attr_destroy(&attributes);
}

static void render_accept(int32_t result, uint64_t gap, uint64_t now) {
    watchdog_present(now);
    uint64_t gap_ms = gap / 1000000ull;
    if (result == VK_SUBOPTIMAL_VALUE) ++render_sub;
    else ++render_ok;
    render_last_result = result;
    if (gap_ms > render_max_gap_ms) render_max_gap_ms = gap_ms;
    if (!render_seen && render_pending_n < 2) {
        snprintf(render_pending[render_pending_n++], sizeof(render_pending[0]), "first present result=%d", (int)result);
        render_seen = 1;
    }
    if (gap >= 400000000ull && (!render_stall_ns || now >= render_stall_ns + SECOND_NS) && render_pending_n < 2) {
        render_stall_ns = now;
        snprintf(render_pending[render_pending_n++], sizeof(render_pending[0]), "present stall gap_ms=%llu", (unsigned long long)gap_ms);
    }
    if (!render_summary_ns) render_summary_ns = now;
    if (now && now >= render_summary_ns + 10ull * SECOND_NS &&
        (render_ok != render_sent_ok || render_sub != render_sent_sub || render_other != render_sent_other || render_max_gap_ms != render_sent_gap))
        render_summary_due = 1;
}

static void render_take(char notes[2][160], int *note_count, char *summary, size_t summary_n, int *have_summary, uint64_t now) {
    int count = render_pending_n > 2 ? 2 : render_pending_n;
    for (int i = 0; i < count; ++i) memcpy(notes[i], render_pending[i], sizeof(render_pending[0]));
    render_pending_n = 0;
    *note_count = count;
    *have_summary = 0;
    if (!render_summary_due) return;
    render_summary_due = 0;
    if (now) render_summary_ns = now;
    render_sent_ok = render_ok;
    render_sent_sub = render_sub;
    render_sent_other = render_other;
    render_sent_gap = render_max_gap_ms;
    snprintf(summary, summary_n, "render summary ok=%llu suboptimal=%llu other=%llu last=%d %s max_gap_ms=%llu",
        (unsigned long long)render_ok, (unsigned long long)render_sub, (unsigned long long)render_other,
        (int)render_last_result, render_result_name(render_last_result), (unsigned long long)render_max_gap_ms);
    *have_summary = 1;
}

void frostpunk_render_log(const char *line) {
    const char *path;
    char text[512];
    uint64_t now;
    int fd, trimmed, wrote;
    if (!line || !line[0]) return;
    path = getenv("FROSTPUNK_RENDER_LOG");
    if (!path || !path[0]) return;
    now = monotonic_ns();
    pthread_mutex_lock(&render_mu);
    if (render_recent(line, now)) { pthread_mutex_unlock(&render_mu); return; }
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) { pthread_mutex_unlock(&render_mu); return; }
    trimmed = render_trim(fd, path);
    if (trimmed > 0) fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    else if (trimmed < 0) { close(fd); fd = -1; }
    if (fd >= 0) {
        wrote = snprintf(text, sizeof(text), "%llu %s\n", (unsigned long long)(now / 1000000ull), line);
        if (wrote > 0) {
            if (wrote >= (int)sizeof(text)) wrote = (int)sizeof(text) - 1;
            if (lseek(fd, 0, SEEK_END) >= 0) {
                ssize_t n = write(fd, text, (size_t)wrote);
                (void)n;
            }
        }
        close(fd);
    }
    pthread_mutex_unlock(&render_mu);
}

void frostpunk_render_present_result(int32_t result) {
    char line[96], summary[192];
    int have_summary = 0;
    uint64_t now = monotonic_ns();
    if (result == VK_SUCCESS_VALUE || result == VK_SUBOPTIMAL_VALUE) return;
    snprintf(line, sizeof(line), "present result=%d %s", (int)result, render_result_name(result));
    acquire();
    ++render_other;
    render_last_result = result;
    if (!render_summary_ns) render_summary_ns = now;
    if (now && now >= render_summary_ns + 10ull * SECOND_NS &&
        (render_ok != render_sent_ok || render_sub != render_sent_sub || render_other != render_sent_other || render_max_gap_ms != render_sent_gap)) {
        render_summary_ns = now;
        render_sent_ok = render_ok;
        render_sent_sub = render_sub;
        render_sent_other = render_other;
        render_sent_gap = render_max_gap_ms;
        render_summary_due = 0;
        snprintf(summary, sizeof(summary), "render summary ok=%llu suboptimal=%llu other=%llu last=%d %s max_gap_ms=%llu",
            (unsigned long long)render_ok, (unsigned long long)render_sub, (unsigned long long)render_other,
            (int)render_last_result, render_result_name(result), (unsigned long long)render_max_gap_ms);
        have_summary = 1;
    }
    release();
    frostpunk_render_log(line);
    if (have_summary) frostpunk_render_log(summary);
}
#else
void frostpunk_render_log(const char *line) { (void)line; }
void frostpunk_render_present_result(int32_t result) { (void)result; }
#endif
