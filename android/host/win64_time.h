#pragma once
#include "cpu64.h"
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
int win64_time_call(CPU *cpu,const char *name);
int win64_time_winmm_call(CPU *cpu,const char *name);
void win64_time_self_test(void);
uint64_t win64_game_clock_ns(clockid_t clock);
void win64_time_pause(int paused);
#ifdef __cplusplus
}
#endif
