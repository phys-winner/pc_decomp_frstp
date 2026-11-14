/* Clang's C11 atomic builtins for the Windows host fixture. The NDK's patched
 * stdatomic.h unconditionally includes Bionic sys/cdefs.h, unavailable on MSVC.
 * Android uses its real NDK stdatomic.h. No locks or atomics are emulated. */
#pragma once
typedef enum {memory_order_relaxed=__ATOMIC_RELAXED,memory_order_consume=__ATOMIC_CONSUME,
    memory_order_acquire=__ATOMIC_ACQUIRE,memory_order_release=__ATOMIC_RELEASE,
    memory_order_acq_rel=__ATOMIC_ACQ_REL,memory_order_seq_cst=__ATOMIC_SEQ_CST} memory_order;
typedef _Atomic(unsigned) atomic_uint;
typedef _Atomic(int) atomic_int;
#define atomic_load_explicit(p,m) __c11_atomic_load((p),(m))
#define atomic_store_explicit(p,v,m) __c11_atomic_store((p),(v),(m))
#define atomic_load(p) atomic_load_explicit((p),memory_order_seq_cst)
#define atomic_store(p,v) atomic_store_explicit((p),(v),memory_order_seq_cst)
#define atomic_thread_fence(m) __c11_atomic_thread_fence(m)
