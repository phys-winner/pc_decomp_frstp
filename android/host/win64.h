#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif

void win64_init(uint64_t image, size_t image_bytes, uint64_t stack, size_t stack_bytes);
/* Launcher's "skip intro videos" (PORT_SKIP_INTRO, default on): only the logos before the menu. */
extern int win64_skip_intro;
void win64_region_add(uint64_t address, size_t bytes, int writable);
void win64_region_remove(uint64_t address);
/* Serialize physical release/protection plus region changes. Nested calls are
 * supported; no guest callback or wait may run within this mutation boundary. */
void win64_region_mutation_begin(void);
void win64_region_mutation_end(void);
void win64_image_add(uint64_t address, size_t bytes);
void *win64_pointer(uint64_t address,size_t bytes,int writable);
const char *win64_string_a(uint64_t address);
uint64_t win64_argument(CPU *c,unsigned index);
uint64_t win64_alloc(size_t bytes,int zero);
void win64_free(uint64_t address);
void win64_set_error(uint32_t value);
uint32_t win64_get_error(void);
int win64_dispatch_import(CPU *c, const char *dll, const char *name);
int win64_dispatch_dynamic(CPU *c, uint64_t address);
void win64_self_test(void);
#ifdef __cplusplus
}
#endif
