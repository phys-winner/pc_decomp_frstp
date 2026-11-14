#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Configure once before guest audio calls. DSO/profile are trusted host paths;
 * the build consumer verifies their pinned provenance. No guestRSP pop here. */
int win64_openal_configure(const char *dso_path,const char *config_root);
int win64_openal_dispatch(CPU *cpu,uint64_t original_target);
/* Query-only WINMM profile over native endpoint inventory. No wave playback
 * driver or successful WAVEOUTCAPSW is exposed; parent owns guest RSP pop. */
int win64_openal_winmm_call(CPU *cpu,const char *name);
/* Only after guest workers stop. All native contexts/devices are closed. */
void win64_openal_shutdown(void);
void win64_openal_pause(int paused);
void win64_openal_self_test(void);
#ifdef __cplusplus
}
#endif
