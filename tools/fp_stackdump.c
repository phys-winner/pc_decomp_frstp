/* fp_stackdump: native backtraces of every thread of a debuggable app without root.
 *
 *   adb push fp-stackdump /data/local/tmp/
 *   adb shell run-as PKG cp /data/local/tmp/fp-stackdump ./fp-stackdump
 *   adb shell run-as PKG ./fp-stackdump PID [frames]
 *
 * Runs with the app's uid (run-as), attaches to each thread with PTRACE_SEIZE +
 * PTRACE_INTERRUPT, prints pc/lr/sp and the frame-pointer chain as
 * "module+0xVADDR" (address minus the module's load base, i.e. the ELF virtual
 * address that llvm-symbolizer expects for objects linked at 0), then detaches.
 * Threads are stopped one at a time, for well under a millisecond each. */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/ptrace.h>

typedef struct { uint64_t start, end, offset; char path[256]; } map_t;
static map_t maps[8192];
static int map_count;
static pid_t target;

static void load_maps(void) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", target);
    FILE *f = fopen(path, "r");
    if (!f) { perror("maps"); exit(1); }
    while (fgets(line, sizeof(line), f) && map_count < 8192) {
        map_t *m = &maps[map_count];
        char perms[8] = {0}; unsigned long long s, e, o; int n = 0;
        if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %n", &s, &e, perms, &o, &n) < 4) continue;
        m->start = s; m->end = e; m->offset = o;
        char *p = line + n; size_t len = strcspn(p, "\n");
        if (len >= sizeof(m->path)) len = sizeof(m->path) - 1;
        memcpy(m->path, p, len); m->path[len] = 0;
        ++map_count;
    }
    fclose(f);
}

/* Load base of a file mapping: the lowest mapping of the same path with offset 0. */
static uint64_t base_of(const char *path) {
    uint64_t base = UINT64_MAX;
    for (int i = 0; i < map_count; ++i)
        if (!strcmp(maps[i].path, path) && maps[i].offset == 0 && maps[i].start < base) base = maps[i].start;
    return base;
}

static void describe(uint64_t a, char *out, size_t size) {
    for (int i = 0; i < map_count; ++i) {
        if (a >= maps[i].start && a < maps[i].end) {
            const char *name = maps[i].path[0] ? maps[i].path : "[anon]";
            uint64_t base = maps[i].path[0] == '/' ? base_of(maps[i].path) : UINT64_MAX;
            if (base != UINT64_MAX) snprintf(out, size, "%s+0x%llx", name, (unsigned long long)(a - base));
            else snprintf(out, size, "%s(0x%llx+0x%llx)", name, (unsigned long long)maps[i].start,
                          (unsigned long long)(a - maps[i].start));
            return;
        }
    }
    snprintf(out, size, "?");
}

static int readable(uint64_t a) {
    for (int i = 0; i < map_count; ++i) if (a >= maps[i].start && a + 16 <= maps[i].end) return 1;
    return 0;
}

static int peek(pid_t tid, uint64_t a, uint64_t *v) {
    struct iovec local = { v, 8 }, remote = { (void *)(uintptr_t)a, 8 };
    if (process_vm_readv(tid, &local, 1, &remote, 1, 0) == 8) return 1;
    errno = 0; long w = ptrace(PTRACE_PEEKDATA, tid, (void *)(uintptr_t)a, 0);
    if (errno) return 0;
    *v = (uint64_t)w; return 1;
}

static void dump_thread(pid_t tid, int frames) {
    char path[96], comm[64] = "?", wchan[64] = "?";
    snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", target, tid);
    FILE *f = fopen(path, "r"); if (f) { if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
    snprintf(path, sizeof(path), "/proc/%d/task/%d/wchan", target, tid);
    f = fopen(path, "r"); if (f) { if (fgets(wchan, sizeof(wchan), f)) wchan[strcspn(wchan, "\n")] = 0; fclose(f); }
    if (ptrace(PTRACE_SEIZE, tid, 0, 0)) { printf("THREAD %d %s seize failed errno=%d\n", tid, comm, errno); return; }
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0)) { printf("THREAD %d %s interrupt failed errno=%d\n", tid, comm, errno); ptrace(PTRACE_DETACH, tid, 0, 0); return; }
    int status = 0;
    if (waitpid(tid, &status, __WALL) != tid) { printf("THREAD %d %s wait failed errno=%d\n", tid, comm, errno); ptrace(PTRACE_DETACH, tid, 0, 0); return; }
    struct user_pt_regs regs; struct iovec io = { &regs, sizeof(regs) };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &io)) { printf("THREAD %d %s regs failed errno=%d\n", tid, comm, errno); ptrace(PTRACE_DETACH, tid, 0, 0); return; }
    char where[400];
    printf("THREAD %d \"%s\" wchan=%s x8=%llu x0=0x%llx x1=0x%llx x2=0x%llx x19=0x%llx x20=0x%llx sp=0x%llx\n", tid, comm, wchan,
           (unsigned long long)regs.regs[8], (unsigned long long)regs.regs[0], (unsigned long long)regs.regs[1],
           (unsigned long long)regs.regs[2], (unsigned long long)regs.regs[19], (unsigned long long)regs.regs[20],
           (unsigned long long)regs.sp);
    describe(regs.pc, where, sizeof(where)); printf("  #00 pc 0x%llx %s\n", (unsigned long long)regs.pc, where);
    describe(regs.regs[30], where, sizeof(where)); printf("  #01 lr 0x%llx %s\n", (unsigned long long)regs.regs[30], where);
    uint64_t fp = regs.regs[29];
    for (int i = 2; i < frames && fp && readable(fp); ++i) {
        uint64_t next = 0, ret = 0;
        if (!peek(tid, fp, &next) || !peek(tid, fp + 8, &ret)) break;
        ret &= 0x0000ffffffffffffull; /* strip PAC/TBI bits if any */
        describe(ret, where, sizeof(where));
        printf("  #%02d fp 0x%llx %s\n", i, (unsigned long long)ret, where);
        if (next <= fp) break;
        fp = next;
    }
    ptrace(PTRACE_DETACH, tid, 0, 0);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s PID [frames] [TID...]\n", argv[0]); return 2; }
    target = atoi(argv[1]);
    int frames = argc > 2 ? atoi(argv[2]) : 48;
    load_maps();
    if (argc > 3) { for (int i = 3; i < argc; ++i) dump_thread(atoi(argv[i]), frames); return 0; }
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/task", target);
    DIR *d = opendir(path); if (!d) { perror("task"); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') dump_thread(atoi(e->d_name), frames);
    closedir(d);
    return 0;
}
