#pragma once
#include "cpu64.h"
/* Submit copies an IME command; the original UI thread applies it to the same editor. */
int frostpunk_mobile_text_submit(unsigned action,const uint16_t *text,unsigned units,unsigned before,unsigned after);
void fp_mobile_text_process(CPU *cpu);
void frostpunk_mobile_text_cancel(void);
int frostpunk_mobile_text_editing(void);
void frostpunk_mobile_ime_viewport(float fraction);
float fp_mobile_ime_visible_fraction(void);
/* UI-thread only: validates the live registry and visible editing ancestors. */
uint64_t fp_mobile_text_focused_editor(void);
