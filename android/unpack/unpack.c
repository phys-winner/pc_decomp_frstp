/* XZ Embedded chunks use the mapped output as their dictionary and decode
 * on up to four cores. NativeLibrary.java checks SHA-256 before publishing
 * the result atomically. Adapted from The Void android/unpack/unpack.c.
 * UNPACK_MAIN builds the isolated device fixture.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "xz.h"

#define MAX_CHUNKS 32
#define MAX_THREADS 4

typedef struct {
    const uint8_t *in;
    uint8_t *out;
    int count;
    uint64_t in_off[MAX_CHUNKS], out_off[MAX_CHUNKS];
    uint32_t in_size[MAX_CHUNKS], out_size[MAX_CHUNKS];
    _Atomic int next;
    _Atomic int failed;   /* the first error: -1 - xz_ret */
} job_t;

static void *worker(void *arg)
{
    job_t *j = (job_t *)arg;
    for (int k; !atomic_load(&j->failed) && (k = atomic_fetch_add(&j->next, 1)) < j->count;) {
        struct xz_dec *dec = xz_dec_init(XZ_SINGLE, 0);
        if (!dec) { int none = 0; atomic_compare_exchange_strong(&j->failed, &none, -1 - XZ_MEM_ERROR); break; }
        struct xz_buf b = { .in = j->in + j->in_off[k], .in_pos = 0, .in_size = j->in_size[k],
                            .out = j->out + j->out_off[k], .out_pos = 0, .out_size = j->out_size[k] };
        enum xz_ret ret = xz_dec_run(dec, &b);
        xz_dec_end(dec);
        if (ret != XZ_STREAM_END || b.out_pos != b.out_size || b.in_pos != b.in_size) {
            int none = 0;
            atomic_compare_exchange_strong(&j->failed, &none, -1 - (ret == XZ_STREAM_END ? XZ_DATA_ERROR : ret));
            break;
        }
    }
    return NULL;
}

/* Unpacks chunks of `chunk` bytes (the last one shorter), packed one after
 * another in in[0..in_size) as streams of the sizes `sizes`, into a new
 * file fd of `total` bytes. Returns total, or -1 - xz_ret for a broken
 * stream, -100 - errno for a failed write, -400 for a bad chunk table. */
static long long unpack(const uint8_t *in, size_t in_size, int fd, uint64_t total, uint32_t chunk,
                        const uint32_t *sizes, int count)
{
    job_t j;
    if (count < 1 || count > MAX_CHUNKS || !chunk || chunk > (128u << 20) || !total || total > (1ull << 30) ||
        (uint64_t)(count - 1) * chunk >= total ||
        (uint64_t)count * chunk < total) return -400;
    memset(&j, 0, sizeof j);
    uint64_t at = 0;
    for (int k = 0; k < count; ++k) {
        if (!sizes[k] || sizes[k] > INT32_MAX) return -400;
        j.in_off[k] = at;
        j.in_size[k] = sizes[k];
        j.out_off[k] = (uint64_t)k * chunk;
        j.out_size[k] = (uint32_t)(k + 1 < count ? chunk : total - j.out_off[k]);
        at += sizes[k];
    }
    if (at != in_size) return -400;
    xz_crc32_init();
#ifdef XZ_USE_CRC64
    xz_crc64_init();
#endif
    if (ftruncate(fd, (off_t)total)) return -100 - errno;
    /* Reserve blocks before mmap writes so ENOSPC returns an error instead of SIGBUS. */
    int reserved = posix_fallocate(fd, 0, (off_t)total);
    if (reserved) return -100 - reserved;
    void *out = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (out == MAP_FAILED) return -100 - errno;
    j.in = in;
    j.out = (uint8_t *)out;
    j.count = count;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = (int)(cores < 1 ? 1 : cores > MAX_THREADS ? MAX_THREADS : cores);
    if (threads > count) threads = count;
    pthread_t pool[MAX_THREADS];
    int started = 0;
    for (; started < threads - 1; ++started)
        if (pthread_create(&pool[started], NULL, worker, &j)) break;
    worker(&j);   /* this thread works too */
    for (int i = 0; i < started; ++i) pthread_join(pool[i], NULL);
    long long result = atomic_load(&j.failed) ? atomic_load(&j.failed) : (long long)total;
    if (result >= 0 && msync(out, (size_t)total, MS_SYNC)) result = -100 - errno;
    munmap(out, (size_t)total);
    return result;
}

#ifdef UNPACK_MAIN
#include <sys/stat.h>
#include <time.h>

/* xzunpack IN.xz OUT TOTAL CHUNK SIZE1,SIZE2,... (assets/native/index.txt's fields) */
int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: %s IN.xz OUT TOTAL CHUNK SIZE1,SIZE2,...\n", argv[0]); return 2; }
    uint32_t sizes[MAX_CHUNKS];
    int count = 0;
    for (char *p = argv[5]; *p && count < MAX_CHUNKS;) {
        char *end;
        errno = 0;
        unsigned long n = strtoul(p, &end, 10);
        if (errno || end == p || n > INT32_MAX || (*end && *end != ',')) return 2;
        sizes[count++] = (uint32_t)n;
        p = *end ? end + 1 : end;
    }
    int in = open(argv[1], O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st)) { perror(argv[1]); return 1; }
    const uint8_t *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, in, 0);
    if (data == MAP_FAILED) { perror("mmap"); return 1; }
    int out = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror(argv[2]); return 1; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long long n = unpack(data, (size_t)st.st_size, out, strtoull(argv[3], NULL, 10), (uint32_t)strtoul(argv[4], NULL, 10),
                         sizes, count);
    if (close(out) && n >= 0) n = -100 - errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (n < 0) { fprintf(stderr, "unpack failed: %lld\n", n); return 1; }
    printf("%lld bytes in %.2f s (%.1f MB/s), %d chunks\n", n, s, (double)n / s / 1e6, count);
    return 0;
}
#else
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

/* Unpacks the APK's asset `name` (chunks of `chunk` bytes packed as streams
 * of the sizes `sizes`) into the file `path`, `total` bytes, made anew.
 * Returns the bytes written, or a negative error (see unpack; -200: the
 * asset is missing or cannot be read, -300: the file cannot be made). */
JNIEXPORT jlong JNICALL Java_com_frostpunk_staticrecomp_NativeLibrary_nativeUnpack(JNIEnv *env, jclass cls, jobject manager,
    jstring name, jstring path, jlong total, jint chunk, jintArray sizes)
{
    (void)cls;
    jsize count = sizes ? (*env)->GetArrayLength(env, sizes) : 0;
    if (!manager || !name || !path || count < 1 || count > MAX_CHUNKS || total <= 0 || chunk <= 0) return -400;
    uint32_t table[MAX_CHUNKS];
    (*env)->GetIntArrayRegion(env, sizes, 0, count, (jint *)table);
    if ((*env)->ExceptionCheck(env)) return -400;
    AAssetManager *assets = AAssetManager_fromJava(env, manager);
    const char *asset_name = (*env)->GetStringUTFChars(env, name, NULL);
    AAsset *asset = assets && asset_name ? AAssetManager_open(assets, asset_name, AASSET_MODE_BUFFER) : NULL;
    if (asset_name) (*env)->ReleaseStringUTFChars(env, name, asset_name);
    if (!asset) return -200;
    /* Stored uncompressed in the APK (no deflate on top of xz): mapped, not copied. */
    const void *data = AAsset_getBuffer(asset);
    off64_t size = AAsset_getLength64(asset);
    if (!data || size <= 0) { AAsset_close(asset); return -200; }
    const char *file = (*env)->GetStringUTFChars(env, path, NULL);
    int fd = file ? open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    if (file) (*env)->ReleaseStringUTFChars(env, path, file);
    if (fd < 0) { AAsset_close(asset); return -300; }
    long long n = unpack((const uint8_t *)data, (size_t)size, fd, (uint64_t)total, (uint32_t)chunk, table, (int)count);
    if (n >= 0 && fsync(fd)) n = -100 - errno;
    if (close(fd) && n >= 0) n = -100 - errno;
    AAsset_close(asset);
    return (jlong)n;
}

#endif
