#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {uint64_t target;const char *name;} fp_native_hook;
void fp_native_hooks_sort(fp_native_hook *hooks,unsigned count);
const char *fp_native_hook_name(const fp_native_hook *hooks,unsigned count,uint64_t target);

void fp_native_validation_init(void);
void fp_native_validation_invalidate(void);
int fp_native_memory_mutation(uint64_t target);
void fp_native_stack_release_attempt(uint64_t target,uint64_t rcx,uint64_t rdx);
void fp_native_memory_mutation_begin(uint64_t target,uint64_t rcx,uint64_t rdx);
void fp_native_memory_mutation_end(void);
int fp_native_target_valid(uint64_t target);
int fp_native_stack_cached(uint64_t address,size_t bytes,uint64_t owner_low,uint64_t owner_high);
int fp_native_stack_prepare(uint64_t address,size_t bytes,uint64_t owner_low,uint64_t owner_high);
typedef struct {uint64_t calls,hits,accepted,no_owner,bounds,query,protection,span,reservation_queries,commit_queries,owned_reservation_hits,owned_commit_hits;} fp_native_stack_stats;
void fp_native_stack_statistics(fp_native_stack_stats *stats);
/* stack_base must be the real native TEB StackBase captured at the callback. */
int fp_native_stack_reservation(uint64_t rsp,uint64_t stack_base,uint64_t *reservation_low);
/* Returns the current committed RW tail containing RSP, or zero on failure. */
uint64_t fp_native_stack_commit_limit(uint64_t rsp,uint64_t stack_base,uint64_t reservation_low);
