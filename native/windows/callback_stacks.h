#pragma once
#include <stddef.h>
#include <stdint.h>
#define FP_CALLBACK_STACK_LIMIT 16
typedef struct {uint64_t allocated,freed,active,pending,destructors_on_stack;} fp_callback_stack_stats;
int fp_callback_stacks_init(void);
void *fp_callback_stack_alloc(unsigned index,size_t bytes);
void fp_callback_stacks_reap(void);
void fp_callback_stacks_statistics(fp_callback_stack_stats *stats);
/* Only this thread's live FLS owner can authorize an exact callback allocation. */
int fp_callback_stack_owned(uint64_t rsp,uint64_t stack_base,uint64_t *low,uint64_t *identity,unsigned *index);
/* A guest release/decommit attempt retires cache authority before native code runs. */
void fp_callback_stack_retire(uint64_t address,int all);
