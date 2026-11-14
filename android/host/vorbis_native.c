/* Native mdct_backward for the game's statically linked libvorbis (SoundEngine hot path).
 *
 * Measured 2026-11-06 on the test phone and tablet: SoundEngine used 25-45% of a big core,
 * about half of it in the translated libvorbis inverse MDCT (mdct_backward 0x141C5C490 and its
 * butterflies), where every loop iteration is a translated basic-block dispatch.
 *
 * The functions below are the float path of libvorbis 1.3.7 lib/mdct.c (BSD-3-Clause, notice
 * below). The guest code at 0x141C5C490 is that function compiled by MSVC with scalar SSE:
 * the same expressions, each a single-precision IEEE operation. This file is compiled with
 * -ffp-contract=off (no fused multiply-add), so every result is the same IEEE operation on
 * the same operands. mdct_lookup has the same layout on Win64 and ARM64 (no `long` fields):
 * n@0, log2n@4, trig@8, bitrev@16, scale@24, as the guest code reads it.
 *
 * Self-check: the first VORBIS_VERIFY_CALLS calls of a process (or every call with
 * debug.fp.vorbis_verify=1) also run the translated original on a copy of the output and
 * compare all n floats bit for bit. One mismatch disables the native path for the process
 * (VORBIS_MDCT_MISMATCH ...); debug.fp.vorbis_native=0 disables it from the start.
 *
 * The three codebook decoders further down are
 * self-checked the same way (VORBIS_CODEBOOK_VERIFIED / VORBIS_CODEBOOK_MISMATCH);
 * debug.fp.vorbis_codebook=0 keeps only the native MDCT.
 *
 * Copyright (c) 2002-2020 Xiph.org Foundation
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met: redistributions of source code
 * must retain the above copyright notice, this list of conditions and the following
 * disclaimer; redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other materials
 * provided with the distribution; neither the name of the Xiph.org Foundation nor the names
 * of its contributors may be used to endorse or promote products derived from this software
 * without specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 * HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "vorbis_native.h"
#include <inttypes.h>
#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

typedef struct {
    int n;
    int log2n;
    float *trig;
    int *bitrev;
    float scale;
} mdct_lookup;
_Static_assert(sizeof(mdct_lookup) == 32, "guest mdct_lookup layout");

#define cPI3_8 .38268343236508977175F
#define cPI2_8 .70710678118654752441F
#define cPI1_8 .92387953251128675613F
#define HALVE(x) ((x)*.5f)

static inline void mdct_butterfly_8(float *x) {
    float r0 = x[6] + x[2];
    float r1 = x[6] - x[2];
    float r2 = x[4] + x[0];
    float r3 = x[4] - x[0];
    x[6] = r0 + r2;
    x[4] = r0 - r2;
    r0 = x[5] - x[1];
    r2 = x[7] - x[3];
    x[0] = r1 + r0;
    x[2] = r1 - r0;
    r0 = x[5] + x[1];
    r1 = x[7] + x[3];
    x[3] = r2 + r3;
    x[1] = r2 - r3;
    x[7] = r1 + r0;
    x[5] = r1 - r0;
}

static inline void mdct_butterfly_16(float *x) {
    float r0 = x[1] - x[9];
    float r1 = x[0] - x[8];
    x[8] += x[0];
    x[9] += x[1];
    x[0] = (r0 + r1) * cPI2_8;
    x[1] = (r0 - r1) * cPI2_8;
    r0 = x[3] - x[11];
    r1 = x[10] - x[2];
    x[10] += x[2];
    x[11] += x[3];
    x[2] = r0;
    x[3] = r1;
    r0 = x[12] - x[4];
    r1 = x[13] - x[5];
    x[12] += x[4];
    x[13] += x[5];
    x[4] = (r0 - r1) * cPI2_8;
    x[5] = (r0 + r1) * cPI2_8;
    r0 = x[14] - x[6];
    r1 = x[15] - x[7];
    x[14] += x[6];
    x[15] += x[7];
    x[6] = r0;
    x[7] = r1;
    mdct_butterfly_8(x);
    mdct_butterfly_8(x + 8);
}

static inline void mdct_butterfly_32(float *x) {
    float r0 = x[30] - x[14];
    float r1 = x[31] - x[15];
    x[30] += x[14];
    x[31] += x[15];
    x[14] = r0;
    x[15] = r1;
    r0 = x[28] - x[12];
    r1 = x[29] - x[13];
    x[28] += x[12];
    x[29] += x[13];
    x[12] = r0 * cPI1_8 - r1 * cPI3_8;
    x[13] = r0 * cPI3_8 + r1 * cPI1_8;
    r0 = x[26] - x[10];
    r1 = x[27] - x[11];
    x[26] += x[10];
    x[27] += x[11];
    x[10] = (r0 - r1) * cPI2_8;
    x[11] = (r0 + r1) * cPI2_8;
    r0 = x[24] - x[8];
    r1 = x[25] - x[9];
    x[24] += x[8];
    x[25] += x[9];
    x[8] = r0 * cPI3_8 - r1 * cPI1_8;
    x[9] = r1 * cPI3_8 + r0 * cPI1_8;
    r0 = x[22] - x[6];
    r1 = x[7] - x[23];
    x[22] += x[6];
    x[23] += x[7];
    x[6] = r1;
    x[7] = r0;
    r0 = x[4] - x[20];
    r1 = x[5] - x[21];
    x[20] += x[4];
    x[21] += x[5];
    x[4] = r1 * cPI1_8 + r0 * cPI3_8;
    x[5] = r1 * cPI3_8 - r0 * cPI1_8;
    r0 = x[2] - x[18];
    r1 = x[3] - x[19];
    x[18] += x[2];
    x[19] += x[3];
    x[2] = (r1 + r0) * cPI2_8;
    x[3] = (r1 - r0) * cPI2_8;
    r0 = x[0] - x[16];
    r1 = x[1] - x[17];
    x[16] += x[0];
    x[17] += x[1];
    x[0] = r1 * cPI3_8 + r0 * cPI1_8;
    x[1] = r1 * cPI1_8 - r0 * cPI3_8;
    mdct_butterfly_16(x);
    mdct_butterfly_16(x + 16);
}

static inline void mdct_butterfly_first(const float *T, float *x, int points) {
    float *x1 = x + points - 8;
    float *x2 = x + (points >> 1) - 8;
    float r0, r1;
    do {
        r0 = x1[6] - x2[6];
        r1 = x1[7] - x2[7];
        x1[6] += x2[6];
        x1[7] += x2[7];
        x2[6] = r1 * T[1] + r0 * T[0];
        x2[7] = r1 * T[0] - r0 * T[1];
        r0 = x1[4] - x2[4];
        r1 = x1[5] - x2[5];
        x1[4] += x2[4];
        x1[5] += x2[5];
        x2[4] = r1 * T[5] + r0 * T[4];
        x2[5] = r1 * T[4] - r0 * T[5];
        r0 = x1[2] - x2[2];
        r1 = x1[3] - x2[3];
        x1[2] += x2[2];
        x1[3] += x2[3];
        x2[2] = r1 * T[9] + r0 * T[8];
        x2[3] = r1 * T[8] - r0 * T[9];
        r0 = x1[0] - x2[0];
        r1 = x1[1] - x2[1];
        x1[0] += x2[0];
        x1[1] += x2[1];
        x2[0] = r1 * T[13] + r0 * T[12];
        x2[1] = r1 * T[12] - r0 * T[13];
        x1 -= 8;
        x2 -= 8;
        T += 16;
    } while (x2 >= x);
}

static inline void mdct_butterfly_generic(const float *T, float *x, int points, int trigint) {
    float *x1 = x + points - 8;
    float *x2 = x + (points >> 1) - 8;
    float r0, r1;
    do {
        r0 = x1[6] - x2[6];
        r1 = x1[7] - x2[7];
        x1[6] += x2[6];
        x1[7] += x2[7];
        x2[6] = r1 * T[1] + r0 * T[0];
        x2[7] = r1 * T[0] - r0 * T[1];
        T += trigint;
        r0 = x1[4] - x2[4];
        r1 = x1[5] - x2[5];
        x1[4] += x2[4];
        x1[5] += x2[5];
        x2[4] = r1 * T[1] + r0 * T[0];
        x2[5] = r1 * T[0] - r0 * T[1];
        T += trigint;
        r0 = x1[2] - x2[2];
        r1 = x1[3] - x2[3];
        x1[2] += x2[2];
        x1[3] += x2[3];
        x2[2] = r1 * T[1] + r0 * T[0];
        x2[3] = r1 * T[0] - r0 * T[1];
        T += trigint;
        r0 = x1[0] - x2[0];
        r1 = x1[1] - x2[1];
        x1[0] += x2[0];
        x1[1] += x2[1];
        x2[0] = r1 * T[1] + r0 * T[0];
        x2[1] = r1 * T[0] - r0 * T[1];
        T += trigint;
        x1 -= 8;
        x2 -= 8;
    } while (x2 >= x);
}

static void mdct_butterflies(const mdct_lookup *init, float *x, int points) {
    const float *T = init->trig;
    int stages = init->log2n - 5;
    int i, j;
    if (--stages > 0) mdct_butterfly_first(T, x, points);
    for (i = 1; --stages > 0; i++)
        for (j = 0; j < (1 << i); j++)
            mdct_butterfly_generic(T, x + (points >> i) * j, points >> i, 4 << i);
    for (j = 0; j < points; j += 32) mdct_butterfly_32(x + j);
}

static void mdct_bitreverse(const mdct_lookup *init, float *x) {
    int n = init->n;
    const int *bit = init->bitrev;
    float *w0 = x;
    float *w1 = x = w0 + (n >> 1);
    const float *T = init->trig + n;
    do {
        float *x0 = x + bit[0];
        float *x1 = x + bit[1];
        float r0 = x0[1] - x1[1];
        float r1 = x0[0] + x1[0];
        float r2 = r1 * T[0] + r0 * T[1];
        float r3 = r1 * T[1] - r0 * T[0];
        w1 -= 4;
        r0 = HALVE(x0[1] + x1[1]);
        r1 = HALVE(x0[0] - x1[0]);
        w0[0] = r0 + r2;
        w1[2] = r0 - r2;
        w0[1] = r1 + r3;
        w1[3] = r3 - r1;
        x0 = x + bit[2];
        x1 = x + bit[3];
        r0 = x0[1] - x1[1];
        r1 = x0[0] + x1[0];
        r2 = r1 * T[2] + r0 * T[3];
        r3 = r1 * T[3] - r0 * T[2];
        r0 = HALVE(x0[1] + x1[1]);
        r1 = HALVE(x0[0] - x1[0]);
        w0[2] = r0 + r2;
        w1[0] = r0 - r2;
        w0[3] = r1 + r3;
        w1[1] = r3 - r1;
        T += 4;
        bit += 4;
        w0 += 4;
    } while (w0 < w1);
}

static void mdct_backward(const mdct_lookup *init, const float *in, float *out) {
    int n = init->n;
    int n2 = n >> 1;
    int n4 = n >> 2;
    const float *iX = in + n2 - 7;
    float *oX = out + n2 + n4;
    const float *T = init->trig + n4;
    do {
        oX -= 4;
        oX[0] = -iX[2] * T[3] - iX[0] * T[2];
        oX[1] = iX[0] * T[3] - iX[2] * T[2];
        oX[2] = -iX[6] * T[1] - iX[4] * T[0];
        oX[3] = iX[4] * T[1] - iX[6] * T[0];
        iX -= 8;
        T += 4;
    } while (iX >= in);
    iX = in + n2 - 8;
    oX = out + n2 + n4;
    T = init->trig + n4;
    do {
        T -= 4;
        oX[0] = iX[4] * T[3] + iX[6] * T[2];
        oX[1] = iX[4] * T[2] - iX[6] * T[3];
        oX[2] = iX[0] * T[1] + iX[2] * T[0];
        oX[3] = iX[0] * T[0] - iX[2] * T[1];
        iX -= 8;
        oX += 4;
    } while (iX >= in);
    mdct_butterflies(init, out + n2, n2);
    mdct_bitreverse(init, out);
    {
        float *oX1 = out + n2 + n4;
        float *oX2 = out + n2 + n4;
        float *iY = out;
        T = init->trig + n2;
        do {
            oX1 -= 4;
            oX1[3] = iY[0] * T[1] - iY[1] * T[0];
            oX2[0] = -(iY[0] * T[0] + iY[1] * T[1]);
            oX1[2] = iY[2] * T[3] - iY[3] * T[2];
            oX2[1] = -(iY[2] * T[2] + iY[3] * T[3]);
            oX1[1] = iY[4] * T[5] - iY[5] * T[4];
            oX2[2] = -(iY[4] * T[4] + iY[5] * T[5]);
            oX1[0] = iY[6] * T[7] - iY[7] * T[6];
            oX2[3] = -(iY[6] * T[6] + iY[7] * T[7]);
            oX2 += 4;
            iY += 8;
            T += 8;
        } while (iY < oX1);
        iY = out + n2 + n4;
        oX1 = out + n4;
        oX2 = oX1;
        do {
            oX1 -= 4;
            iY -= 4;
            oX2[0] = -(oX1[3] = iY[3]);
            oX2[1] = -(oX1[2] = iY[2]);
            oX2[2] = -(oX1[1] = iY[1]);
            oX2[3] = -(oX1[0] = iY[0]);
            oX2 += 4;
        } while (oX2 < iY);
        iY = out + n2 + n4;
        oX1 = out + n2 + n4;
        oX2 = out + n2;
        do {
            oX1 -= 4;
            oX1[0] = iY[3];
            oX1[1] = iY[2];
            oX1[2] = iY[1];
            oX1[3] = iY[0];
            iY += 4;
        } while (oX1 > oX2);
    }
}

/* ---- codebook decoders: vorbis_book_decodev_set and vorbis_book_decodevv_add ----
 *
 * Measured 2026-11-06 with the native MDCT in place: vorbis_book_decodev_set (0x141C54E60,
 * called by floor0_inverse1) took about 20% of the SoundEngine thread on the test tablet and
 * vorbis_book_decodevv_add (0x141C54AF0, res2_inverse) about 15% on the test phone. Every decoded
 * value costs a guest call to oggpack_look (0x140DFE110) and a chain of translated blocks.
 *
 * The functions below are lib/codebook.c of libvorbis 1.3.7 and lib/bitwise.c of libogg as
 * the guest compiled them, ported from the translated code rather than from the sources: Win64
 * `long` is 32 bits (oggpack_buffer endbyte@0 endbit@4 buffer@8 ptr@16 storage@24; codebook
 * dim@0 used_entries@8 valuelist@0x18 codelist@0x20 dec_codelengths@0x30 dec_firsttable@0x38
 * dec_firsttablen@0x40 dec_maxlength@0x44), arithmetic wraps at 32 bits (-fwrapv), shift
 * counts are masked like x86 shifts, code lengths are signed char, the overflow state is
 * ptr NULL / endbyte storage / endbit 1, decodev_set copies raw 32-bit words and
 * decodevv_add adds with the SSE NaN rules of the translated addss (frostpunk_addss).
 */

typedef struct {
    int32_t endbyte;
    int32_t endbit;
    uint64_t buffer;
    uint64_t ptr;
    int32_t storage;
    int32_t padding;
} ogg_bits;
_Static_assert(sizeof(ogg_bits) == 32 && offsetof(ogg_bits, ptr) == 16 && offsetof(ogg_bits, storage) == 24,
               "guest oggpack_buffer layout");

typedef struct {
    int32_t dim;
    int32_t entries;
    int32_t used_entries;
    int32_t padding;
    uint64_t static_book;
    uint64_t valuelist;       /* float * */
    uint64_t codelist;        /* uint32_t *, bit-reversed code words */
    uint64_t dec_index;
    uint64_t dec_codelengths; /* signed char * */
    uint64_t dec_firsttable;  /* uint32_t * */
    int32_t dec_firsttablen;
    int32_t dec_maxlength;
} codebook;
_Static_assert(offsetof(codebook, valuelist) == 0x18 && offsetof(codebook, codelist) == 0x20 &&
               offsetof(codebook, dec_codelengths) == 0x30 && offsetof(codebook, dec_firsttable) == 0x38 &&
               offsetof(codebook, dec_firsttablen) == 0x40 && offsetof(codebook, dec_maxlength) == 0x44,
               "guest codebook layout");

/* oggpack_look, 0x140DFE110. The mask is libogg's mask[] (the guest table at 0x1421E56C0). */
static inline int32_t bits_look(const ogg_bits *b, int32_t bits) {
    if ((uint32_t)bits > 32u) return -1;
    uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1u;
    int32_t endbit = b->endbit;
    int32_t total = bits + endbit;
    if (b->endbyte >= b->storage - 4) {
        if (b->endbyte > b->storage - ((total + 7) >> 3)) return -1;
        if (!total) return 0;
    }
    uint64_t p = b->ptr;
    uint32_t ret = (uint32_t)rd8(p) >> (endbit & 31);
    if (total > 8) {
        ret |= (uint32_t)rd8(p + 1) << ((8 - endbit) & 31);
        if (total > 16) {
            ret |= (uint32_t)rd8(p + 2) << ((16 - endbit) & 31);
            if (total > 24) {
                ret |= (uint32_t)rd8(p + 3) << ((24 - endbit) & 31);
                if (total > 32 && endbit) ret |= (uint32_t)rd8(p + 4) << ((32 - endbit) & 31);
            }
        }
    }
    return (int32_t)(ret & mask);
}

/* oggpack_adv, inlined by the guest compiler. */
static inline void bits_adv(ogg_bits *b, int32_t bits) {
    int32_t total = bits + b->endbit;
    if (b->endbyte > b->storage - ((total + 7) >> 3)) {
        b->ptr = 0;
        b->endbyte = b->storage;
        b->endbit = 1;
        return;
    }
    int32_t bytes = total / 8;
    b->ptr += (uint64_t)(int64_t)bytes;
    b->endbyte += bytes;
    b->endbit = total & 7;
}

/* decode_packed_entry_number, inlined into both decoders. */
static inline int32_t decode_entry(const codebook *book, ogg_bits *b) {
    int32_t read = book->dec_maxlength;
    int32_t lo, hi;
    int32_t lok = bits_look(b, book->dec_firsttablen);
    if (lok >= 0) {
        int32_t entry = (int32_t)rd32(book->dec_firsttable + 4u * (uint64_t)(int64_t)lok);
        if (entry < 0) {
            lo = (entry >> 15) & 0x7fff;
            hi = book->used_entries - (entry & 0x7fff);
        } else {
            bits_adv(b, (int8_t)rd8(book->dec_codelengths + (uint64_t)(int64_t)entry - 1u));
            return entry - 1;
        }
    } else {
        lo = 0;
        hi = book->used_entries;
    }
    lok = bits_look(b, read);
    while (lok < 0 && read > 1) lok = bits_look(b, --read);
    if (lok < 0) return -1;
    uint32_t testword = __builtin_bitreverse32((uint32_t)lok);
    while (hi - lo > 1) {
        int32_t p = (hi - lo) >> 1;
        int32_t test = rd32(book->codelist + 4u * (uint64_t)(int64_t)(lo + p)) > testword;
        lo += p & (test - 1);
        hi -= p & -test;
    }
    int32_t length = (int8_t)rd8(book->dec_codelengths + (uint64_t)(int64_t)lo);
    if (length <= read) {
        bits_adv(b, length);
        return lo;
    }
    bits_adv(b, read);
    return -1;
}

static inline uint64_t book_values(const codebook *book, int32_t entry) {
    return book->valuelist + 4u * (uint64_t)(int64_t)(int32_t)((uint32_t)book->dim * (uint32_t)entry);
}

/* decodev_add writes complete vectors, including a final partial partition. */
static int32_t book_decodev_add(const codebook *book,uint64_t a,ogg_bits *b,int32_t n) {
    if(book->used_entries<=0 || n<=0)return 0;
    int64_t i=0;
    do {
        int32_t entry=decode_entry(book,b);if(entry==-1)return -1;
        uint64_t values=book_values(book,entry);
        for(int32_t j=0;j<book->dim;++j,++i){
            uint64_t out=a+4u*(uint64_t)i,value=values+4u*(uint64_t)(int64_t)j;
            float first,second;
            /* The MSVC small-vector switch reverses ADDSS operands; NaN payloads depend on it. */
            if(book->dim<=8){first=rdss(value);second=rdss(out);}
            else{first=rdss(out);second=rdss(value);}
            wrss(out,frostpunk_addss(first,second));
        }
    }while(i<n);
    return 0;
}

/* vorbis_book_decodev_set, 0x141C54E60. */
static int32_t book_decodev_set(const codebook *book, uint64_t a, ogg_bits *b, int32_t n) {
    if (n <= 0) return 0;
    if (book->used_entries <= 0) {
        memset((void *)(uintptr_t)a, 0, (size_t)(uint32_t)n * 4u); /* rep stosd */
        return 0;
    }
    int64_t i = 0;
    do {
        int32_t entry = decode_entry(book, b);
        if (entry == -1) return -1;
        uint64_t t = book_values(book, entry);
        for (int32_t j = 0; i < n && j < book->dim; ++i, ++j)
            wr32(a + 4u * (uint64_t)i, rd32(t + 4u * (uint64_t)(int64_t)j));
    } while (i < n);
    return 0;
}

/* vorbis_book_decodevv_add, 0x141C54AF0, for used_entries > 0 and ch > 0 (checked by the
 * caller: the original tests used_entries first and divides by ch only after it). */
static int32_t book_decodevv_add(const codebook *book, uint64_t a, int32_t offset, int32_t ch, ogg_bits *b,
                                 int32_t n) {
    int32_t first = offset / ch, m = (offset + n) / ch;
    if (first >= m) return 0;
    int64_t i = first;
    int32_t chptr = 0;
    do {
        int32_t entry = decode_entry(book, b);
        if (entry == -1) return -1;
        uint64_t t = book_values(book, entry);
        for (int32_t j = 0; j < book->dim; ++j) {
            uint64_t out = rd64(a + 8u * (uint64_t)(int64_t)chptr) + 4u * (uint64_t)i;
            wrss(out, frostpunk_addss(rdss(out), rdss(t + 4u * (uint64_t)(int64_t)j)));
            if (++chptr == ch) {
                chptr = 0;
                ++i;
            }
        }
    } while (i < m);
    return 0;
}

/* ---- guest entries, self-checks, routing ---- */

void L_000141C5C490(CPU *c); /* translated originals (hidden in the same DSO) */
void L_000141C54E60(CPU *c);
void L_000141C551D0(CPU *c);
void L_000141C54AF0(CPU *c);

#define VORBIS_VERIFY_CALLS 512u
#define VORBIS_MAX_N 8192
#define CODEBOOK_VERIFY_CALLS 4096u
#define CODEBOOK_VERIFY_VALUES (1u << 20) /* larger calls are not shadowed (run the original) */
#define CODEBOOK_VERIFY_ENTRIES 4096u
static atomic_int vorbis_configured = 0;
static atomic_int native_state = 0;   /* mdct_backward: 1 enabled, -1 disabled */
static atomic_int codebook_state = 0; /* both codebook decoders: 1 enabled, -1 disabled */
static atomic_int verify_always = 0;
static _Atomic unsigned verified_calls, mismatches;
static _Atomic unsigned long long native_calls;
static _Atomic unsigned codebook_verified[3];
static _Atomic unsigned long long codebook_calls[3];
static const char *const codebook_names[3] = {"decodev_set", "decodevv_add", "decodev_add"};

static int property_int(const char *name, int fallback) {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, value) > 0 && value[0]) return atoi(value);
#endif
    (void)name;
    return fallback;
}

static void vorbis_configure(void) {
    if (atomic_load_explicit(&vorbis_configured, memory_order_acquire)) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&vorbis_configured, &expected, -1)) {
        while (atomic_load_explicit(&vorbis_configured, memory_order_acquire) < 0) {}
        return;
    }
    int mdct = property_int("debug.fp.vorbis_native", 1) != 0;
    int books = mdct && property_int("debug.fp.vorbis_codebook", 1) != 0;
    atomic_store(&verify_always, property_int("debug.fp.vorbis_verify", 0) != 0);
    atomic_store(&native_state, mdct ? 1 : -1);
    atomic_store(&codebook_state, books ? 1 : -1);
    fprintf(stderr, "VORBIS_NATIVE mdct_backward=%d codebook=%d verify_first=%u/%u verify_always=%d\n", mdct, books,
            VORBIS_VERIFY_CALLS, CODEBOOK_VERIFY_CALLS, atomic_load(&verify_always));
    atomic_store_explicit(&vorbis_configured, 1, memory_order_release);
}

static void guest_return(CPU *c) { c->rip = pop64(c); }

static void native_mdct_backward(CPU *c) {
    const mdct_lookup *lookup = (const mdct_lookup *)(uintptr_t)c->rcx;
    const float *in = (const float *)(uintptr_t)c->rdx;
    float *out = (float *)(uintptr_t)c->r8;
    int n = lookup->n;
    int verify = atomic_load_explicit(&verify_always, memory_order_relaxed) ||
                 atomic_load_explicit(&verified_calls, memory_order_relaxed) < VORBIS_VERIFY_CALLS;
    if (verify && n >= 64 && n <= VORBIS_MAX_N && !(n & (n - 1))) {
        /* Translated original on a private output copy, below the live guest stack. */
        float *expected = malloc((size_t)n * sizeof(float));
        if (expected) {
            memcpy(expected, out, (size_t)n * sizeof(float));
            CPU original = *c;
            original.rsp = (original.rsp - 1024) & ~UINT64_C(15);
            original.r8 = (uint64_t)(uintptr_t)expected;
            push64(&original, UINT64_C(0xe5ee0010));
            L_000141C5C490(&original);
            mdct_backward(lookup, in, out);
            unsigned count = atomic_fetch_add(&verified_calls, 1) + 1;
            if (memcmp(expected, out, (size_t)n * sizeof(float))) {
                int at = 0;
                while (at < n && !memcmp(&expected[at], &out[at], sizeof(float))) ++at;
                uint32_t want = 0, got = 0;
                memcpy(&want, &expected[at], 4); memcpy(&got, &out[at], 4);
                atomic_fetch_add(&mismatches, 1);
                atomic_store(&native_state, -1);
                fprintf(stderr, "VORBIS_MDCT_MISMATCH n=%d index=%d expected=0x%08x native=0x%08x call=%u native_disabled=1\n",
                        n, at, want, got, count);
                memcpy(out, expected, (size_t)n * sizeof(float)); /* keep the original result */
            } else if (count == VORBIS_VERIFY_CALLS) {
                fprintf(stderr, "VORBIS_MDCT_VERIFIED calls=%u bit_identical=1 last_n=%d\n", count, n);
            }
            free(expected);
            atomic_fetch_add_explicit(&native_calls, 1, memory_order_relaxed);
            guest_return(c);
            return;
        }
    }
    mdct_backward(lookup, in, out);
    atomic_fetch_add_explicit(&native_calls, 1, memory_order_relaxed);
    guest_return(c);
}

/* Codebook self-check: during the first CODEBOOK_VERIFY_CALLS calls of each decoder (every
 * call with debug.fp.vorbis_verify=1) the translated original runs on the live state and does
 * the real work, while the native decoder runs on copies (bitstream state, outputs) and
 * predicts it. A different return value, bitstream state or output word disables all native
 * codebook decoders for the process; the original's result is already in place. */
static int codebook_verify_now(int fn) {
    return atomic_load_explicit(&verify_always, memory_order_relaxed) ||
           atomic_load_explicit(&codebook_verified[fn], memory_order_relaxed) < CODEBOOK_VERIFY_CALLS;
}

static int codebook_check(int32_t result, int32_t predicted, const ogg_bits *actual, const ogg_bits *shadow,
                          const char **field, uint64_t *want, uint64_t *got) {
    if (result != predicted) { *field = "return"; *want = (uint32_t)result; *got = (uint32_t)predicted; return 1; }
    if (actual->endbyte != shadow->endbyte) {
        *field = "endbyte"; *want = (uint32_t)actual->endbyte; *got = (uint32_t)shadow->endbyte; return 1;
    }
    if (actual->endbit != shadow->endbit) {
        *field = "endbit"; *want = (uint32_t)actual->endbit; *got = (uint32_t)shadow->endbit; return 1;
    }
    if (actual->ptr != shadow->ptr) { *field = "ptr"; *want = actual->ptr; *got = shadow->ptr; return 1; }
    if (actual->buffer != shadow->buffer || actual->storage != shadow->storage) {
        *field = "buffer"; *want = actual->buffer; *got = shadow->buffer; return 1;
    }
    return 0;
}

static void codebook_report(int fn, unsigned call, const char *field, int channel, long long index, uint64_t want,
                            uint64_t got) {
    if (field) {
        atomic_store(&codebook_state, -1);
        atomic_fetch_add(&mismatches, 1);
        fprintf(stderr,
                "VORBIS_CODEBOOK_MISMATCH fn=%s field=%s channel=%d index=%lld expected=0x%" PRIx64
                " native=0x%" PRIx64 " call=%u native_disabled=1\n",
                codebook_names[fn], field, channel, index, want, got, call);
    } else if (call == CODEBOOK_VERIFY_CALLS) {
        fprintf(stderr, "VORBIS_CODEBOOK_VERIFIED fn=%s calls=%u bit_identical=1\n", codebook_names[fn], call);
    }
}

static void native_decodev_set(CPU *c) {
    const codebook *book = (const codebook *)(uintptr_t)c->rcx;
    uint64_t a = c->rdx;
    ogg_bits *b = (ogg_bits *)(uintptr_t)c->r8;
    int32_t n = (int32_t)(uint32_t)c->r9;
    if (n > 0 && (uint32_t)n <= CODEBOOK_VERIFY_VALUES && codebook_verify_now(0)) {
        uint32_t *predicted = malloc((size_t)n * 4u);
        if (predicted) {
            memcpy(predicted, (const void *)(uintptr_t)a, (size_t)n * 4u);
            ogg_bits shadow = *b;
            int32_t native_result = book_decodev_set(book, (uint64_t)(uintptr_t)predicted, &shadow, n);
            L_000141C54E60(c); /* the original does the real work and returns to the caller */
            int32_t result = (int32_t)(uint32_t)c->rax;
            unsigned call = atomic_fetch_add(&codebook_verified[0], 1) + 1;
            const char *field = NULL;
            long long index = -1;
            uint64_t want = 0, got = 0;
            if (!codebook_check(result, native_result, b, &shadow, &field, &want, &got)) {
                const uint32_t *actual = (const uint32_t *)(uintptr_t)a;
                for (int32_t k = 0; k < n; ++k)
                    if (actual[k] != predicted[k]) {
                        field = "value"; index = k; want = actual[k]; got = predicted[k];
                        break;
                    }
            }
            codebook_report(0, call, field, -1, index, want, got);
            free(predicted);
            return;
        }
    }
    c->rax = (uint32_t)book_decodev_set(book, a, b, n);
    atomic_fetch_add_explicit(&codebook_calls[0], 1, memory_order_relaxed);
    guest_return(c);
}

static void native_decodev_add(CPU *c) {
    const codebook *book = (const codebook *)(uintptr_t)c->rcx;
    uint64_t a = c->rdx;
    ogg_bits *b = (ogg_bits *)(uintptr_t)c->r8;
    int32_t n = (int32_t)(uint32_t)c->r9;
    int saved_errno=errno;
    if(book->dim<=0 || (uint32_t)book->dim>CODEBOOK_VERIFY_VALUES || n>(int32_t)CODEBOOK_VERIFY_VALUES ||
       (book->used_entries>0 && book->used_entries>INT32_MAX/book->dim)) {L_000141C551D0(c);return;}
    size_t values=n>0?((uint64_t)(uint32_t)n+(uint32_t)book->dim-1)/(uint32_t)book->dim*(uint32_t)book->dim:0;
    if (n > 0 && values <= CODEBOOK_VERIFY_VALUES && codebook_verify_now(2)) {
        uint32_t *predicted = malloc(values * 4u);
        if (predicted) {
            memcpy(predicted, (const void *)(uintptr_t)a, values * 4u);
            ogg_bits shadow = *b;
            int32_t native_result = book_decodev_add(book, (uint64_t)(uintptr_t)predicted, &shadow, n);
            L_000141C551D0(c); /* the original does the real work and returns to the caller */
            int32_t result = (int32_t)(uint32_t)c->rax;
            unsigned call = atomic_fetch_add(&codebook_verified[2], 1) + 1;
            const char *field = NULL;
            long long index = -1;
            uint64_t want = 0, got = 0;
            if (!codebook_check(result, native_result, b, &shadow, &field, &want, &got)) {
                const uint32_t *actual = (const uint32_t *)(uintptr_t)a;
                for (int32_t k = 0; (size_t)k < values; ++k)
                    if (actual[k] != predicted[k]) {
                        field = "value"; index = k; want = actual[k]; got = predicted[k];
                        break;
                    }
            }
            codebook_report(2, call, field, -1, index, want, got);
            free(predicted);
            errno=saved_errno;return;
        }
    }
    c->rax = (uint32_t)book_decodev_add(book, a, b, n);
    atomic_fetch_add_explicit(&codebook_calls[2], 1, memory_order_relaxed);
    guest_return(c);errno=saved_errno;
}

/* decodevv_add self-check. Pass 1 decodes on a copy of the bitstream state to find the
 * touched output window (rows [first, last] of channels [0, channels)); pass 2 runs
 * book_decodevv_add itself on a copy of that window through a substitute channel-pointer
 * array, so the production code path is the one being compared. Returns 0 when the call
 * cannot be shadowed (the caller then runs the original without counting it). */
static int verify_decodevv_add(CPU *c, const codebook *book, uint64_t a, int32_t offset, int32_t ch, ogg_bits *b,
                               int32_t n) {
    int32_t first = offset / ch, m = (offset + n) / ch;
    ogg_bits probe = *b;
    int64_t i = first, last = (int64_t)first - 1;
    int32_t chptr = 0, channels = 0;
    unsigned decoded = 0;
    if (first < m) {
        do {
            int32_t entry = decode_entry(book, &probe);
            if (entry == -1) break;
            if (++decoded > CODEBOOK_VERIFY_ENTRIES) return 0;
            for (int32_t j = 0; j < book->dim; ++j) {
                if (i > last) last = i;
                if (chptr + 1 > channels) channels = chptr + 1;
                if (++chptr == ch) {
                    chptr = 0;
                    ++i;
                }
            }
            if (last - first + 1 > (int64_t)CODEBOOK_VERIFY_VALUES) return 0;
        } while (i < m);
    }
    size_t rows = last >= first ? (size_t)(last - first + 1) : 0;
    if (rows * (size_t)channels > CODEBOOK_VERIFY_VALUES) return 0;
    float *window = rows ? malloc(rows * (size_t)channels * sizeof(float)) : NULL;
    uint64_t *pointers = calloc((size_t)ch, sizeof(uint64_t));
    if ((rows && !window) || !pointers) {
        free(window);
        free(pointers);
        return 0;
    }
    for (int32_t k = 0; k < channels; ++k) {
        uint64_t row0 = rd64(a + 8u * (uint64_t)k) + 4u * (uint64_t)(int64_t)first;
        memcpy(window + (size_t)k * rows, (const void *)(uintptr_t)row0, rows * sizeof(float));
        pointers[k] = (uint64_t)(uintptr_t)(window + (size_t)k * rows) - 4u * (uint64_t)(int64_t)first;
    }
    ogg_bits shadow = *b;
    int32_t native_result = book_decodevv_add(book, (uint64_t)(uintptr_t)pointers, offset, ch, &shadow, n);
    L_000141C54AF0(c); /* the original does the real work and returns to the caller */
    int32_t result = (int32_t)(uint32_t)c->rax;
    unsigned call = atomic_fetch_add(&codebook_verified[1], 1) + 1;
    const char *field = NULL;
    int channel = -1;
    long long index = -1;
    uint64_t want = 0, got = 0;
    if (!codebook_check(result, native_result, b, &shadow, &field, &want, &got) &&
        (probe.endbyte != shadow.endbyte || probe.endbit != shadow.endbit || probe.ptr != shadow.ptr)) {
        field = "probe"; want = (uint32_t)probe.endbyte; got = (uint32_t)shadow.endbyte;
    }
    for (int32_t k = 0; !field && k < channels; ++k) {
        const uint32_t *actual =
            (const uint32_t *)(uintptr_t)(rd64(a + 8u * (uint64_t)k) + 4u * (uint64_t)(int64_t)first);
        const uint32_t *predicted = (const uint32_t *)(window + (size_t)k * rows);
        for (size_t r = 0; r < rows; ++r)
            if (actual[r] != predicted[r]) {
                field = "value"; channel = k; index = (long long)(first + (int64_t)r);
                want = actual[r]; got = predicted[r];
                break;
            }
    }
    codebook_report(1, call, field, channel, index, want, got);
    free(window);
    free(pointers);
    return 1;
}

static void native_decodevv_add(CPU *c) {
    const codebook *book = (const codebook *)(uintptr_t)c->rcx;
    uint64_t a = c->rdx;
    int32_t offset = (int32_t)(uint32_t)c->r8;
    int32_t ch = (int32_t)(uint32_t)c->r9;
    ogg_bits *b = (ogg_bits *)(uintptr_t)rd64(c->rsp + 0x28);
    int32_t n = (int32_t)rd32(c->rsp + 0x30);
    if (book->used_entries <= 0) {
        c->rax = 0;
        guest_return(c);
        return;
    }
    if (ch <= 0) { /* the translated division semantics (ch 0, INT_MIN / -1) stay the original's */
        L_000141C54AF0(c);
        return;
    }
    if (codebook_verify_now(1)) {
        if (!verify_decodevv_add(c, book, a, offset, ch, b, n)) L_000141C54AF0(c);
        return;
    }
    c->rax = (uint32_t)book_decodevv_add(book, a, offset, ch, b, n);
    atomic_fetch_add_explicit(&codebook_calls[1], 1, memory_order_relaxed);
    guest_return(c);
}

void (*fp_vorbis_native_lookup(uint64_t target))(CPU *) {
    vorbis_configure();
    if (target == UINT64_C(0x141C5C490))
        return atomic_load_explicit(&native_state, memory_order_relaxed) > 0 ? native_mdct_backward : NULL;
    if (atomic_load_explicit(&codebook_state, memory_order_relaxed) <= 0) return NULL;
    if (target == UINT64_C(0x141C54E60)) return native_decodev_set;
    if (target == UINT64_C(0x141C54AF0)) return native_decodevv_add;
    if (target == UINT64_C(0x141C551D0)) return native_decodev_add;
    return NULL;
}

