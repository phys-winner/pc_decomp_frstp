/* Native vorbisfile for the game's streamed sounds (OGGStream), in place of the lifted libvorbis.
 *
 * Every sound of sounds.dat is Ogg Vorbis.  The engine's OGGStream (research/ida, 2026-11-18:
 * 0x1410C8880 open, 0x1410C87E0 read, 0x1410C8220 destructor) embeds an OggVorbis_File at +48 and
 * calls three vorbisfile entries of the lifted libvorbis:
 *   0x141BAB340 ov_clear(vf)
 *   0x141BAB460 ov_open_callbacks(datasource, vf, NULL, 0, callbacks)  read only, no seek/tell/close
 *   0x141BAB940 ov_read(vf, buffer, length, 0, 2, 1, &bitstream)       16-bit signed little endian
 * After a successful open it reads vf->vi (Win64 offset 104) and vi->channels (+4), vi->rate (+8,
 * a 4-byte Win64 long) directly.  These entries now run libvorbis 1.3.7 and libogg 1.3.5 natively
 * (reference/, BSD; compiled with -ffp-contract=off like vorbis_native.c, whose mdct and codebook
 * code already matched the game's bit for bit).  The native OggVorbis_File lives in native
 * memory; the guest one holds its handle (+0), a magic (+16) and vi (+104), which points to a
 * vorbis_info copy in the Win64 layout.  The engine's read callback is called as guest code.
 *
 * debug.fp.vorbis_verify=1 records the compressed bytes each native stream reads and, when the
 * stream is cleared, decodes the same bytes with the lifted vorbisfile (fp_original_ aliases of
 * tools/dead_code.py; its read callback is the sentinel 0xE6300000 served by
 * frostpunk_vorbisfile_dispatch) and compares the PCM byte for byte: VORBISFILE_NATIVE lines.
 * debug.fp.vorbisfile_native=0 uses the lifted vorbisfile only.  The lifted libvorbis stays for
 * the audio track of the videos, which the engine decodes with the low-level API.
 */
#include "cpu64.h"
#include <stdatomic.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/system_properties.h>
#include <vorbis/vorbisfile.h>

#define FP_VF_MAGIC UINT64_C(0x4656464E54414E46)   /* "FNATNFVF" */
#define FP_VF_RETURN UINT64_C(0xE5EE0500)
#define FP_VF_READ_SENTINEL UINT64_C(0xE6300000)

typedef struct {
    int32_t version, channels, rate, bitrate_upper, bitrate_nominal, bitrate_lower, bitrate_window, pad;
    uint64_t codec_setup;
} win64_vorbis_info;                     /* vorbis_info with Win64's 4-byte long */
_Static_assert(sizeof(win64_vorbis_info) == 40, "Win64 vorbis_info");

typedef struct {
    OggVorbis_File vf;
    win64_vorbis_info vi;
    CPU *cpu;                            /* the guest CPU of the current entry */
    uint64_t datasource, read_func;
    unsigned char *record;               /* verification: compressed input and native PCM */
    size_t record_size, record_cap;
    unsigned char *pcm;
    size_t pcm_size, pcm_cap;
} native_stream;

static int fp_vf_native = -1, fp_vf_verify;
static _Atomic unsigned long long fp_vf_streams, fp_vf_bytes, fp_vf_verified, fp_vf_mismatches;
static _Atomic unsigned long long fp_vf_ns;

static void fp_vf_configure(void) {
    if (fp_vf_native >= 0) return;
    char value[PROP_VALUE_MAX] = {0};
    int native = !(__system_property_get("debug.fp.vorbisfile_native", value) > 0 && value[0] == '0');
    value[0] = 0;
    fp_vf_verify = native && __system_property_get("debug.fp.vorbis_verify", value) > 0 && value[0] == '1';
    fp_vf_native = native;
    fprintf(stderr, "VORBISFILE_NATIVE native=%d verify=%d decoder=\"%s\"\n", native, fp_vf_verify, vorbis_version_string());
}

static uint64_t fp_vf_call(CPU *c, uint64_t fn, void (*lifted)(CPU *), const uint64_t *args, unsigned count) {
    uint64_t rsp0 = c->rsp;
    uint64_t sp = ((rsp0 - 256) & ~UINT64_C(15)) - 32 - 8 * (count > 4 ? ((count - 4 + 1) & ~1u) : 0);
    for (unsigned i = 4; i < count; ++i) wr64(sp + 32 + 8 * (i - 4), args[i]);
    c->rsp = sp;
    c->rcx = count > 0 ? args[0] : 0; c->rdx = count > 1 ? args[1] : 0;
    c->r8 = count > 2 ? args[2] : 0; c->r9 = count > 3 ? args[3] : 0;
    push64(c, FP_VF_RETURN);
    if (lifted) lifted(c); else dispatch(c, fn);
    if (c->rsp != sp) { fprintf(stderr, "STOP vorbisfile guest call stack imbalance\n"); _Exit(73); }
    c->rsp = rsp0;
    return c->rax;
}

static void fp_append(unsigned char **buf, size_t *size, size_t *cap, const void *data, size_t n) {
    if (*size + n > *cap) {
        size_t next = *cap ? *cap * 2 : 1 << 20;
        while (next < *size + n) next *= 2;
        unsigned char *grown = realloc(*buf, next);
        if (!grown) return;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *size, data, n);
    *size += n;
}

/* The engine's read callback: size_t read(void *ptr, size_t size, size_t nmemb, void *datasource). */
static size_t fp_vf_read(void *ptr, size_t size, size_t nmemb, void *handle) {
    native_stream *s = handle;
    uint64_t args[4] = {(uint64_t)(uintptr_t)ptr, size, nmemb, s->datasource};
    size_t got = (size_t)fp_vf_call(s->cpu, s->read_func, NULL, args, 4);
    if (fp_vf_verify && got) fp_append(&s->record, &s->record_size, &s->record_cap, ptr, got * size);
    return got;
}

/* ------------------------------------------------------------------ verification */
void fp_original_L_000141BAB340(CPU *c);
void fp_original_L_000141BAB460(CPU *c);
void fp_original_L_000141BAB940(CPU *c);
static _Thread_local struct { const unsigned char *data; size_t size, pos; } fp_vf_replay;

/* The lifted decoder's read callback (sentinel address): serve the recorded bytes. */
int frostpunk_vorbisfile_dispatch(CPU *c, uint64_t target) {
    if (target != FP_VF_READ_SENTINEL) return 0;
    size_t size = c->rdx, nmemb = c->r8, want = size * nmemb;
    size_t left = fp_vf_replay.size - fp_vf_replay.pos;
    if (want > left) want = left - left % (size ? size : 1);
    memcpy((void *)(uintptr_t)c->rcx, fp_vf_replay.data + fp_vf_replay.pos, want);
    fp_vf_replay.pos += want;
    c->rax = size ? want / size : 0;
    return 1;   /* the dispatcher pops the return address */
}

static void fp_vf_verify_stream(CPU *c, native_stream *s) {
    if (!s->pcm_size || !s->record_size) return;
    unsigned char *shadow = calloc(1, 4096);   /* larger than Win64 OggVorbis_File */
    unsigned char *pcm = malloc(s->pcm_size + 8192), *buffer = calloc(1, 64);
    if (!shadow || !pcm || !buffer) { free(shadow); free(pcm); free(buffer); return; }
    uint64_t *callbacks = (uint64_t *)buffer;   /* read, seek, close, tell */
    callbacks[0] = FP_VF_READ_SENTINEL;
    fp_vf_replay.data = s->record; fp_vf_replay.size = s->record_size; fp_vf_replay.pos = 0;
    uint64_t open_args[5] = {1, (uint64_t)(uintptr_t)shadow, 0, 0, (uint64_t)(uintptr_t)callbacks};
    int opened = (int)(int32_t)(uint32_t)fp_vf_call(c, 0, fp_original_L_000141BAB460, open_args, 5);
    size_t got = 0;
    if (opened == 0) {
        int bitstream = 0;
        while (got < s->pcm_size) {
            uint64_t read_args[7] = {(uint64_t)(uintptr_t)shadow, (uint64_t)(uintptr_t)(pcm + got), 4096, 0, 2, 1, (uint64_t)(uintptr_t)&bitstream};
            int n = (int)(int32_t)(uint32_t)fp_vf_call(c, 0, fp_original_L_000141BAB940, read_args, 7);
            if (n <= 0) break;
            got += (size_t)n;
        }
        uint64_t clear_args[1] = {(uint64_t)(uintptr_t)shadow};
        fp_vf_call(c, 0, fp_original_L_000141BAB340, clear_args, 1);
    }
    size_t n = got < s->pcm_size ? got : s->pcm_size;
    size_t diff = n;
    for (size_t i = 0; i < n; ++i) if (pcm[i] != s->pcm[i]) { diff = i; break; }
    int same = opened == 0 && n == s->pcm_size && diff == n;
    unsigned long long verified = ++fp_vf_verified;
    if (!same) {
        unsigned long long bad = ++fp_vf_mismatches;
        if (bad <= 16) {
            int max_delta = 0;
            for (size_t i = 0; i + 1 < n; i += 2) {
                int a = (int16_t)(s->pcm[i] | s->pcm[i + 1] << 8), b = (int16_t)(pcm[i] | pcm[i + 1] << 8);
                if (abs(a - b) > max_delta) max_delta = abs(a - b);
            }
            fprintf(stderr, "VORBISFILE_NATIVE MISMATCH stream=%llu opened=%d native_bytes=%zu lifted_bytes=%zu first_diff=%zu max_sample_delta=%d\n",
                    verified, opened, s->pcm_size, got, diff, max_delta);
        }
    }
    if (verified <= 4 || !(verified & (verified - 1)))
        fprintf(stderr, "VORBISFILE_NATIVE verified streams=%llu mismatches=%llu last_bytes=%zu\n", verified,
                (unsigned long long)fp_vf_mismatches, s->pcm_size);
    free(shadow); free(pcm); free(buffer);
}

/* ov_read for 16-bit signed little-endian output with the game's rounding.  libvorbis converts
 * with vorbis_ftoi of os.h: the MSVC x64 build of the game uses _mm_cvtsd_si32 (round to nearest
 * even; NaN gives INT_MIN), the portable branch floor(f + .5); they differ by one LSB on halves.
 * The rest is ov_read_filter of vorbisfile.c 1.3.7 over ov_read_float. */
static long fp_ov_read_s16le(OggVorbis_File *vf, char *buffer, int length, int *bitstream) {
    if (vf->ready_state < 2 /* OPENED */) return OV_EINVAL;
    const vorbis_info *vi = ov_info(vf, -1);
    long channels = vi ? vi->channels : 0;
    if (channels < 1 || channels > 255) return OV_EINVAL;
    long bytespersample = 2 * channels;
    if (length / bytespersample <= 0) return OV_EINVAL;
    float **pcm = NULL;
    long samples = ov_read_float(vf, &pcm, (int)(length / bytespersample), bitstream);
    if (samples <= 0) return samples;
    for (long i = 0; i < channels; i++) {
        const float *src = pcm[i];
        short *dest = (short *)buffer + i;
        for (long j = 0; j < samples; j++) {
            double f = (double)(src[j] * 32768.f);
            long val = __builtin_isnan(f) ? INT32_MIN : lrint(f);
            if (val > 32767) val = 32767;
            else if (val < -32768) val = -32768;
            *dest = (short)val;
            dest += channels;
        }
    }
    return samples * bytespersample;
}

/* ------------------------------------------------------------------ the entries */
static native_stream *fp_vf_native_of(uint64_t vf) {
    if (rd64(vf + 16) != FP_VF_MAGIC) return NULL;
    return (native_stream *)(uintptr_t)rd64(vf);
}
static void fp_vf_release(CPU *c, uint64_t vf) {
    native_stream *s = fp_vf_native_of(vf);
    if (!s) return;
    s->cpu = c;
    if (fp_vf_verify) fp_vf_verify_stream(c, s);
    ov_clear(&s->vf);
    free(s->record);
    free(s->pcm);
    free(s);
    wr64(vf, 0);
    wr64(vf + 16, 0);
    wr64(vf + 104, 0);
}

void L_000141BAB340(CPU *c) {   /* int ov_clear(OggVorbis_File *) */
    fp_vf_configure();
    if (!fp_vf_native || !fp_vf_native_of(c->rcx)) { fp_original_L_000141BAB340(c); return; }   /* lifted or never opened */
    fp_vf_release(c, c->rcx);
    c->rax = 0;
    c->rip = pop64(c);
}

void L_000141BAB460(CPU *c) {   /* int ov_open_callbacks(void *, OggVorbis_File *, const char *, long, ov_callbacks) */
    fp_vf_configure();
    if (!fp_vf_native) { fp_original_L_000141BAB460(c); return; }
    uint64_t datasource = c->rcx, vf = c->rdx, initial = c->r8, ibytes = (uint64_t)(int64_t)(int32_t)(uint32_t)c->r9;
    uint64_t callbacks = rd64(c->rsp + 40);   /* the struct is passed by reference */
    int result = OV_EFAULT;
    fp_vf_release(c, vf);
    native_stream *s = calloc(1, sizeof(*s));
    if (s && !rd64(callbacks + 8) && !rd64(callbacks + 16) && !rd64(callbacks + 24)) {
        s->cpu = c;
        s->datasource = datasource;
        s->read_func = rd64(callbacks);
        ov_callbacks native = {fp_vf_read, NULL, NULL, NULL};
        result = ov_open_callbacks(s, &s->vf, (const char *)(uintptr_t)initial, (long)ibytes, native);
    } else if (s) {
        fprintf(stderr, "VORBISFILE_NATIVE seekable callbacks are not supported; using the lifted decoder\n");
        free(s);
        fp_original_L_000141BAB460(c);
        return;
    }
    if (result == 0) {
        const vorbis_info *vi = ov_info(&s->vf, -1);
        s->vi.version = vi->version;
        s->vi.channels = vi->channels;
        s->vi.rate = (int32_t)vi->rate;
        s->vi.bitrate_upper = (int32_t)vi->bitrate_upper;
        s->vi.bitrate_nominal = (int32_t)vi->bitrate_nominal;
        s->vi.bitrate_lower = (int32_t)vi->bitrate_lower;
        s->vi.bitrate_window = (int32_t)vi->bitrate_window;
        wr64(vf, (uint64_t)(uintptr_t)s);
        wr64(vf + 16, FP_VF_MAGIC);
        wr64(vf + 104, (uint64_t)(uintptr_t)&s->vi);
        unsigned long long n = ++fp_vf_streams;
        if (n <= 2 || !(n & (n - 1)))
            fprintf(stderr, "VORBISFILE_NATIVE streams=%llu pcm_bytes=%llu decode_ms=%.1f (last %d ch %d Hz)\n", n,
                    (unsigned long long)fp_vf_bytes, fp_vf_ns / 1e6, vi->channels, (int)vi->rate);
    } else {
        free(s);
    }
    c->rax = (uint64_t)(uint32_t)result;
    c->rip = pop64(c);
}

void L_000141BAB940(CPU *c) {   /* long ov_read(OggVorbis_File *, char *, int, int, int, int, int *) */
    fp_vf_configure();
    if (!fp_vf_native) { fp_original_L_000141BAB940(c); return; }
    native_stream *s = fp_vf_native_of(c->rcx);
    if (!s) { fp_original_L_000141BAB940(c); return; }   /* a stream the lifted decoder opened */
    char *buffer = (char *)(uintptr_t)c->rdx;
    int length = (int)(int32_t)(uint32_t)c->r8, bigendian = (int)(int32_t)(uint32_t)c->r9;
    int word = (int)(int32_t)(uint32_t)rd64(c->rsp + 40), sgned = (int)(int32_t)(uint32_t)rd64(c->rsp + 48);
    int *bitstream = (int *)(uintptr_t)rd64(c->rsp + 56);
    s->cpu = c;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long got = word == 2 && sgned && !bigendian ? fp_ov_read_s16le(&s->vf, buffer, length, bitstream)
                                                : ov_read(&s->vf, buffer, length, bigendian, word, sgned, bitstream);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fp_vf_ns += (unsigned long long)((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec));
    if (got > 0) {
        fp_vf_bytes += (unsigned long long)got;
        if (fp_vf_verify) fp_append(&s->pcm, &s->pcm_size, &s->pcm_cap, buffer, (size_t)got);
    }
    c->rax = (uint64_t)(uint32_t)(int32_t)got;
    c->rip = pop64(c);
}
