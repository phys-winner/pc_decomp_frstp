#pragma once
#include "cpu64.h"
#include "../app/host_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Configure before creating guest windows. The API and its callbacks outlive
 * every HWND and acquired surface reference. This adapter owns event polling.
 * Replacement requires no live surface HWND or active adapter operation. */
void win64_window_configure(const FrostpunkAndroidHost *host);
/* USER32 handler; the parent import dispatcher alone pops the return address. */
int win64_window_call(CPU *cpu,const char *name);
/* Direct handler of the message-loop imports for the import cache (win64.c), else NULL. */
int (*win64_window_fast_import(const char *name))(CPU *);
/* SHELL32's void DragAcceptFiles delegate; stores WS_EX_ACCEPTFILES only.
 * No Android file-drop delivery backend is provided. CPU/RSP are preserved. */
void win64_window_drag_accept_files(CPU *cpu);
/* A successful acquire owns a real Android window reference until release. */
ANativeWindow *win64_window_acquire(uint64_t hwnd,uint64_t *generation);
void win64_window_release(ANativeWindow *window);
/* DirectInput reads the same native pump: focused=1, unfocused=0, invalid=-1.
 * Valid HWNDs copy 256 Set-1/DIK key bytes; invalid HWNDs leave output intact. */
int win64_window_input_snapshot(uint64_t hwnd,uint8_t keys[256]);
/* Synthetic taps wait for delivery and a subsequent complete Game::Update, never screen rendering. */
uint64_t win64_window_ui_begin(void);
void win64_window_ui_end(uint64_t epoch);
int frostpunk_android_touch_ready(uint32_t ticket);
void win64_window_ime_key(uint32_t key,uint32_t scan);
/* ABI/headless checks only. The standalone fixture tests guest callbacks. */
void win64_window_self_test(void);
#ifdef __cplusplus
}
#endif
