#pragma once
#include "win64.h"
#include <stdio.h>
int win64_kernel_call(CPU *c, const char *name);
/* Direct handler of SetEvent, ResetEvent, ReleaseSemaphore or WaitForSingleObject for the import cache (win64.c), else NULL. */
int (*win64_kernel_fast_import(const char *name))(CPU *);
void win64_kernel_self_test(void);
/* Host paths must be existing, disjoint directories, set before the first API. */
int win64_kernel_configure_roots(const char *game,const char *profile);
void win64_kernel_roots_self_test(void);
/* A running native thread owns the token's internal reference independently of
 * guest handles. complete releases that reference; acquired refs protect API
 * users racing CloseHandle. Destructors must not re-enter kernel APIs. */
uint64_t win64_kernel_thread_create(void *payload,void (*destroy)(void *),int inherit,void **token);
void *win64_kernel_thread_acquire(uint64_t id,uint32_t any_access,void **token);
void win64_kernel_thread_release(void *token);
void win64_kernel_thread_complete(void *token,uint32_t exit_code);
uint32_t win64_kernel_thread_exit_code(void *token);
uint64_t win64_kernel_thread_duplicate(void *token,uint32_t access,int inherit);
/* Private manual event: no guest handle is created. The queue owns one token
 * reference; each wait retains its own reference until unregistering. */
void *win64_kernel_queue_event_create(void);
void win64_kernel_queue_event_set(void *token,int signaled);
void win64_kernel_queue_event_destroy(void *token);
uint32_t win64_kernel_queue_wait(const uint64_t *ids,unsigned count,void *token,uint32_t milliseconds);
/* Anchored secure binary CRT open. Caller owns returned duplicate fd and token;
 * token keeps the file's sharing reservation until stream_close. No guest
 * handle or raw host path is exposed. Returns Windows CRT errno (zero success). */
int win64_kernel_stream_open(uint64_t path,int wide,int writing,int *fd,void **token);
FILE *win64_kernel_stream_file(int fd,void *token,const char *mode);
void win64_kernel_stream_close(void *token);
