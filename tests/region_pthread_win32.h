/* Host-only pthread subset for exercising extracted Android region code.
 * Production Android uses Bionic pthreads; mutex behavior remains recursive. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
typedef CRITICAL_SECTION pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef INIT_ONCE pthread_once_t;
typedef HANDLE pthread_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
#define PTHREAD_MUTEX_RECURSIVE 1
static int pthread_mutexattr_init(pthread_mutexattr_t *a){*a=0;return 0;}
static int pthread_mutexattr_settype(pthread_mutexattr_t *a,int kind){*a=kind;return 0;}
static int pthread_mutexattr_destroy(pthread_mutexattr_t *a){(void)a;return 0;}
static int pthread_mutex_init(pthread_mutex_t *m,const pthread_mutexattr_t *a){(void)a;return !InitializeCriticalSectionEx(m,0,0);}
static int pthread_mutex_lock(pthread_mutex_t *m){EnterCriticalSection(m);return 0;}
static int pthread_mutex_unlock(pthread_mutex_t *m){LeaveCriticalSection(m);return 0;}
static BOOL CALLBACK region_once(PINIT_ONCE once,PVOID data,PVOID *context){(void)once;(void)context;((void(*)(void))data)();return TRUE;}
static int pthread_once(pthread_once_t *once,void (*fn)(void)){return !InitOnceExecuteOnce(once,region_once,(void*)fn,NULL);}
typedef struct {void *(*fn)(void*);void *argument;} region_thread_start;
static DWORD WINAPI region_thread_entry(void *data){region_thread_start s=*(region_thread_start*)data;free(data);s.fn(s.argument);return 0;}
static int pthread_create(pthread_t *thread,const void *attr,void *(*fn)(void*),void *arg){
    (void)attr;region_thread_start *s=malloc(sizeof(*s));if(!s)return 12;*s=(region_thread_start){fn,arg};
    *thread=CreateThread(NULL,0,region_thread_entry,s,0,NULL);if(!*thread){free(s);return 12;}return 0;
}
static int pthread_join(pthread_t thread,void **result){(void)result;int status=WaitForSingleObject(thread,INFINITE)!=WAIT_OBJECT_0;CloseHandle(thread);return status;}
static int sched_yield(void){SwitchToThread();return 0;}
