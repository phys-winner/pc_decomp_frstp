/* Android-only landing pads for the currently supported guest boundary. */
#ifndef FP_ANDROID_EH_ENTRY_NOMASK_H
#define FP_ANDROID_EH_ENTRY_NOMASK_H
#if !defined(__ANDROID__) || !defined(__aarch64__)
#error Android AArch64 EH preinclude used for another target
#endif
#include "cpu64.h"

/* shortcut: guest throws and escaping mask-changing callbacks are unsupported;
 * requalify this override before adding either boundary. Native setjmp stays. */
#undef ES3_EH_ENTER
#define ES3_EH_ENTER(va)                                                      \
    jmp_buf _ehjb;                                                            \
    es3_ehframe_t _ehf;                                                       \
    es3_eh_push(&_ehf, (va), &_ehjb);                                         \
    if (_setjmp(_ehjb)) { _ind = _ehf.resume; goto _ljump; }

#endif
