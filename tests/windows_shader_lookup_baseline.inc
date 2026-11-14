#include "cpu64.h"
void baseline_L_0001410596A9(CPU *c);
void baseline_L_000141059750(CPU *c);
void baseline_L_000141059732(CPU *c);
void baseline_L_0001410597E9(CPU *c);
void baseline_L_000141059890(CPU *c);
void baseline_L_000141059872(CPU *c);

void baseline_L_0001410596A9(CPU *c)
{
    c->rip = 0x1410596A9ull;
    wr64((c->rsp + 0x40ull), (uint64_t)(c->rbp));                /* 0001410596A9: mov qword ptr [rsp + 0x40], rbp */
    wr64((c->rsp + 0x48ull), (uint64_t)(c->rdi));                /* 0001410596AE: mov qword ptr [rsp + 0x48], rdi */
    wr64((c->rsp + 0x50ull), (uint64_t)(c->r12));                /* 0001410596B3: mov qword ptr [rsp + 0x50], r12 */
    flags_logicz(c, R32(c->r11) & R32(c->r11), 4);               /* 0001410596B8: test r11d, r11d */
    if ((c->zf || (c->sf != c->of))) goto L_00014105970F;        /* 0001410596BB: jle 0x14105970f */
    SET64(c->r12, rd64((c->rsi + 0x60ull)));                     /* 0001410596BD: mov r12, qword ptr [rsi + 0x60] */
    SET64(c->rbp, GVA(0x1421ECEBA));                             /* 0001410596C1: lea rbp, [rip + 0x11937f2] */
    flags_logicz(c, c->r15 & c->r15, 8);                         /* 0001410596C8: test r15, r15 */
    if (!c->zf) { SET64(c->rbp, c->r15); }                       /* 0001410596CB: cmovne rbp, r15 */
    /* nop */                                                    /* 0001410596CF: nop  */
L_0001410596D0:
    c->rip = 0x1410596D0ull;
    SET64(c->rax, (uint64_t)(int64_t)(int32_t)(R32(c->r9)));     /* 0001410596D0: movsxd rax, r9d */
    SET64(c->r8, c->rbp);                                        /* 0001410596D3: mov r8, rbp */
    SET64(c->r10, (c->rax + c->rax*8));                          /* 0001410596D6: lea r10, [rax + rax*8] */
    SET64(c->r10, op_shl(c, c->r10, 0x7u, 8));                   /* 0001410596DA: shl r10, 7 */
    SET64(c->rdi, (c->r12 + c->r10*1));                          /* 0001410596DE: lea rdi, [r12 + r10] */
    SET64(c->rax, c->rdi);                                       /* 0001410596E2: mov rax, rdi */
    SET64(c->r8, flags_sub(c, c->r8, c->rdi, 8));                /* 0001410596E5: sub r8, rdi */
    /* nop */                                                    /* 0001410596E8: nop dword ptr [rax + rax] */
L_0001410596F0:
    c->rip = 0x1410596F0ull;
    SET32(c->rdx, (rd8((c->rax))));                              /* 0001410596F0: movzx edx, byte ptr [rax] */
    SET32(c->rcx, (rd8((c->rax + c->r8*1))));                    /* 0001410596F3: movzx ecx, byte ptr [rax + r8] */
    SET32(c->rdx, flags_sub(c, R32(c->rdx), R32(c->rcx), 4));    /* 0001410596F8: sub edx, ecx */
    if (!c->zf) goto L_000141059703;                             /* 0001410596FA: jne 0x141059703 */
    SET64(c->rax, flags_incs(c, c->rax, 8));                     /* 0001410596FC: inc rax */
    flags_logicz(c, R32(c->rcx) & R32(c->rcx), 4);               /* 0001410596FF: test ecx, ecx */
    if (!c->zf) goto L_0001410596F0;                             /* 000141059701: jne 0x1410596f0 */
L_000141059703:
    c->rip = 0x141059703ull;
    flags_logicz(c, R32(c->rdx) & R32(c->rdx), 4);               /* 000141059703: test edx, edx */
    if (c->zf) { dispatch(c, 0x141059750ull); return; }          /* 000141059705: je 0x141059750 */
    SET32(c->r9, flags_incs(c, R32(c->r9), 4));                  /* 000141059707: inc r9d */
    flags_sub(c, R32(c->r9), R32(c->r11), 4);                    /* 00014105970A: cmp r9d, r11d */
    if ((c->sf != c->of)) goto L_0001410596D0;                   /* 00014105970D: jl 0x1410596d0 */
L_00014105970F:
    c->rip = 0x14105970Full;
    SET64(c->rdx, c->r15);                                       /* 00014105970F: mov rdx, r15 */
    SET64(c->rcx, c->rsi);                                       /* 000141059712: mov rcx, rsi */
    push64(c, 0x14105971Aull); dispatch(c, 0x141058000ull); c->rip = 0x14105971Aull; /* 000141059715: call 0x141058000 */
    SET64(c->rcx, (c->rax + 0x200ull));                          /* 00014105971A: lea rcx, [rax + 0x200] */
    wr64((c->r14), (uint64_t)(c->rcx));                          /* 000141059721: mov qword ptr [r14], rcx */
    flags_logicz(c, c->rbx & c->rbx, 8);                         /* 000141059724: test rbx, rbx */
    if (c->zf) goto L_000141059732;                              /* 000141059727: je 0x141059732 */
    SET64(c->rax, flags_add(c, c->rax, 0x400ull, 8));            /* 000141059729: add rax, 0x400 */
    wr64((c->rbx), (uint64_t)(c->rax));                          /* 00014105972F: mov qword ptr [rbx], rax */
L_000141059732:
    c->rip = 0x141059732ull;
    SET64(c->rdi, rd64((c->rsp + 0x48ull)));                     /* 000141059732: mov rdi, qword ptr [rsp + 0x48] */
    SET64(c->rbp, rd64((c->rsp + 0x40ull)));                     /* 000141059737: mov rbp, qword ptr [rsp + 0x40] */
    SET64(c->r12, rd64((c->rsp + 0x50ull)));                     /* 00014105973C: mov r12, qword ptr [rsp + 0x50] */
    SET64(c->rbx, rd64((c->rsp + 0x58ull)));                     /* 000141059741: mov rbx, qword ptr [rsp + 0x58] */
    SET64(c->rsp, flags_add(c, c->rsp, 0x20ull, 8));             /* 000141059746: add rsp, 0x20 */
    SET64(c->r15, pop64(c));                                     /* 00014105974A: pop r15 */
    SET64(c->r14, pop64(c));                                     /* 00014105974C: pop r14 */
    SET64(c->rsi, pop64(c));                                     /* 00014105974E: pop rsi */
    c->rsp += 8; return;                                         /* 00014105974F: ret  */
}

void baseline_L_000141059750(CPU *c)
{
    c->rip = 0x141059750ull;
    SET64(c->rax, (c->rdi + 0x200ull));                          /* 000141059750: lea rax, [rdi + 0x200] */
    wr64((c->r14), (uint64_t)(c->rax));                          /* 000141059757: mov qword ptr [r14], rax */
    flags_logicz(c, c->rbx & c->rbx, 8);                         /* 00014105975A: test rbx, rbx */
    if (c->zf) { dispatch(c, 0x141059732ull); return; }          /* 00014105975D: je 0x141059732 */
    SET64(c->rcx, rd64((c->rsi + 0x60ull)));                     /* 00014105975F: mov rcx, qword ptr [rsi + 0x60] */
    SET64(c->rcx, flags_add(c, c->rcx, 0x400ull, 8));            /* 000141059763: add rcx, 0x400 */
    SET64(c->rcx, flags_add(c, c->rcx, c->r10, 8));              /* 00014105976A: add rcx, r10 */
    wr64((c->rbx), (uint64_t)(c->rcx));                          /* 00014105976D: mov qword ptr [rbx], rcx */
    dispatch(c, 0x141059732ull); return;                         /* 000141059770: jmp 0x141059732 */
}

void baseline_L_000141059732(CPU *c)
{
    c->rip = 0x141059732ull;
    SET64(c->rdi, rd64((c->rsp + 0x48ull)));                     /* 000141059732: mov rdi, qword ptr [rsp + 0x48] */
    SET64(c->rbp, rd64((c->rsp + 0x40ull)));                     /* 000141059737: mov rbp, qword ptr [rsp + 0x40] */
    SET64(c->r12, rd64((c->rsp + 0x50ull)));                     /* 00014105973C: mov r12, qword ptr [rsp + 0x50] */
    SET64(c->rbx, rd64((c->rsp + 0x58ull)));                     /* 000141059741: mov rbx, qword ptr [rsp + 0x58] */
    SET64(c->rsp, flags_add(c, c->rsp, 0x20ull, 8));             /* 000141059746: add rsp, 0x20 */
    SET64(c->r15, pop64(c));                                     /* 00014105974A: pop r15 */
    SET64(c->r14, pop64(c));                                     /* 00014105974C: pop r14 */
    SET64(c->rsi, pop64(c));                                     /* 00014105974E: pop rsi */
    c->rsp += 8; return;                                         /* 00014105974F: ret  */
}

void baseline_L_0001410597E9(CPU *c)
{
    c->rip = 0x1410597E9ull;
    wr64((c->rsp + 0x40ull), (uint64_t)(c->rbp));                /* 0001410597E9: mov qword ptr [rsp + 0x40], rbp */
    wr64((c->rsp + 0x48ull), (uint64_t)(c->rdi));                /* 0001410597EE: mov qword ptr [rsp + 0x48], rdi */
    wr64((c->rsp + 0x50ull), (uint64_t)(c->r12));                /* 0001410597F3: mov qword ptr [rsp + 0x50], r12 */
    flags_logicz(c, R32(c->r11) & R32(c->r11), 4);               /* 0001410597F8: test r11d, r11d */
    if ((c->zf || (c->sf != c->of))) goto L_00014105984F;        /* 0001410597FB: jle 0x14105984f */
    SET64(c->r12, rd64((c->rsi + 0x60ull)));                     /* 0001410597FD: mov r12, qword ptr [rsi + 0x60] */
    SET64(c->rbp, GVA(0x1421ECEBA));                             /* 000141059801: lea rbp, [rip + 0x11936b2] */
    flags_logicz(c, c->r15 & c->r15, 8);                         /* 000141059808: test r15, r15 */
    if (!c->zf) { SET64(c->rbp, c->r15); }                       /* 00014105980B: cmovne rbp, r15 */
    /* nop */                                                    /* 00014105980F: nop  */
L_000141059810:
    c->rip = 0x141059810ull;
    SET64(c->rax, (uint64_t)(int64_t)(int32_t)(R32(c->r9)));     /* 000141059810: movsxd rax, r9d */
    SET64(c->r8, c->rbp);                                        /* 000141059813: mov r8, rbp */
    SET64(c->r10, (c->rax + c->rax*8));                          /* 000141059816: lea r10, [rax + rax*8] */
    SET64(c->r10, op_shl(c, c->r10, 0x7u, 8));                   /* 00014105981A: shl r10, 7 */
    SET64(c->rdi, (c->r12 + c->r10*1));                          /* 00014105981E: lea rdi, [r12 + r10] */
    SET64(c->rax, c->rdi);                                       /* 000141059822: mov rax, rdi */
    SET64(c->r8, flags_sub(c, c->r8, c->rdi, 8));                /* 000141059825: sub r8, rdi */
    /* nop */                                                    /* 000141059828: nop dword ptr [rax + rax] */
L_000141059830:
    c->rip = 0x141059830ull;
    SET32(c->rdx, (rd8((c->rax))));                              /* 000141059830: movzx edx, byte ptr [rax] */
    SET32(c->rcx, (rd8((c->rax + c->r8*1))));                    /* 000141059833: movzx ecx, byte ptr [rax + r8] */
    SET32(c->rdx, flags_sub(c, R32(c->rdx), R32(c->rcx), 4));    /* 000141059838: sub edx, ecx */
    if (!c->zf) goto L_000141059843;                             /* 00014105983A: jne 0x141059843 */
    SET64(c->rax, flags_incs(c, c->rax, 8));                     /* 00014105983C: inc rax */
    flags_logicz(c, R32(c->rcx) & R32(c->rcx), 4);               /* 00014105983F: test ecx, ecx */
    if (!c->zf) goto L_000141059830;                             /* 000141059841: jne 0x141059830 */
L_000141059843:
    c->rip = 0x141059843ull;
    flags_logicz(c, R32(c->rdx) & R32(c->rdx), 4);               /* 000141059843: test edx, edx */
    if (c->zf) { dispatch(c, 0x141059890ull); return; }          /* 000141059845: je 0x141059890 */
    SET32(c->r9, flags_incs(c, R32(c->r9), 4));                  /* 000141059847: inc r9d */
    flags_sub(c, R32(c->r9), R32(c->r11), 4);                    /* 00014105984A: cmp r9d, r11d */
    if ((c->sf != c->of)) goto L_000141059810;                   /* 00014105984D: jl 0x141059810 */
L_00014105984F:
    c->rip = 0x14105984Full;
    SET64(c->rdx, c->r15);                                       /* 00014105984F: mov rdx, r15 */
    SET64(c->rcx, c->rsi);                                       /* 000141059852: mov rcx, rsi */
    push64(c, 0x14105985Aull); dispatch(c, 0x141058000ull); c->rip = 0x14105985Aull; /* 000141059855: call 0x141058000 */
    SET64(c->rcx, (c->rax + 0x280ull));                          /* 00014105985A: lea rcx, [rax + 0x280] */
    wr64((c->r14), (uint64_t)(c->rcx));                          /* 000141059861: mov qword ptr [r14], rcx */
    flags_logicz(c, c->rbx & c->rbx, 8);                         /* 000141059864: test rbx, rbx */
    if (c->zf) goto L_000141059872;                              /* 000141059867: je 0x141059872 */
    SET64(c->rax, flags_add(c, c->rax, 0x300ull, 8));            /* 000141059869: add rax, 0x300 */
    wr64((c->rbx), (uint64_t)(c->rax));                          /* 00014105986F: mov qword ptr [rbx], rax */
L_000141059872:
    c->rip = 0x141059872ull;
    SET64(c->rdi, rd64((c->rsp + 0x48ull)));                     /* 000141059872: mov rdi, qword ptr [rsp + 0x48] */
    SET64(c->rbp, rd64((c->rsp + 0x40ull)));                     /* 000141059877: mov rbp, qword ptr [rsp + 0x40] */
    SET64(c->r12, rd64((c->rsp + 0x50ull)));                     /* 00014105987C: mov r12, qword ptr [rsp + 0x50] */
    SET64(c->rbx, rd64((c->rsp + 0x58ull)));                     /* 000141059881: mov rbx, qword ptr [rsp + 0x58] */
    SET64(c->rsp, flags_add(c, c->rsp, 0x20ull, 8));             /* 000141059886: add rsp, 0x20 */
    SET64(c->r15, pop64(c));                                     /* 00014105988A: pop r15 */
    SET64(c->r14, pop64(c));                                     /* 00014105988C: pop r14 */
    SET64(c->rsi, pop64(c));                                     /* 00014105988E: pop rsi */
    c->rsp += 8; return;                                         /* 00014105988F: ret  */
}

void baseline_L_000141059890(CPU *c)
{
    c->rip = 0x141059890ull;
    SET64(c->rax, (c->rdi + 0x280ull));                          /* 000141059890: lea rax, [rdi + 0x280] */
    wr64((c->r14), (uint64_t)(c->rax));                          /* 000141059897: mov qword ptr [r14], rax */
    flags_logicz(c, c->rbx & c->rbx, 8);                         /* 00014105989A: test rbx, rbx */
    if (c->zf) { dispatch(c, 0x141059872ull); return; }          /* 00014105989D: je 0x141059872 */
    SET64(c->rcx, rd64((c->rsi + 0x60ull)));                     /* 00014105989F: mov rcx, qword ptr [rsi + 0x60] */
    SET64(c->rcx, flags_add(c, c->rcx, 0x300ull, 8));            /* 0001410598A3: add rcx, 0x300 */
    SET64(c->rcx, flags_add(c, c->rcx, c->r10, 8));              /* 0001410598AA: add rcx, r10 */
    wr64((c->rbx), (uint64_t)(c->rcx));                          /* 0001410598AD: mov qword ptr [rbx], rcx */
    dispatch(c, 0x141059872ull); return;                         /* 0001410598B0: jmp 0x141059872 */
}

void baseline_L_000141059872(CPU *c)
{
    c->rip = 0x141059872ull;
    SET64(c->rdi, rd64((c->rsp + 0x48ull)));                     /* 000141059872: mov rdi, qword ptr [rsp + 0x48] */
    SET64(c->rbp, rd64((c->rsp + 0x40ull)));                     /* 000141059877: mov rbp, qword ptr [rsp + 0x40] */
    SET64(c->r12, rd64((c->rsp + 0x50ull)));                     /* 00014105987C: mov r12, qword ptr [rsp + 0x50] */
    SET64(c->rbx, rd64((c->rsp + 0x58ull)));                     /* 000141059881: mov rbx, qword ptr [rsp + 0x58] */
    SET64(c->rsp, flags_add(c, c->rsp, 0x20ull, 8));             /* 000141059886: add rsp, 0x20 */
    SET64(c->r15, pop64(c));                                     /* 00014105988A: pop r15 */
    SET64(c->r14, pop64(c));                                     /* 00014105988C: pop r14 */
    SET64(c->rsi, pop64(c));                                     /* 00014105988E: pop rsi */
    c->rsp += 8; return;                                         /* 00014105988F: ret  */
}
