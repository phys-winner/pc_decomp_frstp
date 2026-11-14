#ifndef FP_PERF_COUNTERS_H
#define FP_PERF_COUNTERS_H

enum { FP_PERF_MAIN, FP_PERF_IMPORT, FP_PERF_PAYLOAD, FP_PERF_DEPENDENCY, FP_PERF_DYNAMIC, FP_PERF_D3D11,
       FP_PERF_STEAM, FP_PERF_OPENAL, FP_PERF_SHADER, FP_PERF_DINPUT, FP_PERF_EXPORT, FP_PERF_MISS, FP_PERF_KINDS };

#ifdef FP_PERF_COUNTERS
void fp_perf_count_import(const char *dll, const char *name);
void fp_perf_count_kind(unsigned kind);
void fp_perf_tick(void);
#define FP_PERF(kind) fp_perf_count_kind(kind)
#define FP_PERF_IMPORT_NAME(dll, name) fp_perf_count_import((dll), (name))
#define FP_PERF_TICK() fp_perf_tick()
#else
#define FP_PERF(kind) ((void)0)
#define FP_PERF_IMPORT_NAME(dll, name) ((void)0)
#define FP_PERF_TICK() ((void)0)
#endif

#endif
