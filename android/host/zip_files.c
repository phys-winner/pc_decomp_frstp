#define _GNU_SOURCE
#include "zip_files.h"
#include "../../reference/launcher-zipmount/zipmount.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static zm_mount *mount;
static int archive_fd = -1;
static void initialize(void) {
    mount = zm_from_env("GAME");
    const char *fd = getenv("PORT_MOUNT_FD_GAME");
    if (mount && fd) archive_fd = atoi(fd);
    if (mount) fprintf(stderr, "FP_ZIP_MOUNT files=%zu extraction=0\n", zm_count(mount));
}
static const char *root(void) {
    const char *path = getenv("PORT_PART_GAME");
    return path && *path ? path : getenv("FROSTPUNK_GAME_ROOT");
}
int fp_data_root_matches(const char *path) {
    const char *base = root(); char canonical[PATH_MAX];
    return base && realpath(base, canonical) && !strcmp(path, canonical);
}
static int available(const char *key) {
    const char *value = getenv(key); return value && !strcmp(value, "1");
}
static int physical(const char *name, uint64_t *offset, uint64_t *length, struct stat *st) {
    const char *base = root(); if (!base) { errno = ENOENT; return -1; }
    int dir = open(base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), saved = errno; close(dir); errno = saved;
    if (fd < 0) return -1;
    if (fstat(fd, st) || !S_ISREG(st->st_mode)) { close(fd); errno = EINVAL; return -1; }
    *offset = 0; *length = (uint64_t)st->st_size; return fd;
}
static int source(const char *name, uint64_t *offset, uint64_t *length, struct stat *st) {
    int fd = physical(name, offset, length, st);
    if (fd >= 0) return fd;
    if (errno != ENOENT && errno != ENOTDIR) return -1;
    pthread_once(&once, initialize);
    const zm_entry *entry = zm_find(mount, name);
    if (!entry || entry->flags & ZM_SIDECAR) { errno = ENOENT; return -1; }
    fd = fcntl(archive_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (fstat(fd, st)) { int saved = errno; close(fd); errno = saved; return -1; }
    st->st_mode = S_IFREG | 0444; st->st_size = (off_t)entry->size;
    st->st_ino = (ino_t)(entry->offset + 1); st->st_dev = (dev_t)0x46505a;
    st->st_mtim.tv_sec = (time_t)(entry->time / 1000); st->st_mtim.tv_nsec = (long)(entry->time % 1000) * 1000000;
    *offset = entry->offset; *length = entry->size; return fd;
}
int fp_data_open(const char *relative, fp_data_file *file) {
    if (!relative || !*relative || relative[0] == '/' || strchr(relative, '\\') || strchr(relative, ':')) return 0;
    for (const char *at = relative; *at;) {
        const char *end = strchr(at, '/'); size_t n = end ? (size_t)(end - at) : strlen(at);
        if (!n || (n == 1 && at[0] == '.') || (n == 2 && at[0] == '.' && at[1] == '.')) return 0;
        if (!end) break; at = end + 1;
    }
    pthread_once(&once, initialize);
    const char *name = relative, *tail = NULL;
    int port = available("FROSTPUNK_PORT_DATA"), console = available("FROSTPUNK_SWITCH_DATA");
    if (port && !strcasecmp(relative, "common.idx")) name = "port-ui.common.idx";
    if (port && !strcasecmp(relative, "localizations.idx")) name = "port-ui.localizations.idx";
    if (console && !strcasecmp(relative, "templates-switch.idx")) name = "switch-ui.templates-switch.idx";
    if (port && !strcasecmp(relative, "common.dat")) tail = "port-ui.common.append";
    if (port && !strcasecmp(relative, "localizations.dat")) tail = "port-ui.localizations.append";
    if (console && !strcasecmp(relative, "templates.dat")) tail = "switch-ui.templates.append";
    if (!mount && name == relative && !tail) return 0;
    if (name == relative && !tail) {
        struct stat st; uint64_t at, size; int local = physical(name, &at, &size, &st);
        if (local >= 0) { close(local); return 0; }
        if (errno != ENOENT && errno != ENOTDIR) return 0;
    }
    memset(file, 0, sizeof(*file)); file->fd[0] = file->fd[1] = -1;
    file->fd[0] = source(name, &file->offset[0], &file->length[0], &file->stat);
    if (file->fd[0] < 0) return 0;
    file->size = file->length[0];
    if (tail) {
        struct stat ignored;
        file->fd[1] = source(tail, &file->offset[1], &file->length[1], &ignored);
        if (file->fd[1] < 0 || file->length[1] > INT64_MAX - file->size) { fp_data_close(file); errno = EINVAL; return 0; }
        file->size += file->length[1]; file->stat.st_size = (off_t)file->size;
    }
    return 1;
}
void fp_data_close(fp_data_file *file) {
    for (unsigned i = 0; i < 2; ++i) if (file->fd[i] >= 0) { close(file->fd[i]); file->fd[i] = -1; }
}
long long fp_data_read(const fp_data_file *file, void *buffer, size_t size, uint64_t at) {
    if (at >= file->size) return 0;
    if (size > file->size - at) size = (size_t)(file->size - at);
    size_t done = 0;
    while (done < size) {
        unsigned segment = at >= file->length[0];
        uint64_t local = segment ? at - file->length[0] : at;
        size_t count = size - done;
        if (count > file->length[segment] - local) count = (size_t)(file->length[segment] - local);
        ssize_t n; do n = pread(file->fd[segment], (char *)buffer + done, count, (off_t)(file->offset[segment] + local)); while (n < 0 && errno == EINTR);
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n; at += (uint64_t)n;
    }
    return (long long)done;
}
int fp_data_stat(const char *relative, struct stat *st) {
    fp_data_file file; if (!fp_data_open(relative, &file)) return 0;
    *st = file.stat; fp_data_close(&file); return 1;
}
typedef struct { fp_data_file file; uint64_t position; } cookie;
static int stream_read(void *opaque, char *buffer, int size) {
    cookie *stream = opaque; long long n = fp_data_read(&stream->file, buffer, (size_t)size, stream->position);
    if (n > 0) stream->position += (uint64_t)n; return (int)n;
}
static fpos64_t stream_seek(void *opaque, fpos64_t delta, int origin) {
    cookie *stream = opaque;
    uint64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? stream->position : stream->file.size;
    if (origin < SEEK_SET || origin > SEEK_END || (delta < 0 && (uint64_t)(-(delta + 1)) + 1 > base)
        || (delta >= 0 && (uint64_t)delta > INT64_MAX - base)) { errno = EINVAL; return -1; }
    stream->position = base + (uint64_t)delta; return (fpos64_t)stream->position;
}
static int stream_close(void *opaque) { cookie *stream = opaque; fp_data_close(&stream->file); free(stream); return 0; }
FILE *fp_data_stream(const fp_data_file *file) {
    cookie *stream = calloc(1, sizeof(*stream)); if (!stream) return NULL;
    stream->file = *file; stream->file.fd[0] = stream->file.fd[1] = -1;
    for (unsigned i = 0; i < 2; ++i) if (file->fd[i] >= 0) {
        stream->file.fd[i] = fcntl(file->fd[i], F_DUPFD_CLOEXEC, 0);
        if (stream->file.fd[i] < 0) { stream_close(stream); return NULL; }
    }
    FILE *result = funopen64(stream, stream_read, NULL, stream_seek, stream_close);
    if (!result) stream_close(stream); return result;
}
FILE *fp_data_fopen(const char *path, const char *mode) {
    const char *base = root(); size_t n = base ? strlen(base) : 0;
    if (n && !strncmp(path, base, n) && path[n] == '/' && !strcmp(mode, "rb")) {
        fp_data_file file;
        if (fp_data_open(path + n + 1, &file)) { FILE *stream = fp_data_stream(&file); fp_data_close(&file); return stream; }
    }
    if (!strcmp(mode, "rb")) { int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); if (fd < 0) return NULL;
        FILE *stream = fdopen(fd, mode); if (!stream) close(fd); return stream; }
    return fopen(path, mode);
}
void *fp_data_map(const fp_data_file *file, uint64_t at, size_t size, int prot, void *requested, void **base, size_t *bytes) {
    if (at > file->size || size > file->size - at) { errno = EINVAL; return MAP_FAILED; }
    if (!requested && size <= file->length[0] && at <= file->length[0] - size) {
        size_t page = (size_t)sysconf(_SC_PAGESIZE); uint64_t physical_at = file->offset[0] + at;
        size_t delta = (size_t)(physical_at % page);
        if (size > SIZE_MAX - delta) { errno = EOVERFLOW; return MAP_FAILED; }
        *bytes = size + delta; *base = mmap(NULL, *bytes, prot, MAP_PRIVATE, file->fd[0], (off_t)(physical_at - delta));
        return *base == MAP_FAILED ? MAP_FAILED : (char *)*base + delta;
    }
    /* shortcut: composite mapped views copy their requested range; optimize page joins if large views become a measured memory cost. */
    *bytes = size; *base = mmap(requested, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (*base == MAP_FAILED) return MAP_FAILED;
    if ((requested && *base != requested) || fp_data_read(file, *base, size, at) != (long long)size || mprotect(*base, size, prot)) {
        int saved = errno; munmap(*base, size); errno = saved ? saved : EINVAL; return MAP_FAILED;
    }
    return *base;
}
typedef struct { size_t wanted, at; char *name; size_t capacity; struct stat *stat; int found; } listing;
static int visit(const zm_entry *entry, void *opaque) {
    listing *list = opaque;
    if (memchr(entry->path, '/', entry->length)) return 0;
    if (list->at++ != list->wanted) return 0;
    if ((size_t)entry->length >= list->capacity) return -1;
    memcpy(list->name, entry->path, entry->length); list->name[entry->length] = 0;
    memset(list->stat, 0, sizeof(*list->stat)); list->stat->st_mode = S_IFREG | 0444;
    list->stat->st_size = (off_t)entry->size; list->stat->st_ino = (ino_t)(entry->offset + 1);
    list->stat->st_mtim.tv_sec = (time_t)(entry->time / 1000); list->found = 1; return 1;
}
int fp_data_next(size_t *cursor, char *name, size_t capacity, struct stat *st) {
    pthread_once(&once, initialize); if (!mount) return 0;
    listing list = {*cursor, 0, name, capacity, st, 0}; zm_list(mount, "", visit, &list);
    if (list.found) { fp_data_stat(name, st); ++*cursor; } return list.found;
}
