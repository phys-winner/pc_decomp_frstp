#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit virtual guest desktop. All-zero disables it. Configure before
 * guest execution; this function only validates/copies private policy. */
void win64_display_configure(uint32_t width,uint32_t height,uint32_t refresh_hz);
/* USER32/GDI32/SHCORE handler. Bounded CPU DIB32/mono bitmap storage is real;
 * native cursor images remain explicitly unsupported. Parent alone pops RSP. */
int win64_display_call(CPU *cpu,const char *name);
/* Run after win64_init, before guest execution; preserves desktop policy. */
void win64_display_self_test(void);
#ifdef __cplusplus
}
#endif
