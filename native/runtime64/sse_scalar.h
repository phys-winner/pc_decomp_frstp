#ifndef FROSTPUNK_SSE_SCALAR_H
#define FROSTPUNK_SSE_SCALAR_H

/* SSE preserves a source NaN payload and uses negative indefinite for invalid
 * finite/infinite operations; ARM floating-point arithmetic differs here. */
/* shortcut: default nearest rounding with masked exceptions; model MXCSR
 * rounding, DAZ/FTZ and sticky flags before non-default guest modes are used. */
static inline float frostpunk_sse_result(float a, float b, float result) {
    uint32_t aa, bb, rr;
    memcpy(&aa, &a, 4); memcpy(&bb, &b, 4); memcpy(&rr, &result, 4);
    if ((aa & 0x7FFFFFFFu) > 0x7F800000u) rr = aa | 0x00400000u;
    else if ((bb & 0x7FFFFFFFu) > 0x7F800000u) rr = bb | 0x00400000u;
    else if ((rr & 0x7FFFFFFFu) > 0x7F800000u) rr = 0xFFC00000u;
    memcpy(&result, &rr, 4);
    return result;
}
#if defined(FROSTPUNK_WINDOWS_HOST) && defined(__clang__) && (defined(_M_X64) || defined(__x86_64__))
static inline float frostpunk_addss(float a,float b) {
    __asm__ volatile("addss %1,%0" : "+x"(a) : "x"(b));return a;
}
static inline float frostpunk_mulss(float a,float b) {
    __asm__ volatile("mulss %1,%0" : "+x"(a) : "x"(b));return a;
}
#else
static inline float frostpunk_addss(float a, float b) { return frostpunk_sse_result(a, b, a + b); }
static inline float frostpunk_mulss(float a, float b) { return frostpunk_sse_result(a, b, a * b); }

#endif

#endif
