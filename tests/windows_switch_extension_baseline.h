static void baseline_140ED24F5(CPU *c) {
    c->rip=0x140ED24F5ull;
    SET8L(c->rdx, 0x1u);                                         /* 000140ED24F5: mov dl, 1 */
    SET64(c->rcx, c->rdi);                                       /* 000140ED24F7: mov rcx, rdi */
    push64(c, 0x140ED24FFull); dispatch(c, 0x140ED2630ull); c->rip = 0x140ED24FFull; /* 000140ED24FA: call 0x140ed2630 */
    SET64(c->rax, rd64((c->rdi + 0xC8ull)));                     /* 000140ED24FF: mov rax, qword ptr [rdi + 0xc8] */
    SET64(c->rsi, rd64((c->rax)));                               /* 000140ED2506: mov rsi, qword ptr [rax] */
    SET64(c->rax, flags_add(c, c->rax, 0x8ull, 8));              /* 000140ED2509: add rax, 8 */
    wr32((c->rdi + 0xD0ull), (uint32_t)(flags_add(c, rd32((c->rdi + 0xD0ull)), 0xFFFFFFF8u, 4))); /* 000140ED250D: add dword ptr [rdi + 0xd0], -8 */
    wr64((c->rdi + 0xC8ull), (uint64_t)(c->rax));                /* 000140ED2514: mov qword ptr [rdi + 0xc8], rax */
    flags_logicz(c, c->rsi & c->rsi, 8);                         /* 000140ED251B: test rsi, rsi */
    if (c->zf) { dispatch(c, 0x140ED2055ull); return; }          /* 000140ED251E: je 0x140ed2055 */
    SET8L(c->rdx, 0x1u);                                         /* 000140ED2524: mov dl, 1 */
    SET64(c->rcx, (c->rdi + 0x180ull));                          /* 000140ED2526: lea rcx, [rdi + 0x180] */
    push64(c, 0x140ED2532ull); dispatch(c, 0x140E26C40ull); c->rip = 0x140ED2532ull; /* 000140ED252D: call 0x140e26c40 */
    SET64(c->rax, rd64((c->rsi)));                               /* 000140ED2532: mov rax, qword ptr [rsi] */
    SET64(c->rcx, c->rsi);                                       /* 000140ED2535: mov rcx, rsi */
    { uint64_t _ct = rd64((c->rax)); push64(c, 0x140ED253Aull); dispatch(c, _ct); } c->rip = 0x140ED253Aull; /* 000140ED2538: call qword ptr [rax] */
    SET32(c->rcx, R32(c->rax));                                  /* 000140ED253A: mov ecx, eax */
    SET64(c->rdx, c->rsi);                                       /* 000140ED253C: mov rdx, rsi */
    SET64(c->rcx, rd64((c->rdi + c->rcx*8 + 0x1A8ull)));         /* 000140ED253F: mov rcx, qword ptr [rdi + rcx*8 + 0x1a8] */
    push64(c, 0x140ED254Cull); dispatch(c, 0x140E9D330ull); c->rip = 0x140ED254Cull; /* 000140ED2547: call 0x140e9d330 */
    SET64(c->rcx, (c->rdi + 0x180ull));                          /* 000140ED254C: lea rcx, [rdi + 0x180] */
    push64(c, 0x140ED2558ull); dispatch(c, 0x140E26C90ull); c->rip = 0x140ED2558ull; /* 000140ED2553: call 0x140e26c90 */
    dispatch(c, 0x140ED2055ull); return;                         /* 000140ED2558: jmp 0x140ed2055 */
    dispatch(c,0x140ED255Dull);
}

static void baseline_140F9A5D2(CPU *c) {
    c->rip=0x140F9A5D2ull;
    SET64(c->rax, rd64((c->rdi + 0x168ull)));                    /* 000140F9A5D2: mov rax, qword ptr [rdi + 0x168] */
    SET64(c->rdx, (c->rdi + 0xD0ull));                           /* 000140F9A5D9: lea rdx, [rdi + 0xd0] */
    SET64(c->rcx, (c->rdi + 0xC0ull));                           /* 000140F9A5E0: lea rcx, [rdi + 0xc0] */
    sse_load_ss(&c->xmm[6], rdss((c->rax + 0x100ull)));          /* 000140F9A5E7: movss xmm6, dword ptr [rax + 0x100] */
    push64(c, 0x140F9A5F4ull); dispatch(c, 0x140F94C10ull); c->rip = 0x140F9A5F4ull; /* 000140F9A5EF: call 0x140f94c10 */
    SET64(c->rax, rd64((c->rdi + 0x168ull)));                    /* 000140F9A5F4: mov rax, qword ptr [rdi + 0x168] */
    sse_load_ss(&c->xmm[0], rdss((c->rax + 0x104ull)));          /* 000140F9A5FB: movss xmm0, dword ptr [rax + 0x104] */
    c->xmm[0].f32[0] = c->xmm[0].f32[0] - rdss((c->rsp + 0x24ull)); /* 000140F9A603: subss xmm0, dword ptr [rsp + 0x24] */
    c->xmm[0].f32[0] = frostpunk_addss(c->xmm[0].f32[0], rdss((c->rdi + 0xC4ull))); /* 000140F9A609: addss xmm0, dword ptr [rdi + 0xc4] */
    wrss((c->rdi + 0xC4ull), c->xmm[0].f32[0]);                  /* 000140F9A611: movss dword ptr [rdi + 0xc4], xmm0 */
    dispatch(c, 0x140F9A53Bull); return;                         /* 000140F9A619: jmp 0x140f9a53b */
    dispatch(c,0x140F9A61Eull);
}

static void baseline_140F9A61E(CPU *c) {
    c->rip=0x140F9A61Eull;
    SET64(c->rax, rd64((c->rdi + 0x168ull)));                    /* 000140F9A61E: mov rax, qword ptr [rdi + 0x168] */
    SET64(c->rdx, (c->rdi + 0xD0ull));                           /* 000140F9A625: lea rdx, [rdi + 0xd0] */
    SET64(c->rcx, (c->rdi + 0xC0ull));                           /* 000140F9A62C: lea rcx, [rdi + 0xc0] */
    /* extent ends mid-function: fall through */
    dispatch(c, 0x140F9A633ull); return;
    dispatch(c,0x140F9A633ull);
}

static void baseline_140F9A6AB(CPU *c) {
    c->rip=0x140F9A6ABull;
    sse_load_ss(&c->xmm[3], rdss(GVA(0x142A88DF8)));             /* 000140F9A6AB: movss xmm3, dword ptr [rip + 0x1aee745] */
    { XMM _s = c->xmm[1]; c->xmm[1].u64[0] ^= _s.u64[0]; c->xmm[1].u64[1] ^= _s.u64[1]; } /* 000140F9A6B3: xorps xmm1, xmm1 */
    sse_load_ss(&c->xmm[0], rdss(GVA(0x142A88DFC)));             /* 000140F9A6B6: movss xmm0, dword ptr [rip + 0x1aee73e] */
    sse_load_ss(&c->xmm[2], rdss(GVA(0x1421F52B8)));             /* 000140F9A6BE: movss xmm2, dword ptr [rip + 0x125abf2] */
    c->xmm[7] = rdxm(GVA(0x142A88F20));                          /* 000140F9A6C6: movaps xmm7, xmmword ptr [rip + 0x1aee853] */
    { XMM _d = c->xmm[3], _s = c->xmm[3]; c->xmm[3].u32[0] = _d.u32[1]; c->xmm[3].u32[1] = _d.u32[0]; c->xmm[3].u32[2] = _s.u32[2]; c->xmm[3].u32[3] = _s.u32[3]; } /* 000140F9A6CD: shufps xmm3, xmm3, 0xe1 */
    c->xmm[3].f32[0] = c->xmm[0].f32[0];                         /* 000140F9A6D1: movss xmm3, xmm0 */
    { XMM _d = c->xmm[3], _s = c->xmm[3]; c->xmm[3].u32[0] = _d.u32[2]; c->xmm[3].u32[1] = _d.u32[1]; c->xmm[3].u32[2] = _s.u32[0]; c->xmm[3].u32[3] = _s.u32[3]; } /* 000140F9A6D5: shufps xmm3, xmm3, 0xc6 */
    c->xmm[3].f32[0] = c->xmm[1].f32[0];                         /* 000140F9A6D9: movss xmm3, xmm1 */
    { XMM _d = c->xmm[3], _s = c->xmm[3]; c->xmm[3].u32[0] = _d.u32[3]; c->xmm[3].u32[1] = _d.u32[1]; c->xmm[3].u32[2] = _s.u32[2]; c->xmm[3].u32[3] = _s.u32[0]; } /* 000140F9A6DD: shufps xmm3, xmm3, 0x27 */
    c->xmm[3].f32[0] = c->xmm[2].f32[0];                         /* 000140F9A6E1: movss xmm3, xmm2 */
    { XMM _d = c->xmm[3], _s = c->xmm[3]; c->xmm[3].u32[0] = _d.u32[1]; c->xmm[3].u32[1] = _d.u32[2]; c->xmm[3].u32[2] = _s.u32[3]; c->xmm[3].u32[3] = _s.u32[0]; } /* 000140F9A6E5: shufps xmm3, xmm3, 0x39 */
    wrxm((c->rsp + 0x20ull), c->xmm[3]);                         /* 000140F9A6E9: movaps xmmword ptr [rsp + 0x20], xmm3 */
    wrxm((c->rdi + 0xC0ull), c->xmm[3]);                         /* 000140F9A6EE: movdqu xmmword ptr [rdi + 0xc0], xmm3 */
    dispatch(c, 0x140F9A549ull); return;                         /* 000140F9A6F6: jmp 0x140f9a549 */
    dispatch(c,0x140F9A6FBull);
}

static void baseline_141AFFBEF(CPU *c) {
    c->rip=0x141AFFBEFull;
    SET64(c->rcx, rd64(GVA(0x14400C200)));                       /* 000141AFFBEF: mov rcx, qword ptr [rip + 0x250c60a] */
    flags_logicz(c, c->rcx & c->rcx, 8);                         /* 000141AFFBF6: test rcx, rcx */
    if (c->zf) { dispatch(c,0x000141AFFC3Full); return; }                              /* 000141AFFBF9: je 0x141affc3f */
    SET32(c->rdx, rd32((c->rdi + 0x8ull)));                      /* 000141AFFBFB: mov edx, dword ptr [rdi + 8] */
    /* nop */                                                    /* 000141AFFBFE: nop  */
L_000141AFFC00:
    c->rip = 0x141AFFC00ull;
    SET64(c->rax, rd64((c->rcx)));                               /* 000141AFFC00: mov rax, qword ptr [rcx] */
    flags_sub(c, rd32((c->rax)), R32(c->rdx), 4);                /* 000141AFFC03: cmp dword ptr [rax], edx */
    if (c->zf) goto L_000141AFFC24;                              /* 000141AFFC05: je 0x141affc24 */
    SET64(c->rcx, rd64((c->rcx + 0x548ull)));                    /* 000141AFFC07: mov rcx, qword ptr [rcx + 0x548] */
    flags_logicz(c, c->rcx & c->rcx, 8);                         /* 000141AFFC0E: test rcx, rcx */
    if (!c->zf) goto L_000141AFFC00;                             /* 000141AFFC11: jne 0x141affc00 */
    SET32(c->rax, (c->rcx + 0x1ull));                            /* 000141AFFC13: lea eax, [rcx + 1] */
    SET64(c->rbx, rd64((c->rsp + 0x88ull)));                     /* 000141AFFC16: mov rbx, qword ptr [rsp + 0x88] */
    SET64(c->rsp, flags_add(c, c->rsp, 0x60ull, 8));             /* 000141AFFC1E: add rsp, 0x60 */
    SET64(c->rdi, pop64(c));                                     /* 000141AFFC22: pop rdi */
    c->rsp += 8; return;                                         /* 000141AFFC23: ret  */
L_000141AFFC24:
    c->rip = 0x141AFFC24ull;
    SET64(c->rcx, (c->rsp + 0x20ull));                           /* 000141AFFC24: lea rcx, [rsp + 0x20] */
    wr32((c->rsp + 0x20ull), (uint32_t)(0x654u));                /* 000141AFFC29: mov dword ptr [rsp + 0x20], 0x654 */
    wr32((c->rsp + 0x28ull), (uint32_t)(R32(c->rdx)));           /* 000141AFFC31: mov dword ptr [rsp + 0x28], edx */
    push64(c, 0x141AFFC3Aull); dispatch(c, 0x141AFEB70ull); c->rip = 0x141AFFC3Aull; /* 000141AFFC35: call 0x141afeb70 */
    push64(c, 0x141AFFC3Full); dispatch(c, 0x141B01410ull); c->rip = 0x141AFFC3Full; /* 000141AFFC3A: call 0x141b01410 */
    dispatch(c,0x141AFFC3Full);
}
