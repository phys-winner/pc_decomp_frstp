/* Native Theora video decoding in place of the lifted libtheora of Frostpunk.exe.
 *
 * The game plays its videos (30 Ogg Theora files in videos.dat: cinematics and logos at
 * 2560x1080 and 30 fps, tutorial clips down to 432x336) through the legacy libtheora API of
 * "Xiph.Org libtheora 1.2.0alpha 20100924 (Ptalarbvorm)", lifted.  The unmodified decoder of
 * libtheora 1.2.0alpha1 (reference/libtheora-1.2.0alpha1, BSD, released as 20100923) runs here
 * natively, in C (no assembly), behind the eleven legacy entries that the engine's
 * VideoDecoderTheora calls (found in research/ida/Frostpunk.exe.i64 on 2026-11-18):
 *
 *   0x141B9E9C0 theora_decode_header   0x141B9EEC0 theora_info_init
 *   0x141B9EB30 theora_decode_init     0x141B9EEE0 theora_info_clear
 *   0x141B9EC30 theora_decode_packetin 0x141B9EF30 theora_clear
 *   0x141B9EC90 theora_decode_YUVout   0x141B9EFB0 theora_comment_init (a thunk to th_comment_init)
 *   0x141B9EE90 theora_granule_time    0x141B9F000 theora_comment_clear (a thunk to th_comment_clear)
 *   0x141B9F010 theora_control
 *
 * theora_info, theora_comment, theora_state and yuv_buffer have the same layout on Win64 and
 * ARM64 (ints and pointers); ogg_packet does not (Win64 `long` is 4 bytes) and is converted.
 * The engine asks for post-processing level 0 (theora_control 3) and sets the granule position
 * (5), so the decoded pixels are fixed by the Theora specification.  The decoder's state is
 * native memory referenced from the guest structures, created and freed only by these entries.
 * The lifted bodies stay for the engine's (unused) Theora encoder and for the check below.
 *
 * debug.fp.theora_verify=1 runs the lifted decoder alongside on shadow structures (through the
 * fp_original_ aliases of tools/dead_code.py), compares every decoded frame byte for byte and
 * logs both decode times: THEORA_NATIVE lines in native.log.  debug.fp.theora_native=0 calls the
 * lifted decoder only.
 */
#include "cpu64.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/system_properties.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshift-op-parentheses"
#pragma clang diagnostic ignored "-Wparentheses"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "../../reference/libtheora-1.2.0alpha1/lib/apiwrapper.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/bitpack.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/decapiwrapper.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/decinfo.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/decode.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/dequant.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/fragment.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/huffdec.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/idct.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/info.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/internal.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/quant.c"
#include "../../reference/libtheora-1.2.0alpha1/lib/state.c"
#pragma clang diagnostic pop

_Static_assert(sizeof(theora_info) == 112, "theora_info (the engine clears 0x70 bytes)");
_Static_assert(sizeof(theora_state) == 32 && offsetof(theora_state, granulepos) == 8, "theora_state");
_Static_assert(sizeof(theora_comment) == 32, "theora_comment");
_Static_assert(sizeof(yuv_buffer) == 48 && offsetof(yuv_buffer, y) == 24, "yuv_buffer");

/* ------------------------------------------------------------------ guest helpers */
#define FP_THEORA_RETURN UINT64_C(0xE5EE0400)
static int fp_theora_native = -1, fp_theora_verify;

static void fp_theora_configure(void) {
    if (fp_theora_native >= 0) return;
    char value[PROP_VALUE_MAX] = {0};
    fp_theora_native = !(__system_property_get("debug.fp.theora_native", value) > 0 && value[0] == '0');
    value[0] = 0;
    fp_theora_verify = fp_theora_native && __system_property_get("debug.fp.theora_verify", value) > 0 && value[0] == '1';
    fprintf(stderr, "THEORA_NATIVE native=%d verify=%d decoder=\"%s\"\n", fp_theora_native, fp_theora_verify, OC_VENDOR_STRING);
}

/* A Win64 call of a lifted body below the current guest frame. */
static uint64_t fp_lifted_call(CPU *c, void (*fn)(CPU *), uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t rsp0 = c->rsp;
    uint64_t sp = ((rsp0 - 128) & ~UINT64_C(15)) - 32;
    c->rsp = sp;
    c->rcx = a0; c->rdx = a1; c->r8 = a2; c->r9 = a3;
    push64(c, FP_THEORA_RETURN);
    fn(c);
    if (c->rsp != sp) { fprintf(stderr, "STOP theora lifted call stack imbalance\n"); _Exit(73); }
    c->rsp = rsp0;
    return c->rax;
}

/* Win64 ogg_packet: packet@0, long bytes@8, b_o_s@12, e_o_s@16, granulepos@24, packetno@32. */
static ogg_packet fp_theora_packet(uint64_t p) {
    ogg_packet op;
    op.packet = (unsigned char *)(uintptr_t)rd64(p);
    op.bytes = (long)(int32_t)rd32(p + 8);
    op.b_o_s = (long)(int32_t)rd32(p + 12);
    op.e_o_s = (long)(int32_t)rd32(p + 16);
    op.granulepos = (ogg_int64_t)rd64(p + 24);
    op.packetno = (ogg_int64_t)rd64(p + 32);
    return op;
}

static double fp_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ verification shadows */
void fp_original_L_000141B9E9C0(CPU *c);
void fp_original_L_000141B9EB30(CPU *c);
void fp_original_L_000141B9EC30(CPU *c);
void fp_original_L_000141B9EC90(CPU *c);
void fp_original_L_000141B9EE90(CPU *c);
void fp_original_L_000141B9EEC0(CPU *c);
void fp_original_L_000141B9EEE0(CPU *c);
void fp_original_L_000141B9EF30(CPU *c);
void fp_original_L_000141B9EFB0(CPU *c);
void fp_original_L_000141B9F000(CPU *c);
void fp_original_L_000141B9F010(CPU *c);

#define FP_SHADOWS 16
static struct { uint64_t guest; void *shadow; } fp_shadows[FP_SHADOWS * 3];
static void *fp_shadow(uint64_t guest, size_t size, int create) {
    for (unsigned i = 0; i < sizeof(fp_shadows) / sizeof(*fp_shadows); ++i)
        if (fp_shadows[i].guest == guest) return fp_shadows[i].shadow;
    if (!create) return NULL;
    for (unsigned i = 0; i < sizeof(fp_shadows) / sizeof(*fp_shadows); ++i)
        if (!fp_shadows[i].guest) {
            fp_shadows[i].guest = guest;
            fp_shadows[i].shadow = calloc(1, size);
            return fp_shadows[i].shadow;
        }
    fprintf(stderr, "THEORA_NATIVE verification: no free shadow slot\n");
    return NULL;
}
static void fp_shadow_drop(uint64_t guest) {
    for (unsigned i = 0; i < sizeof(fp_shadows) / sizeof(*fp_shadows); ++i)
        if (fp_shadows[i].guest == guest) { free(fp_shadows[i].shadow); fp_shadows[i].guest = 0; fp_shadows[i].shadow = NULL; }
}

static struct {
    unsigned long long frames, verified, mismatches;
    double native_s, lifted_s;
    int w, h;
} fp_theora_stats;
static void fp_theora_report(void) {
    if (!fp_theora_stats.frames || fp_theora_stats.frames % 300) return;
    fprintf(stderr, "THEORA_NATIVE frames=%llu size=%dx%d native_ms_per_frame=%.2f verified=%llu mismatches=%llu lifted_ms_per_frame=%.2f\n",
            fp_theora_stats.frames, fp_theora_stats.w, fp_theora_stats.h, 1e3 * fp_theora_stats.native_s / fp_theora_stats.frames,
            fp_theora_stats.verified, fp_theora_stats.mismatches,
            fp_theora_stats.verified ? 1e3 * fp_theora_stats.lifted_s / fp_theora_stats.verified : 0.0);
}
static int fp_plane_equal(const unsigned char *a, int a_stride, const unsigned char *b, int b_stride, int w, int h) {
    for (int y = 0; y < h; ++y)
        if (memcmp(a + (ptrdiff_t)y * a_stride, b + (ptrdiff_t)y * b_stride, (size_t)w)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ the legacy entries */
#define A1 (c->rcx)
#define A2 (c->rdx)
#define A3 (c->r8)
#define A4 (c->r9)
#define RETURN_TO_GUEST(value) do { c->rax = (uint64_t)(value); c->rip = pop64(c); } while (0)
#define LIFTED_ONLY(fn) do { fp_theora_configure(); if (!fp_theora_native) { fp_original_##fn(c); return; } } while (0)

void L_000141B9EEC0(CPU *c) {   /* theora_info_init(theora_info *) */
    LIFTED_ONLY(L_000141B9EEC0);
    theora_info_init((theora_info *)(uintptr_t)A1);
    if (fp_theora_verify) {
        void *s = fp_shadow(A1, sizeof(theora_info), 1);
        if (s) fp_lifted_call(c, fp_original_L_000141B9EEC0, (uint64_t)(uintptr_t)s, 0, 0, 0);
    }
    RETURN_TO_GUEST(0);
}
void L_000141B9EFB0(CPU *c) {   /* theora_comment_init(theora_comment *) */
    LIFTED_ONLY(L_000141B9EFB0);
    theora_comment_init((theora_comment *)(uintptr_t)A1);
    if (fp_theora_verify) {
        void *s = fp_shadow(A1, sizeof(theora_comment), 1);
        if (s) fp_lifted_call(c, fp_original_L_000141B9EFB0, (uint64_t)(uintptr_t)s, 0, 0, 0);
    }
    RETURN_TO_GUEST(0);
}
void L_000141B9E9C0(CPU *c) {   /* theora_decode_header(theora_info *, theora_comment *, ogg_packet *) */
    LIFTED_ONLY(L_000141B9E9C0);
    uint64_t ci = A1, cc = A2, p = A3;
    ogg_packet op = fp_theora_packet(p);
    int result = theora_decode_header((theora_info *)(uintptr_t)ci, (theora_comment *)(uintptr_t)cc, &op);
    if (fp_theora_verify) {
        void *si = fp_shadow(ci, sizeof(theora_info), 0), *sc = fp_shadow(cc, sizeof(theora_comment), 0);
        if (si && sc) {
            int lifted = (int)(int32_t)(uint32_t)fp_lifted_call(c, fp_original_L_000141B9E9C0, (uint64_t)(uintptr_t)si, (uint64_t)(uintptr_t)sc, p, 0);
            if (lifted != result) fprintf(stderr, "THEORA_NATIVE header result native=%d lifted=%d\n", result, lifted);
        }
    }
    RETURN_TO_GUEST((uint32_t)result);
}
void L_000141B9EB30(CPU *c) {   /* theora_decode_init(theora_state *, theora_info *) */
    LIFTED_ONLY(L_000141B9EB30);
    uint64_t th = A1, ci = A2;
    int result = theora_decode_init((theora_state *)(uintptr_t)th, (theora_info *)(uintptr_t)ci);
    const theora_info *info = (const theora_info *)(uintptr_t)ci;
    fprintf(stderr, "THEORA_NATIVE open %ux%u frame %ux%u fps %u/%u result=%d\n", info->width, info->height, info->frame_width,
            info->frame_height, info->fps_numerator, info->fps_denominator, result);
    if (fp_theora_verify) {
        void *si = fp_shadow(ci, sizeof(theora_info), 0), *st = fp_shadow(th, sizeof(theora_state), 1);
        if (si && st) fp_lifted_call(c, fp_original_L_000141B9EB30, (uint64_t)(uintptr_t)st, (uint64_t)(uintptr_t)si, 0, 0);
    }
    RETURN_TO_GUEST((uint32_t)result);
}
void L_000141B9EC30(CPU *c) {   /* theora_decode_packetin(theora_state *, ogg_packet *) */
    LIFTED_ONLY(L_000141B9EC30);
    uint64_t th = A1, p = A2;
    ogg_packet op = fp_theora_packet(p);
    double t0 = fp_now();
    int result = theora_decode_packetin((theora_state *)(uintptr_t)th, &op);
    fp_theora_stats.native_s += fp_now() - t0;
    if (fp_theora_verify) {
        void *st = fp_shadow(th, sizeof(theora_state), 0);
        if (st) {
            double t1 = fp_now();
            int lifted = (int)(int32_t)(uint32_t)fp_lifted_call(c, fp_original_L_000141B9EC30, (uint64_t)(uintptr_t)st, p, 0, 0);
            fp_theora_stats.lifted_s += fp_now() - t1;
            if (lifted != result) fprintf(stderr, "THEORA_NATIVE packetin result native=%d lifted=%d\n", result, lifted);
        }
    }
    RETURN_TO_GUEST((uint32_t)result);
}
void L_000141B9EC90(CPU *c) {   /* theora_decode_YUVout(theora_state *, yuv_buffer *) */
    LIFTED_ONLY(L_000141B9EC90);
    uint64_t th = A1, out = A2;
    double t0 = fp_now();
    int result = theora_decode_YUVout((theora_state *)(uintptr_t)th, (yuv_buffer *)(uintptr_t)out);
    fp_theora_stats.native_s += fp_now() - t0;
    const yuv_buffer *yuv = (const yuv_buffer *)(uintptr_t)out;
    fp_theora_stats.frames++;
    fp_theora_stats.w = yuv->y_width;
    fp_theora_stats.h = yuv->y_height;
    if (fp_theora_verify) {
        void *st = fp_shadow(th, sizeof(theora_state), 0);
        yuv_buffer lifted;
        memset(&lifted, 0, sizeof(lifted));
        if (st && result == 0) {
            double t1 = fp_now();
            int r = (int)(int32_t)(uint32_t)fp_lifted_call(c, fp_original_L_000141B9EC90, (uint64_t)(uintptr_t)st, (uint64_t)(uintptr_t)&lifted, 0, 0);
            fp_theora_stats.lifted_s += fp_now() - t1;
            int same = r == 0 && lifted.y_width == yuv->y_width && lifted.y_height == yuv->y_height && lifted.uv_width == yuv->uv_width &&
                       lifted.uv_height == yuv->uv_height &&
                       fp_plane_equal(yuv->y, yuv->y_stride, lifted.y, lifted.y_stride, yuv->y_width, yuv->y_height) &&
                       fp_plane_equal(yuv->u, yuv->uv_stride, lifted.u, lifted.uv_stride, yuv->uv_width, yuv->uv_height) &&
                       fp_plane_equal(yuv->v, yuv->uv_stride, lifted.v, lifted.uv_stride, yuv->uv_width, yuv->uv_height);
            fp_theora_stats.verified++;
            if (!same && ++fp_theora_stats.mismatches <= 8)
                fprintf(stderr, "THEORA_NATIVE MISMATCH frame=%llu %dx%d lifted %dx%d result=%d\n", fp_theora_stats.frames,
                        yuv->y_width, yuv->y_height, lifted.y_width, lifted.y_height, r);
        }
    }
    fp_theora_report();
    RETURN_TO_GUEST((uint32_t)result);
}
void L_000141B9EE90(CPU *c) {   /* double theora_granule_time(theora_state *, ogg_int64_t) */
    LIFTED_ONLY(L_000141B9EE90);
    double t = theora_granule_time((theora_state *)(uintptr_t)A1, (ogg_int64_t)A2);
    c->xmm[0].u64[1] = 0;
    c->xmm[0].f64[0] = t;
    c->rip = pop64(c);
}
void L_000141B9F010(CPU *c) {   /* theora_control(theora_state *, int, void *, size_t) */
    LIFTED_ONLY(L_000141B9F010);
    uint64_t th = A1, req = A2, buf = A3, size = A4;
    int result = theora_control((theora_state *)(uintptr_t)th, (int)(int32_t)(uint32_t)req, (void *)(uintptr_t)buf, (size_t)size);
    if (fp_theora_verify) {
        void *st = fp_shadow(th, sizeof(theora_state), 0);
        if (st) fp_lifted_call(c, fp_original_L_000141B9F010, (uint64_t)(uintptr_t)st, req, buf, size);
    }
    RETURN_TO_GUEST((uint32_t)result);
}
void L_000141B9EF30(CPU *c) {   /* theora_clear(theora_state *) */
    LIFTED_ONLY(L_000141B9EF30);
    uint64_t th = A1;
    theora_clear((theora_state *)(uintptr_t)th);
    if (fp_theora_verify) {
        void *st = fp_shadow(th, sizeof(theora_state), 0);
        if (st) fp_lifted_call(c, fp_original_L_000141B9EF30, (uint64_t)(uintptr_t)st, 0, 0, 0);
        fp_shadow_drop(th);
    }
    RETURN_TO_GUEST(0);
}
void L_000141B9EEE0(CPU *c) {   /* theora_info_clear(theora_info *) */
    LIFTED_ONLY(L_000141B9EEE0);
    uint64_t ci = A1;
    theora_info_clear((theora_info *)(uintptr_t)ci);
    if (fp_theora_verify) {
        void *si = fp_shadow(ci, sizeof(theora_info), 0);
        if (si) fp_lifted_call(c, fp_original_L_000141B9EEE0, (uint64_t)(uintptr_t)si, 0, 0, 0);
        fp_shadow_drop(ci);
    }
    RETURN_TO_GUEST(0);
}
void L_000141B9F000(CPU *c) {   /* theora_comment_clear(theora_comment *) */
    LIFTED_ONLY(L_000141B9F000);
    uint64_t cc = A1;
    theora_comment_clear((theora_comment *)(uintptr_t)cc);
    if (fp_theora_verify) {
        void *sc = fp_shadow(cc, sizeof(theora_comment), 0);
        if (sc) fp_lifted_call(c, fp_original_L_000141B9F000, (uint64_t)(uintptr_t)sc, 0, 0, 0);
        fp_shadow_drop(cc);
    }
    RETURN_TO_GUEST(0);
}
