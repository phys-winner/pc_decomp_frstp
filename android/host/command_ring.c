/* Original command-ring entries with ARM acquire/release and MSVC fences. */
#include "cpu64.h"
static uint32_t fp_ring_load32(uint64_t p){return __atomic_load_n((uint32_t*)(uintptr_t)p,__ATOMIC_ACQUIRE);}
static void fp_ring_store32(uint64_t p,uint32_t v){__atomic_store_n((uint32_t*)(uintptr_t)p,v,__ATOMIC_RELEASE);}
void L_000140E26E40(CPU *c)
{
    c->rip = 0x140E26E40ull;
    SET64(c->rdx, rd64((c->rcx + 0x18ull)));                     /* 000140E26E40: mov rdx, qword ptr [rcx + 0x18] */
    SET64(c->r10, c->rcx);                                       /* 000140E26E44: mov r10, rcx */
    SET32(c->rax, fp_ring_load32((c->rcx + 0x28ull)));                     /* 000140E26E47: mov eax, dword ptr [rcx + 0x28] */
    fp_ring_store32((c->rdx + 0x4ull), (uint32_t)(R32(c->rax)));            /* 000140E26E4A: mov dword ptr [rdx + 4], eax */
    SET32(c->rdx, fp_ring_load32((c->rcx + 0x10ull)));                     /* 000140E26E4D: mov edx, dword ptr [rcx + 0x10] */
    SET32(c->r8, fp_ring_load32((c->rcx + 0x28ull)));                      /* 000140E26E50: mov r8d, dword ptr [rcx + 0x28] */
    SET32(c->rdx, flags_add(c, R32(c->rdx), 0xBu, 4));           /* 000140E26E54: add edx, 0xb */
    SET32(c->rax, fp_ring_load32((c->rcx + 0x8ull)));                      /* 000140E26E57: mov eax, dword ptr [rcx + 8] */
    SET32(c->r8, flags_add(c, R32(c->r8), R32(c->rdx), 4));      /* 000140E26E5A: add r8d, edx */
    SET32(c->r8, flags_logicz(c, R32(c->r8) & 0xFFFFFFFCu, 4));  /* 000140E26E5D: and r8d, 0xfffffffc */
    SET32(c->r9, R32(c->r8));                                    /* 000140E26E61: mov r9d, r8d */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);                     /* 000140E26E64: lock or dword ptr [rsp], 0 */
    fp_ring_store32((c->rsp), (uint32_t)(flags_logicz(c, fp_ring_load32((c->rsp)) | 0x0u, 4))); /* 000140E26E64: lock or dword ptr [rsp], 0 */
    SET64(c->rdx, (c->r8 + 0x8ull));                             /* 000140E26E69: lea rdx, [r8 + 8] */
    SET32(c->r8, flags_logicz(c, R32(c->r8) ^ R32(c->r8), 4));   /* 000140E26E6D: xor r8d, r8d */
    flags_sub(c, c->rdx, c->rax, 8);                             /* 000140E26E70: cmp rdx, rax */
    wr64((c->r10 + 0x18ull), (uint64_t)(c->r8));                 /* 000140E26E73: mov qword ptr [r10 + 0x18], r8 */
    SET32(c->rcx, R32(c->r8));                                   /* 000140E26E77: mov ecx, r8d */
    wr64((c->r10 + 0x20ull), (uint64_t)(c->r8));                 /* 000140E26E7A: mov qword ptr [r10 + 0x20], r8 */
    SET32(c->rcx, ((c->cf || c->zf)) ? (uint32_t)(R32(c->r9)) : R32(c->rcx)); /* 000140E26E7E: cmovbe ecx, r9d */
    wr64((c->r10 + 0x28ull), (uint64_t)(c->r8));                 /* 000140E26E82: mov qword ptr [r10 + 0x28], r8 */
    fp_ring_store32((c->r10 + 0x10ull), (uint32_t)(R32(c->rcx)));           /* 000140E26E86: mov dword ptr [r10 + 0x10], ecx */
    c->rsp += 8; return;                                         /* 000140E26E8A: ret  */
}
void L_000140E26F30(CPU *c)
{
    c->rip = 0x140E26F30ull;
    SET64(c->r8, c->rcx);                                        /* 000140E26F30: mov r8, rcx */
    SET32(c->rcx, fp_ring_load32((c->rcx + 0x10ull)));                     /* 000140E26F33: mov ecx, dword ptr [rcx + 0x10] */
    SET32(c->rax, fp_ring_load32((c->r8 + 0xB0ull)));                      /* 000140E26F36: mov eax, dword ptr [r8 + 0xb0] */
    SET32(c->rcx, flags_sub(c, R32(c->rcx), R32(c->rax), 4));    /* 000140E26F3D: sub ecx, eax */
    if (!c->sf) goto L_000140E26F45;                             /* 000140E26F3F: jns 0x140e26f45 */
    SET32(c->rcx, flags_add(c, R32(c->rcx), fp_ring_load32((c->r8 + 0x8ull)), 4)); /* 000140E26F41: add ecx, dword ptr [r8 + 8] */
L_000140E26F45:
    c->rip = 0x140E26F45ull;
    flags_logicz(c, R32(c->rcx) & R32(c->rcx), 4);               /* 000140E26F45: test ecx, ecx */
    if (c->zf) goto L_000140E26F7A;                              /* 000140E26F47: je 0x140e26f7a */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);                     /* 000140E26F49: lock or dword ptr [rsp], 0 */
    fp_ring_store32((c->rsp), (uint32_t)(flags_logicz(c, fp_ring_load32((c->rsp)) | 0x0u, 4))); /* 000140E26F49: lock or dword ptr [rsp], 0 */
    SET32(c->rax, fp_ring_load32((c->r8 + 0xB0ull)));                      /* 000140E26F4E: mov eax, dword ptr [r8 + 0xb0] */
    SET64(c->rdx, rd64((c->r8)));                                /* 000140E26F55: mov rdx, qword ptr [r8] */
    SET64(c->rdx, flags_add(c, c->rdx, c->rax, 8));              /* 000140E26F58: add rdx, rax */
    wr64((c->r8 + 0xB8ull), (uint64_t)(c->rdx));                 /* 000140E26F5B: mov qword ptr [r8 + 0xb8], rdx */
    SET64(c->rax, (c->rdx + 0x8ull));                            /* 000140E26F62: lea rax, [rdx + 8] */
    wr64((c->r8 + 0xC0ull), (uint64_t)(c->rax));                 /* 000140E26F66: mov qword ptr [r8 + 0xc0], rax */
    SET32(c->rax, fp_ring_load32((c->rdx + 0x4ull)));                      /* 000140E26F6D: mov eax, dword ptr [rdx + 4] */
    fp_ring_store32((c->r8 + 0xC8ull), (uint32_t)(R32(c->rax)));            /* 000140E26F70: mov dword ptr [r8 + 0xc8], eax */
    SET8L(c->rax, 0x1u);                                         /* 000140E26F77: mov al, 1 */
    c->rsp += 8; return;                                         /* 000140E26F79: ret  */
L_000140E26F7A:
    c->rip = 0x140E26F7Aull;
    SET8L(c->rax, flags_logicz(c, R8L(c->rax) ^ R8L(c->rax), 1)); /* 000140E26F7A: xor al, al */
    c->rsp += 8; return;                                         /* 000140E26F7C: ret  */
}
void L_000140E26F90(CPU *c)
{
    c->rip = 0x140E26F90ull;
    SET64(c->rax, rd64((c->rcx + 0xB8ull)));                     /* 000140E26F90: mov rax, qword ptr [rcx + 0xb8] */
    SET32(c->rdx, flags_logicz(c, R32(c->rdx) ^ R32(c->rdx), 4)); /* 000140E26F97: xor edx, edx */
    SET64(c->r9, c->rcx);                                        /* 000140E26F99: mov r9, rcx */
    SET32(c->r8, fp_ring_load32((c->rax + 0x4ull)));                       /* 000140E26F9C: mov r8d, dword ptr [rax + 4] */
    SET32(c->rax, fp_ring_load32((c->rcx + 0xB0ull)));                     /* 000140E26FA0: mov eax, dword ptr [rcx + 0xb0] */
    SET64(c->r8, flags_add(c, c->r8, 0xBull, 8));                /* 000140E26FA6: add r8, 0xb */
    SET64(c->r8, flags_logicz(c, c->r8 & 0xFFFFFFFFFFFFFFFCull, 8)); /* 000140E26FAA: and r8, 0xfffffffffffffffc */
    SET64(c->rax, flags_add(c, c->rax, c->r8, 8));               /* 000140E26FAE: add rax, r8 */
    SET32(c->r8, fp_ring_load32((c->rcx + 0x8ull)));                       /* 000140E26FB1: mov r8d, dword ptr [rcx + 8] */
    { uint64_t _n=c->rax, _d=(uint64_t)(c->r8);                  /* 000140E26FB5: div r8 */
      if (c->rdx != 0) RECOMP_TODO(0x140E26FB5, "div: true 128-bit numerator"); /* 000140E26FB5: div r8 */
      c->rax=_n/_d; c->rdx=_n%_d; }                              /* 000140E26FB5: div r8 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);                     /* 000140E26FB8: lock or dword ptr [rsp], 0 */
    fp_ring_store32((c->rsp), (uint32_t)(flags_logicz(c, fp_ring_load32((c->rsp)) | 0x0u, 4))); /* 000140E26FB8: lock or dword ptr [rsp], 0 */
    SET32(c->r10, flags_logicz(c, R32(c->r10) ^ R32(c->r10), 4)); /* 000140E26FBD: xor r10d, r10d */
    SET64(c->rcx, (uint64_t)(int64_t)(int32_t)(R32(c->rdx)));    /* 000140E26FC0: movsxd rcx, edx */
    SET64(c->rcx, flags_add(c, c->rcx, 0x8ull, 8));              /* 000140E26FC3: add rcx, 8 */
    wr64((c->r9 + 0xB8ull), (uint64_t)(c->r10));                 /* 000140E26FC7: mov qword ptr [r9 + 0xb8], r10 */
    flags_sub(c, c->rcx, c->r8, 8);                              /* 000140E26FCE: cmp rcx, r8 */
    wr64((c->r9 + 0xC0ull), (uint64_t)(c->r10));                 /* 000140E26FD1: mov qword ptr [r9 + 0xc0], r10 */
    SET32(c->rax, R32(c->r10));                                  /* 000140E26FD8: mov eax, r10d */
    fp_ring_store32((c->r9 + 0xC8ull), (uint32_t)(R32(c->r10)));            /* 000140E26FDB: mov dword ptr [r9 + 0xc8], r10d */
    SET32(c->rax, ((c->cf || c->zf)) ? (uint32_t)(R32(c->rdx)) : R32(c->rax)); /* 000140E26FE2: cmovbe eax, edx */
    fp_ring_store32((c->r9 + 0xB0ull), (uint32_t)(R32(c->rax)));            /* 000140E26FE5: mov dword ptr [r9 + 0xb0], eax */
    c->rsp += 8; return;                                         /* 000140E26FEC: ret  */
}
