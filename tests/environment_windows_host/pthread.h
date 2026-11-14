#pragma once
/* Windows host fixture only. Android builds use real Bionic pthreads. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
static int pthread_mutex_lock(pthread_mutex_t *p){AcquireSRWLockExclusive(p);return 0;}
static int pthread_mutex_unlock(pthread_mutex_t *p){ReleaseSRWLockExclusive(p);return 0;}
