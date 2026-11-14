/* Forced into the test validation object only; finish a writer after capturing epoch. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
LONG64 fp_test_read_acquire64(LONG64 const volatile *source);
#define ReadAcquire64 fp_test_read_acquire64
